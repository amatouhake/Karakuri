# Karakuri

Server-side bots for Minecraft Bedrock that are built to be driven by AI agents.
A [LeviLamina](https://github.com/LiteLDev/LeviLamina) mod for Bedrock Dedicated Server (BDS).

> [!NOTE]
> Not an official LeviMC product. Not approved by LeviMC and not associated
> with LeviMC. NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR
> ASSOCIATED WITH MOJANG OR MICROSOFT. Karakuri is an independent community
> project, published and maintained by amatouhake.

## Status

**Early design stage. There is nothing to install yet.**

What exists today:

- [Design notes](docs/DESIGN.md): goals, the layered AI architecture, the bot
  API, and the plan for the first prototype.
- [Spike 0](docs/SPIKE-0.md): a feasibility probe that spawned one
  `SimulatedPlayer` on a live LeviLamina server and checked survival basics
  (damage, death, respawn, hunger, eating, sleeping, break times, pickup,
  navigation). Its code is in [`spike/`](spike).

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

## Platform

- LeviLamina 26.60.x on BDS 1.26.60.6, Windows x64.
- Bots are BDS `SimulatedPlayer` instances, exposed by LeviLamina.

## Building the spike

Requirements: Windows x64, Visual Studio 2022 (MSVC and Windows SDK),
[xmake](https://xmake.io), and **LLVM 22** (`clang-cl`). The clang bundled
with Visual Studio is too old to build the LeviLamina server SDK.

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
cd spike
xmake f -p windows -a x64 -m release -y
xmake build KarakuriSpike
```

The first configure builds the LeviLamina SDK from source and takes a while.
Copy `bin/KarakuriSpike/` into the `mods/` folder of a LeviLamina server.
On the server console, `kspike spawn <x> <y> <z>` spawns a bot named `kbot`;
see `handle()` in [`spike/src/Mod.cpp`](spike/src/Mod.cpp) for the other
actions. The spike is a probe, not a usable feature.

## Contributing

The project is still taking shape and is not ready for outside code
contributions. Issues with questions, ideas or pointers to prior work are
welcome.

## License

Copyright (C) 2026 amatouhake.

Karakuri is licensed under the GNU Lesser General Public License, version 3
only (`LGPL-3.0-only`); see [COPYING.LESSER](COPYING.LESSER) and
[COPYING](COPYING).
