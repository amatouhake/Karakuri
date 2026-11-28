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

- Bots you can spawn and give jobs from commands: go somewhere, mine, place,
  craft, smelt, use chests, pick up items, eat and attack, with reflexes that
  fight back and eat on their own.
  Going somewhere uses Karakuri's own pathfinder, which digs through walls,
  bridges gaps and pillars up with blocks from the hotbar. What
  was seen working on a live server, and what was not checked yet, is in
  [docs/VALIDATION.md](docs/VALIDATION.md).
- [Design notes](docs/DESIGN.md): goals, the layered AI architecture, the bot
  API, and the plan.
- [Spike 0](docs/SPIKE-0.md): what BDS's `SimulatedPlayer` does on a live
  server, which shaped the design. Its probe code is in [`spike/`](spike).

- An MCP server inside the mod, so AI agents such as Claude Code can drive the
  bots (see [Agents (MCP)](#agents-mcp)).

Not there yet: swimming, choosing a weapon before a fight, fleeing at low
health, and running scripts in one call (`run_script`).

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

`/karakuri` (alias `/kr`), operator only. Bot names are registered for command completion (not yet checked in a client).

| Command | What it does |
|---|---|
| `kr spawn <name> [pos]` | Spawn a survival bot (at your position when run by a player) |
| `kr remove <bot>` / `kr list` | Remove a bot / list bots |
| `kr status <bot>` | Position, health, hunger, current job, last result, inventory |
| `kr goto <bot> <pos>` | Go to a position, digging, bridging and pillaring when needed (scaffold blocks such as dirt or cobblestone must be in the hotbar) |
| `kr mine <bot> <pos>` | Walk within reach and break a block with the best tool in the hotbar |
| `kr place <bot> <pos>` | Walk within reach and place the held block |
| `kr hold <bot> <slot>` | Select a hotbar slot (0–8) |
| `kr eat <bot>` | Eat food from the hotbar |
| `kr attack <bot>` | Attack the nearest monster |
| `kr collect <bot>` | Walk over dropped items nearby to pick them up |
| `kr craft <bot> <item> [count]` | Craft from the inventory with the game's recipes; 3x3 recipes use a crafting table nearby (or place one from the hotbar) |
| `kr smelt <bot> <item> [count]` | Smelt in a furnace nearby (or place one), with fuel from the inventory |
| `kr chest <bot> put\|take <item\|all> [count]` | Move items between the inventory and the nearest chest |
| `kr stop <bot>` | Cancel the current job |
| `kr reflexes <bot> on\|off` | Turn the automatic fight and eat reflexes on or off |
| `kr debug ...` | Developer probes (block classification, movement primitives) |

Jobs run in the background; their results go to the server log and to
`kr status`. A dead bot respawns on its own after one second.

## Agents (MCP)

The mod runs an MCP server (Streamable HTTP) on `http://127.0.0.1:47821/mcp`,
reachable from the same machine only. Change the address or turn it off in
`mods/Karakuri/config/config.json`, which is created on first start:

```json
{ "mcp": { "enabled": true, "host": "127.0.0.1", "port": 47821 } }
```

This repository's [`.mcp.json`](.mcp.json) points Claude Code at it; from
another folder:

```powershell
claude mcp add --transport http karakuri http://127.0.0.1:47821/mcp
```

Tools: `list_bots`, `spawn_bot`, `remove_bot`, `bot_status`, `goto`, `mine`,
`place`, `hold`, `craft`, `smelt`, `chest_put`, `chest_take`, `collect_items`,
`eat`, `attack`, `stop`, `set_reflexes`, `find_blocks`, `nearby_entities`,
`get_block`, `wait_for_event`.

Jobs run in the background. Pass `wait_seconds` to a job tool to get its
result in the same call, or use `wait_for_event` with the returned
`event_cursor` to hear about job results, reflexes, damage and deaths.

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
