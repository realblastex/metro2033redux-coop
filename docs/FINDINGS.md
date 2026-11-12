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
- The client's own input does not move its body yet. A 6 s backward pad input moved slot 1 by 0.1 m. **This is the
  next problem.**

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
- `*(g_level+0x38)` (g_level RVA 0xFBD820) is **null** in single-player gameplay. Scripts reach the player through
  graph links instead (RVA 0x735B30).
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

`@spawn_player [slot]` on the host, run after the client joined (`@wait_log incoming connection`, then ~15 s):
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

Problems seen:
- The client's own pad input does not move slot 1 (verified). The cause is unknown. Next: find how pad input
  reaches an actor in MP (`net_core::update_input`, client→host input packets) and which entity the client's camera
  and input use. The client still has the SP `player` (id 0) from the quick save.
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
