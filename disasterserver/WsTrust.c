/* Trusted WebSocket proxies (see WsTrust.h). */
#include <WsTrust.h>
#include <WsProto.h>
#include <io/Socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix) || defined(__unix__)
  #include <netdb.h>
  #include <pthread.h>
  #include <time.h>
  static void trust_sleep(int sec) { struct timespec ts = { sec, 0 }; nanosleep(&ts, NULL); }
#else
  static void trust_sleep(int sec) { Sleep((DWORD)sec * 1000); }
#endif

#define TRUST_MAX      32
#define TRUST_NAMES    512
#define TRUST_REFRESH  600   /* seconds between lookups of the host names */

typedef struct
{
    int      family;         /* AF_INET / AF_INET6 */
    uint8_t  addr[16];
    int      prefix;         /* bits that must match (32 / 128 for one address) */
} TrustEntry;

typedef struct
{
    TrustEntry e[TRUST_MAX];
    int        n;
} TrustList;

/* Two lists: readers use g_lists[g_active]; the lookup thread fills the other one and then
   switches. A list is only rewritten 10 minutes after it stopped being the active one. */
static TrustList g_lists[2];
static volatile int g_active;
static char g_names[TRUST_NAMES];
static volatile bool g_thread_started;

static bool parse_ip(const char *text, int *family, uint8_t *addr)
{
    struct in_addr v4;
    struct in6_addr v6;
    if (inet_pton(AF_INET, text, &v4) == 1)
    {
        *family = AF_INET;
        memcpy(addr, &v4, 4);
        return true;
    }
    if (inet_pton(AF_INET6, text, &v6) == 1)
    {
        *family = AF_INET6;
        memcpy(addr, &v6, 16);
        return true;
    }
    return false;
}

static void add_entry(TrustList *l, int family, const uint8_t *addr, int prefix)
{
    if (l->n >= TRUST_MAX) return;
    TrustEntry *t = &l->e[l->n++];
    t->family = family;
    memset(t->addr, 0, sizeof(t->addr));
    memcpy(t->addr, addr, family == AF_INET ? 4 : 16);
    t->prefix = prefix;
}

/* one name of the list: an address, a.b.c.d/nn, or a host name to look up */
static void add_name(TrustList *l, const char *name)
{
    char text[128];
    snprintf(text, sizeof(text), "%s", name);
    int prefix = -1;
    char *slash = strchr(text, '/');
    if (slash)
    {
        *slash = 0;
        prefix = atoi(slash + 1);
    }
    int family;
    uint8_t addr[16];
    if (parse_ip(text, &family, addr))
    {
        int full = family == AF_INET ? 32 : 128;
        add_entry(l, family, addr, prefix < 0 || prefix > full ? full : prefix);
        return;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(text, NULL, &hints, &res) != 0) return;
    for (struct addrinfo *a = res; a; a = a->ai_next)
    {
        if (a->ai_family == AF_INET)
            add_entry(l, AF_INET, (const uint8_t *)&((struct sockaddr_in *)a->ai_addr)->sin_addr, 32);
        else if (a->ai_family == AF_INET6)
            add_entry(l, AF_INET6, (const uint8_t *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr, 128);
    }
    freeaddrinfo(res);
}

static void refresh(void)
{
    int next = 1 - g_active;
    TrustList *l = &g_lists[next];
    l->n = 0;
    char names[TRUST_NAMES];
    snprintf(names, sizeof(names), "%s", g_names);
    for (char *tok = strtok(names, ", \t"); tok; tok = strtok(NULL, ", \t"))
        add_name(l, tok);
    g_active = next;
}

#if defined(__unix) || defined(__unix__)
static void *trust_thread(void *unused)
#else
static DWORD WINAPI trust_thread(LPVOID unused)
#endif
{
    (void)unused;
    for (;;)
    {
        trust_sleep(TRUST_REFRESH);
        refresh();
    }
    return 0;
}

void ws_trust_start(const char *names)
{
    snprintf(g_names, sizeof(g_names), "%s", names ? names : "");
    refresh();
    if (g_thread_started) return;
    g_thread_started = true;
#if defined(__unix) || defined(__unix__)
    pthread_t thr;
    if (pthread_create(&thr, NULL, trust_thread, NULL) == 0) pthread_detach(thr);
#else
    HANDLE thr = CreateThread(NULL, 0, trust_thread, NULL, 0, NULL);
    if (thr) CloseHandle(thr);
#endif
}

static bool prefix_match(const uint8_t *a, const uint8_t *b, int bits)
{
    int whole = bits / 8, rest = bits % 8;
    if (memcmp(a, b, (size_t)whole) != 0) return false;
    if (!rest) return true;
    uint8_t mask = (uint8_t)(0xff << (8 - rest));
    return (a[whole] & mask) == (b[whole] & mask);
}

bool ws_trust_is(const char *ip)
{
    int family;
    uint8_t addr[16];
    if (!ip || !parse_ip(ip, &family, addr)) return false;
    /* an IPv4 client seen through an IPv6 socket: ::ffff:a.b.c.d */
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (family == AF_INET6 && !memcmp(addr, mapped, 12))
    {
        family = AF_INET;
        memmove(addr, addr + 12, 4);
    }
    const TrustList *l = &g_lists[g_active];
    for (int i = 0; i < l->n; i++)
        if (l->e[i].family == family && prefix_match(l->e[i].addr, addr, l->e[i].prefix)) return true;
    return false;
}

bool ws_trust_forwarded(const char *request, size_t len, char *out, size_t cap)
{
    char value[96];
    if (!ws_header(request, len, "X-Real-IP", value, sizeof(value))) return false;
    int family;
    uint8_t addr[16];
    if (!parse_ip(value, &family, addr)) return false;
    return inet_ntop(family, addr, out, (socklen_t)cap) != NULL;
}
