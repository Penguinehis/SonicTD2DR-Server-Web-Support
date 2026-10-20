#include <Mod.h>
#include <Log.h>
#include <States.h>
#include <CMath.h>
#include <Colors.h>

bool charselect_check_state(Server *server)
{
	RAssert(server);

	if (server_ingame(server) <= 1)
	{
		Warn("charselect_check_state: only %d in-game player(s), returning to lobby", server_ingame(server));
		return lobby_init(server);
	}

	bool should = true;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *peer = (PeerData *)server->peers.ptr[i];
		if (!peer)
			continue;

		if (!peer->in_game)
			continue;

		if (peer->exe_char == EX_NONE && peer->surv_char == CH_NONE)
			should = false;
	}

	if (should)
		return game_init(server->lobby.exe, server->lobby.map, server);

	return true;
}

bool server_is_exe(Server *server, uint16_t id)
{
	const uint16_t *ids = server->state == ST_CHARSELECT ? server->lobby.exes : server->game.exes;
	int n = server->state == ST_CHARSELECT ? server->lobby.exe_count : server->game.exe_count;
	for (int i = 0; i < n; i++)
		if (ids[i] == id)
			return true;
	return false;
}

/* A mod's round with several EXEs (Mod "exes": 2-3), none (0), or everyone (free_for_all): the first
   one is chosen as the game does (lobby.exe), the others at random among the rest (at least one
   survivor stays, but in a free-for-all) */
static void charselect_mod_exes(Server *server)
{
	Lobby *l = &server->lobby;
	l->exe_count = 0;
	l->ffa = false;
	if (l->exe != UINT16_MAX)
		l->exes[l->exe_count++] = l->exe;
	if (!g_mod.active || (g_mod.exes == 1 && !g_mod.ffa))
		return;
	int ingame = server_ingame(server);
	int want = g_mod.ffa ? SERVER_MAX_EXES : g_mod.exes;
	if (!g_mod.ffa && want > ingame - 1)
		want = ingame - 1;
	if (want < 0)
		want = 0;
	if (want == 0)
	{
		l->exe = UINT16_MAX;
		l->exe_count = 0;
		return;
	}
	l->ffa = g_mod.ffa;
	PeerData *pool[256];
	int np = 0;
	for (size_t i = 0; i < server->peers.capacity && np < 256; i++)
	{
		PeerData *p = (PeerData *)server->peers.ptr[i];
		if (p && p->in_game && p->id != l->exe && !p->mod_tool)
			pool[np++] = p;
	}
	while (l->exe_count < want && np > 0)
	{
		int k = rand() % np;
		l->exes[l->exe_count++] = pool[k]->id;
		pool[k] = pool[--np];
	}
	Info("mod round: %d EXE(s)%s", l->exe_count, l->ffa ? " (free-for-all)" : "");
}

bool charselect_choose_exe(Server *server, uint16_t *id)
{
	RAssert(server);

	uint32_t weight = 0;

	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *peer = (PeerData *)server->peers.ptr[i];
		if (!peer)
			continue;

		if (!peer->in_game)
			continue;

		
		peer->exe_char = EX_NONE;
		peer->surv_char = CH_NONE;

		weight += peer->exe_chance;
	}

	if (weight == 0)
		weight++;

	uint32_t rnd = rand() % weight;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *peer = (PeerData *)server->peers.ptr[i];
		if (!peer)
			continue;

		if (!peer->in_game)
			continue;

		if (peer->exe_chance >= 100)
		{
			*id = peer->id;
			return true;
		}

		if (rnd < peer->exe_chance && !peer->mod_tool)
		{
			Info("%s (id %d, c %d) is exe!", peer->nickname.value, peer->id, peer->exe_chance);

			peer->exe_chance = g_config.vanilla_balancing ? 1 : (1 + rand() % 3);
			*id = peer->id;

			return true;
		}

		rnd -= peer->exe_chance;
	}

	*id = -1;
	return false;
}

bool charselect_state_handle(PeerData *v, Packet *packet)
{
	
	PacketRead(passtrough, packet, packet_read8, uint8_t);
	PacketRead(type, packet, packet_read8, uint8_t);

	bool res = true;
	Packet pack;
	switch (type)
	{
	case CLIENT_REQUEST_EXECHARACTER:
	{
		if (!v->in_game)
			break;

		
		AssertOrDisconnect(v->server, server_is_exe(v->server, v->id));
		PacketRead(id, packet, packet_read8, uint8_t);

		/* Wire values are 1-based. Range-check before the decrement: `id` is
		   unsigned, so `id >= 0` after a wrap would not catch anything. */
		AssertOrDisconnect(v->server, id >= 1);
		AssertOrDisconnect(v->server, id <= EX_EXELLER + 1);

		id--;
		v->exe_char = id;

		PacketCreate(&pack, SERVER_LOBBY_EXECHARACTER_RESPONSE);
		PacketWrite(&pack, packet_write8, id);
		RAssert(packet_send(v->peer, &pack, true));

		PacketCreate(&pack, SERVER_LOBBY_CHARACTER_CHANGE);
		PacketWrite(&pack, packet_write16, v->id);
		PacketWrite(&pack, packet_write8, id);
		server_broadcast(v->server, &pack, true);

		const char *exes[] = {
			"Classic Exe",
			"Chaos",
			"Exetior",
			"Exeller"};

		Info("%s " LOG_RST "(id %d) choses [" LOG_RED "%s" LOG_RST "]!", v->nickname.value, v->id, exes[id]);
		return charselect_check_state(v->server) || lobby_init(v->server);
	}

	case CLIENT_REQUEST_CHARACTER:
	{
		if (!v->in_game)
			break;

		if (v->surv_char != CH_NONE)
			break;

		
		AssertOrDisconnect(v->server, !server_is_exe(v->server, v->id));
		PacketRead(id, packet, packet_read8, uint8_t);

		/* Wire values are 1-based. Range-check before the decrement: `id` is
		   unsigned, so `id >= 0` after a wrap would not catch anything. */
		AssertOrDisconnect(v->server, id >= 1);
		AssertOrDisconnect(v->server, id <= CH_SALLY + 1);

		id--;

		uint8_t avail = v->server->lobby.avail[id];

if (g_config.vanilla_balancing ||
(g_config.gamemodes[0] && id == CH_CREAM))
		{
			
			if (avail) v->server->lobby.avail[id] = 0;
			PacketCreate(&pack, SERVER_LOBBY_CHARACTER_RESPONSE);
			PacketWrite(&pack, packet_write8, id + 1);
			PacketWrite(&pack, packet_write8, avail);
			RAssert(packet_send(v->peer, &pack, true));
			if (avail)
			{
				v->surv_char = id;
				PacketCreate(&pack, SERVER_LOBBY_CHARACTER_CHANGE);
				PacketWrite(&pack, packet_write16, v->id);
				PacketWrite(&pack, packet_write8, id + 1);
				server_broadcast(v->server, &pack, true);
			}
		}
		else if (avail)
		{
			
			int ingame    = server_ingame(v->server);
			int threshold = (ingame + 3) / 4;
			if (threshold < 1) threshold = 1;

			v->server->lobby.surv_reservations[id]++;
			v->surv_char = id;

			PacketCreate(&pack, SERVER_LOBBY_CHARACTER_RESPONSE);
			PacketWrite(&pack, packet_write8, id + 1);
			PacketWrite(&pack, packet_write8, 1);
			RAssert(packet_send(v->peer, &pack, true));

			
			for (size_t j = 0; j < v->server->peers.capacity; j++)
			{
				PeerData *r = (PeerData *)v->server->peers.ptr[j];
				if (!r || !r->in_game || r->id == v->id)
					continue;
				if (server_is_exe(v->server, r->id) && r->exe_char != EX_NONE)
				{
					PacketCreate(&pack, SERVER_LOBBY_CHARACTER_CHANGE);
					PacketWrite(&pack, packet_write16, r->id);
					PacketWrite(&pack, packet_write8, r->exe_char);
					RAssert(packet_send(v->peer, &pack, true));
				}
				else if (!server_is_exe(v->server, r->id) && r->surv_char != CH_NONE)
				{
					PacketCreate(&pack, SERVER_LOBBY_CHARACTER_CHANGE);
					PacketWrite(&pack, packet_write16, r->id);
					PacketWrite(&pack, packet_write8, r->surv_char + 1);
					RAssert(packet_send(v->peer, &pack, true));
				}
			}

			
			if ((int)v->server->lobby.surv_reservations[id] >= threshold)
			{
				v->server->lobby.avail[id] = 0;
				for (size_t j = 0; j < v->server->peers.capacity; j++)
				{
					PeerData *r = (PeerData *)v->server->peers.ptr[j];
					if (!r || !r->in_game || r->surv_char != id)
						continue;
					PacketCreate(&pack, SERVER_LOBBY_CHARACTER_CHANGE);
					PacketWrite(&pack, packet_write16, r->id);
					PacketWrite(&pack, packet_write8, id + 1);
					server_broadcast(v->server, &pack, true);
				}
			}
		}
		else
		{
			
			PacketCreate(&pack, SERVER_LOBBY_CHARACTER_RESPONSE);
			PacketWrite(&pack, packet_write8, id + 1);
			PacketWrite(&pack, packet_write8, 0);
			RAssert(packet_send(v->peer, &pack, true));
		}

		const char *survs[] = {
			"Tails",
			"Knuckles",
			"Eggman",
			"Amy",
			"Cream",
			"Sally"};

		Info("%s " LOG_RST "(id %d) choses [" LOG_GRN "%s" LOG_RST "]!", v->nickname.value, v->id, survs[id]);
		return charselect_check_state(v->server) || lobby_init(v->server);
	}

	case CLIENT_CHAT_MESSAGE:
	{
		PacketRead(pid, packet, packet_read16, uint16_t);
		PacketRead(msg, packet, packet_readstr, String);
		AssertOrDisconnect(v->server, string_length(&msg) <= 40);

		v->timeout = 0;

		Info("%s " LOG_RST "(id %d): %s", v->nickname.value, v->id, msg.value);
		if (!server_cmd_handle(v->server, server_cmd_parse(&msg), v, &msg))
		{
			packet->buff[0] = 0;
			server_broadcast_ex(v->server, packet, true, v->id);
		}

		break;
	}

	default:
		RAssert(server_msg_handle(v->server, type, v, packet));
		break;
	}

	return res;
}

bool charselect_state_tick(Server *server)
{
	if (server->lobby.countdown <= 0)
	{
		server->lobby.countdown += TICKSPERSEC;

		if (--server->lobby.countdown_sec == 0)
		{
			for (size_t i = 0; i < server->peers.capacity; i++)
			{
				PeerData *peer = (PeerData *)server->peers.ptr[i];
				if (!peer)
					continue;

				if (!peer->in_game)
					continue;

				if (peer->exe_char == EX_NONE && peer->surv_char == CH_NONE)
					server_disconnect(server, peer->peer, DR_AFKTIMEOUT, NULL);
			}
		}

		Packet pack;
		PacketCreate(&pack, SERVER_CHAR_TIME_SYNC);
		PacketWrite(&pack, packet_write8, server->lobby.countdown_sec);
		server_broadcast(server, &pack, true);
	}

	server->lobby.countdown -= server->delta;
	return true;
}

bool charselect_init(int8_t map, Server *server)
{
	Debug("Attepting to enter ST_CHARSELECT...");
	RAssert(server);
	RAssert(charselect_choose_exe(server, &server->lobby.exe));
	if (server->lobby.exe != UINT16_MAX)
		server->lobby.exe = mod_pick_exe(server, server->lobby.exe);

	if (server->lobby.exe == UINT16_MAX)
	{
		Err("Failed to pick exe for some reason!");
		return lobby_init(server);
	}
	charselect_mod_exes(server);   /* (a mod's: several, or none) */

	server->state = ST_CHARSELECT;
	server->lobby.countdown_sec = 30;
	server->lobby.countdown = TICKSPERSEC;
	memset(server->lobby.avail, 1, sizeof(server->lobby.avail));
	memset(server->lobby.surv_reservations, 0, sizeof(server->lobby.surv_reservations));

	Packet pack;
	bool several = server->lobby.exe_count != 1 || server->lobby.ffa;
	if (!several)
	{
		PacketCreate(&pack, SERVER_LOBBY_EXE);
		PacketWrite(&pack, packet_write16, server->lobby.exe);
		PacketWrite(&pack, packet_write16, map);
		server_broadcast(server, &pack, true);
	}
	else
	{
		/* each EXE is told it is the EXE (its game picks an EXE then); the others: the first one. Then
		   everyone gets the list (SERVER_LOBBY_EXES) */
		for (size_t i = 0; i < server->peers.capacity; i++)
		{
			PeerData *p = (PeerData *)server->peers.ptr[i];
			if (!p)
				continue;
			PacketCreate(&pack, SERVER_LOBBY_EXE);
			PacketWrite(&pack, packet_write16, server_is_exe(server, p->id) ? p->id : server->lobby.exe);
			PacketWrite(&pack, packet_write16, map);
			RAssert(packet_send(p->peer, &pack, true));
		}
		PacketCreate(&pack, SERVER_LOBBY_EXES);
		PacketWrite(&pack, packet_write8, server->lobby.exe_count);
		for (int i = 0; i < server->lobby.exe_count; i++)
			PacketWrite(&pack, packet_write16, server->lobby.exes[i]);
		PacketWrite(&pack, packet_write8, server->lobby.ffa);
		server_broadcast(server, &pack, true);
	}

	PacketCreate(&pack, SERVER_CHAR_TIME_SYNC);
	PacketWrite(&pack, packet_write8, server->lobby.countdown_sec);
	server_broadcast(server, &pack, true);

	Info(LOG_YLW "Server is now in " LOG_PUR "Character Select");
	server->lobby.map = map;

	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData *data = (PeerData *)server->peers.ptr[i];

		if (!data)
			continue;

		if (data->in_game)
			continue;

		char msg[256];
		snprintf(msg, 100, "map: %s" CLRCODE_RST, g_mapList[map].name);
		server_send_msg(server, data->peer, msg);
	}

	return true;
}

bool charselect_state_join(PeerData *v)
{
	return true;
}

bool charselect_state_left(PeerData *v)
{
	if (v->surv_char != CH_NONE)
	{
if (g_config.vanilla_balancing ||
(g_config.gamemodes[0] && v->surv_char == CH_CREAM))
			v->server->lobby.avail[v->surv_char] = 1;
		else if (v->server->lobby.avail[v->surv_char])
			v->server->lobby.surv_reservations[v->surv_char]--;
	}

	if (server_is_exe(v->server, v->id))
	{
		/* an EXE left: the game's round needs it; a mod's round of several goes on with the others */
		Lobby *l = &v->server->lobby;
		int k = 0;
		for (int i = 0; i < l->exe_count; i++)
			if (l->exes[i] != v->id)
				l->exes[k++] = l->exes[i];
		l->exe_count = (uint8_t)k;
		if (l->exe == v->id)
			l->exe = k ? l->exes[0] : UINT16_MAX;
		if (!k)
			return lobby_init(v->server);
	}
	if (server_ingame(v->server) <= 1)
		return lobby_init(v->server);

	if (!g_config.vanilla_balancing)
	{
		
		int new_threshold = (server_ingame(v->server) + 3) / 4;
		if (new_threshold < 1) new_threshold = 1;
		Packet pack;
		for (int c = 0; c <= CH_SALLY; c++)
		{
			if (!v->server->lobby.avail[c]) continue;
			if ((int)v->server->lobby.surv_reservations[c] >= new_threshold)
			{
				v->server->lobby.avail[c] = 0;
				for (size_t j = 0; j < v->server->peers.capacity; j++)
				{
					PeerData *r = (PeerData *)v->server->peers.ptr[j];
					if (!r || !r->in_game || r->surv_char != c) continue;
					PacketCreate(&pack, SERVER_LOBBY_CHARACTER_CHANGE);
					PacketWrite(&pack, packet_write16, r->id);
					PacketWrite(&pack, packet_write8, c + 1);
					server_broadcast(v->server, &pack, true);
				}
			}
		}
	}

	return charselect_check_state(v->server) || lobby_init(v->server);
}
