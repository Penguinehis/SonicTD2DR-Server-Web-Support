/* Authenticated encryption for the relay tunnel.
 *
 * Built only from SHA-256, which the relay already needs, so there is no extra
 * dependency to ship. The construction is deliberately conventional:
 *
 *   keystream : HMAC(k_enc, nonce || counter)      - a PRF in counter mode
 *   tag       : HMAC(k_mac, header || nonce || ciphertext), truncated to 16
 *
 * That is encrypt-then-MAC: the tag covers the ciphertext and the routing
 * header, so neither the payload nor the session id can be altered. Each
 * direction gets its own pair of keys so a frame cannot be reflected back at
 * the sender, and a sliding window rejects replays while still tolerating the
 * reordering that is normal on UDP.
 */
#include <RelayProto.h>
#include <string.h>

void relay_derive_keys(const uint8_t session_key[32], bool host_side, RelayCrypto *out)
{
    static const char *lab_h2r_e = "h2r-enc";
    static const char *lab_h2r_m = "h2r-mac";
    static const char *lab_r2h_e = "r2h-enc";
    static const char *lab_r2h_m = "r2h-mac";

    const char *send_e = host_side ? lab_h2r_e : lab_r2h_e;
    const char *send_m = host_side ? lab_h2r_m : lab_r2h_m;
    const char *recv_e = host_side ? lab_r2h_e : lab_h2r_e;
    const char *recv_m = host_side ? lab_r2h_m : lab_h2r_m;

    memset(out, 0, sizeof(*out));
    hmac_sha256(session_key, 32, send_e, strlen(send_e), out->send_enc);
    hmac_sha256(session_key, 32, send_m, strlen(send_m), out->send_mac);
    hmac_sha256(session_key, 32, recv_e, strlen(recv_e), out->recv_enc);
    hmac_sha256(session_key, 32, recv_m, strlen(recv_m), out->recv_mac);
    out->send_counter = 1;
    out->recv_highest = 0;
    out->recv_window = 0;
    out->ready = true;
}

/* HMAC in counter mode; `nonce` is the 8-byte frame counter. */
void relay_keystream_xor(const uint8_t key[32], const uint8_t nonce[8],
                         uint8_t *data, size_t len)
{
    uint8_t block[32], msg[12];
    uint32_t counter = 0;
    size_t done = 0;

    memcpy(msg, nonce, 8);
    while (done < len)
    {
        relay_put32(msg + 8, counter++);
        hmac_sha256(key, 32, msg, sizeof(msg), block);

        size_t take = len - done;
        if (take > sizeof(block)) take = sizeof(block);
        for (size_t i = 0; i < take; i++) data[done + i] ^= block[i];
        done += take;
    }
}

static void tag_for(const uint8_t key[32], const uint8_t *header, const uint8_t nonce[8],
                    const uint8_t *cipher, size_t len, uint8_t out[RELAY_TAG_SIZE])
{
    uint8_t full[32];

    /* One HMAC over header || nonce || ciphertext. Lengths are fixed or implied
       by the datagram size, so no length-extension ambiguity arises here. */
    uint8_t buf[RELAY_MAX_FRAME + RELAY_HEADER_SIZE + 8];
    size_t n = 0;
    memcpy(buf + n, header, RELAY_HEADER_SIZE); n += RELAY_HEADER_SIZE;
    memcpy(buf + n, nonce, 8);                  n += 8;
    if (len) { memcpy(buf + n, cipher, len);    n += len; }

    hmac_sha256(key, 32, buf, n, full);
    memcpy(out, full, RELAY_TAG_SIZE);
}

int relay_seal(RelayCrypto *c, uint8_t *frame, size_t header_len, size_t payload_len)
{
    if (!c->ready) return -1;
    if (payload_len > RELAY_MAX_PAYLOAD) return -1;

    uint8_t *nonce  = frame + header_len;
    uint8_t *cipher = nonce + 8;

    uint64_t ctr = c->send_counter++;
    for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)(ctr >> (56 - i * 8));

    relay_keystream_xor(c->send_enc, nonce, cipher, payload_len);
    tag_for(c->send_mac, frame, nonce, cipher, payload_len, cipher + payload_len);

    return (int)(header_len + 8 + payload_len + RELAY_TAG_SIZE);
}

/* Returns the plaintext length, or -1 if the frame is forged, replayed or
   truncated. On success the payload is decrypted in place. */
int relay_open(RelayCrypto *c, uint8_t *frame, size_t header_len, size_t frame_len)
{
    if (!c->ready) return -1;
    if (frame_len < header_len + 8 + RELAY_TAG_SIZE) return -1;

    uint8_t *nonce  = frame + header_len;
    uint8_t *cipher = nonce + 8;
    size_t payload_len = frame_len - header_len - 8 - RELAY_TAG_SIZE;

    uint8_t want[RELAY_TAG_SIZE];
    tag_for(c->recv_mac, frame, nonce, cipher, payload_len, want);

    uint8_t diff = 0;
    for (size_t i = 0; i < RELAY_TAG_SIZE; i++)
        diff |= (uint8_t)(want[i] ^ cipher[payload_len + i]);
    if (diff) return -1;

    uint64_t ctr = 0;
    for (int i = 0; i < 8; i++) ctr = (ctr << 8) | nonce[i];
    if (ctr == 0) return -1;

    /* Sliding replay window: accept anything newer than the highest seen, and
       anything inside the window that has not been seen before. */
    if (ctr > c->recv_highest)
    {
        uint64_t shift = ctr - c->recv_highest;
        c->recv_window = shift >= 64 ? 0 : (c->recv_window << shift);
        c->recv_window |= 1ull;
        c->recv_highest = ctr;
    }
    else
    {
        uint64_t back = c->recv_highest - ctr;
        if (back >= 64) return -1;
        uint64_t bit = 1ull << back;
        if (c->recv_window & bit) return -1;
        c->recv_window |= bit;
    }

    relay_keystream_xor(c->recv_enc, nonce, cipher, payload_len);
    return (int)payload_len;
}
