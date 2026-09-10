# Satisfaction and activity choice

With `Alles.Brain.Enable=1`, each managed character compares expected satisfaction over the next ten minutes.
The score combines named motives with individual weights, current fulfillment, depletion rates and diminishing
returns. The initial authored motives are security, rest, discovery, achievement, companionship and resources.
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
Preferences remain authored, rather than changing from a model-generated explanation.

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
.alles effect player 123 pursue_quest craftsmanship 0.2
.alles flush player 123
```

`motive` takes an identifier, weight (0–10), depletion per hour (0–10), and satiation (0–1). It preserves existing
fulfillment. New authored dimensions start unfulfilled. At most 32 dimensions are accepted and total weight
must remain positive. `effect` binds an installed activity to a defined motive with a net effect in [-1,1].
The installed activity names are `pursue_quest`, `discover_work`, `explore_place`, `rest`, `visit_companion` and
`help_companion`. Use the flush receipt to distinguish requested persistence from an acknowledged save.

`planning.satisfaction` includes dimensions, authored activity effects, learned outcomes, selected objective,
maintain-state value, alternative contributions, travel estimates, perceived risk, route reasoning and cooldowns.
`planning.body` reports the owning intention and physical progress/failure counters. Objective purpose, observed
activity time and completion time distinguish arrival from actual achievement. Version 11 planning snapshots
persist the model and receipts; versions 1–10 load explicit defaults.

Unit tests cover valuation, safer routes, ownership, invalid inputs, replay prevention and snapshot migration.
The live rest keeper is `TestObservatory_SatisfactionProducesObservedRest`; see the e2e inventory for its fixture.
Large-cohort performance and long-term behavioral quality require separate measured runs.
