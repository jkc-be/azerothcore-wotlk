# GM-only POV validation, 2026-09-06

Implementation: `36ffd8eda` with Playerbots `c405aac5f956c791f6891efb39bf149c1325f338`.
The following commit only wraps C++ lines and formats frontend/tests/docs. The installed binary contains the
same C++ behavior. This was an authorized update of the existing local disposable stack, without another realm.

- Clang 18, Ubuntu 24.04 container on Fedora, RelWithDebInfo, static scripts/modules. Worldserver, authserver and
  unit_tests built. The separately integrated mod-python-api remained disabled.
- CTest unit target passed. Nine bridge unit tests passed. Frontend JavaScript syntax check passed.
- C++ linter passed on all changed C++ files. Full-tree lint reports pre-existing violations elsewhere.
  SQL linter cannot fetch this fork's nonexistent `origin/master`; no SQL files changed.
- `TestObservatory_GmPovConnectionLock` passed against the existing world with 50 active bots. It proved GM login
  from pause, native POV list/watch/switch/stop, ordinary-account rejection, blocked movement and money/death commands,
  world mailbox and HTTP control rejection, character-selection locking, two GM leases and abrupt disconnect cleanup.
- The test found the ordinary one-minute offline-session grace period retaining the observer lock. The implementation
  now bypasses that grace period for observers; the complete test passed after that fix.
- A separate attempt stopped on an SRP logon-proof timeout before the second GM reached realm login. That is recorded
  as a setup failure, not a passing scenario. The subsequent complete attempt passed in 5.89 seconds. The final short
  observer window achieved about 0.82 simulated seconds per real second at requested 1×, below capacity target.
- Native addon rendering, cinematic exit, target death and map/instance-transfer camera behavior still require manual
  client checks. Ordinary mode regression testing and the 100-bot/10×/24-hour benchmark remain separate pending checks.

No claim of measured 10× performance or full gameplay timing equivalence is made here. The native observer test is
excluded from scientific comparisons because a GM can load additional grids and change visibility workload.
