#!/bin/sh
set -eu
stock=/usr/bin/zte_topsw_devui
root=/data/u60-screen
plain() { exec "$stock"; }
[ -f "$root/enabled" ] && [ ! -f "$root/disable" ] && [ -f "$root/screen.so" ] || plain
sha=$(sha256sum "$stock" | cut -d' ' -f1)
[ "$sha" = e3914e78a8488cb736770f0ac9fb8ce10e0e5222fa50285f08e9e8be90d7f1e9 ] || plain
export LD_PRELOAD="$root/screen.so" U60_SCREEN=1
exec "$stock"
