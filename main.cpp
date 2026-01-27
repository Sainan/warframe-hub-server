#include <deque>
#include <iostream>

#include <crc32.hpp>
#include <crc32c.hpp>
#include <HttpRequestTask.hpp>
#include <joaat.hpp>
#include <json.hpp>
#include <lzf.hpp>
#include <MemoryRefReader.hpp>
#include <Server.hpp>
#include <ServerServiceUdp.hpp>
#include <Socket.hpp>
#include <string.hpp>
#include <StringWriter.hpp>
#include <time.hpp>
#include <utility.hpp>

#ifdef DOCKER
#include <signal.h>
#endif

using namespace soup;

#ifdef DOCKER
#define SERVER_HOST "spaceninjaserver"
#else
#define SERVER_HOST "localhost"
#endif

struct ReportHubPeerDtorTask : public HttpRequestTask
{
	ReportHubPeerDtorTask(const std::string& level)
		: HttpRequestTask(buildRequest(level))
	{
	}

	static HttpRequest buildRequest(const std::string& level)
	{
		std::string path = "/custom/hubDropped?level=";
		path.append(level);

		HttpRequest hr(SERVER_HOST, std::move(path));
		hr.use_tls = false;
		return hr;
	}
};

static bool is_u32_or_below(const std::string_view& salt)
{
	return salt == "b471e49539930dc9b5a131e6247c7387E"
		|| salt == "b471e49539930dc9b5a131e6247c7387D"
		|| salt == "b471e49539930dc9b5a131e6247c7387B"
		|| salt == "b471e49539930dc9b5a131e6247c7387A"
		;
}

static std::string packData(const std::string& data, const std::string_view& salt)
{
	StringWriter sw;

	sw.skip(5); // placeholder for compression byte + CRC

	uint32_t magic = 0x80000000;
	sw.u32_le(magic);

	sw.str_lp<u16_le_t>(data);

	if (is_u32_or_below(salt))
	{
		uint32_t initial = crc32::hash((const uint8_t*)sw.data.data() + 5, sw.data.size() - 5);
		*(uint32_t*)(sw.data.data() + 1) = Endianness::toNetwork(crc32::hash((const uint8_t*)salt.data(), salt.size(), initial));	
	}
	else
	{
		uint32_t initial = crc32c::hash((const uint8_t*)sw.data.data() + 5, sw.data.size() - 5);
		*(uint32_t*)(sw.data.data() + 1) = Endianness::toNetwork(crc32c::hash((const uint8_t*)salt.data(), salt.size(), initial));
	}

	//std::cout << "Server says: " << string::bin2hex(sw.data) << std::endl;

#if false // Not using compression for outgoing packets so bot makers aren't forced to implement it
	if (uint16_t decompressed_size = sw.data.size() - 1;
		decompressed_size > 0x3F
		)
	{
		uint8_t buffer[0x1000];
		if (auto compressed_size = lzf::compress(sw.data.data() + 1, sw.data.size() - 1, buffer + 2, sizeof(buffer) - 2);
			compressed_size != 0 && (compressed_size + 2) < sw.data.size()
			)
		{
			buffer[0] = (decompressed_size >> 6) | 0x80;
			buffer[1] = (decompressed_size & 0x3F) | 0xC0;
			return std::string((const char*)buffer, compressed_size + 2);
		}
	}
#endif

	SOUP_MOVE_RETURN(sw.data);
}

enum IncomingMsgIds : uint8_t
{
	CMSG_MOVE = 0,
	CMSG_ZONE_PAIRS = 1,
	CMSG_JOIN = 3,
	CMSG_LEAVE = 4,
	CMSG_HEARTBEAT = 5,
	CMSG_CONTROL = 7,
	CMSG_LOADOUT = 8,
};

enum OutgoingMsgIds : uint8_t
{
	HMSG_MOVE = 0,
	HMSG_ZONE_PAIRS = 1,
	HMSG_PEER_INFO = 2,
	HMSG_JOIN = 3,
	HMSG_KICK = 4,
	HMSG_HEARTBEAT = 5,
	HMSG_CONTROL = 7,
	HMSG_HIDE_PEER = 9,
};

static bool is_u35_or_below(const std::string_view& salt)
{
	return salt == "b471e49539930dc9b5a131e6247c7387F"
		|| is_u32_or_below(salt)
		;
}

template <typename T>
static void ser_str(T& s, const std::string_view& salt, std::string& str)
{
	if (is_u35_or_below(salt))
	{
		s.template str_lp<u32_le_t>(str);
	}
	else
	{
		uint32_t len = str.size();
		s.oml(len);
		s.str(len, str);
	}
}

struct HubPeer
{
	// Peers send a heartbeat every 30 seconds, so we need to account for latency at least. Packet loss could also be a factor.
	static constexpr time_t TIMEOUT_MS = 69'000;

	SocketAddr addr;
	uint16_t id;
	time_t last_sign_of_life;
	std::string_view salt;
	std::string name;
	std::string acctid;
	std::string clan_name;
	std::string title;
	std::string loadout;
	std::string level;
	std::string status;
	std::vector<std::pair<uint8_t, uint8_t>> zone_pairs;
	int16_t x, y, z;
	int8_t rotation;
	uint8_t zone;
	uint32_t last_recv_seq_id = 0;
	uint32_t last_send_seq_id = 0;
	uint32_t buffer_expected_size = 0;
	std::string buffer;
	std::deque<std::string> pending_reliables;

	~HubPeer()
	{
		//std::cout << "~HubPeer: level=" << level << std::endl;

		if (!level.empty())
		{
			Scheduler::get()->add<ReportHubPeerDtorTask>(level);
		}
	}

	bool isU41orAbove() const noexcept
	{
		return salt == "b471e49539930dc9b5a131e6247c7387H";
	}

	void sendReliablePacket(Socket& s, const std::string& data)
	{
		++this->last_send_seq_id;
		std::cout << this->addr.toString() << " - Sending reliable packet to peerId=" << this->id << " with seqId=" << this->last_send_seq_id << std::endl;

		StringWriter sw;
		{ uint8_t b = 0xb8; sw.u8(b); }
		sw.u16_le(this->id);
		sw.u32_le(this->last_send_seq_id);
		{ uint8_t b = 0xCC; sw.u8(b); }
		sw.raw((void*)data.data(), data.size());
		s.udpServerSend(addr, this->pending_reliables.emplace_back(packData(sw.data, this->salt)));
	}

	void sendBigPacket(Socket& s, const std::string& data)
	{
		if (data.size() <= 0x49E)
		{
			StringWriter sw;
			{ uint8_t b = 0xb4; sw.u8(b); }
			s.udpServerSend(this->addr, packData(sw.data + data, this->salt));
		}
		else
		{
			uint32_t total_length = static_cast<uint32_t>(data.size());
			{
				++this->last_send_seq_id;
				std::cout << this->addr.toString() << " - Sending reliable packet to peerId=" << this->id << " with seqId=" << this->last_send_seq_id << std::endl;

				StringWriter sw;
				{ uint8_t b = 0xb8; sw.u8(b); }
				sw.u16_le(this->id);
				sw.u32_le(this->last_send_seq_id);
				{ uint8_t b = 0x90; sw.u8(b); }
				sw.u32_le(total_length);
				sw.raw((void*)data.data(), 0x493);
				s.udpServerSend(this->addr, this->pending_reliables.emplace_back(packData(sw.data, this->salt)));
			}
			for (uint32_t offset = 0x493; offset != total_length; )
			{
				uint32_t remaining_bytes = total_length - offset;
				uint32_t chunk_size = remaining_bytes > 0x493 ? 0x493 : remaining_bytes;

				++this->last_send_seq_id;
				std::cout << this->addr.toString() << " - Sending reliable packet to peerId=" << this->id << " with seqId=" << this->last_send_seq_id << std::endl;

				StringWriter sw;
				{ uint8_t b = 0xb8; sw.u8(b); }
				sw.u16_le(this->id);
				sw.u32_le(this->last_send_seq_id);
				{ uint8_t b = 0x90; sw.u8(b); }
				{ uint32_t dw = 0; sw.u32_le(dw); }
				sw.raw((void*)(data.data() + offset), chunk_size);
				s.udpServerSend(this->addr, this->pending_reliables.emplace_back(packData(sw.data, this->salt)));

				offset += chunk_size;
			}
		}
	}

	void resendUnackedPackets(Socket& s)
	{
		if (!this->pending_reliables.empty())
		{
			const auto seq_id = (this->last_send_seq_id - (this->pending_reliables.size() - 1));
			std::cout << addr.toString() << " - Resending reliable packet to peerId=" << this->id << " with seqId=" << this->last_send_seq_id << std::endl;
			s.udpServerSend(this->addr, this->pending_reliables.front());
		}
	}

	void introduceTo(HubPeer& other, Socket& s)
	{
		StringWriter sw;
		{ uint8_t b = HMSG_PEER_INFO; sw.u8(b); }
		sw.u16_le(this->id);
		sw.i16_le(this->x);
		sw.i16_le(this->y);
		sw.i16_le(this->z);
		sw.i8(this->rotation);
		ser_str(sw, other.salt, this->name);
		ser_str(sw, other.salt, this->acctid);
		ser_str(sw, other.salt, this->clan_name);
		if (other.isU41orAbove())
		{
			ser_str(sw, other.salt, this->title);
		}
		ser_str(sw, other.salt, this->loadout);
		{ std::string unk; ser_str(sw, other.salt, unk); }
		sw.u8(this->zone);
		sw.skip(2);
		other.sendBigPacket(s, sw.data);

		if (other.canSeeZone(this->zone))
		{
			this->sendStatusTo(other, s);
		}
	}

	bool canSeeZone(uint8_t zone) const noexcept
	{
		if (zone == this->zone)
		{
			return true;
		}
		for (const auto& zp : zone_pairs)
		{
			if ((zp.first == zone && zp.second == this->zone) || (zp.first == this->zone && zp.second == zone))
			{
				return true;
			}
		}
		return false;
	}

	void sendPositionTo(HubPeer& other, Socket& s)
	{
		StringWriter sw;
		{ uint8_t b = 0xb4; sw.u8(b); }
		{ uint8_t b = HMSG_MOVE; sw.u8(b); }
		sw.u16_le(this->id);
		sw.i16_le(this->x);
		sw.i16_le(this->y);
		sw.i16_le(this->z);
		sw.i8(this->rotation);
		s.udpServerSend(other.addr, packData(sw.data, other.salt));
	}

	void sendStatusTo(HubPeer& other, Socket& s)
	{
		if (!this->status.empty())
		{
			//std::cout << other.addr.toString() << " - Sending status of peerId=" << this->id << std::endl;

			{
				StringWriter sw;
				{ uint8_t b = HMSG_CONTROL; sw.u8(b); }
				sw.u16_le(this->id);
				ser_str(sw, other.salt, this->status);
				other.sendReliablePacket(s, sw.data);
			}

			{
				auto msg = soup::make_unique<JsonObject>();
				msg->add("emote", "");

				JsonObject obj;
				obj.add("from", this->acctid);
				obj.add("to", "zone");
				obj.add("msg", std::move(msg));
				auto data = obj.encode();

				StringWriter sw;
				{ uint8_t b = HMSG_CONTROL; sw.u8(b); }
				sw.u16_le(this->id);
				ser_str(sw, other.salt, data);
				other.sendReliablePacket(s, sw.data);
			}

			{
				StringWriter sw;
				{ uint8_t b = HMSG_CONTROL; sw.u8(b); }
				sw.u16_le(this->id);
				ser_str(sw, other.salt, this->status);
				other.sendReliablePacket(s, sw.data);
			}
		}
	}
};
static std::vector<HubPeer> peers;

static HubPeer* get_peer_by_id(uint16_t id)
{
	for (auto& peer : peers)
	{
		if (peer.id == id)
		{
			return &peer;
		}
	}
	return nullptr;
}

static void new_number_who_dis(Socket& s, SocketAddr& addr, const std::string_view& salt)
{
	StringWriter sw;
	{ uint8_t b = 0xb4; sw.u8(b); }
	{ uint8_t b = HMSG_KICK; sw.u8(b); }
	{ uint16_t b = 0xFFFF; sw.u16_le(b); }
	s.udpServerSend(addr, packData(sw.data, salt));
}

static void broadcast_kick(Socket& s, uint16_t peerId)
{
	StringWriter sw;
	{ uint8_t b = 0xb4; sw.u8(b); }
	{ uint8_t b = HMSG_KICK; sw.u8(b); }
	sw.u16_le(peerId);
	for (const auto& peer : peers)
	{
		s.udpServerSend(peer.addr, packData(sw.data, peer.salt));
	}
}

int main(int argc, const char** argv)
{
	Server serv;

	ServerServiceUdp srv([](Socket& s, SocketAddr&& addr, std::string&& data, ServerServiceUdp&)
	{
		MemoryRefReader sr(data);

		uint8_t unk_byte;
		sr.u8(unk_byte);
		if (unk_byte != 0)
		{
			uint16_t expected_decompressed_size = unk_byte;
			if (unk_byte & 0x80)
			{
				expected_decompressed_size &= 0x3F;
				while (unk_byte & 0x40)
				{
					sr.u8(unk_byte);
					expected_decompressed_size <<= 6;
					expected_decompressed_size |= unk_byte & 0x3F;
				}
			}

			char buffer[0x1000];
			const auto decompressed_size = lzf::decompress(data.data() + sr.getPosition(), data.size() - sr.getPosition(), buffer, sizeof(buffer));
			if (decompressed_size != expected_decompressed_size)
			{
				std::cout << addr.toString() << " - Decompressed size mismatch (got " << decompressed_size << ", expected " << expected_decompressed_size << "): " << string::bin2hex(data) << std::endl;
				//return;
			}
			data = std::string(buffer, decompressed_size);
			sr = MemoryRefReader(data);
		}

		//std::cout << addr.toString() << " > " << string::bin2hex(data) << std::endl;

		uint32_t chksum;
		sr.u32_be(chksum);
		//std::cout << "Recvd chksum: " << chksum << std::endl;

		uint32_t initial = crc32c::hash((const uint8_t*)data.data() + sr.getPosition(), data.size() - sr.getPosition(), 0);
		std::string_view salt = "b471e49539930dc9b5a131e6247c7387H"; // >= U41
		if (crc32c::hash((const uint8_t*)salt.data(), salt.size(), initial) != chksum)
		{
			salt = "b471e49539930dc9b5a131e6247c7387G"; // < U41 && >= U35.5
			if (crc32c::hash((const uint8_t*)salt.data(), salt.size(), initial) != chksum)
			{
				salt = "b471e49539930dc9b5a131e6247c7387F"; // < U35.5 && >= U33
				if (crc32c::hash((const uint8_t*)salt.data(), salt.size(), initial) != chksum)
				{
					initial = crc32::hash((const uint8_t*)data.data() + sr.getPosition(), data.size() - sr.getPosition(), 0);
					salt = "b471e49539930dc9b5a131e6247c7387E"; // < U33 && >= U28
					if (crc32::hash((const uint8_t*)salt.data(), salt.size(), initial) != chksum)
					{
						salt = "b471e49539930dc9b5a131e6247c7387D"; // < U28 && >= U27
						if (crc32::hash((const uint8_t*)salt.data(), salt.size(), initial) != chksum)
						{
							salt = "b471e49539930dc9b5a131e6247c7387B"; // < U27 && >= U23
							if (crc32::hash((const uint8_t*)salt.data(), salt.size(), initial) != chksum)
							{
								salt = "b471e49539930dc9b5a131e6247c7387A"; // < U23
								if (crc32::hash((const uint8_t*)salt.data(), salt.size(), initial) != chksum)
								{
									std::cout << addr.toString() << " - Checksum mismatch" << std::endl;
									return;
								}
							}
						}
					}
				}
			}
		}
		//std::cout << addr.toString() << " - salt = " << salt << std::endl;

		{
			uint32_t magic;
			sr.u32_le(magic);
			//std::cout << "Magic: " << magic << std::endl;
			if (magic != 0x80000000)
			{
				std::cout << addr.toString() << " - Invalid magic" << std::endl;
				return;
			}
		}

		{
			std::string packed_data;
			sr.str_lp<u16_le_t>(packed_data); // max size = 49Fh
			//std::cout << "packed_data: " << string::bin2hex(packed_data) << std::endl;
			data = std::move(packed_data);
		}
		sr = MemoryRefReader(data);

		sr.u8(unk_byte);
		if (unk_byte == 0xB8)
		{
			uint16_t peerId;
			sr.u16_le(peerId);
			uint32_t seqId;
			sr.u32_le(seqId);
			sr.u8(unk_byte);

			HubPeer* peer = get_peer_by_id(peerId);
			if (!peer || peer->addr != addr)
			{
				std::cout << addr.toString() << " - Ignoring reliable packet/ack from unknown peer" << std::endl;
				new_number_who_dis(s, addr, salt);
				return;
			}

			peer->last_sign_of_life = time::millis();

			if (unk_byte == 0xC8)
			{
				if (peer->pending_reliables.empty())
				{
					std::cout << addr.toString() << " - Unexpected ack from peerId=" << peerId << " for seqId=" << seqId << " (no acks were pending)" << std::endl;
				}
				else
				{
					std::cout << addr.toString() << " - Got ack from peerId=" << peerId << " for seqId=" << seqId << std::endl;
					peer->pending_reliables.pop_front();
				}
				return;
			}

			//std::cout << addr.toString() << " - Reliable packet from peerId=" << peerId << " with seqId=" << seqId << std::endl;

			if (seqId != peer->last_recv_seq_id + 1)
			{
				std::cout << addr.toString() << " - Ignoring out of order packet from peerId=" << peerId << " with seqId=" << seqId << std::endl;
				return;
			}
			peer->last_recv_seq_id = seqId;

			std::cout << addr.toString() << " - Sending ack to peerId=" << peerId << " for seqId=" << seqId << std::endl;
			StringWriter sw;
			{ uint8_t b = 0xb8; sw.u8(b); }
			sw.u16_le(peerId);
			sw.u32_le(seqId);
			{ uint8_t b = 0xc8; sw.u8(b); }
			s.udpServerSend(addr, packData(sw.data, salt));

			if (unk_byte == 0x90)
			{
				uint32_t total_length;
				sr.u32_le(total_length);
				peer->buffer.append(data.data() + sr.getPosition(), data.size() - sr.getPosition());
				if (total_length != 0)
				{
					peer->buffer_expected_size = total_length;
					return;
				}
				if (peer->buffer.size() < peer->buffer_expected_size)
				{
					return;
				}
				data = std::move(peer->buffer);
				sr = MemoryRefReader(data);
				peer->buffer_expected_size = 0;
				peer->buffer.clear();
			}
			else
			{
				// 0xCC
			}
		}
		else
		{
			// unk_byte=0xB4
		}

		uint8_t packet_id = -1;
		sr.u8(packet_id);
		switch (packet_id)
		{
		case CMSG_MOVE:
			{
				bool ok = false;
				for (auto& peer : peers)
				{
					if (peer.addr == addr)
					{
						ok = true;
						peer.last_sign_of_life = time::millis();
						peer.resendUnackedPackets(s);

						const auto old_zone = peer.zone;
						sr.i16_le(peer.x);
						sr.i16_le(peer.y);
						sr.i16_le(peer.z);
						sr.i8(peer.rotation);
						sr.u8(peer.zone);

						if (peer.zone != old_zone)
						{
							std::cout << addr.toString() << " - Moved into zone " << (int)peer.zone << std::endl;

							// The client will have already hidden all peers it can no longer see now, but we still have to:
							for (auto& other : peers)
							{
								if (peer.id != other.id && peer.level == other.level)
								{
									// Inform the client of peers it can now see.
									if (peer.canSeeZone(other.zone))
									{
										other.sendPositionTo(peer, s);
										other.sendStatusTo(peer, s);
									}

									// Inform peers who can no longer see this client.
									if (!other.canSeeZone(peer.zone))
									{
										StringWriter sw;
										{ uint8_t b = 0xb4; sw.u8(b); }
										{ uint8_t b = HMSG_HIDE_PEER; sw.u8(b); }
										sw.u16_le(peer.id);
										s.udpServerSend(other.addr, packData(sw.data, other.salt));
									}
								}
							}
						}

						StringWriter sw;
						{ uint8_t b = 0xb4; sw.u8(b); }
						{ uint8_t b = HMSG_MOVE; sw.u8(b); }
						sw.u16_le(peer.id);
						sw.i16_le(peer.x);
						sw.i16_le(peer.y);
						sw.i16_le(peer.z);
						sw.i8(peer.rotation);
						for (auto& other : peers)
						{
							if (peer.id != other.id && peer.level == other.level && other.canSeeZone(peer.zone))
							{
								s.udpServerSend(other.addr, packData(sw.data, other.salt));
							}
						}
						break;
					}
				}
				if (!ok)
				{
					std::cout << addr.toString() << " - CMSG_MOVE from unknown peer, asking them to rejoin" << std::endl;
					new_number_who_dis(s, addr, salt);
				}
			}
			break;

		case CMSG_ZONE_PAIRS:
			for (auto& peer : peers)
			{
				if (peer.addr == addr)
				{
					peer.zone_pairs.clear();
					peer.zone_pairs.reserve((data.size() - sr.getPosition()) / 2 - 1);
					uint8_t lo, hi;
					while (true)
					{
						sr.u8(lo);
						sr.u8(hi);
						if (lo == 0xff && hi == 0xff)
						{
							break;
						}
						peer.zone_pairs.emplace_back(lo, hi);
					}
					break;
				}
			}
			break;

		case CMSG_JOIN:
			{
				HubPeer peer{
					.addr = addr,
					.id = 0,
					.last_sign_of_life = time::millis(),
					.salt = salt,
				};
				if (auto ptr = sr.getMemoryView(1); ptr && *(const uint8_t*)ptr != 24) // < U22
				{
					sr.i16_le(peer.x);
					sr.i16_le(peer.y);
					sr.i16_le(peer.z);
					sr.i8(peer.rotation);
					sr.u8(peer.zone);
					ser_str(sr, salt, peer.name);
					ser_str(sr, salt, peer.acctid);
					ser_str(sr, salt, peer.clan_name);
					ser_str(sr, salt, peer.level);
				}
				else // >= U23
				{
					ser_str(sr, salt, peer.acctid);
					sr.i16_le(peer.x);
					sr.i16_le(peer.y);
					sr.i16_le(peer.z);
					sr.i8(peer.rotation);
					sr.u8(peer.zone);
					ser_str(sr, salt, peer.name);
					ser_str(sr, salt, peer.clan_name);
					if (peer.isU41orAbove())
					{
						ser_str(sr, salt, peer.title);
					}
					ser_str(sr, salt, peer.level);
				}

				for (auto i = peers.begin(); i != peers.end(); )
				{
					if (i->addr == addr
						|| i->acctid == peer.acctid
						|| time::millisSince(i->last_sign_of_life) > HubPeer::TIMEOUT_MS
						)
					{
						broadcast_kick(s, i->id);
						i = peers.erase(i);
					}
					else
					{
						++i;
					}
				}

				while (get_peer_by_id(peer.id))
				{
					if (++peer.id == 0xFFFF)
					{
						std::cout << addr.toString() << " - Attempted join but we're at capacity" << std::endl;
						return;
					}
				}

				{
					StringWriter sw;
					{ uint8_t b = 0xb4; sw.u8(b); }
					{ uint8_t b = HMSG_JOIN; sw.u8(b); }
					sw.u16_le(peer.id);
					s.udpServerSend(addr, packData(sw.data, salt));
				}

				{
					StringWriter sw;
					{ uint8_t b = 0xb4; sw.u8(b); }
					{ uint8_t b = HMSG_ZONE_PAIRS; sw.u8(b); }
					ser_str(sw, salt, peer.level);
					s.udpServerSend(addr, packData(sw.data, salt));
				}

				std::cout << addr.toString() << " - " << peer.name << " (" << peer.acctid;
				if (!peer.clan_name.empty())
				{
					std::cout << ", " << peer.clan_name;
				}
				std::cout << ") is joining " << peer.level << ", zone " << (int)peer.zone << ", assigned id " << peer.id << std::endl;

				peers.emplace_back(std::move(peer));
				peer.level.clear();
			}
			break;

		case CMSG_LEAVE:
			{
				uint16_t peerId;
				sr.u16_le(peerId);
				std::cout << addr.toString() << " - Leaving, peerId=" << peerId << std::endl;

				for (auto i = peers.begin(); i != peers.end(); )
				{
					if (i->addr == addr || time::millisSince(i->last_sign_of_life) > HubPeer::TIMEOUT_MS)
					{
						broadcast_kick(s, i->id);
						i = peers.erase(i);
					}
					else
					{
						++i;
					}
				}
			}
			break;

		case CMSG_HEARTBEAT:
			{
				uint16_t peerId;
				sr.u16_le(peerId);
				if (auto peer = get_peer_by_id(peerId); peer && peer->addr == addr)
				{
					//std::cout << addr.toString() << " - Still alive" << std::endl;
					peer->last_sign_of_life = time::millis();
					peer->resendUnackedPackets(s);

					StringWriter sw;
					{ uint8_t b = 0xb4; sw.u8(b); }
					{ uint8_t b = HMSG_HEARTBEAT; sw.u8(b); }
					sw.u16_le(peerId);
					s.udpServerSend(addr, packData(sw.data, salt));
				}
				else
				{
					std::cout << addr.toString() << " - CMSG_HEARTBEAT from unknown peer, asking them to rejoin" << std::endl;
					new_number_who_dis(s, addr, salt);
				}
			}
			break;

		case CMSG_CONTROL:
			{
				uint16_t peerId;
				sr.u16_le(peerId);
				auto peer = get_peer_by_id(peerId);
				if (!peer || peer->addr != addr)
				{
					std::cout << addr.toString() << " - CMSG_CONTROL from unknown peer, asking them to rejoin" << std::endl;
					new_number_who_dis(s, addr, salt);
					return;
				}
				peer->resendUnackedPackets(s);

				std::string msg;
				ser_str(sr, salt, msg);

				std::cout << addr.toString() << " - Got control message: " << msg << std::endl;

				if (auto jr = json::decode(msg); jr && jr->isObj())
				{
					if (jr->reinterpretAsObj().contains("status"))
					{
						peer->status = msg;
					}

					if (jr->reinterpretAsObj().contains("loadout"))
					{
						peer->loadout = jr->reinterpretAsObj().at("loadout").asObj().encode();
						for (auto& other : peers)
						{
							if (other.id != peerId && other.level == peer->level)
							{
								peer->introduceTo(other, s);
							}
						}
					}
					else
					{
						std::string_view to = "all";
						if (jr->reinterpretAsObj().contains("to"))
						{
							to = jr->reinterpretAsObj().at("to").asStr().value;
						}
						unsigned recipients = 0;
						if (to == "all" || to == "dojo")
						{
							for (auto& other : peers)
							{
								if (other.id != peerId && other.level == peer->level)
								{
									StringWriter sw;
									{ uint8_t b = HMSG_CONTROL; sw.u8(b); }
									sw.u16_le(peerId);
									ser_str(sw, other.salt, msg);
									other.sendReliablePacket(s, sw.data);
									++recipients;
								}
							}
						}
						else if (to == "zone")
						{
							for (auto& other : peers)
							{
								if (other.id != peerId && other.level == peer->level && other.canSeeZone(peer->zone))
								{
									StringWriter sw;
									{ uint8_t b = HMSG_CONTROL; sw.u8(b); }
									sw.u16_le(peerId);
									ser_str(sw, other.salt, msg);
									other.sendReliablePacket(s, sw.data);
									++recipients;
								}
							}
						}
						else
						{
							for (auto& other : peers)
							{
								if (other.acctid == to)
								{
									StringWriter sw;
									{ uint8_t b = HMSG_CONTROL; sw.u8(b); }
									sw.u16_le(peerId);
									ser_str(sw, other.salt, msg);
									other.sendReliablePacket(s, sw.data);
									++recipients;
									break;
								}
							}
						}
						std::cout << addr.toString() << " - Forwarded to " << recipients << " peer(s)" << std::endl;
					}
				}
			}
			break;

		case CMSG_LOADOUT:
			{
				uint16_t peerId;
				sr.u16_le(peerId);
				if (auto peer = get_peer_by_id(peerId); peer && peer->addr == addr)
				{
					ser_str(sr, salt, peer->loadout);

					for (auto& other : peers)
					{
						if (peer->id != other.id && peer->level == other.level)
						{
							other.introduceTo(*peer, s);
							peer->introduceTo(other, s);
						}
					}

					if (peer->level.starts_with("SCENARIOEVENTHUB5"))
					{
						std::string data = R"({"scenario":{"endTime":"2000000000"}})";

						StringWriter sw;
						{ uint8_t b = HMSG_CONTROL; sw.u8(b); }
						sw.u16_le(peerId);
						ser_str(sw, salt, data);
						peer->sendReliablePacket(s, sw.data);
					}
				}
				else
				{
					std::cout << addr.toString() << " - CMSG_LOADOUT from unknown peer, asking them to rejoin" << std::endl;
					new_number_who_dis(s, addr, salt);
				}
			}
			break;

		default:
			std::cout << addr.toString() << " - Unknown packet with id " << (int)packet_id << ": " << string::bin2hex(data) << std::endl;
			break;
		}
	});

	if (!serv.bindUdp(6952, &srv))
	{
		std::cout << "Failed to bind UDP/6952" << std::endl;
		return 1;
	}
	std::cout << "Bound to UDP/6952" << std::endl;

#ifdef DOCKER
	// Ctrl+C not killing your software? According to the professional ChatGPTs hired by Docker Inc, it's not an issue. Why? Because there's a workaround!
	signal(SIGTERM, [](int) { exit(0); });
#endif

	serv.run();
}
