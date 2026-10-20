#ifndef CONFIG_H
#define CONFIG_H

#include <cJSON.h>
#include <Api.h>
#include <io/Threads.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef SYS_ANDROID
#define CONFIG_FILE "Config.json"
#define BANS_FILE "Bans.json"
#define OPERATORS_FILE "Operators.json"
#define TIMEOUTS_FILE "Timeouts.json"
#define REPORTS_FILE "Reports.json"
#define LOBBY_TIMEOUTS_FILE "LobbyTimeouts.json"
#else
#define ANDROID_DIR "/data/data/com.teamexeempire.disaster2d/files/"
#define CONFIG_FILE ANDROID_DIR "Config.json"
#define BANS_FILE ANDROID_DIR "Bans.json"
#define OPERATORS_FILE ANDROID_DIR "Operators.json"
#define TIMEOUTS_FILE ANDROID_DIR "Timeouts.json"
#define REPORTS_FILE ANDROID_DIR "Reports.json"
#define LOBBY_TIMEOUTS_FILE ANDROID_DIR "LobbyTimeouts.json"
#endif

/* Message of the day is a list of chat lines; a single string still works. */
#define MOTD_MAX_LINES 12
#define MOTD_LINE_SIZE 256

typedef struct
{
int32_t port;
int32_t max_simultaneous_players;
int32_t server_count;
int32_t ping_limit;
bool log_debug;
bool log_file;
bool anticheat;
bool pride;
bool vanilla_balancing;
bool gamemodes[10];
float retained_velocity_decay;
int retained_velocity_decay_start_ticks;
int retained_extrapolation_limit_ticks_x;
int retained_extrapolation_limit_ticks_y;
int relay_fill_interval_ticks;
int relay_heartbeat_interval_ticks;
int relay_correction_interval_ticks;
int relay_priority_interval_ticks;
bool map_list[20];
Mutex map_list_lock;

char motd[MOTD_MAX_LINES][MOTD_LINE_SIZE];
int32_t motd_lines;

/* moderation */
int32_t votekick_vote_seconds;
int32_t votekick_start_cooldown_seconds;
int32_t votekick_lobby_block_seconds;
int32_t report_cooldown_seconds;

/* browser players (WebGate.c) */
bool web_clients;
/* trusted WebSocket proxies (WsTrust.h): names / addresses, comma separated */
char web_proxies[256];

/* relay */
bool relay_enabled;
char relay_directory_host[64];
int32_t relay_directory_port;
char relay_username[32];
char relay_password[64];
} Config;

SERVER_API extern Config g_config;
SERVER_API extern cJSON* g_bans;
SERVER_API extern cJSON* g_timeouts;
SERVER_API extern cJSON* g_ops;
SERVER_API extern cJSON* g_reports;
SERVER_API extern cJSON* g_lobbyTimeouts;
SERVER_API extern Mutex g_banMut;
SERVER_API extern Mutex g_timeoutMut;
SERVER_API extern Mutex g_opMut;
SERVER_API extern Mutex g_reportMut;
SERVER_API extern Mutex g_lobbyTimeoutMut;

SERVER_API bool config_init(void);
SERVER_API bool config_save(void);

SERVER_API bool ban_add(const char* nickname, const char* udid, const char* ip);
SERVER_API bool ban_revoke(const char* udid, const char* ip);
SERVER_API bool ban_check(const char* udid, const char* ip, bool* result);

SERVER_API bool timeout_set(const char* nickname, const char* udid, const char* ip, uint64_t timestamp);
SERVER_API bool timeout_revoke(const char* udid, const char* ip);
SERVER_API bool timeout_check(const char* udid, const char* ip, uint64_t* result);

SERVER_API bool op_add(const char* nickname, const char* ip);
SERVER_API bool op_revoke(const char* ip);
SERVER_API bool op_check(const char* ip, bool* result);

SERVER_API bool report_add(const char* reporter, const char* target_nickname, const char* target_udid,
	const char* target_ip, const char* reason, int32_t server_id);
SERVER_API bool report_clear(const char* key);
SERVER_API bool report_clear_all(void);

/* Per-lobby timeouts: a player kicked out of one server instance is kept out of
   that instance only, and the block survives a restart until it expires. */
SERVER_API bool lobby_timeout_set(const char* nickname, const char* udid, const char* ip,
	int32_t server_id, uint64_t until);
SERVER_API bool lobby_timeout_check(const char* udid, const char* ip, int32_t server_id, uint64_t* until);
SERVER_API bool lobby_timeout_revoke(const char* udid, const char* ip, int32_t server_id);
SERVER_API bool lobby_timeout_clear_all(void);

bool collection_save(const char* file, cJSON* value);

#endif
