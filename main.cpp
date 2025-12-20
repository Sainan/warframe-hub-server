#include <deque>
#include <iostream>

#include <crc32c.hpp>
#include <lzf.hpp>
#include <MemoryRefReader.hpp>
#include <Server.hpp>
#include <ServerServiceUdp.hpp>
#include <Socket.hpp>
#include <string.hpp>
#include <StringWriter.hpp>
#include <time.hpp>

#ifdef DOCKER
#include <signal.h>
#endif

using namespace soup;

[[nodiscard]] static std::string addHeader(const std::string& data)
{
	uint32_t initial = crc32c::hash((const uint8_t*)data.data(), data.size());
	uint32_t hash = crc32c::hash((const uint8_t*)"b471e49539930dc9b5a131e6247c7387G", 33, initial); // < U41
	//uint32_t hash = crc32c::hash((const uint8_t*)"b471e49539930dc9b5a131e6247c7387H", 33, initial); // >=U41

	StringWriter sw;

	uint8_t compression_byte = 0;
	sw.u8(compression_byte);

	sw.u32_be(hash);

	//std::cout << "Server says: " << string::bin2hex(sw.data) << string::bin2hex(data) << std::endl;
	return sw.data + data;
}

[[nodiscard]] static std::string packData(const std::string& data)
{
	StringWriter sw;

	uint32_t magic = 0x80;
	sw.u32_be(magic);

	sw.str_lp<u16_le_t>(data);

	return addHeader(sw.data);
}

enum IncomingMsgIds : uint8_t
{
	CMSG_MOVE = 0,
	CMSG_JOIN = 3,
	CMSG_LEAVE = 4, // contains the peer id, e.g. for 77h: 0007F6C91D 00000080 0400 B4 04 7700
	CMSG_HEARTBEAT = 5,
	CMSG_CONTROL = 7,
	CMSG_LOADOUT = 8,
};

enum OutgoingMsgIds : uint8_t
{
	HMSG_MOVE = 0,
	HMSG_PEER_INFO = 2,
	HMSG_JOIN = 3,
	HMSG_KICK = 4,
	HMSG_HEARTBEAT = 5,
	HMSG_CONTROL = 7,
};

struct HubPeer
{
	SocketAddr addr;
	uint16_t id;
	//time_t connected_at;
	time_t last_sign_of_life; // Connections that had no traffic in 30 seconds time out.
	std::string name;
	std::string acctid;
	std::string clan_name;
	std::string loadout;
	std::string level;
	int16_t x, y, z;
	int8_t rotation;
	uint8_t zone;
	uint32_t last_recv_seq_id = 0;
	uint32_t last_send_seq_id = 0;
	uint32_t buffer_expected_size = 0;
	std::string buffer;
	std::deque<std::string> pending_reliables;

	void sendBigPacket(Socket& s, const std::string& data)
	{
		if (data.size() <= 0x49E)
		{
			StringWriter sw;
			{ uint8_t b = 0xb4; sw.u8(b); }
			s.udpServerSend(addr, packData(sw.data + data));
		}
		else
		{
			uint32_t total_length = static_cast<uint32_t>(data.size());
			{
				++this->last_send_seq_id;
				std::cout << addr.toString() << " - Sending reliable packet to peerId=" << this->id << " with seqId=" << this->last_send_seq_id << std::endl;

				StringWriter sw;
				{ uint8_t b = 0xb8; sw.u8(b); }
				sw.u16_le(this->id);
				sw.u32_le(this->last_send_seq_id);
				{ uint8_t b = 0x90; sw.u8(b); }
				sw.u32_le(total_length);
				sw.raw((void*)data.data(), 0x493);
				s.udpServerSend(addr, this->pending_reliables.emplace_back(packData(sw.data)));
			}
			for (uint32_t offset = 0x493; offset != total_length; )
			{
				uint32_t remaining_bytes = total_length - offset;
				uint32_t chunk_size = remaining_bytes > 0x493 ? 0x493 : remaining_bytes;

				++this->last_send_seq_id;
				std::cout << addr.toString() << " - Sending reliable packet to peerId=" << this->id << " with seqId=" << this->last_send_seq_id << std::endl;

				StringWriter sw;
				{ uint8_t b = 0xb8; sw.u8(b); }
				sw.u16_le(this->id);
				sw.u32_le(this->last_send_seq_id);
				{ uint8_t b = 0x90; sw.u8(b); }
				{ uint32_t dw = 0; sw.u32_le(dw); }
				sw.raw((void*)(data.data() + offset), chunk_size);
				s.udpServerSend(addr, this->pending_reliables.emplace_back(packData(sw.data)));

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
		sw.str_lp<u8_t>(this->name);
		sw.str_lp<u8_t>(this->acctid);
		sw.str_lp<u8_t>(this->clan_name);
		sw.oml(this->loadout.size());
		sw.str(this->loadout.size(), this->loadout.data());
		sw.skip(4); // unk
		other.sendBigPacket(s, sw.data);
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

static void new_number_who_dis(Socket& s, SocketAddr& addr)
{
	StringWriter sw;
	{ uint8_t b = 0xb4; sw.u8(b); }
	{ uint8_t b = HMSG_KICK; sw.u8(b); }
	{ uint16_t b = 0xFFFF; sw.u16_le(b); }
	s.udpServerSend(addr, packData(sw.data));
}

static void broadcast_kick(Socket& s, uint16_t peerId)
{
	StringWriter sw;
	{ uint8_t b = 0xb4; sw.u8(b); }
	{ uint8_t b = HMSG_KICK; sw.u8(b); }
	sw.u16_le(peerId);
	for (const auto& peer : peers)
	{
		s.udpServerSend(peer.addr, packData(sw.data));
	}
}

int main(int argc, const char** argv)
{
	Server serv;

	ServerServiceUdp srv([](Socket& s, SocketAddr&& addr, std::string&& data, ServerServiceUdp&)
	{
		MemoryRefReader sr(data);

		uint8_t compression_byte;
		sr.u8(compression_byte);
		if (compression_byte != 0)
		{
			if (compression_byte & 0x80)
			{
				sr.skip(1);
			}

			char buffer[3000];
			const auto decompressed_size = lzf::decompress(data.data() + sr.getPosition(), data.size() - sr.getPosition(), buffer, sizeof(buffer));
			data = std::string(buffer, decompressed_size);
			sr = MemoryRefReader(data);
		}

		//std::cout << addr.toString() << " > " << string::bin2hex(data) << std::endl;

		uint32_t chksum;
		sr.u32_be(chksum);
		//std::cout << "Recvd chksum: " << chksum << std::endl;

		uint32_t initial = crc32c::hash((const uint8_t*)data.data() + 5, data.size() - 5, 0);
		uint32_t hash = crc32c::hash((const uint8_t*)"b471e49539930dc9b5a131e6247c7387G", 33, initial); // < U41
		//uint32_t hash = crc32c::hash((const uint8_t*)"b471e49539930dc9b5a131e6247c7387H", 33, initial); // >=U41
		//std::cout << "Calcd chksum: " << hash << std::endl;

		/*if (chksum != hash)
		{
			std::cout << addr.toString() << " - Checksum mismatch" << std::endl;
			return;
		}*/

		uint32_t magic;
		sr.u32_be(magic);
		//std::cout << "Magic: " << magic << std::endl;
		if (magic != 0x80)
		{
			std::cout << addr.toString() << " - Invalid magic" << std::endl;
			return;
		}

		{
			std::string packed_data;
			sr.str_lp<u16_le_t>(packed_data); // max size = 49Fh
			//std::cout << "packed_data: " << string::bin2hex(packed_data) << std::endl;
			data = std::move(packed_data);
		}
		sr = MemoryRefReader(data);

		uint8_t unk_byte;
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
				new_number_who_dis(s, addr);
				return;
			}

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
			s.udpServerSend(addr, packData(sw.data));

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

						sr.i16_le(peer.x);
						sr.i16_le(peer.y);
						sr.i16_le(peer.z);
						sr.i8(peer.rotation);
						sr.u8(peer.zone);

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
							if (peer.id != other.id && peer.level == other.level)
							{
								s.udpServerSend(other.addr, packData(sw.data));
							}
						}
						break;
					}
				}
				if (!ok)
				{
					std::cout << addr.toString() << " - CMSG_MOVE from unknown peer, asking them to rejoin" << std::endl;
					new_number_who_dis(s, addr);
				}
			}
			break;

		case CMSG_JOIN:
			{
				for (auto i = peers.begin(); i != peers.end(); )
				{
					if (i->addr == addr || time::millisSince(i->last_sign_of_life) > 30'000)
					{
						broadcast_kick(s, i->id);
						i = peers.erase(i);
					}
					else
					{
						++i;
					}
				}

				uint16_t peerId = 0;
				while (get_peer_by_id(peerId))
				{
					if (++peerId == 0xFFFF)
					{
						std::cout << addr.toString() << " - Attempted join but we're at capacity" << std::endl;
						return;
					}
				}

				auto& peer = peers.emplace_back(HubPeer{ addr, peerId, time::millis() });
				sr.str_lp<u8_t>(peer.acctid);
				sr.i16_le(peer.x);
				sr.i16_le(peer.y);
				sr.i16_le(peer.z);
				sr.i8(peer.rotation);
				sr.u8(peer.zone);
				sr.str_lp<u8_t>(peer.name);
				sr.str_lp<u8_t>(peer.clan_name);
				sr.str_lp<u8_t>(peer.level);

				StringWriter sw;
				{ uint8_t b = 0xb4; sw.u8(b); }
				{ uint8_t b = HMSG_JOIN; sw.u8(b); }
				sw.u16_le(peer.id);
				s.udpServerSend(addr, packData(sw.data));

				std::cout << addr.toString() << " - " << peer.name << " (" << peer.acctid << ") is joining, assigned id " << peer.id << std::endl;
			}
			break;

		case CMSG_LEAVE:
			for (auto i = peers.begin(); i != peers.end(); )
			{
				if (i->addr == addr || time::millisSince(i->last_sign_of_life) > 30'000)
				{
					broadcast_kick(s, i->id);
					i = peers.erase(i);
				}
				else
				{
					++i;
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
					s.udpServerSend(addr, packData(sw.data));
				}
				else
				{
					std::cout << addr.toString() << " - CMSG_HEARTBEAT from unknown peer, asking them to rejoin" << std::endl;
					new_number_who_dis(s, addr);
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
					new_number_who_dis(s, addr);
					return;
				}
				peer->resendUnackedPackets(s);

				uint32_t len;
				sr.oml(len);
				std::string msg;
				sr.str(len, msg);

				std::cout << addr.toString() << " - Got control message: " << msg << std::endl;

				StringWriter sw;
				{ uint8_t b = 0xb4; sw.u8(b); }
				{ uint8_t b = HMSG_CONTROL; sw.u8(b); }
				sw.u16_le(peerId);
				sw.oml(len);
				sw.str(len, msg);
				for (auto& other : peers)
				{
					if (other.id != peerId && other.level == peer->level)
					{
						s.udpServerSend(other.addr, packData(sw.data));
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
					uint32_t len;
					sr.oml(len);
					sr.str(len, peer->loadout);

					for (auto& other : peers)
					{
						if (peer->id != other.id && peer->level == other.level)
						{
							other.introduceTo(*peer, s);
							peer->introduceTo(other, s);
						}
					}
				}
				else
				{
					std::cout << addr.toString() << " - CMSG_LOADOUT from unknown peer, asking them to rejoin" << std::endl;
					new_number_who_dis(s, addr);
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
