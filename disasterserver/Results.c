#include <Player.h>
#include <Packet.h>
#include <Server.h>
#include <Colors.h>
#include <States.h>
#include <stdlib.h>

#define PLRSTATE_ESCAPED 4
#define PLRSTATE_ALIVE 3
#define PLRSTATE_DEAD 2
#define PLRSTATE_DEMONIZED 1
#define PLRSTATE_EXE 0
#define RESULTS_MAX_PLAYERS (MAX_SUPPORTED_PLAYERS * 2)

static uint16_t results_counter16(uint32_t value)
{
	if (value > UINT16_MAX)
		return UINT16_MAX;

	return (uint16_t)value;
}

const char* SHAMES_1[] = 
{
	CLRCODE_RED "(my skill issue makes me allergic to moving)",
	CLRCODE_RED "(i'm afraid to leave my camp spot)",
	CLRCODE_RED "(how am i not bored of camping)",
	CLRCODE_RED "(i'm too bad to move around the map)",
	CLRCODE_RED "(i just like being a brick)"
};
#define SHAMES1_CNT 5

const char* SHAMES_2[] = 
{
	CLRCODE_RED "(i can't win without camping bodies)",
	CLRCODE_RED "(i'm afraid revived players will make me lose)",
	CLRCODE_RED "(i camp bodies cuz im bad)"
};
#define SHAMES2_CNT 3

int compare_results(const void* plr1, const void* plr2)
{
	const PeerData* a = *(const PeerData**)plr1;
	const PeerData* b = *(const PeerData**)plr2;
	int a_state;
	int b_state;

	if (a->plr.flags & PLAYER_KILLER)
		a_state = 0;
	else if (a->plr.flags & PLAYER_LEFT)
		a_state = 4;
	else if (a->plr.flags & PLAYER_DEMONIZED)
		a_state = 3;
	else if (a->plr.flags & PLAYER_DEAD)
		a_state = 2;
	else
		a_state = 1;

	if (b->plr.flags & PLAYER_KILLER)
		b_state = 0;
	else if (b->plr.flags & PLAYER_LEFT)
		b_state = 4;
	else if (b->plr.flags & PLAYER_DEMONIZED)
		b_state = 3;
	else if (b->plr.flags & PLAYER_DEAD)
		b_state = 2;
	else
		b_state = 1;

	if (a_state != b_state)
		return a_state - b_state;

	if (a->plr.stats.danger_time < b->plr.stats.danger_time)
		return 1;

	if (a->plr.stats.danger_time > b->plr.stats.danger_time)
		return -1;

	return (a->id > b->id) - (a->id < b->id);
}

bool results_send(Server* server, PeerData* v, PeerData* data, bool has_quit)
{
	uint8_t type = PLRSTATE_ALIVE;

	if (data->plr.flags & PLAYER_ESCAPED)
		type = PLRSTATE_ESCAPED;
	if (data->plr.flags & PLAYER_DEMONIZED)
		type = PLRSTATE_DEMONIZED;
	else if (data->plr.flags & PLAYER_DEAD)
		type = PLRSTATE_DEAD;
	else if (server_is_exe(server, data->id) || (data->plr.flags & PLAYER_KILLER))
		type = PLRSTATE_EXE;

	Packet pack;
	PacketCreate(&pack, SERVER_RESULTS_DATA);

	String nickname;
	const char* postfix = "";

	if (g_config.pride)
	{
		if (data->plr.stats.brain_damage && (data->plr.flags & PLAYER_ESCAPED))
			postfix = SHAMES_1[rand() % SHAMES1_CNT];

		if (data->plr.stats.camp_time >= 30 * TICKSPERSEC)
			postfix = SHAMES_2[rand() % SHAMES2_CNT];
	}

	nickname.len = snprintf(nickname.value, 129, "%s %s", data->nickname.value, postfix) + 1;

	PacketWrite(&pack, packet_writestr, nickname);
	PacketWrite(&pack, packet_write8, 	data->exe_char != EX_NONE ? data->exe_char : data->surv_char);
	PacketWrite(&pack, packet_write8,	server->game.ending);
	PacketWrite(&pack, packet_write16,	server->game.time_sec)
	PacketWrite(&pack, packet_write8,	has_quit);
	PacketWrite(&pack, packet_write8,	type);

	PacketWrite(&pack, packet_write16,		results_counter16(data->plr.stats.rings));
	PacketWrite(&pack, packet_write16,		results_counter16(data->plr.stats.kills));
	PacketWrite(&pack, packet_write16,		results_counter16(data->plr.stats.damage));
	PacketWrite(&pack, packet_write16,		results_counter16(data->plr.stats.damage_taken));
	PacketWrite(&pack, packet_write16,		results_counter16(data->plr.stats.stun_time));
	PacketWrite(&pack, packet_write16,		results_counter16(data->plr.stats.stuns));
	PacketWrite(&pack, packet_write16,		results_counter16(data->plr.stats.hp_restored));
	PacketWrite(&pack, packet_writedouble,	data->plr.stats.survive_time);
	PacketWrite(&pack, packet_writedouble,	data->plr.stats.danger_time);

	return packet_send(v->peer, &pack, true);
}

bool results_init(Server* server)
{
	Debug("Attepting to enter ST_RESULTS...");
	server->state = ST_RESULTS;
	server->results.countdown = 15 * TICKSPERSEC;

	Packet pack;
	PacketCreate(&pack, SERVER_RESULTS);
	PacketWrite(&pack, packet_write8, server->game.map);
	server_broadcast(server, &pack, true);

	Info("Server is now in " LOG_PUR "Results");
	return true;
}

bool results_uninit(Server* server)
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
		PeerData* player = (PeerData*)server->game.left.ptr[i];
		if (!player)
			continue;

		free(player);
	}
	dylist_free(&server->game.left);
	free(server->game.pending_demonize);
	server->game.pending_demonize = NULL;

	return lobby_init(server);
}

bool results_state_tick(Server* server)
{
	server->results.countdown -= server->delta;
	
	if (server->results.countdown <= 0)
		return results_uninit(server);

	return true;
}

bool results_state_handle(PeerData* v, Packet* packet)
{
	PacketRead(_passtrough, packet, packet_read8, uint8_t);
	PacketRead(type, packet, packet_read8, uint8_t);

	switch (type) 
	{
		case CLIENT_RESULTS_REQUEST:
		{
			PeerData* sort[RESULTS_MAX_PLAYERS];
			size_t len = 0;
			memset(sort, 0, sizeof(sort));

			for (size_t i = 0; i < v->server->peers.capacity; i++)
			{
				PeerData* data = (PeerData*)v->server->peers.ptr[i];
				if (!data)
					continue;

				if(!data->in_game)
					continue;

				if (len >= RESULTS_MAX_PLAYERS)
					break;

				sort[len++] = data;
			}

			for (size_t i = 0; i < v->server->game.left.capacity; i++)
			{
				PeerData* data = (PeerData*)v->server->game.left.ptr[i];
				if (!data)
					continue;

				if(!data->in_game)
					continue;

				if (len >= RESULTS_MAX_PLAYERS)
					break;

				sort[len++] = data;
			}

			qsort(sort, len, sizeof(PeerData*), compare_results);

			for (size_t i = 0; i < len; i++)
			{
				PeerData* data = (PeerData*)sort[i];
				if (!data)
					continue;

				RAssert(results_send(v->server, v, data, data->plr.flags & PLAYER_LEFT));
			}
			break;
		}
		
		case CLIENT_CHAT_MESSAGE:
		{
			if (v->in_game)
				break;

			PacketRead(pid, packet, packet_read16, uint16_t);
			PacketRead(msg, packet, packet_readstr, String);
			AssertOrDisconnect(v->server, string_length(&msg) <= 40);

			v->timeout = 0;

			Info("[%s] (id %d): %s", v->nickname.value, v->id, msg.value);
			if (!server_cmd_handle(v->server, server_cmd_parse(&msg), v, &msg))
				server_broadcast_ex(v->server, packet, true, v->id);

			break;
		}
	}

	return true;
}