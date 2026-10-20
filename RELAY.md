# Relay support

## Why

Most people who want to host cannot forward a port: they are behind CGNAT, on
a dynamic address, or on a network they do not control. The usual answer is a
VPN like Radmin, which every player also has to install.

Instead, the server dials **out** to a relay and keeps that one UDP flow open.
Players connect to the relay's public address, and their traffic is carried
over that flow. Nothing is installed on either side: the stock game client
connects to an ordinary address and port.

## The pieces

| Piece | Runs where | In this repo |
|---|---|---|
| Game server | your machine | yes — `disasterserver/Relay.c` is the tunnel client |
| Directory | one central host | no |
| Relay | one per region | no |
| Portal | beside the directory | no |

A host authenticates once with the **directory**, which knows accounts and
regions. It returns the endpoint of the relay for that account's region plus a
short-lived signed ticket. The host then presents that ticket to the **relay**,
which verifies only the signature — so relays hold no account data at all, and
adding a region is just another daemon holding the same signing key.

## Getting an account

Register at **https://relaysc.dr2.site**, create a server, and paste the block
it gives you into `Config.json`. You get a block of consecutive public ports,
one per lobby.

The network is run by DragonCoreSSH and shared with other mods and forks. It
is free to use; servers that misbehave can be reported from the portal and
suspended.

## Wire protocol

Everything is UDP. All integers are little-endian. Every frame starts with an
8-byte header:

```
0  magic0 = 0xBE
1  magic1 = 0x01        protocol version
2  type
3  lobby                which of this account's ports
4  session (uint32)     which player, 0 for control frames
```

### Host to directory

```
DIR_HELLO      0x10   host -> dir    username
DIR_CHALLENGE  0x11   dir  -> host   16-byte nonce
DIR_AUTH       0x12   host -> dir    HMAC-SHA256(relay password, nonce || username)
DIR_OFFER      0x13   dir  -> host   region endpoints + ticket
DIR_FAIL       0x14   dir  -> host   reason byte
```

The password never crosses the wire; only a MAC over a nonce the directory
chose. Reason bytes are 1 unknown account, 2 bad MAC, 3 already in use,
4 suspended — the server turns these into messages that say what to fix.

### Host to relay

```
HELLO          0x01   host -> relay  username
CHALLENGE      0x02   relay -> host  nonce
AUTH           0x03   host -> relay  the ticket from the directory
AUTH_OK        0x04   relay -> host  base_port, port_count
AUTH_FAIL      0x05   relay -> host  reason byte
SESSION_OPEN   0x06   relay -> host  lobby, synthetic address
SESSION_CLOSE  0x07   both ways
DATA           0x08   both ways      one ENet datagram
KEEPALIVE      0x09   host -> relay  holds the NAT mapping open
```

### The ticket

24 bytes of fields, then a 32-byte session key sealed under a key derived from
the signing key, then a 32-byte MAC over both:

```
0   account_id
4   base_port
8   port_count
12  issued_at
16  expires_at
20  region_id
24  sealed session key (32 bytes)
56  HMAC-SHA256 over bytes 0..55
```

A relay checks the MAC and the expiry and nothing else.

### After AUTH

Every frame is encrypted and authenticated with the session key: HMAC-CTR for
confidentiality, encrypt-then-MAC for integrity, with separate keys per
direction and a sliding replay window. A frame carries an 8-byte nonce and a
16-byte tag on top of the header, which is why payloads are capped at 1368
bytes and the server pins its ENet MTU to match when the relay is enabled.

## Synthetic addresses

The address a host sees for a player is

```
HMAC(relay secret || per-host salt, player's real address) mapped into 10/8
```

The relay secret never leaves the relay, and the per-host salt stops two hosts
from correlating the same player. The input is the address the relay observed,
not anything the client sent, so a player cannot shake it off by editing their
UDID — which makes it a better ban key than the game has natively.

The consequence is that a host cannot learn a player's real address, and that
is intended. If you are porting this, keep that property: it is the reason
players are willing to join servers run by strangers.

## Browser players

The web version of the game cannot send UDP. It opens a WebSocket to the same port
number over TCP (`ws://address:port/`) and sends each ENet datagram as one binary
message. Both ends understand this:

- The server (`disasterserver/WebGate.c`) listens on TCP on every lobby port next to
  UDP. Each WebSocket gets its own loopback UDP socket to the lobby, exactly like a
  relay session, and the player's real address is what bans and operators see. It is
  on by default; `"web_clients": false` in the `server` section of Config.json turns it
  off. A host that forwards ports must forward TCP as well as UDP for browser players.
- The relay listens on TCP on each public game port too. A WebSocket is just another
  session: same synthetic address, per-address limit and ENet filter. Nothing changes
  for hosts, so an existing host works for browser players as soon as its relay is
  updated.

There is no TLS: the web game must be served over http, since browsers refuse `ws://`
from an https page.

## Running your own network

Nothing above depends on a secret you do not have. A relay needs a signing key
shared with the directory and a local address secret it generates itself; a
directory needs an account store. Both are small.

If you would rather use an existing network than run one, the DragonCoreSSH
Relay above is open to other mods and forks.
