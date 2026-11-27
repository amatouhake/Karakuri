# Validation

What the Karakuri mod was seen to do on a live server. "Builds and tests pass" is not listed
here; only behavior read from the server log is. Raw logs are not kept in the repository.

## Setup

- LeviLamina 26.60.7 / BDS 1.26.60.6 / Windows 11, survival, normal difficulty.
- No real client connected: everything was driven from the server console with
  [`tools/bds_driver.py`](../tools/bds_driver.py).

## 2026-11-28: bot manager, first skills and reflexes

Build: `Karakuri.dll` SHA-256 `09755e22e9c0c7fac5979207ac3f521dc778117cd4e5d55292bbd593f30ae6dd`
(the committed source differs only by dropping a duplicated `status` log line).

| Area | Seen | Not seen / limits |
|---|---|---|
| Spawn, list, remove | Several named bots; duplicate names and names of online players are refused; `list` and bot-name completion data update on spawn and remove. | Completion was not looked at in a client. |
| Respawn | A bot spawned at y=120 died on landing, got its respawn point pinned at the landing spot (0,67,0), and respawned there 20 ticks later instead of falling again. | |
| `goto` | Walked 15 blocks with step-ups and reported `done`. Unreachable targets fail with a reason (`target unreachable on foot; stopped 2.3 blocks away`, `no path to target`). | The engine's navigation often finds no path for targets about 16 or more blocks away, and sometimes for shorter ones. |
| `mine` | Broke stone with a wooden pickaxe (`done: broke minecraft:stone`). Refuses blocks out of reach and air. | Tool choice is up to the caller (`hold`). |
| `place` | Placed cobblestone and reported it after checking the world. Refuses occupied spots and spots with nothing to place against. | |
| `eat` reflex | Started at hunger 6 and ate bread until the stop threshold or until the food ran out. | Under a strong hunger effect it ate all its bread. |
| `attack` and fight reflex | Explicit `attack` killed a husk in 5 hits with an iron sword. When hurt with a monster near, the fight reflex took over and killed the attacker, then ended after 40 calm ticks. | Chasing a mob, the bot walked into a hole and fell to 3 HP: the engine's navigation does not avoid drops, and there is no low-health reflex yet. |
| Pause and resume | A `goto` was interrupted by the fight reflex, resumed after the fight, and finished (`done`). | |
| `stop`, `reflexes on/off`, `hold` | Work as described in the README. | |

**Not checked yet** (needs a real client): whether bots are visible and look right, whether the held
item shows correctly after `hold`, whether a bot is invisible after its first death (an open CFSP
issue), and command completion in the chat box.
