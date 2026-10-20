#include <Maps.h>
#include <Player.h>
#include <Server.h>
#include <Zone.h>

bool player_add_error(Server* server, PeerData* v, uint16_t by)
{
	v->plr.errors += by;

	Debug("%d error is now %d", v->id, v->plr.errors);
	if (v->plr.errors >= 30000)
	{
		server_disconnect(server, v->peer, DR_OTHER, "Fun fact: dial-up internet connection was relevant 20 years ago!");
		return false;
	}

	return true;
}

void player_check_zone(Server* server, PeerData* v)
{
	if (!g_config.anticheat)
		return;

	if (!server->game.started)
		return;

	if (server->game.map == 8 &&
		server->game.elapsed < 2.0 * TICKSPERSEC)
		return;

	if (v->plr.pos.x == 0 && v->plr.pos.y == 0)
		return;

	if (server->game.map < 0 || server->game.map > MAP_COUNT)
		return;  /* a mod's map: no zones */

	const Zone* zones = g_mapZone[server->game.map];
	const size_t zonesSize = g_mapZoneSize[server->game.map];
const ZoneBounds* bounds = &g_mapZoneBounds[server->game.map];
if (bounds->valid &&
    (v->plr.pos.x < bounds->min_x || v->plr.pos.y < bounds->min_y ||
     v->plr.pos.x > bounds->max_x || v->plr.pos.y > bounds->max_y))
return;

	for (size_t i = 0; i < zonesSize; i++)
	{
		const Zone zone = zones[i];
		if (v->plr.pos.x >= zone.x && v->plr.pos.y >= zone.y && v->plr.pos.x <= zone.x + zone.w && v->plr.pos.y <= zone.y + zone.h)
		{
			Debug("%d is inside invalid area %d", v->id, i);
			if (!player_add_error(server, v, server->game.map == 6 ? 30 : 1000))
				return;
		}
	}
}

void player_history_reset(Player *player)
{
    player->history_head = 0;
    player->history_count = 0;
}

void player_history_append(
    Player *player,
    double elapsed,
    Vector2 pos,
    uint8_t state,
    bool attacking)
{
    PlayerHistorySample *sample = &player->history[player->history_head];

    *sample = (PlayerHistorySample)
    {
        .elapsed = elapsed,
        .pos = pos,
        .state = state,
        .attacking = attacking
    };

    player->history_head = (player->history_head + 1) % PLAYER_HISTORY_TICKS;

    if (player->history_count < PLAYER_HISTORY_TICKS)
        player->history_count++;
}

bool player_history_at_or_before(
    const Player *player,
    double target_elapsed,
    PlayerHistorySample *out)
{
    if (player->history_count == 0)
        return false;

    bool found = false;
    PlayerHistorySample oldest = {0};

    for (uint16_t i = 0; i < player->history_count; i++)
    {
        uint16_t index =
            (player->history_head + PLAYER_HISTORY_TICKS
             - player->history_count + i)
            % PLAYER_HISTORY_TICKS;
        PlayerHistorySample sample = player->history[index];

        if (i == 0)
            oldest = sample;

        if (sample.elapsed <= target_elapsed)
        {
            *out = sample;
            found = true;
        }
    }

    if (!found)
        *out = oldest;

    return true;
}

