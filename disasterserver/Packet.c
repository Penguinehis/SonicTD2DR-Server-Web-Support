#include <enet/enet.h>
#include <ctype.h>
#include <Packet.h>
#include <CMath.h>
#include <Server.h>
#include <string.h>

#ifdef __GNUC__ 
	#define BYTESWAP_16(x) __builtin_bswap16((x))
	#define BYTESWAP_32(x) __builtin_bswap32((x))
	#define BYTESWAP_64(x) __builtin_bswap64((x))
#else
	#define BYTESWAP_16(x) _byteswap_ushort((x))
	#define BYTESWAP_32(x) _byteswap_ulong((x))
	#define BYTESWAP_64(x) _byteswap_uint64((x))
#endif

String string_new(const char* value)
{
	String str = { .value = {0}, .len = 0 };
	if (!value)
		return str;

	/* Truncate rather than overflow: callers pass config strings (motd) and
	   formatted buffers that can legitimately be longer than the field. */
	size_t len = strlen(value) + 1;
	if (len > sizeof(str.value))
		len = sizeof(str.value);

	memcpy(str.value, value, len);
	str.value[sizeof(str.value) - 1] = 0;
	str.len = (uint16_t)len;

	return str;
}

size_t string_length(String* str)
{
	size_t len = 0;

	for (size_t i = 0; i < str->len; i++)
	{
		if (str->value[i] == '\0')
			break;

		len += (str->value[i] & 0xc0) != 0x80;
	}

	return len;
}

String string_lower(String str)
{
	for (int i = 0; i < str.len; i++)
		str.value[i] = (char)tolower(str.value[i]);

	return str;
}

bool packet_new(Packet* packet, PacketType type)
{
	RAssert(packet);
	packet->len = 0;
	packet->pos = 0;

	PacketWrite(packet, packet_write8, 0);
	PacketWrite(packet, packet_write8, (uint8_t)type);

	return true;
}

Packet packet_from(ENetPacket* packet)
{
	Packet pack = (Packet) { .pos = 0,  .len = (uint8_t)packet->dataLength };
	memcpy(pack.buff, packet->data, (uint8_t)packet->dataLength);
	enet_packet_destroy(packet);
	return pack;
}

bool packet_send(ENetPeer* peer, Packet* packet, bool reliable)
{
	PeerData* data = (PeerData*)peer->data;
	if(data && data->disconnecting)
		return true;

	packet->pos = 0;
	if (!reliable && packet->len > 1 &&
	    packet->buff[1] == CLIENT_PLAYER_DATA &&
	    peer->totalWaitingData > 262144)
		return true;
	ENetPacket* pack = enet_packet_create(packet, packet->len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
	return enet_peer_send(peer, reliable ? 0 : 1, pack) == 0;
}

bool packet_send_id(struct Server* server, uint16_t id, Packet* packet, bool reliable)
{
	packet->pos = 0;
	ENetPacket* pack = enet_packet_create(packet, packet->len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);

	for(int32_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* data = (PeerData*)server->peers.ptr[i];
		if(!data)
			continue;

		if(data->id != id)
			continue;
			
		return enet_peer_send(data->peer, reliable ? 0 : 1, pack) == 0;
	}

	return false;
}

bool packet_seek(Packet* packet, int wh)
{
	RAssert(wh >= 0);
	RAssert(wh < PACKET_MAXSIZE);
	RAssert(wh < packet->len);

	packet->pos = wh;
	return true;
}

bool packet_read8(Packet* packet, uint8_t* out)
{
	RAssert(packet->pos < packet->len);
	*out = packet->buff[packet->pos++];

	return true;
}

bool packet_read16(Packet* packet, uint16_t* out)
{
	RAssert(packet);
	RAssert(out);
	RAssert((size_t)packet->pos + sizeof(uint16_t) <= packet->len);

	uint16_t value;
	memcpy(&value, &packet->buff[packet->pos], sizeof(value));

#ifdef SYS_BIG_ENDIAN
	value = BYTESWAP_16(value);
#endif

	*out = value;
	packet->pos += sizeof(value);

	return true;
}

bool packet_read32(Packet* packet, uint32_t* out)
{
	RAssert(packet);
	RAssert(out);
	RAssert((size_t)packet->pos + sizeof(uint32_t) <= packet->len);

	uint32_t value;
	memcpy(&value, &packet->buff[packet->pos], sizeof(value));

#ifdef SYS_BIG_ENDIAN
	value = BYTESWAP_32(value);
#endif

	*out = value;
	packet->pos += sizeof(value);

	return true;
}

bool packet_read64(Packet* packet, uint64_t* out)
{
	RAssert(packet);
	RAssert(out);
	RAssert((size_t)packet->pos + sizeof(uint64_t) <= packet->len);

	uint64_t value;
	memcpy(&value, &packet->buff[packet->pos], sizeof(value));

#ifdef SYS_BIG_ENDIAN
	value = BYTESWAP_64(value);
#endif

	*out = value;
	packet->pos += sizeof(value);

	return true;
}

bool packet_readfloat(Packet* packet, float* out)
{
	RAssert(packet);
	RAssert(out);
	RAssert((size_t)packet->pos + sizeof(float) <= packet->len);

	uint32_t bits;
	memcpy(&bits, &packet->buff[packet->pos], sizeof(bits));

#ifdef SYS_BIG_ENDIAN
	bits = BYTESWAP_32(bits);
#endif

	float value;
	memcpy(&value, &bits, sizeof(value));

	*out = value;
	packet->pos += sizeof(bits);

	return true;
}

bool packet_readdouble(Packet* packet, double* out)
{
	RAssert(packet);
	RAssert(out);
	RAssert((size_t)packet->pos + sizeof(double) <= packet->len);

	uint64_t bits;
	memcpy(&bits, &packet->buff[packet->pos], sizeof(bits));

#ifdef SYS_BIG_ENDIAN
	bits = BYTESWAP_64(bits);
#endif

	double value;
	memcpy(&value, &bits, sizeof(value));

	*out = value;
	packet->pos += sizeof(bits);

	return true;
}

bool packet_readstr(Packet* packet, String* out)
{
	out->len = 0;

	while (1)
	{
		RAssert(packet->pos < packet->len);
		RAssert(out->len < 128);

		char ch = packet->buff[packet->pos];
		out->value[out->len] = ch;

		packet->pos++;
		out->len++;

		if (ch == '\0')
			break;
	}

	return true;
}

bool packet_write8(Packet* packet, uint8_t value)
{
	RAssert(packet->pos + 1 < PACKET_MAXSIZE);

	if (packet->pos + 1 >= packet->len)
		packet->len++;

	packet->buff[packet->pos++] = value;
	return true;
}

bool packet_write16(Packet* packet, uint16_t value)
{
	RAssert(packet->pos + 2 < PACKET_MAXSIZE);

	if (packet->pos + 2 >= packet->len)
		packet->len += 2;

#ifdef SYS_BIG_ENDIAN
	value = BYTESWAP_16(value);
#endif

	uint8_t* ptr = (uint8_t*)&value;
	packet->buff[packet->pos++] = ptr[0];
	packet->buff[packet->pos++] = ptr[1];
	return true;
}

bool packet_write32(Packet* packet, uint32_t value)
{
	RAssert(packet->pos + 4 < PACKET_MAXSIZE);

	if (packet->pos + 4 >= packet->len)
		packet->len += 4;

#ifdef SYS_BIG_ENDIAN
	value = BYTESWAP_32(value);
#endif

	uint8_t* ptr = (uint8_t*)&value;
	packet->buff[packet->pos++] = ptr[0];
	packet->buff[packet->pos++] = ptr[1];
	packet->buff[packet->pos++] = ptr[2];
	packet->buff[packet->pos++] = ptr[3];
	return true;
}

bool packet_write64(Packet* packet, uint64_t value)
{
	RAssert(packet->pos + 8 < PACKET_MAXSIZE);

	if (packet->pos + 8 >= packet->len)
		packet->len += 8;

#ifdef SYS_BIG_ENDIAN
	value = BYTESWAP_64(value);
#endif

	uint8_t* ptr = (uint8_t*)&value;
	packet->buff[packet->pos++] = ptr[0];
	packet->buff[packet->pos++] = ptr[1];
	packet->buff[packet->pos++] = ptr[2];
	packet->buff[packet->pos++] = ptr[3];
	packet->buff[packet->pos++] = ptr[4];
	packet->buff[packet->pos++] = ptr[5];
	packet->buff[packet->pos++] = ptr[6];
	packet->buff[packet->pos++] = ptr[7];
	return true;
}

bool packet_writefloat(Packet* packet, float value)
{
	RAssert(packet->pos + 4 < PACKET_MAXSIZE);

	if (packet->pos + 4 >= packet->len)
		packet->len += 4;

#ifdef SYS_BIG_ENDIAN
	value = BYTESWAP_32(value);
#endif

	uint8_t* ptr = (uint8_t*)&value;
	packet->buff[packet->pos++] = ptr[0];
	packet->buff[packet->pos++] = ptr[1];
	packet->buff[packet->pos++] = ptr[2];
	packet->buff[packet->pos++] = ptr[3];
	return true;
}

bool packet_writedouble(Packet* packet, double value)
{
	RAssert(packet->pos + 8 < PACKET_MAXSIZE);

	if (packet->pos + 8 >= packet->len)
		packet->len += 8;

#ifdef SYS_BIG_ENDIAN
	value = BYTESWAP_64(value);
#endif

	uint8_t* ptr = (uint8_t*)&value;
	packet->buff[packet->pos++] = ptr[0];
	packet->buff[packet->pos++] = ptr[1];
	packet->buff[packet->pos++] = ptr[2];
	packet->buff[packet->pos++] = ptr[3];
	packet->buff[packet->pos++] = ptr[4];
	packet->buff[packet->pos++] = ptr[5];
	packet->buff[packet->pos++] = ptr[6];
	packet->buff[packet->pos++] = ptr[7];
	return true;
}

bool packet_writestr(Packet* packet, String value)
{
if (!value.len)
return true;

 RAssert((size_t)packet->pos + value.len < PACKET_MAXSIZE);
memcpy(&packet->buff[packet->pos], value.value, value.len);
packet->pos += value.len;
packet->len = packet->pos > packet->len ? packet->pos : packet->len;

	return true;
}
