"""Reset startup failure regression; no services or databases are changed."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("reset_realm", ROOT / "apps/observatory/reset_realm.py")
reset_realm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reset_realm)


class ResetRealm(unittest.TestCase):
    def test_startup_exit_fails_immediately_and_stops_managed_services(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / "run"
            base.mkdir()
            (base / "roster.json").write_text(json.dumps({"initialRaces": {"1": 1},
                                                        "bots": [{"name": "Humana", "race": 1}]}))
            (base / "authserver.conf").write_text("[authserver]\n")
            (root / ".config/systemd/user").mkdir(parents=True)

            def command(args, **kwargs):
                failed = args == ["systemctl", "--user", "is-active", "--quiet", "acore-worldserver"]
                return subprocess.CompletedProcess(args, 3 if failed else 0)

            with patch.object(reset_realm.Path, "home", return_value=root), patch.object(
                reset_realm.shutil, "copy2"
            ), patch.object(reset_realm.subprocess, "run", side_effect=command) as run, patch.object(
                reset_realm.time, "sleep"
            ) as sleep:
                with self.assertRaisesRegex(RuntimeError, "acore-worldserver exited during startup"):
                    reset_realm.deploy(root, base)
                sleep.assert_not_called()
                self.assertEqual(run.call_args.args[0], ["systemctl", "--user", "stop", *reset_realm.SERVICES])


if __name__ == "__main__":
    unittest.main()
