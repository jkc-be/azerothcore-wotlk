import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


HELPER = Path(__file__).resolve().parents[2] / "integrations/playerbots/apply_patch.py"
SPEC = importlib.util.spec_from_file_location("playerbots_patch_helper", HELPER)
patch_helper = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(patch_helper)

PATCH = """diff --git a/source.cpp b/source.cpp
--- a/source.cpp
+++ b/source.cpp
@@ -1 +1 @@
-before
+after
"""


class PlayerbotsPatchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="playerbots-patch-test-")
        self.addCleanup(self.directory.cleanup)
        self.repository = Path(self.directory.name) / "provider"
        self.repository.mkdir()
        self.source = self.repository / "source.cpp"
        self.source.write_text("before\n")
        self.patch = Path(self.directory.name) / "integration.patch"
        self.patch.write_text(PATCH)
        self.git("init", "--quiet")
        self.git("add", "source.cpp")
        self.git("-c", "user.name=Test Fixture", "-c", "user.email=fixture@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture")

    def git(self, *arguments):
        subprocess.run(
            ["git", "-c", "core.hooksPath=/dev/null", "-C", str(self.repository), *arguments],
            check=True, capture_output=True,
        )

    def test_check_is_read_only_then_apply_is_idempotent(self):
        patch_helper.prepare(self.repository, self.patch)
        self.assertEqual(self.source.read_text(), "before\n")
        patch_helper.prepare(self.repository, self.patch, apply=True)
        self.assertEqual(self.source.read_text(), "after\n")
        message = patch_helper.prepare(self.repository, self.patch, apply=True)
        self.assertIn("already applied", message)
        self.assertEqual(self.source.read_text(), "after\n")

    def test_dirty_target_is_preserved(self):
        self.source.write_text("before\nlocal work\n")
        with self.assertRaisesRegex(RuntimeError, "local changes"):
            patch_helper.prepare(self.repository, self.patch, apply=True)
        self.assertEqual(self.source.read_text(), "before\nlocal work\n")

    def test_incompatible_patch_does_not_change_sources(self):
        self.patch.write_text(PATCH.replace("-before", "-different revision"))
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            patch_helper.prepare(self.repository, self.patch, apply=True)
        self.assertEqual(self.source.read_text(), "before\n")


if __name__ == "__main__":
    unittest.main()
