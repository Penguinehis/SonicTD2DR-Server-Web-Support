# Third-party components

## ENet — `enet/`

Copyright (c) 2002-2024 Lee Salzman. MIT licensed; the full text is in
`enet/LICENSE`.

## cJSON — `disasterserver/cJSON.c`, `include/cJSON.h`

Copyright (c) 2009-2017 Dave Gamble and cJSON contributors. MIT licensed; the
notice is at the top of both files.

## Lua 5.4 — `lua/`

Copyright (C) 1994-2024 Lua.org, PUC-Rio. MIT licensed; the notice is at the end of
`lua/lua.h` (the mods run in it).

## libdatachannel and mbedTLS (not in this repository)

`docker/build_deps.sh` downloads and builds them while the toolchain image is made, for the
WebRTC connection of web players: libdatachannel (Paul-Louis Ageneau, MPL 2.0) with libjuice
and usrsctp, and mbedTLS (Apache 2.0). Builds without them use WebSocket only.

## Everything else

The server itself, including the relay client, is MIT licensed — see
`LICENSE`. It derives from DisasterServer by Team Exe Empire, whose copyright
is preserved there.

SHA-256 and HMAC-SHA256 in `disasterserver/Sha256.c` are an original
implementation of FIPS 180-4 and RFC 2104, not vendored from anywhere.
