# Validation

What the Karakuri mod was seen to do on a live server. "Builds and tests pass" is not listed
here; only behavior read from the server log is. Raw logs are not kept in the repository.

## Setup

- LeviLamina 26.60.7 / BDS 1.26.60.6 / Windows 11, survival, normal difficulty.
- No real client connected: everything was driven from the server console with
  [`tools/bds_driver.py`](../tools/bds_driver.py).

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
