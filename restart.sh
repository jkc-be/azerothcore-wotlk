#!/usr/bin/env bash
# Observatory MySQL retune + clean Goldshire restart. Run as your user — no sudo.
set -euo pipefail

OBS="$HOME/.local/share/azeroth-observatory"
NEW_RUN="goldshire-$(date +%Y%m%d-%H%M%S)-$$"
MYSQL_PWD="$(cat "$OBS/secrets/mysql-root")"
export MYSQL_PWD

# Validate before stopping services or restoring the disposable fixture.
python3 - "$OBS" "$NEW_RUN" <<'PY'
import pathlib
import sys

obs = pathlib.Path(sys.argv[1])
config = {}
for name in ('worldserver.conf', 'modules/playerbots.conf'):
    for line in (obs / 'server/etc' / name).read_text().splitlines():
        key, separator, value = line.partition('=')
        if separator and not key.lstrip().startswith('#'):
            config[key.strip()] = value.strip().strip('"')
errors = []
required = {
    'Observatory.Enable': '1',
    'Observatory.DisposableAcknowledgement': 'DISPOSABLE_BOTS_ONLY',
    'AiPlayerbot.Enabled': '1',
    'AiPlayerbot.RandomBotAutologin': '1',
    'AiPlayerbot.RandomBotAccountPrefix': 'obsbot',
    'AiPlayerbot.DisabledWithoutRealPlayer': '0',
    'AiPlayerbot.EnablePeriodicOnlineOffline': '0',
    'AiPlayerbot.CommandServerPort': '0',
    'Console.Enable': '0',
    'Ra.Enable': '0',
    'SOAP.Enabled': '0',
}
for key, expected in required.items():
    if config.get(key) != expected:
        errors.append(f'{key} must be {expected}')
try:
    maximum = int(config.get('AiPlayerbot.MaxRandomBots', '0'))
    minimum = int(config.get('AiPlayerbot.MinRandomBots', '0'))
    target = int(config.get('Observatory.BotCount', '100'))
    if not 0 <= target <= maximum <= 100 or maximum == 0 or minimum != maximum:
        errors.append('Bot pool must have equal Min/MaxRandomBots in 1..100 and contain Observatory.BotCount')
except ValueError:
    errors.append('Bot counts must be integers')
for key, suffix in (('LoginDatabaseInfo', 'auth'), ('WorldDatabaseInfo', 'world'),
                    ('CharacterDatabaseInfo', 'characters'), ('PlayerbotsDatabaseInfo', 'playerbots')):
    parts = config.get(key, '').split(';')
    if len(parts) != 5 or parts[:2] != ['127.0.0.1', '3307'] or parts[-1] != f'obs_goldshire_{suffix}':
        errors.append(f'{key} must use 127.0.0.1:3307 / obs_goldshire_{suffix}')
if not (obs / 'fixtures/goldshire-30-clean.sql.gz').is_file():
    errors.append('Missing clean Goldshire fixture')
if (obs / 'runs' / sys.argv[2]).exists():
    errors.append('Refusing to reuse an existing run directory')
if errors:
    sys.exit('Restart preflight failed:\n  ' + '\n  '.join(errors))
PY

wait_for() {
  local timeout="$1" description="$2"
  shift 2
  local deadline=$((SECONDS + timeout))
  until "$@"; do
    if (( SECONDS >= deadline )); then
      echo "Timed out waiting for $description" >&2
      return 1
    fi
    sleep 1
  done
}

stopped() {
  local result=0
  podman exec obs-build pgrep -x "$1" >/dev/null || result=$?
  [[ "$result" == 1 ]]
}

# --- 1) Pause: world then auth ---
podman exec obs-build bash -lc 'kill -TERM $(pgrep -x worldserver) 2>/dev/null || true'
wait_for 120 'worldserver shutdown' stopped worldserver
podman exec obs-build bash -lc 'kill -TERM $(pgrep -x authserver) 2>/dev/null || true'
wait_for 60 'authserver shutdown' stopped authserver

# --- 2) Recreate MySQL with tuning as mysqld flags ---
# Use host network (not pasta): avoids SELinux pasta_t → fusefs_t denials on Fedora.
# Bind 127.0.0.1:3307 to match worldserver/auth DatabaseInfo.
podman stop obs-mysql
podman rm obs-mysql
MYSQL_ROOT_PASSWORD="$MYSQL_PWD" podman run -d --name obs-mysql \
  --network=host \
  -v obs-mysql-data:/var/lib/mysql \
  -e MYSQL_ROOT_PASSWORD \
  docker.io/library/mysql:8.4 \
  --bind-address=127.0.0.1 \
  --port=3307 \
  --skip-log-bin \
  --innodb-buffer-pool-size=4G \
  --innodb-io-capacity=2000 \
  --innodb-io-capacity-max=8000 \
  --transaction-isolation=READ-COMMITTED \
  --innodb-flush-log-at-trx-commit=2 \
  --innodb-log-buffer-size=32M

mysql_ready() {
  podman exec --env MYSQL_PWD obs-mysql mysql -uroot -NBe 'SELECT 1' >/dev/null 2>&1
}
if ! wait_for 120 'MySQL readiness' mysql_ready; then
  podman logs --tail 30 obs-mysql
  exit 1
fi

podman exec --env MYSQL_PWD obs-mysql mysql -uroot -e \
  "SHOW VARIABLES WHERE Variable_name IN ('log_bin','innodb_buffer_pool_size','innodb_io_capacity','innodb_io_capacity_max','transaction_isolation','innodb_flush_log_at_trx_commit');"

# --- 3) Restore clean Goldshire fixture ---
zcat "$OBS/fixtures/goldshire-30-clean.sql.gz" \
  | podman exec -i --env MYSQL_PWD obs-mysql mysql -uroot

# Keep Darkmoon Faire and its setup phases out of this simulation. State 5 is
# GAMEEVENT_INTERNAL: the scheduler never starts it. Preserve the event/spawn data.
podman exec --env MYSQL_PWD obs-mysql mysql -uroot obs_goldshire_world -e \
  "UPDATE game_event SET world_event = 5
   WHERE eventEntry IN (3, 4, 5, 23, 71, 77) AND description LIKE 'Darkmoon Faire%';"
unset MYSQL_PWD

# --- 4) New run directory (must not already exist) ---
sed -i "s|^Observatory.Directory = .*|Observatory.Directory = \"$OBS/runs/$NEW_RUN\"|" \
  "$OBS/server/etc/worldserver.conf"
mkdir -p "$OBS/logs/$NEW_RUN"

mkdir -p "$HOME/.config/systemd/user"
cat > "$HOME/.config/systemd/user/observatory-bridge.service" <<EOF
[Unit]
Description=Observatory Elwynn legacy bridge

[Service]
Type=simple
WorkingDirectory=/home/janc/projects/azerothcore-wotlk
ExecStart=/home/janc/.pyenv/versions/3.11.15/bin/python3 -u /home/janc/projects/azerothcore-wotlk/apps/observatory/bridge.py --spool $OBS/runs/$NEW_RUN --token-file $OBS/token --maps $OBS/map-art-complete --port 8787
Restart=on-failure
RestartSec=2
UMask=0077

[Install]
WantedBy=default.target
EOF
systemctl --user daemon-reload

# --- 5) Start auth, then world, then bridge ---
podman exec -d obs-build bash -lc \
  "cd $OBS && ./server/bin/authserver -c ./server/etc/authserver.conf >> logs/authserver.log 2>&1"
sleep 2
podman exec -d obs-build bash -lc \
  "cd $OBS && ./server/bin/worldserver -c ./server/etc/worldserver.conf >> logs/$NEW_RUN/worldserver.log 2>&1"
systemctl --user restart observatory-bridge.service

world_ready() {
  if ! podman exec obs-build pgrep -x worldserver >/dev/null; then
    tail -n 40 "$OBS/logs/$NEW_RUN/worldserver.log" >&2
    exit 1
  fi
  [[ -s "$OBS/runs/$NEW_RUN/latest.json" ]]
}
wait_for 300 'worldserver telemetry' world_ready
podman exec obs-build pgrep -a authserver
podman exec obs-build pgrep -a worldserver
systemctl --user status observatory-bridge.service --no-pager
