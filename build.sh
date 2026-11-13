#!/bin/sh
# Cross-build screen.so for the U60 (aarch64, musl). Uses `zig cc` (pip install ziglang).
set -eu
cd "$(dirname "$0")"
ZIG="${ZIG:-python3 -m ziglang}"
$ZIG cc -target aarch64-linux-musl -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
    -fno-stack-protector -Wl,-soname,screen.so -o screen.so screen.c -lpthread
