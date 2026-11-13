// metro_coop.dll entry point. Loads the per-install config, starts our log + the engine log capture, installs the
// engine hooks (engine.cpp), then runs a 16 ms loop that executes `exec_on_start` and logs a heartbeat every ~10 s.
//
// Mod folder = the folder that holds metro_coop.dll (normally <game dir>\metro_coop\):
//   metro_coop.ini  config (created with defaults on first run; the launcher writes mode/join_address/engine_bind_port)
//   metro_coop.log  our log (heartbeat, @-command results)
//   engine.log      every line the engine logs, timestamped
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

#include "engine.h"
#include "logger.h"

namespace metro_coop {

struct Config {
    std::string exec_on_start;      // ';'-separated engine console commands and @-commands (see README)
    uint32_t exec_delay_s = 5;      // delay after injection before exec_on_start starts
    bool keep_active = true;        // keep running while the window is unfocused (network play needs this)
    uint16_t engine_bind_port = 0;  // 0 = engine default 1000; e.g. 1001 for a second instance on the same PC
    uint32_t auto_spawn_s = 15;     // host: spawn a body for a joining player this many seconds after it connects (0 = off)
    std::string join_address = "127.0.0.1";  // F11 joins this host
    std::string mode;               // set by the launcher: "host" = host once a level is loaded, "join" = join join_address
                                    // once the main menu is up; empty = only exec_on_start and the F9/F11 hotkeys
};

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "key = value" lines; lines starting with ';' or '#' are comments (values may contain ';').
static Config load_config(const std::string& path) {
    Config c;
    std::ifstream in(path);
    if (!in) {
        std::ofstream out(path);
        // [coop] lets the launcher use Get/WritePrivateProfileString; load_config skips the section line (no '=')
        out << "[coop]\n; Metro 2033 Redux co-op - see README for exec_on_start commands\n"
               "exec_on_start = \nexec_delay_s = 5\nkeep_active = true\nengine_bind_port = 0\nauto_spawn_s = 15\n"
               "join_address = 127.0.0.1\nmode = \n";
        return c;
    }
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "exec_on_start") c.exec_on_start = v;
        else if (k == "exec_delay_s") c.exec_delay_s = (uint32_t)strtoul(v.c_str(), nullptr, 0);
        else if (k == "keep_active") c.keep_active = v == "true" || v == "1";
        else if (k == "engine_bind_port") c.engine_bind_port = (uint16_t)strtoul(v.c_str(), nullptr, 0);
        else if (k == "auto_spawn_s") c.auto_spawn_s = (uint32_t)strtoul(v.c_str(), nullptr, 0);
        else if (k == "join_address") c.join_address = v;
        else if (k == "mode") c.mode = v;
    }
    return c;
}

static std::atomic<bool> g_running{false};

// Runs exec_on_start one command at a time. Handled here: "@wait <s>", "@wait_log <text> [timeout_s]" (until an
// engine log line contains <text>, default 120 s) and "@skip_video [timeout_s]" (pad A every second until the engine
// logs "^skip_video"). Everything else goes to engine_exec (console commands and the other @-commands).
class ExecRunner {
public:
    explicit ExecRunner(const Config& c) : cmds_(c.exec_on_start), next_ms_(GetTickCount64() + c.exec_delay_s * 1000ull) {}

    void tick() {
        uint64_t now = GetTickCount64();
        if (wait_deadline_) {
            if (engine_wait_log_hit() || now >= wait_deadline_) {
                LOG_INFO(std::string("exec: log wait ") + (engine_wait_log_hit() ? "done" : "TIMED OUT"));
                wait_deadline_ = next_press_ = 0;
                engine_wait_log("");
            } else if (next_press_ && now >= next_press_) {
                engine_exec("@pad 1000 0 0 200");
                next_press_ = now + 1000;
            }
            return;
        }
        while (now >= next_ms_ && pos_ < cmds_.size()) {
            size_t q = cmds_.find(';', pos_);
            if (q == std::string::npos) q = cmds_.size();
            std::string c = trim(cmds_.substr(pos_, q - pos_));
            pos_ = q + 1;
            // macros: "@host" (on a loaded single-player level) and "@join <ip>" expand in place
            std::string macro;
            // "gamesave coop_session" first: what a host death reloads until the first checkpoint while hosting
            if (c == "@host") macro = "@title Metro Redux [host];@place left;gamesave coop_session;@wait 1;@net_host;@wait 1;@players 1;@link_actor";
            else if (c.rfind("@join ", 0) == 0)
                macro = "@title Metro Redux [client];connect " + trim(c.substr(6)) +
                        ";@wait_log mapstate_loaded_and_waiting 60;@mp 1;@wait 1;@finish_load;@wait 1;@unpause_all"
                        // both bodies spawn on one spot (the client then sees the host's gun inside its own view):
                        // once our body arrives, step 1.2 m in front of the host (open in Hunter and l19; behind the
                        // Hunter start is a wall)
                        ";@wait_log pid== 60;@wait 1;@goto s0 1.2;@place right";
            if (!macro.empty()) {
                if (pos_ > cmds_.size()) pos_ = cmds_.size();  // macro was the last command
                cmds_.insert(pos_, macro + ";");
                continue;
            }
            if (c.rfind("@wait ", 0) == 0) { next_ms_ = now + atoi(c.c_str() + 6) * 1000ull; return; }
            bool skip = c.rfind("@skip_video", 0) == 0;
            if (skip || c.rfind("@wait_log ", 0) == 0) {
                std::string text = skip ? "^skip_video" : c.substr(10);
                unsigned timeout = 120;
                if (skip) {
                    if (c.size() > 12) timeout = atoi(c.c_str() + 12);
                } else if (size_t sp = text.find_last_of(' ');
                           sp != std::string::npos && sp + 1 < text.size() &&
                           text.find_first_not_of("0123456789", sp + 1) == std::string::npos) {
                    timeout = atoi(text.c_str() + sp + 1);  // trailing number = timeout
                    text.resize(sp);
                }
                LOG_INFO("exec: waiting for log '" + text + "' (" + std::to_string(timeout) + " s)");
                engine_wait_log(text);
                wait_deadline_ = now + timeout * 1000ull;
                next_press_ = skip ? now : 0;
                return;
            }
            if (!c.empty()) engine_exec(c);
        }
    }

    // runs right after the command in progress (a running @wait / @wait_log finishes first)
    void insert_next(const std::string& c) {
        if (pos_ > cmds_.size()) pos_ = cmds_.size();
        cmds_.insert(pos_, c + ";");
    }

    // runs after whatever is still queued (hotkeys)
    void push(const std::string& c) {
        if (pos_ > cmds_.size()) pos_ = cmds_.size();
        if (!cmds_.empty() && cmds_.back() != ';') cmds_ += ';';
        cmds_ += c;
    }

private:
    std::string cmds_;
    size_t pos_ = 0;
    uint64_t next_ms_, wait_deadline_ = 0, next_press_ = 0;
};

static HMODULE g_module = nullptr;

// Folder of metro_coop.dll, with a trailing backslash: config and logs live next to the DLL (in the game folder).
static std::string instance_dir() {
    char dll[MAX_PATH] = {};
    GetModuleFileNameA(g_module, dll, MAX_PATH);
    std::string dir(dll);
    size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? ".\\" : dir.substr(0, slash + 1);
}

static DWORD WINAPI main_thread(LPVOID) {
    std::string dir = instance_dir();
    Logger::instance().init(dir + "metro_coop.log");
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    LOG_INFO(std::string("metro_coop starting in ") + exe);
    Config cfg = load_config(dir + "metro_coop.ini");
    LOG_INFO("exec_on_start: '" + cfg.exec_on_start + "' after " + std::to_string(cfg.exec_delay_s) + " s");

    Sleep(2000);  // as in the tested builds: let the game finish starting before hooking
    if (!engine_init(dir + "engine.log")) {
        LOG_ERROR("engine_init failed - wrong metro.exe build? (expects SHA-256 097df1a2...)");
        return 0;
    }
    if (cfg.engine_bind_port) engine_set_bind_port(cfg.engine_bind_port);
    if (cfg.keep_active) engine_keep_active();
    engine_set_auto_spawn(cfg.auto_spawn_s);

    ExecRunner exec(cfg);
    // shortcut: "host at the first level start after the menu" assumes injection before the menu loads (the launcher
    // injects ~3 s after launch, the menu comes ~14 s later); if injection is late, press F9 in the level
    bool auto_done = cfg.mode != "host" && cfg.mode != "join";
    // A host restarted after a death (or a test) loads this save itself: past the title screen, then gameload; the
    // auto host below fires once the loaded level runs.
    char load[64] = {};
    if (cfg.mode == "host" && GetEnvironmentVariableA("METRO_COOP_LOAD", load, sizeof(load)))
        exec.push("@wait_log playing video 30;@wait 1;@pad 1000 0 0 200;@wait 2;@pad 0010 0 0 200;@wait 2;"
                  "@pad 1000 0 0 200;@wait 2;gameload " + std::string(load) + ";@wait_log load save bin 30;@wait 1;@finish_load");
    if (!auto_done) LOG_INFO("mode: " + cfg.mode);
    uint64_t start_ms = GetTickCount64();
    g_running = true;
    for (uint64_t tick = 1; g_running; ++tick) {
        // F9 = host (on a loaded single-player level), F11 = join join_address (F10 hung the client during connect: the game uses it); only while our window is in front
        static bool f9 = false, f11 = false;
        bool front = false;
        DWORD pid = 0;
        if (HWND fg = (HWND)engine_real_foreground_window()) GetWindowThreadProcessId(fg, &pid);
        front = pid == GetCurrentProcessId();
        static uint64_t join_at = 0;  // when join mode connected (0 = not yet)
        bool k9 = front && (GetAsyncKeyState(VK_F9) & 0x8000), k11 = front && (GetAsyncKeyState(VK_F11) & 0x8000);
        if (k9 && !f9) { LOG_INFO("hotkey F9: host"); exec.push("@host"); }
        if (k11 && !f11) { LOG_INFO("hotkey F11: join " + cfg.join_address); exec.push("@join " + cfg.join_address); }
        f9 = k9; f11 = k11;
        // the menu is the 1st level start, but only once someone presses a key at the title screen; joining from the
        // title screen works too (smoke test), so "join" also fires after ~25 s
        if (!auto_done && (engine_level_starts() >= (cfg.mode == "host" ? 2 : 1) ||
                           (cfg.mode == "join" && GetTickCount64() - start_ms >= 25000))) {
            auto_done = true;
            LOG_INFO("auto " + cfg.mode);
            exec.insert_next(cfg.mode == "host" ? "@wait 2;@unpause_all;@host" : "@wait 2;@join " + cfg.join_address);
            if (cfg.mode == "join") join_at = GetTickCount64();
        }
        // Restart this game through the launcher (args "--host" / "--join <ip> [--second]"); the new game inherits
        // our environment (METRO_COOP_*). A fresh process is the only reliable way back into a network game: a
        // second in-process reload crashed (RVA 0x3175A) and an in-process reconnect never loads the level (FINDINGS 5.9).
        auto restart = [&](const std::string& args) {
            std::string cmd = "\"" + dir + "MetroCoopLauncher.exe\" " + args;
            STARTUPINFOA si = {sizeof(si)};
            PROCESS_INFORMATION pi = {};
            LOG_INFO("restarting: " + cmd);
            if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                TerminateProcess(GetCurrentProcess(), 0);
            }
            LOG_ERROR("launcher start failed: " + std::to_string(GetLastError()));
        };
        uint64_t now = GetTickCount64();
        // Host died: restart and load the last checkpoint this game wrote while hosting ("auto_save", written
        // loadable since FINDINGS 5.10), else "coop_session" (written by @host). The client loses the host and
        // restarts too (below).
        // shortcut: a down/revive system may replace this later.
        static uint64_t dead_since = 0;
        if (engine_host_dead()) {
            if (!dead_since) dead_since = now;
            if (now - dead_since > 4000) {
                const char* save = engine_checkpoint_saved() ? "auto_save" : "coop_session";
                LOG_INFO(std::string("host died: restarting from ") + save);
                SetEnvironmentVariableA("METRO_COOP_LOAD", save);
                restart("--host");
                dead_since = 0;
            }
        } else dead_since = 0;
        // Client lost the host, or join mode got no session within 90 s (the host wasn't up yet): restart in join mode.
        static bool in_session = false;
        static uint64_t lost_since = 0;
        // restarts in a row without a session; inherited by the restarted game. Cap 3 (about 5 min) so a host that
        // quit for good doesn't keep the client restarting.
        static int retries = [] { char v[8] = {}; return GetEnvironmentVariableA("METRO_COOP_RETRY", v, 8) ? atoi(v) : 0; }();
        if (engine_client_in_session()) in_session = true, lost_since = 0, retries = 0;
        else if (retries < 3 && (in_session || (join_at && now - join_at > 90000))) {
            if (!lost_since) lost_since = now;
            if (now - lost_since > 5000) {
                SetEnvironmentVariableA("METRO_COOP_RETRY", std::to_string(in_session ? 1 : retries + 1).c_str());
                restart("--join " + cfg.join_address + (cfg.engine_bind_port ? " --second" : ""));
                in_session = false;
                join_at = 0;
            }
        }
        exec.tick();
        engine_tick();
        if (tick % 625 == 0) LOG_INFO("heartbeat: " + engine_stats());  // ~10 s; tests read state from here
        Sleep(16);
    }
    return 0;
}

}  // namespace metro_coop

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        metro_coop::g_module = module;
        // never do real work under the loader lock: start a thread instead
        if (HANDLE t = CreateThread(nullptr, 0, metro_coop::main_thread, nullptr, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        metro_coop::g_running = false;
    }
    return TRUE;
}
