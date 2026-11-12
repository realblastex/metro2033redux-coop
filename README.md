# Metro 2033 Redux co-op

A work-in-progress co-op mod for **Metro 2033 Redux** (Steam, PC). Instead of writing our own netcode, it revives
the multiplayer code that 4A Games left inside `metro.exe` (ENet-based dev multiplayer) and makes it run a story
level with two players.

This repo holds the **clean source** and the **findings**. It is the project link for this game's entry in the
coop-anything directory. Game files are not included and never will be.

## Status (2026-11-12)

What works:

- Two game instances run on one PC. The second instance is allowed by `injector --launch`.
- The host turns a running single-player level into a network game. The client connects, loads the host's level
  from the host's quick save, and both stay in gameplay.
- The host's player character is linked to network slot 0. The host's movement replicates to the client.
- The joining player gets its own body (the host spawns it automatically 15 s after the join). The client sees the host's Artyom walk around
  from its own camera. The client walks its own body with its own input, and the host sees that body move.
- Both instances are driven without a human. A fake XInput pad and console commands run from a config file.
- Fast start takes about 30 s from launch to both players in-game.
- A second, armed level works: on `l19_defence` both players get their own body and rifle, and the client can fire.
  Three game crashes that assume one single player exist were guarded for this (FINDINGS 5.5).

What does not work yet:

- Both bodies spawn on the same spot; the client moves itself away with `@teleport`. See
  [docs/FINDINGS.md](docs/FINDINGS.md), section 5.
- NPCs near the players replicate from the host (distance priority, ~128 bytes per packet); far NPCs did not update
  within 15 s. An active monster moved on the host shows up on the client. Entities the host spawns (`@spawn`) exist on
  both sides with the same id.
- The client's body has no knife (the torch is there). Doors are not in the network list; door use between players is
  untested. Damage, enemy AI and monster fights are untested (Hunter starts unarmed; in `l19_defence` no monster
  attacked within 2.5 min; a spawned monster stays asleep).
- No AI or story-event sync has been checked. No level transitions. Not tested over LAN or the Internet, only
  over loopback.

QA level: manual bot runs (scripted, log-checked). There is no automated suite yet. Longest run: ~4 min of co-op,
24 moves, both alive, final positions equal on both sides.

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

Output: `build\Release\metro_coop.dll` and `build\Release\injector.exe`.

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
5. Check the heartbeat lines. Per instance, logs are in `%LOCALAPPDATA%\MetroCoop\<game folder name>\`:
   `metro_coop.log` (ours) and `engine.log` (every engine log line).

Windows Firewall asks about network access the first time `metro.exe` binds UDP port 1000. Loopback works without
allowing it. LAN play will need it allowed.

## Try it by hand (two copies on one PC)

1. Start both copies with `injector --launch` and inject the DLL (as `scripts\run_test.ps1` does). Set
   `engine_bind_port = 1001` in the second copy's `metro_coop.ini`.
2. Copy 1: load a save (or start a new game) and wait until you are in gameplay. Press **F9**: it becomes the host.
3. Copy 2: at the main menu press **F11**: it joins `join_address` (default `127.0.0.1`), loads the host's level and
   gets its own body about 15 s later.

Verified 10:30 with key presses sent by a script (F9 on the host, F11 on the client). The hotkeys only react while
that copy's window is in front. F10 is not usable: the game itself reacts to it, and a join started with F10 hung the
client.

## Config: `metro_coop.ini`

| key | default | meaning |
|---|---|---|
| `exec_on_start` | empty | `;`-separated engine console commands and `@`-commands, run in order |
| `exec_delay_s` | 5 | seconds after injection before `exec_on_start` starts |
| `keep_active` | true | keep running while the window is unfocused |
| `engine_bind_port` | 0 | engine UDP port (0 = 1000); use 1001 for a second instance on the same PC |
| `join_address` | 127.0.0.1 | host address for F11 |
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
| `injector/` | `injector --launch <metro.exe>` (single-instance patch) and `injector <exe> <dll>` (inject) |
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
