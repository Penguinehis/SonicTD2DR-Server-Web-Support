#ifndef PLAYER_H
#define PLAYER_H
#include <io/Time.h>
#include <Packet.h>
#include <CMath.h>
#include <stdint.h>
#include <stdbool.h>

#define PLAYER_HISTORY_TICKS (4 * 60)
typedef enum
{
PLAYER_NONE = 0,
PLAYER_ESCAPED = 0x1 << 0,
PLAYER_DEAD = 0x1 << 1,
PLAYER_DEMONIZED = 0x1 << 2,
PLAYER_REVIVED = 0x1 << 3,
PLAYER_CANTREVIVE = 0x1 << 4,
PLAYER_LEFT = 0x1 << 5,
PLAYER_KILLER = 0x1 << 6,
PLAYER_FIRST_HALF_DEAD = 0x1 << 7,
PLAYER_FROZEN_DEMON = 0x1 << 8
} PlayerFlags;
typedef struct
{
    double elapsed;
    Vector2 pos;
    uint8_t state;
    bool attacking;
} PlayerHistorySample;
typedef struct
{
uint8_t ready;
uint16_t seq;
uint16_t errors;
uint8_t ex_teleport;
double timeout;
uint8_t mod_tool;
uint32_t mod_tool_timer;
uint32_t chunk;

bool is_attacking;
double attack_timer;
TimeStamp last_attack;
uint16_t ping_last;
double ping_total;
double ping_timer;
uint16_t rings;
TimeStamp last_rings;
TimeStamp tails_last_proj;
uint16_t heal_rings;
uint8_t state;
PlayerFlags flags;
bool kill_reported;
bool onehit_checked;
uint8_t death_timer_sec;
double death_timer;
double revival;
int32_t revival_init[5];
uintptr_t data[4];
Vector2 start_pos;
Vector2 pos;
double last_recv_elapsed;
uint32_t dead_fully_tick;
float recv_vx;
float recv_vy;
Vector2 prev_recv_pos;
bool prev_recv_valid;
/* the last HP the player's game sent (mods: player.hp, on_hp) */
int8_t net_hp;
bool net_valid;
bool net_hp_seen;
uint32_t prev_recv_tick;
PlayerHistorySample history[PLAYER_HISTORY_TICKS];
uint16_t history_head;
uint16_t history_count;
struct
{
double survive_time;
double danger_time;
double camp_time;
double braindead_time;
bool brain_damage;
uint32_t stun_time;
uint32_t stuns;
uint32_t hp_restored;
uint32_t rings;
    uint32_t damage_raw;
    uint32_t damage_taken_raw;
uint32_t damage;
uint32_t damage_taken;
uint32_t kills;
} stats;
} Player;
#define SET_FLAG(x, flag) x |= (flag)
#define DEL_FLAG(x, flag) x = x & ~(flag)
struct Server;
struct PeerData;
bool player_add_error(struct Server* server, struct PeerData* v, uint16_t by);
void player_check_zone(struct Server* server, struct PeerData* v);
void player_history_reset(Player *player);
void player_history_append(Player *player, double elapsed, Vector2 pos, uint8_t state, bool attacking);
bool player_history_at_or_before(const Player *player, double target_elapsed, PlayerHistorySample *out);
#endif
