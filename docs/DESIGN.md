# Karakuri design notes (draft)

- Started: 2026-11-27
- Status: early prototype. The feasibility spike is done ([SPIKE-0.md](SPIKE-0.md)); the mod has a bot manager, the first L1 skills and two L0 reflexes ([VALIDATION.md](VALIDATION.md)).

## 1. In one sentence

A server-side LeviLamina mod that adds AI agents and bots that play Minecraft Bedrock survival. A client-side mod (LeviLamina Client) is optional.

## 2. Background

The project grew out of frustrations with building and using Endbot, a fake-player plugin on Endstone:

- Command completion is weak.
- Menus that plugins can add are hard to read.
- The bot should be able to handle complex work in principle, but writing scripts for it is tedious, whether in game or on the web.

Endbot aimed for stable operation. Karakuri also aims to be actually usable, but it is more experimental.

## 3. Goals and non-goals

**Goals**

- Design from the start for bots driven by AI agents.
- Cover survival work in general: gathering, crafting, terraforming, farming, combat, helping players.
- Provide the groundwork needed to solve the kinds of tasks players are likely to hand out.
- Allow a reasonable amount of manual control (commands with completion, forms).

**Non-goals**

- Creative mode. If you want creative-style building, it makes more sense for an agent to edit the world directly on the server than to go through a player.
- Permission design and operational polish. These wait until we know how practical the bots are.

**Intended users**: server operators and their players.

## 4. Platform

### 4.1 Why LeviLamina

| | Endstone | LeviLamina |
|---|---|---|
| How it gets in | Injects a DLL at startup (`LD_PRELOAD` on Linux) | Patches `bedrock_server.exe` with PeEditor into `bedrock_server_mod.exe`, which loads PreLoader |
| How it finds functions | Its own table of byte signatures (82 entries for BDS 1.26.65) | Generated BDS internal headers plus per-build `bedrock-runtime-data` (closed source), resolved by prelink and PreLoader |
| What plugins can touch | Only a Bukkit-like public API | BDS internal classes almost as-is; direct hooks are possible |
| Languages and OS | Python / C++; native on Windows and Linux | C++20; native on Windows (Linux via Wine) |
| Latest (2026-11) | v0.11.16 (BDS 1.26.65) | v26.60.9 (BDS 1.26.60.6) |

To build a detailed fake player, direct access to BDS internals makes LeviLamina the better fit. The costs we accept:

- Tight coupling to a specific BDS build.
- Dependence on the closed-source PreLoader and `bedrock-runtime-data`.
- Being Windows-centric.

LeviLamina's command registration supports dynamic candidates (soft enums, e.g. bot names) and typed arguments, so completion can be done well.

**Target version**: the BDS supported by the latest LeviLamina (currently 26.60.x and BDS 1.26.60.6).

### 4.2 The fake-player base: `SimulatedPlayer`

LeviLamina exposes BDS's `SimulatedPlayer` (derived from `ServerPlayer`).

- **Creation**: `SimulatedPlayer::create(name, pos, dim, rot)`
- **Actions**: `simulateDestroyBlock`, `simulateAttack`, `simulateUseItem`, `simulateInteract`, `simulateLocalMove` / `simulateWorldMove`, `simulateMoveToLocation`, `simulateNavigateToLocation`, `simulateLookAt`, `simulateJump`, `simulateRespawn`, `simulateChat`, and more.

**Known constraints**

- The constructor and tick processing (`preAiStep` / `postAiStep`, `_updateMovement`, etc.) are not callable (`MCNAPI`) and cannot be replaced.
- Attacking and using items have a 10-tick cooldown.
- There is no API equivalent to operating container GUIs; inventories have to be edited directly.
- Chunk loading seems to differ from real players (it overrides `_createChunkSource` and others). In practice, chunks, physics and mob spawning around the bot kept running with no real player online.
- Confirmed in a live run (2026-11-28, [SPIKE-0.md](SPIKE-0.md)):
  - Taking damage, death, regeneration, hunger, eating, sleeping, item pickup and tool-dependent break times all behave as in survival.
  - The respawn point is the spawn position, so the mod must manage it.
  - Reach is not enforced for simulated players, so skills enforce it (8.1).
  - Some `simulate*` calls report success when nothing happened, so skills confirm results in the world.
  - `simulateNavigateToLocation` only walks, and finds no path to targets more than 16 blocks away (measured on flat ground). It does not dig, pillar up or bridge. Karakuri plans its own paths and moves with `simulateMoveToLocation` plus jumps.
- No systematic list of behavioral differences from real players (achievements and so on) has been found.

**Existing implementation worth studying**: [CoralFans-Dev/CFSP](https://github.com/CoralFans-Dev/CFSP) (AGPL-3.0, supports 26.60.*, maintained)

- Centered on manual control through the `/sp` command and GUI forms.
- Fixes the held-item display by sending `MobEquipmentPacket` manually.
- Has fixed bugs with breaking liquid blocks and with load range.
- Has an open issue: the bot becomes invisible after its first death.
- Karakuri is LGPL-3.0, so CFSP's AGPL code is not copied. Only ideas are taken from it.

**How Karakuri differs from CFSP**: an agent-facing API, a skill layer and a reflex layer.

## 5. AI architecture

### 5.1 Basic approach

Stack layers that run at different speeds. If a slow layer (the LLM) stalls, the fast layers keep running.

Minecraft agents (Mindcraft, Voyager, Project Sid) and other fields (three-layer robot architectures, System 1/2 style VLAs, game AI, PUBG Ally) have converged on roughly the same shape.

### 5.2 Layers

| Layer | Rate | Implementation | Role |
|---|---|---|---|
| L0 reflex | Every tick (20 Hz) | Fixed code with priorities | Escape lava, fire, falls, suffocation, falling blocks, low HP. Can always override the layers above |
| L1 execution | Every tick | Behavior tree | Run skills (move, mine, place, craft, smelt, eat, chest work, combat, follow). Pathfinding runs as an async node, with recovery branches for getting stuck |
| L2 selection | 2–5 Hz | Utility AI | Arbitrate between fight, flee, eat, sleep and keep working. Hysteresis prevents flip-flopping |
| L3 planning | Per event, every few seconds | HTN / GOAP | Break goals (e.g. an iron pickaxe) into skill sequences, checked against preconditions and effects. Crafting dependencies are solved exactly |
| L4 small model (optional) | About 1 Hz | 1–4B language model, structured output | Routine chat, mapping vague instructions to goals, choosing decomposition rules |
| L5 LLM | Tens of seconds to minutes, per event | Large LLM | Talking with people, long-term goals, generating code for new skills and HTN rules, reflecting on failures |

### 5.3 "Small model" means two different things

- **Small language model (L4)**: fast replies for chat and intent parsing. Precedent: PUBG Ally (a behavior tree for reflexes, a roughly 2B SLM for chat and intent). Once logs accumulate, distill it from a larger model.
- **Small control model**: only continuous control, such as combat spacing, bow aim and shield timing, leaves room to beat hand-written code. Postponed, in this order: collect logs with hand-written rules → behavior cloning → small-scale reinforcement learning. Avoiding lava and falls, pathfinding and eating are fine hand-written.

### 5.4 Keep the decision makers swappable

We do not decide up front whether to include a general decision model (a single learned model between the LLM and the reflexes that decides "what to do now"):

- Integrated models such as Optimus-3, JARVIS-VLA and SIMA assume large datasets, pixel input and Java Edition. They cannot be used as they are.
- Karakuri has no training data and no evaluation task suite yet.
- The server knows the exact state, so hand-written L2 and L3 can already be quite accurate.

Instead, three things are prepared from the start:

1. **Decision logs**: record a state summary, which layer and decider made the decision, the chosen action, and the outcome (success or failure, damage, time taken). These logs are the material for distillation and learning.
2. **Evaluation tasks**: tasks that can be scored automatically, such as "collect 16 logs", "survive the night" and "make an iron pickaxe". They measure whether replacing a rule with a model is worth it. A resettable world (e.g. copied from a template) makes every run start from the same conditions.
3. **A common decider interface (later)**: each layer's decider takes "input = state summary, output = chosen action plus confidence", so a rule, a small model or an LLM can be plugged in. This is not abstracted until a second implementation appears in the same layer, to avoid premature abstraction.

If learned models are added, start with narrow specialist models, not a general decision model. Order to try:

1. **Distilling skill and goal selection**: train a small model to imitate the LLM's decision logs (TinyAgent style). The logs accumulate naturally, so this can be tried first.
2. **Combat fine control**: spacing, aim, shield. The reward is clear.

### 5.5 Contracts between layers

**Passed down**

- L5 → L3: `Goal{type, params, priority, deadline, constraints}`, plus decomposition rules or skill code when needed.
- L5 does not drive L1 or L0 directly.
- Generated code is not registered in the skill library until it passes validation (types, preconditions, a run in a test world).

**Passed up**

- `Event{kind, severity, snapshot}`
- Example kinds: `plan_failed`, `goal_done`, `threat`, `player_chat`, `stuck`, `damaged`, `health_low`, `death`, `inventory_changed`
- The snapshot is a summarized state (position, HP, inventory changes, nearby mobs).

**Interrupt rules**

1. L0 can interrupt at any time. Afterwards it either resumes the interrupted skill or reports failure upward.
2. L2 switching has hysteresis.
3. LLM responses arrive asynchronously as plan updates. They are applied at a tick boundary, and only if they pass L2/L3 validation.
4. While waiting for the LLM, the current goal continues.
5. An LLM timeout never stops the bot.

### 5.6 Ideas to adopt

| Idea | Source | How Karakuri uses it |
|---|---|---|
| Separate talking from acting | Cicero | Chat replies are conditioned on "current goal and state"; chat never decides actions directly |
| Actions as code | CodeAct, Voyager, Code as Policies | The LLM writes short scripts that call the skill API. Validated scripts are saved and reused. This is the answer to "scripting is tedious" |
| Offer only what is feasible | SayCan | Show the LLM only skills whose preconditions hold (the server knows the exact state, so no learning is needed) |
| Tell the LLM about interruptions | Mindcraft | When a reflex interrupts, notify the LLM so it can re-decide. Resume the original work when idle |
| Plan and act in parallel | Parallelized Planning-Acting, Speculative Actions | The game never pauses while the LLM is thinking |
| Let the LLM only state goals | LLM+P | The LLM specifies goals; the planner does the exact decomposition |

## 6. Bot API primitives

**Observation**

- Self: HP, hunger, air, burning, fall speed, effects, equipment, inventory.
- Surroundings: block search by type and radius, entity list (hostile or not, distance, line of sight, reachable or not), light level, biome, time.
- The LLM gets query-style summaries.

**Actions**

- Low level: movement input, look, jump, sneak, sprint, attack, use, slot selection.
- High level: path movement (goal types, with settings for digging, scaffolding and acceptable falls), mining a given block, placing, crafting, smelting, equipping, eating, chest work, following, attacking, fleeing.
- All are cancellable async jobs that return completion or a failure reason.

**Events**: see `Event` in 5.5.

**Skill persistence**: saving, searching and versioning named scripts; execution logs; a sandbox; time limits.

**External interface**: MCP (or WebSocket).

- Whether the server lives inside the BDS process or in a separate sidecar process is open.
- Either way, calls are queued and run on the game thread.
- It listens on localhost only.
- MCP is request/response, so events pushed by the server may not reach Claude Code. If they do not, a blocking tool such as `wait_for_event` fills the gap; interruptions are then noticed on the next call.

**Script execution**: an agent calling one tool per action is too slow to judge practicality. Provide `run_script`, which runs a short script calling the skill API in one go. This needs an embedded language and a sandbox (Endbot used Luau).

## 7. Scope of the first prototype

0. **Feasibility check (spike)**: done; results in [SPIKE-0.md](SPIKE-0.md). Crafting and furnaces have no API and were not checked. One `SimulatedPlayer` was spawned on 26.60.x to check:
   - Which APIs actually exist in the target version's `SimulatedPlayer.h`.
   - Whether nearby chunks and mobs keep running with no real player close by.
   - Whether mobs target the bot, and whether damage, death and respawn work.
   - Whether it picks up items, gets hungry, can eat and can sleep.
   - Whether breaking blocks takes survival-equivalent time and reflects the tool.
   - How crafting, furnaces and chests can be handled.
   - Whether `simulateNavigateToLocation` can dig, pillar up or bridge.

   These results decide how much of chapter 5 can be built.
1. A base that creates and drives `SimulatedPlayer` on LeviLamina. **Done**: `BotManager` spawns and removes named bots, pins respawn points, respawns dead bots, and runs one per-tick driver (observe → L0 → job step). Game pointers are never kept across ticks.
2. L0 reflexes and the basic L1 skills (move, mine, place, craft, eat, chest work).
   - **Done**: jobs `goto` (engine navigation), `mine`, `place`, `eat`, `attack`, each cancellable and ending in `done` or a failure reason. Reflexes `fight` (when hurt with a monster near) and `eat` (when hungry) pause the job and resume it afterwards. Job bookkeeping and reflex arbitration are pure headers in `src/core` with unit tests.
   - **Done**: a pathfinder (`src/core/Pathfinder.h`): resumable A* over a block-query interface with walk, step up, drop (up to 3), dig through, bridge, pillar and dig down; no digging next to liquids or under falling blocks; scaffold blocks come from the hotbar. `goto` uses it and executes one step at a time, confirming each step and replanning on mismatch; `mine` and `place` walk within reach first.
   - **Next**: crafting, smelting and chest work by direct manipulation (8.1), weapon choice and a low-health reflex, swimming, and moving `attack`'s chase onto the pathfinder.
3. Expose this as MCP and use Claude Code itself as the first agent. No LLM inside the mod: the fastest way to see how practical it is.
   - This is a prototype exception: Claude Code (in the L5 role) calls L1 skills directly. The 5.5 rule "L5 does not drive L1 directly" applies once L3 exists.
   - `run_script` and event waiting are built here too.
4. Manual control through commands with completion. **Started**: `/karakuri` (`/kr`) has typed subcommands and bot-name completion; it grows with the skills.

**Added afterwards**

- L2 Utility AI
- L3 HTN/GOAP
- Skill library
- Cicero-style chat
- Calling an LLM from inside the mod
- Client mod (built on LeviLamina Client, like Lamium. Server-side forms stay minimal; players with the mod get a dedicated UI)

**Postponed**

- Distillation into small models
- Behavior cloning and reinforcement learning for combat
- Data generation through self-play

## 8. Decisions (2026-11-27)

### 8.1 Same rules as players; direct means are allowed

"Same rules" and "same inputs" are treated separately.

- **Kept (rules)**: recipes and material use, crafting tables and furnaces within reach, break times and tool differences, reach distance, inventory capacity, hunger.
- **May be direct (means)**: container GUI slot clicks need not be imitated; inventories can be moved server-side. Rules such as "only nearby containers that can be opened" and "move only what fits" are still checked.
- **Rule-breaking operations**: giving items, teleporting and the like are separate debug commands for administrators only.

Three reasons:

- Breaking the rules would make it no different from creative mode, defeating the point of targeting survival.
- It stays fair to other players in multiplayer.
- Evaluations and training data keep their meaning in the real game.

### 8.2 Development and prototyping on native Windows

- Development and prototyping run on native Windows. Running on Linux is checked later with Wine.
- Parts that do not depend on BDS (planner, script execution, MCP) are written OS-independently, to ease a move if a native Linux LeviLamina appears.

### 8.3 Representative tasks (core of the evaluation suite)

| # | Task | Mainly exercises | Pass condition |
|---|---|---|---|
| A | Make an iron pickaxe from a fresh world | Planning, crafting chains, mining, furnace, pathfinding | Obtained within a time limit (e.g. two in-game days) without dying |
| B | Survive a night while working (e.g. while gathering wood) | Reflexes, combat, interrupting and resuming work | Alive in the morning, and the work has properly resumed |
| C | Deliver resources requested in chat to a chest (e.g. "two stacks of cobblestone in this chest") | Dialogue, instruction parsing, containers, long tasks | The given chest holds the given amount |

- Spike 0 showed the body mechanics these tasks need work. Crafting, furnaces and container transfer must be built in the mod (8.1).
- The order of the first skills is decided by what these three tasks need.

## 9. Open questions

- Where the MCP server lives (inside the BDS process or a sidecar).
- The embedded language and sandbox for `run_script` (Luau, JS, Lua, ...).
- How fine-grained MCP tools should be (how much low-level control to expose).
- API keys and cost if the mod calls an LLM itself.
- Remaining behavioral differences from real players (achievements, statistics, the invisibility-after-death issue seen in CFSP).
- When Mojang removed symbols from BDS (no primary source found; does not affect the implementation).

## 10. References

**Platform**

- LeviLamina: https://github.com/LiteLDev/LeviLamina , https://levimc.org/LeviLamina/
- PreLoader: https://github.com/LiteLDev/PreLoader
- bedrock-runtime-data: https://github.com/LiteLDev/bedrock-runtime-data
- Endstone: https://github.com/EndstoneMC/endstone
- CFSP: https://github.com/CoralFans-Dev/CFSP
- endstone-bot: https://github.com/ReallocAll/endstone-bot
- bedboo: https://levimc.org/bedboo/
- Script API SimulatedPlayer: https://learn.microsoft.com/en-us/minecraft/creator/scriptapi/minecraft/server-gametest/simulatedplayer

**Minecraft AI agents**

- Mindcraft: https://github.com/kolbytn/mindcraft
- MineCollab: https://arxiv.org/abs/2504.17950
- Voyager: https://arxiv.org/abs/2305.16291
- Odyssey: https://arxiv.org/abs/2407.15325
- Project Sid: https://arxiv.org/abs/2411.00114
- Parallelized Planning-Acting: https://arxiv.org/abs/2503.03505
- mineflayer-pathfinder: https://github.com/PrismarineJS/mineflayer-pathfinder
- Baritone: https://github.com/cabaletta/baritone
- GITM: https://github.com/OpenGVLab/GITM
- JARVIS-1: https://arxiv.org/abs/2311.05997
- Optimus-1: https://arxiv.org/abs/2408.03615
- STEVE-1: https://arxiv.org/abs/2306.00937
- VPT: https://arxiv.org/abs/2206.11795
- minecraft-mcp-server: https://github.com/yuniko-software/minecraft-mcp-server

**Other fields**

- Figure Helix: https://www.figure.ai/news/helix
- GR00T N1: https://arxiv.org/abs/2503.14734
- π0.5: https://arxiv.org/abs/2504.16054
- Gemini Robotics 1.5: https://deepmind.google/blog/gemini-robotics-15-brings-ai-agents-into-the-physical-world/
- SayCan: https://say-can.github.io
- Code as Policies: https://arxiv.org/abs/2209.07753
- Nav2 BT Navigator: https://docs.nav2.org/configuration/packages/configuring-bt-navigator.html
- Three-layer architecture (Gat): https://u.cs.biu.ac.il/~galk/teach/current/intsys/readings/on-three-layer-arch-tla-1998.pdf
- GOAP in F.E.A.R.: https://pages.cs.wisc.edu/~dyer/cs540/handouts/gdc2006_orkin_jeff_fear.pdf
- HTN in Decima: https://guerrilla-games.com/read/htn-planning-in-decima
- PUBG Ally (NVIDIA ACE): https://developer.nvidia.com/blog/how-krafton-built-pubg-ally-a-co-playable-character-powered-by-nvidia-ace/
- SIMA 2: https://arxiv.org/abs/2512.04797
- Cradle: https://arxiv.org/abs/2403.03186
- OpenAI Five: https://cdn.openai.com/dota-2.pdf
- Cicero: https://noambrown.github.io/papers/22-Science-Diplomacy.pdf
- CodeAct: https://arxiv.org/abs/2402.01030
- LLM+P: https://arxiv.org/abs/2304.11477
- Speculative Actions: https://arxiv.org/abs/2510.04371
- TinyAgent: https://arxiv.org/abs/2409.00608
