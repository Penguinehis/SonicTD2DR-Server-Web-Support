/* Hostname lookup for the relay client.
 *
 * The Linux builds are statically linked, and static glibc cannot use
 * getaddrinfo without dragging in matching NSS shared objects at run time -
 * exactly the dependency that static linking exists to avoid. So on unix this
 * speaks DNS directly over UDP, which needs nothing but a socket.
 *
 * Windows has no such problem: winsock resolves names itself, so there we just
 * call getaddrinfo.
 */
#include <Relay.h>
#include <Log.h>
#include <io/Socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix) || defined(__unix__)

#define DNS_PORT      53
#define DNS_TIMEOUT   3000
#define DNS_MAX_MSG   512

/* Writes "www.example.com" as 3www7example3com0. */
static size_t dns_encode_name(uint8_t *out, size_t cap, const char *host)
{
    size_t w = 0;
    const char *label = host;

    for (;;)
    {
        const char *dot = strchr(label, '.');
        size_t len = dot ? (size_t)(dot - label) : strlen(label);
        if (len == 0 || len > 63) return 0;
        if (w + len + 1 >= cap) return 0;

        out[w++] = (uint8_t)len;
        memcpy(out + w, label, len);
        w += len;

        if (!dot) break;
        label = dot + 1;
    }

    if (w + 1 >= cap) return 0;
    out[w++] = 0;
    return w;
}

/* Steps over a name, following one level of compression pointer. */
static bool dns_skip_name(const uint8_t *msg, size_t len, size_t *pos)
{
    while (*pos < len)
    {
        uint8_t n = msg[*pos];
        if (n == 0) { (*pos)++; return true; }
        if ((n & 0xC0) == 0xC0)
        {
            if (*pos + 1 >= len) return false;
            *pos += 2;                 /* a pointer always ends the name */
            return true;
        }
        if (n > 63) return false;
        *pos += 1 + n;
    }
    return false;
}

static bool nameserver_from_resolv(struct sockaddr_in *out)
{
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return false;

    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "nameserver", 10) != 0) continue;

        char *p = line + 10;
        while (*p == ' ' || *p == '\t') p++;

        char *end = p;
        while (*end && *end != '\n' && *end != '\r' && *end != ' ' && *end != '%') end++;
        *end = 0;

        in_addr_t a = inet_addr(p);
        if (a != INADDR_NONE)
        {
            memset(out, 0, sizeof(*out));
            out->sin_family = AF_INET;
            out->sin_port = htons(DNS_PORT);
            out->sin_addr.s_addr = a;
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static bool dns_query(const struct sockaddr_in *server, const char *host, uint32_t *out_ip_be)
{
    uint8_t msg[DNS_MAX_MSG];
    size_t w = 0;

    uint16_t id = (uint16_t)(rand() & 0xffff);
    msg[w++] = (uint8_t)(id >> 8); msg[w++] = (uint8_t)id;
    msg[w++] = 0x01; msg[w++] = 0x00;      /* standard query, recursion desired */
    msg[w++] = 0x00; msg[w++] = 0x01;      /* one question                      */
    msg[w++] = 0x00; msg[w++] = 0x00;
    msg[w++] = 0x00; msg[w++] = 0x00;
    msg[w++] = 0x00; msg[w++] = 0x00;

    size_t n = dns_encode_name(msg + w, sizeof(msg) - w, host);
    if (!n) return false;
    w += n;

    msg[w++] = 0x00; msg[w++] = 0x01;      /* A     */
    msg[w++] = 0x00; msg[w++] = 0x01;      /* IN    */

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;

    if (sendto(s, msg, w, 0, (const struct sockaddr *)server, sizeof(*server)) < 0)
    { close(s); return false; }

    struct pollfd p;
    p.fd = s;
    p.events = POLLIN;
    if (Poll(&p, 1, DNS_TIMEOUT) <= 0) { close(s); return false; }

    uint8_t rsp[DNS_MAX_MSG];
    int rn = (int)recv(s, rsp, sizeof(rsp), 0);
    close(s);
    if (rn < 12) return false;

    if (((uint16_t)rsp[0] << 8 | rsp[1]) != id) return false;
    if ((rsp[3] & 0x0f) != 0) return false;                 /* rcode */

    int qd = (rsp[4] << 8) | rsp[5];
    int an = (rsp[6] << 8) | rsp[7];
    if (an < 1) return false;

    size_t pos = 12;
    for (int i = 0; i < qd; i++)
    {
        if (!dns_skip_name(rsp, (size_t)rn, &pos)) return false;
        pos += 4;
    }

    for (int i = 0; i < an; i++)
    {
        if (!dns_skip_name(rsp, (size_t)rn, &pos)) return false;
        if (pos + 10 > (size_t)rn) return false;

        int type = (rsp[pos] << 8) | rsp[pos + 1];
        int rdlen = (rsp[pos + 8] << 8) | rsp[pos + 9];
        pos += 10;
        if (pos + (size_t)rdlen > (size_t)rn) return false;

        if (type == 1 && rdlen == 4)                        /* A record */
        {
            memcpy(out_ip_be, rsp + pos, 4);
            return true;
        }
        pos += (size_t)rdlen;                               /* CNAME etc. */
    }
    return false;
}
#endif /* unix */

bool relay_resolve(const char *host, uint32_t *out_ip_be)
{
    if (!host || !*host || !out_ip_be) return false;

    /* A literal address needs no lookup at all. inet_addr returns in_addr_t on
       unix and unsigned long on winsock, so keep it to a fixed-width type. */
    uint32_t literal = (uint32_t)inet_addr(host);
    if (literal != (uint32_t)INADDR_NONE)
    {
        *out_ip_be = literal;
        return true;
    }

#if defined(__unix) || defined(__unix__)
    struct sockaddr_in ns;
    if (nameserver_from_resolv(&ns))
    {
        if (dns_query(&ns, host, out_ip_be)) return true;
        Debug("Relay: the system resolver did not answer for %s, trying public ones.", host);
    }

    /* Last resort so a host with an unusable resolv.conf still comes up. */
    static const char *fallback[] = { "1.1.1.1", "8.8.8.8" };
    for (size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]); i++)
    {
        memset(&ns, 0, sizeof(ns));
        ns.sin_family = AF_INET;
        ns.sin_port = htons(DNS_PORT);
        ns.sin_addr.s_addr = inet_addr(fallback[i]);
        if (dns_query(&ns, host, out_ip_be)) return true;
    }
    return false;
#else
    /* winsock resolves names without any of the static-linking trouble */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;

    struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;
    memcpy(out_ip_be, &a->sin_addr, 4);
    freeaddrinfo(res);
    return true;
#endif
}
