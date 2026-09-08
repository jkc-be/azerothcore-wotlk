# Alles brain and Playerbots body

`Alles.Brain.Enable = 1` enables an experimental execution controller for configured Alles owners. It requires
`Alles.Objectives.Enable = 1` and the existing autonomous `new rpg` eligibility checks. It defaults to off.
The worker protocol, personal memories, private geography and durable objective book remain compatible.

The brain selects and retains intentions through the objective book, model decisions, information requests,
readiness policies and cooperation. The body executes typed skills: travel, investigation, quest work, follow,
repair, supplies and rendezvous. Follow, preparation and cooperation continue using their existing Alles
adapters. Travel, investigation and quest work use Playerbots physical actions through a separate noncombat
executor. That executor never ticks the ordinary roaming/grinding/quest-selection strategy engine.

Class buffs, cures, food and physical interaction receipts run as maintenance. Existing class combat and death
recovery retain control during those interruptions. Quest combat reuses the existing tactical target selection,
limited to quest-relevant targets during an owned quest's attempt phase. It does not select an independent XP
grinding goal. Existing accepted-quest loot knowledge remains part of that executor; this change does not add
new personal knowledge or a global quest guide. The body never abandons quests to free log space.
An owned quest only interacts with its own quest menu entries. Investigation can accept new work. Loot handling
selects corpses and quest objects, opens targets already within reach, and defers failed attempts so incidental
loot cannot indefinitely prevent quest travel.

## Ownership and resumption

The command contains the owner's generation, attachment, objective ID and skill. Old generations cannot issue
commands or release a newer body. A five-second missing heartbeat suspends noncombat execution rather than
enabling a second autonomous planner. Combat and death recovery remain available. Human/selfbot/external
control yields immediately. No body object retains new live Player/Creature pointers across updates.

The current intention remains in the objective book during combat. Transient movement can resume its committed
leg afterward. Logout, reset and attachment changes discard the route; existing snapshot reconciliation loads
the intention and reconstructs execution from the current world. No movement commands are persisted or replayed.

## Travel and investigation

The existing area resolver supplies executor-only destinations for already-known areas. The body chooses a leg
of at most 160 yards along the navigation-mesh corridor, then validates and walks its smoothed path. This stays
within the core path generator's point budget even when the destination is far away. Progress follows that path,
so a road that temporarily leads away from the destination is valid. Interrupted time does not count against
the navigation budget. Repeated partial-path endpoints are remembered to reject oscillating routes.

For quest work, a map-marker centroid can sit above a cave. The body can replace it with a nearby quest-related
spawn's actual elevation, using the existing tactical quest-loot predicate within 200 yards of that marker and
1,500 yards of the bot. These approach coordinates remain executor data. The body still needs a valid navigation
path and ordinary gameplay interactions; an anchor does not prove that a target is alive or that work is complete.

Thirty seconds of active motion without route progress retires a leg. Three failed route attempts report a
navigation obstruction. A place intention can then try at most three distinct destination anchors before the
brain defers it through its existing retry policy. Travel never uses teleport recovery or random nudges after
a failed route. Current support is ordinary same-map navigation; transport/portal planning is not implemented.

Arriving in the requested area transitions travel into investigation. It does not complete a discover-work
objective: accepting actual new work in that area still supplies the completion evidence. Repeated empty searches
without covering distinct positions report inability to explore after a minute, without recording that the area
has no work. Spatially complete empty surveys retain their existing two-minute exhaustion policy.

## Observation and validation

The ordinary snapshot's `planning.body` reports attachment, objective, skill, state, interruption and route
waypoint/advance/stall/failure counters. `alles_body` journal events report skill transitions and alternate
destination attempts. These are execution observations; the dashboard memory interview remains read-only and
cannot issue commands. Existing UI consumers may ignore these additive fields.

`AllesBody*` unit tests exercise command ownership, heartbeat expiry, combat/recovery retention, detours,
fast movement, oscillation and interruption timing. The existing exploration e2e has optional `brain`,
`travelStarted` and `combatInterruption` fixture flags. See the [suite guide](../../e2e/suites/alles/README.md).
It requires real client-observed movement and a saved newly acquired quest, with optional combat/resumption
evidence for the same intention. Unit coverage does not establish live acceptance.

This is the first implementation of the body boundary, not a replacement for every Playerbots subsystem.
Cross-map travel, new motives/personality models, long-term route learning, dungeon planning and a redesigned
combat planner are outside this implementation. Existing cooperation, resource and human-handoff e2e remain
necessary regression checks before enabling the controller for a shared cohort.

## Accelerated experiments and control bots

Alles can now run in an isolated Observatory world. The world’s existing fixed-step clock still requires four
`obs_` databases and a fresh run directory. The ordinary realm remains at real time. The world thread polls
interpreter completions and persistence while gameplay is paused; it does not move, speak or plan physical actions
from that maintenance path. Objective/body heartbeats use gameplay time, including at accelerated rates.

The Observatory snapshot carries each Alles owner's planning and memory state alongside authoritative core movement,
combat and progression counters. Its source is `alles-simulation`. The separate Alles journal retains detailed brain
transitions. Trolls in the experimental deployment are absent from `Alles.Owners`, so they use the original Playerbots
planner and never attach the new body executor. Every navigation change in the body remains attachment-gated.
The dashboard marks Trolls as the control group. Race and starting-area differences still affect comparisons.

`Observatory.RaceRoster` pins a prepared `GUID:RACE` pool, also covered by `Observatory.BotGuids`.
`Observatory.RaceCounts` supplies ten space-separated counts in playable race ID order: 1,2,3,4,5,6,7,8,10,11.
Their sum must match `Observatory.BotCount`. The dashboard can replace quotas without changing the total;
scaling down logs bots out and preserves their characters. Classes are randomly selected when preparing the pool,
and scaling up reuses those identities and their progress. Requests above a race's prepared capacity are rejected.

Max mode accepts an LLM queue limit of 1–64 (default 8). This counts admitted memory, planning and conversation jobs,
including leased jobs, rather than raw pending perceptions or provider token counts. All three share admission capacity.
At the limit, the world holds gameplay and stops accumulating new time debt while the worker continues on real time.
It resumes when the queue drops to half the limit. The bridge also reduces requested speed under queue pressure.
Lowering a limit lets already admitted work drain; it does not discard or cancel those jobs. Manual speed ends this
Max guard. The existing bounded queues and fallback policy still apply in manual mode.

Existing `AiPlayerbot.AutoDestroyJunk` controls body bag cleanup. With it enabled, the body uses the ordinary guarded
junk action; loot collection retains an in-progress corpse approach and quest menus stay focused on the owned quest.
Normal group invitations remain available through the body's maintenance strategy.
