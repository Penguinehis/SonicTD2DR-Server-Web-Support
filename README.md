# betterserver

A community-maintained server for *Sonic.exe The Disaster 2D Remake*: the PC game, the
browser version (sonicweb.dr2.site) and the phone version (sonicmb.dr2.site) all join it.
It also has relay support, so you can host without forwarding a port, and Lua mods.

Derived from DisasterServer by Team Exe Empire. MIT licensed — see `LICENSE`.

## What you get

- The full game server: lobbies, the map vote, the character select, every map's events,
  results, moderation, an admin console
- **Web players**: the browser and phone versions join over WebSocket on the same port
  numbers as the game (TCP next to UDP), and over WebRTC data channels when the build has
  WebRTC (UDP on each port + 10000), which is faster
- **Mods**: Lua scripts, new maps, characters and game modes, downloaded by the players when
  they join (`Mod.json`; mods are made with the Mod Studio at modsc.dr2.site)
- **Relay support**: your server dials out, players join at a public address, and no router
  configuration is involved
- Memory-safety and performance fixes over the original, multi-line MOTD, player reports,
  vote-kick that works mid-match, per-lobby kick timeouts that survive a restart

Builds for Windows (x64, x86, ARM64, ARM) and Linux (x64, x86, ARM64, ARM).

## Building

Everything happens in Docker, so nothing has to be installed on your machine beyond Docker
itself:

```
./build-docker.sh              # windows-x64 + linux-arm64
./build-docker.sh all          # all eight targets
./build-docker.sh linux-x64    # just one
```

Binaries land in `dist/`. On Windows use `build-docker.bat` with the same arguments. The
first run builds the toolchain image, including the WebRTC libraries (libdatachannel and
mbedTLS, fetched and built by `docker/build_deps.sh`), so it takes a while; later builds are
quick.

Building natively instead is an ordinary CMake project:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

A native build has no WebRTC unless you point `-DWEBRTC_DEPS=<dir>` at libdatachannel and
mbedTLS built as in `docker/build_deps.sh`; web players then use WebSocket only, which works
the same, a little slower.

See `BUILD_INSTRUCTIONS.txt` for more.

## Running

Start it once and it writes a `Config.json` you can edit, then restart. A config written by
an older build is upgraded in place: new settings appear with their defaults.

The server listens on `port` (8606 by default). Forward it as **UDP** for the PC game.

### Web players

In `Config.json`, `server.web_clients` (on by default) lets the browser and phone versions
join:

- forward the same port numbers as **TCP** too (WebSocket), one per lobby;
- and, for the faster WebRTC connection, **UDP on each port + 10000** (8606 → 18606).

The web game is served over plain http on purpose: browsers do not allow `ws://`
connections from an https page.

`server.web_proxies` lists sites that forward phone players (the installed phone app
connects through its site); from those addresses the player's real address is read from the
`X-Real-IP` header. Leave it as it is unless you run such a site yourself.

### Mods

Put a `Mod.json` next to the server:

```json
{ "file": "mods/mymod-1.0.0.dmod", "url": "https://modsc.dr2.site/dl/mymod/1.0.0", "settings": {} }
```

`file` is the mod on this machine, `url` where the players' games download it from (the mod
site gives it), `settings` the mod's own options. Without a `Mod.json` the server plays the
normal game.

### Relay support

Set the `relay` block in `Config.json`:

```json
"relay": {
    "enabled": true,
    "directory_host": "relaysc.dr2.site",
    "directory_port": 9000,
    "username": "your-account",
    "password": "your-password"
}
```

Credentials come from a relay portal — see [RELAY.md](RELAY.md) for how the system works and
how to get an account on the DragonCoreSSH Relay network.

**Player addresses never reach you.** The relay replaces every player's address with a
stable stand-in in `10.0.0.0/8`, derived from a secret only the relay holds. Bans and
operator entries keyed on it keep working, and the same player gets the same address every
time — but you cannot recover their real one.

**Your lobbies map to consecutive public ports.** Lobby 1 is the port the portal shows you,
lobby 2 the next, and so on. The server tells clients which port to move to, so lobby
switching works normally.

## Using your own infrastructure

The relay daemon, the directory and the web portal are not in this repository. You do not
need them to run a server — either join an existing relay network or host the plain way with
forwarded ports.

## Contributing

Issues and pull requests are welcome. If you are adding a protocol message, keep `Packet.h`
ordering stable — the clients index into that enum.
