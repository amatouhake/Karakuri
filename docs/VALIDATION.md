# Validation

What the Karakuri mod was seen to do on a live server. "Builds and tests pass" is not listed
here; only behavior read from the server log is. Raw logs are not kept in the repository.

## Setup

- LeviLamina 26.60.7 / BDS 1.26.60.6 / Windows 11, survival, normal difficulty.
- No real client connected: everything was driven from the server console with
  [`tools/bds_driver.py`](../tools/bds_driver.py).

## 2026-11-28: surviving the night

Build: `c7440a3`, `Karakuri.dll` SHA-256 `86936a03ab42cf4e291c5b3e6aad4dcbc5ed03493870c0c7fd6bdb83a08569cd`
(some rows were seen on development builds just before it, as noted); the last table is on the
next commit's build (SHA-256 `627decc8c4abb71d75c21550b098fe1620174fcbb7446259cf9a7a008f1dfbe4`).

| Case | Seen |
|---|---|
| `mine` iron ore with only a wooden pickaxe | `failed: minecraft:iron_ore needs a better tool to drop anything`; with a stone pickaxe `done`, and `collect_items` got the raw iron |
| Fight reflex with a pickaxe held and a sword in the hotbar | Switched to the sword, killed a husk in 3 hits |
| Flee reflex (development build) | At 6 HP with two husks, fled about 40 blocks while health recovered. Died twice: once to hits during knockback, once in deep water (a husk followed it in, then drowning) |
| Rising in deep water (`/kr debug rise*`) | Jumping bobs and sinks; moving straight up rises about 1.5 blocks per second |
| Idle in deep water | Surfaced from y=55 and floated 20 s without damage |
| `goto` out of deep water to the shore | 11 swim steps, out over the bank, `done`, no damage |
| `sleep` by day | `failed: beds only work at night` |
| `sleep` at night | Walked 19 steps to a bed and slept; the `day` event followed. With a bed only in the inventory it placed one and slept |
| `shelter` at night, then a zombie summoned next to it | 10 cobblestone walls and roof; the bot stayed at 20 HP for 15 s |
| `night` / `day` events | Arrive as world events (empty bot) and match any bot filter |

**Agent run: through a night on normal difficulty.** A headless Claude Code (karakuri tools only)
spawned a bot at dusk with an empty inventory and was told to keep it working and alive until
daybreak. The bot was alive at 20 HP when day broke, but **it died three times** during the night,
so representative task B is not passed yet:

1. A creeper and a witch caught it while `shelter` stalled; the flee reflex ran it about 50 blocks
   into more monsters and a second creeper killed it.
2. Right after respawning at spawn, among zombies, with reflexes turned off by the agent.
3. An arrow while walking away from zombies.

The bot survived the last third of the night in a 1x1 hole the agent dug and capped by hand.
Its feedback, and what was done:

| Feedback | Status |
|---|---|
| A job sent just before death ran on the respawned bot | Fixed: jobs are refused while the bot is dead (seen) |
| The fight reflex engaged zombies bare-handed | Fixed: without a weapon, the bot flees from 14 HP instead of 6 (unit tested) |
| `shelter` stalled silently | Fixed: it centers the bot first and finishes `done` when closed, or fails naming the gaps (seen: `sheltered at (12,68,12)`) |
| Some hurt events were missing (throttled to one per second) | Changed to two per second, and every damage cause is now named |
| Flee runs away from the nearest attacker into other monsters, with no notion of safe ground | Open |
| No "dig in and hold safe ground" job (what worked) | Open; candidate for the next round |
| `mine` walks far on its own and spends blocks while pathing | Open |
| `collect_items` sometimes stops 1.5 blocks short | Open |
| `find_blocks` limited to 16 blocks; no "any log" search | Open |
| Reading events from cursor 0 replays old night/day events | By design (use the returned cursor) |

## 2026-11-28: crafting, smelting and chests

Build: `f68b6fe`, `Karakuri.dll` SHA-256 `99d04bb0cfb2c5adde3b7c3e6347b201a3469897b5d734c8bf42e1a2e1be5712`,
except the last table, which is on the next commit's build (SHA-256
`77a8de8cd3adf67517371282e81368cafa1f38dfcfb3e722f17dbaccd453b3df`).

**Skills over MCP** (items given with console commands first):

| Case | Seen |
|---|---|
| `craft` oak_planks 8, stick 4, crafting_table | `done`; `inventory_change` -2 oak_log +8 oak_planks, -2 oak_planks +4 stick, -4 oak_planks +1 crafting_table |
| `craft` wooden_pickaxe with 2 planks | `failed: missing ingredients` (needs 3) |
| `craft` furnace (a 3x3 recipe) with no table nearby | Placed the crafting table from the hotbar, then crafted: -8 cobblestone, -1 crafting_table, +1 furnace |
| `smelt` raw_iron 3 | Placed the furnace from the hotbar, loaded 1 coal, took 3 iron ingots after about 32 s |
| `craft` iron_pickaxe | -3 iron_ingot, -2 stick, +1 iron_pickaxe |
| `chest_put` / `chest_take` | Put 2 planks into a chest and took 1 back; taking diamonds failed with `the chest has none` |

**Agent run: from nothing to an iron pickaxe.** A headless Claude Code (karakuri tools only,
peaceful, daylight cycle off, leftover stations near the origin removed first) was asked to spawn a
bot with an empty inventory and get an iron pickaxe. It succeeded in about 12.5 minutes of game
time with 68 jobs and no deaths: logs, planks, a crafting table, a wooden pickaxe, a shaft dug down
with `goto` to y=55, cobblestone, a stone pickaxe, coal ore, iron ore at y=44-45, a furnace, `smelt`,
and the iron pickaxe. Its feedback, and what was done about it:

| Feedback | Status |
|---|---|
| A furnace crafted with a full hotbar landed in slot 9 and could not be placed; working around it took about 15 calls | Fixed: station items are moved into the hotbar before placing (seen: a furnace in slot 9 was placed and used to smelt) |
| Stations were searched only 4 blocks up and down, though the tool says 16 | Fixed: a full 16-block cube |
| `wait_seconds` over the client's request timeout returned "operation timed out" with no job id | Fixed: waits are capped at 45 s, and the description points to `wait_for_event` |
| `place` said "nothing was placed" when the bot stood in the spot | Fixed: `the bot is standing in that spot` (seen) |
| `collect_items` timed out "after 145 items" while picking up one | Fixed wording: it counts pickup attempts |
| Two iron ores broke with no drop | Not resolved: possibly the wrong tool tier, possibly lost in nearby water. `mine` should report the tool and refuse blocks the tool cannot harvest |
| `goto` gave up on long digs through an aquifer with only "search limit reached" | Open: the search does not say water was the obstacle |
| Scaffolding silently uses planks and dirt the agent wanted to keep | Open |
| No "any log" search | Open |

## 2026-11-28: MCP server

Build: the commit that adds this section, `Karakuri.dll` SHA-256
`78b60de79a7446f96557baeb9dfb4bc5c5cf0936ebbca96dd0ba6ca6e58d669a`. Driven with a minimal JSON-RPC client over HTTP (the same requests an MCP client sends).

| Case | Seen |
|---|---|
| Start | `MCP server listening on http://127.0.0.1:47821/mcp`; `config/config.json` written with the defaults |
| `initialize`, `tools/list` | Protocol version `2025-06-18` negotiated; 17 tools listed with input schemas |
| Observation | `find_blocks` found oak logs 11.4 blocks away; `nearby_entities` listed an item and a sheep; `get_block` classified grass as solid; an unknown bot returned `isError` with `no bot named nobody` |
| Jobs with `wait_seconds` | `mine` on a log 11 blocks away walked within reach and returned `state: done` in the same call (about 6 s) |
| Jobs without waiting, then `wait_for_event` | `mine` returned `running` with a job id; `wait_for_event` returned its `job_finished` event; with nothing new it returned an empty list after the timeout |
| `collect_items` | Picked up dropped oak logs, a sapling, a stick and dirt (inventory checked with `bot_status`) |
| Shutdown | `stop` on the console shut the server down cleanly with the MCP server running (`Quit correctly`) |

**Agent run (same build).** A headless Claude Code (`claude -p` with this repository's `.mcp.json`,
only the karakuri tools allowed) was asked to spawn a bot, mine two oak logs, collect the drops and
report the log count. It called `spawn_bot`, `find_blocks`, `mine` twice and `collect_items` (all
with `wait_seconds`), then `bot_status`, and reported 2 oak logs; the server log matches (every job
`done`, no replans). It picked a tree trunk reachable from the ground over a nearer single log.
Its feedback on the tools:

- `collect_items` and `mine` do not say what was picked up; it needed `bot_status` to find out.
- `find_blocks` sorts by distance only and does not group logs into trees.
- Right after `spawn_bot` the bot is still falling into place (`on_ground: false`).

**Follow-up (next commit, `Karakuri.dll` tested: SHA-256 `613403b95988677b9bec03ef112c1f51a9b1e219ee86a315c1bebfa1040a4eaa`;
the commit only adds a sentence to the `mine` tool description).** Job results and `job_finished`
events now carry `inventory_change`. Seen over MCP: `collect_items` returned
`{"minecraft:oak_log": 1}` and its detail `inventory +1 oak_log`; `mine` returned an empty change
because the drop was still on the ground, which its description now says.

While collecting items in an earlier run, the bot dug through blocks to reach drops (the pathfinder
may break blocks on the way).

## 2026-11-28: pathfinder

Build: `8d00003`, `Karakuri.dll` SHA-256 `c339582e87c820ed8d8605cf7041a921e458ee2af549a485d97d985ea91d64c8`.
Test structures were rebuilt before the run (a stone strip at y=99, a 4-block gap, a sealed stone
box, a 3-high stone pillar).

| Case | Seen |
|---|---|
| Flat strip, 20 and 40 blocks (the engine's navigation failed beyond 16) | `done`, 0 replans; plans of 22 and 42 steps took 0.2 and 0.4 ms |
| 4-block gap on the strip, 16 cobblestone in the hotbar | Plan with 4 bridge steps; `done`, 0 replans; four cobblestone used |
| Top of the pillar (6,70,6) from the ground | Plan: walk 5, ascend 1, pillar 2; `done`, 0 replans |
| Inside the sealed box (1,67,-7) | Plan: pillar 1, 2 blocks broken, 1184 nodes in 20 ms; `done`, 0 replans |
| From the origin to (12,68,12) (the engine found no path from there) | `done`, 0 replans |
| Across the area where a bot once fell into a hole, to (0,67,27) | `done`, health stayed 20 |
| `mine` on a block 8 blocks away | Walked within reach (11 steps), then broke it |
| A husk attack during a `goto` | The fight reflex took over and killed it; the job replanned from where the bot stood and finished |

Limits seen in this run:

- On an earlier development build, a target in mid-air over a lake ((0,67,30)) used up the scaffold
  blocks on a partial path and then failed with `no path`; a plan that hit that build's 40000-node
  limit took about 1.6 s of server time, at up to 3000 nodes per tick. `8d00003` caps plans at 20000
  nodes and 800 nodes per tick; a long failing plan was not timed on it.
- The fight reflex fights with whatever is in hand (it killed the husk with a wooden pickaxe in 7
  hits while a sword was in the hotbar).
- `attack` still chases with the engine's navigation (16-block limit, does not avoid drops).
- Water is avoided, not swum through.

## 2026-11-28: bot manager, first skills and reflexes

Builds:

- `e1c56b7`, `Karakuri.dll` SHA-256 `3b1a84850239c4b2442b0973de2ce7e5f8cc0155c787ab5c5a82b8ea8884c848`: everything below except the last two rows.
- `1bb21b8`, `Karakuri.dll` SHA-256 `59fb36085f4a86bb6a966608ff45dfdaa520135b02c1490875384ba0528df81f`: disable and enable, navigation range, and a smoke test (spawn, `mine`, `goto`).

| Area | Seen | Not seen / limits |
|---|---|---|
| Spawn, list, remove | Two named bots spawned; a second `spawn` with an existing bot name was refused; `list` showed both and updated after `remove`. | Refusing the name of an online real player was not tried (no player was online). |
| Respawn | A bot spawned at y=120 died on landing, got its respawn point pinned at the landing spot (0,67,0), and respawned there instead of falling again. Bots spawned on the ground get the respawn point at their feet. | Respawning right into a fight with the killer is prevented in code but was not reproduced. |
| `goto` | Walked to targets up to 16 blocks away and reported `done`. Unreachable targets fail with a reason (`target unreachable on foot; stopped 2.3 blocks away`, `no path to target`). `stop` mid-walk cancelled it (`cancelled`). | See "Navigation range" below. |
| `mine` | Broke stone with a wooden pickaxe (`done: broke minecraft:stone`). Refused a block out of reach and an air block. `stop` mid-break (stone by hand) cancelled it and the stone stayed. | Tool choice is up to the caller (`hold`). |
| `place` | Placed cobblestone and confirmed it in the world. Refused an occupied spot and a spot with nothing to place against. | |
| `eat` | Explicit `eat` at hunger 0 ate one bread (`done: hunger 5`). With full hunger it failed (`did not finish eating`), since food cannot be eaten then. | |
| Eat reflex | Logged `none -> eat` at hunger 6 and ate bread until `eat -> none` at hunger 20. Another time it stopped at hunger 10 because the bread ran out. | |
| `attack` | With reflexes off, `attack` killed a husk in 3 hits with an iron sword. | |
| Fight reflex | When a husk hit the bot (`hurt 1 ticks ago`, threat near), the reflex took over, killed the husk in 3 hits, and ended after the calm period. | `stop` during a reflex was not observed: the fight ended before the command arrived. |
| Pause and resume | A `goto` was interrupted by the fight reflex, resumed after the fight, and finished (`done`). Seen on two builds. | |
| `hold`, `reflexes on/off`, `status` | Work as described in the README. | |
| Disable and enable (`1bb21b8`) | `ll disable Karakuri` removed the live bot (0 players online). While disabled, `/kr` answers `Command version mismatch`. After `ll enable Karakuri`, `/kr` works again and new bots spawn. | |
| Navigation range (`1bb21b8`) | On a flat stone strip, from two start points, `goto` succeeded at 8, 12 and 16 blocks and failed with `no path to target` at 17, 18, 19, 20, 24, 32 and 40 blocks. | Earlier, on natural terrain, some shorter trips also failed from particular start points; the cause is unknown. |

Also seen during development: chasing a mob, a bot walked into a hole and fell to 3 HP. The
engine's navigation does not avoid drops, and there is no low-health reflex yet.

**Not checked yet** (needs a real client): whether bots are visible and look right, whether the
held item shows correctly after `hold`, whether a bot is invisible after its first death (an open
CFSP issue), and whether bot names complete in the chat box.
