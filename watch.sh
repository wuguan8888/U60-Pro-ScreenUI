#!/bin/sh
set -eu
trap '' HUP
root=/data/u60-screen
plain='{"name":"zte_topsw_devui","instances":{"instance1":{"command":["/usr/bin/zte_topsw_devui"],"respawn":["3600","5","5"]}}}'
extended='{"name":"zte_topsw_devui","instances":{"instance1":{"command":["/bin/sh","/data/u60-screen/run.sh"],"respawn":["3600","5","5"]}}}'
set_ui() { ubus call service set "$1" >/dev/null; }
while [ -f "$root/enabled" ] && [ ! -f "$root/disable" ]; do
    cfg=$(ubus call service list '{"name":"zte_topsw_devui"}' 2>/dev/null || true)
    printf '%s' "$cfg" | grep -q '/data/u60-screen/run.sh' || set_ui "$extended" || true
    sleep 3
done
set_ui "$plain" || true
rm -f "$root/enabled" "$root/disable"
exit 0
