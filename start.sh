#!/bin/sh
set -eu
root=/data/u60-screen
[ -f "$root/enabled" ] || exit 0
if ! pidof u60-screen-watch >/dev/null 2>&1; then
    if ! ps | grep -q '[u]60-screen/watch.sh'; then
        nohup sh "$root/watch.sh" >/tmp/u60-screen-watch.log 2>&1 &
    fi
fi
