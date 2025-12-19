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
	//std::string level;
	int16_t x, y, z;
	int8_t rotation;
	uint8_t zone;
	uint8_t state = 0; // Need to defer introductions of remote peers a bit otherwise they are ignored.
	uint32_t last_seq_id = 0;
	uint32_t buffer_expected_size = 0;
	std::string buffer;

	void introduceTo(HubPeer& other, Socket& s)
	{
		StringWriter sw;
		{ uint8_t b = 0xb4; sw.u8(b); }
		{ uint8_t b = HMSG_PEER_INFO; sw.u8(b); }
		sw.u16_le(this->id);
		sw.i16_le(this->x);
		sw.i16_le(this->y);
		sw.i16_le(this->z);
		sw.i8(this->rotation);
		sw.str_lp<u8_t>(this->name);
		sw.str_lp<u8_t>(this->acctid);
		sw.str_lp<u8_t>(this->clan_name);
		std::string loadout;
		//std::string loadout = R"({"a":{"a":"/Lotus/Powersuits/Ember/Ember"}})";
		//std::string loadout = R"({"a":{"a":"/Lotus/Powersuits/Ember/Ember","pricol":{"t0":-65326,"t1":-65326,"t2":-65326,"t3":-65326,"m0":-65326,"m1":-65326,"en":-65326},"Skins":["0/Lotus/Upgrades/Skins/Ember/EmberHelmet","1/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","2/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","5/Lotus/Upgrades/Skins/Fairy/FairyNobleAnims","6/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","7/Lotus/Upgrades/Skins/Ember/EmberSkin","8/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","9/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","10/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","11/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","15/Lotus/Upgrades/Skins/Ember/EmberEffectsSetDefault"]},"d":{"a":"/Lotus/Weapons/Tenno/Melee/LongSword/LongSword","hiddenWhenHolstered":true,"Skins":["2/Lotus/Upgrades/Skins/HolsterCustomizations/SwordUpperBack"]},"r":51,"syndicateLevelSimple":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"gbt1":0,"gbt2":0,"gbt3":0,"sbt1":0,"sbt2":0,"sbt3":0})";
		//std::string loadout = R"({"a":{"a":"/Lotus/Powersuits/Wisp/WispPrime","archonCrystalCounts":2113,"pricol":{"t0":-16777216,"t1":-16777216,"t2":-16777216,"t3":-394759,"m0":-13073153,"m1":-10092442,"en":-10092442,"e1":-13073153},"attcol":{"t0":-16777216,"t1":-16777216,"t2":-16777216,"t3":-394759,"m0":-13073153,"m1":-10092442,"en":-13073153,"e1":-10092442},"sigcol":{"e1":-2138275712},"syancol":{"t0":-16777216,"t1":-16777216,"t2":-16777216,"t3":-394759,"m0":-13073153,"m1":-10092442,"en":-10092442,"e1":-13073153},"Skins":["0/Lotus/Upgrades/Skins/Wisp/SWCovenWispHelmet","1/Lotus/Upgrades/Skins/Armor/SentEvoArmor/SentEvoArmor2A","2/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","5/Lotus/Upgrades/Skins/Wisp/WispAgileAnims","6/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","7/Lotus/Upgrades/Skins/Wisp/WispPrimeSkin","8/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","9/Lotus/Upgrades/Skins/Armor/SentEvoArmor/SentEvoArmor2A","10/Lotus/Upgrades/Skins/Armor/WarframeDefaults/EmptyCustomization","11/Lotus/Upgrades/Skins/Wisp/WispPrimeDefaultCape","15/Lotus/Upgrades/Skins/Wisp/WispEffectsSetDefault","16/Lotus/Upgrades/Skins/Effects/Kuva/KuvaLightningEphemera","25/Lotus/Upgrades/Skins/Crowns/LaurelHaloCrown","26/Lotus/Upgrades/Skins/Voices/DefaultWarframeVoiceItem"]},"b":{"a":"/Lotus/Weapons/Tenno/ThrowingWeapons/TnOraxiaFlechette/TnOraxiaFlechette","hiddenWhenHolstered":true,"pricol":{"t0":-394759,"t1":-16777216,"t2":-6544014,"t3":-1650298,"m0":-6544014,"m1":-1658626,"en":-6544014},"Skins":["2/Lotus/Upgrades/Skins/HolsterCustomizations/PistolHipsDual"]},"c":{"a":"/Lotus/Weapons/Grineer/KuvaLich/LongGuns/Tonkor/KuvaTonkor","hiddenWhenHolstered":true,"pricol":{"t0":-16777216,"t1":-16777216,"t2":-16777216,"t3":-394759,"m0":-13073153,"m1":-10092442,"en":-10092442},"Skins":["2/Lotus/Upgrades/Skins/HolsterCustomizations/RifleUpperBack"]},"d":{"a":"/Lotus/Weapons/Tenno/Melee/Polearms/PrimeGuandao/PrimeGuandaoWeapon","pricol":{"t0":-16777216,"t1":-16777216,"t2":-16777216,"t3":-1577993,"m0":-394759,"m1":-394759,"en":-394759,"e1":-394759},"Skins":["2/Lotus/Upgrades/Skins/HolsterCustomizations/StaffCrossed"]},"r":30,"syndicateLevelSimple":[0,0,1,1,1,0,0,0,1,1,0,1,1,1,0,0,0,0,0,1,1,0,1,1,1,1,1,1,0,0,1,1,1,1,1,1,1,1],"gbt1":0,"gbt2":0,"gbt3":0,"sbt1":0,"sbt2":0,"sbt3":0})";
		sw.oml(loadout.size());
		sw.str(loadout.size(), loadout.data());
		sw.skip(69); // Client complaints that the packet is 'invalid' if it's not padded?
		s.udpServerSend(other.addr, packData(sw.data));
	}
};
static std::vector<HubPeer> peers;
static uint16_t next_peer_id = 1;

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
		std::cout << "Client says: " << string::bin2hex(data) << std::endl;

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

		//std::cout << "Client says: " << string::bin2hex(data) << std::endl;

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

			//std::cout << addr.toString() << " - Reliable packet from peerId=" << peerId << " with seqId=" << seqId << std::endl;

			HubPeer* pPeer = nullptr;
			for (auto& peer : peers)
			{
				if (peer.id == peerId)
				{
					pPeer = &peer;

					if (seqId != peer.last_seq_id + 1)
					{
						std::cout << addr.toString() << " - Ignoring out of order packet" << std::endl;
						return;
					}
					peer.last_seq_id = seqId;

					//std::cout << addr.toString() << " - Sending ack to peerId=" << peerId << " for seqId=" << peer.last_seq_id << std::endl;

					StringWriter sw;
					{ uint8_t b = 0xb8; sw.u8(b); }
					sw.u16_le(peerId);
					sw.u32_le(seqId);
					{ uint8_t b = 0xc8; sw.u8(b); }
					s.udpServerSend(addr, packData(sw.data));
				}
			}
			if (!pPeer)
			{
				std::cout << addr.toString() << " - Ignoring reliable packet from unknown peer" << std::endl;
				return;
			}

			sr.u8(unk_byte);
			if (unk_byte == 0x90)
			{
				uint32_t total_length;
				sr.u32_le(total_length);
				pPeer->buffer.append(data.data() + sr.getPosition(), data.size() - sr.getPosition());
				if (total_length != 0)
				{
					pPeer->buffer_expected_size = total_length;
					return;
				}
				if (pPeer->buffer.size() < pPeer->buffer_expected_size)
				{
					return;
				}
				data = std::move(pPeer->buffer);
				sr = MemoryRefReader(data);
				pPeer->buffer_expected_size = 0;
				pPeer->buffer.clear();
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
							if (peer.id != other.id)
							{
								s.udpServerSend(other.addr, packData(sw.data));
							}
						}

						if (peer.state == 0)
						{
							peer.state = 1;
						}
						else if (peer.state == 1)
						{
							peer.state = 2;
							for (auto& other : peers)
							{
								if (peer.id != other.id)
								{
									other.introduceTo(peer, s);
								}
							}
						}
						break;
					}
				}
				if (!ok)
				{
					std::cout << addr.toString() << " - CMSG_MOVE from unknown peer, asking them to rejoin" << std::endl;
					StringWriter sw;
					{ uint8_t b = 0xb4; sw.u8(b); }
					{ uint8_t b = HMSG_KICK; sw.u8(b); }
					{ uint16_t b = 0xFFFF; sw.u16_le(b); }
					s.udpServerSend(addr, packData(sw.data));
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

				auto& peer = peers.emplace_back(HubPeer{ addr, next_peer_id++, time::millis() });
				sr.str_lp<u8_t>(peer.acctid);
				sr.skip(8); // unk
				sr.str_lp<u8_t>(peer.name);
				sr.str_lp<u8_t>(peer.clan_name);
				//sr.str_lp<u8_t>(peer.level);

				StringWriter sw;
				{ uint8_t b = 0xb4; sw.u8(b); }
				{ uint8_t b = HMSG_JOIN; sw.u8(b); }
				sw.u16_le(peer.id);
				s.udpServerSend(addr, packData(sw.data));

				std::cout << addr.toString() << " - " << peer.name << " (" << peer.acctid << ") is joining, assigned id " << peer.id << std::endl;

				for (auto& other : peers)
				{
					if (other.id != peer.id)
					{
						peer.introduceTo(other, s);
					}
				}
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
				for (auto& peer : peers)
				{
					if (peer.id == peerId)
					{
						//std::cout << addr.toString() << " - Still alive" << std::endl;
						peer.last_sign_of_life = time::millis();

						StringWriter sw;
						{ uint8_t b = 0xb4; sw.u8(b); }
						{ uint8_t b = HMSG_HEARTBEAT; sw.u8(b); }
						sw.u16_le(peerId);
						s.udpServerSend(addr, packData(sw.data));

						break;
					}
				}
			}
			break;

		case CMSG_CONTROL:
			{
				uint16_t peerId;
				sr.u16_le(peerId);

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
				for (auto& peer : peers)
				{
					if (peer.id != peerId)
					{
						s.udpServerSend(peer.addr, packData(sw.data));
					}
					else
					{
						peer.last_sign_of_life = time::millis();
					}
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
