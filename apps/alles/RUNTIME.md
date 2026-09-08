# Agent runtime contract

The game owns per-actor admission, scheduling and result validation. `PilotCoordinator` still owns memory jobs,
perception prefixes and evidence fences. The common scheduler stores bounded descriptors, not a second copy of the
memory store. Conversations and plans keep their existing delivery and objective revision checks.

## Protocol and execution

The authenticated line protocol remains the game/agent boundary. A v2 `worker_hello` contains the existing profile,
model, timeout (20 seconds) and planning schema (1), plus `contractVersion: 2`, `maxInFlight: 1..32` and
`maxCallsPerJob: 1`. The reply adds the active policy and durable reservation buckets. Polling takes one job at a time
and can execute multiple returned jobs concurrently on the same multiplexed connection; no worker-side waiting queue
is introduced. Different actors may execute concurrently; each actor has one slot across every purpose.

Purposes are memory, planning, conversation and interview. Legacy v1 workers remain restricted to their negotiated
purposes and single connection slot; they cannot receive interviews. Workers must still match the exact model and
profile. The new worker fingerprint includes the interview prompt, execution driver, backend profile and protocol
capabilities, and excludes mutable scheduling limits. Deploying it requires a compatible world configuration and
profile-matched ledger; never delete an old ledger to obtain a fresh allowance.

The initial adapters perform at most one actual model call per job, with no retries and no game tools. They explicitly
reject negotiation of a multi-call job. The call limiter counts each acquisition separately (including fixtures that
exercise several calls per logical operation); enabling an agent loop requires extending the negotiated call-budget
contract, not removing this guard. This implementation does not grant a remote framework gameplay capabilities.

`callCount` (0 or 1) and `usageKnown` accompany reports. A rejected local context has zero calls; an HTTP attempt has one,
even if it fails. Unknown token usage is marked unknown, not claimed to be zero measured usage. `backendCalls` counts
received authenticated reports since world boot; missing/late reports may remain unknown. `usedRequests` is the durable
lifetime reservation count, including attempts whose execution is uncertain. Neither means delivered speech/actions.
Latency histograms and token totals describe received reports; `unknownUsageJobs` identifies incomplete token totals.
Memory application and conversation delivery retain separate existing telemetry.

Cancellation fences results immediately. Logical cancellation, timeout or disconnect does not refund reservations.
Unknown physical completion retains capacity through its bounded fence; a fully consumed successful response can end
that fence. Successful gameplay results hold the actor slot until the world runtime takes them for validation.
Result queues, descriptor queues, transport frames, connection buffers and retained attempts all have independent bounds.

## Scheduling and controls

The provisional defaults are one concurrent job/call, two waiting jobs per actor, ten globally, 48 KiB per actor and
240 KiB globally. Queue age defaults to 20 real seconds, leaving a separate bounded execution window. Policy accepts
1–20 seconds of waiting, 1–32 concurrent jobs/calls, 1–64 waiting jobs per actor, 1–256 globally, 1 KiB–1 MiB per actor
and up to 16 MiB globally. The global count/byte bounds must be at least their per-actor bounds. These are implementation
safety bounds, not a measured provider capacity.

Actors rotate after dispatch. Within an actor, live conversation outranks planning/memory, which outrank interviews;
priority ages every five real seconds. Causal keys prevent overtaking an earlier turn. A queued plan can replace only
its own actor/generation/decision key, keeping the original age and expiry. On queue shrink, higher priority and older
waiting work survives first; outcomes name every eviction. Memory refusal consumes an explicitly counted fallback
through the original coordinator, or retains perceptions with their original ingress/deadline fence if they cannot
currently be consumed. Routine death salience and reflex formation are unchanged. External workers no longer inherit
the in-process fake's fixed four-jobs-per-minute owner bucket.

`Alles.Interpreter.BudgetMode` defaults to `limited`; `rolling` is accepted as its old spelling. `unlimited` removes the
RPM gate, and `trial` explicitly enables the finite test reservation budget. Queue, execution, context and concurrency
bounds always apply. The Go service enforces actual-call RPM/concurrency immediately before each HTTP attempt. The world
also retains conservative durable reservation admission. Real-time buckets keep accounting bounded at high throughput;
rounding can postpone replenishment by less than a second, never bring it forward. Rate changes preserve usage.

The worker process retains usage across reconnects. On process restart it recovers reservations conservatively as
possibly late calls within the 45-second job fence. The ledger retains 105 seconds for this recovery, while its ordinary
rate window is one minute. A world restart with recent reservations also drains uncertain prior execution for 45 real
seconds. New bucket checkpoints intentionally fail closed in old binaries that cannot parse them. Preserve compatible
world/worker/ledger backups for rollback; do not replay a new profile's ledger under an unrelated old profile.

Dashboard scheduling uses a separate private `Alles.Interpreter.ControlTokenFile`, different from the worker token.
The bridge reads this file server-side; the browser continues to use its existing Observatory bearer token. The new
`GET /api/interpreter` and `POST /api/interpreter/policy` routes do not enable ordinary-world speed/population/GM controls.
No browser operation accepts a provider URL or credentials.

`interpreter_policy` carries run, command ID, expected revision and a closed schema-1 policy. Stale runs/revisions,
reused command IDs with different content and invalid bounds fail explicitly. The transport persists the override
with a private temporary file, file fsync, atomic rename and directory fsync. The old active policy remains visible
while persistence is pending. Only durable completion advances the active revision. The default override path is
`TrialLedger + ".policy.json"`; startup restores it, failing closed on unknown/corrupt schema. Lower concurrency drains
existing work. The panel shows requested, pending, active and negotiated effective capacity separately.

## Grounded interviews

Production interviews now use `interview_submit`/`interview_status` through the common scheduler and reservation path.
They cannot form/rehearse memory, deliver chat, execute actions or modify objectives. The dedicated control capability
is required; there is no direct model fallback when it is absent. At most eight observer HTTP requests and sixteen
bounded interview receipts are retained. The world admits only configured, currently live owners.

The bridge ranks that owner's committed evidence using named subject/source matches and supported name prefixes
(`Humane`, `human*`), then topic words and salience. It never resolves unknown names against the global character table.
Its configurable evidence byte budget replaces the 24-row selection cutoff. Retrieval reports candidates, duplicate or
oversized exclusions, selected counts, trimming and store-wide coverage. Positive entity counts also include matches
omitted by the context budget, preventing an omitted match from becoming a claim of never having met someone.

The world supplies a fresh personal snapshot at admission and again at dispatch: observed area, activity/health, own
quest titles/statuses and the current objective/step with revision and known destination. Historical objectives and
unrelated private geography are excluded. The worker prompt distinguishes observations, plans, hearsay and unknown
state. Interview history resolves references but never supplies new factual evidence. Scripted fixtures verify the
contract and retrieval; they do not establish a real model's factual accuracy.

## Native AI SDK adapter

The optional local process in `ai-sdk/` uses pinned `ai@6.0.277` and `@ai-sdk/openai-compatible@2.0.74` with structured
outputs enabled, `maxRetries: 0`, abort signals and no tools. It implements the bounded execution API used by the Go
worker; C++ has no provider SDK dependencies. An SDK is distinct from a model gateway/provider.

The package choices were checked against the official npm registry during implementation. Relevant official APIs:
[generateText](https://ai-sdk.dev/docs/reference/ai-sdk-core/generate-text),
[settings and cancellation](https://ai-sdk.dev/docs/ai-sdk-core/settings), and
[compatible provider](https://ai-sdk.dev/providers/openai-compatible-providers).
The real SDK runs against a local scripted HTTP provider in tests, covering structured success, malformed output and
throttling without retries. No real provider execution is part of those tests.

For a later authorized deployment, install with `npm ci --ignore-scripts` in `ai-sdk/`, then run `node server.mjs` using
server-side `ALLES_PROVIDER_URL`, `ALLES_MODEL`, optional `ALLES_PROVIDER_KEY`, and a separate `ALLES_ADAPTER_TOKEN` of at
least 32 characters. It binds loopback port 8780 (override with `ALLES_ADAPTER_PORT`). Node 22+ is required. The adapter's
`--fingerprint` prints its route/model/package/structured-output profile without a model call. Configure the Go worker
with driver `ai-sdk`, that `backend_profile`, base URL `http://127.0.0.1:8780/v1`, and `api_key_env` naming the environment
variable containing the adapter token. Backend profile is verified both at readiness and on each completion. The direct
compatible/Ollama driver remains available. Endpoints and credentials belong to secret server configuration.

## Simulation and acceptance

`Alles.SchedulingProfile=simulation` is accepted only with `Observatory.Enable`; the existing disposable acknowledgement,
`obs_` database guards, fresh run directory and human-observer 1x restriction remain intact. The existing loop still
owns the simulation spool; `Alles.Telemetry.Directory` must be empty so startup cannot archive an ordinary realm's
telemetry. Point the bridge at the isolated spool with its isolated `--world-conf`. The existing loop
executes ordinary 10 ms steps. A world-thread maintenance hook services transport, durable controls and bounded results
while paused without gameplay dispatch/application. Real leases expire during pause and results are revalidated after
resume. Game-time and real-time expiry counters are distinct. Auto uses smoothed occupancy, waiting age, utilization
and drops in addition to tick debt, with separate pressure thresholds and a ten-second reduction cooldown.

Build and live acceptance are separate from implementation. Run the C++ scheduler/bridge/policy tests after an explicitly
authorized build, then the existing Alles live e2e on disposable fixtures, then isolated baseline/1x/2x/5x/10x comparisons.
Do not reset the existing five characters, restart the ordinary realm merely to test this patch, reuse simulated future
databases, or claim resumable accelerated worlds. Browser rendering, real-provider grounding, clock comparisons and
achieved-speed measurements still require their corresponding acceptance runs.
