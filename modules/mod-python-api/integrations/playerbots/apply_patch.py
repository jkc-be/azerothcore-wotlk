"""Check/apply the source patch to an existing Playerbots checkout; never fetch or change branches."""

import argparse
from pathlib import Path
import subprocess


def git(repository, *arguments):
    return subprocess.run(
        ["git", "-C", str(repository), *arguments], capture_output=True, text=True, check=False,
    )


def prepare(repository, patch, apply=False):
    if git(repository, "rev-parse", "--show-toplevel").returncode:
        raise RuntimeError("the Playerbots path must be an existing Git checkout")
    patch = patch.resolve()
    if git(repository, "apply", "--reverse", "--check", str(patch)).returncode == 0:
        return "Playerbots external-control patch is already applied."
    paths = [line.split(" b/", 1)[1] for line in patch.read_text().splitlines() if line.startswith("diff --git ")]
    changed = git(repository, "status", "--porcelain", "--", *paths)
    if changed.returncode or changed.stdout.strip():
        raise RuntimeError("patch targets contain local changes; preserve/reconcile them before applying")
    check = git(repository, "apply", "--check", str(patch))
    if check.returncode:
        raise RuntimeError("patch does not match this Playerbots revision:\n" + check.stderr.strip())
    if not apply:
        return "Playerbots patch check passed. Use --apply to apply these source changes."
    result = git(repository, "apply", str(patch))
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return "Applied Playerbots source patch. No build or server process was started."


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path, help="path to the existing modules/mod-playerbots checkout")
    parser.add_argument("--apply", action="store_true", help="apply after checking; default is read-only")
    arguments = parser.parse_args()
    patch = Path(__file__).with_name("playerbots-external-control.patch")
    try:
        print(prepare(arguments.repository.resolve(), patch, arguments.apply))
    except RuntimeError as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
