# Integration gates

These tests require an explicitly authorized configure/build. They do not connect to a database.
Enable `BUILD_TESTING` with static mod-alles and static mod-playerbots. The module registers its domain
sources with the existing `unit_tests` target.

`ctest -R '^alles_statement_layout_mismatch$' --output-on-failure` runs a separate consumer compiled with
`MOD_ALLES` deliberately removed. It calls the same layout guard as module registration and startup;
the test passes only if the process fails with the specific layout mismatch diagnostic. It never allocates
or executes a prepared statement. A failed launch or unrelated crash does not satisfy the diagnostic check.

On 2026-09-07 the native static auth/world build, all 97 alles unit tests and the deliberate mismatch CTest passed.
The public `MOD_ALLES` definition was verified in generated compile commands for database, game, modules,
auth/world and test consumers. Disabled/enabled pilot startups reached readiness, and the updater imported
the module migration.
The remaining startup matrix and database failure-injection gates are still open.

Build and runtime matrix still required before W1 can be marked verified:

- Module absent, including an old cached static linkage setting: no `MOD_ALLES` definition; boot without tables.
- Module present but linkage disabled: no `MOD_ALLES` definition; boot without tables.
- Static mod-alles and static mod-playerbots: public definition reaches database, game, modules, authserver
  and other consumers; updater imports `alles_0001_owner_memory.sql` before statement preparation.
- Dynamic mod-alles, or static mod-alles with absent/disabled/dynamic playerbots: configuration rejects it.
- Compiled module with `Alles.Enable=0`: the schema is still required; layout guard runs at registration/startup.
- A mismatched consumer fails before statement use, as above (passed).

Enabled first-light source requires 1-64 configured player owners, `Alles.Worker.Mode=inprocess-fake`,
`Alles.SchedulingProfile=pilot`, and `Observatory.Enable=0`. Invalid or unsupported combinations must fail startup.
No provider or Go binary is required. These startup cases have not been executed.
Apply the module migration manually before startup when automatic updates are disabled. Do not use ordinary
player data for destructive fixture or isolation tests.

The DAO needs one additional core fix beyond the plan's original inventory: `PreparedStatement.cpp` now
explicitly instantiates `SetValidData<double>`. The parameter variant and MySQL binding already supported
double, but the out-of-line template omitted that instantiation. Storage binding tests use the actual
double parameters for confidence and salience, preserving their precision across writes and reloads.

The subsequent live gate uses the native stack and the plan's six configured actors at ordinary 1x speed. Arrange
one death witness, one listener in speech range and another actor outside that range. The witness's ordinary SAY
must form only hearsay in the listener; the distant actor must learn nothing. Verify self recall, a flush revision
acknowledgment followed by relog, and fallback with `Alles.Worker.FakeWithhold=1`. Fake withholding normally reaches
its 25-second permit expiry before the 45-real-second admission deadline; separate coordinator fixtures cover
admission expiry. Record the actual path and observed timing. A human's assessment of the watched scene remains
separate from these correctness checks.

Also exercise same-character bot/human handoff with an in-flight snapshot and a queued old logout. Verify that
store generation and memories survive, attachment changes, no load overwrites pending writes, and the old record
does not close the new session. Source fixtures alone do not establish any of these live gates.
