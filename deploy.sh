#!/bin/sh
# Install or update the on-device screen UI over SSH.
#
#   DRY=1 ./deploy.sh     # default: check only, change nothing
#   DRY=0 ./deploy.sh     # copy files and reload the UI
#
# Environment (optional):
#   HOST=192.168.0.1 PORT=2222 USER=root
#
# This script never disables SSH host-key checking, never kills zte_topsw_devui,
# and does not edit /etc/rc.local. After a first install, add the start.sh line
# yourself as described in README.md, then run: sh -n /etc/rc.local
set -eu
cd "$(dirname "$0")"

HOST=${HOST:-192.168.0.1}
PORT=${PORT:-2222}
USER=${USER:-root}
STOCK_SHA=e3914e78a8488cb736770f0ac9fb8ce10e0e5222fa50285f08e9e8be90d7f1e9
RELOAD='{"name":"zte_topsw_devui","instances":{"instance1":{"command":["/usr/bin/zte_topsw_devui"],"respawn":["3600","5","5"]}}}'

SSH="ssh -p $PORT -o BatchMode=yes -o ConnectTimeout=8 $USER@$HOST"
SCP="scp -P $PORT -o BatchMode=yes -o ConnectTimeout=8"

need() { [ -f "$1" ] || { echo "missing $1"; exit 1; }; }
need screen.so
need bg.bin
need run.sh
need watch.sh
need start.sh
need SHA256SUMS

if command -v sha256sum >/dev/null 2>&1; then
    SUM=sha256sum
elif command -v shasum >/dev/null 2>&1; then
    SUM="shasum -a 256"
else
    echo "need sha256sum or shasum"; exit 1
fi

$SUM -c SHA256SUMS

echo "checking $USER@$HOST:$PORT ..."
$SSH 'echo ok; sha256sum /usr/bin/zte_topsw_devui; ls -la /data/u60-screen 2>/dev/null || echo "no /data/u60-screen yet"; pidof zte_topsw_devui || true; grep -n u60-screen/start.sh /etc/rc.local || echo "rc.local has no start.sh line yet"' | tee /tmp/u60-screen-precheck.txt

DEVUI_SHA=$($SSH 'sha256sum /usr/bin/zte_topsw_devui' | awk '{print $1}')
if [ "$DEVUI_SHA" != "$STOCK_SHA" ]; then
    echo "refusing to install: /usr/bin/zte_topsw_devui sha256 is $DEVUI_SHA"
    echo "this build only supports $STOCK_SHA (B31 stock UI)"
    exit 1
fi
echo "stock UI hash ok"

if [ "${DRY:-1}" = 1 ]; then
    echo "dry run ok (set DRY=0 to install or update)"
    exit 0
fi

STAMP=$(date +%Y%m%d%H%M%S)
$SSH 'mkdir -p /data/u60-screen && chmod 755 /data/u60-screen'

# Scripts and wallpaper first (not mapped into a running process the same way).
for f in run.sh watch.sh start.sh bg.bin; do
    $SCP "$f" "$USER@$HOST:/data/u60-screen/$f.new"
    $SSH "cd /data/u60-screen && chmod 755 $f.new && mv $f.new $f"
done
$SSH 'chmod 644 /data/u60-screen/bg.bin; chmod 755 /data/u60-screen/run.sh /data/u60-screen/watch.sh /data/u60-screen/start.sh'

# Never overwrite a live screen.so in place: write a new inode, then mv.
if $SSH 'test -f /data/u60-screen/screen.so'; then
    $SSH "cd /data/u60-screen && cp -p screen.so screen.so.bak.$STAMP && sync"
    echo "backed up previous screen.so to screen.so.bak.$STAMP"
fi
$SCP screen.so "$USER@$HOST:/data/u60-screen/screen.so.new"
NEW_SHA=$($SUM screen.so | awk '{print $1}')
GOT=$($SSH 'sha256sum /data/u60-screen/screen.so.new' | awk '{print $1}')
[ "$GOT" = "$NEW_SHA" ] || { echo "remote sha mismatch: $GOT"; exit 1; }
$SSH 'cd /data/u60-screen && chmod 755 screen.so.new && mv screen.so.new screen.so && touch enabled && rm -f disable && sync && sha256sum screen.so'

# First install: start the watcher before reloading, otherwise the UI comes
# back as the stock binary and stays there.
$SSH 'if ! ps | grep -q "[u]60-screen/watch.sh"; then nohup sh /data/u60-screen/watch.sh >/tmp/u60-screen-watch.log 2>&1 & fi; sleep 1; ps | grep "[u]60-screen/watch.sh" || { echo "watch.sh failed to start"; exit 1; }'

# Reload through procd. Do not kill zte_topsw_devui. The stock command comes
# up first; watch.sh puts run.sh back within about 3 seconds, then the UI
# restarts once more with LD_PRELOAD.
$SSH "ubus call service set '$RELOAD'"

echo "waiting for screen.so to be mapped..."
ok=0
i=0
while [ "$i" -lt 12 ]; do
    i=$((i + 1))
    sleep 5
    if $SSH 'p=$(pidof zte_topsw_devui) && grep -q screen.so /proc/$p/maps'; then
        ok=1
        break
    fi
    echo "  not yet ($i/12)"
done
if [ "$ok" != 1 ]; then
    echo "screen.so was not mapped. The stock UI may still be running."
    echo "Check /tmp/u60-screen-watch.log on the device, then see README.md for rollback."
    exit 1
fi

$SSH 'p=$(pidof zte_topsw_devui); echo "pid=$p"; grep screen.so /proc/$p/maps | head -1; ls -li /data/u60-screen/screen.so'
echo "install ok"
if ! grep -q 'u60-screen/start.sh' /tmp/u60-screen-precheck.txt; then
    echo
    echo "This will not persist across reboot until /etc/rc.local starts start.sh."
    echo "Add this line near the end of /etc/rc.local, before exit 0:"
    echo "  /bin/sh /data/u60-screen/start.sh"
    echo "Then run:  sh -n /etc/rc.local"
fi
