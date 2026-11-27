# Spike 0: SimulatedPlayer on a live server

Results of step 0 in [DESIGN.md](DESIGN.md) chapter 7. Only what was actually seen in the server log is written here.

- **Date**: 2026-11-28 (two sessions; the second added placing, reach, cancelling and the header checks)
- **Environment**: LeviLamina 26.60.7 / BDS 1.26.60.6 / Windows 11. A local test server (survival, normal difficulty). No real player ever connected.
- **Mod**: [`spike/`](../spike), `KarakuriSpike.dll` SHA-256 `73DB8742BA5303114CCACEA66C240A05C2357510C2A3D940D3C79262F2755D6D` for the first session; `028717638A05403B14469DDB4936DD61E5D0639A2FC79488C632E4920946193B` for the second, which added a `place` action.
- **Method**: send `kspike <action>` from the server console and read the `[KSPIKE]` lines in the log. The raw log is not kept in the repository.

## Results

| Check | Result | What was seen |
|---|---|---|
| Spawning | Works | `SimulatedPlayer::create(name, pos, dim, rot)`, then `setPlayerGameType(Survival)`. `isSimulated=true`. The xuid changes on every spawn. |
| Chunks and physics with no player online | Work | Spawned at y=120, the bot fell and landed on the ground (y=67). Nearby blocks could be read. |
| Fall damage and death | Work | Took damage=50 (cause=5) on landing and died. `PlayerDieEvent` fires. |
| Respawn | Works, with a catch | `simulateRespawn()` revives the bot with HP and hunger back at 20. The respawn point is the spawn position, so a bot spawned in mid-air falls and dies again on every respawn. The mod must set the respawn point to a spot on the ground (see "Respawn point" below). |
| Items on death | Dropped | Items held were gone after respawning. |
| Targeted by mobs | Works | A zombie summoned at night went for the bot, hit it for 3 damage at a time with knockback, and killed it. |
| Natural spawning | Happens | With no player online, hostile mobs spawned around the bot at night and attacked it. |
| Attacking | Works | With an iron sword, `simulateLookAt` + `simulateAttack()` killed a zombie in three hits; it dropped rotten flesh. During the cooldown `simulateAttack()` returns `false`. |
| Natural regeneration | Works | With full hunger, HP went from 17 to 19. |
| Item pickup | Works | Walking near dropped items picks them up (dirt and cobblestone). |
| Hunger | Works | A hunger effect drained hunger to 0, then starvation damage (cause=16) started. |
| Eating | Works | Holding bread and running `simulateUseItem()` for 40 ticks gave +5 hunger and used one bread. |
| Sleeping | Works | `simulateInteract` on a bed at night set `sleeping=true`. A few seconds later it was morning (daytime 230) and the bot woke up. The bot alone can skip the night. |
| Break time and tools | As in survival | Grass by hand: 19 ticks (vanilla 18). Stone by hand: 151 ticks (vanilla 150). Stone with a wooden pickaxe: 24 ticks (vanilla 23), dropping cobblestone. |
| Chests | Opening only | `simulateInteract` returns `true`, but there is no API to move items. As DESIGN.md 4.2 says, inventories and containers must be edited directly. |
| Crafting and furnaces | Not checked | No `simulate*` API exists. They have to be implemented by direct manipulation (allowed by "direct means" in DESIGN.md 8.1). |
| Placing blocks | Works | `simulateUseItemInSlotOnBlock` on a neighbor's face places the held block. It returned `true` once when nothing was placed (the spot was taken by a bed), so results must be checked in the world. Clicking the face of an air block fills that air block. |
| Reach | Not enforced | The bot broke and placed blocks 10 blocks away. Skills must check reach themselves (DESIGN.md 8.1). |
| Cancelling | Works | `simulateStopDestroyingBlock` mid-break left the stone in place. `simulateStopMoving` mid-path stopped the bot within about 0.3 blocks. |
| Respawn point | Settable | `Player::setRespawnPosition(BlockPos, DimensionType)` is `MCAPI`. Set it once the terrain under the bot is loaded; at creation time the chunks may not be. |
| Containers, crafting, furnaces (headers) | Reachable | Chest and furnace block actors expose `getContainer()` (virtual, so callable). `Recipes::forEachRecipeFor` and `Recipes::getFurnaceRecipeResult` are `MCAPI`. Not exercised yet. |
| Navigation | Walking only | Built a path to a point about 16 blocks away (straight line) and arrived in about 5 seconds, stepping up one-block ledges. For targets that cannot be reached on foot (a pillar top, an enclosed box) it returns `isFullPath=false` and stops at the closest point. It never digs, pillars up or bridges. A target in the air above water gives a path of length 0. Targets more than 16 blocks away get no path (measured later on flat ground, see VALIDATION.md); on natural terrain some shorter trips also failed from particular start points, cause unknown. |

## What this means for the design

- The engine already handles basic survival body mechanics (damage, death, regeneration, hunger, eating, sleeping, break times, pickup). The body-related parts of L0 and L1 can be thin wrappers.
- The mod has to manage respawn points and enforce reach.
- Containers, crafting and furnaces are all built in the mod by direct manipulation; the needed game functions are reachable.
- The engine's navigation only walks, and only up to 16 blocks. A pathfinder with digging, scaffolding and bridging has to be written, and it is the biggest piece of L1 work (DESIGN.md chapter 7, step 2).
- Hostile mobs spawn at night even with no player online. An idle bot dies, so L0 reflexes (flee, fight, sleep) are needed early.
- The HP readable during `ActorHurtEvent` is already after the damage (the spike log labels it `hpBefore`, which is wrong).

## Build notes

- LeviLamina's server SDK is built from source on your machine and needs LLVM 22. The clang 19 bundled with Visual Studio 2022 fails (`incomplete type 'CommandRegistry'`). It built with LLVM 22.1.8 with `C:\Program Files\LLVM\bin` first on `PATH`.
- The WinGet build of xmake unpacks itself into `%TEMP%\.xmake0`. If files there go missing, downloads and extraction hang or fail with "cannot import module". Deleting the folder and running xmake again fixes it.
- `levibuildscript`'s mod packer calls git, so outside a git repository only the packaging step fails (the DLL still builds).
- Register commands directly in `enable()`, not in a `ServerCommandRegisterEvent` listener: that event has already fired by the time the mod is enabled.
- Command parameter structs need external linkage (not in an anonymous namespace): the command layer reflects over them with Boost.PFR.
