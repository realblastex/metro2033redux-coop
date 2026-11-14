#pragma once
// Direct access to 4A engine functions (Metro 2033 Redux, Steam Next-Gen build): engine log capture + console commands.
#include <string>
#include <cstdint>

struct HWND__;  // windows.h HWND, without pulling windows.h into every includer

namespace metro_coop {

// Finds engine functions by pattern and hooks the engine line logger (slog) so every engine log line
// is appended to engine_log_path. Safe to call once; returns false if a pattern was not found.
bool engine_init(const std::string& engine_log_path);

// Queues one console command string; it runs on the game's window thread at its next PeekMessageA call
// (same console path the game uses for "-map"/"-server" on the command line). Any-thread safe.
bool engine_exec(const std::string& cmd);

// Keep the game running while its window is unfocused (no "GAME PAUSED"); needed for network play.
void engine_keep_active();

// The real foreground window (GetForegroundWindow is hooked when keep_active is on). Returns HWND as void*.
::HWND__* engine_real_foreground_window();

// One-line diagnostics for the heartbeat: MP flag (1 = network game), net frame count, players vector size.
std::string engine_stats();

// Arm a wait for an engine log line containing text; engine_wait_log_hit() turns true once one is logged.
void engine_wait_log(const std::string& text);
bool engine_wait_log_hit();

// Level starts seen since engine_init ("loader_finish: mapstate_running"; the main menu counts as one).
int engine_level_starts();

// Host of a network game whose own player is dead (health entity+0x308 <= 0).
bool engine_host_dead();
// Client in a network game with its own controlled body (false once the host is gone).
bool engine_client_in_session();
// A checkpoint (auto_save) was written while networked; such saves are written loadable (MP flag cleared).
bool engine_checkpoint_saved();
// This game runs the network server (it hosted; the ENet host survives story level changes).
bool engine_hosting();

// Changes the engine's local ENet bind port (default 1000). Needed for a second instance on the same PC.
bool engine_set_bind_port(uint16_t port);

// Host: when a peer connects, queue "@spawn_player <its slot>" delay_s seconds later (0 = off).
// engine_tick() (called from the DLL's loop) fires it.
void engine_set_auto_spawn(uint32_t delay_s);

// Queues one message on our own channel over the engine's ENet connection (to every peer; sent on the net thread
// at its next service call). type 1/2 = ping/pong self-check. Any-thread safe; false if the queue is full.
bool engine_msg_send(uint8_t type, const void* data, size_t len);
void engine_tick();

} // namespace metro_coop
