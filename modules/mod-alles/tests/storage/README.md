# Snapshot persistence

`SnapshotDao` is a world-thread service. Construct it after database startup and module layout validation,
then keep it alive through `OnAfterUnloadAllMaps` while the pools remain open. The runtime connects this lifecycle
for the configured in-process pilot; database-backed behavior still needs live validation.

`StartLoad(owner, generation)` queues four prepared queries on one query-holder connection: actor metadata,
pending perceptions, memories, then actor metadata again. All three statement forms use a left-join seed so
successful empty results contain one null sentinel row. A null query result always means failure, including
when the owner has never existed. Child reads fetch at most the configured cap plus one; the extra row detects
oversized snapshots. Missing actor metadata with retained children is corrupt and is rejected.

The before/after actor metadata must match. Consistency relies on mod-alles being the sole writer, each
snapshot being one transaction, revisions increasing on every material/decay change, and retries preserving
the exact snapshot at the same revision. External database edits during operation violate that contract.
A revision change returns a retryable `RevisionChanged` value; query failures return `QueryFailed` and never
an empty snapshot. Only `Loaded` and `Missing` carry an installable snapshot. The caller applies generation
fencing and `ActorStore::FinishLoad`; that method resets boot-local admission timestamps and emission IDs.

`StartSave` validates the shared domain snapshot before allocating any statement, admits one write per owner
and at most four globally, and keeps the `AsyncCommitTransaction` completion future. `Poll` checks futures
without waiting and returns bounded owned values. It does not invoke callbacks or mutate ActorStore. Pass
only `Committed` as success to `CompleteSave`; submitted failed transactions remain dirty for an ordered retry.
A rejected start submitted nothing; return that captured request to the store as a failed save so it can retry.

Shutdown sequence, using one explicit deadline such as `steady_clock::now() + seconds(30)`:

1. Stop worker/gameplay intake, resolve or retain ordered pending inputs according to village/lab policy,
   and call `StopAsync`. Keep handling final lifecycle records through session/map teardown.
2. Call `DrainOlderWrites(deadline)` directly on the shutdown thread. Apply completed true/false outcomes to
   the store. `UndrainedOlderWrite` is a status report, **not a completed transaction**: retain its store state
   and future, and do not acknowledge it as failed or capture a replacement write for that owner.
3. For each drained dirty owner, use `ActorStore::CaptureFinalSave(owner, realTimeMs)` and call
   `CommitFinal(request, deadline)`. This bypasses ordinary retry delay without fabricating clock values.
   The call rejects outstanding reads/writes for that owner. It uses `DirectCommitTransaction` and then a
   prepared committed-revision readback. A void commit return never proves durability. Missing/failed
   readback returns `ReadbackFailed`; a different revision returns `RevisionMismatch`.
4. Report all failed/undrained owners without eviction or durability claims. An existing pending load must
   complete and install successfully before its owner can produce a final save. Closing read intake does
   not turn unresolved loads into empty snapshots.

The deadline limits waiting for older futures and admission of each synchronous database operation.
`DatabaseWorkerPool::GetFreeConnection` currently spins without a timeout, and `MySQLConnection` configures no
explicit connect/read/write timeout options. The core's synchronous calls cannot be cancelled by this DAO;
an admitted call can overrun the budget or remain blocked. A hard wall-clock shutdown bound is therefore
not implemented by this API and requires additional core support. If commit overruns, no readback starts
and durability remains unconfirmed; if
readback finishes late with the expected revision, `Committed` is accompanied by `budgetExceeded=true`.
No fallback thread performs database writes after returning from shutdown.

`SnapshotStatementsTest.cpp` exercises real prepared parameter storage without a database: typed equal-ID
isolation, 64-bit counters/times, nullable references/depth, double precision, gated speech, Unicode byte
ownership, and rejected submissions before allocation. All six cases passed on 2026-09-07.

Still-required authorized integration proofs: existing/missing/corrupt/over-cap loads; failed child/actor
queries; concurrent revision changes; write commit failure/retry; delayed older writes; deadline expiry;
final commit/readback failure; and final perception/logout ordering. A passing parameter test is not evidence
that MySQL transactions or shutdown persistence have passed those gates.
