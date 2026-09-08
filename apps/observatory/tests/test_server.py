"""Native CLI regression checks; no services or databases are changed."""

import contextlib
import importlib.machinery
import importlib.util
import io
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
loader = importlib.machinery.SourceFileLoader("native_server", str(ROOT / "server"))
spec = importlib.util.spec_from_loader(loader.name, loader)
server = importlib.util.module_from_spec(spec)
loader.exec_module(server)


class NativeServer(unittest.TestCase):
    def test_effective_world_config_overrides_ordinary_config(self):
        result = subprocess.CompletedProcess(
            [],
            0,
            stdout=(
                "{ path=/bin/worldserver ; argv[]=/bin/worldserver -c /runs/fresh/worldserver.conf ; ignore_errors=no ; }"
            ),
        )
        with patch.object(server.subprocess, "run", return_value=result):
            self.assertEqual(server.world_config(), Path("/runs/fresh/worldserver.conf"))

    def test_stopped_simulation_cannot_resume(self):
        with patch.object(server, "simulation", return_value=True), patch.object(
            server, "state", return_value="inactive"
        ), patch.object(server.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "./server reset"):
                server.start()
            run.assert_not_called()

    def test_restart_refuses_before_stopping_active_simulation(self):
        with patch.object(server.sys, "argv", ["server", "restart"]), patch.object(
            server, "simulation", return_value=True
        ), patch.object(server, "stop") as stop:
            with self.assertRaisesRegex(RuntimeError, "./server reset"):
                server.main()
            stop.assert_not_called()

    def test_simulation_stop_uses_manager_not_unconnected_console_fifo(self):
        with patch.object(server, "simulation", return_value=True), patch.object(
            server.subprocess, "run"
        ) as run, contextlib.redirect_stdout(io.StringIO()):
            server.stop()
            run.assert_called_once_with(
                ["systemctl", "--user", "stop", server.WORKER, server.DASHBOARD, server.WORLD, server.AUTH], check=True
            )

    def test_reset_dispatches_tracked_helper(self):
        with patch.object(server.sys, "argv", ["server", "reset"]), patch.object(server.subprocess, "run") as run:
            server.main()
            run.assert_called_once_with(
                [server.sys.executable, str(ROOT / "apps/observatory/reset_realm.py")], check=True
            )


if __name__ == "__main__":
    unittest.main()
