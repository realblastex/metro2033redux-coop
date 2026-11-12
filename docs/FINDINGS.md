# Findings — Metro 2033 Redux engine multiplayer

All RVAs are for the unpacked Next-Gen `metro.exe`, SHA-256
`097df1a2be592e652e5f082bb7ecbcf4043d159034f65d2ec6ed34b7efb96f31`. Labels:
**verified** = seen in a test run (logs/screenshots), **derived** = read from disassembly/decompilation and not
yet exercised, **unknown** = not found yet.

## 1. Current state (verified 2026-11-12)

- Two instances on one PC: host `@net_host` on a loaded story level (`2033\l01_hunter`); client `connect 127.0.0.1`.
  The client receives the host's quick save (`[net] qsave size: ~448 KB`) and loads the same level by itself.
- The host's single-player actor is linked to net slot 0 (`@link_actor`). Its entity is `player`, id 0.
- Host movement replicates: the slot-0 net position on both sides followed a 4 s forward/back pad input
  (x -7.3 to -3.5 and back).
- Both instances stay alive in gameplay. The client crash at RVA 0x39A2FD is fixed (see 3.4).
- The joining player gets a body of its own (`@spawn_player 1` on the host after the client joined, see 5). Slot 1 is
  linked to the new entity (id 0x10B) on both sides. The client sees the host's Artyom walk away from its own camera.
- The client controls its own body and its movement replicates to the host (verified 09:56): client pad forward 4 s
  moved slot 1 from x=-7.3 to -5.1, strafe moved z from -15.7 to -16.4, and the host's copy of slot 1 shows the same
  values.
- The host renders the client's body (verified 10:00): after a right-stick turn (`@pad 0 0 0 1500 32000 0`) the
  host's screenshot shows the second Artyom standing where slot 1 is.
- Both bodies spawn on the same spot. `@teleport 1 0 0` on the client (after its body appeared) moved it 1 m
  (x -7.3 to -6.3), and the host got the new position (verified 10:02). The transform is a 4×4 matrix at
  entity+0xC0 (translation at +0xF0); set_transform = entity vtbl+0x190(&m, 0).
- Soak run (verified 10:16–10:21): ~4 min of co-op, host 12 and client 12 forward/back moves. Both stayed alive (no
  event-1000 crash). At the end both sides show the same positions: s0 (-4.8,-2.6,-16.0), s1 (-5.6,-2.6,-16.1).
- NPCs (verified 10:33, `@dump npc` on both sides ~60 s after the join): same 3477 entities with the same ids; 14
  `npc*` entities, positions equal within 0.06 m (e.g. `npc_scene_0001` -157.39 vs -157.45). Unknown: whether the
  host replicates NPCs or both sides simulate the same scripted scene. Most of these NPCs are far from the start room.
- NPC replication test (verified 10:36): the host teleported `npc_friend_fx_0004` (id 0x3BE) by +100 m
  (`@teleport 100 0 0 3BE`). The host's copy moved to x=-49.93; the client's copy stayed at -149.93 for 15 s. So that
  far NPC's move did not reach the client.
- Nearby NPCs replicate (verified 10:42): the host moved `man_place_0003` (0x16D, ~12 m from the players) by +2 m;
  the client's copy showed the new position (1.21,-4.67,-4.22) within 6 s.
- How (derived from RVA 0x747630, list verified with `@dump net`): the host keeps a list of networked entities at
  net_core+0x168 (16-byte entries `{u16 id, …, handle at +8}`, count u16 at +0x172). In Hunter it holds 39
  entities: both players and all human NPCs (`man_place_*`, `sanitarka`, `npc_friend_fx_*`, guards, …). Per client
  slot, every entity gets a priority from entity vtbl+0x170 × distance × view-direction factors, accumulated until
  sent. The send job writes the top ones with entity vtbl+0x180 into a ~128-byte budget per packet. A far NPC
  (~140 m) never won a slot within 15 s.
- The client registers 51 networked entities, the host 39 (verified 10:45, `@dump net` on both). The client's extra
  12: nine `nosalis_*` monsters (`nosalis_side1` … `nosalis_side4_2`, `nosalis_top`), `suhoi_old_0001`,
  `hunter_old_0001`, `man_place_0017`. The host never sends entities it doesn't list, so the client runs those on its
  own. Likely desync source once monsters fight. Unknown why the lists differ (the host loaded an SP save, the
  client the host's quick save).
- Registration (verified writer, derived conditions): a hardware watch on the count (global RVA 0xFD5192) during
  `@host` caught only the insert RVA 0x831C0, called from `FUN_140747F60(net_core, &handle)` ("make relevant", logs
  `* relevant: <name>` to engine.log). Nothing registers during a plain SP load (list empty until `@host`).
  Conditions: MP flag set; entity vtbl+0xB80 == 0 (not destroyed); vtbl+0xB88 != 0 (active); flags `+0x10C & 0x204
  == 4`; priority vtbl+0x170 ≥ 0. Low-priority entities also need `*(g_level+0x220) ≥ 5`. Max 256 entities
  (`!too many network synch entities`). Runtime registration (derived): the activate/deactivate update RVA 0x6D6DA0
  calls make-relevant when an entity becomes active and unregisters it (RVA 0x747CA0) when it goes inactive; the
  attach/detach path RVA 0x6DD360 does the same. The client's 12 extra entries are dormant: see 5.4.
- Second level works (verified 14:13–14:20): on `l19_defence` both players spawn armed and the client can fire its
  weapon, after three single-player-assumption crashes were guarded (5.5).
- **Next problems:** combat (damage between players, NPCs targeting the client), AI, doors and story events are not
  checked. Level transitions, save/load and LAN are not tested.

## 2. How the pieces fit (verified unless marked)

### 2.1 Single instance check
WinMain-ish code at RVA 0x655C20 creates a named mutex (`METRO2033-AEEF602F-E62E-4d1a-A055-DFC0E240F4FB-<user>`)
and exits on `ERROR_ALREADY_EXISTS`. `injector --launch` starts the game suspended. It flips the `jne` after
`cmp edi, 0B7h` (bytes `81 FF B7 00 00 00 75 1F 41 B9 10 00 00 00`) to `jmp`, then resumes it. No file on disk is
changed.

### 2.2 Console and engine log
- `slog` RVA 0x65C0C0 (raw line logger; `rlog` printf form at 0x65C460) is hooked and every line goes to `engine.log`.
- `GetConsole` RVA 0x667E40 (global RVA 0xFBD770); `console->vtbl[0x40/8](con, "cmd")` executes a command.
  Commands must run on the game's window thread (calling `server` from another thread crashed). They are queued and
  run from a `PeekMessageA` hook.
- The main menu is itself a level: `map X` from the menu needs `disconnect` first.

### 2.3 Starting a network game
- `server` and `connect` both go to net start RVA 0x6B7EA0. **With** an address it sets the MP flag (RVA 0xFBDF2A)
  and connects as a client. **Without** an address it becomes the server, but only if the MP flag is already 1.
  Otherwise it starts a local-only game (`net_core.host` = 1). Map loads clear the flag again (RVA 0x6BAC10).
- `@net_host` = clear `net_core.host` (1 = local marker), set MP flag, call net start without an address. The
  already-loaded single-player level keeps running as a server. (Loading a level in MP mode instead spawns no player
  actor: scripts log `player is absent`.)
- Engine ENet port: UDP 1000. A second instance on one PC needs a different local bind port:
  `enet_host_create` (RVA 0x93AB20) is hooked to change only the bind port (`engine_bind_port`).
- Default client topology is a peer mesh (`[net] connecting to peer`).
- `net_core` global RVA 0xFD5020: +0 ENet host (1 = local-only), +8 local player index (0 = server), +0x18 players
  vector `{array, u16 cap, u16 size}`, net_player = 0x70 bytes, ctor RVA 0x4C0E0, resize RVA 0x4E8B0.
- Player-slot fix (port of MetroDeveloper `multiplayer_fix`): on an incoming connection the server writes a slot past
  the vector end (crash RVA 0x6B8E7C). `send_props_pack` (RVA 0x4CA00) is hooked to grow the vector first.

### 2.4 Things that stall a network game
- After a map load the engine waits in `mapstate_loaded_and_waiting` (paused, net not serviced) until a key press.
  `@finish_load` calls `loader_finish(&g_loader)` (RVA 0x69E4C0, g_loader RVA 0xFBDEA0).
- A `loader` pause pushed before injection is never popped, so the game clock (timer RVA 0xFC10B8, ms at RVA
  0xFC1124) never advances and the net send job (RVA 0x6B4D00) never sends. `@unpause_all` pops the pause stack
  (g_loader +0x16FE0 `{handles, u16 cap, u16 size}`, POP PAUSE RVA 0x664380).
- Focus: the game polls `GetForegroundWindow` to pause when unfocused. `keep_active` hooks it and filters deactivation
  messages.

### 2.5 Input without a window
`XInputGetState` (xinput1_3 ordinal 2) is hooked for pad 0 (`@pad`). Keyboard and mouse are DirectInput8 and need
focus. `PostMessage(WM_KEYDOWN)` does nothing.

## 3. Net players and actors

### 3.1 net_player layout (0x70 bytes)
| offset | meaning |
|---|---|
| +0x30 | ENet peer |
| +0x48 | network position (3 floats) |
| +0x58 | actor handle (cached) |
| +0x64 | actor entity id (u16, 0xFFFF = none) |
| +0x68 | state bits; `(x >> 16) & 7`: 4 = waiting for an actor, 5 = linked |

### 3.2 Linking (verified)
- `link_actor(net_player*, u16 id)` RVA 0x6B89F0 calls `set_actor_id` RVA 0x4C330 (+0x64 = id, drops +0x58) and sets
  state 5. `actor_of(net_player*, 1)` RVA 0x6B93F0 resolves +0x64 through g_server (RVA 0xFBD850, lookup RVA 0x6A3CC0)
  into +0x58 and returns `&+0x58`.
- Only the server's player spawner (RVA 0x6F6210, also RVA 0x6B04C0: "no empty slot for player [%s]->[%s], did you
  forget 'instead_of' link?") calls `link_actor`, and only when `net_core+8 == 0` (server). In single player that
  field is 1, so a story level's actor is never linked. `@link_actor` does it by hand after `@net_host`.
- On the client, per-player state parsing (RVA 0x6B92C0) copies +0x64 from the host for non-local slots. The
  slot-assign message (net update RVA 0x6B32F0, case 2) carries the client's own actor id: -1 means reset (RVA
  0x6B8E30), any other value links it (RVA 0x6B89F0).

### 3.3 Finding the local player (verified)
- `*(g_level+0x38)` (g_level RVA 0xFBD820) is the entity this instance controls (entity base; id = u16 at +0x108).
  Correction: an earlier read said it is null in single-player gameplay. The heartbeat (`ctl=`) shows id 0 on the
  host after the save loads. On the client it is null until its body spawns, then it is 0x10B. A client's send job
  (RVA 0x6B4D00) sends its state (msg 0, 31 bytes, ~15 Hz) only while it is set. Scripts reach the player through
  graph links (RVA 0x735B30).
- The actor aim update RVA 0x399DD0 (only caller RVA 0x399A30) runs per frame on the player. It is hooked to remember
  the actor. Handles point at **entity+0xA0**. The entity id is the u16 at (entity+0xA0)+0x68. The name is the
  string object at (entity+0xA0)+0x110, with chars at +0x14. The menu level has its own actor, so the list is
  cleared on `@finish_load`.

### 3.4 Client crash RVA 0x39A2FD (fixed)
The net player lookup by entity (RVA 0x6B78F0) works in two modes:
- MP mode: it returns the slot whose +0x64 equals the entity id.
- SP mode (MP flag 0; the joining client was in it): it returns slot 0 for the player or a null handle.

Either way it can return a slot with no actor, and the aim code dereferences the null handle. The fix hooks the
lookup. While networked (`net_core.host` > 1), it returns null when the slot's actor doesn't resolve; callers handle
null. In plain single player the guard must stay off: it crashed save loads at RVA 0x3175A.

## 4. Fast start (verified)
- The Hunter intro is an in-engine scene of ~55 s that pad A cannot skip. A save made right after it
  (`2033_auto_save`, copied to `2033_coop_start`) loads in about 1 s with `gameload coop_start`.
- `gameload` returns silently until a profile is signed in (`*(menu+0x2050)` empty). Pressing pad A, START, A at the
  menu signs it in.
- After `gameload`, wait for the engine line `load save bin` before `@finish_load`. The first
  `mapstate_loaded_and_waiting` is too early and crashed at RVA 0x3175A.
- A save made after `@net_host` crashed on load at RVA 0x3175A. Make the start save without any net commands.

## 5. A body for the joining player

### 5.1 What works (verified 2026-11-12, two runs)

`@spawn_player [slot]` on the host, run after the client joined (`@wait_log incoming connection`, then ~15 s). Since
10:10 the host does this by itself: the incoming-connection hook queues it `auto_spawn_s` (15) seconds later
(verified). It skips slots that already have an actor.
1. `FUN_1406a9a80(g_server, &src_handle, &list, 0xFFFF)` builds the clone list from the SP player (the actor the aim
   hook found). `src_handle` = the entity's handle object, `*(entity+0xA0)`. `list` = `{u16 triples*, u16 cap,
   u16 size}`, triple = `{src id, new id, parent}`. Result: 20 entities (the player and its children), root new id
   0x10B.
2. `FUN_1406ba940(_, 0, &list, &src_handle, &null, &null, 0, is_player=1, slot, 0, 0, 0, 0)` queues the reliable spawn
   event (kind 4). With is_player it calls `link_actor(slot, root new id)` at once.
3. The host executes its own event in RVA 0x6B2490 (reliable event queue runner, jump table RVA 0x6B2C50). The client
   gets the same event and logs `[net] pid==1, eid==267` (267 = 0x10B).

Result: heartbeat `s1=10B/5` on both sides. The client's screenshot shows the host's Artyom from the client's camera,
and he walks away when the host moves.

After the spawn event the client's controlled entity (`*(g_level+0x38)`) is the new body (0x10B). The client's pad
moves it, and the client sends its state to the host, which applies it to slot 1 (client-authoritative movement).
An early run looked like "input does nothing": a backward input hit a wall behind the start spot, and a forward
input was blocked by the host's body standing on the same spot.

Problems seen:
- The client logs `!G entity_players_knife: player is absent` / `entity_players_torch: player is absent` once at the
  spawn.
- The host logs `!No ammo item. Save level, bleat !!!` hundreds of thousands of times, starting right after
  `@link_actor` (before the spawn). Harmless so far, but it bloats `engine.log`.
- Both bodies spawn on the same spot. A spawn offset is still missing.
- Once the client showed the MP pause menu (`^mainmenu default_mp`) about 25 s after its pad input. The cause is
  unknown.

### 5.2 How the engine clones (derived)

- Writer: `FUN_1406a6760(g_server, &handle, &writer)` writes the header (RVA 0x49EA0) and then entity vtbl+0x150.
  Reader: `FUN_1406a7a00(g_server, &out, ver, &reader, 0, force_new, id)` (header, then vtbl+0x148).
- Stream: `FUN_1406bb890(&stream, &buffer, 0 = read / 1 = write, flags, 0)`. The save uses flags 0x30, the MP respawn
  manager 0x10.
- Instantiation: RVA 0x6A9D50 → 0x6A7D70 (walks the triples) → 0x6A85C0 (serialize the source with a6760, read it
  back with RVA 0x6A8A10 under the new id, flag 0x10000 on +0x6C) → entity vtbl+0xB70 remaps links. RVA 0x694DA0 then
  copies the transform (entity vtbl+0x190).
- RVA 0x6951C0 = "spawn N copies" (server only): a9a80 per copy, then ba940 with is_player=0. It has 24 callers in
  game code.

### 5.4 Monsters, spawned copies, doors, items (session 6, 2026-11-12)

`@dump` now prints `f=` (u32 flags at entity+0x10C) and `b6=`/`b7=` (bytes written by RVA 0x6D6DA0; b6=1 seen only on
active entities).

Verified (two-instance runs 13:25–13:45):
- Flag bits seen: 0x8 = networked (set on `special_nosalis_hunter_0000` by `@host`, f 7 → F); 0x10000 = spawned
  copy (every clone); 0x2 on a `player` = the other instance's body (host: 0 f=D, 0x10B f=1000F; client: 0 f=F,
  0x10B f=1000D); 0x100 on `player_map_*` = the local player's map. 0x20 is set on the dormant intro monsters.
- The client's 12 extra net-list entries (5.1/1) are dormant: the nine `nosalis_side*` / `nosalis_top` have f=0x27,
  b6=0 on **both** sides, same positions. The client never clears them from its list, but nothing moves them.
- Active monster replicates: the host moved `special_nosalis_hunter_0000` (0x6A37, b6=1, networked) by
  (31,-11.5,4) with `@teleport … 6A37`; the client's copy showed the same position (-1.25,-4.42,3.65) at its first
  check ~14 s later and kept it. This nosalis is scripted/idle; it did not attack.
- Spawned copies replicate: new `@spawn <id hex>` on the host (same path as `@spawn_player`, but `is_player=0`,
  slot -1, arg7=1 like the script spawner RVA 0x4F7050 → RVA 0x6951C0) cloned `ai_nosalis_actor_0026` (0xAA1D) as
  0x463 on **both** sides (same id, position, f=0x1E000). The copy stays inactive (b6=0) and is not networked; a host
  `@teleport` of it cleared f 0xE000 but the client copy did not move. The `ai_nosalis_*` entities (f=0xE000, 50 of
  them near x -52..-65) look like spawn templates. How a spawned monster gets woken is **unknown** (activity mask =
  entity vtbl+0x660, RVA 0x6D7200).
- Doors are not networked: 40 `*door*` entities in Hunter, all f=0x4003 / 0x4103 (`door_use_*`), b6=0, no 0x8 bit,
  except `door_guard_hospital` / `door_hospital_living_1` (f=0xE003, b6=1). A door test failed to aim: X pressed near
  `door_use_0003` with the camera facing a wall; no state change on either side. Untested.
- The client's body has no knife: the clone (20 entities) contains `player_torch_0022` (0x20A),
  `torch_podsvet_0012`, `players_hands_0012`, `player_map_0005` and timers. `players_knife_0009` (0x1F, at the
  origin, f=0x4002) is not a child of `player`, so it is not cloned. Matches the client log
  `entity_players_knife: player is absent`.
- Combat is not testable from the fast-start save: weapons are holstered in the station (`take_item
  [unarmed_box]`), and no monster attacks early in Hunter.

### 5.5 Armed test level: l19_defence (session 6)

`disconnect;server;map 2033\l19_defence` from the menu (SP) starts the player armed (assault rifle) in a tunnel at
x≈225. A save made 8 s in with the console command `gamesave coop_l19` (written as `2033_coop_l19`, 716 KB) loads
with `gameload coop_l19` like `coop_start`. Three crashes had to be fixed before two players got into it; all three
are game code that assumes a single player always exists (fixes in `src/engine.cpp`, verified 14:03–14:20):

| crash RVA | where | cause | fix |
|---|---|---|---|
| 0x361292 (host, then client) | per-frame update RVA 0x360FD0 of an entity that tracks the player | reads `*(g_level+0x38)+0xF0` with no null check. The host's controlled entity goes null after net start until slot 0 has an actor (`@link_actor` ran 1 s later); the client has none until its body spawns | `@net_host` links slot 0 in the same frame; hook skips RVA 0x360FD0 while there is no controlled entity |
| 0x20D44C (client, qsave load) | RVA 0x20D3A0 `int f(obj, id, _, use_player, flag)` | tests the controlled entity for null, then dereferences it anyway (same crash as session 3's MP-mode l02 load) | hook passes `use_player = 0` while there is no controlled entity |
| 0x5260D5 (client, when its body fired) | weapon HUD draw RVA 0x525F60 | HUD slot bind RVA 0x526AA0 logs `!No ammo item. Save level, bleat !!!` and leaves the slot's ammo-type index (+0xA8) unset; the draw indexes the weapon-type ammo table (`*(g_level+0x2A500) + type*0x178`, table at +0x98; type 23 of 26) with garbage | hook zeroes each slot's +0xA8 before the bind |

Results (verified):
- Host and client both in l19 with their own bodies (client body 0x167, `s1=167/5` on both). Both fired with RT
  (`@pad 0 0 0 4000 0 0 0 255`); the client's screenshot shows its own muzzle flash. Both alive for ~2.5 min.
- The host networks 43 entities (humans: diggers, traders, kids, …), the client 69. The client's extra 26 are
  lurkers (`lurker_*`, `lur_*`, `nosach*`), all dormant on both sides (f=0x67, b6=0) and 150+ m away. No monster
  attacked within 2.5 min; walking forward ran into a wall after ~4 m.
- Monster fights are script-gated, not proximity-gated (verified 14:22–14:30): both players teleported next to
  `lurker_farm_0002` (x -71) and to the lurker spawn template `vs_clone_lur_first_part` (x 58) and stood there
  ~1 min, also firing. No lurker became active or networked (`* relevant:` none). Both stayed alive and the 290 m
  teleports replicated. `l17_trolley_combat` and `l03_chase` start in cutscenes. A save at a real fight is needed.
- Unknown: whether the host sees the client's shots (sound/flash/damage), damage between players, AI targeting the
  client. The client still logs `No ammo item` (7 times before the guard), so its ammo count is probably not real.

### 5.3 Older notes (static, before the run)

In MP levels the player bodies already exist in the level, one per slot, linked with `instead_of`. Respawn
(`actions/net/respawn_ex`, RVA 0x4367F8) only picks a spawn point (RVA 0x3F9E4C) and links via RVA 0x6F5F20, then
RVA 0x6B04C0. A story level has a single `player`, so a second body must be created.

Entity creation primitives:
- `FUN_1406a7a00(g_server, &out, ver, reader, 0, force_new, id_or_ffff)` reads a header with RVA 0x4A0C0.
  - Header (binary): u32 `class`, u32 `static_data_key`, stringz `att_bone_id`, u16 id, u16 `parent_id`,
    48-byte `att_offset`.
  - Id: `id_or_ffff`, else the stream id. RVA 0x4BBE0 gives a free id on collision.
  - It builds the object via `*DAT_140fbded8(&class, &sdk)`, registers it (g_server vtbl+0x28), and loads it with
    **entity vtbl+0x148(reader, ver)**.
- `FUN_1406a50f0(g_server, &out, ver, &class, &sdk, new_id, parent, &name, &xform, …, record)` creates with an
  explicit id.
  - Used by the network "ui clone" helpers (RVA 0x6A6B00 / 0x6A7450, called from RVA 0x6ADBB0). They clone a hierarchy
    from records (`entities_params`, `entities/rec_%04d`) and then remap links with entity vtbl+0xB70(&idmap).
- Level load creates all entities from records with RVA 0x6A62A0 (caller RVA 0x3CBBD0, "load dynamic bin").
- New ids: allocator RVA 0x4BAC0 on g_server+0x148, plus 0x100.
- Spawn events: RVA 0x6BA940 builds the network spawn event (kind 4 in the reliable queue at net_core+0x108; type 5/6,
  6 = player; slot in bits 8..12 of word 4). For players it also calls `link_actor(slot, root new id)`.
  RVA 0x6A9A80 builds the `{src id, new id, parent}` list.
- The engine clone script action `actions/engine/clone` (vtable RVA 0xB96BE0, execute = spawner RVA 0x6F6210) only
  gets link targets and sends the event. The host instantiates it when it runs its own event queue (see 5.2).

Runtime check done: over ~90 s of Hunter (intro and walking), creation (RVA 0x6A62A0 / 0x6A7A00) ran only for level
loads (caller RVA 0x3CBC5E). The story level spawns nothing early on.

Other open items:
- MetroDeveloper's TODO says `net_core::load_net_time` lacks a `r_u32` read (packet desync) and
  `net_core::update_players` looks broken. Expect state-parsing bugs on the client.
- Host logs `!G can't find player on level for take_item` and `entity_players_torch: player is absent` in MP-mode
  loads (scripts expect one single-player actor).

## 6. Corrections to early analysis
- The Steam `metro.exe` is SteamStub-protected (`.text` entropy 8.0). Early "97.8 % encrypted code" conclusions came
  from disassembling encrypted bytes. The unpacked exe analyses normally.
- MetroDeveloper is dead on Next-Gen: its Redux-Steam `rlog` signature no longer matches, so it disables itself.
  37 of its signatures broke (`tools/re/mdscan.py` diffs its pattern table across exes).

## 7. Tools
`tools/re/*.py` (Python 3 + capstone): `disx.py <exe> <rva> <n> [f]` disassembly with import and string names,
`strs.py <exe> <regex>` strings, `xref.py <exe> <string>...` code references to strings, `callers.py <exe> <rva>...`
direct callers, `memwrite.py <exe> <disp> <size> [lo hi]` functions writing `[reg+disp]` (optionally also touching a
global range). A Ghidra project with GhidraMCP was used for decompilation.
