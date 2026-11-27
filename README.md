# Karakuri

Server-side bots for Minecraft Bedrock that are built to be driven by AI agents.
A [LeviLamina](https://github.com/LiteLDev/LeviLamina) mod for Bedrock Dedicated Server (BDS).

> [!NOTE]
> Not an official LeviMC product. Not approved by LeviMC and not associated
> with LeviMC. NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR
> ASSOCIATED WITH MOJANG OR MICROSOFT. Karakuri is an independent community
> project, published and maintained by amatouhake.

## Status

**Early prototype. No release yet; expect everything to change.**

What exists today:

- Bots you can spawn and give simple jobs from commands: walk, mine, place,
  eat and attack, with reflexes that fight back and eat on their own. What
  was seen working on a live server, and what was not checked yet, is in
  [docs/VALIDATION.md](docs/VALIDATION.md).
- [Design notes](docs/DESIGN.md): goals, the layered AI architecture, the bot
  API, and the plan.
- [Spike 0](docs/SPIKE-0.md): what BDS's `SimulatedPlayer` does on a live
  server, which shaped the design. Its probe code is in [`spike/`](spike).

Not there yet: a pathfinder that digs and builds (the engine's navigation only
walks short distances), crafting, furnaces, chests, and the agent interface
(MCP).

## What it aims to be

- **Bots that play survival**: gathering, crafting, mining, farming, combat
  and helping players, under the same rules as players (recipes, break times,
  reach, hunger). Creative mode is out of scope.
- **Agent-first**: an API that AI agents can call, exposed over MCP, with
  short scripts (`run_script`) instead of one tool call per action.
- **Layered AI**: fast hand-written reflexes and skills at 20 Hz underneath,
  planners above them, and an LLM on top that sets goals. If the LLM is slow
  or offline, the bot keeps working.
- **Usable by hand too**: commands with proper completion.
- **Optional client mod** (later) on LeviLamina Client for a better UI.

The intended users are server operators and their players.

## Commands

`/karakuri` (alias `/kr`), operator only. Bot names complete in the chat box.

| Command | What it does |
|---|---|
| `kr spawn <name> [pos]` | Spawn a survival bot (at your position when run by a player) |
| `kr remove <bot>` / `kr list` | Remove a bot / list bots |
| `kr status <bot>` | Position, health, hunger, current job, last result, inventory |
| `kr goto <bot> <pos>` | Walk to a position (walking only, for now) |
| `kr mine <bot> <pos>` | Break a block within reach, with whatever the bot holds |
| `kr place <bot> <pos>` | Place the held block at a position within reach |
| `kr hold <bot> <slot>` | Select a hotbar slot (0–8) |
| `kr eat <bot>` | Eat food from the hotbar |
| `kr attack <bot>` | Attack the nearest monster |
| `kr stop <bot>` | Cancel the current job |
| `kr reflexes <bot> on\|off` | Turn the automatic fight and eat reflexes on or off |

Jobs run in the background; their results go to the server log and to
`kr status`. A dead bot respawns on its own after one second.

## Platform

- LeviLamina 26.60.x on BDS 1.26.60.6, Windows x64.
- Bots are BDS `SimulatedPlayer` instances, exposed by LeviLamina.

## Building

Requirements: Windows x64, Visual Studio 2022 (MSVC and Windows SDK),
[xmake](https://xmake.io), and **LLVM 22** (`clang-cl`). The clang bundled
with Visual Studio is too old to build the LeviLamina server SDK.

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
xmake f -p windows -a x64 -m release -y
xmake build Karakuri
xmake build KarakuriTests; xmake run KarakuriTests
```

The first configure builds the LeviLamina SDK from source and takes a while.
Copy `bin/Karakuri/` into the `mods/` folder of a LeviLamina server.

For testing without a client, [`tools/bds_driver.py`](tools/bds_driver.py)
runs the server and forwards lines appended to a file to its console.

## Contributing

The project is still taking shape and is not ready for outside code
contributions. Issues with questions, ideas or pointers to prior work are
welcome.

## License

Copyright (C) 2026 amatouhake.

Karakuri is licensed under the GNU Lesser General Public License, version 3
only (`LGPL-3.0-only`); see [COPYING.LESSER](COPYING.LESSER) and
[COPYING](COPYING).
