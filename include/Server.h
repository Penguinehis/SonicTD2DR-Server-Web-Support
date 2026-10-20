#ifndef SERVER_H
#define SERVER_H

#include <Auth.h>
#include <DyList.h>
#include <Lib.h>
#include <Log.h>
#include <Vote.h>
#include <Player.h>
#include <Packet.h>
#include <io/Threads.h>
#include <io/Time.h>
#include <enet/enet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define TICKSPERSEC 60
#define SERVER_MAX_EXES 8   /* a mod's round: up to 3 EXEs, or everyone (free-for-all, the first 8) */
#define MAX_SUPPORTED_PLAYERS 100
#define DEFAULT_MAX_SIMULTANEOUS_PLAYERS 100
#define MAP_FART_ZONE MAP_COUNT

#define PING_INITIAL_DELAY_TICKS (5 * TICKSPERSEC)
#define PING_REPORT_INTERVAL_TICKS (10 * TICKSPERSEC)
#define PING_REPORT_DELTA_MS 10
#define PING_REPORTS_PER_TICK 1
#define MAX_SERVICE_EVENTS_PER_LOOP 256

/* How long a worker may sleep when its lobby is completely empty. An empty
   ST_LOBBY has no per-tick work, and a connecting peer wakes the select()
   inside enet_host_service immediately, so this only costs idle wake-ups. */
#define SERVER_IDLE_SLEEP_MS 100

#define RELAY_HEARTBEAT_INTERVAL_TICKS (2 * TICKSPERSEC)
#define MAX_CATCHUP_TICKS 3
#define RELAY_PRIORITY_DURATION_TICKS ((uint32_t)(3.5 * TICKSPERSEC))
#define RELAY_COMBAT_RADIUS 800.0f
#define RELAY_TIER_STABILIZATION_TICKS (2 * TICKSPERSEC)
#define RELAY_TIER_DOWNGRADE_MARGIN 2
#define RELAY_AOI_RADIUS_PX 500.0f
#define RELAY_AOI_OFF_ZONE_INTERVAL_TICKS (TICKSPERSEC / 3)
#define RELAY_SEND_BUDGET 1024
#define GAME_ENTITY_CAPACITY 16384
#define GAME_MAX_DESPAWNS_PER_TICK 2048
#define MAX_CREAM_RINGS_QUERY 512

#ifndef PLAYER_HISTORY_TICKS
#define PLAYER_HISTORY_TICKS (4 * TICKSPERSEC)
#endif
#define HIGH_PING_DEFENDER_MS 150
#define MAX_DEFENDER_REWIND_MS 250

#define BUILD_VERSION 1101

#define STR_HELPER(x) #x
#define STRINGIFY(x) STR_HELPER(x)

struct Server;
struct auth_peer_data;
typedef struct
{
    Vector2 offset;
    float last_vx;
    float last_vy;
    uint32_t last_tick;
    uint32_t aoi_last_send_tick;
    uint16_t recipient_id;
    bool valid;
} RelayCorrectionState;

typedef struct PeerData
{
uint16_t id;
String ip;
ENetPeer *peer;

Player plr;
String nickname;
String udid;
uint8_t lobby_icon;
int8_t pet;

bool verified;
bool in_game;
bool op;
bool palette_from_default; /* the last palette "from" list was the default skin's no-op */
bool ready;
bool mod_tool;
bool is_mobile;
bool can_vote;
bool voted;
bool disconnecting;
/* mods (Mod.h): the web game's mod api (0: cannot load mods), the mod loaded, client events */
uint16_t mod_api;
uint32_t mod_anim_sec;   /* CLIENT_MOD_ANIM's rate (Mod.c) */
int mod_anim_count;
bool mod_ready;
uint32_t mod_event_tick;
uint8_t mod_event_count;
uint32_t mod_out_sec;     /* packets a mod sends to this player this second (Mod.c's limit) */
uint16_t mod_out_count;
uint8_t mod_sync_dirty[16];  /* game.sync keys (MOD_MAX_SYNC bits) this player still has to get */
bool is_bot;  /* always false here (Mod.c is shared with betterserverbots, which has bots) */

auth_peer_data auth;

enum
{
CH_NONE = -1,

CH_TAILS,
CH_KNUX,
CH_EGGMAN,
CH_AMY,
CH_CREAM,
CH_SALLY
} surv_char;

enum
{
EX_NONE = -1,

EX_ORIGINAL,
EX_CHAOS,
EX_EXETIOR,
EX_EXELLER
} exe_char;

bool should_timeout;

uint64_t last_report_time;

uint8_t exe_chance;
double timeout;
double vote_cooldown;

uint8_t player_data[PACKET_MAXSIZE - 2];
uint8_t player_data_len;
bool player_data_valid;
uint8_t last_relay_state;
Vector2 relay_latest_pos;
Vector2 relay_previous_pos;
uint16_t relay_latest_xspd;
uint16_t relay_latest_yspd;
uint32_t relay_latest_tick;
uint32_t relay_previous_tick;
uint32_t relay_update_interval;
uint32_t priority_until_tick;
double relay_next_elapsed;
uint32_t relay_last_fill_tick;
uint32_t relay_last_send_tick;
Vector2 relay_last_sent_pos;
float relay_last_sent_vx;
float relay_last_sent_vy;
bool relay_last_sent_valid;
bool relay_stop_pending;
bool relay_force_pending;
    RelayCorrectionState relay_corrections[MAX_SUPPORTED_PLAYERS + 1];

uint16_t ping_reported_value;
uint32_t ping_last_report_tick;
bool ping_has_report;
bool ping_initial_pending;

struct Server *server;
} PeerData;

typedef struct
{

double countdown;
uint8_t countdown_sec;
Vote vote;
PeerData kick_target;

uint8_t maps[3];
uint8_t votes[3];
uint8_t pool[20];
uint8_t pool_size;
uint8_t wait_queue[20];
uint8_t wait_size;

int8_t map;
uint16_t exe;
/* the EXEs (a mod's rounds can have several, or none: Mod "exes"); exe is the first, or UINT16_MAX */
uint16_t exes[SERVER_MAX_EXES];
uint8_t exe_count;
bool ffa;                  /* a mod's free-for-all: everyone an EXE */
bool avail[CH_SALLY + 1];
uint8_t surv_reservations[CH_SALLY + 1];

int8_t pending_map;
bool pending_map_set;
} Lobby;

typedef enum
{
BS_NONE,
BS_DEACTIVATED,
BS_ACTIVATED,
} BigRingState;

typedef enum
{
ED_EXEWIN,
ED_SURVWIN,
ED_TIMEOVER
} Ending;

typedef struct
{
int exe;                   /* the (first) EXE, -1 none */
uint16_t exes[SERVER_MAX_EXES];
uint8_t exe_count;
bool ffa;
int8_t map;
bool started;
bool sudden_death;
int match_initial_survivors;
int death_order_count;
bool last_minute_sweep_done;
bool sixty_second_sweep_done;
bool duel_triggered;
double duel_ring_clear_at;
double duel_boost_until;
double ring_spawn_disabled_until;
uint16_t *pending_demonize;
size_t pending_demonize_capacity;
size_t pending_demonize_count;

double start_timeout;
double time;
double elapsed;
uint16_t time_sec;
double end;
Ending ending;

BigRingState bring_state;
uint8_t bring_loc;

uint16_t entid;
DyList entities;

bool rings[256];
uint8_t ring_coff;
double ring_accum;
int base_red_ring_chance;
int active_red_ring_count;
int base_ring_coff;
int total_red_rings_spawned;
int total_red_rings_picked_up;
uint16_t cream_guard_id;
int onehit_checked_count;
bool last_survivor_disabled;

DyList left;

uint32_t tick;
uint32_t relay_accumulator;
uint8_t relay_tier;
uint8_t relay_pending_tier;
uint16_t relay_stable_ticks;
uint32_t ping_initial_tick;
uint16_t ping_cursor;
uint16_t relay_source_cursor;
uint16_t relay_recipient_cursor;
} Game;

typedef struct
{
double countdown;
} Results;

typedef struct Server
{
uint16_t id;
atomic_bool running;

enum
{
ST_LOBBY,
ST_MAPVOTE,
ST_CHARSELECT,
ST_GAME,
ST_RESULTS
} state;
Mutex state_lock;

Lobby lobby;
Game game;
Results results;

int8_t last_map;

double delta;
DyList peers;
ENetHost *host;

uint32_t tick;
void *mod;  /* the lobby's mod Lua (Mod.c), NULL without a mod */
} Server;

typedef struct
{
int8_t index;
bool regular_rotation;
} NormalizedMap;

bool server_normalize_map_argument(const char *text, NormalizedMap *out);

bool server_state_joined(PeerData *v);
bool server_state_left(PeerData *v);
bool server_state_handle(PeerData *v, Packet *packet);
bool server_msg_handle(Server *server, PacketType type, PeerData *v, Packet *packet);
bool server_cmd_handle(Server *server, unsigned long hash, PeerData *v, String *msg);
unsigned long server_cmd_parse(String *string);

bool server_global_init(void);
bool server_worker(Server *server);
bool server_broadcast(Server *server, Packet *packet, bool reliable);
bool server_broadcast_ex(Server *server, Packet *packet, bool reliable, uint16_t ignore);
bool server_send_msg(Server *server, ENetPeer *peer, const char *message);
bool server_send_motd(Server *server, ENetPeer *peer);
bool server_broadcast_msg(Server *server, const char *message);
bool relay_cache_player_data(PeerData *source, const Packet *incoming);
void relay_invalidate_player_data(PeerData *source);
void relay_mark_priority_near(Server *server, const Vector2 *center);
void server_relay_fill(Server *server);
void server_relay_tick(Server *server);

int server_total(Server *server);
int server_ingame(Server *server);
/* plays an EXE (this round's, or in character select the chosen ones) */
bool server_is_exe(Server *server, uint16_t id);
PeerData *server_find_peer(Server *server, uint16_t id);
void report_normalize_name(const char *src, char *dst, size_t dst_size);
PeerData *server_resolve_peer(Server *server, const char *query, const char **problem);
bool server_disconnect(Server *server, ENetPeer *peer, DisconnectReason reason, const char *text);
bool server_disconnect_id(Server *server, uint16_t id, DisconnectReason reason, const char *text);

#endif
