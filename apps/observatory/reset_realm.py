#!/usr/bin/env python3
"""Create a fresh disposable native Alles realm; ordinary acore_* databases are read-only sources."""

import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import time

SERVICES = ("acore-alles-interpreter", "acore-observatory-bridge", "acore-worldserver", "acore-authserver")


def prepare(root, worker_config, initial_races=None):
    initial_races = {1: 4, 8: 2} if initial_races is None else initial_races
    capacities = {race: 20 if race == 1 else 5 for race in (1, 2, 3, 4, 5, 6, 7, 8, 10, 11)}
    if not initial_races or any(race not in capacities or not 0 <= count <= capacities[race]
        for race, count in initial_races.items()) or not sum(initial_races.values()):
        raise ValueError("Initial race counts must fit the prepared roster and include at least one bot")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d%H%M%S%f")
    base = root / "var/run" / ("alles-accelerated-" + stamp)
    base.mkdir(mode=0o700)
    prefix = "obs_alles_" + stamp
    mysql = [
        "mysql",
        "--defaults-extra-file=" + str(root / "var/setup/mysql-client.cnf"),
        "--batch",
        "--skip-column-names",
        "--raw",
    ]

    def query(sql):
        r = subprocess.run(mysql, input=sql, text=True, capture_output=True, check=True)
        return r.stdout

    print(f"Preparing clean realm at {base}", flush=True)
    for kind in ("auth", "world", "characters", "playerbots"):
        print(f"Copying clean acore_{kind} fixture...", flush=True)
        db = prefix + "_" + kind
        query(f"CREATE DATABASE `{db}` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci")
        backup = base / (kind + ".fixture.sql")
        with backup.open("wb") as out:
            os.chmod(backup, 0o600)
            subprocess.run(
                [
                    "mysqldump",
                    "--defaults-extra-file=" + str(root / "var/setup/mysql-client.cnf"),
                    "--single-transaction",
                    "--skip-lock-tables",
                    "--no-tablespaces",
                    "--set-gtid-purged=OFF",
                    "acore_" + kind,
                ],
                stdout=out,
                check=True,
            )
        with backup.open("rb") as src:
            subprocess.run(mysql + [db], stdin=src, stdout=subprocess.DEVNULL, check=True)
        print("Prepared fresh database", db, flush=True)
    rows = query(f"""SELECT c.guid,c.account,c.name,c.race,c.class FROM `{prefix}_characters`.characters c
    JOIN `{prefix}_auth`.account a ON a.id=c.account
    WHERE c.level=1 AND c.totaltime=0 AND a.username REGEXP '^RNDBOT[0-9]+$' ORDER BY c.guid""")
    pool = [
        dict(
            zip(
                ("guid", "account", "name", "race", "class"),
                [int(v) if i != 2 else v for i, v in enumerate(row.split("\t"))],
            )
        )
        for row in rows.splitlines()
    ]
    rng = random.SystemRandom()
    roster = []
    accounts = set()
    races = [1, 2, 3, 4, 5, 6, 7, 8, 10, 11]
    for race in races:
        for n in range(20 if race == 1 else 5):
            candidates = [r for r in pool if r["race"] == race and r["account"] not in accounts]
            if not candidates:
                raise RuntimeError(f"Clean fixture lacks enough unused accounts for race {race}")
            classes = sorted({r["class"] for r in candidates})
            klass = rng.choice(classes)
            bot = rng.choice([r for r in candidates if r["class"] == klass])
            roster.append(bot)
            accounts.add(bot["account"])
    guids = ",".join(str(r["guid"]) for r in roster)
    query(f"UPDATE `{prefix}_characters`.characters SET online=0; UPDATE `{prefix}_auth`.account SET online=0;")
    query(
        f"UPDATE `{prefix}_auth`.realmlist SET address='127.0.0.1',localAddress='127.0.0.1',port=8085,flag=0 WHERE id=1"
    )
    events = ",".join(
        f"(0,{r['guid']},UNIX_TIMESTAMP(),0,'{event}',1)"
        for r in roster
        for event in ("randomize", "teleport", "level")
    )
    # Fresh level-one, never-played pool only. Existing source characters and all four source DBs stay untouched.
    query(f"""INSERT INTO `{prefix}_playerbots`.playerbots_account_type(account_id,account_type,assignment_date)
    VALUES {','.join(f'({a},1,NOW())' for a in accounts)} ON DUPLICATE KEY UPDATE account_type=1,assignment_date=NOW();
    DELETE FROM `{prefix}_playerbots`.playerbots_random_bots WHERE owner=0 AND bot IN ({guids})
     AND event IN ('logout','add','randomize','teleport','level');
    INSERT INTO `{prefix}_playerbots`.playerbots_random_bots(owner,bot,time,validIn,event,value)
    VALUES {events};""")
    for name in query(f"SHOW TABLES FROM `{prefix}_characters` LIKE 'alles\\_%'").splitlines():
        query(f"DELETE FROM `{prefix}_characters`.`{name}`")
    # Predictable names apply only to this new, offline simulation copy.
    prefixes = {
        1: "Human",
        2: "Orc",
        3: "Dwarf",
        4: "Nightelf",
        5: "Undead",
        6: "Tauren",
        7: "Gnome",
        8: "Troll",
        10: "Bloodelf",
        11: "Draenei",
    }

    def letters(number):
        result = ""
        while True:
            result = chr(97 + number % 26) + result
            number = number // 26 - 1
            if number < 0:
                return result

    counts = {}
    for r in roster:
        index = counts.get(r["race"], 0)
        counts[r["race"]] = index + 1
        desired = prefixes[r["race"]] + letters(index)
        collisions = query(
            f"SELECT c.guid,a.username FROM `{prefix}_characters`.characters c "
            f"JOIN `{prefix}_auth`.account a ON a.id=c.account "
            f"WHERE c.name='{desired}' AND c.guid!={r['guid']}"
        )
        for line in collisions.splitlines():
            guid, account = line.split("\t")
            if not re.fullmatch(r"RNDBOT[0-9]+", account):
                raise RuntimeError("Requested name belongs to a non-bot in the copy")
            query(
                f"UPDATE `{prefix}_characters`.characters SET name='Archived{letters(int(guid))}' "
                f"WHERE guid={int(guid)}"
            )
        query(f"UPDATE `{prefix}_characters`.characters SET name='{desired}' WHERE guid={r['guid']}")
        r["name"] = desired
    for d in ("logs", "alles"):
        (base / d).mkdir(mode=0o700)
    settings = {
        "BindIP": "127.0.0.1",
        "WorldServerPort": "8085",
        "RealmServerPort": "3724",
        "RealmID": "1",
        "LogsDir": str(base / "logs"),
        "PidFile": str(base / "world.pid"),
        "Updates.EnableDatabases": "0",
        "Ra.Enable": "0",
        "SOAP.Enabled": "0",
        "Console.Enable": "0",
        "Observatory.Enable": "1",
        "Observatory.DisposableAcknowledgement": "DISPOSABLE_BOTS_ONLY",
        "Observatory.Directory": str(base / "simulation"),
        "Observatory.BotCount": str(sum(initial_races.values())),
        "Observatory.BotGuids": guids,
        "Observatory.Trace": "0",
        "Observatory.RaceRoster": ",".join(f"{r['guid']}:{r['race']}" for r in roster),
        "Observatory.RaceCounts": " ".join(str(initial_races.get(race, 0)) for race in races),
        "Observatory.LlmQueueLimit": "8",
        "Observatory.AllowGmObservers": "1",
        "Observatory.ObserverMode": "1",
        "Observatory.JournalSegmentBytes": "67108864",
        "Alles.Enable": "1",
        "Alles.Brain.Enable": "1",
        "Alles.Objectives.Enable": "1",
        "Alles.Owners": ",".join(f"player:{r['guid']}" for r in roster if r["race"] != 8),
        "Alles.Worker.Profile": subprocess.check_output(
            [str(root / "env/dist/bin/alles-interpreter"), "--config", str(worker_config), "--fingerprint"], text=True
        ).strip(),
        "Alles.Worker.Mode": "bridge",
        "Alles.Speech.Enable": "1",
        "Alles.Conversation.Enable": "1",
        "Alles.Interpreter.BudgetMode": "rolling",
        "Alles.Interpreter.RequestsPerMinute": "120",
        "Alles.Interpreter.PolicyFile": str(base / "interpreter-policy.json"),
        "Alles.Interpreter.TrialLedger": str(base / "worker-ledger.ndjson"),
        "Alles.Telemetry.Directory": str(base / "alles"),
        "AiPlayerbot.MinRandomBots": "65",
        "AiPlayerbot.MaxRandomBots": "65",
        "AiPlayerbot.CommandServerPort": "0",
        "AiPlayerbot.EnablePeriodicOnlineOffline": "0",
        "AiPlayerbot.DisabledWithoutRealPlayer": "0",
        "AiPlayerbot.RandomBotAutologin": "1",
        "AiPlayerbot.Enabled": "1",
        "AiPlayerbot.BotActiveAlone": "100",
        "AiPlayerbot.botActiveAloneSmartScale": "0",
        "AiPlayerbot.AutoDestroyJunk": "1",
        "AiPlayerbot.OriginalTravel": "0",
    }

    def conf(path):
        result = {}
        for line in path.read_text().splitlines():
            m = re.match(r"^\s*([\w.]+)\s*=\s*(.*?)\s*$", line)
            if m:
                result[m[1]] = m[2].split(" #", 1)[0].strip().strip('"')
        return result

    world = conf(root / "env/dist/etc/worldserver.conf")
    module = conf(root / "env/dist/etc/modules/playerbots.conf")
    for key, kind in [
        ("LoginDatabaseInfo", "auth"),
        ("WorldDatabaseInfo", "world"),
        ("CharacterDatabaseInfo", "characters"),
        ("PlayerbotsDatabaseInfo", "playerbots"),
    ]:
        parts = (world | module)[key].split(";")
        parts[-1] = prefix + "_" + kind
        settings[key] = ";".join(parts)
    overlay = base / "settings.conf"
    overlay.write_text("[worldserver]\n" + "\n".join(f'{k} = "{v}"' for k, v in settings.items()) + "\n")
    os.chmod(overlay, 0o600)
    for kind in ("worldserver", "authserver"):
        target = base / (kind + ".conf")
        subprocess.run(
            [
                sys.executable,
                str(root / "apps/observatory/render_config.py"),
                "render",
                "--template",
                str(root / f"env/dist/etc/{kind}.conf"),
                "--overlay",
                str(overlay),
                "--output",
                str(target),
            ],
            check=True,
        )
        os.chmod(target, 0o600)

    def envkey(key):
        key = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", key)
        key = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", key)
        return "AC_" + re.sub(r"[. -]", "_", key).upper()

    (base / "environment").write_text("\n".join(f"{envkey(k)}={json.dumps(v)}" for k, v in settings.items()) + "\n")
    os.chmod(base / "environment", 0o600)
    (base / "roster.json").write_text(
        json.dumps({"prefix": prefix, "bots": roster, "initialRaces": initial_races}, indent=2) + "\n"
    )
    for path in (
        root / "env/dist/etc/worldserver.conf",
        root / "env/dist/etc/authserver.conf",
        root / "env/dist/etc/modules/alles.conf",
        root / "env/dist/etc/modules/playerbots.conf",
    ):
        if path.exists():
            shutil.copy2(path, base / ("rollback-" + path.name))
    clean = query(f"SELECT name,level,totaltime FROM `{prefix}_characters`.characters WHERE guid IN ({guids})")
    if any(row.split("\t")[1:] != ["1", "0"] for row in clean.splitlines()):
        raise RuntimeError("Selected pool is not a clean level-one fixture")
    (base / "initial-characters.tsv").write_text(clean)
    print("Prepared run", base, flush=True)
    return base


def deploy(root, base):
    roster = json.loads((base / "roster.json").read_text())
    initial_races = {int(race): count for race, count in roster["initialRaces"].items()}
    expected_names = set()
    for race, count in initial_races.items():
        expected_names.update(bot["name"] for bot in [b for b in roster["bots"] if b["race"] == race][:count])
    units = Path.home() / ".config/systemd/user"
    backup = base / "unit-backup"
    backup.mkdir(mode=0o700, exist_ok=True)
    for service in ("acore-worldserver", "acore-authserver", "acore-observatory-bridge", "acore-alles-interpreter"):
        p = units / (service + ".service")
        shutil.copy2(p, backup / p.name)
        drop = units / (service + ".service.d")
        if drop.exists():
            shutil.copytree(drop, backup / drop.name, dirs_exist_ok=True)
    print("Stopping previous realm gracefully...", flush=True)
    subprocess.run(["systemctl", "--user", "stop", *SERVICES], check=True)
    # Separate rendered configs and process overrides preserve the ordinary-realm configuration.
    auth = base / "authserver.conf"
    s = auth.read_text().replace(str(base / "world.pid"), str(base / "auth.pid"))
    auth.write_text(s)
    world_override = f"""[Service]
    ExecStart=
    ExecStart={root}/env/dist/bin/worldserver -c {base}/worldserver.conf
    EnvironmentFile=
    EnvironmentFile={base}/environment
    Restart=no
    """
    auth_override = f"""[Service]
    ExecStart=
    ExecStart={root}/env/dist/bin/authserver -c {base}/authserver.conf
    Restart=no
    """
    bridge_command = (
        f"/usr/bin/python3 {root}/apps/observatory/bridge.py --spool {base}/simulation "
        f"--token-file {Path.home()}/.local/share/azeroth-observatory/token --port 8778 "
        f"--maps {Path.home()}/.local/share/azeroth-observatory/map-art --world-conf {base}/worldserver.conf "
        f"--worker-log {root}/env/dist/logs/alles-interpreter.log"
    )
    bridge_override = f"""[Service]
    ExecStart=
    ExecStart={bridge_command}
    """
    for service, text in [
        ("acore-worldserver", world_override),
        ("acore-authserver", auth_override),
        ("acore-observatory-bridge", bridge_override),
    ]:
        drop = units / (service + ".service.d")
        drop.mkdir(exist_ok=True)
        (drop / "zz-accelerated.conf").write_text("\n".join(line.strip() for line in text.splitlines()) + "\n")
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    try:
        subprocess.run(["systemctl", "--user", "start", *SERVICES], check=True)
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            try:
                snapshot = json.loads((base / "simulation/latest.json").read_text())
                bots = snapshot.get("bots", [])
                names = {bot.get("name") for bot in bots}
                if snapshot.get("fault"):
                    raise RuntimeError(f"New realm reported fault: {snapshot['fault']}")
                if (
                    snapshot.get("ready")
                    and len(bots) == len(expected_names)
                    and names == expected_names
                    and sum(bool(bot.get("controlGroup")) for bot in bots) == initial_races.get(8, 0)
                    and snapshot.get("requestedSpeed") == 1
                    and not snapshot.get("llmGuard")
                    and not snapshot.get("paused")
                ):
                    for service in SERVICES:
                        subprocess.run(["systemctl", "--user", "is-active", "--quiet", service], check=True)
                    (base / "reset-proof.json").write_text(json.dumps(snapshot, indent=2) + "\n")
                    print(f"Ready at 1x: {', '.join(sorted(names))}. Dashboard: http://localhost:8778/", flush=True)
                    return
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            time.sleep(2)
        raise RuntimeError("Fresh realm did not reach its requested roster at 1x within 180 seconds")
    except BaseException:
        subprocess.run(["systemctl", "--user", "stop", *SERVICES], check=False)
        raise


def reset(root, initial_races=None):
    """Prepare first, then switch services. Never resume or delete an earlier run."""
    os.umask(0o077)
    (root / "var/run").mkdir(parents=True, exist_ok=True)
    with (root / "var/run/server-reset.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Another realm reset is already running") from None
        if subprocess.run(["systemctl", "is-active", "--quiet", "mysql"]).returncode:
            raise RuntimeError("MySQL must be running before reset")
        if subprocess.run(["systemctl", "--user", "is-active", "--quiet", "acore-observatory"]).returncode == 0:
            raise RuntimeError("Stop the separate Observatory service before resetting the native realm")
        for name in ("mysql", "mysqldump", "systemctl"):
            if not shutil.which(name):
                raise RuntimeError(f"Missing required command: {name}")
        etc = root / "env/dist/etc"
        module = (etc / "modules/alles.conf").read_text()
        token = re.search(r'^\s*Alles.Worker.TokenFile\s*=\s*"([^"\n]+)"', module, re.M)
        if not token:
            raise RuntimeError("Alles.Worker.TokenFile must identify the configured bridge worker")
        worker_config = Path(token[1]).parent / "worker.json"
        for path in (
            worker_config,
            root / "var/setup/mysql-client.cnf",
            root / "env/dist/bin/worldserver",
            root / "env/dist/bin/authserver",
            root / "env/dist/bin/alles-interpreter",
        ):
            if not path.is_file():
                raise RuntimeError(f"Missing native setup file: {path}")
        # Validate the model configuration before copying any databases.
        subprocess.run(
            [str(root / "env/dist/bin/alles-interpreter"), "--config", str(worker_config), "--fingerprint"],
            stdout=subprocess.DEVNULL,
            check=True,
        )
        base = prepare(root, worker_config, initial_races)
        deploy(root, base)
        pointer = root / "var/run/server-realm.json"
        temporary = pointer.with_suffix(".tmp")
        temporary.write_text(json.dumps({"directory": str(base)}, indent=2) + "\n")
        os.replace(temporary, pointer)


if __name__ == "__main__":
    try:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("--humans", type=int, default=4)
        parser.add_argument("--orcs", type=int, default=0)
        parser.add_argument("--trolls", type=int, default=2, help="Unmanaged Playerbots control group")
        args = parser.parse_args()
        reset(Path(__file__).resolve().parents[2], {1: args.humans, 2: args.orcs, 8: args.trolls})
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"reset: {error}", file=sys.stderr)
        sys.exit(1)
