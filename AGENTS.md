# Karakuri agent guide

Karakuri is a server-side LeviLamina mod (Minecraft Bedrock Dedicated Server,
Windows x64, C++20, clang-cl, xmake) that adds bots meant to be driven by AI
agents. This file is the working manual for coding agents.

## Where things are

- [docs/DESIGN.md](docs/DESIGN.md): goals, architecture, decisions, open
  questions. Read it before proposing structure.
- [docs/SPIKE-0.md](docs/SPIKE-0.md): what `SimulatedPlayer` was seen to do
  on a live server.
- [docs/VALIDATION.md](docs/VALIDATION.md): what the mod was seen to do, per
  build. A server result goes there with the DLL hash.
- `src/core`: pure logic (jobs, reflex arbitration, the pathfinder search)
  without game types, tested in `tests/` (the pathfinder on ASCII maps).
- `src/mod`: game glue: `BotManager` (per-tick driver), `Jobs` and `PathJob`
  (L1 skills, path execution), `World` (block classification for the
  pathfinder), `Commands`, `Debug` (`/kr debug` probes), `Game` (helpers).
- `spike/`: the feasibility probe mod. Throwaway code with its own
  `xmake.lua`.
- `tools/bds_driver.py`: runs a test server and forwards console commands
  from a file.
- Machine-specific paths: `AGENTS.local.md` if present (gitignored; never
  copy it into tracked files).

## Talking to the user

- Reply in the user's language. Code, comments, commit messages and repo
  docs are English.
- Do not claim runtime behavior nobody has seen. "It builds" is not "it
  works on the server". A result seen on the server goes into the docs with
  the build it was seen on.
- Decisions made in chat that affect the design go into DESIGN.md as the
  current state (rewrite the passage, do not append dated rounds).

## Build

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"   # LLVM 22 is required
xmake f -p windows -a x64 -m release -y
xmake build Karakuri                                 # -> bin/Karakuri/
xmake build KarakuriTests; xmake run KarakuriTests   # must print "all tests passed"
```

- Put logic that can be a pure function in `src/core` with a test first.
- Never keep `SimulatedPlayer*` or other game pointers across ticks; keep
  names, ids or positions and resolve them each tick.
- Confirm a skill's result from world state; some `simulate*` calls report
  success when nothing happened.

- Game types are in the SDK headers in the xmake package cache
  (`%LOCALAPPDATA%/.xmake/packages/l/levilamina/<version>/*/include/mc/`).
  Search there before guessing a name.
- Build notes and known environment problems: SPIKE-0.md, "Build notes".

## Rules

- Same rules as players (DESIGN.md 8.1): direct inventory manipulation is
  fine, but recipes, reach, capacity, break times and hunger are respected.
  Rule-breaking operations are admin-only debug commands.
- Never copy code from other mods. CFSP is AGPL-3.0 and Karakuri is
  LGPL-3.0: take ideas only.
- Run work that touches game state on the server thread.

## Git

- One logical change per commit. Subject: English, imperative, sentence
  case, about 70 characters.
- Work on a branch and do not push unless the user asks.
