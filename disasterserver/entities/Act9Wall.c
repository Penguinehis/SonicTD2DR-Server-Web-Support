#include "Server.h"
#include <Config.h>
#include <entities/Act9Wall.h>

bool act9wall_init(Server* server, Entity* entity)
{
	Act9Wall* wall = (Act9Wall*)entity;
	wall->start_time = 0.0;
	wall->init_elapsed = server->game.elapsed;
	wall->shrink_started = false;
	return true;
}

bool act9wall_tick(Server* server, Entity* entity)
{
	Act9Wall* wall = (Act9Wall*)entity;

	if (!server->game.started)
		return true;

	double time = (server->game.time_sec * TICKSPERSEC + server->game.time);
	if (!wall->shrink_started)
	{
		// The original: 5 s in, and only with 2:30 or less left (its Act 9 rounds are about that
		// long, so the walls close through the whole round). The port's balancing makes rounds
		// longer (4 minutes with a full lobby): there the walls would stand still for the first
		// half, so they start 5 s in and close through the whole round as well.
		if (server->game.elapsed - wall->init_elapsed < 5.0 * TICKSPERSEC ||
			(g_config.vanilla_balancing && time > 150.0 * TICKSPERSEC))
			return true;

		wall->start_time = time;
		wall->shrink_started = true;
	}

	if (wall->start_time <= 0.0)
		return true;

	double off = (double)(wall->start_time - time) / (double)wall->start_time;

	double x = wall->pos.x * off;
	double y = wall->pos.y * off;
	
	Packet pack;
	PacketCreate(&pack, SERVER_ACT9WALL_STATE);
	PacketWrite(&pack, packet_write8, wall->wid);
	PacketWrite(&pack, packet_write16, (uint16_t)x);
	PacketWrite(&pack, packet_write16, (uint16_t)y);
	server_broadcast(server, &pack, false);
	
	switch(wall->wid)
	{
		case 0:
		{
			x = -2240;
			y = y - 768;
			break;
		}

		case 1:
		{
			x = x - 2240;
			break;
		}

		case 2:
		{
			x = (3072 - x) + 64;
			break;
		}

		default:
		{
			Err("Invalid wall id!");
			return false;
		}
	}

	return true;
}