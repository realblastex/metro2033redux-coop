# Metro 2033 Redux co-op

A work-in-progress co-op mod for **Metro 2033 Redux** (Steam, PC). Instead of writing our own netcode, it revives
the multiplayer code that 4A Games left inside `metro.exe` (ENet-based dev multiplayer) and makes it run a story
level with two players.

This repo holds the **clean source** and the **findings**. It is the project link for this game's entry in the
coop-anything directory. Game files are not included and never will be.

## Status (2026-11-13)

What works:

- Two game instances run on one PC. The second instance is allowed by `injector --launch` (the launcher does the same).
- `MetroCoopLauncher.exe` (in `<game dir>\metro_coop\`) has **Host** and **Join <IP>** buttons. Host: load a save or
  start a new game, and the game hosts by itself once the level is loaded. Join: the game joins from the main menu by
  itself. Verified on one PC with `scripts\smoke_test.ps1 -Launcher` (PASS, 8 checks).
- All mod files (DLL, launcher, `metro_coop.ini`, logs) live in `<game dir>\metro_coop\`, nothing in AppData.
- The host turns a running single-player level into a network game. The client connects, loads the host's level
  from the host's quick save, and both stay in gameplay.
- The host's player character is linked to network slot 0. The host's movement replicates to the client.
- The joining player gets its own body (the host spawns it automatically 15 s after the join). The client sees the host's Artyom walk around
  from its own camera. The client walks its own body with its own input, and the host sees that body move.
- Both instances are driven without a human. A fake XInput pad and console commands run from a config file.
- Fast start takes about 30 s from launch to both players in-game.
- A second, armed level works: on `l19_defence` both players get their own body and rifle, the client can fire, and
  the host sees the client's muzzle flash and tracers. Shooting the other player damages them: the host killed the
  client's body, and both sides agree it is dead.
- Handcar ride on `l03_chase` (early story): the client's body rides along with the host's handcar
  (FINDINGS 5.8). `scripts\smoke_test.ps1 -Save coop_ch3` passes. The nosalis attack on the handcar kills the host
  with the single-player death screen; the client rides on (host death handling is not done).
- Host death: both games restart and load the host's last checkpoint (`coop_auto_save`, or `coop_session` = the save
  made when hosting started if no checkpoint came since), then the host hosts and the client joins again by
  themselves (about 65 s; FINDINGS 5.9, 5.10).
- Story level changes: the host goes on to the next level and hosts again there; the client restarts and joins
  the new level by itself (about 40 s). The host keeps the inventory carried over from the last level, and the
  joiner's new body is a copy of it (FINDINGS 5.11). Tested with the console commands a level's end runs
  (`save_player <level>`, then `change_map <level>`), not yet by playing a level to its end.
- Checkpoint saves written while hosting load again (FINDINGS 5.10). They go to `coop_auto_save`, so the host's
  own single-player `auto_save` stays untouched. The joiner's game writes no saves while it is in a session, so its
  own saves stay untouched too.
- The windows are titled `Metro Redux [host]` and `Metro Redux [client]` after `@host` / `@join`.
  Three game crashes that assume one single player exist were guarded for this (FINDINGS 5.5).

What does not work yet:

- Back up your saves before hosting anyway (the mod is early). A client that joined while a scripted fight was running crashed
  once on loading the host's level (FINDINGS 5.10).

- Both bodies spawn on the same spot; `@join` then moves the client 1.2 m in front of the host. A wall there would
  block that. See [docs/FINDINGS.md](docs/FINDINGS.md), section 5.
- The two-weapons view after a weapon switch (FINDINGS 5.7) did not come back in a retest on 2026-11-13, and the
  owner no longer sees it. The cause was never found.
- The host draws the client's body wrong: up close it shows a big bare arm with a blade, while the client holds its
  AK. The client's weapon choice never reaches the host.
- NPCs near the players replicate from the host (distance priority, ~128 bytes per packet); far NPCs did not update
  within 15 s. An active monster moved on the host shows up on the client. Entities the host spawns (`@spawn`) exist on
  both sides with the same id.
- The client's body has no knife (the torch is there). Its ammo items are cloned detached; the mod re-attaches them
  (`@fix_ammo`, automatic).
- A dead joiner gets a new body 5 s later, 1.2 m in front of the host (a wall there would block it). Doors are not in the network list; door use between players is
  untested. Damage, enemy AI and monster fights are untested (Hunter starts unarmed; in `l19_defence` no monster
  attacked within 2.5 min; a spawned monster stays asleep).
- No AI or story-event sync has been checked. No level transitions. Not tested over LAN or the Internet, only
  over loopback.

QA level: automated smoke test. `scripts\smoke_test.ps1 [-Save coop_start|coop_l19] [-Respawn]` runs both copies and
checks 8 facts from the logs (both alive, no crash, body spawned and linked, client controls it, both movements seen,
positions equal on both sides); `-Respawn` adds a kill of the client by the host and checks the automatic respawn
(10 checks). Last runs: Hunter and `l19_defence` `RESULT: PASS (8 checks)`, `l19_defence -Respawn`
`RESULT: PASS (10 checks)`. `-Launcher` starts both copies through `MetroCoopLauncher.exe` instead (auto host/join, same 8 checks,
`RESULT: PASS (8 checks)`). Longest manual run: ~4 min of
co-op, 24 moves, both alive, final positions equal on both sides.

Verified test run (log lines from `metro_coop.log`, host then client):

```
host   engine: link_actor check: resolves to ...8710 'player'
host   heartbeat: ... local_idx=0 s0=0/5/(-3.5,-2.6,-15.9)   <- host pad forward moved slot 0 from x=-7.3
client heartbeat: ... local_idx=1 s0=0/5/(-5.1,-2.6,-15.7)   <- client sees the same slot move
host   engine: spawn_player 20 entities, root new id 0x10B
client [net] pid==1, eid==267                                  <- engine.log: client got its body (0x10B)
client heartbeat: ... local_idx=1 s0=0/5/(-3.5,-2.6,-15.9) s1=10B/5/(-7.3,-2.6,-16.0) ctl=10B@(-7.3,-4.4,-16.0)
client heartbeat: ... s1=10B/5/(-5.0,-2.6,-16.4) ctl=10B@(-5.0,-4.4,-16.4)   <- after client pad forward + strafe
host   heartbeat: ... s1=10B/5/(-5.0,-2.6,-16.4)                               <- host sees the client move
```

Heartbeat fields: `sN=<actor id>/<slot state>/(<net position>)`, `ctl=<id>@(<pos>)` = the entity this instance
controls, `aN@(x,z)` = actors seen by the aim hook.

## Game identity

| | |
|---|---|
| Game | Metro 2033 Redux, Steam app 286690, Next-Gen update |
| Engine | 4A Engine |
| Executable | `metro.exe`, **unpacked** (SteamStub removed), SHA-256 `097df1a2be592e652e5f082bb7ecbcf4043d159034f65d2ec6ed34b7efb96f31` |
| OS | Windows 11 x64 |

Engine functions are found by byte pattern, but several diagnostics use fixed RVAs of this exact build. Another
`metro.exe` build will need the patterns and RVAs checked. Unpacking the Steam exe is up to you; this repo does not
ship or modify game files.

## Build

Requires Visual Studio 2022 (MSVC, x64) and CMake ≥ 3.20.

```
cmake -S . -B build
cmake --build build --config Release
```

Output: `build\Release\metro_coop.dll`, `build\Release\MetroCoopLauncher.exe` and `build\Release\injector.exe`.

## Install and play

1. Make a folder `metro_coop` inside the game folder (the one with `metro.exe`). Copy `metro_coop.dll` and
   `MetroCoopLauncher.exe` into it. The game folder must be writable: the mod keeps `metro_coop.ini` and its logs
   there.
2. Host: run `MetroCoopLauncher.exe`, click **Host a game**, then load a save or start a new game. About 2 s after the
   level has loaded, the window title changes to `Metro Redux [host]`. Allow `metro.exe` in Windows Firewall
   (UDP port 1000) when Windows asks.
3. Friend: run `MetroCoopLauncher.exe`, type the host's IP address, click **Join**. Press a key at the title screen.
   The game joins once the menu is up (or 25 s after start), loads the host's level and gets its own body about
   15 s later. If joining fails, press **F11** in the menu to retry.
4. Two copies on one PC (testing): tick **Second copy on this PC** for the joining copy, and join `127.0.0.1`.

The launcher needs the unpacked `metro.exe` described under "Game identity". LAN and Internet play are **not tested
yet**; only two copies on one PC are. Older builds kept their files in `%LOCALAPPDATA%\MetroCoop\`; that folder is
no longer used and can be deleted.

## Run a two-instance test on one PC

1. Make two copies of the game folder (hard links are fine), each with the unpacked `metro.exe`. Never use your real
   install or your real saves.
2. Point the scripts at them:
   ```
   $env:METRO_HOST_DIR   = '<game copy 1>'
   $env:METRO_CLIENT_DIR = '<game copy 2>'
   ```
3. Make the fast-start save once (host only; this writes the auto-save made right after the Hunter intro):
   ```
   scripts\set_ini.ps1 host "exec_on_start=@wait_log playing video 30;disconnect;server;map 2033\l01_hunter;@wait_log mapstate_loaded_and_waiting 60;@finish_load;@wait 1;@unpause_all;@wait_log ^gamesave auto_save 120" exec_delay_s=0
   scripts\run_test.ps1 -Wait 90
   ```
   Then copy `2033_auto_save` to `2033_coop_start` in the save folder your copies use. A Steam emulator keeps them
   apart from your real Steam saves. Do not run tests against your real Steam save folder.
4. Run host and client:
   ```
   scripts\set_ini.ps1 host "exec_on_start=@wait_log playing video 30;@wait 1;@pad 1000 0 0 200;@wait 2;@pad 0010 0 0 200;@wait 2;@pad 1000 0 0 200;@wait 2;gameload coop_start;@wait_log load save bin 30;@wait 1;@finish_load;@wait 1;@unpause_all;@wait 4;@host;@wait_log incoming connection 60;@wait 20;@pad 0 0 32000 4000;@wait 40;@pad 0 0 0 1500 32000 0" exec_delay_s=0
   scripts\set_ini.ps1 client "exec_on_start=@wait_log playing video 30;@wait 20;@join 127.0.0.1;@wait 20;@teleport 1 0 0;@wait 15;@pad 0 0 32000 4000;@wait 6;@pad 0 32000 0 3000" exec_delay_s=0 engine_bind_port=1001
   scripts\run_test.ps1 -Client -Wait 125
   ```
   The host spawns the client's body by itself 15 s after the client connects (`auto_spawn_s`). The host walks
   forward and later turns around; its screenshot then shows the client's body.
5. Check the heartbeat lines. Per instance, logs are in `<game copy>\metro_coop\` (`run_test.ps1` copies the
   build there):
   `metro_coop.log` (ours) and `engine.log` (every engine log line).

Windows Firewall asks about network access the first time `metro.exe` binds UDP port 1000. Loopback works without
allowing it. LAN play will need it allowed.

## Try it by hand (two copies on one PC)

1. Start both copies with `injector --launch` and inject the DLL (as `scripts\run_test.ps1` does), or use the
   launcher. Set `engine_bind_port = 1001` in the second copy's `metro_coop.ini`.
2. Copy 1: load a save (or start a new game) and wait until you are in gameplay. Press **F9**: it becomes the host.
3. Copy 2: at the main menu press **F11**: it joins `join_address` (default `127.0.0.1`), loads the host's level and
   gets its own body about 15 s later.

Verified 10:30 with key presses sent by a script (F9 on the host, F11 on the client). The hotkeys only react while
that copy's window is in front. F10 is not usable: the game itself reacts to it, and a join started with F10 hung the
client.

Two PCs on a LAN (**not tested yet**): both PCs need the game, the DLL and the injector. On the host PC allow
`metro.exe` in Windows Firewall (UDP port 1000). On the client PC set `join_address = <host's LAN IP>` and leave
`engine_bind_port = 0` (the 1001 trick is only for two copies on one PC). Then F9 on the host, F11 on the client.

## Config: `metro_coop.ini`

In `<game dir>\metro_coop\`, next to the DLL. Created with defaults on first run.

| key | default | meaning |
|---|---|---|
| `exec_on_start` | empty | `;`-separated engine console commands and `@`-commands, run in order |
| `exec_delay_s` | 5 | seconds after injection before `exec_on_start` starts |
| `keep_active` | true | keep running while the window is unfocused |
| `engine_bind_port` | 0 | engine UDP port (0 = 1000); use 1001 for a second instance on the same PC |
| `join_address` | 127.0.0.1 | host address for F11 and the launcher's Join |
| `mode` | empty | written by the launcher: `host` = host at the first level start after the menu; `join` = join `join_address` once the menu is up or 25 s after start; empty = only `exec_on_start` and F9/F11 |
| `auto_spawn_s` | 15 | host: spawn a body for a joining player this many seconds after it connects (0 = off) |

`@`-commands:

| command | what it does |
|---|---|
| `@host` | on a loaded single-player level: become the network host (= `@net_host;@wait 1;@players 1;@link_actor`) |
| `@join <ip>` | connect to a host and enter its level (= `connect <ip>;@wait_log mapstate_loaded_and_waiting 60;@mp 1;@wait 1;@finish_load;@wait 1;@unpause_all`) |
| `@wait <s>` | pause the list |
| `@wait_log <text> [timeout_s]` | pause until an engine log line contains `<text>` (default 120 s) |
| `@skip_video [timeout_s]` | press pad A every second until a video is skipped |
| `@pad <buttons hex> <lx> <ly> <ms> [rx ry lt rt]` | fake XInput pad 0 (A=1000 B=2000 X=4000 Y=8000 START=0010); right stick turns the camera; triggers 0..255 (RT = fire) |
| `@mp <0/1>` | set the engine's multiplayer flag |
| `@finish_load` | finish a level load (same as the "press any key" after loading) |
| `@unpause_all` | pop every engine pause (a pause left from startup freezes the game clock) |
| `@net_host` | turn the loaded single-player level into a network host |
| `@players <n>` | resize the engine's net player list (`@players 1` drops a phantom slot) |
| `@link_actor [slot] [id hex]` | link a net player slot to an actor (default: slot 0 + the local player) |
| `@spawn_player [slot]` | host only: spawn a copy of the local player as the body of net slot `slot` (default 1) |
| `@teleport <dx> <dy> <dz> [id hex]` | move the entity this instance controls (or entity `id`) by an offset (metres) |
| `@fix_ammo <id hex\|s<slot>>` | attach the detached cloned ammo items near a body to it (runs by itself after spawns) |
| `@slotdump [slot]` | log the raw 0x70-byte `net_player` of `slot` (diagnostics) |
| `@respawn [slot]` | host: give net slot `slot` (default 1) a new body; runs by itself 5 s after that body dies |
| `@aim <id hex> [ms]` | turn this instance's view toward entity `id` with the fake right stick (yaw only) |
| `@title <text>` | rename this instance's window (`@host` / `@join` set `[host]` / `[client]`) |
| `@place left\|right` | windowed: move this window to the left edge, or right next to the `[host]` window on this PC (`@host` / `@join` do it, so two copies sit side by side) |
| `@inv` / `@links` / `@refs` / `@select` / `@detach` / `@setflag` / `@hexdump` | diagnostics for the weapon bug (FINDINGS 5.7) |
| `@goto <id hex> [dist]` | move the entity this instance controls `dist` m (default 2) in front of entity `id` |
| `@spawn <id hex>` | host only: spawn a copy of entity `id` through the engine's network spawn event (diagnostics) |
| `@dump [filter]` | log id, name, position and flags of entities whose name contains `filter` (diagnostics) |
| `@signal <name>` | raise a level signal |
| `@watch <rva hex>` / `@watch_slot <n>` / `@watch_net_time` | hardware write-watchpoints (diagnostics) |

Useful engine console commands: `disconnect`, `server`, `map 2033\l01_hunter`, `connect <ip>`, `gameload <save>`.

## Layout

| path | what |
|---|---|
| `src/engine.cpp` | all engine hooks and `@`-commands, with the RVAs and offsets they rely on |
| `src/dllmain.cpp` | config, logs, the `exec_on_start` runner, the heartbeat |
| `injector/` | `injector --launch <metro.exe>` (single-instance patch), `injector <exe> <dll>` (inject), and `launcher.cpp` = `MetroCoopLauncher.exe` (Host / Join window; `--host`, `--join <ip> [--second]` for scripts) |
| `scripts/` | test runner, ini helper, screenshots |
| `tools/re/` | static analysis helpers for the exe (Python + capstone): disassembly, strings, xrefs, callers, memory writers |
| `docs/FINDINGS.md` | everything learned about the engine, with RVAs, verified results and next steps |
| `third_party/minhook` | MinHook (BSD-2-Clause) |

## Working on this project

- This repo is the single source of truth. Code changes, findings (`docs/FINDINGS.md`) and, later, testable builds
  (as GitHub releases) go here.
- Keep it clean: only source that the build uses, no game files, saves, dumps, logs with local paths, or personal
  data. Verify a claim with a test run before writing it as "verified".

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD-2-Clause), vendored copy from MetroDeveloper.
- [MetroDeveloper](https://github.com/tsnest/MetroDeveloper) by its authors: its notes and its pre-Next-Gen
  `multiplayer_fix` pointed at the engine's leftover multiplayer.

## License

MIT, see [LICENSE](LICENSE). Third-party code keeps its own license (MinHook: BSD-2-Clause,
`third_party/minhook/LICENSE.txt`). Game files are not covered and are not part of this repo.
