#include <Mod.h>
#include "Server.h"
#include <States.h>
#include <Maps.h>
#include <CMath.h>
#include <Colors.h>
#include <time.h>

bool mapvote_check_state(Server* server)
{
	RAssert(server);

	uint8_t count = 0;
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* peer = (PeerData*)server->peers.ptr[i];
		if (!peer)
			continue;

		if (peer->in_game && peer->voted)
			count++;
	}

	if (count >= server_ingame(server))
	{
		if (server->lobby.countdown_sec > 3)
		{
			server->lobby.countdown = 0;
			server->lobby.countdown_sec = 4;
		}
	}

	return true;
}

bool mapvote_state_handle(PeerData* v, Packet* packet)
{
	
	PacketRead(passtrough, packet, packet_read8, uint8_t);
	PacketRead(type, packet, packet_read8, uint8_t);

	Packet pack;
	bool res = true;

	switch (type)
	{			
		case CLIENT_VOTE_REQUEST:
		{
			if(!v->in_game)
				break;

			PacketRead(map, packet, packet_read8, uint8_t);
			AssertOrDisconnect(v->server, !v->voted);
			AssertOrDisconnect(v->server, map < 3);

			v->server->lobby.votes[map]++;
			v->voted = true;

			PacketCreate(&pack, SERVER_VOTE_SET);
			for (int i = 0; i < 3; i++)
				PacketWrite(&pack, packet_write8, v->server->lobby.votes[i]);

			Info("%s " LOG_RST "(id %d) voted for [" LOG_BLU "%s" LOG_RST "]!", v->nickname.value, v->id, g_mapList[v->server->lobby.maps[map]].name);
			server_broadcast(v->server, &pack, true);
			mapvote_check_state(v->server);
			break;
		}

		case CLIENT_CHAT_MESSAGE:
		{
			PacketRead(pid, packet, packet_read16, uint16_t);
			PacketRead(msg, packet, packet_readstr, String);
			AssertOrDisconnect(v->server, string_length(&msg) <= 40);

			v->timeout = 0;
			Info("%s " LOG_RST "(id %d): %s", v->nickname.value, v->id, msg.value);
			if (!server_cmd_handle(v->server, server_cmd_parse(&msg), v, &msg))
				server_broadcast_ex(v->server, packet, true, v->id);

			break;
		}

		default:
			RAssert(server_msg_handle(v->server, type, v, packet));
			break;
	}

	return res;
}

bool mapvote_pool_init(Server* server)
{
	int enabled[MAP_COUNT];
	int ecnt = 0;

	MutexLock(g_config.map_list_lock);
	for (int i = 0; i < MAP_COUNT; i++)
	{
		if (g_config.map_list[i])
			enabled[ecnt++] = i;
	}
	MutexUnlock(g_config.map_list_lock);

	server->lobby.pool_size = 0;
	if (ecnt == 0)
		return true;

	int target = (ecnt * 34 + 99) / 100;
	if (target < 3)
		target = 3;

	for (int i = ecnt - 1; i > 0; i--)
	{
		int j = rand() % (i + 1);
		int t = enabled[i];
		enabled[i] = enabled[j];
		enabled[j] = t;
	}

for (int i = 0; i < ecnt && server->lobby.pool_size < target; i++)
{
bool waiting = false;

for (int j = 0; j < server->lobby.wait_size; j++)
{
if (server->lobby.wait_queue[j] == enabled[i])
{
waiting = true;
break;
}
}

if (!waiting)
server->lobby.pool[server->lobby.pool_size++] =
(uint8_t)enabled[i];
}

for (int i = 0; server->lobby.pool_size < target; i++)
server->lobby.pool[server->lobby.pool_size++] =
(uint8_t)enabled[i % ecnt];

	return true;
}

bool mapvote_state_tick(Server* server)
{
	bool res = true;

	if (server->lobby.countdown <= 0)
	{
		server->lobby.countdown += TICKSPERSEC;
	
		if (--server->lobby.countdown_sec == 0)
		{
			
			int8_t indeces[3] = { -1, -1, -1 };
			int count = 0;

			int largest = 0;
			for (int i = 0; i < 3; i++)
			{
				uint8_t values = server->lobby.votes[i];
				if (values > largest)
				{
					largest = values;
					count = 0;
				}

				if (values == largest)
				{
					indeces[count] = server->lobby.maps[i];
					count++;
				}
			}

			
			int8_t won = indeces[rand() % count];
			server->last_map = won;

			if (!g_config.vanilla_balancing)
			{
				for (int i = 0; i < server->lobby.pool_size; i++)
				{
					if (server->lobby.pool[i] != won)
						continue;

					for (int j = i + 1; j < server->lobby.pool_size; j++)
						server->lobby.pool[j - 1] = server->lobby.pool[j];
					server->lobby.pool_size--;
					break;
				}

				if (server->lobby.wait_size < sizeof(server->lobby.wait_queue))
					server->lobby.wait_queue[server->lobby.wait_size++] = (uint8_t)won;

				while (server->lobby.pool_size < 3 && server->lobby.wait_size > 0)
				{
					server->lobby.pool[server->lobby.pool_size++] = server->lobby.wait_queue[0];
					for (int i = 1; i < server->lobby.wait_size; i++)
						server->lobby.wait_queue[i - 1] = server->lobby.wait_queue[i];
					server->lobby.wait_size--;
				}

				while (server->lobby.pool_size > 0 &&
					server->lobby.pool_size < 3)
				{
					uint8_t map = server->lobby.pool[0];
					server->lobby.pool[server->lobby.pool_size++] = map;
				}
			}

			Info(LOG_YLW "Map is [" LOG_BLU "%s" LOG_RST "]", g_mapList[won].name);

			int8_t next_map = won;
if (server->lobby.pending_map_set)
{
next_map = server->lobby.pending_map;
server->lobby.pending_map = -1;
server->lobby.pending_map_set = false;
}

return charselect_init(next_map, server) || lobby_init(server);
		}

		Packet pack;
		PacketCreate(&pack, SERVER_VOTE_TIME_SYNC);
		PacketWrite(&pack, packet_write8, server->lobby.countdown_sec);
		server_broadcast(server, &pack, true);
	}

	server->lobby.countdown -= server->delta;
	
	return res;
}

bool mapvote_init(Server* server)
{
	Debug("Attepting to enter ST_MAPVOTE...");
	RAssert(server);

	
	time_t seed = time(NULL);
	Debug("Mapvote seed: %d", seed);
	srand((unsigned int)seed);

	RAssert(server);
	server->state = ST_MAPVOTE;
	server->lobby.countdown_sec = 30;
	server->lobby.countdown = TICKSPERSEC;
	memset(server->lobby.votes, 0, sizeof(server->lobby.votes));

if (!g_config.vanilla_balancing)
		RAssert(mapvote_pool_init(server));

	uint8_t source[64];
	int source_size = 0;
	MutexLock(g_config.map_list_lock);
	if (g_config.vanilla_balancing)
	{
		for (int i = 0; i < MAP_COUNT; i++)
		{
			if (g_config.map_list[i])
				source[source_size++] = (uint8_t)i;
		}
	}
	else
	{
		source_size = server->lobby.pool_size;
		memcpy(source, server->lobby.pool, (size_t)source_size);
	}
	MutexUnlock(g_config.map_list_lock);
	mod_add_vote_maps(source, &source_size, (int)sizeof(source));  /* a mod's maps */

	if (source_size > 0)
	{
		for (int i = 0; i < 3; i++)
		{
			int attempts = 0;
			uint8_t map;
			do
			{
				map = source[rand() % source_size];
				attempts++;
			}
			while (attempts < 100 &&
				((map == server->last_map && source_size > 1) ||
				(i > 0 && (map == server->lobby.maps[0] ||
					(i > 1 && map == server->lobby.maps[1])))));
			server->lobby.maps[i] = map;
		}
	}

	Packet pack;
	PacketCreate(&pack, SERVER_VOTE_MAPS);
	for (int i = 0; i < 3; i++)
		PacketWrite(&pack, packet_write8, server->lobby.maps[i]);
	server_broadcast(server, &pack, true);

	PacketCreate(&pack, SERVER_VOTE_TIME_SYNC);
	PacketWrite(&pack, packet_write8, server->lobby.countdown_sec);
	server_broadcast(server, &pack, true);

	Info(LOG_YLW "Server is now in " LOG_PUR "Map Vote");
	Info("Maps: " LOG_RED "[%s] " LOG_BLU "[%s] " LOG_YLW "[%s]", g_mapList[server->lobby.maps[0]].name, g_mapList[server->lobby.maps[1]].name, g_mapList[server->lobby.maps[2]].name);
	return true;
}

bool mapvote_state_join(PeerData* v)
{
	return true;
}

bool mapvote_state_left(PeerData* v)
{
	if(server_ingame(v->server) <= 1)
		return lobby_init(v->server);

	RAssert(mapvote_check_state(v->server));
	return true;
}