#include <Mod.h>
#include "Lib.h"
#include <enet/enet.h>
#include <Server.h>
#include <Config.h>
#include <Relay.h>
#include <WebGate.h>
#include <Colors.h>
#include <CMath.h>
#include <Log.h>
#include <States.h>
#include <Packet.h>
#include <ctype.h>
#include <io/Threads.h>
#include <io/Time.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <cJSON.h>

#ifdef SYS_USE_SDL2
#include <ui/Main.h>
#endif

cJSON *ip_addr_list = NULL;
Mutex ip_addr_mut;

bool server_global_init(void)
{
	if (ip_addr_list)
		return true;

	ip_addr_list = cJSON_CreateObject();
	RAssert(ip_addr_list);
	MutexCreate(ip_addr_mut);
	return true;
}

bool peer_identity_process(PeerData *v, const char *addr, bool is_banned, uint64_t timeout, bool do_timeout)
{
	MutexLock(ip_addr_mut);
	{
		if (!v->op && (cJSON_HasObjectItem(ip_addr_list, addr) || cJSON_HasObjectItem(ip_addr_list, v->udid.value)))
		{
			server_disconnect(v->server, v->peer, DR_IPINUSE, NULL);
			MutexUnlock(ip_addr_mut);
			return false;
		}
	}
	MutexUnlock(ip_addr_mut);

	if (is_banned)
	{
		Info("%s banned by host (id %d, ip %s)", v->nickname.value, v->id, addr);
		RAssert(server_disconnect(v->server, v->peer, DR_BANNEDBYHOST, NULL));
		return false;
	}

	size_t effective_cap = (g_config.vanilla_balancing &&
		g_config.max_simultaneous_players > 7)
		? 7 : (size_t)g_config.max_simultaneous_players;
	if (v->server->peers.noitems >= effective_cap)
	{
		v->should_timeout = false;
		server_disconnect(v->server, v->peer, DR_LOBBYFULL, NULL);
		return false;
	}

	if (do_timeout && timeout != 0)
	{
		time_t tm = time(NULL);
		time_t val = timeout - tm;
		if (val > 0)
		{
			Info("%s is rate-limited (id %d, ip %s)", v->nickname.value, v->id, addr);
			RAssert(server_disconnect(v->server, v->peer, DR_RATELIMITED, NULL));
			return false;
		}
		else
			AssertOrDisconnect(v->server, timeout_revoke(v->udid.value, addr));
	}

	if (!dylist_push(&v->server->peers, v))
	{
		v->should_timeout = false;
		server_disconnect(v->server, v->peer, DR_OTHER, "Internal error: failed to add peer to list.");
		return false;
	}

	if (!server_state_joined(v))
	{
		v->should_timeout = false;
		server_disconnect(v->server, v->peer, DR_OTHER, "Internal error: failed to join state.");
		return false;
	}

	
	Packet pack;
	PacketCreate(&pack, SERVER_IDENTITY_RESPONSE);
	PacketWrite(&pack, packet_write8, v->server->state == ST_LOBBY);
	PacketWrite(&pack, packet_write16, v->id);
	RAssert(packet_send(v->peer, &pack, true));

	
	if (!v->in_game)
	{
		
		for (size_t i = 0; i < v->server->peers.capacity; i++)
		{
			PeerData *peer = (PeerData *)v->server->peers.ptr[i];
			if (!peer)
				continue;

			if (peer->id == v->id)
				continue;

			PacketCreate(&pack, SERVER_WAITING_PLAYER_INFO);
			PacketWrite(&pack, packet_write8, v->server->state == ST_GAME && peer->in_game);
			PacketWrite(&pack, packet_write16, peer->id);
			PacketWrite(&pack, packet_writestr, peer->nickname);

			if (v->server->state == ST_GAME && peer->in_game)
			{
				PacketWrite(&pack, packet_write8, server_is_exe(v->server, peer->id));
				PacketWrite(&pack, packet_write8, server_is_exe(v->server, peer->id) ? peer->exe_char : peer->surv_char);
			}
			else
			{
				PacketWrite(&pack, packet_write8, peer->lobby_icon);
			}

			RAssert(packet_send(v->peer, &pack, true));
		}

		
		PacketCreate(&pack, SERVER_WAITING_PLAYER_INFO);
		PacketWrite(&pack, packet_write8, 0);
		PacketWrite(&pack, packet_write16, v->id);
		PacketWrite(&pack, packet_writestr, v->nickname);
		PacketWrite(&pack, packet_write8, v->lobby_icon);
		RAssert(server_broadcast_ex(v->server, &pack, true, v->id));

		char msg[100];
		snprintf(msg, 100, "server " CLRCODE_RED "%d" CLRCODE_RST " of " CLRCODE_BLU "%d" CLRCODE_RST, v->server->id + 1, g_config.server_count);

		server_send_msg(v->server, v->peer, "-----------------------");
		server_send_msg(v->server, v->peer, CLRCODE_RED "bestest/server~ v" STRINGIFY(BUILD_VERSION));
		server_send_msg(v->server, v->peer, "build from " CLRCODE_PUR __DATE__ " " CLRCODE_GRN __TIME__ CLRCODE_RST);
		server_send_msg(v->server, v->peer, msg);
		server_send_msg(v->server, v->peer, "-----------------------");
		server_send_motd(v->server, v->peer);

		if (v->op)
			server_send_msg(v->server, v->peer, CLRCODE_GRN "you're an operator on this server" CLRCODE_RST);

		if (v->server->state >= ST_GAME)
		{
			snprintf(msg, 100, "map: " CLRCODE_GRN "%s" CLRCODE_RST, g_mapList[v->server->game.map].name);
			server_send_msg(v->server, v->peer, msg);
		}
	}

	MutexLock(ip_addr_mut);
	cJSON_AddItemToObject(ip_addr_list, addr, cJSON_CreateTrue());
	cJSON_AddItemToObject(ip_addr_list, v->udid.value, cJSON_CreateTrue());
	MutexUnlock(ip_addr_mut);
	return true;
}

bool peer_identity(PeerData *v, Packet *packet)
{
	RAssert(v);
	RAssert(v->id > 0);

	/* Exactly one IDENTITY per connection. A second one would run the join path
	   again and register this peer in the peer list a second time, leaving a
	   stale reference behind once the peer is freed. Ignore the repeat rather
	   than dropping the client, so a chatty client is never punished for it. */
	if (v->verified)
	{
		Debug("Ignoring repeated IDENTITY from id %d", v->id);
		return true;
	}

	srand((unsigned int)time(NULL));

	bool is_banned;
	uint64_t timeout;

	
	PacketRead(passtrough, packet, packet_read8, uint8_t);
	PacketRead(type, packet, packet_read8, uint8_t);
	PacketRead(build_version, packet, packet_read16, uint16_t);
	PacketRead(server_index, packet, packet_read32, int32_t);
	PacketRead(nickname, packet, packet_readstr, String);
	PacketRead(udid, packet, packet_readstr, String);
	PacketRead(lobby_icon, packet, packet_read8, uint8_t);
	PacketRead(pet, packet, packet_read8, int8_t);

	RAssert(ban_check(udid.value, v->ip.value, &is_banned));
	RAssert(timeout_check(udid.value, v->ip.value, &timeout));
	RAssert(op_check(v->ip.value, &v->op));

	v->should_timeout = true;
	v->disconnecting = false;
	v->mod_tool = false;
	v->is_mobile = false;
	v->nickname = nickname;
	v->udid = udid;
	v->lobby_icon = lobby_icon;
	v->pet = pet;

	if (g_config.anticheat)
	{
		AssertOrDisconnect(v->server, auth_verify_ticket(v, packet));
	}

	/* web players that can load mods (sonicmods) end IDENTITY with 'W' and their mod api */
	v->mod_api = 0;
	v->mod_ready = false;
	if (!g_config.anticheat && packet->pos + 16 <= packet->len)
		packet->pos += 16;  /* (the ticket nobody checks) */
	if (packet->pos + 3 <= packet->len && packet->buff[packet->pos] == 'W')
		v->mod_api = (uint16_t)(packet->buff[packet->pos + 1] | (packet->buff[packet->pos + 2] << 8));
	/* the web game cannot send the PC game's checksums: it is not a modified client */
	if (v->mod_api >= 1)
		v->mod_tool = false;

	bool res = true;
	MutexLock(v->server->state_lock);
	{
		v->in_game = (v->server->state == ST_LOBBY);
		v->exe_chance = 1 + rand() % 4;

		size_t effective_cap = (g_config.vanilla_balancing &&
			g_config.max_simultaneous_players > 7)
			? 7 : (size_t)g_config.max_simultaneous_players;

		/* Vote-kicked from THIS lobby? Move them to another one rather than
		   turning them away from the server entirely. */
		uint64_t lobby_block = 0;
		lobby_timeout_check(v->udid.value, v->ip.value, (int32_t)v->server->id, &lobby_block);
		if (lobby_block)
		{
			for (int i = 0; i < disaster_count(); i++)
			{
				Server *server = disaster_get(i);
				if (!server || server->id == v->server->id)
					continue;

				if (server->peers.noitems >= effective_cap)
					continue;

				uint64_t other_block = 0;
				lobby_timeout_check(v->udid.value, v->ip.value, (int32_t)server->id, &other_block);
				if (other_block)
					continue;

				Packet pack;
				PacketCreate(&pack, SERVER_LOBBY_CHANGELOBBY);
				PacketWrite(&pack, packet_write32, relay_public_port(server->id));
				packet_send(v->peer, &pack, true);

				Info("%s (id %d) is vote-kicked from lobby %d, moving them to lobby %d",
					v->nickname.value, v->id, v->server->id + 1, server->id + 1);
				res = false;
				goto quit;
			}

			/* Nowhere left to send them; say how long the block still has. */
			uint64_t now = (uint64_t)time(NULL);
			uint64_t remaining = lobby_block > now ? lobby_block - now : 0;

			char blocked[160];
			snprintf(blocked, sizeof(blocked),
				"You were vote-kicked from lobby %d. Try again in %u minute(s), or join another lobby.",
				v->server->id + 1, (unsigned)((remaining + 59) / 60));
			server_disconnect(v->server, v->peer, DR_OTHER, blocked);
			res = false;
			goto quit;
		}

		if (v->server->peers.noitems >= effective_cap)
		{
			for (int i = 0; i < disaster_count(); i++)
			{
				Server *server = disaster_get(i);
				if (!server)
					continue;

				if (server->peers.noitems >= effective_cap)
					continue;

				Packet pack;
				PacketCreate(&pack, SERVER_LOBBY_CHANGELOBBY);
				PacketWrite(&pack, packet_write32, relay_public_port(server->id));
				packet_send(v->peer, &pack, true);

				Debug("Redirecting %d to another free server: %d", v->id, server->id);
				res = false;
				goto quit;
			}

			server_disconnect(v->server, v->peer, DR_LOBBYFULL, NULL);
			res = false;
			goto quit;
		}

		if (type != IDENTITY)
		{
			server_disconnect(v->server, v->peer, DR_OTHER, "type != IDENTITY?");
			res = false;
			goto quit;
		}

		if (passtrough)
		{
			server_disconnect(v->server, v->peer, DR_OTHER, "passtrough?");
			res = false;
			goto quit;
		}

		if (build_version != BUILD_VERSION)
		{
			server_disconnect(v->server, v->peer, DR_VERMISMATCH, NULL);
			res = false;
			goto quit;
		}

		if (string_length(&nickname) >= 30)
		{
			server_disconnect(v->server, v->peer, DR_OTHER, "Your nickname is too long! (30 characters max)");
			res = false;
			goto quit;
		}

		if (udid.len <= 0)
		{
			server_disconnect(v->server, v->peer, DR_OTHER, "Version mismatch: please use the correct client version.");
			res = false;
			goto quit;
		}

		if (!mod_client_ok(v))
		{
			mod_refuse(v);
			res = false;
			goto quit;
		}

		res = peer_identity_process(v, v->ip.value, is_banned, timeout, server_index == -1);
		if (!res)
		{
			/* The peer may already have been pushed into the peer list before the
			   failure. Drop it now so no broadcast can reach a peer that is about
			   to be freed. */
			dylist_remove(&v->server->peers, v);
			goto quit;
		}

		Info("%s (id %d) " LOG_YLW "joined.", nickname.value, v->id);
		Info("	IP: %s", v->ip.value);
		Info("	UID: %s", udid.value);
		Info("	Modified: %s", BoolStringify(v->mod_tool));
		Info("	Mobile: %s", BoolStringify(v->is_mobile));
		v->verified = true;
		mod_send_info(v);
		mod_on_join(v);
	}

quit:
	MutexUnlock(v->server->state_lock);
	return res;
}

bool peer_msg(PeerData *v, Packet *packet)
{
	if (v->id == 0)
		return false;

	bool res;
	MutexLock(v->server->state_lock);
	{
		res = server_state_handle(v, packet);
	}
	MutexUnlock(v->server->state_lock);

	return res;
}

bool server_normalize_map_argument(const char *text, NormalizedMap *out)
{
    int value = 0;

    if (sscanf(text, "index %d", &value) == 1)
    {
        if (value < 0 || value > MAP_FART_ZONE)
            return false;
        out->index = (int8_t)value;
        out->regular_rotation = value < MAP_COUNT;
        return true;
    }

    if (sscanf(text, "%d", &value) != 1)
        return false;

    if (value == 0)
    {
        out->index = 0;
        out->regular_rotation = true;
        return true;
    }

    if (value < 1 || value > MAP_COUNT + 1)
        return false;

    out->index = (int8_t)(value - 1);
    out->regular_rotation = out->index < MAP_COUNT;
    return true;
}

typedef struct
{
    uint8_t minimum_active_players;
    uint16_t interval_numerator;
    uint16_t interval_denominator;
} RelayTier;

static const RelayTier relay_tiers[] =
{
    { 1,  5, 4 },
    { 11, 3, 2 },
    { 16, 2, 1 },
    { 25, 5, 2 },
    { 35, 3, 1 },
    { 50, 4, 1 },
    { 65, 5, 1 },
    { 80, 6, 1 },
    { 90, 15, 2 },
    { 95, 10, 1 }
};

#define RELAY_TIER_COUNT (sizeof(relay_tiers) / sizeof(relay_tiers[0]))

static bool relay_cadence_due(Server *server, uint8_t tier)
{
    const RelayTier *current = &relay_tiers[tier];
    server->game.relay_accumulator += current->interval_denominator;
    if (server->game.relay_accumulator < current->interval_numerator)
        return false;
    server->game.relay_accumulator -= current->interval_numerator;
    return true;
}

static uint8_t relay_active_alive_count(Server *server)
{
    uint8_t count = 0;
    for (size_t i = 0; i < server->peers.capacity; i++)
    {
        PeerData *peer = server->peers.ptr[i];
        if (!peer || !peer->in_game)
            continue;
        if (peer->plr.flags & PLAYER_DEAD)
            continue;
        if (peer->plr.flags & PLAYER_ESCAPED)
            continue;
        if (peer->plr.flags & PLAYER_DEMONIZED)
            continue;
        count++;
    }
    return count;
}

static uint8_t relay_tier_for_count(uint8_t active)
{
    uint8_t tier = 0;
    for (uint8_t i = 0; i < RELAY_TIER_COUNT; i++)
    {
        if (active >= relay_tiers[i].minimum_active_players)
            tier = i;
    }
    return tier;
}

static bool relay_recipient_bypasses_aoi(const PeerData *recipient)
{
     

    return (recipient->plr.flags &
        (PLAYER_DEAD | PLAYER_ESCAPED | PLAYER_FROZEN_DEMON)) != 0;
}

static bool relay_source_bypasses_aoi(const PeerData *source) { return
    source->plr.flags & (PLAYER_DEAD | PLAYER_ESCAPED | PLAYER_FROZEN_DEMON); }

static bool relay_pair_in_aoi(
    const PeerData *source,
    const PeerData *recipient)
{
    Vector2 source_pos = source->plr.pos;
    Vector2 recipient_pos = recipient->plr.pos;
    return vector2_dist_squared(&source_pos, &recipient_pos) <=
        RELAY_AOI_RADIUS_PX * RELAY_AOI_RADIUS_PX;
}

void relay_invalidate_player_data(PeerData *source)
{
    source->player_data_len = 0;
    source->player_data_valid = false;
    memset(
        source->relay_corrections,
        0,
        sizeof(source->relay_corrections));
    source->relay_force_pending = true;
    source->relay_stop_pending = true;
    source->relay_next_elapsed = 0.0;
    source->relay_last_fill_tick = 0;
    source->relay_last_send_tick = 0;
}

bool relay_cache_player_data(PeerData *source, const Packet *incoming)
{
    if (incoming->len < 2 || incoming->len - 2 > sizeof(source->player_data))
        return false;

    bool state_changed = source->player_data_valid &&
        source->last_relay_state != source->plr.state;

    source->player_data_len = incoming->len - 2;
    memcpy(source->player_data, incoming->buff + 2, source->player_data_len);
    source->player_data_valid = true;
    source->last_relay_state = source->plr.state;
    if (state_changed)
    {
        source->relay_force_pending = true;
        source->relay_next_elapsed = (double)source->server->tick;
    }
    if (source->plr.prev_recv_valid &&
        source->plr.recv_vx == 0.0f &&
        source->plr.recv_vy == 0.0f)
    {
        source->relay_stop_pending = true;
    }
    return true;
}

static void relay_retained_state_at(
    PeerData *v,
    uint32_t now_tick,
    Vector2 *out_pos,
    float *out_vx,
    float *out_vy)
{
    *out_pos = v->plr.pos;
    *out_vx = 0.0f;
    *out_vy = 0.0f;

    if (!v->plr.prev_recv_valid || relay_source_bypasses_aoi(v))
        return;

    double ticks_since_recv =
        (double)now_tick - v->plr.last_recv_elapsed;

    if (ticks_since_recv < 0.0)
        ticks_since_recv = 0.0;

    if (ticks_since_recv >
        (double)g_config.retained_extrapolation_limit_ticks_x)
        return;

    float decay =
        ticks_since_recv >
            (double)g_config.retained_velocity_decay_start_ticks
        ? powf(
            g_config.retained_velocity_decay,
            (float)(
                ticks_since_recv -
                (double)g_config.retained_velocity_decay_start_ticks))
        : 1.0f;

    double y_ticks =
        ticks_since_recv <
            (double)g_config.retained_extrapolation_limit_ticks_y
        ? ticks_since_recv
        : (double)g_config.retained_extrapolation_limit_ticks_y;

    out_pos->x =
        v->plr.pos.x +
        v->plr.recv_vx * decay * (float)ticks_since_recv;
    out_pos->y =
        v->plr.pos.y +
        v->plr.recv_vy * decay * (float)y_ticks;

    *out_vx = v->plr.recv_vx * decay;
    *out_vy = v->plr.recv_vy * decay;

    if (out_pos->x < 0.0f) out_pos->x = 0.0f;
    if (out_pos->x > 65535.0f) out_pos->x = 65535.0f;
    if (out_pos->y < 0.0f) out_pos->y = 0.0f;
    if (out_pos->y > 65535.0f) out_pos->y = 65535.0f;
}

static double relay_interval_for_tier(uint8_t tier);

static void relay_apply_recipient_extrapolation(
    Server *server,
    PeerData *source,
    PeerData *recipient,
    Vector2 *position,
    float adjusted_vx,
    float adjusted_vy)
{
    const double relay_interval_ticks =
        relay_interval_for_tier(server->game.relay_tier);
    const double relay_interval_ms =
        relay_interval_ticks * (1000.0 / (double)TICKSPERSEC);

    uint16_t effective_ping_ms =
        recipient->peer->lastReceiveTime == 0
            ? 0
            : recipient->peer->roundTripTime;
    if (effective_ping_ms > 500)
        effective_ping_ms = 500;

    RelayCorrectionState *correction =
        &source->relay_corrections[recipient->id];

    if (source->plr.ex_teleport != 0)
    {
        correction->offset.x = 0.0f;
        correction->offset.y = 0.0f;
        correction->last_vx = 0.0f;
        correction->last_vy = 0.0f;
        correction->last_tick = server->tick;
        correction->recipient_id = recipient->id;
        correction->valid = true;
        return;
    }

    Vector2 desired_offset = { 0.0f, 0.0f };
    if (recipient->peer->lastReceiveTime != 0 &&
        relay_interval_ms > 0.0 &&
        (adjusted_vx != 0.0f || adjusted_vy != 0.0f))
    {
        double relay_frames =
            floor((double)effective_ping_ms / relay_interval_ms);

        if ((double)effective_ping_ms < relay_interval_ms)
            relay_frames = 1.0 / 5.0;
        else
            relay_frames /= 2.0;

        desired_offset.x =
            adjusted_vx * (float)relay_interval_ticks *
            (float)relay_frames;
        desired_offset.y =
            adjusted_vy * (float)relay_interval_ticks *
            (float)relay_frames;
    }

    if (!correction->valid ||
        correction->recipient_id != recipient->id)
    {
        correction->offset = desired_offset;
        correction->last_vx = adjusted_vx;
        correction->last_vy = adjusted_vy;
        correction->last_tick = server->tick;
        correction->recipient_id = recipient->id;
        correction->valid = true;
    }
    else
    {
        float previous_speed =
            sqrtf(
                correction->last_vx * correction->last_vx +
                correction->last_vy * correction->last_vy);
        float current_speed =
            sqrtf(
                adjusted_vx * adjusted_vx +
                adjusted_vy * adjusted_vy);
        float step =
            fmaxf(previous_speed, current_speed) *
            (float)relay_interval_ticks;
        float delta_x = desired_offset.x - correction->offset.x;
        float delta_y = desired_offset.y - correction->offset.y;
        float delta_length = sqrtf(delta_x * delta_x + delta_y * delta_y);

        if (current_speed + 0.0001f < previous_speed)
        {
            correction->offset = desired_offset;
        }
        else if (step > 0.0f)
        {
            float subtle_step = step * 0.1f;
            if (delta_length > subtle_step)
            {
                correction->offset.x += delta_x / delta_length * subtle_step;
                correction->offset.y += delta_y / delta_length * subtle_step;
            }
            else
            {
                correction->offset = desired_offset;
            }
        }
        else
        {
            correction->offset = desired_offset;
            if (adjusted_vx == 0.0f && adjusted_vy == 0.0f)
            {
                correction->last_vx = 0.0f;
                correction->last_vy = 0.0f;
            }
        }

        if (adjusted_vx != 0.0f || adjusted_vy != 0.0f)
        {
            correction->last_vx = adjusted_vx;
            correction->last_vy = adjusted_vy;
        }
        correction->last_tick = server->tick;
    }

    position->x += correction->offset.x;
    position->y += correction->offset.y;

    if (position->x < 0.0f) position->x = 0.0f;
    if (position->x > 65535.0f) position->x = 65535.0f;
    if (position->y < 0.0f) position->y = 0.0f;
    if (position->y > 65535.0f) position->y = 65535.0f;
}

static bool relay_send_player_data(Server *server, PeerData *source, PeerData *recipient)
{
    if (!source->player_data_valid || !recipient->peer)
        return true;

    if (source->player_data_len < 8)
        return false;

    Vector2 retained_pos;
    float retained_vx;
    float retained_vy;

    relay_retained_state_at(
        source,
        server->tick,
        &retained_pos,
        &retained_vx,
        &retained_vy);

    if (!relay_source_bypasses_aoi(source) &&
        relay_pair_in_aoi(source, recipient))
        relay_apply_recipient_extrapolation(
            server,
            source,
            recipient,
            &retained_pos,
            retained_vx,
            retained_vy);
    else
        source->relay_corrections[recipient->id].valid = false;

    Packet packet;
    PacketCreate(&packet, CLIENT_PLAYER_DATA);
    PacketWrite(&packet, packet_write16, source->id);

    PacketWrite(
        &packet,
        packet_write16,
        (uint16_t)(retained_pos.x + 0.5f));
    PacketWrite(
        &packet,
        packet_write16,
        (uint16_t)(retained_pos.y + 0.5f));
    PacketWrite(
        &packet,
        packet_write16,
        game_float_to_half(retained_vx));
    PacketWrite(
        &packet,
        packet_write16,
        game_float_to_half(retained_vy));

    size_t tail_len = source->player_data_len - 8;
    /* packet.pos is a uint8_t: letting it reach PACKET_MAXSIZE wraps it to 0
       and would emit a zero-length packet. */
    RAssert((size_t)packet.pos + tail_len < PACKET_MAXSIZE);
    memcpy(
        packet.buff + packet.pos,
        source->player_data + 8,
        tail_len);
    packet.pos += tail_len;
    packet.len = packet.pos;

    return packet_send(recipient->peer, &packet, false);
}

static bool relay_update_tier(Server *server, uint8_t active)
{
    uint8_t current = server->game.relay_tier;
    uint8_t target = relay_tier_for_count(active);

    if (target < current)
    {
        uint8_t lower =
            relay_tiers[current].minimum_active_players > RELAY_TIER_DOWNGRADE_MARGIN
                ? relay_tiers[current].minimum_active_players - RELAY_TIER_DOWNGRADE_MARGIN
                : 0;
        if (active > lower)
            target = current;
    }

    if (target == current)
    {
        server->game.relay_pending_tier = current;
        server->game.relay_stable_ticks = 0;
        return false;
    }

    if (target != server->game.relay_pending_tier)
    {
        server->game.relay_pending_tier = target;
        server->game.relay_stable_ticks = 0;
        return false;
    }

    if (++server->game.relay_stable_ticks < RELAY_TIER_STABILIZATION_TICKS)
        return false;

    server->game.relay_stable_ticks = 0;
    server->game.relay_tier = server->game.relay_pending_tier;
    server->game.relay_pending_tier = server->game.relay_tier;
    return true;
}

void relay_mark_priority_near(Server *server, const Vector2 *center)
{
    uint32_t expiry = server->tick + RELAY_PRIORITY_DURATION_TICKS;
    for (size_t i = 0; i < server->peers.capacity; i++)
    {
        PeerData *recipient = server->peers.ptr[i];
        if (!recipient || !recipient->in_game)
            continue;
        if (vector2_dist_squared(&recipient->plr.pos, (Vector2 *)center) <=
            RELAY_COMBAT_RADIUS * RELAY_COMBAT_RADIUS)
            recipient->priority_until_tick = expiry;
    }
}

static bool relay_pair_is_priority(Server *server, PeerData *source, PeerData *recipient)
{
    if (server_is_exe(server, source->id))
        return true;
     

    if (source->plr.is_attacking)
        return true;
    if (recipient->priority_until_tick <= server->tick)
        return false;
    return vector2_dist_squared(&source->plr.pos, &recipient->plr.pos) <=
        RELAY_COMBAT_RADIUS * RELAY_COMBAT_RADIUS;
}

static double relay_interval_for_tier(uint8_t tier)
{
    const RelayTier *current = &relay_tiers[tier];
    double interval =
        (double)current->interval_numerator /
        (double)(current->interval_denominator
            ? current->interval_denominator
            : 1);

    return interval > 0.0 ? interval : 1.0;
}

void server_relay_fill(Server *server)
{
    for (size_t source_index = 0;
         source_index < server->peers.capacity;
         source_index++)
    {
        PeerData *source = server->peers.ptr[source_index];
        if (!source || !source->in_game || !source->player_data_valid)
            continue;
        bool source_bypasses_aoi = relay_source_bypasses_aoi(source);

        for (size_t recipient_index = 0;
             recipient_index < server->peers.capacity;
             recipient_index++)
        {
            PeerData *recipient =
                server->peers.ptr[recipient_index];

            if (!recipient ||
                !recipient->in_game ||
                recipient->id == source->id)
                continue;

            if (!source_bypasses_aoi &&
                !relay_recipient_bypasses_aoi(recipient) &&
                !relay_pair_in_aoi(source, recipient))
                continue;

            relay_send_player_data(
                server,
                source,
                recipient);
        }
    }
}

void server_relay_tick(Server *server)
{
    uint8_t active = relay_active_alive_count(server);
    relay_update_tier(server, active);
    uint32_t relay_budget = RELAY_SEND_BUDGET;
    size_t source_start = server->game.relay_source_cursor;
    double interval = relay_interval_for_tier(server->game.relay_tier);
    if (interval < 1.0)
        interval = 1.0;

    for (size_t source_offset = 0;
         source_offset < server->peers.capacity;
         source_offset++)
    {
        size_t source_index =
            (source_start + source_offset) % server->peers.capacity;
        PeerData *source = server->peers.ptr[source_index];
        if (!source || !source->in_game || !source->player_data_valid)
            continue;

        bool heartbeat_due =
            source->relay_last_send_tick == 0 ||
            server->tick - source->relay_last_send_tick >=
                (uint32_t)g_config.relay_heartbeat_interval_ticks;

        bool correction_due =
            source->relay_last_send_tick == 0 ||
            server->tick - source->relay_last_send_tick >=
                (uint32_t)g_config.relay_correction_interval_ticks;

        bool normal_due =
            source->relay_next_elapsed <= (double)server->tick;

        bool state_changed =
            !source->relay_last_sent_valid ||
            source->last_relay_state != source->plr.state;

        bool source_due =
            normal_due ||
            heartbeat_due ||
            correction_due ||
            state_changed ||
            source->relay_force_pending ||
            source->relay_stop_pending;

        bool fill_due =
            interval >
                (double)g_config.relay_fill_interval_ticks &&
            (source->relay_last_fill_tick == 0 ||
             server->tick - source->relay_last_fill_tick >=
                 (uint32_t)g_config.relay_fill_interval_ticks);
        bool source_bypasses_aoi = relay_source_bypasses_aoi(source);

        size_t recipient_start =
            source_offset == 0 ? server->game.relay_recipient_cursor : 0;
        for (size_t recipient_offset = 0;
             recipient_offset < server->peers.capacity;
             recipient_offset++)
        {
            size_t recipient_index =
                (recipient_start + recipient_offset) % server->peers.capacity;
            if (relay_budget == 0)
            {
                server->game.relay_source_cursor = (uint16_t)source_index;
                server->game.relay_recipient_cursor = (uint16_t)recipient_index;
                return;
            }
            PeerData *recipient = server->peers.ptr[recipient_index];

            if (!recipient ||
                !recipient->in_game ||
                recipient->id == source->id)
                continue;

            bool pair_bypasses_aoi =
                source_bypasses_aoi ||
                relay_recipient_bypasses_aoi(recipient);
            bool pair_in_aoi =
                relay_pair_in_aoi(source, recipient);

            if (!pair_bypasses_aoi && !pair_in_aoi)
            {
                RelayCorrectionState *correction =
                    &source->relay_corrections[recipient->id];
                bool off_aoi_due =
                    correction->aoi_last_send_tick == 0 ||
                    server->tick - correction->aoi_last_send_tick >=
                        RELAY_AOI_OFF_ZONE_INTERVAL_TICKS;
                if (!off_aoi_due)
                    continue;

                relay_send_player_data(
                    server,
                    source,
                    recipient);
                relay_budget--;
                correction->aoi_last_send_tick = server->tick;
                continue;
            }

            source->relay_corrections[recipient->id].aoi_last_send_tick = 0;

            bool pair_priority =
                relay_pair_is_priority(
                    server,
                    source,
                    recipient);

            if (!pair_priority &&
                !source_due &&
                !fill_due)
                continue;

            relay_send_player_data(
                server,
                source,
                recipient);
            relay_budget--;
        }

        if (source_due)
        {
            source->relay_last_send_tick = server->tick;
            source->relay_next_elapsed =
                (double)server->tick + interval;
            source->relay_last_sent_valid = true;
            source->relay_force_pending = false;
            source->relay_stop_pending = false;
        }

        if (fill_due)
        {
            source->relay_last_fill_tick = server->tick;
        }
    }
    server->game.relay_source_cursor = 0;
    server->game.relay_recipient_cursor = 0;
}

static bool ping_send_record(Server *server, PeerData *source)
{
    Packet packet;
    PacketCreate(&packet, SERVER_GAME_PING);
    PacketWrite(&packet, packet_write16, source->id);
    PacketWrite(&packet, packet_write16, source->peer->roundTripTime);
    server_broadcast_ex(server, &packet, false, source->id);

    source->ping_reported_value = source->peer->roundTripTime;
    source->ping_last_report_tick = server->tick;
    source->ping_has_report = true;
    source->ping_initial_pending = false;
    return true;
}

static void ping_send_initial_batch(Server *server)
{
    if (server->tick < server->game.ping_initial_tick)
        return;

    uint8_t sent = 0;
    size_t scanned = 0;

    while (sent < PING_REPORTS_PER_TICK &&
           scanned < server->peers.capacity)
    {
        size_t index = (server->game.ping_cursor + scanned) % server->peers.capacity;
        PeerData *peer = server->peers.ptr[index];
        scanned++;

        if (!peer || !peer->in_game || !peer->ping_initial_pending)
            continue;

        ping_send_record(server, peer);
        sent++;
        server->game.ping_cursor = (uint16_t)((index + 1) % server->peers.capacity);
    }
}

static void ping_send_changed_reports(Server *server)
{
    size_t scanned = 0;
    while (scanned < server->peers.capacity)
    {
        size_t index =
            (server->game.ping_cursor + scanned) %
            server->peers.capacity;
        PeerData *peer = server->peers.ptr[index];
        scanned++;
        if (!peer || !peer->in_game || !peer->ping_has_report)
            continue;

        uint16_t candidate = peer->plr.ping_last;
        uint16_t difference =
            candidate > peer->ping_reported_value
                ? candidate - peer->ping_reported_value
                : peer->ping_reported_value - candidate;

        bool changed_enough = difference >= PING_REPORT_DELTA_MS;
        bool old_enough = server->tick - peer->ping_last_report_tick >= PING_REPORT_INTERVAL_TICKS;

        if (!changed_enough || !old_enough)
            continue;

        ping_send_record(server, peer);
        server->game.ping_cursor =
            (uint16_t)((index + 1) % server->peers.capacity);
        return;
    }
}

static void server_ping_tick(Server *server)
{
    if (server->state != ST_GAME || !server->game.started)
        return;

    ping_send_initial_batch(server);
    ping_send_changed_reports(server);
}

bool server_worker(Server *server)
{
	srand((unsigned int)time(NULL));

	char thread_name[128];
	snprintf(thread_name, 128, "Worker Thr %d", server->id);
	ThreadVarSet(g_threadName, thread_name);

#ifdef _WIN32
SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#elif defined(__unix) || defined(__unix__)
struct sched_param sp = { .sched_priority = 10 };
pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#endif

	TimeStamp ticker;
	time_start(&ticker);

	double next_tick = time_end(&ticker);
	const double TARGET_FPS = 1000.0 / 60;

	while (atomic_load_explicit(&server->running, memory_order_acquire))
	{
		ENetEvent ev;
		uint8_t peer_drained[MAX_SUPPORTED_PLAYERS + 1] = {0};
        uint32_t serviced_events = 0;

		/* Wait for the next tick inside enet_host_service rather than spinning
		   on a zero timeout: the old code burned the last ~2 ms of every 16.6 ms
		   tick in a busy loop, which costs ~10% of a core per lobby even when
		   nobody is connected. A lobby with no peers at all can wait far longer,
		   because an empty ST_LOBBY has no per-tick work to do. */
		bool lobby_is_idle =
			server->host->connectedPeers == 0 &&
			server->peers.noitems == 0 &&
			server->state == ST_LOBBY &&
			!server->lobby.vote.ongoing;

		int service_timeout;
		if (lobby_is_idle)
			service_timeout = SERVER_IDLE_SLEEP_MS;
		else
		{
			double remaining = next_tick - time_end(&ticker);
			if (remaining <= 0.0)
				service_timeout = 0;
			else
			{
				service_timeout = (int)(remaining + 0.5);
				if (service_timeout < 1)
					service_timeout = 1;
			}
		}
        while (serviced_events < MAX_SERVICE_EVENTS_PER_LOOP &&
               enet_host_service(server->host, &ev, service_timeout) > 0)
		{
            serviced_events++;
			service_timeout = 0;
			switch (ev.type)
			{
			case ENET_EVENT_TYPE_CONNECT:
			{
				Debug("ENET_EVENT_TYPE_CONNECT...");
				ev.peer->data = (PeerData *)malloc(sizeof(PeerData));
				if (!ev.peer->data)
					return false;

				memset(ev.peer->data, 0, sizeof(PeerData));

				PeerData *v = (PeerData *)ev.peer->data;
				v->server = server;
				v->peer = ev.peer;
				v->id = ev.peer->incomingPeerID + 1;
				enet_peer_ping_interval(ev.peer, 250);
				enet_peer_timeout(ev.peer, 64, 15000, 120000);
				/* Through the relay a peer arrives on loopback, so ask the tunnel
				   for the synthetic address it was given. Everything downstream -
				   bans, operators, timeouts, logs - then keys on that instead, and
				   the real address never existed on this machine to begin with. */
				if (!relay_lookup_ip(ev.peer->address.host, ev.peer->address.port,
						v->ip.value, sizeof(v->ip.value)) &&
					!webgate_lookup_ip(ev.peer->address.host, ev.peer->address.port,
						v->ip.value, sizeof(v->ip.value)))
					enet_address_get_host_ip(&ev.peer->address, v->ip.value, sizeof(v->ip.value));
				v->ip.value[sizeof(v->ip.value) - 1] = 0;
				v->ip.len = (uint16_t)(strlen(v->ip.value) + 1);

				Packet packet;
				PacketCreate(&packet, SERVER_PREIDENTITY);
				RAssert(auth_create_ticket(v, &packet));
				RAssert(packet_send(ev.peer, &packet, true));
				break;
			}

			case ENET_EVENT_TYPE_DISCONNECT:
			{
				Debug("ENET_EVENT_TYPE_DISCONNECT...");
				PeerData *v = (PeerData *)ev.peer->data;
				ev.peer->data = NULL;
				if (!v)
					break;

if (!v->op && v->should_timeout &&
    !(v->plr.flags & (PLAYER_DEAD | PLAYER_ESCAPED)))
				{
					uint64_t result;
					if (timeout_check(v->udid.value, v->ip.value, &result) && result == 0)
						timeout_set(v->nickname.value, v->udid.value, v->ip.value, time(NULL) + 5);
				}

				if (v->verified)
				{
					relay_invalidate_player_data(v);
					MutexLock(ip_addr_mut);
					{
						cJSON_DeleteItemFromObject(ip_addr_list, v->udid.value);
						cJSON_DeleteItemFromObject(ip_addr_list, v->ip.value);
					}
					MutexUnlock(ip_addr_mut);
				}

				/* Purge every reference before the free, regardless of how far the
				   peer got through identification. An entry left behind here would
				   be dereferenced by the next broadcast. */
				MutexLock(v->server->state_lock);
				{
					bool was_listed = false;
					while (dylist_remove(&v->server->peers, v))
						was_listed = true;

					if (was_listed && v->verified)
						server_state_left(v);
				}
				MutexUnlock(v->server->state_lock);

				Info("%s (id %d) " LOG_YLW "left.", v->nickname.value, v->id);
				free(v);
				break;
			}

			case ENET_EVENT_TYPE_RECEIVE:
			{
				PeerData *v = (PeerData *)ev.peer->data;
				if (!v)
				{
					enet_packet_destroy(ev.packet);
					continue;
				}

				if (v->id <= MAX_SUPPORTED_PLAYERS && peer_drained[v->id]++ >= 16)
				{
					enet_packet_destroy(ev.packet);
					continue;
				}
				Packet packet = packet_from(ev.packet);

				switch (packet.buff[1])
				{
				case IDENTITY:
				{
					if (!peer_identity(v, &packet))
					{
						Debug("Identity failed for id %d", v->id);
					}
					break;
				}
				default:
				{
					if (!peer_msg(v, &packet))
						break;
				}
				}

				break;
			}
			}
		}

		enet_host_flush(server->host);

		double now = time_end(&ticker);
		int catchup_count = 0;
		while (next_tick <= now && catchup_count < MAX_CATCHUP_TICKS)
		{
			next_tick += TARGET_FPS;
			catchup_count++;
			server->tick++;
			MutexLock(server->state_lock);
			{
				switch (server->state)
				{
				case ST_LOBBY:
				case ST_CHARSELECT:
				case ST_MAPVOTE:
					lobby_state_tick(server);
					break;

				case ST_GAME:
					game_state_tick(server);
					break;

				case ST_RESULTS:
					results_state_tick(server);
					break;
				}
				mod_on_tick(server);

				/* Votes run in every state, so a cheater can be kicked mid-match
				   and not just from the lobby. */
				if (server->peers.noitems > 0)
				{
					for (size_t i = 0; i < server->peers.capacity; i++)
					{
						PeerData *peer = (PeerData *)server->peers.ptr[i];
						if (peer && peer->vote_cooldown > 0)
							peer->vote_cooldown -= server->delta;
					}
				}

				if (server->lobby.vote.ongoing && !vote_tick(server, &server->lobby.vote))
					lobby_check_vote(server);

				if (server->peers.noitems > 0 &&
					server->tick % RELAY_HEARTBEAT_INTERVAL_TICKS == 0)
				{
					Packet pack;
					PacketCreate(&pack, SERVER_HEARTBEAT);
					server_broadcast(server, &pack, true);
				}

				server_ping_tick(server);
			}
			MutexUnlock(server->state_lock);
			server->delta = 1;
		}

		if (next_tick <= now)
			next_tick = now + TARGET_FPS;

		enet_host_flush(server->host);
	}

	enet_host_destroy(server->host);
	return true;
}

bool server_disconnect(Server *server, ENetPeer *peer, DisconnectReason reason, const char *text)
{
	if (server)
	{
		PeerData *data = (PeerData *)peer->data;
		if (data->disconnecting)
			return true;

enet_peer_disconnect(peer, reason);

		if (!text)
		{
			Info("Disconnected id %d %d: No text.", data->id, reason);
		}
		else
		{
			Info("Disconnected id %d %d: %s.", data->id, reason, text);
		}

		data->disconnecting = true;
	}
	else
		return false;

	return true;
}

bool server_disconnect_id(Server *server, uint16_t id, DisconnectReason reason, const char *text)
{
	if (server)
	{
		bool found = false;
		for (size_t i = 0; i < server->peers.capacity; i++)
		{
			PeerData *data = server->peers.ptr[i];
			if (!data)
				continue;

			if (data->id == id)
				return server_disconnect(server, data->peer, reason, text);
		}
	}
	else
		return false;

	return true;
}

int server_total(Server *server)
{
	int count = 0;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *peer = (PeerData *)server->peers.ptr[i];
		if (!peer)
			continue;

		count++;
	}

	return count;
}

int server_ingame(Server *server)
{
	int count = 0;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *peer = (PeerData *)server->peers.ptr[i];
		if (!peer)
			continue;

		if (peer->in_game)
			count++;
	}

	return count;
}

PeerData *server_find_peer(Server *server, uint16_t id)
{
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *v = (PeerData *)server->peers.ptr[i];
		if (!v)
			continue;

		if (v->id == id)
			return v;
	}

	return NULL;
}

bool server_broadcast(Server *server, Packet *packet, bool reliable)
{
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *v = (PeerData *)server->peers.ptr[i];
		if (!v)
			continue;

		packet_send(v->peer, packet, reliable);
	}

	return true;
}

bool server_broadcast_ex(Server *server, Packet *packet, bool reliable, uint16_t ignore)
{
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *v = (PeerData *)server->peers.ptr[i];
		if (!v)
			continue;

		if (v->id == ignore)
			continue;

		packet_send(v->peer, packet, reliable);
	}

	return true;
}

bool server_state_joined(PeerData *v)
{
	v->ping_initial_pending = true;
	v->ping_has_report = false;
	v->ping_last_report_tick = 0;

#ifdef SYS_USE_SDL2
	ui_update_playerlist(v->server);
#endif

	Packet pack;
	PacketCreate(&pack, SERVER_LOBBY_EXE_CHANCE);
	PacketWrite(&pack, packet_write8, v->exe_chance);
	RAssert(packet_send(v->peer, &pack, true));

	PacketCreate(&pack, SERVER_PLAYER_JOINED);
	PacketWrite(&pack, packet_write16, v->id);
	PacketWrite(&pack, packet_writestr, v->nickname);
	PacketWrite(&pack, packet_write8, v->lobby_icon);
	PacketWrite(&pack, packet_write8, v->pet);
	server_broadcast_ex(v->server, &pack, true, v->id);

	switch (v->server->state)
	{
	case ST_LOBBY:
	case ST_CHARSELECT:
	case ST_MAPVOTE:
		return lobby_state_join(v);

	case ST_GAME:
		return game_state_join(v);

	case ST_RESULTS:
		break;
	}

	return true;
}

bool server_state_handle(PeerData *v, Packet *packet)
{
	switch (v->server->state)
	{
	case ST_LOBBY:
	case ST_CHARSELECT:
	case ST_MAPVOTE:
		return lobby_state_handle(v, packet);

	case ST_GAME:
		return game_state_handletcp(v, packet);

	case ST_RESULTS:
		return results_state_handle(v, packet);
	}

	return true;
}

bool server_state_left(PeerData *v)
{
	if (v->verified)
		mod_on_leave(v);
#ifdef SYS_USE_SDL2
	ui_update_playerlist(v->server);
#endif

	Packet pack;
	PacketCreate(&pack, SERVER_PLAYER_LEFT);
	PacketWrite(&pack, packet_write16, v->id);
	server_broadcast(v->server, &pack, true);

	switch (v->server->state)
	{
	case ST_LOBBY:
	case ST_CHARSELECT:
	case ST_MAPVOTE:
		return lobby_state_left(v);

	case ST_GAME:
		return game_state_left(v);

	case ST_RESULTS:
		break;
	}

	return true;
}

unsigned long server_cmd_parse(String *string)
{
	static const char *clr_list[] = CLRLIST;

	String current = {.len = 0};
	bool found_digit = false;

	for (int i = 0; i < string->len; i++)
	{
		if (!found_digit && isspace(string->value[i]))
			continue;
		else
			found_digit = true;

		if (isspace(string->value[i]))
			break;

		bool invalid = false;
		for (int j = 0; j < CLRLIST_LEN; j++)
		{
			if (string->value[i] == clr_list[j][0])
			{
				invalid = true;
				break;
			}
		}

		if (invalid)
			continue;

		current.value[current.len++] = string->value[i];
	}
	current.value[current.len++] = '\0';

	unsigned int hash = 0;
	for (int i = 0; current.value[i] != '\0'; i++)
		hash = 31 * hash + current.value[i];

	return hash;
}

/* Copies a nickname with the chat colour codes stripped and lowercased, so
   ".report SomeGuy" matches a player whose name carries colour markup. Only the
   lead byte of each code is compared, matching server_cmd_parse. */
void report_normalize_name(const char *src, char *dst, size_t dst_size)
{
	static const char *clr_list[] = CLRLIST;
	size_t out = 0;

	for (size_t i = 0; src && src[i] && out + 1 < dst_size; i++)
	{
		bool is_colour = false;
		for (int j = 0; j < CLRLIST_LEN; j++)
		{
			if (src[i] == clr_list[j][0])
			{
				is_colour = true;
				break;
			}
		}

		if (is_colour)
			continue;

		dst[out++] = (char)tolower((unsigned char)src[i]);
	}

	if (dst_size)
		dst[out] = 0;
}

/* Resolves a ".report" argument to exactly one connected peer, by player id or
   by nickname. On failure returns NULL and points *problem at a player-facing
   explanation. Callers already hold the server state lock. */
PeerData *server_resolve_peer(Server *server, const char *query, const char **problem)
{
	char wanted[64];
	report_normalize_name(query, wanted, sizeof(wanted));

	if (!wanted[0])
	{
		*problem = CLRCODE_RED "say who: use a name or a player id";
		return NULL;
	}

	bool numeric = true;
	for (size_t i = 0; wanted[i]; i++)
	{
		if (!isdigit((unsigned char)wanted[i]))
		{
			numeric = false;
			break;
		}
	}

	unsigned long wanted_id = numeric ? strtoul(wanted, NULL, 10) : 0;

	PeerData *exact = NULL;
	PeerData *partial = NULL;
	int exact_hits = 0;
	int partial_hits = 0;

	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *peer = (PeerData *)server->peers.ptr[i];
		if (!peer || !peer->verified)
			continue;

		if (numeric && wanted_id <= UINT16_MAX && peer->id == (uint16_t)wanted_id)
			return peer;

		char name[64];
		report_normalize_name(peer->nickname.value, name, sizeof(name));

		if (strcmp(name, wanted) == 0)
		{
			exact = peer;
			exact_hits++;
		}
		else if (strstr(name, wanted))
		{
			partial = peer;
			partial_hits++;
		}
	}

	if (exact_hits == 1)
		return exact;

	if (exact_hits > 1)
	{
		*problem = CLRCODE_RED "several players use that name, use the player id instead";
		return NULL;
	}

	if (partial_hits == 1)
		return partial;

	if (partial_hits > 1)
	{
		*problem = CLRCODE_RED "that matches several players, be more specific";
		return NULL;
	}

	*problem = CLRCODE_RED "nobody here by that name";
	return NULL;
}

bool server_cmd_handle(Server *server, unsigned long hash, PeerData *v, String *msg)
{
	if (msg->value[0] != '.' && !mod_on_chat(v, msg->value))
		return true;  /* hidden by the mod */
	Packet pack;
	switch (hash)
	{
	default:
		return false;

	case CMD_BAN:
	{
		if (!v->op)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "you aren't an operator."));
			break;
		}

		int ingame = server_total(v->server);
		if (ingame <= 1)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "dude are you gonna ban yourself?"));
			break;
		}

		PacketCreate(&pack, SERVER_LOBBY_CHOOSEBAN);
		RAssert(packet_send(v->peer, &pack, true));
		break;
	}

	case CMD_KICK:
	{
		if (!v->op)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "you aren't an operator."));
			break;
		}

		if (server_total(v->server) <= 1)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "dude are you gonna kick yourself?"));
			break;
		}

		PacketCreate(&pack, SERVER_LOBBY_CHOOSEKICK);
		RAssert(packet_send(v->peer, &pack, true));
		break;
	}

	case CMD_OP:
	{
		if (!v->op)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "you aren't an operator."));
			break;
		}

		if (server_total(v->server) <= 1)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "you're already an operator tho??"));
			break;
		}

		PacketCreate(&pack, SERVER_LOBBY_CHOOSEOP);
		RAssert(packet_send(v->peer, &pack, true));
		break;
	}

	case CMD_LOBBY:
	{
		int ind;
		if (sscanf(msg->value, ".lobby %d", &ind) <= 0)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "example:~ .lobby 1"));
			break;
		}

		if (ind < 1 || ind > disaster_count())
		{
			char msg[128];
			snprintf(msg, 128, CLRCODE_RED "lobby should be between 1 and %d", disaster_count());
			RAssert(server_send_msg(v->server, v->peer, msg));
			break;
		}

		PacketCreate(&pack, SERVER_LOBBY_CHANGELOBBY);
		PacketWrite(&pack, packet_write32, relay_public_port(ind - 1));
		RAssert(packet_send(v->peer, &pack, true));
		break;
	}

	
	case CMD_REPORT:
	{
		const char *argument = strchr(msg->value, ' ');
		while (argument && *argument == ' ')
			argument++;

		if (!argument || !*argument)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "usage: .report name-or-id (reason)"));
			break;
		}

		/* Split "<target> <reason...>". */
		char target_text[64];
		size_t taken = 0;
		while (argument[taken] && !isspace((unsigned char)argument[taken]) &&
			taken + 1 < sizeof(target_text))
		{
			target_text[taken] = argument[taken];
			taken++;
		}
		target_text[taken] = 0;

		const char *reason = argument + taken;
		while (*reason == ' ')
			reason++;

		uint64_t now = (uint64_t)time(NULL);
		if (v->last_report_time && now >= v->last_report_time &&
			now - v->last_report_time < (uint64_t)g_config.report_cooldown_seconds)
		{
			char wait_msg[128];
			snprintf(wait_msg, sizeof(wait_msg), CLRCODE_RED "wait %u more seconds before reporting again",
				(unsigned)((uint64_t)g_config.report_cooldown_seconds - (now - v->last_report_time)));
			RAssert(server_send_msg(v->server, v->peer, wait_msg));
			break;
		}

		const char *problem = NULL;
		PeerData *target = server_resolve_peer(v->server, target_text, &problem);
		if (!target)
		{
			RAssert(server_send_msg(v->server, v->peer, problem));
			break;
		}

		if (target == v)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "you can't report yourself"));
			break;
		}

		const char *reason_text = (reason && *reason) ? reason : "no reason given";

		if (!report_add(v->nickname.value, target->nickname.value, target->udid.value,
			target->ip.value, reason_text, (int32_t)v->server->id))
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "could not save that report, tell the host"));
			break;
		}

		v->last_report_time = now;

		Info("%s " LOG_RST "(id %d) reported %s " LOG_RST "(id %d): %s",
			v->nickname.value, v->id, target->nickname.value, target->id, reason_text);

		char ack[192];
		snprintf(ack, sizeof(ack), CLRCODE_GRN "reported %s" CLRCODE_GRN " - the host will review it",
			target->nickname.value);
		RAssert(server_send_msg(v->server, v->peer, ack));
		break;
	}

case CMD_VK:
{
	if (v->server->lobby.vote.ongoing)
	{
		server_send_msg(v->server, v->peer, CLRCODE_RED "another vote is already in progress.");
		break;
	}

	if (v->vote_cooldown > 0)
	{
		char buffer[128];
		snprintf(buffer, 128, CLRCODE_RED "you cannot start another vote for %ds", (int)(v->vote_cooldown / TICKSPERSEC));
		server_send_msg(v->server, v->peer, buffer);
		break;
	}

	
	int ingame = server_ingame(v->server);
	if (ingame <= 2)
	{
		server_send_msg(v->server, v->peer, CLRCODE_RED "not enough participants.");
		break;
	}

	/* ".vk name-or-id" works without the client picker screen,
	   which matters on builds where that screen misbehaves. */
	const char *who = strchr(msg->value, ' ');
	while (who && *who == ' ')
		who++;

	if (!who || !*who)
	{
		PacketCreate(&pack, SERVER_LOBBY_CHOOSEVOTEKICK);
		RAssert(packet_send(v->peer, &pack, true));
		break;
	}

	const char *problem = NULL;
	PeerData *target = server_resolve_peer(v->server, who, &problem);
	if (!target)
	{
		server_send_msg(v->server, v->peer, problem);
		break;
	}

	if (target == v)
	{
		server_send_msg(v->server, v->peer, CLRCODE_RED "why are you kicking yourself ???");
		break;
	}

	if (target->op)
	{
		server_send_msg(v->server, v->peer, CLRCODE_RED "you're permissionless");
		break;
	}

	memcpy(&v->server->lobby.kick_target, target, sizeof(PeerData));

	if (!vote_init(v->server, &v->server->lobby.vote, VOTE_KICK, target->id))
	{
		server_send_msg(v->server, v->peer, CLRCODE_RED "not enough participants.");
		break;
	}

	char started[555];
	snprintf(started, 555, "%s~ " CLRCODE_RED "started kick vote for " CLRCODE_RST "%s" CLRCODE_RST,
		v->nickname.value, v->server->lobby.kick_target.nickname.value);

	server_broadcast_msg(v->server, "-----------------------");
	server_broadcast_msg(v->server, started);
	server_broadcast_msg(v->server, "type " CLRCODE_GRN ".yes~ to kick or " CLRCODE_RED ".no~ to keep");

	char window2[96];
	snprintf(window2, sizeof(window2), "results will be summarized in " CLRCODE_GRA "%d~ sec",
		g_config.votekick_vote_seconds);
	server_broadcast_msg(v->server, window2);
	server_broadcast_msg(v->server, "-----------------------");

	vote_add(&v->server->lobby.vote, v->id);
	v->vote_cooldown = g_config.votekick_start_cooldown_seconds * TICKSPERSEC;
	break;
}
case CMD_YES:
case CMD_Y:
{
	if (!v->server->lobby.vote.ongoing)
		break;

	if (!v->can_vote)
	{
		server_send_msg(v->server, v->peer, CLRCODE_RED "you can't participate in this vote.");
		break;
	}

	if (v->server->lobby.vote.type == VOTE_KICK && v->server->lobby.kick_target.id == v->id)
	{
		server_send_msg(v->server, v->peer, CLRCODE_RED "why are you kicking yourself ???");
		break;
	}

	switch (vote_add(&v->server->lobby.vote, v->id))
	{
		case 1:
		{
			char buffer[512];
			snprintf(buffer, 512, "%s~ " CLRCODE_GRN "voted~.", v->nickname.value);
			server_broadcast_msg(v->server, buffer);
			break;
		}

		case 0:
		{
			server_send_msg(v->server, v->peer, CLRCODE_RED "you have already voted.");
			break;
		}

		case -1:
		{
			lobby_check_vote(v->server);
			break;
		}
	}
	break;
}
case CMD_NO:
case CMD_N:
{
	if (!v->server->lobby.vote.ongoing)
		break;

	if (!v->can_vote)
	{
		server_send_msg(v->server, v->peer, CLRCODE_RED "you can't participate in this vote.");
		break;
	}

	VoteState state = vote_add_no(&v->server->lobby.vote, v->id);
	if (state == VOTE_ALREADYVOTED)
	{
		server_send_msg(v->server, v->peer, CLRCODE_RED "you have already voted.");
		break;
	}

	char buffer[512];
	snprintf(buffer, 512, "%s~ " CLRCODE_RED "voted no~.", v->nickname.value);
	server_broadcast_msg(v->server, buffer);

	if (vote_rejected(&v->server->lobby.vote))
	{
		server_broadcast_msg(v->server, CLRCODE_RED "vote failed~ - too many no votes");
		v->server->lobby.vote.ongoing = false;
	}
	else if (state == VOTE_FULL)
		lobby_check_vote(v->server);

	break;
}

	case CMD_PLAYERS:
	case CMD_WHO:
	{
		int shown = 0;
		int total = 0;

		for (size_t i = 0; i < v->server->peers.capacity; i++)
		{
			PeerData *peer = (PeerData *)v->server->peers.ptr[i];
			if (!peer || !peer->verified)
				continue;

			total++;
		}

		char header[96];
		snprintf(header, sizeof(header), CLRCODE_GRA "players online (%d):" CLRCODE_RST, total);
		RAssert(server_send_msg(v->server, v->peer, header));

		for (size_t i = 0; i < v->server->peers.capacity; i++)
		{
			PeerData *peer = (PeerData *)v->server->peers.ptr[i];
			if (!peer || !peer->verified)
				continue;

			/* Keep the flood bounded on a busy server. */
			if (shown >= 20)
			{
				char more[64];
				snprintf(more, sizeof(more), CLRCODE_GRA "... and %d more" CLRCODE_RST, total - shown);
				RAssert(server_send_msg(v->server, v->peer, more));
				break;
			}

			char line[192];
			snprintf(line, sizeof(line), CLRCODE_GRA "(%d)" CLRCODE_RST " %s%s",
				peer->id, peer->nickname.value, peer->op ? CLRCODE_GRN " (op)" CLRCODE_RST : "");
			RAssert(server_send_msg(v->server, v->peer, line));
			shown++;
		}

		RAssert(server_send_msg(v->server, v->peer,
			CLRCODE_GRA "use the number with .vk or .report" CLRCODE_RST));
		break;
	}

	case CMD_PING:
	{
		char line[96];
		snprintf(line, sizeof(line), "your ping is " CLRCODE_GRN "%u" CLRCODE_RST " ms",
			(unsigned)v->plr.ping_last);
		RAssert(server_send_msg(v->server, v->peer, line));
		break;
	}

	case CMD_COMMANDS:
	case CMD_CMDS:
	case CMD_HELP:
	{
		char lobby_msg[128];
		snprintf(lobby_msg, 128, CLRCODE_GRA ".lobby" CLRCODE_RST " choose lobby (1-%d)", disaster_count());

		RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".info" CLRCODE_RST " server info"));
		RAssert(server_send_msg(v->server, v->peer, lobby_msg));
		RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".players" CLRCODE_RST " who is online, with their numbers"));
		RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".ping" CLRCODE_RST " show your ping"));
		RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".vk name" CLRCODE_RST " start a kick vote on someone"));
		RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".yes or .no" CLRCODE_RST " vote while one is running"));
		RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".vp" CLRCODE_RST " vote practice mode"));
		RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".report name reason" CLRCODE_RST " report a cheater to the host"));

		if (v->op)
		{
			snprintf(lobby_msg, 128, CLRCODE_GRA ".map" CLRCODE_RST " choose map (1-%d)", MAP_COUNT + 1);
			RAssert(server_send_msg(v->server, v->peer, lobby_msg));

			RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".tm number" CLRCODE_RST " toggle a regular map"));
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".cm number" CLRCODE_RST " queue a map for the next match"));
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".kick" CLRCODE_RST " kick someone ig"));
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".ban" CLRCODE_RST " ban someone ig"));
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_GRA ".op" CLRCODE_RST " op someone ig"));
		}

		break;
	}

	
	case CMD_INFO:
	{
		char msg[100];
		snprintf(msg, 100, "server " CLRCODE_RED "%d" CLRCODE_RST " of " CLRCODE_BLU "%d" CLRCODE_RST, v->server->id + 1, g_config.server_count);

		server_send_msg(v->server, v->peer, "-----------------------");
		server_send_msg(v->server, v->peer, CLRCODE_RED "bestest" CLRCODE_BLU "server" CLRCODE_RST " v" STRINGIFY(BUILD_VERSION));
		server_send_msg(v->server, v->peer, "build from " CLRCODE_PUR __DATE__ " " CLRCODE_GRN __TIME__ CLRCODE_RST);
		server_send_msg(v->server, v->peer, msg);
		server_send_msg(v->server, v->peer, "-----------------------");
		server_send_msg(v->server, v->peer, CLRCODE_GRA "type .help for command list" CLRCODE_RST);
		server_send_motd(v->server, v->peer);
		break;
	}

	
	case CMD_STINK:
	{
		char buff[36];
		if (sscanf(msg->value, ".stink %35s", buff) <= 0)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "example:~ .stink baller"));
			break;
		}

		char format[100];
		snprintf(format, 100, "\\%s~, you /sti@nk~", buff);

		RAssert(server_broadcast_msg(v->server, format));
		break;
	}

	case CMD_TM:
	{
		if (!v->op)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "you aren't an operator."));
			break;
		}

		const char *argument = strchr(msg->value, ' ');
		NormalizedMap map;
		if (!argument || !server_normalize_map_argument(argument + 1, &map))
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "usage: .tm number"));
			break;
		}

		if (!map.regular_rotation)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "Fart Zone is not part of regular rotation"));
			break;
		}

		MutexLock(g_config.map_list_lock);
		g_config.map_list[map.index] = !g_config.map_list[map.index];
		bool enabled = g_config.map_list[map.index];
		MutexUnlock(g_config.map_list_lock);

		char tm_response[256];
		snprintf(tm_response, sizeof(tm_response), "%s is now %s",
			g_mapList[map.index].name,
			enabled ? "enabled" : "disabled");
		RAssert(server_send_msg(v->server, v->peer, tm_response));
		break;
	}

	case CMD_CM:
	{
		if (!v->op)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "you aren't an operator."));
			break;
		}

		const char *argument = strchr(msg->value, ' ');
		NormalizedMap map;
		if (!argument || !server_normalize_map_argument(argument + 1, &map))
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "usage: .cm number"));
			break;
		}

		if (!map.regular_rotation)
		{
			RAssert(server_send_msg(v->server, v->peer, CLRCODE_RED "Fart Zone cannot be queued as a regular map"));
			break;
		}

		bool replacing = v->server->lobby.pending_map_set;
		v->server->lobby.pending_map = map.index;
		v->server->lobby.pending_map_set = true;

		char cm_response[256];
		snprintf(cm_response, sizeof(cm_response), "%s %s queued for the next match",
			g_mapList[map.index].name,
			replacing ? "replacement" : "is");
		RAssert(server_broadcast_msg(v->server, cm_response));
		break;
	}
	}

	return true;
}

bool server_msg_handle(Server *server, PacketType type, PeerData *v, Packet *packet)
{
	switch (type)
	{
	default:
		break;

	case CLIENT_MOD_READY:
	case CLIENT_MOD_EVENT:
	case CLIENT_MOD_ANIM:
		mod_on_packet(v, packet, (uint8_t)type);
		break;

	case CLIENT_LOBBY_CHOOSEBAN:
	{
		if (!v->op)
			break;

		PacketRead(pid, packet, packet_read16, uint16_t);

		for (size_t i = 0; i < v->server->peers.capacity; i++)
		{
			PeerData *peer = (PeerData *)v->server->peers.ptr[i];
			if (!peer)
				continue;

			if (peer->id == pid)
			{
				RAssert(ban_add(peer->nickname.value, peer->udid.value, peer->ip.value));
				server_disconnect(v->server, peer->peer, DR_BANNEDBYHOST, NULL);
				break;
			}
		}
		break;
	}

	case CLIENT_LOBBY_CHOOSEKICK:
	{
		if (!v->op)
			break;

		PacketRead(pid, packet, packet_read16, uint16_t);

		for (size_t i = 0; i < v->server->peers.capacity; i++)
		{
			PeerData *peer = (PeerData *)v->server->peers.ptr[i];
			if (!peer)
				continue;

			if (peer->id == pid)
			{
				RAssert(timeout_set(peer->nickname.value, peer->udid.value, peer->ip.value, time(NULL) + 60));
				server_disconnect(v->server, peer->peer, DR_KICKEDBYHOST, NULL);
				break;
			}
		}
		break;
	}

	case CLIENT_LOBBY_CHOOSEOP:
	{
		if (!v->op)
			break;

		PacketRead(pid, packet, packet_read16, uint16_t);

		for (size_t i = 0; i < v->server->peers.capacity; i++)
		{
			PeerData *peer = (PeerData *)v->server->peers.ptr[i];
			if (!peer)
				continue;

			if (peer->id == pid)
			{
				peer->op = true;

				RAssert(op_add(peer->nickname.value, peer->ip.value));
				server_send_msg(v->server, peer->peer, CLRCODE_GRN "you're an operator now");
				break;
			}
		}
		break;
	}
	}

	return true;
}

bool server_send_msg(Server *server, ENetPeer *peer, const char *message)
{
	Packet pack;
	PacketCreate(&pack, CLIENT_CHAT_MESSAGE);
	PacketWrite(&pack, packet_write16, 0);
	PacketWrite(&pack, packet_writestr, string_lower(__Str(message)));

	RAssert(packet_send(peer, &pack, true));
	return true;
}

bool server_send_motd(Server *server, ENetPeer *peer)
{
	for (int i = 0; i < g_config.motd_lines && i < MOTD_MAX_LINES; i++)
	{
		if (!g_config.motd[i][0])
			continue;

		RAssert(server_send_msg(server, peer, g_config.motd[i]));
	}

	return true;
}

bool server_broadcast_msg(Server *server, const char *message)
{
	Packet pack;
	PacketCreate(&pack, CLIENT_CHAT_MESSAGE);
	PacketWrite(&pack, packet_write16, 0);
	PacketWrite(&pack, packet_writestr, string_lower(__Str(message)));

	server_broadcast(server, &pack, true);
	return true;
}
