#!/bin/sh
# Host tests: test_logic (proxy logic, 104 checks) and test_ui (new pages, recording fake LVGL),
# both against mock_api.py. No device access.
set -eu
cd "$(dirname "$0")"
M=${M:-19097}; A=${A:-19090}; D=${D:-19053}
PW=$(mktemp); cat > "$PW" <<'PWEOF'
#!/bin/sh
export ZTE_AGENT_PASSWORD='pw\back&<x>'
PWEOF
build() { cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-format-truncation -fsanitize=address,undefined \
    -DU60_API_HOST='"127.0.0.1"' "$@" -lpthread; }
build -o test_logic test_logic.c
build -DU60_FAKE_LVGL -o test_ui test_ui.c
mkdir -p preview
python3 mock_api.py $M $A $D > mock.log 2>&1 & MP=$!
trap 'kill $MP 2>/dev/null; rm -f "$PW"' EXIT
for i in $(seq 50); do grep -q ready mock.log && break; sleep 0.1; done
./test_logic $M $A "$PW"
./test_ui $M $A "$PW" $D
