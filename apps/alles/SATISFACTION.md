# Individual motivations and learned activity choice

With `Alles.Brain.Enable=1`, each managed character compares expected satisfaction over the next ten minutes.
The score combines named motives with individual weights, current fulfillment, depletion rates and diminishing
returns. New WoW owners have security, rest, discovery, achievement, companionship and resources needs, alongside
continuing wealth and equipment ambitions. Each new owner gets independently varied, persistent weights; existing
saved preferences are retained. There is no fixed personality class restricting the possible combinations.
They describe simulation preferences; they are not a claim to model human subjective experience.

An activity can affect several motives. Accepted quests can provide achievement and resources; helping a
companion can also provide companionship. Discovery requires a personally new area observation. Rest requires
one minute of observed stationary time outside combat and maintenance. Social visits require an ordinary
interaction delivered to the intended, personally perceived companion. Selecting an activity awards nothing.
Rest contributes continuously while observed; persisted counters prevent replay. Completed rest and social
activities have per-owner ten-minute cooldowns that survive objective-history eviction.

A delivered interaction with the intended companion can complete a visit after they cross an area boundary.
Physical arrival starts a bounded wait even when the destination's area label is stale. Arrival alone grants no
fulfillment. Completed rest, discovery and companion visits appear in the dashboard's ordinary action history.

Security follows observed health and resources follow actual own money. Other fulfillment changes use confirmed
quest counters, rewards, visits, interactions and rest. Gameplay time drives depletion; unavailable/offline time
is not simulated as activity. Bounded success counts and mean execution durations update expectations after an
observed attempt. Up to 32 private route estimates retain observed success and travel-time corrections.
Failure lowers that approach's reliability without crediting its predicted benefits.
Preferences remain authored; learning improves expectations of how to fulfill them without rewriting priorities.

The model-assisted planner and deterministic fallback receive the same core-computed valuations. Worker output
cannot replace the selected preference with a lower-valued activity. A feasible intention has a two-minute
commitment window; subsequent switches need a material score improvement. Perceived danger can end commitment.
Human control, cooperation, combat, recovery, capabilities and actual spending constraints still govern execution.

## Navigation

Activity identity includes purpose, place and, for companionship, person. Exhausted work searches do not exclude
social visits or local rest. Social destinations use recent personally observed positions. Geography anchors
supply navigation geometry for already known areas; they do not reveal remote creatures or other players.

Before pathfinding, the core compares coarse forecasts and retains the current intention plus each purpose's
best candidate, filling at most eight route slots. Navigable corridor length estimates remaining travel time;
partial paths retain uncertainty. Perceived hostile creatures contribute risk along corridor segments. For
known destinations, at most two detours around a visible threat are compared using the same satisfaction model.
A selected detour retains the parent intention and offers no reward for its waypoints. The existing body handles
bounded corridor legs, progress, combat interruptions, failed attempts and local walkable recovery probes.

Staying also includes exposure to personally visible nearby enemies, using the same risk forecast as local
activities. `planning.satisfaction.stayingRisk` exposes that estimate. A character in danger cannot compare
travel against an imaginary safe idle state; actual health observations still determine security fulfillment.

Travel consumes rest and delays the activity's benefit once in the forecast. Actual moving time supplies the
corresponding observed cost. Distances already walked do not improve the score. Staying locally is an explicit
alternative. Unsupported cross-map routes cannot be executed. Quest destinations come from the existing tactical
executor. Once it has an approach position, an owned route policy can direct the same bounded detours. Until
then, the travel estimate is an uncertain prior. Policies are fenced by incarnation, intention and destination.
The implementation does not learn a persistent global route graph or plan transports and portals.

## Preferences and telemetry

GM diagnostics operate on a loaded managed player:

```
.alles motives player 123
.alles motive player 123 companionship 4 0.3 1
.alles motive player 123 craftsmanship 2 0 0.5
.alles ambition player 123 wealth 8 1000
.alles ambition player 123 equipment 6 100
.alles effect player 123 pursue_quest craftsmanship 0.2
.alles flush player 123
```

`motive` takes an identifier, weight (0–10), depletion per hour (0–10), satiation (0–1), and optional
need urgency (0–10). Omitting urgency preserves it. It preserves existing
fulfillment and curve. `ambition` takes an identifier, weight and positive scale in observation units; it selects
the continuing growth curve while retaining the observed quantity. New authored dimensions start unfulfilled. At most 32 dimensions are accepted and total weight
must remain positive. `effect` binds an installed activity to a defined motive with a net effect in [-1,1] for needs, or measured units for ambitions (up to 1e12).
The installed activity names are `pursue_quest`, `discover_work`, `explore_place`, `rest`, `visit_companion` and
`help_companion`. Use the flush receipt to distinguish requested persistence from an acknowledged save.

`planning.satisfaction` includes dimensions, authored activity effects, learned outcomes, selected objective,
maintain-state value, alternative contributions, travel estimates, perceived risk, route reasoning and cooldowns.
`planning.body` reports the owning intention and physical progress/failure counters. Objective purpose, observed
activity time and completion time distinguish arrival from actual achievement. Version 13 planning snapshots
persist ambitions, contextual outcomes, the horizon and need urgency. Version 12 retains its existing
preferences with urgency zero; version 11 retains its existing needs and preferences;
versions 1–10 load explicit legacy defaults. Older owners can acquire ambitions through the commands above.

Unit tests cover valuation, safer routes, ownership, invalid inputs, replay prevention and snapshot migration.
The live rest keeper is `TestObservatory_SatisfactionProducesObservedRest`; see the e2e inventory for its fixture.
Large-cohort performance and long-term behavioral quality require separate measured runs.


## Needs, ambitions and learning

Needs use fulfillment in [0,1], depletion and a diminishing-return response. Continuing ambitions use measurable
nonnegative quantities and `log(1 + value / scale)`: another improvement stays valuable as possessions grow.
Losing and regaining possessions cannot beat retaining them. Weights express relative personal importance;
scale defines meaningful units. `SatisfactionSnapshot.horizonMs` selects one second to one hour of lookahead,
with ten minutes as the default. The score compares one actor's alternatives; it is not a percentage of happiness
or a comparison between people. The browser distinguishes need fulfillment, raw ambitions and personal priorities.

Successful and failed owned attempts teach separate net wealth/equipment outcomes, including explicitly observed
zero yield. Estimates transfer through a global activity prior and specialize by area and level band. Four prior
observations regularize each estimate. Missing measurements do not count as zero. Counts decay after 1,000 samples;
32 activity histories and 128 contexts bound work and storage, without imposing a fixed list of objectives.
Learning changes predictions; only grounded observations change holdings. Reload starts a fresh observation
baseline, and existing objective receipts prevent the same completed attempt being learned again.

Wealth follows carried copper. Equipment uses the core's `GetTotalItemLevel()` quality-adjusted equipped item-level
sum, excluding shirt, tabard, offhand and ranged slots. This is a gear-quality proxy, not optimal combat performance.
Accepted quests supply known net money and currently usable equipment upgrades, respecting reward choices and
replacement slots. These known rewards supersede uncertain priors. Seeking work includes one possible subsequent
quest plus its travel/activity time; finding work cannot grant that later reward. Unknown-work priors initially
expect 50 copper and five equipment points per quest, then measured outcomes revise them. Actual loot and
equipment execution remain with the existing body. Delayed rewards may occur in a later activity's observation
window; the estimates describe observed episodes, not a proven causal model of every item acquisition.

## Reusing the decision model

`SatisfactionModel`, `ForecastAttempt` and their data structures use only the C++ standard library. An environment
adapter supplies observable metrics, preferences, capabilities, costs, consequences and unique outcome receipts.
Names do not have to be WoW concepts. For example, a farming/research simulation can use the following state:

```cpp
SatisfactionSnapshot state;
state.dimensions = {
    {"energy", {1, 0.8, 0.1, 1}},
    {"credits", {4, 10, 0, 0, MotivationCurve::Growth, 10}},
    {"knowledge", {1, 10, 0, 0, MotivationCurve::Growth, 10}}
};
state.activities = {{"sell_crop", {{"credits", 20}}}, {"research", {{"knowledge", 20}}}};
SatisfactionModel model;
model.Restore(state);
auto forecast = ForecastAttempt(10000, 60000, 0.8, model.ExpectedEffects("sell_crop", "market"),
    {{"energy", -0.1}}, {{"credits", -2}});
// Compare candidates with Choose, execute in the environment, and learn only from the observed result.
model.LearnOutcome("sell_crop", "market", true, 60000, {{"credits", 12}});
```

Keep observation application (`Observe`) separate from expectation learning (`LearnOutcome`), and accept each
receipt once. The existing objective receipts provide this fence in WoW. `ForecastAttempt` has no implicit need
names; the older `ForecastActivity` convenience function retains WoW rest/security conventions. New activities
still require real executors: naming an activity does not invent a capability.

Knowledge reuse can improve future choices, but no exponential multiplier is applied to learning or rewards.
This is bounded outcome learning, not neural retraining or arbitrary code generation. Richer plans, markets,
professions and another game's body need corresponding adapters and observations. Long-term human likeness
requires measured behavioral evaluation; the framework and its tests do not establish that by themselves.

`TestObservatory_IndividualMotivationsLearnObservedOutcomes` covers distinct priorities, native client movement,
measured wealth and persisted outcome learning. Pure tests run the separate farming/research example, reverse
choices after learned outcomes, and cover continued ambition, failure effects, transfer and legacy migration.


## Surviving and thriving

Essential needs can carry `urgency`: a deficit subtracts `urgency * (1 - fulfillment)^2` from their response.
As injury or exhaustion deepens, another loss becomes more costly and recovery becomes more valuable. Once
healthy and rested, characters retain their individual ambitions and can choose rewarding opportunities again.
The mechanism is a generic need parameter, not a hard-coded ban on risk or a universal ordering of desires.
New WoW owners start with security urgency 4, rest urgency 2, and a security weight of at least 1.5 after initial
personality variation. Explicitly authored priorities and existing saved characters remain under operator control.

Rest initially predicts up to 0.35 health/security recovery over a minute, subject to the same local danger as
staying. This is an uncertain prior: actual health is always read from the body. Rest episodes that began below
60% health learn their observed recovery; healthy rest supplies no evidence that healing is ineffective. Contexts
now distinguish hurt and well starts. Observing an injury below 60% health outside combat permits reconsidering
commitment and reopening rest, even during its ordinary ten-minute cooldown. A completed rest retains a minimum
30-second interval and starts with zero observed/credited duration when reopened; social cooldowns are unaffected.
Combat and body maintenance still own immediate execution and healing. No predicted recovery grants hit points.

For an existing actor, for example:

```
.alles motive player 123 security 2 0 1 4
.alles motive player 123 rest 1 0.6 1 2
.alles effect player 123 rest security 0.35
```

The dashboard labels currently depleted urgent needs as recovery needs. Injury-versus-health choices, avoiding
further harm, continued worthwhile exploration, cooldown exception fencing and version migration have regression
coverage. These mechanisms express a desire to survive and thrive; they do not guarantee zero deaths or implement
new combat escape, medical or consumable abilities outside the body's supported actions.
