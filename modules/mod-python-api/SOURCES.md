# Design provenance

The queue architecture, headless lifecycle lessons, and shared Python interface were informed by Shoro2's examples:

- [mod-ai-controller, revision 85dd7e1](https://github.com/Shoro2/mod-ai-controller/tree/85dd7e1):
  socket-to-world command queues and per-player state.
- [cec5730](https://github.com/Shoro2/mod-ai-controller/commit/cec5730):
  malformed numeric input, headless teleport acknowledgments and bot-only control fixes.
- [ac-share, revision 732d7d7](https://github.com/Shoro2/ac-share/tree/732d7d7).
- [22e1881](https://github.com/Shoro2/ac-share/commit/22e1881): shared observations/rewards and loss of events when
  retaining only the latest streamed snapshot.

The checked upstream module fix lists **Shoro2 <info@ltdstats.com>** as its author. Preserve that attribution when
committing this adaptation of the mechanisms, using the repository's upstream-author rules.
If copying additional provider implementation code, inspect its own history and
retain its author/license as well.

This implementation uses new request-correlated observations, cumulative event totals, bounded asynchronous I/O
and a provider ownership contract. It does not vendor the example's session-spawning code, reward rules, spell
tables or Python combat simulator.

## Playerbots adapter

The concrete provider is [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots/tree/b949b50), inspected at
`b949b50bfcdd4fab937781bac2d7765e39330e4b`. The adapter calls its `GetPlayerbotAI`, `IsSelfBot`, `Reset` and
`HandleTeleportAck` APIs. The bundled patch adds external control to its existing AI, command, packet and scheduler
paths; existing GPLv2-or-later file notices remain in place. Original contributors are listed in
[Playerbots AUTHORS](https://github.com/mod-playerbots/mod-playerbots/blob/b949b50/AUTHORS.md).

Headless teleport handling is delegated to Playerbots, rather than copying Shoro2's implementation. The supported
core is [the Playerbot branch](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot); the local stock core
is used for source preparation only. No Shoro2 provider code is installed or required by this integration.
