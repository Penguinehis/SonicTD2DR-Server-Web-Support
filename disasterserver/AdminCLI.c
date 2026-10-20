#include <AdminCLI.h>
#include <Config.h>
#include <Lib.h>
#include <Log.h>
#include <Maps.h>
#include <Server.h>
#include <States.h>
#include <cJSON.h>

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define CLI_LINE_MAX 2048
#define CLI_DISPLAY_MAX 160

#if defined(__unix) || defined(__unix__)
#define CLI_MUTEX_LOCK(mut) ((void)pthread_mutex_lock(&(mut)))
#define CLI_MUTEX_UNLOCK(mut) ((void)pthread_mutex_unlock(&(mut)))
#else
#define CLI_MUTEX_LOCK(mut) ((void)WaitForSingleObject((mut), INFINITE))
#define CLI_MUTEX_UNLOCK(mut) ((void)ReleaseMutex((mut)))
#endif

static int g_cli_server = 0;

static const char *state_name(int state)
{
    switch (state)
    {
    case ST_LOBBY:      return "LOBBY";
    case ST_MAPVOTE:    return "MAPVOTE";
    case ST_CHARSELECT: return "CHARSELECT";
    case ST_GAME:       return "GAME";
    case ST_RESULTS:    return "RESULTS";
    default:            return "UNKNOWN";
    }
}

static void safe_text(const char *src, char *dst, size_t dst_size)
{
    if (!dst_size)
        return;

    size_t out = 0;
    if (!src)
        src = "";

    while (*src && out + 1 < dst_size)
    {
        unsigned char ch = (unsigned char)*src++;
        if (ch < 32 || ch == 127 || ch == 0x1b)
            ch = '?';
        dst[out++] = (char)ch;
    }
    dst[out] = '\0';
}

static char *skip_space(char *text)
{
    while (text && *text && isspace((unsigned char)*text))
        text++;
    return text;
}

static char *next_token(char **cursor)
{
    if (!cursor || !*cursor)
        return NULL;

    char *start = skip_space(*cursor);
    if (!start || !*start)
    {
        *cursor = start;
        return NULL;
    }

    if (*start == '\'' || *start == '"')
    {
        char quote = *start++;
        char *read = start;
        char *write = start;

        while (*read && *read != quote)
        {
            if (*read == '\\' && read[1])
                read++;
            *write++ = *read++;
        }
        *write = '\0';
        if (*read == quote)
            read++;
        *cursor = skip_space(read);
        return start;
    }

    char *end = start;
    while (*end && !isspace((unsigned char)*end))
        end++;
    if (*end)
        *end++ = '\0';
    *cursor = skip_space(end);
    return start;
}

static bool parse_long(const char *text, long minimum, long maximum, long *out)
{
    if (!text || !*text || !out)
        return false;

    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno || !end || *end || value < minimum || value > maximum)
        return false;

    *out = value;
    return true;
}

static Server *selected_server(void)
{
    Server *server = disaster_get(g_cli_server);
    if (!server)
        printf("[admin] Server %d is unavailable. Use 'servers' or 'use <id>'.\n", g_cli_server);
    return server;
}

static void print_server_status(Server *server)
{
    if (!server)
        return;

    CLI_MUTEX_LOCK(server->state_lock);
    int total = server_total(server);
    int ingame = server_ingame(server);
    int state = server->state;
    int map = server->game.map;
    uint32_t tick = server->tick;
    uint16_t game_time = server->game.time_sec;
    CLI_MUTEX_UNLOCK(server->state_lock);

    printf("server=%u port=%d state=%s players=%d/%d ingame=%d tick=%u",
           server->id,
           g_config.port + server->id,
           state_name(state),
           total,
           g_config.max_simultaneous_players,
           ingame,
           tick);

    if (state == ST_GAME && map >= 0 && map <= MAP_FART_ZONE)
    {
        char map_name[CLI_DISPLAY_MAX];
        safe_text(g_mapList[map].name, map_name, sizeof(map_name));
        printf(" map=%d:%s time=%us", map, map_name, game_time);
    }
    puts("");
}

static void cmd_servers(void)
{
    int count = disaster_count();
    if (count <= 0)
    {
        puts("[admin] No server instances are configured.");
        return;
    }

    for (int i = 0; i < count; i++)
        print_server_status(disaster_get(i));
}

static void cmd_players(void)
{
    Server *server = selected_server();
    if (!server)
        return;

    puts(" ID  OP GAME  PING  NICKNAME                         IP                    UDID");
    puts("---- -- ---- -----  -------------------------------- --------------------- --------------------------------");

    int count = 0;
    CLI_MUTEX_LOCK(server->state_lock);
    for (size_t i = 0; i < server->peers.capacity; i++)
    {
        PeerData *peer = (PeerData *)server->peers.ptr[i];
        if (!peer)
            continue;

        char nickname[CLI_DISPLAY_MAX];
        char ip[CLI_DISPLAY_MAX];
        char udid[CLI_DISPLAY_MAX];
        safe_text(peer->nickname.value, nickname, sizeof(nickname));
        safe_text(peer->ip.value, ip, sizeof(ip));
        safe_text(peer->udid.value, udid, sizeof(udid));

        printf("%4u  %-2s %-4s %5u  %-32.32s %-21.21s %.64s\n",
               peer->id,
               peer->op ? "Y" : "N",
               peer->in_game ? "Y" : "N",
               peer->plr.ping_last,
               nickname,
               ip,
               udid);
        count++;
    }
    CLI_MUTEX_UNLOCK(server->state_lock);

    if (!count)
        puts("[admin] No connected players.");
}

static bool snapshot_peer(Server *server, uint16_t id, char *nickname, size_t nickname_size,
                          char *udid, size_t udid_size, char *ip, size_t ip_size)
{
    bool found = false;
    CLI_MUTEX_LOCK(server->state_lock);
    PeerData *peer = server_find_peer(server, id);
    if (peer)
    {
        snprintf(nickname, nickname_size, "%s", peer->nickname.value);
        snprintf(udid, udid_size, "%s", peer->udid.value);
        snprintf(ip, ip_size, "%s", peer->ip.value);
        found = true;
    }
    CLI_MUTEX_UNLOCK(server->state_lock);
    return found;
}

static void disconnect_if_same(Server *server, uint16_t id, const char *udid, DisconnectReason reason)
{
    CLI_MUTEX_LOCK(server->state_lock);
    PeerData *peer = server_find_peer(server, id);
    if (peer && strcmp(peer->udid.value, udid) == 0)
        server_disconnect(server, peer->peer, reason, NULL);
    CLI_MUTEX_UNLOCK(server->state_lock);
}

static void cmd_ban_online(const char *id_text)
{
    Server *server = selected_server();
    if (!server)
        return;

    long id_value;
    if (!parse_long(id_text, 1, 65535, &id_value))
    {
        puts("usage: ban <player-id>");
        return;
    }

    char nickname[256], udid[256], ip[256];
    if (!snapshot_peer(server, (uint16_t)id_value, nickname, sizeof(nickname), udid, sizeof(udid), ip, sizeof(ip)))
    {
        printf("[admin] Player id %ld is not connected to server %d.\n", id_value, g_cli_server);
        return;
    }

    if (!ban_add(nickname, udid, ip))
    {
        puts("[admin] Failed to persist the ban; player was not disconnected.");
        return;
    }

    disconnect_if_same(server, (uint16_t)id_value, udid, DR_BANNEDBYHOST);

    char safe_name[CLI_DISPLAY_MAX], safe_ip[CLI_DISPLAY_MAX], safe_udid[CLI_DISPLAY_MAX];
    safe_text(nickname, safe_name, sizeof(safe_name));
    safe_text(ip, safe_ip, sizeof(safe_ip));
    safe_text(udid, safe_udid, sizeof(safe_udid));
    printf("[admin] Banned id=%ld name='%s' ip=%s udid=%s\n", id_value, safe_name, safe_ip, safe_udid);
}

static void cmd_kick(const char *id_text, const char *seconds_text)
{
    Server *server = selected_server();
    if (!server)
        return;

    long id_value;
    long seconds = 5;
    if (!parse_long(id_text, 1, 65535, &id_value) ||
        (seconds_text && !parse_long(seconds_text, 0, 86400, &seconds)))
    {
        puts("usage: kick <player-id> [timeout-seconds]");
        return;
    }

    char nickname[256], udid[256], ip[256];
    if (!snapshot_peer(server, (uint16_t)id_value, nickname, sizeof(nickname), udid, sizeof(udid), ip, sizeof(ip)))
    {
        printf("[admin] Player id %ld is not connected to server %d.\n", id_value, g_cli_server);
        return;
    }

    if (seconds > 0 && !timeout_set(nickname, udid, ip, (uint64_t)time(NULL) + (uint64_t)seconds))
    {
        puts("[admin] Failed to persist the temporary timeout; player was not disconnected.");
        return;
    }

    disconnect_if_same(server, (uint16_t)id_value, udid, DR_KICKEDBYHOST);
    printf("[admin] Kicked player id %ld%s.\n", id_value, seconds > 0 ? " with temporary reconnect timeout" : "");
}

static void cmd_op_online(const char *id_text)
{
    Server *server = selected_server();
    if (!server)
        return;

    long id_value;
    if (!parse_long(id_text, 1, 65535, &id_value))
    {
        puts("usage: op <player-id>");
        return;
    }

    char nickname[256], udid[256], ip[256];
    if (!snapshot_peer(server, (uint16_t)id_value, nickname, sizeof(nickname), udid, sizeof(udid), ip, sizeof(ip)))
    {
        printf("[admin] Player id %ld is not connected to server %d.\n", id_value, g_cli_server);
        return;
    }

    if (!op_add(nickname, ip))
    {
        puts("[admin] Failed to persist operator status.");
        return;
    }

    CLI_MUTEX_LOCK(server->state_lock);
    PeerData *peer = server_find_peer(server, (uint16_t)id_value);
    if (peer && strcmp(peer->udid.value, udid) == 0)
    {
        peer->op = true;
        server_send_msg(server, peer->peer, CLRCODE_GRN "you're an operator now");
    }
    CLI_MUTEX_UNLOCK(server->state_lock);

    printf("[admin] Player id %ld is now an operator.\n", id_value);
}

static void cmd_deop(const char *ip)
{
    if (!ip || !*ip)
    {
        puts("usage: deop <ip>");
        return;
    }

    if (!op_revoke(ip))
    {
        printf("[admin] No operator entry found for '%s'.\n", ip);
        return;
    }

    for (int s = 0; s < disaster_count(); s++)
    {
        Server *server = disaster_get(s);
        if (!server)
            continue;

        CLI_MUTEX_LOCK(server->state_lock);
        for (size_t i = 0; i < server->peers.capacity; i++)
        {
            PeerData *peer = (PeerData *)server->peers.ptr[i];
            if (peer && strcmp(peer->ip.value, ip) == 0)
                peer->op = false;
        }
        CLI_MUTEX_UNLOCK(server->state_lock);
    }

    printf("[admin] Removed operator entry for %s.\n", ip);
}

static void cmd_bans(void)
{
    int count = 0;
    CLI_MUTEX_LOCK(g_banMut);
    for (cJSON *item = g_bans ? g_bans->child : NULL; item; item = item->next)
    {
        char key[CLI_DISPLAY_MAX], name[CLI_DISPLAY_MAX];
        safe_text(item->string, key, sizeof(key));
        safe_text(cJSON_GetStringValue(item), name, sizeof(name));
        printf("%-48s  %s\n", key, name);
        count++;
    }
    CLI_MUTEX_UNLOCK(g_banMut);

    if (!count)
        puts("[admin] Ban list is empty.");
}

static void cmd_ops(void)
{
    int count = 0;
    CLI_MUTEX_LOCK(g_opMut);
    for (cJSON *item = g_ops ? g_ops->child : NULL; item; item = item->next)
    {
        char key[CLI_DISPLAY_MAX], name[CLI_DISPLAY_MAX];
        safe_text(item->string, key, sizeof(key));
        safe_text(cJSON_GetStringValue(item), name, sizeof(name));
        printf("%-24s  %s\n", key, name);
        count++;
    }
    CLI_MUTEX_UNLOCK(g_opMut);

    if (!count)
        puts("[admin] Operator list is empty.");
}

static void cmd_timeouts(void)
{
    int count = 0;
    time_t now = time(NULL);

    CLI_MUTEX_LOCK(g_timeoutMut);
    for (cJSON *item = g_timeouts ? g_timeouts->child : NULL; item; item = item->next)
    {
        cJSON *name_item = cJSON_GetArrayItem(item, 0);
        cJSON *time_item = cJSON_GetArrayItem(item, 1);
        const char *name_value = name_item ? cJSON_GetStringValue(name_item) : "";
        uint64_t until = time_item ? (uint64_t)cJSON_GetNumberValue(time_item) : 0;
        long long remaining = until > (uint64_t)now ? (long long)(until - (uint64_t)now) : 0;

        char key[CLI_DISPLAY_MAX], name[CLI_DISPLAY_MAX];
        safe_text(item->string, key, sizeof(key));
        safe_text(name_value, name, sizeof(name));
        printf("%-48s  remaining=%llds  %s\n", key, remaining, name);
        count++;
    }
    CLI_MUTEX_UNLOCK(g_timeoutMut);

    if (!count)
        puts("[admin] Timeout list is empty.");
}

static void cmd_unban(const char *key)
{
    if (!key || !*key)
    {
        puts("usage: unban <ip-or-udid>");
        return;
    }

    if (ban_revoke(key, key))
        printf("[admin] Removed ban key '%s'. Note: bans store IP and UDID separately; remove both keys to fully unban a player.\n", key);
    else
        printf("[admin] Ban key '%s' was not found.\n", key);
}

static void cmd_untimeout(const char *key)
{
    if (!key || !*key)
    {
        puts("usage: untimeout <ip-or-udid>");
        return;
    }

    if (timeout_revoke(key, key))
        printf("[admin] Removed timeout key '%s'.\n", key);
    else
        printf("[admin] Timeout key '%s' was not found.\n", key);
}

static void cmd_ban_key(const char *key, const char *label)
{
    if (!key || !*key)
    {
        puts("usage: ban-key <ip-or-udid> [label]");
        return;
    }

    if (!label || !*label)
        label = "CLI ban";

    bool existed = false;
    bool saved = true;
    CLI_MUTEX_LOCK(g_banMut);
    if (cJSON_HasObjectItem(g_bans, key))
    {
        existed = true;
    }
    else
    {
        cJSON_AddStringToObject(g_bans, key, label);
        saved = collection_save(BANS_FILE, g_bans);
        if (!saved)
            cJSON_DeleteItemFromObject(g_bans, key);
    }
    CLI_MUTEX_UNLOCK(g_banMut);

    if (existed)
        printf("[admin] Ban key '%s' already exists.\n", key);
    else if (saved)
        printf("[admin] Added offline ban key '%s'.\n", key);
    else
        puts("[admin] Failed to save the ban list.");
}

/* A console broadcast is meant for everyone on the machine, so by default it
   goes to every lobby rather than only the one picked with "use". */
static void cmd_say(char *message, bool every_lobby)
{
    message = skip_space(message);
    if (!message || !*message)
    {
        puts("usage: say <message>          (all lobbies)");
        puts("       say-here <message>     (only the selected lobby)");
        return;
    }

    if (!every_lobby)
    {
        Server *server = selected_server();
        if (!server)
            return;

        CLI_MUTEX_LOCK(server->state_lock);
        server_broadcast_msg(server, message);
        CLI_MUTEX_UNLOCK(server->state_lock);
        printf("[admin] Broadcast sent to lobby %d.\n", g_cli_server);
        return;
    }

    int lobbies = 0;
    int players = 0;
    for (int i = 0; i < disaster_count(); i++)
    {
        Server *server = disaster_get(i);
        if (!server)
            continue;

        CLI_MUTEX_LOCK(server->state_lock);
        server_broadcast_msg(server, message);
        players += (int)server->peers.noitems;
        CLI_MUTEX_UNLOCK(server->state_lock);
        lobbies++;
    }

    printf("[admin] Broadcast sent to %d lobbies (%d player(s) online).\n",
        lobbies, players);
}

static void cmd_lobby(void)
{
    Server *server = selected_server();
    if (!server)
        return;

    bool ok = true;
    CLI_MUTEX_LOCK(server->state_lock);
    if (server->state != ST_LOBBY)
        ok = lobby_init(server);
    CLI_MUTEX_UNLOCK(server->state_lock);

    puts(ok ? "[admin] Server moved to lobby." : "[admin] Failed to move server to lobby.");
}

static void cmd_win(const char *side)
{
    Server *server = selected_server();
    if (!server)
        return;

    Ending ending;
    if (side && strcmp(side, "exe") == 0)
        ending = ED_EXEWIN;
    else if (side && (strcmp(side, "surv") == 0 || strcmp(side, "survivor") == 0))
        ending = ED_SURVWIN;
    else
    {
        puts("usage: win <exe|surv>");
        return;
    }

    bool ok = false;
    CLI_MUTEX_LOCK(server->state_lock);
    if (server->state == ST_GAME)
        ok = game_end(server, ending, false);
    CLI_MUTEX_UNLOCK(server->state_lock);

    puts(ok ? "[admin] Forced game result." : "[admin] Server is not in an active game, or game_end failed.");
}

static void cmd_reports(const char *key_filter)
{
    int targets = 0;

    CLI_MUTEX_LOCK(g_reportMut);
    for (cJSON *item = g_reports ? g_reports->child : NULL; item; item = item->next)
    {
        if (key_filter && strcmp(item->string, key_filter) != 0)
            continue;

        char key[CLI_DISPLAY_MAX], name[CLI_DISPLAY_MAX], ip[CLI_DISPLAY_MAX];
        safe_text(item->string, key, sizeof(key));
        safe_text(cJSON_GetStringValue(cJSON_GetObjectItem(item, "nickname")), name, sizeof(name));
        safe_text(cJSON_GetStringValue(cJSON_GetObjectItem(item, "ip")), ip, sizeof(ip));

        cJSON *count = cJSON_GetObjectItem(item, "count");
        cJSON *list = cJSON_GetObjectItem(item, "reports");
        int total = count ? (int)count->valuedouble : 0;

        printf("%-22s  %-40s  %-16s  %d report(s)\n", name, key, ip, total);

        /* Newest last in the array, so walk it in order and show the tail. */
        int size = list ? cJSON_GetArraySize(list) : 0;
        int start = key_filter ? 0 : (size > 3 ? size - 3 : 0);
        for (int i = start; i < size; i++)
        {
            cJSON *record = cJSON_GetArrayItem(list, i);
            if (!record)
                continue;

            char by[CLI_DISPLAY_MAX], reason[CLI_DISPLAY_MAX];
            safe_text(cJSON_GetStringValue(cJSON_GetObjectItem(record, "by")), by, sizeof(by));
            safe_text(cJSON_GetStringValue(cJSON_GetObjectItem(record, "reason")), reason, sizeof(reason));

            cJSON *when = cJSON_GetObjectItem(record, "time");
            cJSON *srv = cJSON_GetObjectItem(record, "server");
            time_t stamp = when ? (time_t)when->valuedouble : 0;

            char timestr[32] = "?";
            struct tm *tm_info = stamp ? localtime(&stamp) : NULL;
            if (tm_info)
                strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M", tm_info);

            printf("      %s  srv %d  by %-18s  %s\n",
                timestr, srv ? (int)srv->valuedouble : 0, by, reason);
        }

        if (!key_filter && size > 3)
            printf("      ... %d older, use 'reports %s' to see all\n", size - 3, key);

        targets++;
    }
    CLI_MUTEX_UNLOCK(g_reportMut);

    if (!targets)
    {
        if (key_filter)
            printf("[admin] No reports stored under '%s'.\n", key_filter);
        else
            puts("[admin] No reports have been filed.");
    }
}

static void cmd_reports_clear(const char *key)
{
    if (!key)
    {
        puts("[admin] Usage: reports-clear <udid-or-ip> | reports-clear all");
        return;
    }

    if (strcmp(key, "all") == 0)
    {
        if (report_clear_all())
            puts("[admin] All reports cleared.");
        else
            puts("[admin] Failed to clear the report list.");
        return;
    }

    if (report_clear(key))
        printf("[admin] Cleared reports for '%s'.\n", key);
    else
        printf("[admin] No reports stored under '%s'.\n", key);
}

static void cmd_lobby_timeouts(void)
{
    int rows = 0;
    time_t now = time(NULL);

    CLI_MUTEX_LOCK(g_lobbyTimeoutMut);
    for (cJSON *item = g_lobbyTimeouts ? g_lobbyTimeouts->child : NULL; item; item = item->next)
    {
        cJSON *lobbies = cJSON_GetObjectItem(item, "lobbies");
        if (!lobbies)
            continue;

        char key[CLI_DISPLAY_MAX], name[CLI_DISPLAY_MAX];
        safe_text(item->string, key, sizeof(key));
        safe_text(cJSON_GetStringValue(cJSON_GetObjectItem(item, "nickname")), name, sizeof(name));

        for (cJSON *slot = lobbies->child; slot; slot = slot->next)
        {
            time_t until = (time_t)slot->valuedouble;
            long remaining = (long)(until - now);
            if (remaining < 0)
                remaining = 0;

            printf("%-22s  %-40s  lobby %-4s  %ld min left\n",
                name, key, slot->string, (remaining + 59) / 60);
            rows++;
        }
    }
    CLI_MUTEX_UNLOCK(g_lobbyTimeoutMut);

    if (!rows)
        puts("[admin] No per-lobby timeouts are active.");
}

static void cmd_lobby_timeouts_clear(const char *key)
{
    if (!key)
    {
        puts("[admin] Usage: lobby-unblock <ip-or-udid> | lobby-unblock all");
        return;
    }

    if (strcmp(key, "all") == 0)
    {
        if (lobby_timeout_clear_all())
            puts("[admin] All per-lobby timeouts cleared.");
        else
            puts("[admin] Failed to clear per-lobby timeouts.");
        return;
    }

    bool any = false;
    for (int i = 0; i < disaster_count(); i++)
        any |= lobby_timeout_revoke(key, key, i);

    if (any)
        printf("[admin] Cleared lobby timeouts for '%s'.\n", key);
    else
        printf("[admin] No lobby timeouts stored under '%s'.\n", key);
}

static void print_help(void)
{
    puts("Admin CLI commands:");
    puts("  servers                         list all server instances");
    puts("  use <server-id>                 select server instance for commands");
    puts("  status                          show selected server status");
    puts("  players                         list connected players with IDs/IP/UDID");
    puts("  ban <player-id>                 permanently ban connected player (IP + UDID)");
    puts("  ban-key <ip-or-udid> [label]    add an offline ban key");
    puts("  bans                            list persisted bans");
    puts("  unban <ip-or-udid>              remove one persisted ban key");
    puts("  kick <player-id> [seconds]      kick; default reconnect timeout is 5 seconds");
    puts("  timeouts                        list temporary reconnect timeouts");
    puts("  untimeout <ip-or-udid>          remove one timeout key");
    puts("  op <player-id>                  grant persistent operator status");
    puts("  ops                             list persistent operators");
    puts("  deop <ip>                       revoke operator status and current session flag");
    puts("  lobby-blocks                    list active per-lobby vote-kick timeouts");
    puts("  lobby-unblock <key|all>         clear per-lobby timeouts for a player");
    puts("  reports [key]                   list player reports (key shows full history)");
    puts("  reports-clear <key|all>         dismiss reports for one player, or all");
    puts("  say <message>                   broadcast to EVERY lobby");
    puts("  say-here <message>              broadcast to the selected lobby only");
    puts("  lobby                           force selected server back to lobby");
    puts("  win <exe|surv>                  force the current game result");
    puts("  help                            show this command list");
    puts("  quit | exit | stop              cleanly stop all server instances");
}

bool admin_cli_stdin_is_tty(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

int admin_cli_run(void)
{
    char line[CLI_LINE_MAX];

    puts("");
    puts("============================================================");
    puts(" DisasterServer Admin CLI");
    puts(" Type 'help' for commands. Player management no longer depends on SDL.");
    puts("============================================================");
    print_server_status(selected_server());

    for (;;)
    {
        printf("admin[%d]> ", g_cli_server);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
        {
            puts("\n[admin] Console input closed; shutting down cleanly.");
            return 0;
        }

        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char *cursor = skip_space(line);
        char *command = next_token(&cursor);
        if (!command || !*command)
            continue;

        for (char *p = command; *p; p++)
            *p = (char)tolower((unsigned char)*p);

        if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0)
        {
            print_help();
        }
        else if (strcmp(command, "servers") == 0)
        {
            cmd_servers();
        }
        else if (strcmp(command, "use") == 0)
        {
            char *arg = next_token(&cursor);
            long value;
            if (!parse_long(arg, 0, disaster_count() - 1, &value))
                puts("usage: use <server-id>");
            else
            {
                g_cli_server = (int)value;
                printf("[admin] Selected server %d.\n", g_cli_server);
                print_server_status(selected_server());
            }
        }
        else if (strcmp(command, "status") == 0)
        {
            print_server_status(selected_server());
        }
        else if (strcmp(command, "players") == 0 || strcmp(command, "list") == 0)
        {
            cmd_players();
        }
        else if (strcmp(command, "ban") == 0)
        {
            cmd_ban_online(next_token(&cursor));
        }
        else if (strcmp(command, "ban-key") == 0 || strcmp(command, "bankey") == 0)
        {
            char *key = next_token(&cursor);
            char *label = next_token(&cursor);
            cmd_ban_key(key, label);
        }
        else if (strcmp(command, "bans") == 0)
        {
            cmd_bans();
        }
        else if (strcmp(command, "unban") == 0)
        {
            cmd_unban(next_token(&cursor));
        }
        else if (strcmp(command, "kick") == 0 || strcmp(command, "timeout") == 0)
        {
            char *id = next_token(&cursor);
            char *seconds = next_token(&cursor);
            cmd_kick(id, seconds);
        }
        else if (strcmp(command, "timeouts") == 0)
        {
            cmd_timeouts();
        }
        else if (strcmp(command, "untimeout") == 0)
        {
            cmd_untimeout(next_token(&cursor));
        }
        else if (strcmp(command, "op") == 0)
        {
            cmd_op_online(next_token(&cursor));
        }
        else if (strcmp(command, "ops") == 0)
        {
            cmd_ops();
        }
        else if (strcmp(command, "deop") == 0)
        {
            cmd_deop(next_token(&cursor));
        }
        else if (strcmp(command, "lobby-blocks") == 0 || strcmp(command, "lobbyblocks") == 0)
        {
            cmd_lobby_timeouts();
        }
        else if (strcmp(command, "lobby-unblock") == 0 || strcmp(command, "lobbyunblock") == 0)
        {
            cmd_lobby_timeouts_clear(next_token(&cursor));
        }
        else if (strcmp(command, "reports") == 0)
        {
            cmd_reports(next_token(&cursor));
        }
        else if (strcmp(command, "reports-clear") == 0 || strcmp(command, "reportsclear") == 0)
        {
            cmd_reports_clear(next_token(&cursor));
        }
        else if (strcmp(command, "say") == 0 || strcmp(command, "broadcast") == 0)
        {
            cmd_say(cursor, true);
        }
        else if (strcmp(command, "say-here") == 0 || strcmp(command, "sayhere") == 0)
        {
            cmd_say(cursor, false);
        }
        else if (strcmp(command, "lobby") == 0)
        {
            cmd_lobby();
        }
        else if (strcmp(command, "win") == 0)
        {
            char *side = next_token(&cursor);
            if (side)
                for (char *p = side; *p; p++)
                    *p = (char)tolower((unsigned char)*p);
            cmd_win(side);
        }
        else if (strcmp(command, "quit") == 0 || strcmp(command, "exit") == 0 || strcmp(command, "stop") == 0)
        {
            puts("[admin] Shutdown requested.");
            return 0;
        }
        else
        {
            printf("[admin] Unknown command '%s'. Type 'help'.\n", command);
        }
    }
}
