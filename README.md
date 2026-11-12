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
- The joining player gets its own body (`@spawn_player 1` on the host). The client sees the host's Artyom walk around
  from its own camera.
- Both instances are driven without a human. A fake XInput pad and console commands run from a config file.
- Fast start takes about 30 s from launch to both players in-game.

What does not work yet:

- The client's own input does not move its body yet. Both bodies spawn on the same spot. See
  [docs/FINDINGS.md](docs/FINDINGS.md), section 5.
- No AI, door or story-event sync has been checked. No level transitions. Not tested over LAN or the Internet, only
  over loopback.

QA level: manual bot runs (scripted, log-checked). There is no automated suite yet.

Verified test run (log lines from `metro_coop.log`, host then client):

```
host   engine: link_actor check: resolves to ...8710 'player'
host   heartbeat: ... local_idx=0 s0=0/5/(-3.5,-2.6,-15.9)   <- host pad forward moved slot 0 from x=-7.3
client heartbeat: ... local_idx=1 s0=0/5/(-5.1,-2.6,-15.7)   <- client sees the same slot move
host   engine: spawn_player 20 entities, root new id 0x10B
client [net] pid==1, eid==267                                  <- engine.log: client got its body (0x10B)
client heartbeat: ... local_idx=1 s0=0/5/(-3.5,-2.6,-15.9) s1=10B/5/(-7.3,-2.6,-16.0)
```

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
   scripts\set_ini.ps1 host "exec_on_start=@wait_log playing video 30;@wait 1;@pad 1000 0 0 200;@wait 2;@pad 0010 0 0 200;@wait 2;@pad 1000 0 0 200;@wait 2;gameload coop_start;@wait_log load save bin 30;@wait 1;@finish_load;@wait 1;@unpause_all;@wait 4;@net_host;@wait 1;@players 1;@link_actor;@wait_log incoming connection 60;@wait 15;@spawn_player 1;@wait 5;@pad 0 0 32000 4000" exec_delay_s=0
   scripts\set_ini.ps1 client "exec_on_start=@wait_log playing video 30;@wait 20;connect 127.0.0.1;@wait_log mapstate_loaded_and_waiting 60;@mp 1;@wait 1;@finish_load;@wait 1;@unpause_all;@wait 35;@pad 0 0 -32000 6000" exec_delay_s=0 engine_bind_port=1001
   scripts\run_test.ps1 -Client -Wait 105
   ```
5. Check the heartbeat lines. Per instance, logs are in `%LOCALAPPDATA%\MetroCoop\<game folder name>\`:
   `metro_coop.log` (ours) and `engine.log` (every engine log line).

Windows Firewall asks about network access the first time `metro.exe` binds UDP port 1000. Loopback works without
allowing it. LAN play will need it allowed.

## Config: `metro_coop.ini`

| key | default | meaning |
|---|---|---|
| `exec_on_start` | empty | `;`-separated engine console commands and `@`-commands, run in order |
| `exec_delay_s` | 5 | seconds after injection before `exec_on_start` starts |
| `keep_active` | true | keep running while the window is unfocused |
| `engine_bind_port` | 0 | engine UDP port (0 = 1000); use 1001 for a second instance on the same PC |

`@`-commands:

| command | what it does |
|---|---|
| `@wait <s>` | pause the list |
| `@wait_log <text> [timeout_s]` | pause until an engine log line contains `<text>` (default 120 s) |
| `@skip_video [timeout_s]` | press pad A every second until a video is skipped |
| `@pad <buttons hex> <lx> <ly> <ms>` | fake XInput pad 0 (A=1000 B=2000 X=4000 Y=8000 START=0010) |
| `@mp <0/1>` | set the engine's multiplayer flag |
| `@finish_load` | finish a level load (same as the "press any key" after loading) |
| `@unpause_all` | pop every engine pause (a pause left from startup freezes the game clock) |
| `@net_host` | turn the loaded single-player level into a network host |
| `@players <n>` | resize the engine's net player list (`@players 1` drops a phantom slot) |
| `@link_actor [slot] [id hex]` | link a net player slot to an actor (default: slot 0 + the local player) |
| `@spawn_player [slot]` | host only: spawn a copy of the local player as the body of net slot `slot` (default 1) |
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
