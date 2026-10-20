#include <entities/Ring.h>
#include <stdint.h>
#include <CMath.h>

bool ring_init(Server* server, Entity* entity)
{
	Ring* ring = (Ring*)entity;

	int cnt = 0;
	for (int i = 0; i < g_mapList[server->game.map].ring_count; i++)
	{
		if (server->game.rings[i])
			cnt++;
	}

	if (cnt >= g_mapList[server->game.map].ring_count)
		return false;

	int rnd;
gen:
	rnd = rand() % g_mapList[server->game.map].ring_count;

	if (server->game.rings[rnd])
		goto gen;

	server->game.rings[rnd] = true;
	ring->rid = (uint8_t)rnd;

bool demonized = false;
for (size_t i = 0; i < server->peers.capacity; i++)
{
PeerData* peer = (PeerData*)server->peers.ptr[i];
if (peer && peer->in_game && (peer->plr.flags & PLAYER_DEMONIZED))
{
demonized = true;
break;
}
}
if (g_mapList[server->game.map].spawn_red_rings &&
!(server->game.duel_triggered && demonized))
{
int chance = server->game.base_red_ring_chance
- (3 * server->game.total_red_rings_spawned)
+ (4 * server->game.total_red_rings_picked_up);
if (chance > 66)
chance = 66;
if (chance < 0)
chance = 0;
ring->red = rand() % 100 < chance;
if (ring->red)
{
server->game.active_red_ring_count++;
server->game.total_red_rings_spawned++;
}
}
else
ring->red = 0;

	Packet pack;
	PacketCreate(&pack, SERVER_RING_STATE);
	PacketWrite(&pack, packet_write8, 0);
	PacketWrite(&pack, packet_write8, ring->rid);
	PacketWrite(&pack, packet_write16, ring->id);
	PacketWrite(&pack, packet_write8, ring->red);
	server_broadcast(server, &pack, true);

	return true;
}

bool ring_uninit(Server* server, Entity* entity)
{
	Ring* ring = (Ring*)entity;
	server->game.rings[ring->rid] = 0;

	Packet pack;
	PacketCreate(&pack, SERVER_RING_STATE);
	PacketWrite(&pack, packet_write8, 1);
	PacketWrite(&pack, packet_write8, ring->rid);
	PacketWrite(&pack, packet_write16, ring->id);
	server_broadcast(server, &pack, true);

	return true;
}