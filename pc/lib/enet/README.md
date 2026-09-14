# ENet 1.3.18 (vendored)

Reliable UDP networking library by Lee Salzman, MIT licensed (see LICENSE).
Source: https://github.com/lsalzman/enet at tag v1.3.18, unmodified.

Used by both the game client (`pc/src/pc_net.c`) and the town server
(`server/`). Only the library sources and headers are vendored; the
autotools, CMake, docs, and Visual Studio files from upstream are omitted.
