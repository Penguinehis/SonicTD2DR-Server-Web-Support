#include <Mod.h>
#include <io/Time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <Player.h>
#include <States.h>
#include <CMath.h>
#include <DyList.h>
#include <Server.h>
#include <Palette.h>
#include <Colors.h>
#include <entities/Ring.h>
#include <entities/CreamRing.h>
#include <entities/BlackRing.h>
#include <entities/TailsProjectile.h>
#include <entities/EggmanTracker.h>
#include <entities/ExellerClone.h>

uint16_t normalize_ping(uint16_t ping, uint16_t subtract, uint16_t min_ping, uint16_t max_ping)
{
    if (ping > subtract)
        ping -= subtract;
    else
        ping = 0;

    if (min_ping > 0 && ping < min_ping)
        ping = min_ping;
    if (max_ping > 0 && ping > max_ping)
        ping = max_ping;

    return ping;
}

bool game_end(Server* server, Ending ending, bool achiv)
{
	if (server->game.end > 0)
		return true;
	mod_on_round_end(server, (int)ending);

	Packet packet;
	switch (ending)
	{
		case ED_EXEWIN:
		{
			PacketCreate(&packet, SERVER_GAME_EXE_WINS);
			PacketWrite(&packet, packet_write8, achiv);
			Info("Ending is ED_EXEWIN");
			break;
		}

		case ED_SURVWIN:
		{
			PacketCreate(&packet, SERVER_GAME_SURVIVOR_WIN);
			PacketWrite(&packet, packet_write8, achiv);
			Info("Ending is ED_SURVWIN");
			break;
		}

		case ED_TIMEOVER:
		{
			PacketCreate(&packet, SERVER_GAME_TIME_OVER);
			PacketWrite(&packet, packet_write8, achiv);
			Info("Ending is ED_TIMEOVER");
			break;
		}
	}

	server_broadcast(server, &packet, true);
	server->game.end = 5 * TICKSPERSEC;
	server->game.ending = ending;
	return true;
}

static int game_count_alive_survivors(Server* server);

static void game_onehitchecklist_mark(PeerData* data)
{
	Server* server = data->server;
	if (!g_config.gamemodes[9] ||
		!server->game.started ||
		server_is_exe(server, data->id) ||
		data->plr.onehit_checked)
		return;

	data->plr.onehit_checked = true;
	if (++server->game.onehit_checked_count >=
		server->game.match_initial_survivors)
if (!game_end(server, ED_EXEWIN, true))
return;
}

bool game_state_check(Server* server)
{
	if (server->game.end > 0)
		return true;

	uint8_t escaped = 0;
	uint8_t dead = 0;
	uint8_t exes = 0;

	int total = 0;
	{
		for (size_t i = 0; i < server->peers.capacity; i++)
		{
			PeerData* v = (PeerData*)server->peers.ptr[i];
			if (!v)
				continue;

			if(!v->in_game)
				continue;

			if (v->plr.flags & PLAYER_ESCAPED)
				escaped++;

			if (v->plr.flags & PLAYER_DEAD || v->plr.flags & PLAYER_DEMONIZED)
				dead++;

			if (server_is_exe(server, v->id))
				exes++;
		}

		total = server_ingame(server);
		total -= (exes + dead + escaped);
	}

	if (server->game.ffa)
	{   /* a free-for-all: its time, or the last EXE in it */
		int in = 0;
		for (int i = 0; i < server->game.exe_count; i++)
			if (server_find_peer(server, server->game.exes[i]))
				in++;
		if (in <= 1)
			RAssert(game_end(server, ED_EXEWIN, true));
		return true;
	}
	if (total <= 0 && server->game.exe_count == 0)
	{   /* no EXE: the survivors are all out (escaped: they win; else the time's) */
		RAssert(game_end(server, escaped > 0 ? ED_SURVWIN : ED_TIMEOVER, true));
		return true;
	}

	if (total <= 0)
	{
		if (escaped > 0)
		{
			RAssert(game_end(server, ED_SURVWIN, true));
		}
		else
		{
			RAssert(game_end(server, ED_EXEWIN, true));
		}
	}

if (g_config.gamemodes[4] && !server->game.last_survivor_disabled &&
game_count_alive_survivors(server) == 1)
RAssert(game_end(server, ED_SURVWIN, true));

	return true;
}

static int game_count_alive_survivors(Server* server)
{
	int n = 0;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* data = (PeerData*)server->peers.ptr[i];
		if (!data || !data->in_game)
			continue;
		if (server_is_exe(server, data->id))
			continue;
		if (data->plr.flags & (PLAYER_DEAD | PLAYER_DEMONIZED | PLAYER_ESCAPED))
			continue;
		n++;
	}
	return n;
}

static int game_stun_additive(Server *server)
{
return g_config.gamemodes[0] || g_config.gamemodes[2] ||
g_config.gamemodes[5] ? 5 : 3;
}

static int game_count_demonized(Server* server)
{
	int n = 0;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* data = (PeerData*)server->peers.ptr[i];
		if (!data || !data->in_game)
			continue;
		if (server_is_exe(server, data->id))
			continue;
		if (data->plr.flags & PLAYER_DEMONIZED)
			n++;
	}
	return n;
}

static bool game_first_half_guarantee_ready(Server* server, PeerData* victim)
{
	if (!server->game.last_minute_sweep_done)
		return false;
	if (!(victim->plr.flags & PLAYER_FIRST_HALF_DEAD))
		return false;

	int quota = server->game.match_initial_survivors / 2;
	if (quota < 1)
		quota = 1;
	return game_count_demonized(server) < quota;
}

bool game_init(int exe, int8_t map, Server* server)
{
	Debug("Attepting to enter ST_GAME...");
	RAssert(server_ingame(server) > 1);

	server->state = ST_GAME;
	server->game = (Game)
	{
		.map = map,
		.exe = server->lobby.exe_count ? (int)server->lobby.exes[0] : -1,
		.exe_count = server->lobby.exe_count,
		.ffa = server->lobby.ffa,
		.bring_state = BS_NONE,
		.bring_loc = (uint8_t)rand(),
.cream_guard_id = 0,
.onehit_checked_count = 0,
.last_survivor_disabled = false,
		.sudden_death = false,
		.started = false,
		.match_initial_survivors = 0,
		.death_order_count = 0,
		.last_minute_sweep_done = false,
		.sixty_second_sweep_done = false,
		.duel_triggered = false,
		.duel_ring_clear_at = 0.0,
.duel_boost_until = 0.0,
		.ring_spawn_disabled_until = 0.0,
		.ring_accum = 0.0,
.base_red_ring_chance = 30,
.active_red_ring_count = 0,
		.base_ring_coff = 0,
		.total_red_rings_spawned = 0,
		.total_red_rings_picked_up = 0,
		.end = 0.0,
		.entid = 0,
		.time = TICKSPERSEC,
		.elapsed = 0.0,
		.start_timeout = 30.0 * TICKSPERSEC
	};

	memcpy(server->game.exes, server->lobby.exes, sizeof server->game.exes);
	(void)exe;
	server->game.pending_demonize_capacity = (size_t)g_config.max_simultaneous_players;
	server->game.pending_demonize =
		calloc(server->game.pending_demonize_capacity,
			sizeof(*server->game.pending_demonize));
	RAssert(server->game.pending_demonize);

	memset(server->game.rings, 0, sizeof(server->game.rings));

	if (!dylist_create(&server->game.entities, GAME_ENTITY_CAPACITY))
		return false;
	Debug("Entity list created.");

	if (!dylist_create(&server->game.left, (size_t)g_config.max_simultaneous_players))
		return false;

	server->game.ping_initial_tick = server->tick + PING_INITIAL_DELAY_TICKS;
	Debug("Left players list created.");

	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* v = (PeerData*)server->peers.ptr[i];
		if (!v)
			continue;

		if (!v->in_game)
			continue;

v->ping_initial_pending = true;
v->ping_has_report = false;
v->ping_last_report_tick = 0;

		memset(&v->plr, 0, sizeof(Player));
		relay_invalidate_player_data(v);
		
		if (server_is_exe(server, v->id))
			SET_FLAG(v->plr.flags, PLAYER_KILLER);

		v->plr.ready = false;
		v->plr.mod_tool = v->mod_tool;
		time_start(&v->plr.tails_last_proj);

		for (int i = 0; i < 5; i++)
			v->plr.revival_init[i] = -1;
	}

	

	{
		Packet char_sync;
		for (size_t cs = 0; cs < server->peers.capacity; cs++)
		{
			PeerData* cp = (PeerData*)server->peers.ptr[cs];
			if (!cp || !cp->in_game)
				continue;

			PacketCreate(&char_sync, SERVER_LOBBY_CHARACTER_CHANGE);
			PacketWrite(&char_sync, packet_write16, cp->id);
			PacketWrite(&char_sync, packet_write8,
				server_is_exe(server, cp->id)
					? (uint8_t)cp->exe_char
					: (uint8_t)(cp->surv_char + 1));
			server_broadcast(server, &char_sync, true);
		}
	}

	Packet pack;
	PacketCreate(&pack, SERVER_LOBBY_GAME_START);
	server_broadcast(server, &pack, true);

	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* v = (PeerData*)server->peers.ptr[i];
		if (!v)
			continue;

		if (v->in_game)
			continue;

		for (size_t j = 0; j < server->peers.capacity; j++)
		{
			PeerData* er = (PeerData*)server->peers.ptr[j];
			if (!er)
				continue;

			if (er->id == v->id)
				continue;

			PacketCreate(&pack, SERVER_WAITING_PLAYER_INFO);
			PacketWrite(&pack, packet_write8, er->in_game);
			PacketWrite(&pack, packet_write16, er->id);
			PacketWrite(&pack, packet_writestr, er->nickname);

			if (er->in_game)
			{
				PacketWrite(&pack, packet_write8, server_is_exe(server, er->id));
				PacketWrite(&pack, packet_write8, server_is_exe(server, er->id) ? er->exe_char : er->surv_char);
			}
			else
			{
				PacketWrite(&pack, packet_write8, er->lobby_icon);
			}

			RAssert(packet_send(v->peer, &pack, true));
		}
	}

	Info(LOG_YLW "Server is now in " LOG_PUR "Game");
	return true;
}

bool game_uninit(Server* server, bool show_results)
{
	if (show_results)
		return results_init(server);
	else
	{
		for (size_t i = 0; i < server->game.entities.capacity; i++)
		{
			Entity* entity = (Entity*)server->game.entities.ptr[i];
			if (!entity)
				continue;

			free(entity);
		}
		dylist_free(&server->game.entities);

		
		for (size_t i = 0; i < server->game.left.capacity; i++)
		{
			PeerData* data = (PeerData*)server->game.left.ptr[i];
			if (!data)
				continue;

			free(data);
		}
		dylist_free(&server->game.left);
		free(server->game.pending_demonize);
		server->game.pending_demonize = NULL;

		return lobby_init(server);
	}
}

bool game_spawn(Server* server, Entity* entity, size_t len, Entity** out)
{
	entity->id = ++server->game.entid;
	Debug("Allocated entity \"%s\" (id %d, size %d)", entity->tag, entity->id, len);

	Entity* ent = (Entity*)malloc(len);
	if(!ent)
		return false;

	memcpy(ent, entity, len);
	if (ent->init && !ent->init(server, ent))
	{
		free(ent);
		return false;
	}

	dylist_push(&server->game.entities, ent);
	if (out)
		*out = ent;
	return true;
}

bool game_despawn(Server* server, Entity** out, uint16_t id)
{
	bool res = true;
	Entity* tar = NULL;

	for (size_t i = 0; i < server->game.entities.capacity; i++)
	{
		Entity* entity = (Entity*)server->game.entities.ptr[i];
		if (!entity)
			continue;

		if (entity->id == id)
		{
			if (entity->uninit && !entity->uninit(server, entity))
				Warn("uninit failed for entity %d", entity->id);

			Debug("Deallocated entity \"%s\" (id %d)", entity->tag, entity->id);
			tar = entity;
			break;
		}
	}

	res = dylist_remove(&server->game.entities, tar);
	if (res && !out)
		free(tar);
	else if (out)
		*out = tar;
	else
		Warn("Failed to find entity %d", id);

	return res;
}

int game_find(Server* server, Entity** out, char* tag, size_t count)
{
	int i = 0;

	for (size_t it = 0; it < server->game.entities.capacity; it++)
	{
		Entity* entity = (Entity*)server->game.entities.ptr[it];
		if (!entity)
			continue;

		Debug("Search %s (id %d) vs %s", entity->tag, entity->id, tag);
		if (strcmp(entity->tag, tag) == 0)
		{
			if (!out)
				i++;
			else
				out[i++] = entity;

			if ((size_t)i >= count)
				break;
		}
	}

	Debug("Search for \"%s\" found %d entities", tag, i);
	return i;
}

bool game_bigring(Server* server, BigRingState state)
{
	RAssert(server);

if (g_config.gamemodes[6] && state == BS_ACTIVATED)
return true;

	if (server->game.bring_state == state)
		return true;

	Packet packet;
	switch (state)
	{
		case BS_NONE:
			break;

		case BS_DEACTIVATED:
		{
			Info("Big ring is deactivated!");

			PacketCreate(&packet, SERVER_GAME_SPAWN_RING);
			PacketWrite(&packet, packet_write8, 0);
			PacketWrite(&packet, packet_write8, server->game.bring_loc);
			server_broadcast(server, &packet, true);
			break;
		}

		case BS_ACTIVATED:
		{
			Info("Big ring is activated!");

			PacketCreate(&packet, SERVER_GAME_SPAWN_RING);
			PacketWrite(&packet, packet_write8, 1);
			PacketWrite(&packet, packet_write8, server->game.bring_loc);
			server_broadcast(server, &packet, true);
			break;
		}
	}

	server->game.bring_state = state;
	return true;
}

bool game_checkstart(Server* server)
{
	if (server->game.started)
		return true;

	uint8_t cnt = 0;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* v = (PeerData*)server->peers.ptr[i];
		if (!v)
			continue;
		
		if(!v->in_game)
			continue;

		if (v->plr.ready)
			cnt++;
	}

	if (cnt >= server_ingame(server))
	{
		Packet pack;
		PacketCreate(&pack, SERVER_GAME_PLAYERS_READY);
		server_broadcast(server, &pack, true);

		srand((unsigned int)time(NULL));
		RAssert(g_mapList[server->game.map].cb.init(server));

if (g_config.gamemodes[3])
{
server->game.time_sec = 9 * 60;
server->game.time = 0;
}
else if (!g_config.vanilla_balancing)
		{
			int n = server_ingame(server);
			double duration;
			if (n <= 2)
			{
				duration = 120.0;
			}
			else if (n <= 8)
			{
				const double seg1_a = 231.111111;
				const double seg1_b = 111.111111;
				const double seg1_r = 0.46;
				duration = seg1_a - seg1_b *
					pow(seg1_r, (double)(n - 2));
			}
			else
			{
				const double seg1_a = 231.111111;
				const double seg1_b = 111.111111;
				const double seg1_r = 0.46;
				double f1_8 = seg1_a - seg1_b * pow(seg1_r, 6.0);
				const double span_seconds = 370.0;
				const double frac_at_100 = 0.90;
				double b2 = span_seconds / frac_at_100;
				double a2 = f1_8 + b2;
				double r2 = pow(1.0 - frac_at_100, 1.0 / 92.0);
				duration = a2 - b2 * pow(r2, (double)(n - 8));
			}

			duration *= 1.15;
			double result = round(duration);
			if (result < 120.0)
				result = 120.0;

			{
				const double seg1_a = 231.111111;
				const double seg1_b = 111.111111;
				const double seg1_r = 0.46;
				double f1_8_cap = seg1_a - seg1_b * pow(seg1_r, 6.0);
				const double span_seconds = 370.0;
				const double frac_at_100 = 0.90;
				double b2_cap = span_seconds / frac_at_100;
				double a2_cap = f1_8_cap + b2_cap;
				double hard_cap = round(a2_cap * 1.15);
				if (result > hard_cap)
					result = hard_cap;
			}

			server->game.time_sec = (uint16_t)result;
			server->game.time = 0;
		}

mod_round_time(server);
mod_on_round_start(server);
Packet time_packet;
PacketCreate(&time_packet, SERVER_GAME_TIME_SYNC);
PacketWrite(&time_packet, packet_write16,
(uint16_t)server->game.time_sec * TICKSPERSEC);
server_broadcast(server, &time_packet, true);

		Info(LOG_YLW "Game started! " LOG_RST "(Time %ds)", server->game.time_sec);

		server->game.started = true;
		server->game.match_initial_survivors = server_ingame(server) - server->game.exe_count;   /* (a mod's round: several EXEs, or none) */
server->game.last_survivor_disabled =
server->game.match_initial_survivors == 1;
if (g_config.gamemodes[0])
for (size_t i = 0; i < server->peers.capacity; i++)
{
PeerData *data = (PeerData *)server->peers.ptr[i];
if (data && data->in_game && data->surv_char == CH_CREAM)
server->game.cream_guard_id = data->id;
}
	}

	return true;
}

static bool game_demonize_inner(Server* server, PeerData* data, bool force);

bool game_demonize(Server* server, PeerData* data, bool force)
{
	bool was = (data->plr.flags & PLAYER_DEMONIZED) != 0;
	bool r = game_demonize_inner(server, data, force);
	if (r && !was && (data->plr.flags & PLAYER_DEMONIZED))
		mod_on_demonize(data);
	return r;
}

static bool game_demonize_inner(Server* server, PeerData* data, bool force)
{
	Packet pack;
	PacketCreate(&pack, SERVER_GAME_DEATHTIMER_END);

if (g_config.gamemodes[1])
force = true;
else if (g_config.gamemodes[2])
{
SET_FLAG(data->plr.flags, PLAYER_CANTREVIVE);
PacketWrite(&pack, packet_write8, 0);
RAssert(packet_send(data->peer, &pack, true));
return true;
}

if (server->game.sixty_second_sweep_done && !g_config.gamemodes[1])
	{
		SET_FLAG(data->plr.flags, PLAYER_CANTREVIVE);
		PacketWrite(&pack, packet_write8, 0);
		RAssert(packet_send(data->peer, &pack, true));
		return true;
	}

	int players = 0;
	int demonized = 0;

	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* v = (PeerData*)server->peers.ptr[i];
		if (!v)
			continue;
		
		if(!v->in_game)
			continue;

		if (server_is_exe(server, v->id))
			continue;
		
		if (v->plr.flags & PLAYER_DEMONIZED)
			demonized++;

		players++;
	}

	int cap;
	if (!g_config.vanilla_balancing)
	{
		if (server->game.last_minute_sweep_done)
			cap = (players + 1) / 2;
		else
		{
			const double start_n = 12.0;
			const double start_pct = 25.0;
			const double end_n = 70.0;
			const double end_pct = 10.0;
			double pct = players <= (int)start_n
				? start_pct
				: start_pct + (end_pct - start_pct) *
					((players - start_n) / (end_n - start_n));
			if (pct < end_pct)
				pct = end_pct;
			cap = (int)(players * pct / 100.0);
		}
	}
	else
		cap = players / 2;

	if (cap < 1)
		cap = 1;

	if (force || demonized < cap)
	{
		DEL_FLAG(data->plr.flags, PLAYER_DEAD);
		SET_FLAG(data->plr.flags, PLAYER_DEMONIZED);
		relay_invalidate_player_data(data);
		
		data->plr.stats.rings = 0;

		Info("%s " LOG_RST "(id %d)" LOG_RED " was demonized!", data->nickname.value, data->id);
		PacketWrite(&pack, packet_write8, 1);
	}
	else
	{
		SET_FLAG(data->plr.flags, PLAYER_CANTREVIVE);

		Info("%s " LOG_RST "(id %d)" LOG_RED " died!", data->nickname.value, data->id);
		PacketWrite(&pack, packet_write8, 0);
	}

	RAssert(packet_send(data->peer, &pack, true));
	return true;
}

bool game_state_join(PeerData* v)
{
	(void)v; 
	return true;
}

bool game_state_left(PeerData* v)
{
	if (v->server->game.end > 0)
		return true;

	if (!v->in_game)
		return true;

	RAssert(g_mapList[v->server->game.map].cb.left(v));

	if(server_ingame(v->server) <= 1)
		return game_uninit(v->server, false);

	if (!v->server->game.started)
	{
		if (server_is_exe(v->server, v->id) && v->server->game.exe_count <= 1)
			return game_uninit(v->server, false);

		return game_checkstart(v->server);
	}

	game_onehitchecklist_mark(v);

	PeerData* data = (PeerData*)malloc(sizeof(PeerData));
	RAssert(data);
	memcpy(data, v, sizeof(PeerData));

	
	SET_FLAG(data->plr.flags, PLAYER_LEFT);
	dylist_push(&v->server->game.left, data);

	if (server_is_exe(v->server, v->id))
	{
		Game *gm = &v->server->game;
		int k = 0;
		for (int i = 0; i < gm->exe_count; i++)
			if (gm->exes[i] != v->id && server_find_peer(v->server, gm->exes[i]))
				gm->exes[k++] = gm->exes[i];
		gm->exe_count = (uint8_t)k;   /* (its results row: PLAYER_KILLER, set at the start) */
		if (k == 0)
			return game_end(v->server, ED_SURVWIN, gm->elapsed >= TICKSPERSEC * TICKSPERSEC);
		if (gm->exe == v->id)
			gm->exe = gm->exes[0];
		if (gm->ffa && k == 1)
			return game_end(v->server, ED_EXEWIN, true);
	}

	RAssert(game_state_check(v->server));
	return true;
}

bool game_state_handletcp(PeerData* v, Packet* packet)
{
	if (v->plr.flags & PLAYER_FROZEN_DEMON)
		return true;

	
	PacketRead(passtrough, packet, packet_read8, uint8_t);
	PacketRead(type, packet, packet_read8, uint8_t);

	uint8_t stats_type = 0xff;
	if (type == CLIENT_STATS_REPORT)
		RAssert(packet_read8(packet, &stats_type));

	if (v->plr.flags & (PLAYER_DEAD | PLAYER_ESCAPED))
	{
		switch (type)
		{
			case CLIENT_CHAT_MESSAGE:
			case CLIENT_PING:
			case CLIENT_PLAYER_DATA:
				break;

			case CLIENT_STATS_REPORT:
				if (stats_type != 2)
					return true;
				break;

			default:
				return true;
		}
	}

	switch (type)
	{
		default:
			RAssert(server_msg_handle(v->server, type, v, packet));
			break;
			
		case CLIENT_PLAYER_POTATER:
		case CLIENT_SOUND_EMIT:
		case CLIENT_SPAWN_EFFECT: 
		case CLIENT_PET_PALETTE:
		case CLIENT_SPRING_USE:
		case CLIENT_MERCOIN_BONUS:
		case CLIENT_RING_BROKE:
		{
			AssertOrDisconnect(v->server, v->in_game);
			server_broadcast_ex(v->server, packet, true,  v->id);
			break;
		}

		case CLIENT_PLAYER_PALETTE:
		{
			AssertOrDisconnect(v->server, v->in_game);
			/* any colours (skins, the custom editor): it is not a cheat; only the packet's shape is checked */
			if (!palette_player_check(v, packet))
				palette_player_reset(v);
			else
				server_broadcast_ex(v->server, packet, true, v->id);
			break;
		}

		case CLIENT_PLAYER_HEAL_PART:
		{
			AssertOrDisconnect(v->server, v->in_game);
			PacketRead(x, packet, packet_read16, uint16_t);
			PacketRead(y, packet, packet_read16, uint16_t);
			PacketRead(rings, packet, packet_read16, uint16_t);
			v->plr.heal_rings = v->plr.rings;

			if (rings < 10)
			{
				server_disconnect(v->server, v->peer, DR_OTHER, "pusy");
				return true;
			}

			if (rings >= 140 && v->server->game.map != 20 && !g_mod.active)
			{
				server_disconnect(v->server, v->peer, DR_OTHER, "dicus");
				return true;
			}

			server_broadcast_ex(v->server, packet, true, v->id);
			break;
		}

		case CLIENT_PLAYER_HEAL:
		{
			AssertOrDisconnect(v->server, v->in_game);
			PacketRead(id, packet, packet_read16, uint16_t);
			PacketRead(rings, packet, packet_read16, uint16_t);

			if (rings < 10)
			{
				server_disconnect(v->server, v->peer, DR_OTHER, "pusy");
				return true;
			}

			if (rings >= 140 && v->server->game.map != 20 && !g_mod.active)
			{
				server_disconnect(v->server, v->peer, DR_OTHER, "dicus");
				return true;
			}

			if (v->plr.mod_tool)
			{
				Packet pack;
				PacketCreate(&pack, SERVER_RING_COLLECTED);
				PacketWrite(&pack, packet_write8, 0);
				PacketWrite(&pack, packet_write16, 0);
				PacketWrite(&pack, packet_write8, true);
				PacketWrite(&pack, packet_write8, false);
				RAssert(packet_send(v->peer, &pack, true));
				break;
			}

			v->plr.heal_rings = 0;
			v->plr.stats.hp_restored++;

			server_broadcast_ex(v->server, packet, true, v->id);
			break;
		}

		case CLIENT_STATS_REPORT:
		{
			AssertOrDisconnect(v->server, v->in_game);

			switch (stats_type)
			{
				case 0:
				{
					v->plr.stats.hp_restored++;
					break;
				}

				case 1:
				{
					PacketRead(recv, packet, packet_read16, uint16_t);
					PacketRead(dmgr, packet, packet_read16, uint16_t);
					PacketRead(sec, packet, packet_read8, uint8_t);

					PeerData* rec = server_find_peer(v->server, recv);
					PeerData* damager = server_find_peer(v->server, dmgr);
					if (!rec || !damager)
						break;
					if (g_mod.active)
						mod_on_hurt(damager, rec, 0, sec);  /* (a stun: the victim reports it) */

					rec->plr.stats.stun_time += sec;
					damager->plr.stats.stuns++;
if ((!g_config.vanilla_balancing || g_config.gamemodes[0] ||
g_config.gamemodes[2] || g_config.gamemodes[5]) &&
						server_is_exe(v->server, rec->id) && !v->server->game.ffa &&
						v->server->game.time_sec > 120)
					{
v->server->game.time_sec += game_stun_additive(v->server);
						Packet tsync;
						PacketCreate(&tsync, SERVER_GAME_TIME_SYNC);
						PacketWrite(&tsync, packet_write16,
							(uint16_t)v->server->game.time_sec * TICKSPERSEC);
						server_broadcast(v->server, &tsync, false);
					}
if (g_config.gamemodes[5] && server_is_exe(v->server, rec->id) && !v->server->game.ffa &&
rec->plr.stats.stun_time >=
((9 + (2 * v->server->game.match_initial_survivors)) > 60 ?
60 : (9 + (2 * v->server->game.match_initial_survivors))))
RAssert(game_end(v->server, ED_SURVWIN, true));
					break;
				}

				case 2:
				{
					PacketRead(id, packet, packet_read16, uint16_t);
					PacketRead(dmg, packet, packet_read16, uint16_t);
					PacketRead(hp_wire, packet, packet_read16, uint16_t);
					int16_t hp = (int16_t)hp_wire;
					
					PeerData* data = server_find_peer(v->server, id);
					if (data && g_mod.active)
						mod_on_hurt(data, v, dmg, 0);  /* (the victim reports who hit it, and how hard) */
					if (!data || !data->in_game || !data->peer)
						break;

					if (dmg > 0)
						game_onehitchecklist_mark(data);

					if (hp <= 0 && !v->plr.kill_reported)
					{
						data->plr.stats.kills++;
						v->plr.kill_reported = true;
					}

					data->plr.stats.damage_raw += dmg;
					data->plr.stats.damage = data->plr.stats.damage_raw / 20;
					break;
				}

				case 3:
				{
					PacketRead(dmg, packet, packet_read8, uint8_t);
					if (dmg > 0)
						game_onehitchecklist_mark(v);
v->plr.stats.damage_taken_raw += dmg;
					v->plr.stats.damage_taken =
						v->plr.stats.damage_taken_raw / 20;
					break;
				}
			}
			break;
		}

		case CLIENT_PLAYER_HURT:
		{
			AssertOrDisconnect(v->server, v->in_game);
			server_broadcast_ex(v->server, packet, true, v->id);
			break;
		}

		case CLIENT_TPROJECTILE_STARTCHARGE:
		{
			AssertOrDisconnect(v->server, v->in_game);
			time_start(&v->plr.tails_last_proj);
			break;
		}

		case CLIENT_TPROJECTILE:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, !server_is_exe(v->server, v->id));
			AssertOrDisconnect(v->server, v->surv_char == CH_TAILS);

			PacketRead(x, packet, packet_read16, uint16_t);
			PacketRead(y, packet, packet_read16, uint16_t);
			PacketRead(dir, packet, packet_read8, int8_t);
			PacketRead(dmg, packet, packet_read8, uint8_t);
			PacketRead(exe, packet, packet_read8, uint8_t);
			PacketRead(chg, packet, packet_read8, uint8_t);
			AssertOrDisconnect(v->server, dir >= -1 && dir <= 1);

			if (v->mod_tool)
			{
				if (dir > 0)
					dir = -1;
				else if (dir < 0)
					dir = 1;
			}

			bool check_balls = true;
			if (v->plr.flags & PLAYER_DEMONIZED)
			{
				AssertOrDisconnect(v->server, dmg <= 60);

				if (dmg >= 60)
				{
					double last = time_end(&v->plr.tails_last_proj);
					check_balls = last > 1500 && last < 12000;
					AssertOrDisconnect(v->server, check_balls);
				}
			}
			else
			{
				AssertOrDisconnect(v->server, dmg <= 6);

				if (dmg >= 6)
				{
					double last = time_end(&v->plr.tails_last_proj);
					check_balls = last > 1500 && last < 12000;
					AssertOrDisconnect(v->server, check_balls);
				}
			}

			if (!check_balls)
				break;

			RAssert(game_spawn(v->server, (Entity*)&(MakeTailsProj(x, y, v->id, dir, v->plr.flags & PLAYER_DEMONIZED, chg, dmg)), sizeof(TProjectile), NULL));
			break;
		}

		case CLIENT_TPROJECTILE_HIT:
		{
			AssertOrDisconnect(v->server, v->in_game);

Entity* ents;
if (game_find(v->server, &ents, "tproj", 1))
{
TProjectile* projectile = (TProjectile*)ents;
if (g_config.vanilla_balancing)
game_despawn(v->server, NULL, projectile->id);
			}

			break;
		}

		case CLIENT_RING_COLLECTED:
		{
			AssertOrDisconnect(v->server, v->in_game);
			PacketRead(id, packet, packet_read8, uint8_t);
			PacketRead(eid, packet, packet_read16, uint16_t);

			Ring* ent = NULL;
			bool res = game_despawn(v->server, (Entity**)&ent, eid);
			if (res)
			{
				PeerData* data = server_find_peer(v->server, v->id);
				RAssert(data);

if (!ent->red)
				{
					time_start(&data->plr.last_rings);
					data->plr.rings++;
					data->plr.stats.rings++;
					mod_on_ring(data);
				}
				else
{
if (v->server->game.active_red_ring_count > 0)
v->server->game.active_red_ring_count--;
					v->server->game.total_red_rings_picked_up++;
}

				Packet pack;
				PacketCreate(&pack, SERVER_RING_COLLECTED);
				PacketWrite(&pack, packet_write8, id);
				PacketWrite(&pack, packet_write16, eid);
				PacketWrite(&pack, packet_write8, ent->red);
				PacketWrite(&pack, packet_write8, data->plr.rings > 0);

				free(ent);
				RAssert(packet_send(v->peer, &pack, true));
			}
			break;
		}

		case CLIENT_CREAM_SPAWN_RINGS:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, !server_is_exe(v->server, v->id));
			AssertOrDisconnect(v->server, v->surv_char == CH_CREAM);
			if (v->mod_tool)
			{
				Packet pack;
				PacketCreate(&pack, SERVER_RING_COLLECTED);
				PacketWrite(&pack, packet_write8, 0);
				PacketWrite(&pack, packet_write16, 0);
				PacketWrite(&pack, packet_write8, true);
				PacketWrite(&pack, packet_write8, false);
				RAssert(packet_send(v->peer, &pack, true));
				break;
			}

			PacketRead(x, packet, packet_read16, uint16_t);
			PacketRead(y, packet, packet_read16, uint16_t);
			PacketRead(red_ring, packet, packet_read8, uint8_t);

			Vector2 pos = { (float)x, (float)y };
			AssertOrDisconnect(v->server, vector2_dist(&pos, &v->plr.pos) <= 40);

			static double PI = 0.0;
			if(PI == 0.0) 
				PI = acos(-1);

			if (red_ring)
			{				
				CreamRing* rings[MAX_CREAM_RINGS_QUERY];
				int cnt = game_find(v->server, (Entity**)rings,
					"cring", MAX_CREAM_RINGS_QUERY);
				
				bool red_ring_space_available = true;
				for (int i = 0; i < cnt; i++)
				{
					CreamRing* ring = rings[i];
					if (ring->red && vector2_dist(&ring->pos, &pos) < 150)
					{
						red_ring_space_available = false;
						break;
					}
				}
				if (!red_ring_space_available)
					break;
				
				int16_t posX[2] = { 25, -27 };
				int16_t posY[2] = { 0, 0 };
				for (int i = 0; i < 2; i++)
				{
					uint16_t rX = x + posX[i];
					uint16_t rY = y + posY[i];

					RAssert(game_spawn(v->server, (Entity*)&(MakeCreamRing(rX, rY, red_ring)), sizeof(CreamRing), NULL));
				}
			}
			else
			{
				static const int16_t posX[7] =
					{ 26, 0, -27, 36, 0, -37, 0 };
				static const int16_t posY[7] =
					{ 0, -26, 0, 0, -36, 0, 36 };

				int alive_cream = 0;
				if (!g_config.vanilla_balancing)
				{
					for (size_t i = 0; i < v->server->peers.capacity; i++)
					{
						PeerData* d = (PeerData*)v->server->peers.ptr[i];
						if (d && d->in_game && d->surv_char == CH_CREAM
							&& !(d->plr.flags & (PLAYER_DEAD | PLAYER_DEMONIZED)))
							alive_cream++;
					}
				}
				int spawn_count = g_config.vanilla_balancing ? 3 : (alive_cream > 4 ? 3 : 7 - alive_cream);
				if (spawn_count < 3) spawn_count = 3;

				for (int i = 0; i < spawn_count; i++)
					RAssert(game_spawn(v->server, (Entity*)&(MakeCreamRing(x + posX[i], y + posY[i], red_ring)), sizeof(CreamRing), NULL));
			}

			break;
		}

		case CLIENT_ETRACKER:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, !server_is_exe(v->server, v->id));
			AssertOrDisconnect(v->server, v->surv_char == CH_EGGMAN);
			PacketRead(x, packet, packet_read16, uint16_t);
			PacketRead(y, packet, packet_read16, uint16_t);
			
game_spawn(v->server, (Entity*)&(MakeEggTrack(x, y)),
				sizeof(EggTracker), NULL);
			break;
		}

		case CLIENT_ETRACKER_ACTIVATED:
		{
			AssertOrDisconnect(v->server, v->in_game);
			PacketRead(eid, packet, packet_read16, uint16_t);
			
			Entity* ents[50];
			int found = game_find(v->server, ents, "eggtrack", 50);

			for (int i = 0; i < found; i++)
			{
				EggTracker* entity = (EggTracker*)ents[i];
				
if (entity->id == eid)
				{
					entity->activ_id = v->id;
					game_despawn(v->server, NULL, eid);
					break;
				}
			}

			break;
		}

		case CLIENT_ERECTOR_BRING_SPAWN:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, server_is_exe(v->server, v->id));
			AssertOrDisconnect(v->server, v->exe_char == EX_EXETIOR);
			PacketRead(x, packet, packet_read16, uint16_t);
			PacketRead(y, packet, packet_read16, uint16_t);

			if (v->mod_tool)
			{
				RAssert(game_spawn(v->server, (Entity*)&(MakeCreamRing(x, y, false)), sizeof(CreamRing), NULL));
				break;
			}

			BRing* rings[128];
			int cnt = game_find(v->server, (Entity**)rings, "bring", 128);
			
			Vector2 pos = { x, y };
			bool bring_space_available = true;
			for(int i = 0; i < cnt; i++)
			{
				BRing* ring = rings[i];
				if (vector2_dist(&ring->pos, &pos) < 100)
				{
					bring_space_available = false;
					break;
				}
			}
			if (!bring_space_available)
				break;
			
			game_spawn(v->server, (Entity*)(&(MakeBlackRing(x, y))), sizeof(BRing), NULL);
			break;
		}

		case CLIENT_BRING_COLLECTED:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, !server_is_exe(v->server, v->id));
			PacketRead(eid, packet, packet_read16, uint16_t);

			if (game_despawn(v->server, NULL, eid))
			{
				Packet pack;
				PacketCreate(&pack, SERVER_BRING_COLLECTED);
				packet_send(v->peer, &pack, true);
PacketCreate(&pack, SERVER_FORCE_DAMAGE);
PacketWrite(&pack, packet_write16,
game_count_alive_survivors(v->server) >= 8 ? 40 :
(g_config.vanilla_balancing ? 0 : 20));
RAssert(packet_send(v->peer, &pack, true));
			}
			break;
		}

		case CLIENT_ERECTOR_BALLS:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, server_is_exe(v->server, v->id));

			PacketRead(x, packet, packet_readfloat, float);
			PacketRead(y, packet, packet_readfloat, float);

			if(!v->mod_tool) 
			{
				Packet pack;
				PacketCreate(&pack, CLIENT_ERECTOR_BALLS);
				PacketWrite(&pack, packet_writefloat, x);
				PacketWrite(&pack, packet_writefloat, y);
				server_broadcast(v->server, &pack, true);
			}
			else
			{
				for(int i = -3; i < 3; i++)
				{
					RAssert(game_spawn(v->server, (Entity*)&(MakeCreamRing(x + i * 8, y, false)), sizeof(CreamRing), NULL));
				}
			}

			break;
		}

		case CLIENT_EXELLER_SPAWN_CLONE:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, server_is_exe(v->server, v->id));
			AssertOrDisconnect(v->server, v->exe_char == EX_EXELLER);
			AssertOrDisconnect(v->server, game_find(v->server, NULL, "exclone", 3) < 2);

			PacketRead(x, packet, packet_read16, uint16_t);
			PacketRead(y, packet, packet_read16, uint16_t);
			PacketRead(dir, packet, packet_read8, int8_t);
			game_spawn(v->server, (Entity*)&(MakeExellerClone(x, y, dir, v->id)), sizeof(ExellerClone), NULL);
			break;
		}

		case CLIENT_EXELLER_TELEPORT_CLONE:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, server_is_exe(v->server, v->id));
			AssertOrDisconnect(v->server, v->exe_char == EX_EXELLER);
			
			PacketRead(eid, packet, packet_read16, uint16_t);

			ExellerClone* clone;
			if (game_despawn(v->server, (Entity**)&clone, eid))
			{
				if (v->mod_tool)
				{
					Packet pack;
					PacketCreate(&pack, SERVER_EXELLERCLONE_STATE);
					PacketWrite(&pack, packet_write8, 0);
					PacketWrite(&pack, packet_write16, clone->id);
					PacketWrite(&pack, packet_write16, clone->owner);
					PacketWrite(&pack, packet_write16, (uint16_t)v->plr.pos.x);
					PacketWrite(&pack, packet_write16, (uint16_t)v->plr.pos.y);
					PacketWrite(&pack, packet_write8, clone->dir);
					RAssert(packet_send(v->peer, &pack, true));
					free(clone);
					break;
				}

				Packet pack;
				PacketCreate(&pack, SERVER_EXELLERCLONE_STATE);
				PacketWrite(&pack, packet_write8, 1);
				PacketWrite(&pack, packet_write16, clone->id);
				server_broadcast(v->server, &pack, true);

				free(clone);
				v->plr.ex_teleport = 60;
			relay_invalidate_player_data(v);
			}
			break; 
		}

		case CLIENT_PLAYER_ESCAPED:
		{
			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, !server_is_exe(v->server, v->id));

			if (v->mod_tool)
			{
				RAssert(server_disconnect(v->server, v->peer, DR_SERVERTIMEOUT, NULL));
				break;
			}

			if (v->plr.flags & PLAYER_DEAD || v->plr.flags & PLAYER_DEMONIZED)
				break;

			if (v->plr.flags & PLAYER_ESCAPED)
				break;

			SET_FLAG(v->plr.flags, PLAYER_ESCAPED);
			v->plr.dead_fully_tick = v->server->tick;

			Packet pack;
			PacketCreate(&pack, SERVER_PLAYER_ESCAPED);
			RAssert(packet_send(v->peer, &pack, true));

			PacketCreate(&pack, SERVER_GAME_PLAYER_ESCAPED);
			PacketWrite(&pack, packet_write16, v->id);
			server_broadcast(v->server, &pack, true);
			mod_on_escape(v);

			RAssert(game_state_check(v->server));
			break;
		}

		case CLIENT_PLAYER_DEATH_STATE:
		{
			if (v->server->game.end > 0)
				break;

			AssertOrDisconnect(v->server, v->in_game);
			AssertOrDisconnect(v->server, !server_is_exe(v->server, v->id));
			AssertOrDisconnect(v->server, !(v->plr.flags & PLAYER_DEMONIZED));

			PacketRead(dead, packet, packet_read8, uint8_t);
			PacketRead(rtimes, packet, packet_read8, uint8_t);

			Packet pack;
			if (dead &&
				v->server->game.map == 8 &&
				v->server->game.elapsed < 2.0 * TICKSPERSEC)
			{
				PacketCreate(&pack, SERVER_PLAYER_DEATH_STATE);
				PacketWrite(&pack, packet_write16, v->id);
				PacketWrite(&pack, packet_write8, 0);
				PacketWrite(&pack, packet_write8, 0);
				RAssert(packet_send(v->peer, &pack, true));
				break;
			}

			PacketCreate(&pack, SERVER_PLAYER_DEATH_STATE);
			PacketWrite(&pack, packet_write16, v->id);
			PacketWrite(&pack, packet_write8, dead);
			PacketWrite(&pack, packet_write8, rtimes);
			server_broadcast(v->server, &pack, true);

			PacketCreate(&pack, SERVER_REVIVAL_STATUS);
			PacketWrite(&pack, packet_write8, false)
			PacketWrite(&pack, packet_write16, v->id);
			server_broadcast(v->server, &pack, true);

			if (dead)
			{
				if (v->plr.flags & PLAYER_DEAD || v->plr.flags & PLAYER_ESCAPED)
					break;

				SET_FLAG(v->plr.flags, PLAYER_DEAD);
				game_onehitchecklist_mark(v);
				mod_on_death(v);
				if (v->server->game.end > 0)
					break;
if (g_config.gamemodes[0] &&
v->id == v->server->game.cream_guard_id)
return game_end(v->server, ED_EXEWIN, true);
				if (!g_config.vanilla_balancing)
				{
					int first_half_quota =
						v->server->game.match_initial_survivors / 2;
					if (first_half_quota < 1)
						first_half_quota = 1;
					if (!(v->plr.flags & PLAYER_FIRST_HALF_DEAD) &&
						v->server->game.death_order_count < first_half_quota)
					{
						SET_FLAG(v->plr.flags, PLAYER_FIRST_HALF_DEAD);
						v->server->game.death_order_count++;
					}
				}

bool immediate = (v->plr.flags & PLAYER_REVIVED) ||
v->server->game.elapsed < 2.0 * TICKSPERSEC ||
v->server->game.sudden_death ||
g_config.gamemodes[7] ||
(!g_config.vanilla_balancing && game_count_demonized(v->server) == 0);
				if (immediate)
				{
					v->plr.dead_fully_tick = v->server->tick;
					if (!g_config.vanilla_balancing &&
						v->server->game.pending_demonize_count <
							v->server->game.pending_demonize_capacity)
						v->server->game.pending_demonize[
							v->server->game.pending_demonize_count++] = v->id;
					else
						RAssert(game_demonize(v->server, v, false));
				}
				else
				{
					PeerData* exe = NULL;   /* the nearest EXE */
					float best = 1e30f;
					for (int e = 0; e < v->server->game.exe_count; e++)
					{
						PeerData *c = server_find_peer(v->server, v->server->game.exes[e]);
						float dd = c ? vector2_dist(&v->plr.pos, &c->plr.pos) : 1e30f;
						if (c && dd < best) { best = dd; exe = c; }
					}
					v->plr.death_timer_sec = 30;

					PacketCreate(&pack, SERVER_GAME_DEATHTIMER_TICK);
					PacketWrite(&pack, packet_write8, exe && vector2_dist(&v->plr.pos, &exe->plr.pos) <= 240);
					PacketWrite(&pack, packet_write16, v->id);
					PacketWrite(&pack, packet_write8, v->plr.death_timer_sec);
					server_broadcast(v->server, &pack, true);
				}
			}
			else
			{
				AssertOrDisconnect(v->server, v->plr.death_timer_sec > 0);
				DEL_FLAG(v->plr.flags, PLAYER_DEAD);
				relay_invalidate_player_data(v);
			}

			RAssert(game_state_check(v->server));
			break;
		}

		case CLIENT_REVIVAL_PROGRESS:
		{
			if (v->server->game.end > 0)
				break;

			PacketRead(pid, packet, packet_read16, uint16_t);
			PacketRead(rings, packet, packet_read8, uint8_t);

			PeerData* to_revive = server_find_peer(v->server, pid);
			AssertOrDisconnect(v->server, to_revive);

			if (!to_revive || !(to_revive->plr.flags & PLAYER_DEAD))
				break;

			Packet pack;
			if (to_revive->plr.flags & PLAYER_CANTREVIVE)
			{
				to_revive->plr.death_timer_sec = 0;
				to_revive->plr.death_timer = 0;

				PacketCreate(&pack, SERVER_REVIVAL_STATUS);
				PacketWrite(&pack, packet_write8, 0);
				PacketWrite(&pack, packet_write16, to_revive->id);
				server_broadcast(v->server, &pack, true);
				break;
			}

			if (to_revive->plr.revival <= 0)
			{
				PacketCreate(&pack, SERVER_REVIVAL_STATUS);
				PacketWrite(&pack, packet_write8, 1);
				PacketWrite(&pack, packet_write16, to_revive->id);
				server_broadcast(v->server, &pack, true);
			}

			to_revive->plr.revival += 0.015 + (0.004 * rings);
			if (to_revive->plr.revival < 1)
			{
				PacketCreate(&pack, SERVER_REVIVAL_PROGRESS);
				PacketWrite(&pack, packet_write16, to_revive->id);
				PacketWrite(&pack, packet_writedouble, to_revive->plr.revival);
				server_broadcast(v->server, &pack, false);

				
				{
					bool has = false;
					int ind = 0;

					for (int i = 0; i < 5; i++)
					{
						if (to_revive->plr.revival_init[i] == -1)
						{
							ind = i;
							break;
						}

						if (to_revive->plr.revival_init[i] == v->id)
							has = true;
					}

					if (!has)
						to_revive->plr.revival_init[ind] = v->id;
				}
			}
			else
			{
				to_revive->plr.stats.rings = 0;
				to_revive->plr.kill_reported = false;

				SET_FLAG(to_revive->plr.flags, PLAYER_REVIVED);
				DEL_FLAG(to_revive->plr.flags, PLAYER_DEAD);

				PacketCreate(&pack, SERVER_REVIVAL_STATUS);
				PacketWrite(&pack, packet_write8, 0);
				PacketWrite(&pack, packet_write16, to_revive->id);
				server_broadcast(v->server, &pack, true);

				PacketCreate(&pack, SERVER_REVIVAL_REVIVED);
				packet_send(to_revive->peer, &pack, true);

				for (int i = 0; i < 5; i++)
				{
					if (to_revive->plr.revival_init[i] == -1)
						break;

					PeerData* data = server_find_peer(v->server, to_revive->plr.revival_init[i]);
					if(!data)
						continue;
					
					PacketCreate(&pack, SERVER_REVIVAL_RINGSUB);
					packet_send(data->peer, &pack, true);

					Debug("Removed rings from %d", to_revive->plr.revival_init[i]);
				}

				
				Info("%s " LOG_RST "(id %d)" LOG_GRN " was revived!", to_revive->nickname.value, to_revive->id);
			}
			break;
		}

		case CLIENT_CHAT_MESSAGE:
		{
			v->timeout = 0;

			PacketRead(pid, packet, packet_read16, uint16_t);
			PacketRead(msg, packet, packet_readstr, String);
			AssertOrDisconnect(v->server, string_length(&msg) <= 40);

			Info("%s " LOG_RST "(id %d): %s", v->nickname.value, v->id, msg.value);
			if (!server_cmd_handle(v->server, server_cmd_parse(&msg), v, &msg))
			{
				packet->buff[0] = 0;
				server_broadcast_ex(v->server, packet, true, v->id);
			}

			break;
		}
		
		case CLIENT_PING:
		{
			if (!v->server->game.started)
				break;

			Packet pack;
			PacketCreate(&pack, SERVER_PONG);
			PacketWrite(&pack, packet_write16, v->peer->roundTripTime);
			packet_send(v->peer, &pack, false);
			v->plr.ping_last = v->peer->roundTripTime;
			break;
		}

		case CLIENT_PLAYER_DATA:
		{
			if (!v->server->game.started)
				break;

			if ((v->plr.flags & (PLAYER_DEAD | PLAYER_ESCAPED)) && !(v->plr.flags & PLAYER_DEMONIZED))
			{
				bool should_silence;
				if (!(v->plr.flags & PLAYER_DEAD))
					should_silence = v->plr.dead_fully_tick > 0 &&
						(v->server->tick - v->plr.dead_fully_tick) >= 2 * TICKSPERSEC;
				else if (v->plr.death_timer_sec > 0)
					should_silence = false;
				else
					should_silence = v->plr.dead_fully_tick > 0 &&
						(v->server->tick - v->plr.dead_fully_tick) >= 2 * TICKSPERSEC;
				if (should_silence)
					break;
			}

			PacketRead(x, packet, packet_read16, uint16_t);
			PacketRead(y, packet, packet_read16, uint16_t);
			PacketRead(_xspd, packet, packet_read16, uint16_t);
			PacketRead(_yspd, packet, packet_read16, uint16_t);
			
			PacketRead(state, packet, packet_read8, uint8_t);
			PacketRead(_angle, packet, packet_read16, int16_t);
			PacketRead(_index, packet, packet_read8, uint8_t);
			PacketRead(_xscale, packet, packet_read8, int8_t);

			Vector2 new_pos = { x, y };
			#define PLAYER_ATTACKING 1 << 4
			
			int duration = 2000;
			if(!server_is_exe(v->server, v->id))
			{
				PacketRead(hp, packet, packet_read8, int8_t);
				PacketRead(revival, packet, packet_read8, uint8_t);
				PacketRead(rings, packet, packet_read16, int16_t);
				PacketRead(flags, packet, packet_read8, uint8_t);
				if (g_mod.active && v->plr.net_hp_seen && v->plr.net_hp != hp)
					mod_on_hp(v, v->plr.net_hp, hp);
				v->plr.net_hp = hp;
				v->plr.net_hp_seen = true;
				v->plr.net_valid = true;

				if(!(v->plr.flags & PLAYER_DEAD) && !(v->plr.flags & PLAYER_DEMONIZED))
				{
					if (!server_is_exe(v->server, v->id))
					{
						v->plr.rings = rings;
						if (revival < 2)
						{
							if (rings < 0 || (v->server->game.map != MAP_FART_ZONE && rings >= (g_mod.active ? 10000 : 120)))
							{
								server_disconnect(v->server, v->peer, DR_OTHER, "gomunkulus");
								return true;
							}

							if (hp > 100 && !g_mod.active)  /* (a mod's characters may have more) */
							{
								server_disconnect(v->server, v->peer, DR_OTHER, "garic forn — Сьогодні о 04:31");
								return true;
							}
						}
					}
				}

				v->plr.is_attacking = flags & PLAYER_ATTACKING;
				switch(v->surv_char)
				{
					case CH_EGGMAN:
					{
						duration = 3000;
						break;
					}
				}
			}
			else
			{
				PacketRead(flags, packet, packet_read8, uint8_t);
				v->plr.is_attacking = flags & PLAYER_ATTACKING;
			}

if (v->plr.is_attacking)
relay_mark_priority_near(v->server, &new_pos);

			if(v->server->game.end <= 0 && v->plr.is_attacking)
			{
				if(v->plr.attack_timer <= 0)
				{
					time_start(&v->plr.last_attack);
					v->plr.attack_timer = 1;
				}
				else
				{
					double elapsed = time_end(&v->plr.last_attack);
					v->plr.attack_timer += elapsed;
					if(v->plr.attack_timer >= duration)
					{
						server_disconnect(v->server, v->peer, DR_KICKEDBYHOST, "nuh-uh!");
						return true;
					}
					time_start(&v->plr.last_attack);
				}
			}
			else
				v->plr.attack_timer = 0;
			
			
			if(g_config.ping_limit == UINT16_MAX)
			{
				
				float dist = vector2_dist(&v->plr.pos, &new_pos);
				if (v->server->game.started && !v->plr.mod_tool && v->plr.pos.x != 0 && v->plr.pos.y != 0)
				{
					if (v->plr.ex_teleport == 0)
					{
						if (dist > 700)
						{
							switch (v->server->game.map)
							{
								case 15:
								case 13:
								case 8:
								case 6:
									break;
				
								default:
									server_disconnect(v->server, v->peer, DR_OTHER, "blud used a portal gun lmfao");
									return true;
							}
						}
				
						if (dist > 60)
						{
							if (!player_add_error(v->server, v, 500))
								return true;
						}
					}
					else
						v->plr.ex_teleport--;
				}
			}
			
			double now = (double)v->server->tick;

			if (v->plr.prev_recv_valid && now > (double)v->plr.prev_recv_tick)
			{
				double previous = (double)v->plr.prev_recv_tick;
				double midpoint = (previous + now) * 0.5;
				Vector2 midpoint_pos =
				{
					(v->plr.prev_recv_pos.x + new_pos.x) * 0.5f,
					(v->plr.prev_recv_pos.y + new_pos.y) * 0.5f
				};

				player_history_append(
					&v->plr,
					midpoint,
					midpoint_pos,
					state,
					v->plr.is_attacking);
			}

			player_history_append(
				&v->plr,
				now,
				new_pos,
				state,
				v->plr.is_attacking);

			if (v->plr.prev_recv_valid)
			{
				double sample_ticks =
					(double)v->server->tick - (double)v->plr.prev_recv_tick;

				if (sample_ticks > 0.0)
				{
					v->plr.recv_vx =
						(new_pos.x - v->plr.prev_recv_pos.x) / (float)sample_ticks;
					v->plr.recv_vy =
						(new_pos.y - v->plr.prev_recv_pos.y) / (float)sample_ticks;
				}
			}
			else
			{
				v->plr.recv_vx = game_half_to_float(_xspd);
				v->plr.recv_vy = game_half_to_float(_yspd);
			}

			v->plr.prev_recv_pos = new_pos;
			v->plr.prev_recv_tick = v->server->tick;
			v->plr.prev_recv_valid = true;
			v->plr.last_recv_elapsed = (double)v->server->tick;
			v->plr.pos = new_pos;
			v->plr.timeout = 0;
			v->plr.state = state;
			relay_cache_player_data(v, packet);

			if (v->plr.mod_tool)
			{
				if (v->plr.mod_tool_timer < 61)
					v->plr.mod_tool_timer++;
				else
				{
					float dist = vector2_dist(&v->plr.pos, &v->plr.start_pos);
					if (dist > 240)
					{
						Packet pack;
						PacketCreate(&pack, SERVER_PLAYER_BACKTRACK);
						PacketWrite(&pack, packet_writefloat, v->plr.start_pos.x);
						PacketWrite(&pack, packet_writefloat, v->plr.start_pos.y);
						packet_send(v->peer, &pack, true);
					}
					else if (dist >= 300)
					{
						
						server_disconnect(v->server, v->peer, DR_OTHER,
							"@@@@@@@......@@@\n"
							"@@@@@@@......@@@\n"
							"@@@@@@@.......@@\n"
							"@@@@@@@@@@@@....\n"
							"@@@@@@@@@@......\n"
							"....@@..........\n"
							"................\n"
							"@@@@@@@@@@@@....\n"
							"@@@@@@@@@@@@....\n"
							"@@@@@@@@@@@@@...\n"
							"@@@@@@@@@@@@@...\n"
							"@@@@@@@@@@@....."
						);
					}
					break;
				}

				if (v->plr.mod_tool_timer == 60)
				{
					v->plr.start_pos = v->plr.pos;

					Packet pack;
					PacketCreate(&pack, SERVER_FELLA);
					PacketWrite(&pack, packet_write16, v->id);
					PacketWrite(&pack, packet_writefloat, v->plr.start_pos.x);
					PacketWrite(&pack, packet_writefloat, v->plr.start_pos.y);
					server_broadcast(v->server, &pack, true);
					break;
				}
			}

			break;
		}
	}

	if (!v->server->game.started)
	{
		if(!v->plr.ready)
		{
			v->plr.ready = true;
		}

		RAssert(game_checkstart(v->server));
		return true;
	}

	RAssert(packet_seek(packet, 0));
	RAssert(g_mapList[v->server->game.map].cb.tcp_msg(v, packet));
	return true;
}

bool game_entity_tick(Server* server)
{
	uint16_t entits[GAME_MAX_DESPAWNS_PER_TICK];
	int entit = 0;

	for (size_t i = 0; i < server->game.entities.capacity; i++)
	{
		Entity* ent = (Entity*)server->game.entities.ptr[i];
		if (!ent)
			continue;

		if (ent->tick && !ent->tick(server, ent) &&
			entit < GAME_MAX_DESPAWNS_PER_TICK)
			entits[entit++] = ent->id;
	}

	for (int i = 0; i < entit; i++)
	{
		game_despawn(server, NULL, entits[i]);
	}

	return true;
}

static void game_clear_rings(Server* server)
{
	for (size_t i = 0; i < server->game.entities.capacity; i++)
	{
		Entity* entity = (Entity*)server->game.entities.ptr[i];
		if (!entity || strcmp(entity->tag, "ring") != 0)
			continue;

		Ring* ring = (Ring*)entity;
		if (ring->red && server->game.active_red_ring_count > 0)
			server->game.active_red_ring_count--;
		game_despawn(server, NULL, ring->id);
	}
}

bool game_player_tick(Server* server)
{
	if (server->game.pending_demonize_count > 0)
	{
		int len = (int)server->game.pending_demonize_count;
		for (int i = len - 1; i > 0; i--)
		{
			int j = rand() % (i + 1);
			uint16_t id = server->game.pending_demonize[i];
			server->game.pending_demonize[i] = server->game.pending_demonize[j];
			server->game.pending_demonize[j] = id;
		}

		for (int i = 0; i < len; i++)
		{
			PeerData* data = server_find_peer(server,
				server->game.pending_demonize[i]);
			if (!data || !data->in_game)
				continue;
			RAssert(game_demonize(server, data,
				game_first_half_guarantee_ready(server, data)));
			RAssert(game_state_check(server));
		}
		server->game.pending_demonize_count = 0;
	}

	
	/* the (first) EXE; a mod's round may have several (each player: the nearest) or none */
	PeerData* exe = server->game.exe >= 0 ? server_find_peer(server, (uint16_t)server->game.exe) : NULL;

	bool exe_camp = false;
	int survivors = 0, demonized = 0;

	for(size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* data = (PeerData*)server->peers.ptr[i];
		if (!data)
			continue;

		if(!data->in_game)
			continue;

		player_check_zone(server, data);

		
		if (server->delta < 2.5)
		{
			data->plr.ping_timer += server->delta;
			data->plr.ping_total += data->plr.ping_last * server->delta;

			if (data->plr.ping_timer >= 20 * TICKSPERSEC)
			{
				double avg_ping = data->plr.ping_total / data->plr.ping_timer;
				if (avg_ping >= g_config.ping_limit)
				{
					char msg[130];
					snprintf(msg, 130, "Bad connection, try picking closest region for better experience!\nYour average ping for last 20s: %dms", (int)avg_ping);
					server_disconnect(server, data->peer, DR_OTHER, msg);
					continue;
				}

				data->plr.ping_total = 0;
				data->plr.ping_timer = 0;
			}
		}

		PeerData *near_exe = NULL;
		float near_d = 1e30f;
		for (int e = 0; e < server->game.exe_count; e++)
		{
			PeerData *c = server_find_peer(server, server->game.exes[e]);
			float dd = c && c != data ? vector2_dist(&c->plr.pos, &data->plr.pos) : 1e30f;
			if (dd < near_d) { near_d = dd; near_exe = c; }
		}
		if((data->plr.flags & PLAYER_DEAD) && !(data->plr.flags & PLAYER_CANTREVIVE) && near_exe && near_d < 300)
			exe_camp = true;

		if (!server_is_exe(server, data->id) && !(data->plr.flags & PLAYER_DEAD) && !(data->plr.flags & PLAYER_DEMONIZED))
		{
			
			if (!(data->plr.flags & PLAYER_ESCAPED))
			{
				bool in_danger = near_exe && near_d < 300;
				if (in_danger)
					data->plr.stats.danger_time += server->delta;
				
				if(server->game.map != 8 && server->game.map != 6)
				{
					
					uint32_t chunk = ((uint32_t)data->plr.pos.x / 480) + ((uint32_t)data->plr.pos.y / 270);
					if (data->plr.chunk != chunk)
					{
						data->plr.stats.braindead_time = 0;
						data->plr.chunk = chunk;
					}
					else
					{
						data->plr.stats.braindead_time += server->delta * (in_danger ? 0.5 : 1);
						if (data->plr.stats.braindead_time >= 25 * TICKSPERSEC)
							data->plr.stats.brain_damage = true;
					}
				}
			}

			data->plr.stats.survive_time += server->delta;
		}
		
		
		if(data->plr.flags & PLAYER_DEAD && !(data->plr.flags & PLAYER_CANTREVIVE))
		{
			if(data->plr.revival > 0)
			{
				data->plr.revival -= 0.0025 * server->delta;
				
				Packet pack;
				if(data->plr.revival <= 0)
				{
					for(int i = 0; i < 5; i++)
						data->plr.revival_init[i] = -1;

					PacketCreate(&pack, SERVER_REVIVAL_STATUS);
					PacketWrite(&pack, packet_write8, 0);
					PacketWrite(&pack, packet_write16, data->id);
					server_broadcast(server, &pack, true);
				}
				else
				{
					PacketCreate(&pack, SERVER_REVIVAL_PROGRESS);
					PacketWrite(&pack, packet_write16, data->id);
					PacketWrite(&pack, packet_writedouble, data->plr.revival);
					server_broadcast(server, &pack, false);
				}
			}
		}

if (!(data->plr.flags & PLAYER_FROZEN_DEMON) &&
    !(data->plr.flags & (PLAYER_DEAD | PLAYER_ESCAPED)))
		{
			data->plr.timeout += server->delta;
			if (data->plr.timeout >= 15.f * TICKSPERSEC)
				server_disconnect(server, data->peer, DR_AFKTIMEOUT, NULL);
		}

 

if (!(data->plr.flags & PLAYER_ESCAPED))
			continue;

		if (server_is_exe(server, data->id))
			continue;

		if (data->plr.flags & PLAYER_DEMONIZED)
			demonized++;
		else if (!(data->plr.flags & PLAYER_DEAD))
			survivors++;
	}

	if(server->game.map != 8 && exe_camp && exe)
		exe->plr.stats.camp_time += server->delta;

	if (g_config.vanilla_balancing && !server->game.sudden_death &&
		server->game.time_sec <= TICKSPERSEC * 2)
	{
		server->game.sudden_death = true;
		PeerData* sort[MAX_SUPPORTED_PLAYERS];
		int len = 0;
		memset(sort, 0, sizeof(sort));

		for (size_t i = 0; i < server->peers.capacity; i++)
		{
			PeerData* data = (PeerData*)server->peers.ptr[i];
			if (!data || !data->in_game ||
				!(data->plr.flags & PLAYER_DEAD))
				continue;
			sort[len++] = data;
		}

		for (int i = 0; i < len; i++)
		{
			for (int j = i + 1; j < len; j++)
			{
				double total_a = sort[i]->plr.death_timer_sec +
					sort[i]->plr.death_timer / 60.0;
				double total_b = sort[j]->plr.death_timer_sec +
					sort[j]->plr.death_timer / 60.0;
				if (total_a > total_b)
				{
					PeerData* data = sort[i];
					sort[i] = sort[j];
					sort[j] = data;
				}
			}
		}

		for (int i = 0; i < len; i++)
			game_demonize(server, sort[i], false);
	}

	if (!g_config.vanilla_balancing && server->game.started &&
		!server->game.last_minute_sweep_done &&
		server->game.time_sec <= 120)
	{
		int need;
		int quota = server->game.match_initial_survivors / 2;
		if (quota < 1)
			quota = 1;
		need = quota - game_count_demonized(server);

		for (size_t i = 0; i < server->peers.capacity && need > 0; i++)
		{
			PeerData* data = (PeerData*)server->peers.ptr[i];
			if (!data || !data->in_game)
				continue;
			if (server_is_exe(server, data->id))
				continue;
			if (!(data->plr.flags & PLAYER_FIRST_HALF_DEAD) ||
				!(data->plr.flags & PLAYER_DEAD) ||
				(data->plr.flags & PLAYER_DEMONIZED))
				continue;
			RAssert(game_demonize(server, data, true));
			RAssert(game_state_check(server));
			need--;
		}

		server->game.last_minute_sweep_done = true;
		server->game.sudden_death = true;
	}

	if (!g_config.vanilla_balancing && server->game.started &&
		!server->game.sixty_second_sweep_done &&
		server->game.time_sec <= 60)
	{
		server->game.sixty_second_sweep_done = true;
		for (size_t i = 0; i < server->peers.capacity; i++)
		{
			PeerData* data = (PeerData*)server->peers.ptr[i];
			if (!data || !data->in_game ||
				server_is_exe(server, data->id) ||
				!(data->plr.flags & PLAYER_DEMONIZED))
				continue;
			SET_FLAG(data->plr.flags, PLAYER_FROZEN_DEMON);
		}
		RAssert(game_state_check(server));
	}

if (!g_config.vanilla_balancing && server->game.started &&
!server->game.duel_triggered &&
server->game.elapsed >= 5.0 * TICKSPERSEC)
	{
		int alive_pure = 0;
		for (size_t i = 0; i < server->peers.capacity; i++)
		{
			PeerData* data = (PeerData*)server->peers.ptr[i];
			if (!data || !data->in_game ||
				server_is_exe(server, data->id) ||
(data->plr.flags & (PLAYER_DEMONIZED |
					PLAYER_DEAD | PLAYER_CANTREVIVE)))
				continue;
			alive_pure++;
		}

if (alive_pure == 1 && game_count_alive_survivors(server) == 1)
		{
			server->game.duel_triggered = true;
server->game.duel_boost_until =
				server->game.elapsed + 15.0 * TICKSPERSEC;
			game_clear_rings(server);
if (server->game.time_sec > 120)
{
server->game.time_sec = 120;
Packet packet;
PacketCreate(&packet, SERVER_GAME_TIME_SYNC);
PacketWrite(&packet, packet_write16, (uint16_t)120 * TICKSPERSEC);
server_broadcast(server, &packet, true);
}
			server->game.ring_spawn_disabled_until =
				server->game.elapsed + 3.0 * TICKSPERSEC;
			server->game.duel_ring_clear_at = 0.0;
		}
	}

	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* data = (PeerData*)server->peers.ptr[i];
		if (!data)
			continue;

		if(!data->in_game)
			continue;

		if (data->plr.flags & PLAYER_CANTREVIVE)
			continue;

		if (data->plr.flags & PLAYER_DEAD && data->plr.death_timer_sec > 0)
		{
			Packet packet;
			bool exe_near = false;
			bool demonized_near = false;

			
			for (size_t j = 0; j < server->peers.capacity; j++)
			{
				PeerData* check = (PeerData*)server->peers.ptr[j];
				if (!check)
					continue;

				if(!check->in_game)
					continue;

				if (!server_is_exe(server, check->id) && !(check->plr.flags & PLAYER_DEMONIZED))
					continue;

				if (vector2_dist(&data->plr.pos, &check->plr.pos) <= 240)
				{
					if (check->plr.flags & PLAYER_DEMONIZED)
						demonized_near = true;
					else
					{
						demonized_near = false;
						exe_near = true;
						break;
					}
				}
			}

			if (server->game.time_sec < TICKSPERSEC * 2)
			{
				game_demonize(server, data, false);
				continue;
			}

			if (data->plr.death_timer >= TICKSPERSEC)
			{
				if (!exe_near)
				{
					if (--data->plr.death_timer_sec <= 0)
					{
						game_demonize(server, data, false);
						continue;
					}
				}

				PacketCreate(&packet, SERVER_GAME_DEATHTIMER_TICK);
				PacketWrite(&packet, packet_write8, exe_near);
				PacketWrite(&packet, packet_write16, data->id);
				PacketWrite(&packet, packet_write8, data->plr.death_timer_sec);
				server_broadcast(server, &packet, true);

				data->plr.death_timer = 0;
			}

			data->plr.death_timer += (demonized_near ? 0.5f : 1.0f) * server->delta;
		}
	}

	return true;
}

bool game_state_tick(Server* server)
{
    server_relay_tick(server);
	
	if (!server->game.started)
	{
		server->game.start_timeout -= server->delta;
		if (server->game.start_timeout <= 0)
		{
			Warn("Waiting for players took too long, kicking out inactive players!");

			for (size_t j = 0; j < server->peers.capacity; j++)
			{
				PeerData* data = (PeerData*)server->peers.ptr[j];
				if (!data)
					continue;

				if(!data->in_game)
					continue;

				if (!data->plr.ready)
				{
					server_disconnect(server, data->peer, DR_PACKETSNOTRECV, NULL);
					break;
				}
			}

		}
		return true;
	}

	if (server->game.end > 0)
	{
		server->game.end -= server->delta;
		if (server->game.end <= 0)
			RAssert(game_uninit(server, true));

		return true;
	}

	server->game.elapsed += server->delta;
if (server->game.time <= TICKSPERSEC)
	{
if (!g_config.gamemodes[3])
{
		server->game.time_sec--;
}
server->game.time += TICKSPERSEC;

if (!g_config.gamemodes[8] &&
(server->game.ring_spawn_disabled_until <= 0.0 ||
			server->game.elapsed >= server->game.ring_spawn_disabled_until)
)
		{
			if (g_config.vanilla_balancing)
			{
				if (server->game.time_sec % server->game.ring_coff == 0 &&
					!game_spawn(server, (Entity*)&(MakeRing()),
						sizeof(Ring), NULL))
					Debug("Not enough space for rings");
			}
			else
			{
				double mult = 1.0;
				int survivors = game_count_alive_survivors(server);
				if (survivors <= 7)
					mult = 1.0;
				else if (survivors >= 100)
					mult = 2.0;
				else if (survivors <= 20)
				{
					double t = (double)(survivors - 7) / 13.0;
					mult = 1.0 + 0.30 * pow(t, 1.5);
				}
				else
				{
					double t = log((double)survivors / 20.0) / log(5.0);
					mult = 1.30 + 0.70 * t;
				}
if (server->game.duel_triggered &&
server->game.elapsed < server->game.duel_boost_until &&
					game_count_demonized(server) > 0)
			mult *= 12.0;

				server->game.ring_accum +=
					mult / (double)server->game.base_ring_coff;
				while (server->game.ring_accum >= 1.0)
				{
					server->game.ring_accum -= 1.0;
					if (!game_spawn(server, (Entity*)&(MakeRing()),
						sizeof(Ring), NULL))
						Debug("Not enough space for rings");
				}
			}
		}

		Packet packet;
		PacketCreate(&packet, SERVER_GAME_TIME_SYNC);
		PacketWrite(&packet, packet_write16, (uint16_t)server->game.time_sec * TICKSPERSEC);
		server_broadcast(server, &packet, true);

if (server->game.time_sec <= 0 && server->game.ffa)
{   /* a free-for-all's time: the EXE who stunned the others most wins */
	PeerData *best = NULL;
	for (int i = 0; i < server->game.exe_count; i++)
	{
		PeerData *c = server_find_peer(server, server->game.exes[i]);
		if (c && (!best || c->plr.stats.stuns > best->plr.stats.stuns))
			best = c;
	}
	if (best)
	{
		char msg[160];
		snprintf(msg, sizeof msg, "%s wins the free-for-all: %d hit(s)", best->nickname.value, (int)best->plr.stats.stuns);
		server_broadcast_msg(server, msg);
	}
	RAssert(game_end(server, ED_EXEWIN, true));
}
else if (server->game.time_sec <= 0)
RAssert(game_end(server, g_config.gamemodes[6] ?
ED_SURVWIN : ED_TIMEOVER, true));
	}

	RAssert(game_player_tick(server));
	RAssert(game_entity_tick(server));
	RAssert(g_mapList[server->game.map].cb.tick(server));

	server->game.time -= server->delta;
	return true;
}
