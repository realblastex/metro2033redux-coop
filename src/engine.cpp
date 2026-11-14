// 4A engine access. RVAs are for metro_nextgen_patched.exe (SHA-256 097df1a2...); functions are found by pattern.
//   slog       RVA 0x65C0C0  void slog(const char* line)            - raw line logger, called by rlog (printf) at 0x65C460
//   GetConsole RVA 0x667E40  console* GetConsole()                  - global at RVA 0xFBD770
//   console->vtbl[0x40/8](console, const char* cmd)                 - execute command string (used for "-map"/"-server")
#include "engine.h"
#include "logger.h"
#include "MinHook.h"

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <intrin.h>
#include <share.h>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <optional>
#include <bitset>

namespace metro_coop {

static FILE* g_engine_log = nullptr;
static std::mutex g_engine_log_mutex;
using slog_t = void(__fastcall*)(const char*);
static slog_t g_slog_orig = nullptr;
using get_console_t = void*(__fastcall*)();
static get_console_t g_get_console = nullptr;
using peek_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
static peek_t g_peek_orig = nullptr;
static BOOL WINAPI peek_hook(LPMSG m, HWND h, UINT a, UINT b, UINT r);
static bool install_player_slot_fix();
static void install_netframe_counter();
static void engine_watch_net_time();
void engine_watch_write(uintptr_t addr);
static void pad_command(const std::string& args);
static void aim_command(unsigned id, unsigned ms);
static LONG CALLBACK av_veh(EXCEPTION_POINTERS* e);
static std::atomic<int> g_spawn_slot{-1};      // pending body spawn (engine_tick runs it at g_spawn_at)
static std::atomic<uint64_t> g_spawn_at{0};
static std::atomic<bool> g_spawn_force{false};  // respawn: spawn even though the slot still has a (dead) actor
// Engine MP flag (RVA 0xFBDF2A). net start (RVA 0x6B7EA0): with an address arg it sets the flag and connects as a
// client; without one it becomes the network server only if this flag is already 1, else a local-only game.
// So hosting = "@mp 1" then "server" (no arg).
static uint8_t* g_mp_flag = nullptr;
static std::atomic<bool> g_host_gone{false};  // client: the host left (ENet disconnect) or changed the level
// Client: the host changed the level (change_map_cl). Until the host says its new level is ready (MSG_LEVEL), the
// ENet link stays up for our channel but engine packets are dropped both ways, so neither side applies the other
// level's state. Then dllmain runs "disconnect; @join" in this process (FINDINGS 5.13).
static std::atomic<bool> g_level_follow{false};
static std::atomic<bool> g_level_ready{false};
// net_core global (RVA 0xFD5020): +0 ENetHost* host (1 = local-only marker), +8 local player index, +0x18 players.
struct PlayersVec { void* array; uint16_t capacity; uint16_t size; };
static PlayersVec* g_players = nullptr;
using resize_t = bool(__fastcall*)(PlayersVec*, uint32_t);
static resize_t g_resize = nullptr;  // RVA 0x4E8B0, constructs/destroys entries
// Every game save (console "gamesave [name]" = RVA 0x53D70, checkpoints from RVA 0x519C0) is
// (*g_savemgr)->vtbl[0x110](g_savemgr, name), g_savemgr at RVA 0xFBD7A8. A save written with the MP flag set
// can't be loaded (crash RVA 0x3174B/0x3175A); one written with the flag cleared loads (FINDINGS 5.10). The call
// only queues the save (RVA 0x672130: name at +0x1D98, state +0x1D90 = 1); the next frame serializes it and logs
// "total : gamesave", so the flag stays cleared until that line (slog_hook) or 15 s (engine_tick).
using save_t = uintptr_t(__fastcall*)(void*, const char*);
static save_t g_save_orig = nullptr;
static std::atomic<uint64_t> g_save_since{0};  // != 0 while a save runs with the MP flag cleared
static std::atomic<bool> g_checkpoint_saved{false};  // a loadable auto_save was written while networked
static void save_restore_flag(const char* why) {
    uint64_t t = g_save_since.exchange(0);
    if (!t) return;
    *g_mp_flag = 1;
    LOG_INFO(std::string("engine: save done (") + why + "), mp flag back after " + std::to_string(GetTickCount64() - t) + " ms");
}
// Ids of the joiner's body (root + children, from @spawn_player). The save loop (RVA 0x3CD210) leaves them out: a
// hosted save that held the body crashed on load (AV at RVA 0x108BE89). Patch: at RVA 0x3CD59C (per-entity
// "save or skip" decision, entity in rsi) a stub asks save_skip_body and jumps to the loop's next entity
// (RVA 0x3CD9AD) for body entities, else runs the two replaced instructions and continues.
static std::bitset<65536> g_body_ids;
static bool __fastcall save_skip_body(uint8_t* ent) {
    // only game saves: the same loop writes the qsave a joining client loads, which must hold the body
    return g_save_since && g_body_ids.test(*(uint16_t*)(ent + 0x108));  // entity id (handle at entity+0xA0, +0x68)
}
static void install_save_skip(uint8_t* exe) {
    uint8_t* at = exe + 0x3CD59C;
    if (memcmp(at, "\x45\x85\xE4\x0F\x84\x43\x01\x00\x00", 9) != 0) { LOG_ERROR("engine: save loop bytes differ"); return; }
    uint8_t* stub = nullptr;  // within +-2 GB of the exe for a rel32 jmp
    for (uintptr_t a = ((uintptr_t)exe - 0x100000) & ~0xFFFFull; !stub && a > (uintptr_t)exe - 0x70000000; a -= 0x10000)
        stub = (uint8_t*)VirtualAlloc((void*)a, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) { LOG_ERROR("engine: no stub memory near the exe"); return; }
    uint8_t code[] = {
        0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,  // push rax rcx rdx r8 r9 r10 r11
        0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xE4, 0xF0, 0x48, 0x83, 0xEC, 0x20,  // push rbp; mov rbp,rsp; align; shadow
        0x48, 0x89, 0xF1,                                      // mov rcx, rsi
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                    // mov rax, save_skip_body   (offset 28)
        0xFF, 0xD0, 0x48, 0x89, 0xEC, 0x5D, 0x84, 0xC0,        // call rax; mov rsp,rbp; pop rbp; test al,al
        0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58,  // pops (flags kept)
        0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // jz +14; jmp [0x3CD9AD]   (offset 63)
        0x45, 0x85, 0xE4, 0x75, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // test r12d; jnz +14; jmp [0x3CD6E8] (offset 82)
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,        // jmp [0x3CD5A5]   (offset 96)
    };
    auto put = [&](size_t off, const void* v) { memcpy(code + off, &v, 8); };
    put(28, (const void*)&save_skip_body);
    put(63, exe + 0x3CD9AD);
    put(82, exe + 0x3CD6E8);
    put(96, exe + 0x3CD5A5);
    memcpy(stub, code, sizeof(code));
    DWORD old;
    VirtualProtect(at, 9, PAGE_EXECUTE_READWRITE, &old);
    uint8_t jmp[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = (int32_t)(stub - (at + 5));
    memcpy(jmp + 1, &rel, 4);
    memcpy(at, jmp, 9);
    VirtualProtect(at, 9, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 9);
    LOG_INFO("engine: save loop skips the joiner's body");
}
// Level change worker RVA 0x540C0 ("disconnect 1; [server 1;] map <level>"): story transitions run console
// "change_map <level>"; a host keeps its ENet session through it, and its net code then sends clients
// "change_map_cl <level>". A client that follows loads the level itself and crashed applying the level's
// .player data (AV RVA 0x3CA468, FINDINGS 5.11). So a client in a session skips it and counts the host as gone:
// dllmain restarts it and it joins the host's new level like after a host death.
// Session 13: instead it follows in this process (g_level_follow above); the restart is the fallback.
static int local_slot();
static void act_run_local(const std::string& args);  // "@act <0|1> <ordinal> <name>"
static void act_pending_run();
static void msg_forget_peers();
using change_map_t = void(__fastcall*)(const char*);
static change_map_t g_change_map_orig = nullptr;
static void __fastcall change_map_hook(const char* level) {
    // a second call follows after the net state was reset (verified), so once rejoining, block every one
    // The MP flag is already 0 here (the client's level-change code clears it, expecting the worker to load the
    // level). The net frame (RVA 0x6BB150) only runs while it is set, so put it back: our channel must keep running.
    if (g_level_follow || (g_mp_flag && local_slot() >= 1 && g_players && g_players->size >= 2)) {
        LOG_INFO(std::string("engine: host changes level to ") + (level ? level : "?") + ", following (mp was " +
                 std::to_string(g_mp_flag ? *g_mp_flag : -1) + ")");
        g_level_follow = true;
        if (g_mp_flag) *g_mp_flag = 1;
        return;
    }
    LOG_INFO(std::string("engine: change level to ") + (level ? level : "?"));
    g_change_map_orig(level);
}
// RVA 0x3CA400 (virtual, no direct callers) replaces the controlled player *(g_level+0x38) with the one in a level's
// "<level>.player" carry-over data (chunk_player_data). A client loading the host's level after a story level
// change has no controlled entity yet and crashed on the null (AV RVA 0x3CA468); its body comes from the host.
using player_data_t = uintptr_t(__fastcall*)(void*, void*, int);
static player_data_t g_player_data_orig = nullptr;
static uintptr_t __fastcall player_data_hook(void* a, void* data, int flag) {
    uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);  // shortcut: fixed RVA (g_level)
    if (lvl && !*(void**)(lvl + 0x38)) {
        LOG_INFO("engine: level player data skipped (no controlled entity)");
        return 1;
    }
    // Host on a story level change: the record creator RVA 0x6A5B90 gives the carried-over items low ids
    // (0x0..0x23: record ids < 0x100 kept, children from the low pool RVA 0x4B170), and a client joining that
    // world crashed in the qsave load (FINDINGS 5.11). While hosting, three patches send every record through the
    // normal allocator RVA 0x4BBE0 (ids >= 0x100, old->new pair in the remap map) and remap low parent ids too.
    struct { uint32_t rva; const char* orig; const char* patch; int n; } p[] = {
        {0x6A5DA3, "\x0F\x86\x9D\x01\x00\x00", "\x90\x90\x90\x90\x90\x90", 6},  // id < 0x100: no keep-as-is branch
        {0x6A5E23, "\x74\x0E", "\xEB\x0E", 2},                                // child: 0x4BBE0, not the low pool
        {0x6A5F8C, "\xB9\x00\x01\x00\x00", "\xB9\x00\x00\x00\x00", 5},        // parent remap from id 0, not 0x100
    };
    uint8_t* exe = (uint8_t*)GetModuleHandleA(nullptr);
    bool patch = engine_hosting();
    for (auto& q : p) patch = patch && memcmp(exe + q.rva, q.orig, q.n) == 0;
    if (engine_hosting() && !patch) {
        LOG_INFO("engine: level player data skipped (hosting, unknown exe bytes)");
        return 1;
    }
    auto put = [&](bool on) {
        for (auto& q : p) {
            DWORD old;
            VirtualProtect(exe + q.rva, q.n, PAGE_EXECUTE_READWRITE, &old);
            memcpy(exe + q.rva, on ? q.patch : q.orig, q.n);
            VirtualProtect(exe + q.rva, q.n, old, &old);
        }
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    };
    if (patch) put(true);
    uintptr_t r = g_player_data_orig(a, data, flag);
    if (patch) {
        put(false);
        LOG_INFO("engine: level player data applied with new ids (hosting)");
    }
    return r;
}
static uintptr_t __fastcall save_hook(void* mgr, const char* name) {
    uint8_t mp = g_mp_flag ? *g_mp_flag : 0;
    // A joiner's game is a copy of the host's world: its saves would replace the player's own progress with an
    // unloadable or meaningless save, so a client in a session writes none.
    if (mp && *(int32_t*)((uint8_t*)GetModuleHandleA(nullptr) + 0xFD5028) > 0) {  // net_core local index
        LOG_INFO(std::string("engine: save '") + (name ? name : "?") + "' skipped (client)");
        return 0;
    }
    // Checkpoints while hosting go to their own slot so the player's real "auto_save" keeps their own progress
    if (mp && name && !strcmp(name, "auto_save")) name = "coop_auto_save";
    // RVA 0x672130 drops a request while +0x1D90 is busy, so only a request that gets queued clears the flag
    int busy = *(int*)((uint8_t*)mgr + 0x1D90);
    LOG_INFO(std::string("engine: save '") + (name ? name : "?") + "' mp=" + std::to_string(mp) + " state=" + std::to_string(busy));
    if (name && !strcmp(name, "coop_session")) g_checkpoint_saved = false;  // @host: new restart point
    uintptr_t r = g_save_orig(mgr, name);
    if (mp && !g_save_since && *(int*)((uint8_t*)mgr + 0x1D90) == 1 && busy != 1) {
        *g_mp_flag = 0;
        g_save_since = GetTickCount64();
        if (name && !strcmp(name, "coop_auto_save")) g_checkpoint_saved = true;
    }
    return r;
}
static void install_save_hook() {
    static bool done = false;
    uint8_t* exe = (uint8_t*)GetModuleHandleA(nullptr);
    // shortcut: fixed RVA for this exe build (checked against the gamesave handler's bytes)
    if (done || memcmp(exe + 0x53D81, "\x48\x8B\x0D", 3) != 0) return;
    void** mgr = *(void***)(exe + 0xFBD7A8);
    if (!mgr) return;
    void* fn = ((void**)*mgr)[0x110 / 8];
    done = true;  // one attempt
    install_save_skip(exe);
    if (memcmp(exe + 0x540C0, "\x40\x53\x48\x83\xEC\x20\x48\x8B\xD9", 9) == 0 &&
        MH_CreateHook(exe + 0x540C0, (LPVOID)&change_map_hook, (LPVOID*)&g_change_map_orig) == MH_OK &&
        MH_EnableHook(exe + 0x540C0) == MH_OK)
        LOG_INFO("engine: level change hook installed");
    else LOG_ERROR("engine: level change hook failed");
    if (memcmp(exe + 0x3CA400, "\x48\x8B\xC4\x55\x41\x55\x41\x56\x41\x57", 10) == 0 &&
        MH_CreateHook(exe + 0x3CA400, (LPVOID)&player_data_hook, (LPVOID*)&g_player_data_orig) == MH_OK &&
        MH_EnableHook(exe + 0x3CA400) == MH_OK)
        LOG_INFO("engine: level player data guard installed");
    else LOG_ERROR("engine: level player data guard failed");
    bool ok = MH_CreateHook(fn, (LPVOID)&save_hook, (LPVOID*)&g_save_orig) == MH_OK && MH_EnableHook(fn) == MH_OK;
    char b[96];
    snprintf(b, sizeof(b), "engine: save hook at RVA 0x%llX %s", (unsigned long long)((uint8_t*)fn - exe), ok ? "ok" : "FAILED");
    LOG_INFO(b);
}
// After a map loads the engine waits in "mapstate_loaded_and_waiting" (game paused, so net isn't serviced) until a
// key press calls loader_finish(&g_loader) (RVA 0x69E4C0, g_loader RVA 0xFBDEA0; input callback at RVA 0x69E310).
// "@finish_load" makes that call directly.
using loader_finish_t = void(__fastcall*)(void*);
static loader_finish_t g_loader_finish = nullptr;
static void* g_loader = nullptr;
using pause_pop_t = void(__fastcall*)(void*, void**);
static pause_pop_t g_pause_pop = nullptr;  // RVA 0x664380 POP PAUSE(engine, &reason)
// Level signals: igame_level_signal(nullptr, &name_handle, nullptr, 0) (RVA 0x70EE60); name handles come from the
// string container: str_shared(*g_strc (RVA 0xFBD588), chars, len, 0) (RVA 0x656DB0).
using str_shared_t = void*(__fastcall*)(void*, const char*, uint32_t, uint64_t);
static str_shared_t g_str_shared = nullptr;
static void** g_strc = nullptr;
using level_signal_t = void(__fastcall*)(void*, void**, void*, uint32_t);
static level_signal_t g_level_signal = nullptr;
using net_start_t = uint64_t(__fastcall*)(void*, const char*);
static net_start_t g_net_start = nullptr;  // RVA 0x6B7EA0 (this, address or nullptr)
// link_actor(net_player*, u16 entity id) (RVA 0x6B89F0): set_actor_id (RVA 0x4C330: +0x64 = id, drops +0x58 handle)
// and slot state |= 0x50000. The server's player spawner (RVA 0x6F6210) calls it after spawning a player entity,
// but only when net_core+8 == 0 (server), so a single-player level never links its actor to slot 0.
using link_actor_t = void(__fastcall*)(void*, uint16_t);
static link_actor_t g_link_actor = nullptr;
using actor_of_t = void***(__fastcall*)(void*, int);
static actor_of_t g_actor_of = nullptr;  // RVA 0x6B93F0: resolves +0x64 via g_server into the +0x58 handle, returns &+0x58
static uint8_t* g_seen_actors[4] = {};   // actors seen by aim_hook

// "@wait_log <text>": dllmain's command runner arms this and waits until an engine log line contains <text>.
static std::mutex g_wait_mutex;
static std::string g_wait_text;
static std::atomic<bool> g_wait_hit{false};
void engine_wait_log(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_wait_mutex);
    g_wait_text = text;
    g_wait_hit = false;
}
bool engine_wait_log_hit() { return g_wait_hit; }

// Count of "loader_finish: mapstate_running" lines = level starts since injection (the main menu is one too).
static std::atomic<int> g_level_starts{0};
int engine_level_starts() { return g_level_starts; }
static std::atomic<int> g_level_loads{0};  // "loading content\maps\<level>\level": a level load begins (before its blocks)

// Name of an entity (+0x110 -> string object, chars at +0x14); "?" if unreadable. Diagnostics only.
static const char* entity_name(uint8_t* e) {
    __try {
        uint8_t* n = e ? *(uint8_t**)(e + 0x110) : nullptr;
        return n ? (const char*)n + 0x14 : "?";
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
}

// Reads a u32 that may be unmapped; false if it faults. Diagnostics only.
static bool safe_u32(const void* p, uint32_t* out) {
    __try { *out = *(const uint32_t*)p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// g_server's entity with this id (table *(g_server+0x10), 16-byte entries, entity at +8, count u16 at +0x1A), or null
static uint8_t* entity_by_id(unsigned id) {
    uint8_t* server = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD850);  // shortcut: fixed RVA
    for (int i = 0; server && i < *(uint16_t*)(server + 0x1A); ++i) {
        uint8_t* x = *(uint8_t**)(*(uint8_t**)(server + 0x10) + i * 16 + 8);
        if (x && *(uint16_t*)(x + 0x108) == id) return x;
    }
    return nullptr;
}
static uint8_t* controlled() {
    uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);  // shortcut: fixed RVA (as @goto)
    return lvl ? *(uint8_t**)(lvl + 0x38) : nullptr;
}
static int local_slot() { return g_players ? *(int32_t*)((uint8_t*)g_players - 0x18 + 8) : -1; }

static void __fastcall slog_hook(const char* s) {
    if (s && strstr(s, "loading content\\maps\\")) ++g_level_loads;
    if (s && strstr(s, "loader_finish: mapstate_running")) ++g_level_starts, g_body_ids.reset();  // new level: old body gone
    if (s && g_save_since && strstr(s, "total : gamesave")) save_restore_flag("serialized");
    if (s && !g_wait_hit) {
        std::lock_guard<std::mutex> lock(g_wait_mutex);
        if (!g_wait_text.empty() && strstr(s, g_wait_text.c_str())) g_wait_hit = true;
    }
    if (g_engine_log && s) {
        std::lock_guard<std::mutex> lock(g_engine_log_mutex);
        // Collapse repeats: after @link_actor the weapon UI update (RVA 0x526AA0) logs "!No ammo item..." every frame
        // (~4000 lines/s), which made engine.log huge and slow.
        static std::string last;
        static unsigned repeats = 0;
        if (last == s) { ++repeats; g_slog_orig(s); return; }
        if (repeats) fprintf(g_engine_log, "    (previous line repeated %u more times)\n", repeats);
        last = s;
        repeats = 0;
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_engine_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        fputs(s, g_engine_log);
        fputc('\n', g_engine_log);
        fflush(g_engine_log);
    }
    g_slog_orig(s);
}

// First match of "48 8B ? ?" ('?' = wildcard byte) in the executable sections of metro.exe.
static std::optional<uintptr_t> find(const char* sig) {
    std::vector<int> pat;  // -1 = wildcard
    for (const char* p = sig; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { pat.push_back(-1); while (*p == '?') ++p; continue; }
        pat.push_back((int)strtoul(p, (char**)&p, 16));
    }
    auto* base = (uint8_t*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) || sec->Misc.VirtualSize < pat.size()) continue;
        uint8_t* p = base + sec->VirtualAddress;
        for (size_t i = 0, n = sec->Misc.VirtualSize - pat.size(); i <= n; ++i) {
            size_t j = 0;
            while (j < pat.size() && (pat[j] < 0 || p[i + j] == pat[j])) ++j;
            if (j == pat.size()) return (uintptr_t)(p + i);
        }
    }
    return std::nullopt;
}

bool engine_init(const std::string& engine_log_path) {
    auto slog = find("40 53 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 0F B6 01 45 33 C0");
    auto gc = find("48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 ? ? ? ? 48 89 5C 24 38 48 89 74 24 40 48 89 7C 24 20 "
                   "E8 ? ? ? ? 48 8B C8 48 8B F8 FF 80 00 01 00 00 FF 15 ? ? ? ? 48 8B 4F 38 BA 28 21 00 00");
    if (gc) g_get_console = (get_console_t)*gc;
    if (auto mp = find("48 85 ED 74 0A B0 01 88 05 ? ? ? ? EB 07 0F B6 05"))  // mov [rip+disp], al at +7
        g_mp_flag = (uint8_t*)(*mp + 13 + *(int32_t*)(*mp + 9));
    LOG_INFO(std::string("engine: mp flag ") + (g_mp_flag ? "found" : "NOT FOUND"));
    if (auto lf = find("85 C0 74 1E 48 8B 03 48 8B CB FF 50 18 85 C0 74 11 48 8D 0D ? ? ? ? 48 83 C4 20 5B E9")) {
        g_loader = (void*)(*lf + 24 + *(int32_t*)(*lf + 20));                       // lea rcx,[rip+disp]
        g_loader_finish = (loader_finish_t)(*lf + 34 + *(int32_t*)(*lf + 30));     // jmp rel32
    }
    LOG_INFO(std::string("engine: loader_finish ") + (g_loader_finish ? "found" : "NOT FOUND"));
    if (auto pp = find("48 89 5C 24 08 57 48 83 EC 20 48 8B 02 48 8B D9 33 C9 48 8B FA 48 85 C0 48 8D 50 14 48 0F 44 D1"))
        g_pause_pop = (pause_pop_t)*pp;
    if (auto sc = find("48 8B 0D ? ? ? ? 48 8D 15 ? ? ? ? 45 33 C9 48 89 7C 24 40 41 B8 07 00 00 00 E8")) {
        g_strc = (void**)(*sc + 7 + *(int32_t*)(*sc + 3));
        g_str_shared = (str_shared_t)(*sc + 33 + *(int32_t*)(*sc + 29));
    }
    if (auto ls = find("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 48 8B 35 ? ? ? ? "
                       "41 8B E9 4D 8B F0 48 8B FA FF 15"))
        g_level_signal = (level_signal_t)*ls;
    LOG_INFO(std::string("engine: level signal ") + (g_level_signal && g_str_shared ? "found" : "NOT FOUND"));
    if (auto ns = find("48 89 4C 24 08 55 48 81 EC A0 01 00 00 48 83 3D ? ? ? ? 00 48 8B EA"))
        g_net_start = (net_start_t)*ns;
    LOG_INFO(std::string("engine: slog ") + (slog ? "found" : "NOT FOUND") + ", GetConsole " + (gc ? "found" : "NOT FOUND"));
    if (!slog) return false;

    g_engine_log = _fsopen(engine_log_path.c_str(), "w", _SH_DENYNO);  // shared so tests can read it live
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook((LPVOID)*slog, (LPVOID)&slog_hook, (LPVOID*)&g_slog_orig) != MH_OK ||
        MH_EnableHook((LPVOID)*slog) != MH_OK) {
        LOG_ERROR("engine: slog hook failed");
        return false;
    }
    LOG_INFO("engine: engine log -> " + engine_log_path + (g_engine_log ? "" : " (OPEN FAILED)"));
    ((slog_t)*slog)("metro_coop: slog hook self-test");  // must show up as the first engine.log line

    install_player_slot_fix();
    install_netframe_counter();

    LPVOID peek = (LPVOID)GetProcAddress(GetModuleHandleA("user32.dll"), "PeekMessageA");
    if (MH_CreateHook(peek, (LPVOID)&peek_hook, (LPVOID*)&g_peek_orig) != MH_OK || MH_EnableHook(peek) != MH_OK) {
        LOG_ERROR("engine: PeekMessageA hook failed - console commands disabled");
        g_peek_orig = nullptr;
    }
    return gc.has_value();
}

// Console commands must run on the game's window/main thread (calling from our thread crashed inside "server 1").
// They are queued here and drained from the game's own PeekMessageA calls on the thread that owns the game window.
static std::mutex g_cmd_mutex;
static std::vector<std::string> g_cmd_queue;
static std::atomic<bool> g_cmd_pending{false};
static HWND g_game_hwnd = nullptr;
static DWORD game_window_thread() {
    struct Ctx { DWORD tid = 0; HWND h = nullptr; } ctx;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        DWORD pid; DWORD tid = GetWindowThreadProcessId(h, &pid);
        char cls[32];
        if (pid == GetCurrentProcessId() && GetClassNameA(h, cls, sizeof(cls)) && !strcmp(cls, "_uengine_")) {
            ((Ctx*)l)->tid = tid; ((Ctx*)l)->h = h; return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    g_game_hwnd = ctx.h;
    return ctx.tid;
}

// The game pauses ("GAME PAUSED", game clock RVA 0xFC1124 stops) when its window loses focus. In a network game that
// freezes the engine's send timer, so nothing is ever sent. With keep_active we swallow the deactivation messages.
static bool g_keep_active = false;
static WNDPROC g_wndproc_orig = nullptr;
static LRESULT CALLBACK wndproc_hook(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if ((msg == WM_ACTIVATEAPP && !w) || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || msg == WM_KILLFOCUS)
        return 0;
    return CallWindowProcA(g_wndproc_orig, h, msg, w, l);
}
// The pause comes from polling GetForegroundWindow (the only focus API metro.exe imports), so the message filter
// above isn't enough: report our game window as foreground too.
using gfw_t = HWND(WINAPI*)();
static gfw_t g_gfw_orig = nullptr;
// Only metro.exe's own focus check gets the fake answer: other modules in the process (DirectInput decides from the
// real foreground window whether it may hold the mouse) must see the truth, or after Alt-Tab the game kept the mouse
// (cursor hidden, clicks swallowed; owner report 2026-11-13).
static HWND WINAPI gfw_hook() {
    static HMODULE exe = GetModuleHandleA(nullptr);
    HMODULE from = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)_ReturnAddress(), &from);
    if (from == exe) return g_game_hwnd ? g_game_hwnd : g_gfw_orig();
    static HMODULE logged[8] = {};  // diagnostics: once per other module that asks
    int k = 0;
    while (k < 8 && logged[k] && logged[k] != from) ++k;
    if (from && k < 8 && !logged[k]) {
        logged[k] = from;
        char n[MAX_PATH] = "?";
        GetModuleFileNameA(from, n, MAX_PATH);
        LOG_INFO(std::string("engine: GetForegroundWindow called from ") + n + " (real answer)");
    }
    return g_gfw_orig();
}
HWND engine_real_foreground_window() { return g_gfw_orig ? g_gfw_orig() : GetForegroundWindow(); }

void engine_keep_active() {
    g_keep_active = true;
    LPVOID gfw = (LPVOID)GetProcAddress(GetModuleHandleA("user32.dll"), "GetForegroundWindow");
    if (MH_CreateHook(gfw, (LPVOID)&gfw_hook, (LPVOID*)&g_gfw_orig) != MH_OK || MH_EnableHook(gfw) != MH_OK)
        LOG_ERROR("engine: GetForegroundWindow hook failed");
}

// Client: ride along when the host's body rides something (l03_chase handcar). In single player the level script
// attaches 'player' to the handcar (parent handle +0x118, target = parent entity + 0xA0, so its +0x68 is the parent's
// id +0x108; RVA 0x6DD360). Both sides see the host's body with that parent. The joiner's body gets no parent, and
// attaching it did not move it (its controller keeps its own position), so we move it ourselves. Not while the
// client's own script has attached its body (after l03's vision it does, onto the host's spot): forcing both crashed
// the client (AV RVA 0x79CA22, 2026-11-13). shortcut: translation only (no turning with the parent, no walking on it);
// upgrade: find how the single-player player is carried (drezina code, "vs_start_drezina").
static uint8_t* parent_of(uint8_t* e) {
    void** ph = *(void***)(e + 0x118);
    return ph && *ph ? (uint8_t*)*ph - 0xA0 : nullptr;
}
static void ride_follow() {
    static uint8_t* ride = nullptr;  // parent we follow
    static float off[3] = {};
    static uint64_t next_check = 0;
    uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);  // shortcut: fixed RVA (as @goto)
    uint8_t* me = lvl ? *(uint8_t**)(lvl + 0x38) : nullptr;
    if (!me) { ride = nullptr; return; }
    if (GetTickCount64() >= next_check) {
        next_check = GetTickCount64() + 250;
        uint8_t* host = nullptr;
        if (g_players && g_players->size >= 2 && *(int32_t*)((uint8_t*)g_players - 0x18 + 8) >= 1)
            host = entity_by_id(*(uint16_t*)((uint8_t*)g_players->array + 0x64));
        uint8_t* hp = host && host != me ? parent_of(host) : nullptr;
        // The client's own script may attach our body to the same vehicle on the host's exact spot (l03 after the
        // vision). Two bodies on one spot show the host's gun inside our view ("two weapons"). Detach ours (attach
        // to no parent) and put it 0.9 m to the host's side; the follow below then carries it.
        uint8_t* exe = (uint8_t*)GetModuleHandleA(nullptr);
        if (hp && parent_of(me) == hp && exe[0x6DD360] == 0x40 && exe[0x6DD363] == 0x56) {
            float dx = *(float*)(me + 0xF0) - *(float*)(host + 0xF0), dz = *(float*)(me + 0xF8) - *(float*)(host + 0xF8);
            if (dx * dx + dz * dz < 0.36f) {
                using attach_t = void*(__fastcall*)(void*, void**, void**, uint8_t*);
                void *out = nullptr, *none = nullptr;
                uint8_t flags = 0;
                ((attach_t)(exe + 0x6DD360))(me, &out, &none, &flags);
                alignas(16) float m[16];
                memcpy(m, me + 0xC0, sizeof(m));
                m[12] = *(float*)(host + 0xF0) + *(float*)(host + 0xC0) * 0.9f;  // host's right row (+0xC0)
                m[13] = *(float*)(host + 0xF4);
                m[14] = *(float*)(host + 0xF8) + *(float*)(host + 0xC8) * 0.9f;
                using set_xform_t = void(__fastcall*)(void*, float*, int);
                (*(set_xform_t**)me)[0x190 / 8](me, m, 0);
                LOG_INFO("engine: ride: moved our body off the host's spot");
            }
        }
        if (!hp || parent_of(me)) hp = nullptr;
        // only near the host: a scripted teleport (l03's vision) puts our body far away, and following then drags it back
        if (hp) {
            float dx = *(float*)(me + 0xF0) - *(float*)(host + 0xF0), dz = *(float*)(me + 0xF8) - *(float*)(host + 0xF8);
            if (dx * dx + dz * dz > 25) hp = nullptr;
        }
        if (hp != ride) {
            ride = hp;
            if (ride) for (int i = 0; i < 3; ++i) off[i] = *(float*)(me + 0xF0 + 4 * i) - *(float*)(ride + 0xF0 + 4 * i);
            LOG_INFO(std::string("engine: ride ") + (ride ? entity_name(ride + 0xA0) : "off"));
        }
    }
    if (!ride) return;
    alignas(16) float m[16];
    memcpy(m, me + 0xC0, sizeof(m));
    for (int i = 0; i < 3; ++i) m[12 + i] = *(float*)(ride + 0xF0 + 4 * i) + off[i];
    // a jump of more than 5 m since our last set is a script teleport: stop following (re-checked every 250 ms)
    {
        float dx = *(float*)(me + 0xF0) - m[12], dz = *(float*)(me + 0xF8) - m[14];
        if (dx * dx + dz * dz > 25) { ride = nullptr; LOG_INFO("engine: ride off (teleported)"); return; }
    }
    using set_xform_t = void(__fastcall*)(void*, float*, int);
    (*(set_xform_t**)me)[0x190 / 8](me, m, 0);
}

static BOOL WINAPI peek_hook(LPMSG m, HWND h, UINT a, UINT b, UINT r) {
    static DWORD main_tid = 0;
    if (g_keep_active && !g_wndproc_orig) {
        if (!main_tid) main_tid = game_window_thread();
        if (g_game_hwnd && GetCurrentThreadId() == main_tid) {
            g_wndproc_orig = (WNDPROC)SetWindowLongPtrA(g_game_hwnd, GWLP_WNDPROC, (LONG_PTR)&wndproc_hook);
            // the window usually started unfocused, so the game already saw a deactivation: replay an activation
            CallWindowProcA(g_wndproc_orig, g_game_hwnd, WM_ACTIVATEAPP, TRUE, 0);
            CallWindowProcA(g_wndproc_orig, g_game_hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
            CallWindowProcA(g_wndproc_orig, g_game_hwnd, WM_SETFOCUS, 0, 0);
            LOG_INFO("engine: keep_active window hook installed");
        }
    }
    if (g_players && g_players->size >= 2) {
        if (!main_tid) main_tid = game_window_thread();
        if (GetCurrentThreadId() == main_tid) ride_follow();
    }
    if (g_cmd_pending) {
        if (!main_tid) main_tid = game_window_thread();
        if (GetCurrentThreadId() == main_tid) {
            std::vector<std::string> cmds;
            { std::lock_guard<std::mutex> lock(g_cmd_mutex); cmds.swap(g_cmd_queue); g_cmd_pending = false; }
            using exec_t = void(__fastcall*)(void*, const char*);
            for (auto& c : cmds) {
                if (c.rfind("@mp ", 0) == 0) {  // our pseudo-command: set the engine's MP flag (see g_mp_flag)
                    if (g_mp_flag) *g_mp_flag = (uint8_t)atoi(c.c_str() + 4);
                    LOG_INFO("engine: mp flag = " + std::to_string(g_mp_flag ? *g_mp_flag : -1));
                    continue;
                }
                if (c == "@watch_net_time") {  // diagnostics: who writes the net send clock?
                    engine_watch_net_time();
                    continue;
                }
                if (c.rfind("@watch ", 0) == 0) {  // diagnostics: "@watch <rva hex>" = hardware write-watch
                    engine_watch_write((uintptr_t)GetModuleHandleA(nullptr) + strtoull(c.c_str() + 7, nullptr, 16));
                    continue;
                }
                if (c.rfind("@signal ", 0) == 0) {  // raise a level signal (MetroDeveloper binds "mp_respawn" to key B)
                    std::string name = c.substr(8);
                    // signal() dereferences g_level (RVA 0xFBD820, read at +0x1A in it); no level -> crash
                    void* level = g_level_signal ? *(void**)((uint8_t*)g_level_signal + 0x21 + *(int32_t*)((uint8_t*)g_level_signal + 0x1D)) : nullptr;
                    if (!level) LOG_WARN("engine: signal '" + name + "' skipped (no level)");
                    else if (g_str_shared) {
                        void* h = g_str_shared(*g_strc, name.c_str(), (uint32_t)name.size(), 0);
                        LOG_INFO("engine: signal '" + name + "'");
                        g_level_signal(nullptr, &h, nullptr, 0);
                    }
                    continue;
                }
                if (c.rfind("@pad ", 0) == 0) { pad_command(c.substr(5)); continue; }
                if (c.rfind("@title ", 0) == 0) {  // rename this instance's window (runs on the window thread)
                    static std::string title;
                    title = c.substr(7);
                    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM) -> BOOL {
                        if (IsWindowVisible(w) && !GetParent(w)) SetWindowTextA(w, title.c_str());
                        return TRUE;
                    }, 0);
                    continue;
                }
                if (c.rfind("@place ", 0) == 0) {
                    // "@place left|right": windowed only. left = left edge of its monitor; right = just right of the
                    // "Metro Redux [host]" window when that runs on this PC (else the monitor's right edge). Next to
                    // the host instead of the monitor edge because the game is not DPI aware: with display scaling
                    // it sees a 2560 px monitor as 1920 px wide.
                    static bool right;
                    right = c.substr(7) == "right";
                    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM) -> BOOL {
                        if (!IsWindowVisible(w) || GetParent(w) || !(GetWindowLongA(w, GWL_STYLE) & WS_CAPTION)) return TRUE;
                        if (IsIconic(w)) ShowWindow(w, SW_SHOWNOACTIVATE);  // unfocused copies start minimized
                        MONITORINFO mi{sizeof(mi)};
                        GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
                        RECT r, h;
                        GetWindowRect(w, &r);
                        int x = mi.rcWork.left - 7;  // visible edge at the monitor edge (7 px invisible border)
                        HWND host = right ? FindWindowA(nullptr, "Metro Redux [host]") : nullptr;
                        if (host && host != w && GetWindowRect(host, &h)) x = h.right - 14;  // 2 x 7 px invisible border
                        else if (right) x = mi.rcWork.right - (r.right - r.left);
                        SetWindowPos(w, nullptr, x, mi.rcWork.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        return TRUE;
                    }, 0);
                    continue;
                }
                if (c.rfind("@aim ", 0) == 0) {
                    unsigned id = 0xFFFF, ms = 3000;  // "@aim <id hex|s<slot>> [ms]"
                    int slot = -1;
                    if (c[5] == 's') {
                        sscanf_s(c.c_str() + 6, "%d %u", &slot, &ms);
                        if (g_players && slot >= 0 && slot < g_players->size)
                            id = *(uint16_t*)((uint8_t*)g_players->array + slot * 0x70 + 0x64);
                    } else sscanf_s(c.c_str() + 5, "%x %u", &id, &ms);
                    aim_command(id, ms);
                    continue;
                }
                if (c.rfind("@watch_slot ", 0) == 0) {  // diagnostics: who assigns an actor id to net player <idx>?
                    int i = atoi(c.c_str() + 12);
                    if (g_players && i < g_players->size)
                        engine_watch_write((uintptr_t)g_players->array + i * 0x70 + 0x64);
                    continue;
                }
                if (c.rfind("@players ", 0) == 0) {  // experiment: resize net_core.players with the engine's resize
                    if (g_players && g_resize) {
                        LOG_INFO("engine: players " + std::to_string(g_players->size) + " -> " + c.substr(9));
                        g_resize(g_players, (uint32_t)atoi(c.c_str() + 9));
                    }
                    continue;
                }
                if (c == "@level_ready") {  // host, after re-hosting a new level: tell clients to rejoin now
                    engine_msg_send(4 /* MSG_LEVEL */, "", 0);
                    LOG_INFO("engine: level ready sent");
                    continue;
                }
                if (c == "@follow_done") {  // client, right after its "disconnect": engine traffic flows again
                    g_level_follow = false;
                    g_host_gone = false;  // our own disconnect may have looked like the host leaving
                    msg_forget_peers();
                    continue;
                }
                if (c == "@net_host") {
                    // Experiment: turn an already-loaded single-player level (which has the player actor; MP-mode loads
                    // don't spawn one) into a network server without unloading it. net start (RVA 0x6B7EA0) returns
                    // early while net_core.host is set (1 = local-only marker), so clear it, set the MP flag, call it
                    // with no address -> server, player slot 0.
                    if (g_net_start && g_players && g_mp_flag) {
                        void** host = (void**)((uint8_t*)g_players - 0x18);
                        LOG_INFO("engine: net_host (net_core.host was " + std::to_string((uintptr_t)*host) + ")");
                        if ((uintptr_t)*host == 1) *host = nullptr;
                        *g_mp_flag = 1;
                        uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);  // shortcut: fixed RVA
                        void* ctl_before = lvl ? *(void**)(lvl + 0x38) : nullptr;
                        g_net_start(nullptr, nullptr);
                        // Link slot 0 to the SP player in the same frame: with the MP flag set, the controlled entity
                        // *(g_level+0x38) goes null until slot 0 has an actor, and l19_defence crashed at RVA 0x361292
                        // (reads it every frame) before a separate "@link_actor" ran (verified 13:53).
                        if (ctl_before && g_link_actor && g_players->size > 0)
                            g_link_actor((uint8_t*)g_players->array, *(uint16_t*)((uint8_t*)ctl_before + 0xA0 + 0x68));
                        char b2[96];
                        snprintf(b2, sizeof(b2), "engine: net_host ctl %p -> %p", ctl_before, lvl ? *(void**)(lvl + 0x38) : nullptr);
                        LOG_INFO(b2);
                        // note: slot 0's actor link (+0x58 handle / +0x64 id) is empty (0xFFFF) both before and after,
                        // even in plain SP - the SP actor is not tied to a net_player (verified 05:05).
                    }
                    continue;
                }
                if (c.rfind("@link_actor", 0) == 0) {
                    // "@link_actor [slot] [id hex]": tie net player <slot> (default 0) to an actor. Without an id it
                    // uses the single-player actor *(g_level+0x38) (RVA 0xFBD820): handles point at entity+0xA0 and
                    // the entity id is a u16 at +0x68 from there (see player lookup RVA 0x6B78F0).
                    int slot = 0; unsigned id = 0xFFFF;
                    sscanf(c.c_str() + 11, "%d %x", &slot, &id);
                    uint8_t* level = g_level_signal ? *(uint8_t**)((uint8_t*)g_level_signal + 0x21 + *(int32_t*)((uint8_t*)g_level_signal + 0x1D)) : nullptr;
                    uint8_t* player = level ? *(uint8_t**)(level + 0x38) : nullptr;
                    if (!player) player = g_seen_actors[0];  // first actor seen by the aim hook
                    if (id == 0xFFFF && player) id = *(uint16_t*)(player + 0xA0 + 0x68);
                    char buf[200];
                    snprintf(buf, sizeof(buf), "engine: link_actor slot %d id 0x%X (level %p player %p)", slot, id, level, player);
                    LOG_INFO(buf);
                    if (g_link_actor && g_players && slot < g_players->size && id != 0xFFFF) {
                        uint8_t* s = (uint8_t*)g_players->array + slot * 0x70;
                        g_link_actor(s, (uint16_t)id);
                        // check: does the id resolve back to the same actor? (handles point at entity+0xA0)
                        void** h = g_actor_of ? *g_actor_of(s, 1) : nullptr;
                        // entity name: handle at +0x110, chars at +0x14 ("no empty slot for player [%s]" in RVA 0x6B04C0)
                        snprintf(buf, sizeof(buf), "engine: link_actor check: resolves to %p '%s'/'%s' (want %p '%s')",
                                 h && *h ? *h : nullptr, h && *h ? entity_name((uint8_t*)*h) : "",
                                 h && *h ? entity_name((uint8_t*)*h - 0xA0) : "", player, entity_name(player));
                        LOG_INFO(buf);
                    }
                    continue;
                }
                if (c.rfind("@spawn_player", 0) == 0 || c.rfind("@spawn ", 0) == 0) {
                    // "@spawn <id hex>" (server only): same path for any entity, with is_player=0 and slot -1 like
                    // the engine's "spawn N copies" (RVA 0x6951C0, call at RVA 0x69547D).
                    // "@spawn_player [slot]" (server only): spawn a copy of the SP player for net player <slot>
                    // (default 1) the way the engine's player spawner (RVA 0x6F6210) does it: build clone id triples
                    // {src id, new id, parent} with RVA 0x6A9A80, then queue a reliable spawn event with RVA 0x6BA940
                    // (is_player=1: links the slot to the new root id right away; the event is executed locally by
                    // RVA 0x6B2490 and replicated to clients).
                    // shortcut: fixed RVAs for one exe build (SHA-256 097df1a2...), checked by prologue bytes.
                    int slot = 1;
                    unsigned src_id = 0xFFFF;
                    bool is_player = c[6] == '_';
                    if (is_player) sscanf(c.c_str() + 13, "%d", &slot);
                    else { sscanf(c.c_str() + 7, "%x", &src_id); slot = -1; }
                    uint8_t* exe = (uint8_t*)GetModuleHandleA(nullptr);
                    uint8_t* player = is_player ? g_seen_actors[0] : entity_by_id(src_id);
                    void* server = *(void**)(exe + 0xFBD850);
                    bool ok = exe[0x6A9A80] == 0x66 && exe[0x6BA940] == 0x48 && exe[0x6BA941] == 0x8B;
                    char buf[200];
                    snprintf(buf, sizeof(buf), "engine: spawn_player slot %d from %p '%s' (server %p, players %d, ok %d)",
                             slot, player, entity_name(player), server, g_players ? g_players->size : -1, ok);
                    LOG_INFO(buf);
                    if (is_player && c.find(" force") == std::string::npos && g_players && slot < g_players->size &&
                        *(uint16_t*)((uint8_t*)g_players->array + slot * 0x70 + 0x64) != 0xFFFF) {
                        LOG_INFO("engine: spawn_player skipped, slot already has an actor");
                    } else if (ok && player && server && g_players && (!is_player || slot < g_players->size)) {
                        struct { void* data; uint16_t cap; uint16_t size; } list{};
                        void* src = *(void**)(player + 0xA0);  // entity's handle object
                        void* none = nullptr;
                        using triples_t = void(__fastcall*)(void*, void**, void*, uint16_t);
                        using spawn_ev_t = void(__fastcall*)(void*, uint8_t, void*, void**, void**, void**, int, int, int,
                                                             uint8_t, int, void*, void*);
                        ((triples_t)(exe + 0x6A9A80))(server, &src, &list, 0xFFFF);
                        uint16_t n = list.size;
                        uint16_t root = n ? ((uint16_t*)list.data)[(n - 1) * 3 + 1] : 0xFFFF;
                        // shortcut: one joiner; a new body replaces the old body's ids (a dead body left behind gets saved)
                        if (is_player) {
                            g_body_ids.reset();
                            for (uint16_t i = 0; i < n; ++i) g_body_ids.set(((uint16_t*)list.data)[i * 3 + 1]);
                        }
                        snprintf(buf, sizeof(buf), "engine: spawn_player %u entities, root new id 0x%X", n, root);
                        LOG_INFO(buf);
                        std::string names = "engine: spawn_player sources:";  // diagnostics (FINDINGS 5.11)
                        for (uint16_t i = 0; i < n; ++i) {
                            uint16_t sid = ((uint16_t*)list.data)[i * 3];
                            uint8_t* se = entity_by_id(sid);
                            snprintf(buf, sizeof(buf), " %X:%s", sid, se ? entity_name(se + 0xA0) : "-");
                            names += buf;
                        }
                        LOG_INFO(names);
                        // arg7 (event word5 bit 0): script spawners (e.g. RVA 0x4F7050) pass 1, the player spawn 0
                        if (n) ((spawn_ev_t)(exe + 0x6BA940))(nullptr, 0, &list, &src, &none, &none, !is_player, is_player, slot,
                                                             0, 0, nullptr, nullptr);
                        // shortcut: list.data leaks (a few bytes per spawn); the engine frees it with its own allocator.
                    }
                    continue;
                }
                if (c.rfind("@dump", 0) == 0) {
                    // "@dump [name filter]": log id, name, position of g_server's entities (table *(g_server+0x10),
                    // 16-byte entries with the entity at +8, count u16 at +0x1A; walked like the save job RVA 0x3CD210)
                    std::string filter = c.size() > 6 ? c.substr(6) : "";
                    uint8_t* server = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD850);  // shortcut: fixed RVA
                    int n = server ? *(uint16_t*)(server + 0x1A) : 0, shown = 0;
                    for (int i = 0; i < n && shown < 64; ++i) {
                        uint8_t* e = *(uint8_t**)(*(uint8_t**)(server + 0x10) + i * 16 + 8);
                        const char* name = e ? entity_name(e + 0xA0) : "?";
                        if (!e) continue;
                        if (filter == "net") {  // only the networked entities (net_core+0x168, see ctl_str)
                            uint8_t* nc = g_players ? (uint8_t*)g_players - 0x18 : nullptr;
                            bool in = false;
                            for (int k = 0; nc && k < *(uint16_t*)(nc + 0x172) && !in; ++k)
                                in = *(uint16_t*)(*(uint8_t**)(nc + 0x168) + k * 16) == *(uint16_t*)(e + 0x108);
                            if (!in) continue;
                        } else if (!filter.empty() && !strstr(name, filter.c_str())) continue;
                        char buf[200];
                        // f = flags +0x10C (8 = networked, 0x20/0x204 checked by make-relevant RVA 0x747F60);
                        // b6/b7 = bytes written by the activate/deactivate update RVA 0x6D6DA0
                        // h = health float at +0x308 (loader RVA 0x21C130 reads "health" there; meaningful for
                        // entities of that class only)
                        // fwd = x/z of the matrix forward row (+0xE0), shows a door's rotation
                        // p = parent id: handle at +0x118 (attach/detach RVA 0x6DD360), id u16 at handle target +0x68
                        void** ph = *(void***)(e + 0x118);
                        unsigned pid = ph && *ph ? *(uint16_t*)((uint8_t*)*ph + 0x68) : 0xFFFF;
                        snprintf(buf, sizeof(buf), "engine: dump %X '%s' (%.2f,%.2f,%.2f) fwd=(%.2f,%.2f) f=%X b6=%u b7=%u h=%.3f p=%X",
                                 *(uint16_t*)(e + 0x108), name, *(float*)(e + 0xF0), *(float*)(e + 0xF4),
                                 *(float*)(e + 0xF8), *(float*)(e + 0xE0), *(float*)(e + 0xE8), *(uint32_t*)(e + 0x10C),
                                 e[0xB6], e[0xB7], *(float*)(e + 0x308), pid);
                        LOG_INFO(buf);
                        ++shown;
                    }
                    LOG_INFO("engine: dump done, " + std::to_string(n) + " entities, " + std::to_string(shown) + " shown");
                    continue;
                }
                if (c.rfind("@links ", 0) == 0) {  // diagnostics: "@links <id hex>": entity ids behind the player's
                    // hands/knife/torch/heap/timer links (+0x1740.., loader RVA 0x1DB3D0), for base e and e+0xA0
                    unsigned id = 0;
                    sscanf(c.c_str() + 7, "%x", &id);
                    uint8_t* e = entity_by_id(id);
                    for (int base = 0; e && base <= 0xA0; base += 0xA0) {
                        std::string out = "engine: links " + std::to_string(id) + " base+" + std::to_string(base) + ":";
                        for (unsigned off = 0x1740; off <= 0x17A0; off += 0x10) {
                            uint32_t lo, hi, t_lo, t_hi, idw = 0;
                            char b[32] = " ?";
                            if (safe_u32(e + base + off, &lo) && safe_u32(e + base + off + 4, &hi)) {
                                uint8_t* ph = (uint8_t*)(((uint64_t)hi << 32) | lo);
                                if (!ph) snprintf(b, sizeof(b), " null");
                                else if (safe_u32(ph, &t_lo) && safe_u32(ph + 4, &t_hi)) {
                                    uint8_t* t = (uint8_t*)(((uint64_t)t_hi << 32) | t_lo);
                                    snprintf(b, sizeof(b), t && safe_u32(t + 0x68, &idw) ? " %X" : " t0", idw & 0xFFFF);
                                }
                            }
                            out += b;
                            if (safe_u32(e + base + off + 8, &idw)) {  // link: {handle*, u16 id, bool valid}
                                snprintf(b, sizeof(b), "[%X/%u]", idw & 0xFFFF, (idw >> 16) & 0xFF);
                                out += b;
                            }
                        }
                        LOG_INFO(out);
                    }
                    continue;
                }
                if (c.rfind("@inv ", 0) == 0) {
                    // "@inv <id hex>": the body's inventory (*(e+0xB70)): state byte at +8 (low 5 bits = active slot,
                    // 0x20 = gas mask; applied by RVA 0x25DB20). Items as the slot getter RVA 0x25C740 walks them:
                    // owner = *(*(inv+0x28)+8), handles at owner+0x150 (count u16 +0x15A) -> entity+0xA0; item
                    // interface = entity vtbl+0x6F0, slot byte at item+0xAA, item vtbl+0x48 = usable.
                    uint8_t* e = c.compare(5, 3, "ctl") == 0 ? controlled() : entity_by_id(strtoul(c.c_str() + 5, nullptr, 16));
                    uint8_t* inv = e ? *(uint8_t**)(e + 0xB70) : nullptr;
                    if (!inv) { LOG_WARN("engine: inv: no entity/inventory"); continue; }
                    uint8_t* owner = *(uint8_t**)(*(uint8_t**)(inv + 0x28) + 8);
                    std::string out = "engine: inv state=" + std::to_string(inv[8]) + ":";
                    for (int k = 0; owner && k < *(uint16_t*)(owner + 0x15A); ++k) {
                        uint8_t* t = **(uint8_t***)(*(uint8_t**)(owner + 0x150) + k * 8);
                        if (!t) continue;
                        uint8_t* it_e = t - 0xA0;
                        using vf_t = uint8_t*(__fastcall*)(void*);
                        uint8_t* item = ((vf_t)(*(void***)it_e)[0x6F0 / 8])(it_e);
                        char b[112];
                        snprintf(b, sizeof(b), " %X'%s'f%X", *(uint16_t*)(it_e + 0x108), entity_name(t), *(uint32_t*)(it_e + 0x10C));
                        out += b;
                        if (item) {
                            snprintf(b, sizeof(b), "=s%u", item[0xAA]);
                            out += b;
                        }
                    }
                    LOG_INFO(out);
                    continue;
                }
                if (c.rfind("@invst ", 0) == 0) {
                    // diagnostics: "@invst <id hex|ctl>": single-player inventory state stack (FINDINGS 5.7): up to 3
                    // states of 0x1E0 bytes at inv+0x100, count u16 at inv+0x6A0; state u16 +0 = weapon index (0..3 ->
                    // slot 1/2/16/6, else 22 = none, RVA 0x4EBE00); plus inv+0x8A8/0x8A9 and the vtbl+0x300 state byte
                    uint8_t* e = c.compare(7, 3, "ctl") == 0 ? controlled() : entity_by_id(strtoul(c.c_str() + 7, nullptr, 16));
                    uint8_t* inv = e ? *(uint8_t**)(e + 0xB70) : nullptr;
                    if (!inv) { LOG_WARN("engine: invst: no entity/inventory"); continue; }
                    uint8_t st = 0;
                    using get_t = void(__fastcall*)(void*, uint8_t*);
                    ((get_t)(*(void***)inv)[0x300 / 8])(inv, &st);
                    uintptr_t vt = (uintptr_t)*(void**)inv - (uintptr_t)GetModuleHandleA(nullptr);
                    char b[160];
                    snprintf(b, sizeof(b), "engine: invst vt=%llX st=%X 8A8=%X 8A9=%X 6B0=%X 6B2=%X n=%u:", (unsigned long long)vt, st,
                             inv[0x8A8], inv[0x8A9], *(uint16_t*)(inv + 0x6B0), inv[0x6B2], *(uint16_t*)(inv + 0x6A0));
                    std::string out = b;
                    for (int k = 0; vt == 0xB2D218 && k < 3; ++k) {
                        uint8_t* s = inv + 0x100 + k * 0x1E0;
                        snprintf(b, sizeof(b), " [w%X f%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X]", *(uint16_t*)s, s[0x18], s[0x19],
                                 s[0x1A], s[0x1B], s[0x1C], s[0x1D], s[0x1E], s[0x1F], s[0x20], s[0x21]);
                        out += b;
                    }
                    LOG_INFO(out);
                    continue;
                }
                if (c.rfind("@ids ", 0) == 0) {  // diagnostics: "@ids <from hex> <to hex>": existing entities in an id range
                    unsigned lo = 0, hi = 0;
                    sscanf(c.c_str() + 5, "%x %x", &lo, &hi);
                    std::string out = "engine: ids";
                    for (unsigned id = lo; id <= hi && id < 0xFFFF; ++id)
                        if (uint8_t* x = entity_by_id(id)) {
                            char b[96];
                            snprintf(b, sizeof(b), " %X'%s'f%X", id, entity_name(x + 0xA0), *(uint32_t*)(x + 0x10C));
                            out += b;
                        }
                    LOG_INFO(out);
                    continue;
                }
                if (c.rfind("@health ", 0) == 0) {  // tests: "@health <v>" sets our own body's health float (+0x308)
                    if (uint8_t* e = controlled()) *(float*)(e + 0x308) = (float)atof(c.c_str() + 8);
                    LOG_INFO("engine: " + c.substr(1));
                    continue;
                }
                if (c.rfind("@detach ", 0) == 0) {  // diagnostics: "@detach <id hex>" = attach RVA 0x6DD360 to a null parent
                    uint8_t* x = entity_by_id(strtoul(c.c_str() + 8, nullptr, 16));
                    uint8_t* exe = (uint8_t*)GetModuleHandleA(nullptr);
                    if (x && exe[0x6DD360] == 0x40 && exe[0x6DD363] == 0x56) {
                        using attach_t = void*(__fastcall*)(void*, void**, void**, uint8_t*);
                        void *out = nullptr, *parent = nullptr;
                        uint8_t flags = 0;
                        ((attach_t)(exe + 0x6DD360))(x, &out, &parent, &flags);
                        LOG_INFO(std::string("engine: detach ") + entity_name(x + 0xA0));
                    }
                    continue;
                }
                if (c.rfind("@select ", 0) == 0) {
                    // "@select <id hex> <state hex>": apply an inventory state byte to the body (inventory vtbl+0x2F8 =
                    // RVA 0x25DB20: low 5 bits = slot to draw, 0x20 = gas mask; holsters the old slot's item, draws
                    // the new one and notifies the weapon controller)
                    unsigned id = 0, st = 0;
                    sscanf(c.c_str() + 8, "%x %x", &id, &st);
                    uint8_t* x = entity_by_id(id);
                    uint8_t* inv = x ? *(uint8_t**)(x + 0xB70) : nullptr;
                    if (!inv) { LOG_WARN("engine: select: no inventory"); continue; }
                    uint8_t b8 = (uint8_t)st, was = inv[8];
                    using apply_t = void(__fastcall*)(void*, uint8_t*);
                    ((apply_t)(*(void***)inv)[0x2F8 / 8])(inv, &b8);
                    char b[96];
                    snprintf(b, sizeof(b), "engine: select %X state %X -> %X (apply %p)", id, was, inv[8], (*(void***)inv)[0x2F8 / 8]);
                    LOG_INFO(b);
                    continue;
                }
                if (c.rfind("@refs ", 0) == 0) {
                    // "@refs <id hex> [len hex]": diagnostics. Scan the entity's first len bytes for references to
                    // other entities: a pointer to e+0xA0 (uobject base) or to a handle block whose first qword is one.
                    // "[via hex]": scan the object at *(e+via) instead (inventory +0xB70, weapon controller +0x12A8)
                    unsigned id = 0, len = 0x2000, via = 0;
                    sscanf(c.c_str() + 6, "%x %x %x", &id, &len, &via);
                    uint8_t* self = entity_by_id(id);
                    uint8_t* e = self && via ? *(uint8_t**)(self + via) : self;
                    std::string out = "engine: refs " + std::to_string(id) + " via " + std::to_string(via) + ":";
                    auto as_entity = [](uint64_t p) -> uint8_t* {  // p == x+0xA0 of a real entity x?
                        uint32_t lo, hi, idw;
                        if (p < 0x10000 || p > 0x7FFFFFFFFFFF || (p & 7) || !safe_u32((void*)(p + 0x68), &idw)) return nullptr;
                        uint8_t* x = entity_by_id(idw & 0xFFFF);
                        (void)lo; (void)hi;
                        return x && x + 0xA0 == (uint8_t*)p ? x : nullptr;
                    };
                    for (unsigned o = 0; e && o + 8 <= len; o += 8) {
                        uint32_t lo, hi;
                        if (!safe_u32(e + o, &lo) || !safe_u32(e + o + 4, &hi)) break;
                        uint64_t p = ((uint64_t)hi << 32) | lo;
                        uint8_t* x = as_entity(p);
                        const char* how = "";
                        if (!x && p > 0x10000 && p < 0x7FFFFFFFFFFF && !(p & 7) && safe_u32((void*)p, &lo) && safe_u32((void*)(p + 4), &hi)) {
                            x = as_entity(((uint64_t)hi << 32) | lo);
                            how = "h";
                        }
                        if (!x || x == self) continue;
                        char b[80];
                        snprintf(b, sizeof(b), " +%X%s=%X'%s'", o, how, *(uint16_t*)(x + 0x108), entity_name(x + 0xA0));
                        out += b;
                    }
                    LOG_INFO(out);
                    continue;
                }
                if (c.rfind("@act ", 0) == 0) { act_run_local(c.substr(5)); continue; }  // host's script action, forwarded
                if (c.rfind("@poke ", 0) == 0) {
                    // diagnostics: "@poke <id hex|ctl> <offset hex> <size 0|1|2|4> <value hex>": write an entity field
                    // (size 0 = write nothing), then log the entity's position (+0xF0)
                    unsigned id = 0, off = 0, size = 0, val = 0;
                    uint8_t* e = nullptr;
                    if (c.compare(6, 3, "ctl") == 0) { sscanf(c.c_str() + 9, "%x %u %x", &off, &size, &val); e = controlled(); }
                    else { sscanf(c.c_str() + 6, "%x %x %u %x", &id, &off, &size, &val); e = entity_by_id(id); }
                    if (!e) { LOG_WARN("engine: poke: no entity"); continue; }
                    if (size == 1) e[off] = (uint8_t)val;
                    else if (size == 2) *(uint16_t*)(e + off) = (uint16_t)val;
                    else if (size == 4) *(uint32_t*)(e + off) = val;
                    char b[128];
                    snprintf(b, sizeof(b), "engine: poke +%X=%X (%u) pos (%.2f,%.2f,%.2f)", off, val, size,
                             *(float*)(e + 0xF0), *(float*)(e + 0xF4), *(float*)(e + 0xF8));
                    LOG_INFO(b);
                    continue;
                }
                if (c.rfind("@hexdump ", 0) == 0) {  // diagnostics: "@hexdump <id hex> [len hex]" raw entity bytes
                    unsigned id = 0, len = 0x600;
                    uint8_t* e = nullptr;
                    if (c.compare(9, 3, "ctl") == 0) { sscanf(c.c_str() + 12, "%x", &len); if ((e = controlled())) id = *(uint16_t*)(e + 0x108); }
                    else { sscanf(c.c_str() + 9, "%x %x", &id, &len); e = entity_by_id(id); }
                    for (unsigned o = 0; e && o < len; o += 32) {
                        char b[160];
                        int n = snprintf(b, sizeof(b), "engine: hex %X +%03X:", id, o);
                        uint32_t v;
                        bool ok = true;
                        for (unsigned k = 0; k < 32 && (ok = safe_u32(e + o + k, &v)); k += 4) n += snprintf(b + n, sizeof(b) - n, " %08X", v);
                        if (!ok) break;
                        LOG_INFO(b);
                    }
                    continue;
                }
                if (c.rfind("@setflag ", 0) == 0) {  // diagnostics: "@setflag <id hex> <clear hex> <set hex>" on +0x10C
                    unsigned id = 0, clr = 0, set = 0;
                    sscanf(c.c_str() + 9, "%x %x %x", &id, &clr, &set);
                    if (uint8_t* e = entity_by_id(id)) {
                        uint32_t& f = *(uint32_t*)(e + 0x10C);
                        uint32_t was = f;
                        f = (f & ~clr) | set;
                        char b[96];
                        snprintf(b, sizeof(b), "engine: setflag %X f %X -> %X", id, was, f);
                        LOG_INFO(b);
                    } else LOG_WARN("engine: setflag: no entity " + std::to_string(id));
                    continue;
                }
                if (c.rfind("@fix_ammo", 0) == 0) {
                    // "@fix_ammo <id hex|s<slot>>": the player clone copies the ammo items, but leaves them detached
                    // (parent FFFF, flag 0x2000) while the weapons get the new body as parent (dump 16:55), so its
                    // inventory finds no ammo ("No ammo item"). Attach detached cloned ammo_* items within 3 m to the
                    // body with the engine's attach (RVA 0x6DD360: entity, &out handle, &parent handle, &flags).
                    unsigned id = 0xFFFF;
                    if (c.size() > 10 && c[10] == 's') {
                        int slot = atoi(c.c_str() + 11);
                        if (g_players && slot >= 0 && slot < g_players->size)
                            id = *(uint16_t*)((uint8_t*)g_players->array + slot * 0x70 + 0x64);
                    } else sscanf_s(c.c_str() + 9, "%x", &id);
                    uint8_t* body = entity_by_id(id);
                    uint8_t* exe = (uint8_t*)GetModuleHandleA(nullptr);
                    uint8_t* server = *(uint8_t**)(exe + 0xFBD850);  // shortcut: fixed RVAs
                    int n = 0;
                    if (body && server && exe[0x6DD360] == 0x40 && exe[0x6DD363] == 0x56) {
                        using attach_t = void*(__fastcall*)(void*, void**, void**, uint8_t*);
                        void* parent = *(void**)(body + 0xA0);
                        for (int i = 0; i < *(uint16_t*)(server + 0x1A); ++i) {
                            uint8_t* e = *(uint8_t**)(*(uint8_t**)(server + 0x10) + i * 16 + 8);
                            if (!e || !(*(uint32_t*)(e + 0x10C) & 0x10000) || strncmp(entity_name(e + 0xA0), "ammo_", 5)) continue;
                            void** ph = *(void***)(e + 0x118);
                            if (ph && *ph) continue;  // already attached
                            float dx = *(float*)(e + 0xF0) - *(float*)(body + 0xF0), dz = *(float*)(e + 0xF8) - *(float*)(body + 0xF8);
                            if (dx * dx + dz * dz > 9) continue;
                            void* out = nullptr; uint8_t flags = 0;
                            ((attach_t)(exe + 0x6DD360))(e, &out, &parent, &flags);
                            ++n;
                        }
                    }
                    LOG_INFO("engine: fix_ammo body " + std::to_string(id) + ": attached " + std::to_string(n));
                    continue;
                }
                if (c.rfind("@slotdump", 0) == 0) {  // "@slotdump [slot]": hex of the 0x70-byte net_player
                    int slot = 1;
                    sscanf_s(c.c_str() + 9, "%d", &slot);
                    std::string h = "engine: slot " + std::to_string(slot) + ":";
                    if (g_players && slot >= 0 && slot < g_players->size) {
                        uint8_t* p = (uint8_t*)g_players->array + slot * 0x70;
                        char x[4];
                        for (int i = 0; i < 0x70; ++i) { snprintf(x, sizeof(x), "%s%02X", i % 8 ? "" : " ", p[i]); h += x; }
                    }
                    LOG_INFO(h);
                    continue;
                }
                if (c.rfind("@respawn", 0) == 0) {
                    // "@respawn [slot]" (host): spawn a new body onto the still-linked slot (link_actor in the spawn
                    // event re-links it). Not the engine's slot reset (RVA 0x6B8E30): that makes the client reload
                    // the level (change_map_cl) and the reload crashed in a heap free (FINDINGS 5.6).
                    int slot = 1;
                    sscanf_s(c.c_str() + 8, "%d", &slot);
                    g_spawn_force = true;  // engine_tick spawns (skipping the "slot has an actor" check) and fixes its ammo
                    g_spawn_at = GetTickCount64();
                    g_spawn_slot = slot;
                    continue;
                }
                if (c.rfind("@goto ", 0) == 0) {
                    // "@goto <id hex|s<slot>> <dist>": put the controlled entity <dist> m in front of entity <id>.
                    // Guess: matrix rows at +0xC0 right, +0xD0 up, +0xE0 forward, +0xF0 position.
                    // "s<slot>" instead of an id = that net slot's actor (+0x64)
                    unsigned id = 0xFFFF; float dist = 2; int slot = -1;
                    if (c[6] == 's') {
                        sscanf_s(c.c_str() + 7, "%d %f", &slot, &dist);
                        if (g_players && slot >= 0 && slot < g_players->size)
                            id = *(uint16_t*)((uint8_t*)g_players->array + slot * 0x70 + 0x64);
                    } else sscanf_s(c.c_str() + 6, "%x %f", &id, &dist);
                    uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);  // shortcut: fixed RVA
                    uint8_t* e = lvl ? *(uint8_t**)(lvl + 0x38) : nullptr;
                    uint8_t* t = entity_by_id(id);
                    if (e && t) {
                        alignas(16) float m[16], tm[16];
                        memcpy(tm, t + 0xC0, sizeof(tm));
                        // same facing as the target: facing it made "forward" run into the host's body (16:40)
                        for (int i = 0; i < 12; ++i) m[i] = (i % 4 == 3) ? 0 : tm[i];
                        // horizontal forward only, same height as the target (the forward row can tilt)
                        float fl = sqrtf(tm[8] * tm[8] + tm[10] * tm[10]);
                        if (fl < 1e-3f) fl = 1;
                        m[12] = tm[12] + tm[8] / fl * dist; m[13] = tm[13]; m[14] = tm[14] + tm[10] / fl * dist;
                        m[15] = 1;
                        using set_xform_t = void(__fastcall*)(void*, float*, int);
                        (*(set_xform_t**)e)[0x190 / 8](e, m, 0);
                    }
                    char buf[128];
                    snprintf(buf, sizeof(buf), "engine: goto %X dist %.1f: %p -> %p", id, dist, t, e);
                    LOG_INFO(buf);
                    continue;
                }
                if (c.rfind("@teleport", 0) == 0) {
                    // "@teleport <dx> <dy> <dz>": move the controlled entity (*(g_level+0x38)) by an offset. Its 4x4
                    // transform is at entity+0xC0 (translation row at +0xF0); set_transform = entity vtbl+0x190(&m, 0),
                    // as the clone placement (RVA 0x694DA0) uses it. Clients own their body's position, so run it there.
                    // optional 4th arg: entity id (hex) instead of the controlled entity
                    float d[3] = {};
                    unsigned id = 0xFFFF;
                    sscanf_s(c.c_str() + 9, "%f %f %f %x", &d[0], &d[1], &d[2], &id);
                    uint8_t* level = g_level_signal ? *(uint8_t**)((uint8_t*)g_level_signal + 0x21 + *(int32_t*)((uint8_t*)g_level_signal + 0x1D)) : nullptr;
                    uint8_t* e = level ? *(uint8_t**)(level + 0x38) : nullptr;
                    if (id != 0xFFFF) e = entity_by_id(id);
                    if (e) {
                        alignas(16) float m[16];
                        memcpy(m, e + 0xC0, sizeof(m));
                        m[12] += d[0]; m[13] += d[1]; m[14] += d[2];
                        using set_xform_t = void(__fastcall*)(void*, float*, int);
                        (*(set_xform_t**)e)[0x190 / 8](e, m, 0);
                    }
                    char buf[128];
                    snprintf(buf, sizeof(buf), "engine: teleport %p by (%.1f,%.1f,%.1f)", e, d[0], d[1], d[2]);
                    LOG_INFO(buf);
                    continue;
                }
                if (c == "@unpause_all") {
                    // Pause stack on the engine object (g_loader): +0x16FE0 { void** handles; u16 cap; u16 size; },
                    // handle -> string object, chars at +0x14. A pause pushed before we were injected is never popped
                    // and keeps the game clock frozen; pop everything with the engine's POP PAUSE (RVA 0x664380).
                    if (g_loader && g_pause_pop) {
                        auto* base = (uint8_t*)g_loader;
                        for (int guard = 0; *(uint16_t*)(base + 0x16FEA) && guard < 16; ++guard) {
                            void* h = (*(void***)(base + 0x16FE0))[*(uint16_t*)(base + 0x16FEA) - 1];
                            LOG_INFO(std::string("engine: unpause '") + (h ? (const char*)h + 0x14 : "?") + "'");
                            g_pause_pop(g_loader, &h);
                        }
                    }
                    continue;
                }
                if (c == "@finish_load") {
                    LOG_INFO("engine: finish_load");
                    memset(g_seen_actors, 0, sizeof(g_seen_actors));  // the menu level has its own actor
                    if (g_loader_finish) g_loader_finish(g_loader);
                    continue;
                }
                void* console = g_get_console();
                LOG_INFO("engine: exec '" + c + "' (main thread)");
                (*(exec_t**)console)[0x40 / 8](console, c.c_str());
            }
        }
    }
    return g_peek_orig(m, h, a, b, r);
}

// net start (RVA 0x6B7EA0) fills two ENetAddress {ip, port=1000}: [rsp+1C0] = local bind (passed to
// enet_host_create), [rsp+1B0] = remote for "connect <ip>". A client on the same PC as the server can't bind 1000
// too; enet_host_create returns NULL and the game crashes at RVA 0x6B8032. Patching the shared `mov ecx,1000` would
// also move the remote port, so we hook enet_host_create (RVA 0x93AB20) and change only the bind address.
static uint16_t g_bind_port = 0;
using host_create_t = void*(__fastcall*)(uint32_t*, size_t, size_t, uint32_t, uint32_t);
static host_create_t g_host_create_orig = nullptr;

static void* __fastcall host_create_hook(uint32_t* addr, size_t peers, size_t channels, uint32_t in_bw, uint32_t out_bw) {
    if (addr && g_bind_port) ((uint16_t*)addr)[2] = g_bind_port;  // ENetAddress { u32 host; u16 port; }
    return g_host_create_orig(addr, peers, channels, in_bw, out_bw);
}

// Port of MetroDeveloper's multiplayer_fix to the Next-Gen exe. net_core is a global (RVA 0xFD5020); its players
// vector is at +0x18: { net_player* array; u16 capacity; u16 size; }, net_player = 0x70 bytes. When a peer connects,
// the server (net update RVA 0x6B32F0, "[net] incoming connection") looks for a free slot and, if there is none,
// uses index == size without growing the vector, then send_props_pack(pid) (RVA 0x4CA00) and touches the
// unconstructed entry -> crash at RVA 0x6B8E7C. We grow the vector with the engine's own resize (RVA 0x4E8B0,
// constructs entries) right before that call.
using spp_t = uint64_t(__fastcall*)(uint32_t, void*);
static spp_t g_spp_orig = nullptr;

static std::atomic<uint32_t> g_auto_spawn_s{0};
void engine_set_auto_spawn(uint32_t delay_s) { g_auto_spawn_s = delay_s; }
void engine_tick() {
    if (engine_client_in_session()) act_pending_run();
    install_save_hook();  // once the save manager exists (both host and client write checkpoints)
    if (g_save_since && GetTickCount64() - g_save_since > 15000) save_restore_flag("timeout");
    // shortcut: one pending spawn at a time; a second peer joining within the delay replaces the first one's
    int slot = g_spawn_slot;
    static int fix_slot = -1;
    static uint64_t fix_at = 0;
    if (slot >= 0 && GetTickCount64() >= g_spawn_at && g_spawn_slot.compare_exchange_strong(slot, -1)) {
        engine_exec("@spawn_player " + std::to_string(slot) + (g_spawn_force.exchange(false) ? " force" : ""));
        fix_slot = slot; fix_at = GetTickCount64() + 3000;  // the body exists once the host ran its own spawn event
    }
    if (fix_slot >= 0 && GetTickCount64() >= fix_at) { engine_exec("@fix_ammo s" + std::to_string(fix_slot)); fix_slot = -1; }
    // Client: a respawned body appears on the host's spot; step away like the @join macro does for the first body.
    // shortcut: fixed 1.2 m in front of the host; level geometry can block it (in l19 a 2.5 m spot was inside an
    // obstacle at x ~223 and the body could turn but not walk). Upgrade: pick a free spot (nav mesh / spawn points).
    static uint8_t* last_ctl = nullptr;
    uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);  // shortcut: fixed RVA
    uint8_t* ctl = lvl ? *(uint8_t**)(lvl + 0x38) : nullptr;
    int local = *(int32_t*)((uint8_t*)GetModuleHandleA(nullptr) + 0xFD5028);  // net_core local slot index
    if (ctl && last_ctl && ctl != last_ctl && local > 0) {
        engine_exec("@goto s0 1.2");
        engine_exec("@fix_ammo s" + std::to_string(local));
    }
    if (ctl) last_ctl = ctl;
    // Host: respawn a joiner whose body died (health float entity+0x308 <= 0), 5 s after death.
    // shortcut: fixed 5 s, respawns on the host's spot; upgrade with a respawn point / delay setting.
    static uint64_t dead_since[8] = {};
    uint8_t* exe = (uint8_t*)GetModuleHandleA(nullptr);
    bool server = g_players && g_mp_flag && *g_mp_flag && *(int32_t*)(exe + 0xFD5028) == 0;  // net_core local idx 0
    for (int s = 1; server && s < g_players->size && s < 8; ++s) {
        uint16_t id = *(uint16_t*)((uint8_t*)g_players->array + s * 0x70 + 0x64);
        uint8_t* e = id != 0xFFFF ? entity_by_id(id) : nullptr;
        if (!e || *(float*)(e + 0x308) > 0) { dead_since[s] = 0; continue; }
        if (!dead_since[s]) dead_since[s] = GetTickCount64();
        else if (GetTickCount64() - dead_since[s] > 5000) {
            dead_since[s] = 0;
            LOG_INFO("engine: slot " + std::to_string(s) + " body " + std::to_string(id) + " is dead, respawning");
            engine_exec("@respawn " + std::to_string(s));
        }
    }
}

static uint64_t __fastcall spp_hook(uint32_t pid, void* props) {
    // only the incoming-connection call site: return address bytes = imul r12d, r15d, 70h
    if (*(uint32_t*)_ReturnAddress() == 0x70E76B45) {
        if (pid >= g_players->size) {
            LOG_INFO("engine: new player slot " + std::to_string(pid) + " (players " + std::to_string(g_players->size) + ")");
            g_resize(g_players, pid + 1);
        }
        // the joiner still has to load the host's quick save; spawning ~15 s later worked in every test run
        if (g_auto_spawn_s) { g_spawn_at = GetTickCount64() + g_auto_spawn_s * 1000ull; g_spawn_slot = (int)pid; }
    }
    return g_spp_orig(pid, props);
}

// RVA 0x20D3A0 (int f(obj, id, _, use_player, flag)): with use_player != 0 it scales a value by a property of the
// controlled entity *(g_level+0x38). It tests that entity for null but then dereferences it anyway, so it crashes at
// RVA 0x20D44C while there is no controlled entity: the joining client during the qsave load of l19_defence
// (verified 13:59), and MP-mode loads of l02 in session 3. Without an entity, call it with use_player = 0.
using noplayer_t = int(__fastcall*)(void*, int, void*, int, char);
static noplayer_t g_noplayer_orig = nullptr;
static int __fastcall noplayer_hook(void* obj, int id, void* a3, int use_player, char flag) {
    uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);  // shortcut: fixed RVA
    if (use_player && (!lvl || !*(void**)(lvl + 0x38))) use_player = 0;
    return g_noplayer_orig(obj, id, a3, use_player, flag);
}

// RVA 0x360FD0: per-frame update of an entity that tracks the player (in l19_defence) reads *(g_level+0x38)+0xF0
// without a null check (crash RVA 0x361292): on the host between net start and the slot-0 link, on the client until
// its body spawns (verified 13:53 and 14:03). Skip the update while there is no controlled entity.
using track_t = void(__fastcall*)(void*);
static track_t g_track_orig = nullptr;
static void __fastcall track_hook(void* e) {
    uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);  // shortcut: fixed RVA
    if (lvl && *(void**)(lvl + 0x38)) g_track_orig(e);
}

// RVA 0x526AA0 binds the weapon HUD's slots (array at hud+0x108, 0xB8 bytes each, count u16 at +0x112) to ammo items.
// When the inventory has no ammo item it logs "!No ammo item. Save level, bleat !!!" and leaves the slot's ammo-type
// index (+0xA8) unset. The HUD draw (RVA 0x525F60) then indexes the weapon-type record's ammo table (record =
// *(g_level+0x2A500) + type * 0x178, table at +0x98; type 23 of 26 seen) with it and crashed at RVA 0x5260D5 when the
// client's cloned body fired in l19_defence (verified 14:05). Reset the index to 0 first so a failed bind stays safe.
using ammo_bind_t = void(__fastcall*)(uint8_t*, void*, void*);
static ammo_bind_t g_ammo_bind_orig = nullptr;
static void __fastcall ammo_bind_hook(uint8_t* hud, void* a2, void* a3) {
    for (unsigned i = 0; i < *(uint16_t*)(hud + 0x112); ++i)
        *(int32_t*)(*(uint8_t**)(hud + 0x108) + i * 0xB8 + 0xA8) = 0;
    g_ammo_bind_orig(hud, a2, a3);
}

// RVA 0x2AEAF0 (weapon ammo count, -1 = no weapon) looks the ammo item up with inventory vtbl+0xD0 and dereferences
// it without a check. A cloned body has no ammo items (same root cause as the HUD guard above), and the client crashed
// there at RVA 0x2AEBF7 right after a respawned body arrived (verified 15:53).
// shortcut: SEH around the whole call returns -1 on any fault inside; upgrade by giving cloned bodies ammo items.
using ammo_count_t = int(__fastcall*)(void*);
static ammo_count_t g_ammo_count_orig = nullptr;
static int __fastcall ammo_count_hook(void* w) {
    __try { return g_ammo_count_orig(w); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// RVA 0x74C570: timed "remove this entity" callback (runs again later while game time < entity+0x204, then destroys
// it with RVA 0x6A9030 or the net destroy RVA 0x6BA440). After a respawn the client crashed in it at RVA 0x74C6CB on a
// freed corpse (verified 16:03): the corpse was already gone while its timer was still queued. Run it only for
// entities still in g_server's table (pointer compare only, so a freed one is never read).
using remove_cb_t = void(__fastcall*)(uint8_t*);
static remove_cb_t g_remove_cb_orig = nullptr;
static void __fastcall remove_cb_hook(uint8_t* e) {
    uint8_t* server = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD850);  // shortcut: fixed RVA
    for (int i = 0; server && i < *(uint16_t*)(server + 0x1A); ++i)
        if (*(uint8_t**)(*(uint8_t**)(server + 0x10) + i * 16 + 8) == e) { g_remove_cb_orig(e); return; }
    LOG_INFO("engine: skipped removal timer of a freed entity");
}

using lookup_t = void*(__fastcall*)(void*, void***);
static lookup_t g_lookup_orig = nullptr;
static void* __fastcall lookup_hook(void* net_core, void*** handle) {
    uint8_t* slot = (uint8_t*)g_lookup_orig(net_core, handle);
    // Only while networked (net_core.host is a real ENet host, not null / 1 = local-only marker): plain SP code relies
    // on slot 0 coming back for the player (guarding it crashed SP save loads at RVA 0x3175A). Not keyed on the MP
    // flag: the joining client crashed while its flag was 0 (SP branch returns slot 0, which has no actor).
    uintptr_t host = *(uintptr_t*)net_core;
    if (!slot || !g_actor_of || host <= 1) return slot;
    void** h = *g_actor_of(slot, 1);
    if (h && *h) return slot;
    static int logged = 0;
    if (logged++ < 5) {
        char b[160];
        snprintf(b, sizeof(b), "engine: lookup guard: slot %d id 0x%X has no actor (entity id 0x%X)",
                 (int)((slot - (uint8_t*)g_players->array) / 0x70), *(uint16_t*)(slot + 0x64),
                 handle && *handle && **handle ? *(uint16_t*)((uint8_t*)**handle + 0x68) : 0xFFFFu);
        LOG_WARN(b);
    }
    return nullptr;
}

// Actor aim update (RVA 0x399DD0, only caller RVA 0x399A30): runs per frame on an actor that has a weapon (+0xB70).
// Used to find the single-player actor, since *(g_level+0x38) is null in SP gameplay (verified 08:40).
using aim_t = uint64_t(__fastcall*)(uint8_t*);
static aim_t g_aim_orig = nullptr;
static uint64_t __fastcall aim_hook(uint8_t* actor) {
    for (auto& a : g_seen_actors) {
        if (a == actor) break;
        if (!a) {
            a = actor;
            char b[96];
            snprintf(b, sizeof(b), "engine: aim actor %p id 0x%X '%s'", actor, *(uint16_t*)(actor + 0xA0 + 0x68), entity_name(actor));
            LOG_INFO(b);
            break;
        }
    }
    return g_aim_orig(actor);
}

// Diagnostics: who creates entities at runtime? g_server create-from-records (RVA 0x6A62A0) and create-from-stream
// (RVA 0x6A7A00). Hooked by RVA (exe SHA-256 097df1a2...), all integer/pointer args forwarded (<= 8).
using any8_t = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static any8_t g_create_rec_orig = nullptr, g_create_stream_orig = nullptr;
static void log_create(const char* what, void* ret) {
    static std::atomic<int> n{0};
    if (n++ >= 40) return;
    char b[128];
    snprintf(b, sizeof(b), "engine: %s from RVA 0x%llX", what,
             (unsigned long long)((uintptr_t)ret - (uintptr_t)GetModuleHandleA(nullptr)));
    LOG_INFO(b);
}
static uintptr_t __fastcall create_rec_hook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) {
    char w[64]; snprintf(w, sizeof(w), "create_from_records id 0x%X", (unsigned)(b & 0xFFFF));
    log_create(w, _ReturnAddress());
    return g_create_rec_orig(a, b, c, d, e, f, g, h);
}
static uintptr_t __fastcall create_stream_hook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) {
    char w[64]; snprintf(w, sizeof(w), "create_from_stream kind 0x%X", (unsigned)(c & 0xFFFF));
    log_create(w, _ReturnAddress());
    return g_create_stream_orig(a, b, c, d, e, f, g, h);
}

static bool install_player_slot_fix() {
    auto spp = find("48 89 5C 24 18 48 89 6C 24 20 56 57 41 55 41 56 41 57 48 83 EC 60 48 8B F2 44 8B F1 E8");
    auto rs = find("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 0F B7 41 08 8B F2");
    auto nc = find("80 3D ? ? ? ? 00 74 07 C6 05 ? ? ? ? 00 48 8D 0D ? ? ? ? E8");  // lea rcx,[net_core] at +16
    if (!spp || !rs || !nc) { LOG_ERROR("engine: player-slot fix patterns not found"); return false; }
    g_players = (PlayersVec*)(*nc + 23 + *(int32_t*)(*nc + 19) + 0x18);
    g_resize = (resize_t)*rs;
    if (MH_CreateHook((LPVOID)*spp, (LPVOID)&spp_hook, (LPVOID*)&g_spp_orig) != MH_OK || MH_EnableHook((LPVOID)*spp) != MH_OK) {
        LOG_ERROR("engine: send_props_pack hook failed");
        return false;
    }
    LOG_INFO("engine: player-slot fix installed");

    // net_core player lookup by entity (RVA 0x6B78F0): in MP mode it returns the slot whose actor id (+0x64) equals
    // the entity's id (+0x68). Slots without an actor hold 0xFFFF, so any entity without an id (0xFFFF) matched them
    // and callers dereferenced a null actor (client crash at RVA 0x39A2FD, aim-target code). An id-0xFFFF guard alone
    // didn't stop it, so: return "no player" whenever the found slot's actor doesn't resolve (callers handle null).
    if (auto ao = find("40 53 48 83 EC 20 48 8B D9 85 D2 0F 84 ? ? ? ? 44 0F B7 41 64 B8 FF FF 00 00 66 41 3B C0"))
        g_actor_of = (actor_of_t)*ao;
    if (auto lk = find("80 3D ? ? ? ? 00 4C 8B D2 4C 8B C9 74 4D 48 8B 02 48 85 C0 0F 84"); lk && g_actor_of) {
        if (MH_CreateHook((LPVOID)*lk, (LPVOID)&lookup_hook, (LPVOID*)&g_lookup_orig) == MH_OK && MH_EnableHook((LPVOID)*lk) == MH_OK)
            LOG_INFO("engine: player lookup guard installed");
    } else LOG_ERROR("engine: player lookup pattern not found");
    auto np = find("40 55 41 56 41 57 48 83 EC 30 45 33 F6 0F 29 74 24 20 F3 0F 10 35 ? ? ? ? 8B EA 4C 8B F9 45 85 C9");
    if (np && MH_CreateHook((LPVOID)*np, (LPVOID)&noplayer_hook, (LPVOID*)&g_noplayer_orig) == MH_OK &&
        MH_EnableHook((LPVOID)*np) == MH_OK)
        LOG_INFO("engine: no-player guard installed");
    else LOG_ERROR("engine: no-player guard pattern not found");
    AddVectoredExceptionHandler(0, av_veh);  // diagnostics only, see av_veh
    auto ac = find("40 53 56 57 48 83 EC 20 48 8B B9 88 00 00 00 48 8B F1 48 85 FF 0F 84 ? ? ? ? 48 81 C1 80 01");
    if (ac && MH_CreateHook((LPVOID)*ac, (LPVOID)&ammo_count_hook, (LPVOID*)&g_ammo_count_orig) == MH_OK &&
        MH_EnableHook((LPVOID)*ac) == MH_OK)
        LOG_INFO("engine: ammo count guard installed");
    else LOG_ERROR("engine: ammo count guard pattern not found");
    auto rc = find("40 53 48 83 EC 30 8B 05 ? ? ? ? 48 8B D9 39 81 04 02 00 00 76 48 E8");
    if (rc && MH_CreateHook((LPVOID)*rc, (LPVOID)&remove_cb_hook, (LPVOID*)&g_remove_cb_orig) == MH_OK &&
        MH_EnableHook((LPVOID)*rc) == MH_OK)
        LOG_INFO("engine: removal timer guard installed");
    else LOG_ERROR("engine: removal timer guard pattern not found");
    auto tr = find("40 57 48 81 EC A0 00 00 00 83 B9 44 08 00 00 00 48 8B F9 0F 84 ? ? ? ? 48 8B 81 D0 04 00 00");
    if (tr && MH_CreateHook((LPVOID)*tr, (LPVOID)&track_hook, (LPVOID*)&g_track_orig) == MH_OK &&
        MH_EnableHook((LPVOID)*tr) == MH_OK)
        LOG_INFO("engine: player-tracker guard installed");
    else LOG_ERROR("engine: player-tracker guard pattern not found");
    auto ab = find("40 55 41 54 41 55 48 83 EC 40 0F B7 81 12 01 00 00 33 ED 48 89 51 10 4C 8B E9 4D 8B A0 70 0B 00 00");
    if (ab && MH_CreateHook((LPVOID)*ab, (LPVOID)&ammo_bind_hook, (LPVOID*)&g_ammo_bind_orig) == MH_OK &&
        MH_EnableHook((LPVOID)*ab) == MH_OK)
        LOG_INFO("engine: ammo HUD guard installed");
    else LOG_ERROR("engine: ammo HUD guard pattern not found");
    if (auto la = find("40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 81 63 68 FF FF FD FF 81 4B 68 00 00 05 00"))
        g_link_actor = (link_actor_t)*la;
    LOG_INFO(std::string("engine: link_actor ") + (g_link_actor ? "found" : "NOT FOUND"));
    auto aim = find("40 55 57 48 8D AC 24 A8 FC FF FF 48 81 EC 58 04 00 00 48 8B F9 48 8B 0D ? ? ? ? 48 8B 01 FF 90 D8 01 00 00");
    if (!aim || MH_CreateHook((LPVOID)*aim, (LPVOID)&aim_hook, (LPVOID*)&g_aim_orig) != MH_OK || MH_EnableHook((LPVOID)*aim) != MH_OK)
        LOG_ERROR("engine: aim hook failed");
    uint8_t* exe = (uint8_t*)GetModuleHandleA(nullptr);
    if (exe[0x6A62A0] == 0x4C && exe[0x6A7A00] == 0x4C) {  // shortcut: fixed RVAs for one exe build, diagnostics only
        if (MH_CreateHook(exe + 0x6A62A0, (LPVOID)&create_rec_hook, (LPVOID*)&g_create_rec_orig) == MH_OK) MH_EnableHook(exe + 0x6A62A0);
        if (MH_CreateHook(exe + 0x6A7A00, (LPVOID)&create_stream_hook, (LPVOID*)&g_create_stream_orig) == MH_OK) MH_EnableHook(exe + 0x6A7A00);
    }
    return true;
}

bool engine_set_bind_port(uint16_t port) {
    auto at = find("44 89 4C 24 20 53 57 48 83 EC 58 48 8B F9 B9 E0 2A 00 00 FF 15");
    if (!at || MH_CreateHook((LPVOID)*at, (LPVOID)&host_create_hook, (LPVOID*)&g_host_create_orig) != MH_OK ||
        MH_EnableHook((LPVOID)*at) != MH_OK) {
        LOG_ERROR("engine: enet_host_create hook failed");
        return false;
    }
    g_bind_port = port;
    LOG_INFO("engine: ENet bind port -> " + std::to_string(port));
    return true;
}

// Diagnostics: count net frames (RVA 0x6BB150, called by the main loop with &net_core).
static std::atomic<uint64_t> g_netframes{0};
using netframe_t = void(__fastcall*)(void*);
static netframe_t g_netframe_orig = nullptr;
static void __fastcall netframe_hook(void* nc) { ++g_netframes; g_netframe_orig(nc); }
// Diagnostics: log ENet events from enet_host_service (RVA 0x9410E0, called by RVA 0x6B5D00).
// ENetEvent { u32 type; ENetPeer* peer; u8 channel; u32 data; ENetPacket* packet; }
// ENetPacket { size_t refCount; u32 flags; u8* data; size_t dataLength; ... }
using service_t = int(__fastcall*)(void*, uint8_t*, uint32_t);
static service_t g_service_orig = nullptr;
static std::atomic<int> g_events_logged{0};
static std::atomic<uint64_t> g_service_calls{0};
// Client: the host's ENet disconnect (event type 2). The client keeps players=2 and its body after that (its level
// runs on alone), so this is the only sign that the host left. The process restarts afterwards, so never reset.
// Diagnostics: enet_peer_send(peer, channel, packet) (RVA 0x93DB60, resolved from its call in RVA 0x6B1D60).
using peer_send_t = int(__fastcall*)(void*, uint8_t, uint8_t*);
static peer_send_t g_peer_send_orig = nullptr;
static std::atomic<int> g_sends_logged{0};
static std::atomic<uint64_t> g_sends{0};

// Our own message channel on the engine's ENet connection (hybrid netcode, FINDINGS 5.12). The engine only uses
// channel 0 and queues every received packet without looking at the channel (RVA 0x6B5D00), so our packets go on
// channel 0 with a magic prefix and service_hook removes them before the engine sees them.
// Packet = kMsgMagic (4 bytes) + u8 type + payload. Reliable. Peers = the ones the engine itself sends to.
// shortcut: no message before the engine's first send to a peer; fine, the engine sends every net frame.
static const uint8_t kMsgMagic[4] = {0xFE, 'M', 'C', '1'};
enum : uint8_t { MSG_PING = 1, MSG_PONG = 2, MSG_ACT = 3, MSG_LEVEL = 4 };
static std::mutex g_msg_mtx;
static std::vector<std::vector<uint8_t>> g_msg_out;  // queued by engine_msg_send, sent from peer_send_hook
static void* g_msg_peers[4] = {};                    // only touched on the net thread (send/service hooks)
static std::atomic<uint64_t> g_msg_tx{0}, g_msg_rx{0};
static std::atomic<uint64_t> g_engine_send_ms{0};  // last engine enet_peer_send (GetTickCount64)
static std::vector<std::vector<uint8_t>> g_act_log;  // host: forwarded script actions since the level load (g_msg_mtx)
static void* g_replay_peer = nullptr;                // net thread: new peer that still needs g_act_log
static std::vector<std::string> g_act_pending;       // client: received "@act ..." commands not run yet (g_msg_mtx)
// ENet allocator callbacks the engine's ENet uses (enet_malloc/enet_free, set at RVA 0xFE6D20/0xFE6D28).
// shortcut: fixed RVAs of this exe build, like g_level; upgrade to a pattern if another build must work.
using enet_malloc_t = void*(__cdecl*)(size_t);
using enet_free_t = void(__cdecl*)(void*);
static enet_malloc_t enet_malloc_fn() { return *(enet_malloc_t*)((uint8_t*)GetModuleHandleA(nullptr) + 0xFE6D20); }
static enet_free_t enet_free_fn() { return *(enet_free_t*)((uint8_t*)GetModuleHandleA(nullptr) + 0xFE6D28); }

// enet_packet_destroy, as the engine does it after copying a packet (RVA 0x6B5D00):
// ENetPacket { size_t refCount; u32 flags; u8* data; size_t dataLength; void(*freeCallback)(ENetPacket*); void* userData; }
static void msg_packet_destroy(uint8_t* pkt) {
    if (auto cb = *(void(__cdecl**)(uint8_t*))(pkt + 32)) cb(pkt);
    if (!(*(uint32_t*)(pkt + 8) & 4)) enet_free_fn()(*(void**)(pkt + 16));  // 4 = ENET_PACKET_FLAG_NO_ALLOCATE
    enet_free_fn()(pkt);
}

// enet_packet_create(data, len, ENET_PACKET_FLAG_RELIABLE) + enet_peer_send on channel 0.
static bool msg_send_raw(void* peer, const std::vector<uint8_t>& m) {
    auto* pkt = (uint8_t*)enet_malloc_fn()(48);
    if (!pkt) return false;
    memset(pkt, 0, 48);
    auto* data = (uint8_t*)enet_malloc_fn()(m.size());
    if (!data) { enet_free_fn()(pkt); return false; }
    memcpy(data, m.data(), m.size());
    *(uint32_t*)(pkt + 8) = 1;  // ENET_PACKET_FLAG_RELIABLE
    *(uint8_t**)(pkt + 16) = data;
    *(size_t*)(pkt + 24) = m.size();
    if (g_peer_send_orig(peer, 0, pkt) < 0) { msg_packet_destroy(pkt); return false; }  // refCount 0: still ours
    ++g_msg_tx;
    return true;
}

// ENetPeer { ENetListNode dispatchList; ENetHost* host; ... }: a peer of an ENet host that was destroyed (local
// "disconnect") must not be used. net_core+0 = the current ENetHost*.
static bool msg_peer_live(void* p) {
    void* host = g_players ? *(void**)((uint8_t*)g_players - 0x18) : nullptr;
    return p && host && (uintptr_t)host != 1 && *(void**)((uint8_t*)p + 16) == host;
}
static void msg_send_all(const std::vector<uint8_t>& m) {
    for (void*& p : g_msg_peers) {
        if (p && !msg_peer_live(p)) { LOG_INFO("coopmsg: dropping a peer of an old ENet host"); p = nullptr; }
        if (p) msg_send_raw(p, m);
    }
}
static void msg_forget_peers() { for (void*& p : g_msg_peers) p = nullptr; }

static std::vector<uint8_t> msg_make(uint8_t type, const void* data, size_t len) {
    std::vector<uint8_t> m(kMsgMagic, kMsgMagic + 4);
    m.push_back(type);
    m.insert(m.end(), (const uint8_t*)data, (const uint8_t*)data + len);
    return m;
}

static void msg_handle(void* peer, const uint8_t* p, size_t len) {
    ++g_msg_rx;
    uint8_t type = p[4];
    const uint8_t* body = p + 5;
    size_t n = len - 5;
    static int logged = 0;
    if (type == MSG_PING && n >= 4) {
        uint32_t seq = *(const uint32_t*)body;
        if (logged++ < 20) LOG_INFO("coopmsg ping seq=" + std::to_string(seq) + " -> pong");
        engine_msg_send(MSG_PONG, &seq, 4);  // sent from the engine's send path, see msg_flush
    } else if (type == MSG_PONG && n >= 4) {
        if (logged++ < 20) LOG_INFO("coopmsg pong seq=" + std::to_string(*(const uint32_t*)body));
    } else if (type == MSG_LEVEL) {  // host: its new level is up and hosted
        LOG_INFO(std::string("coopmsg level ready") + (g_level_follow ? "" : " (not following, ignored)"));
        if (g_level_follow) g_level_ready = true;
    } else if (type == MSG_ACT && n >= 4 && g_level_follow) {
        // following a level change: these belong to the host's new level; the rejoin replays them
    } else if (type == MSG_ACT && n >= 4) {  // {u8 on; u16 ordinal; name}; only the host sends these
        // A replay arrives at connect, before our level is loaded: engine_tick runs them once we are in session.
        std::lock_guard<std::mutex> lk(g_msg_mtx);
        if (g_act_pending.size() < 1024)
            g_act_pending.push_back("@act " + std::to_string(body[0]) + " " + std::to_string(*(const uint16_t*)(body + 1)) +
                                    " " + std::string((const char*)body + 3, n - 3));
    } else if (logged++ < 20) {
        LOG_INFO("coopmsg unknown type=" + std::to_string(type) + " len=" + std::to_string(len));
    }
}

// Called after the engine's own enet_peer_send, so we only touch ENet in the context where the engine sends (the
// send job may run on another thread than enet_host_service). Sends queued messages, plus a ping every 5 s.
static void msg_flush() {
    std::vector<std::vector<uint8_t>> out;
    { std::lock_guard<std::mutex> lk(g_msg_mtx); out.swap(g_msg_out); }
    if (g_replay_peer) {  // a client joined: replay this level's player actions to it first, in order
        std::vector<std::vector<uint8_t>> log;
        { std::lock_guard<std::mutex> lk(g_msg_mtx); log = g_act_log; }
        for (auto& m : log) msg_send_raw(g_replay_peer, m);
        if (!log.empty()) LOG_INFO("coopmsg replayed " + std::to_string(log.size()) + " actions to a new peer");
        g_replay_peer = nullptr;
    }
    for (auto& m : out) msg_send_all(m);
    static uint64_t next_ping = 0;
    static uint32_t seq = 0;
    uint64_t now = GetTickCount64();
    if (now >= next_ping) { next_ping = now + 5000; ++seq; msg_send_all(msg_make(MSG_PING, &seq, 4)); }
}

bool engine_msg_send(uint8_t type, const void* data, size_t len) {
    std::lock_guard<std::mutex> lk(g_msg_mtx);
    if (g_msg_out.size() > 1024) return false;  // no peer for a long time: drop instead of growing
    g_msg_out.push_back(msg_make(type, data, len));
    return true;
}

static int __fastcall service_hook(void* host, uint8_t* ev, uint32_t timeout) {
    ++g_service_calls;
    // The engine stops sending to a peer it has no player slot for (host after re-hosting a level: "players 2 -> 1"),
    // and then nothing would carry our messages. Flush here too, but only while the engine's send path is idle.
    if (GetTickCount64() - g_engine_send_ms > 200) msg_flush();
    for (;;) {
        int r = g_service_orig(host, ev, timeout);
        if (r <= 0 || !ev) return r;
        uint32_t type = *(uint32_t*)ev;
        void* peer = *(void**)(ev + 8);
        if (type == 2) {
            if (local_slot() >= 1) g_host_gone = true;
            for (void*& p : g_msg_peers) if (p == peer) p = nullptr;
        }
        uint8_t* pkt = *(uint8_t**)(ev + 24);
        size_t len = (type == 3 && pkt) ? *(size_t*)(pkt + 24) : 0;
        uint8_t* data = len ? *(uint8_t**)(pkt + 16) : nullptr;
        if (data && len >= 5 && memcmp(data, kMsgMagic, 4) == 0) {  // ours: handle, free, service again
            msg_handle(peer, data, len);
            msg_packet_destroy(pkt);
            continue;
        }
        if (type == 3 && g_level_follow) { msg_packet_destroy(pkt); continue; }  // host's next level: not for us
        uint8_t first = data ? *data : 0;
        if (type != 3 || len > 4096 || g_events_logged++ < 150)
            LOG_INFO("enet event type=" + std::to_string(type) + " ch=" + std::to_string(ev[16]) +
                     " len=" + std::to_string(len) + " msg=" + std::to_string(first));
        return r;
    }
}

static int __fastcall peer_send_hook(void* peer, uint8_t ch, uint8_t* pkt) {
    ++g_sends;
    bool known = false;
    for (void* p : g_msg_peers) known |= p == peer;
    for (void*& p : g_msg_peers) if (!known && !p) { p = peer; known = true; g_replay_peer = peer; }
    size_t len = pkt ? *(size_t*)(pkt + 24) : 0;
    uint8_t first = (len && *(uint8_t**)(pkt + 16)) ? **(uint8_t**)(pkt + 16) : 0;
    if (len > 4096 || g_sends_logged++ < 60)
        LOG_INFO("enet send ch=" + std::to_string(ch) + " len=" + std::to_string(len) + " msg=" + std::to_string(first));
    g_engine_send_ms = GetTickCount64();
    int r = 0;
    if (g_level_follow && local_slot() >= 1) msg_packet_destroy(pkt);  // our old level's state: keep it from the host
    else r = g_peer_send_orig(peer, ch, pkt);
    msg_flush();
    return r;
}

// Diagnostics: net send job (RVA 0x6B4D00, queued every net frame). Gate: time global RVA 0xFC1124 >= [net_core+0xC].
using send_job_t = void(__fastcall*)(uint8_t*);
static send_job_t g_send_job_orig = nullptr;
static std::atomic<uint64_t> g_send_jobs{0};
static std::atomic<uint32_t> g_last_next_send{0};
static uint32_t* g_net_time = nullptr;  // RVA 0xFC1124
static void __fastcall send_job_hook(uint8_t* nc) {
    ++g_send_jobs;
    g_last_next_send = *(uint32_t*)(nc + 0xC);
    g_send_job_orig(nc);
}

// Bot input: fake XInput pad 0 (metro.exe imports XInputGetState = xinput1_3 ordinal 2; keyboard/mouse are DirectInput8,
// which needs focus). After the first "@pad" the pad stays "connected" (idle) so the game doesn't see it come and go.
struct FakePad { uint32_t packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
static std::mutex g_pad_mutex;
static FakePad g_pad{};
static bool g_pad_on = false;
static uint64_t g_pad_until = 0;
using xgs_t = DWORD(WINAPI*)(DWORD, void*);
static xgs_t g_xgs_orig = nullptr;
// "@aim <id hex> <ms>": turn the controlled entity toward entity <id> with the right stick (yaw only). Error = angle
// between its forward row (+0xE0, x/z) and the direction to the target. The stick sign is not known, so flip it once
// if the error grows.
static unsigned g_aim_id = 0xFFFF;
static float g_aim_sign = 1, g_aim_ref = 0, g_aim_err = 0;
static int g_aim_frames = 0;
static void aim_step() {
    uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);  // shortcut: fixed RVA
    uint8_t* e = lvl ? *(uint8_t**)(lvl + 0x38) : nullptr;
    uint8_t* t = entity_by_id(g_aim_id);
    if (!e || !t) { g_pad.rx = 0; return; }
    float fx = *(float*)(e + 0xE0), fz = *(float*)(e + 0xE8);
    float dx = *(float*)(t + 0xF0) - *(float*)(e + 0xF0), dz = *(float*)(t + 0xF8) - *(float*)(e + 0xF8);
    float err = atan2f(fx * dz - fz * dx, fx * dx + fz * dz);
    g_aim_err = err;
    if (++g_aim_frames % 30 == 0) {
        if (g_aim_frames == 30 || fabsf(err) <= g_aim_ref) g_aim_ref = fabsf(err);
        else if (fabsf(err) > g_aim_ref + 0.05f && g_aim_sign > 0) { g_aim_sign = -1; g_aim_ref = fabsf(err); }
    }
    float rx = fabsf(err) < 0.03f ? 0 : g_aim_sign * (err > 0 ? 1 : -1) * (std::min)(32000.f, 9000 + fabsf(err) * 20000);
    g_pad.rx = (int16_t)rx;
}

static DWORD WINAPI xgs_hook(DWORD user, void* state) {
    if (user == 0 && g_pad_on && state) {
        std::lock_guard<std::mutex> lock(g_pad_mutex);
        if (GetTickCount64() >= g_pad_until) {
            if (g_aim_id != 0xFFFF) {
                char b[96];
                snprintf(b, sizeof(b), "engine: aim %X done, error %.2f rad, sign %.0f", g_aim_id, g_aim_err, g_aim_sign);
                LOG_INFO(b);
                g_aim_id = 0xFFFF;
            }
            g_pad.buttons = 0; g_pad.lx = g_pad.ly = g_pad.rx = g_pad.ry = 0; g_pad.lt = g_pad.rt = 0;
        } else if (g_aim_id != 0xFFFF) aim_step();
        ++g_pad.packet;
        memcpy(state, &g_pad, sizeof(g_pad));
        return ERROR_SUCCESS;
    }
    return g_xgs_orig(user, state);
}
static void aim_command(unsigned id, unsigned ms) {
    pad_command("0 0 0 " + std::to_string(ms));  // hooks XInput if needed; idle pad for <ms>
    std::lock_guard<std::mutex> lock(g_pad_mutex);
    g_aim_id = id; g_aim_sign = 1; g_aim_frames = 0;
}
static void pad_command(const std::string& args) {  // "<buttons hex> <lx> <ly> <ms> [rx ry lt rt]" (sticks -32768..32767)
    if (!g_xgs_orig) {
        HMODULE x = LoadLibraryA("xinput1_3.dll");
        LPVOID f = x ? (LPVOID)GetProcAddress(x, (LPCSTR)2) : nullptr;
        if (!f || MH_CreateHook(f, (LPVOID)&xgs_hook, (LPVOID*)&g_xgs_orig) != MH_OK || MH_EnableHook(f) != MH_OK) {
            LOG_ERROR("engine: XInputGetState hook failed");
            return;
        }
    }
    unsigned b = 0; int lx = 0, ly = 0, ms = 0, rx = 0, ry = 0, lt = 0, rt = 0;  // optional: right stick, triggers 0..255
    sscanf_s(args.c_str(), "%x %d %d %d %d %d %d %d", &b, &lx, &ly, &ms, &rx, &ry, &lt, &rt);
    std::lock_guard<std::mutex> lock(g_pad_mutex);
    g_pad.buttons = (uint16_t)b; g_pad.lx = (int16_t)lx; g_pad.ly = (int16_t)ly; g_pad.rx = (int16_t)rx; g_pad.ry = (int16_t)ry;
    g_pad.lt = (uint8_t)lt; g_pad.rt = (uint8_t)rt;
    g_pad_until = GetTickCount64() + ms;
    g_pad_on = true;
    LOG_INFO("engine: pad buttons=0x" + args);
}

// Diagnostics: hardware write-watchpoint (DR0) on one address in every thread; logs the first writers' RVAs.
static uintptr_t g_watch_addr = 0;
static std::atomic<int> g_watch_hits{0};
static LONG CALLBACK watch_veh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_watch_hits++ < 8) {
        uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
        char buf[160];
        sprintf_s(buf, "watch: write to %p by rip RVA 0x%llX (value now %u) tid %lu", (void*)g_watch_addr,
                  (unsigned long long)(e->ContextRecord->Rip - base), *(uint32_t*)g_watch_addr, GetCurrentThreadId());
        LOG_INFO(buf);
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
// Crash diagnostics: log the first access violations (first chance, so handled ones show up too) with RIP and the
// stack values that point into metro.exe, as RVAs. Added for the client crash in a garbage address during a
// change_map_cl reload (no module at the fault address).
static LONG CALLBACK av_veh(EXCEPTION_POINTERS* e) {
    static std::atomic<int> n{0};
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || n++ >= 6) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr), size = 0x1100000;  // shortcut: exe image size ~17 MB
    uintptr_t* sp = (uintptr_t*)e->ContextRecord->Rsp;
    char b[400];
    int len = snprintf(b, sizeof(b), "engine: AV rip %llX (rva %llX) addr %llX stack rvas:",
                       (unsigned long long)e->ContextRecord->Rip, (unsigned long long)(e->ContextRecord->Rip - base),
                       (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
    for (int i = 0, found = 0; i < 256 && found < 6 && len < 380; ++i) {
        uintptr_t v = sp[i];
        if (v > base && v < base + size) { len += snprintf(b + len, sizeof(b) - len, " %llX", (unsigned long long)(v - base)); ++found; }
    }
    LOG_INFO(b);
    auto* c = e->ContextRecord;  // raw words around rsp: [rsp-8] == rip means a ret to a smashed return address
    snprintf(b, sizeof(b), "engine: AV regs rsp-8=%llX rsp=%llX %llX %llX %llX rcx=%llX rdx=%llX rbx=%llX rdi=%llX rsi=%llX",
             (unsigned long long)sp[-1], (unsigned long long)sp[0], (unsigned long long)sp[1], (unsigned long long)sp[2],
             (unsigned long long)sp[3], (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, (unsigned long long)c->Rbx,
             (unsigned long long)c->Rdi, (unsigned long long)c->Rsi);
    LOG_INFO(b);
    return EXCEPTION_CONTINUE_SEARCH;
}

void engine_watch_write(uintptr_t addr) {
    g_watch_addr = addr;
    AddVectoredExceptionHandler(1, watch_veh);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = addr;
            c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18);  // L0, RW0=write, LEN0=4 bytes
            if (SetThreadContext(t, &c)) ++n;
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    LOG_INFO("watch: DR0 set on " + std::to_string(n) + " threads");
}

static void engine_watch_net_time() { if (g_net_time) engine_watch_write((uintptr_t)g_net_time); }

// Diagnostics (hybrid netcode step 3, FINDINGS 5.12): log every script action block as it activates/deactivates.
// RVA 0x752D50(owner, stream, version) creates every script block: it reads a u32 clsid from the stream, finds the
// factory in the sorted registry (RVA 0x6C8BF0: {u32 clsid; pad; factory*} x count u16 @+10), calls factory->vtbl[0]
// (create), then obj->vtbl[0x20](owner) and (obj+8)->vtbl[0x20](stream, version) (load parameters).
// Factory: +8 clsid, +0x10 interned name string (e.g. "actions/player/hide subj"). For "actions/" blocks we patch
// vtable slots +0x170 (activate; RVA 0x6D040 for actor actions, which runs vtbl+0x198 on each target) and +0x178
// (deactivate; RVA 0x6D090, vtbl+0x1A0 on each target) with one detour that logs the block name.
// shortcut: fixed RVAs, a log cap, names found by a heuristic scan of the name object; this is a probe.
using block_create_t = uint8_t*(__fastcall*)(void*, uint8_t**, uint16_t);
static block_create_t g_block_create_orig = nullptr;
using act_slot_t = uint64_t(__fastcall*)(uint8_t*, uint64_t, uint64_t, uint64_t);
static std::mutex g_act_mtx;
struct ActInfo { std::string name; uint16_t ord; };
static std::unordered_map<uint8_t*, ActInfo> g_act_name;               // block object -> registered name, ordinal
static std::unordered_map<std::string, uint8_t*> g_act_key;            // "name#ordinal" -> block object (this level)
static std::unordered_map<std::string, uint16_t> g_act_count;          // name -> blocks created so far (this level)
static int g_act_level = -1;                                           // g_level_loads when they were made
static std::unordered_map<uintptr_t, act_slot_t> g_act_orig;           // &vtable slot -> original function
static std::atomic<int> g_act_logged{0};

static std::string interned_text(uint8_t* o) {  // first printable C string of >= 4 chars in the name object
    if (!o) return {};
    for (int off = 0; off < 0x40; ++off) {
        const char* c = (const char*)o + off;
        int n = 0;
        while (n < 120 && c[n] >= 0x20 && c[n] < 0x7F) ++n;
        if (n >= 4 && c[n] == 0) return std::string(c, n);
    }
    return {};
}

static uint64_t act_slot_call(int slot, uint8_t* a, uint64_t x, uint64_t y, uint64_t z) {
    act_slot_t orig = nullptr;
    std::string name;
    uint16_t ord = 0;
    {
        std::lock_guard<std::mutex> lk(g_act_mtx);
        auto it = g_act_orig.find((uintptr_t)*(void**)a + slot);
        if (it != g_act_orig.end()) orig = it->second;
        auto n = g_act_name.find(a);
        if (n != g_act_name.end()) { name = n->second.name; ord = n->second.ord; }
    }
    // Host (or single player): forward actions on the player to clients, and keep them for a late joiner's replay.
    // Blocks are matched by name + creation ordinal: both sides load the same level script in the same order.
    if (local_slot() < 1 && name.rfind("actions/player/", 0) == 0) {
        std::vector<uint8_t> body{(uint8_t)(slot == 0x170), (uint8_t)ord, (uint8_t)(ord >> 8)};
        body.insert(body.end(), name.begin(), name.end());
        { std::lock_guard<std::mutex> lk(g_msg_mtx); if (g_act_log.size() < 512) g_act_log.push_back(msg_make(MSG_ACT, body.data(), body.size())); }
        engine_msg_send(MSG_ACT, body.data(), body.size());
    }
    if (g_act_logged++ < 5000) {
        char b[200];
        snprintf(b, sizeof(b), "act %s '%s'#%u a=%p vt=%llX", slot == 0x170 ? "on" : "off", name.c_str(), ord, a,
                 (unsigned long long)((uintptr_t)*(void**)a - (uintptr_t)GetModuleHandleA(nullptr)));
        LOG_INFO(b);
    }
    return orig ? orig(a, x, y, z) : 0;
}
static uint64_t __fastcall act_on_detour(uint8_t* a, uint64_t x, uint64_t y, uint64_t z) { return act_slot_call(0x170, a, x, y, z); }
static uint64_t __fastcall act_off_detour(uint8_t* a, uint64_t x, uint64_t y, uint64_t z) { return act_slot_call(0x178, a, x, y, z); }

static void act_patch_slot(uint8_t* vt, int slot, void* detour) {  // caller holds g_act_mtx
    auto** p = (void**)(vt + slot);
    if (*p == detour || g_act_orig.count((uintptr_t)p)) return;
    DWORD old;
    if (!VirtualProtect(p, 8, PAGE_READWRITE, &old)) return;
    g_act_orig[(uintptr_t)p] = (act_slot_t)*p;
    *p = detour;
    VirtualProtect(p, 8, old, &old);
}

static uint8_t* __fastcall block_create_hook(void* owner, uint8_t** stream, uint16_t ver) {
    uint32_t clsid = 0xFFFFFFFF;  // peek like RVA 0x752D50 does for version >= 0xE (raw u32 at the read position)
    if (ver > 0xD && stream && *stream) clsid = *(uint32_t*)(*(uint8_t**)(*stream + 0x38) + *(int32_t*)(*stream + 0x40));
    uint8_t* obj = g_block_create_orig(owner, stream, ver);
    if (!obj || clsid == 0xFFFFFFFF) return obj;
    auto* reg = ((uint8_t*(__fastcall*)())((uint8_t*)GetModuleHandleA(nullptr) + 0x6C8BF0))();
    uint8_t* e = *(uint8_t**)reg;
    uint16_t n = *(uint16_t*)(reg + 10);
    uint8_t* fac = nullptr;
    for (uint16_t i = 0; i < n && !fac; ++i) if (*(uint32_t*)(e + i * 16) == clsid) fac = *(uint8_t**)(e + i * 16 + 8);
    std::string name = fac ? interned_text(*(uint8_t**)(fac + 0x10)) : std::string();
    static int logged = 0;
    if (logged < 3 && fac) {  // probe check: show the raw name object once
        ++logged;
        char b[200];
        uint8_t* no = *(uint8_t**)(fac + 0x10);
        snprintf(b, sizeof(b), "block clsid=%X name='%s' nameobj=%016llX %016llX %016llX %016llX", clsid, name.c_str(),
                 no ? *(unsigned long long*)no : 0, no ? *(unsigned long long*)(no + 8) : 0,
                 no ? *(unsigned long long*)(no + 16) : 0, no ? *(unsigned long long*)(no + 24) : 0);
        LOG_INFO(b);
    }
    if (name.rfind("actions/", 0) != 0) return obj;
    std::lock_guard<std::mutex> lk(g_act_mtx);
    if (g_level_loads != g_act_level) {  // first block of a new level load: forget the old level's blocks
        g_act_level = g_level_loads;
        g_act_name.clear(); g_act_key.clear(); g_act_count.clear();
        std::lock_guard<std::mutex> lk2(g_msg_mtx);
        g_act_log.clear();
    }
    uint16_t ord = g_act_count[name]++;
    g_act_name[obj] = {name, ord};
    g_act_key[name + "#" + std::to_string(ord)] = obj;
    // Patch a slot only if it is inside the primary vtable, which ends where the +8 interface's vtable starts (e.g.
    // actions/engine/signal[S], vtable RVA 0xE333B0, is 0x178 long: its +0x178 is the +8 interface's destructor).
    uint8_t* vt = *(uint8_t**)obj;
    ptrdiff_t len = *(uint8_t**)(obj + 8) - vt;
    if (len > 0x400) len = 0;  // +8 vtable not adjacent: length unknown, don't patch
    if (len >= 0x178) act_patch_slot(vt, 0x170, (void*)&act_on_detour);
    if (len >= 0x180) act_patch_slot(vt, 0x178, (void*)&act_off_detour);
    return obj;
}

// Client: queue the host's forwarded script actions on the game thread (exec queue).
static void act_pending_run() {
    std::vector<std::string> acts;
    { std::lock_guard<std::mutex> lk(g_msg_mtx); acts.swap(g_act_pending); }
    for (auto& a : acts) engine_exec(a);
}

// Client, game thread: run the host's forwarded action on our block with the same name and ordinal.
// shortcut: a block destroyed mid-level leaves a stale pointer in g_act_key; the vtable check below catches most
// cases. Upgrade: drop entries in the blocks' destructor.
static void act_run_local(const std::string& args) {
    int on = 0, ord = 0, used = 0;
    if (sscanf_s(args.c_str(), "%d %d %n", &on, &ord, &used) < 2 || !used) return;
    std::string name = args.substr(used);
    uint8_t* obj = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_act_mtx);
        auto it = g_act_key.find(name + "#" + std::to_string(ord));
        if (it != g_act_key.end() && g_act_name.count(it->second) && g_act_name[it->second].name == name) obj = it->second;
    }
    static int logged = 0;
    if (logged++ < 200) LOG_INFO("act forwarded " + std::string(on ? "on " : "off ") + name + "#" + std::to_string(ord) + (obj ? "" : " (no such block here)"));
    if (!obj) return;
    int slot = on ? 0x170 : 0x178;
    ptrdiff_t len = *(uint8_t**)(obj + 8) - *(uint8_t**)obj;  // slot must be inside the primary vtable (see above)
    if (len < slot + 8 || len > 0x400) return;
    uint64_t out = 0;
    ((act_slot_t)(*(void***)obj)[slot / 8])(obj, on ? (uint64_t)&out : 0, 0, 0);
}

// Diagnostics: movement action's per-target apply/restore (RVA 0x3EF4C4 / 0x3EF500(action, target)): which entity
// does a client's forwarded `actions/player/movement` reach? (FINDINGS 5.12)
static act_slot_t g_mv_on_orig = nullptr, g_mv_off_orig = nullptr;
static void mv_log(const char* what, uint8_t* a, uint8_t* t) {
    static int logged = 0;
    if (logged++ >= 100) return;
    char b[160];
    uint8_t* lvl = *(uint8_t**)((uint8_t*)GetModuleHandleA(nullptr) + 0xFBD820);
    uint8_t* ctl = lvl ? *(uint8_t**)(lvl + 0x38) : nullptr;
    // target arg -> ref -> entity+0xA0 (like RVA 0x4D7B0, which then casts with entity vtbl+0x6E0)
    uint8_t* r = t ? *(uint8_t**)t : nullptr;
    uint8_t* e = r && *(uint8_t**)r ? *(uint8_t**)r - 0xA0 : nullptr;
    snprintf(b, sizeof(b), "movement %s a=%p target=%p entity=%p id=%X ctl=%p id=%X", what, a, t, e,
             e ? *(uint16_t*)(e + 0x108) : 0xFFFF, ctl, ctl ? *(uint16_t*)(ctl + 0x108) : 0xFFFF);
    LOG_INFO(b);
}
static uint64_t __fastcall mv_on_hook(uint8_t* a, uint64_t t, uint64_t y, uint64_t z) { mv_log("apply", a, (uint8_t*)t); return g_mv_on_orig(a, t, y, z); }
static uint64_t __fastcall mv_off_hook(uint8_t* a, uint64_t t, uint64_t y, uint64_t z) { mv_log("restore", a, (uint8_t*)t); return g_mv_off_orig(a, t, y, z); }

static void install_netframe_counter() {
    auto* exe = (uint8_t*)GetModuleHandleA(nullptr);
    if (MH_CreateHook(exe + 0x3EF4C4, (LPVOID)&mv_on_hook, (LPVOID*)&g_mv_on_orig) == MH_OK) MH_EnableHook(exe + 0x3EF4C4);
    if (MH_CreateHook(exe + 0x3EF500, (LPVOID)&mv_off_hook, (LPVOID*)&g_mv_off_orig) == MH_OK) MH_EnableHook(exe + 0x3EF500);
    if (MH_CreateHook(exe + 0x752D50, (LPVOID)&block_create_hook, (LPVOID*)&g_block_create_orig) == MH_OK) MH_EnableHook(exe + 0x752D50);
    if (auto sj = find("40 55 56 41 54 41 55 41 56 48 8D 6C 24 C9 48 81 EC D0 00 00 00 4C 8D B1 88 01 00 00 4C"))
        if (MH_CreateHook((LPVOID)*sj, (LPVOID)&send_job_hook, (LPVOID*)&g_send_job_orig) == MH_OK) {
            MH_EnableHook((LPVOID)*sj);
            g_net_time = (uint32_t*)(*sj + 0x55 + *(int32_t*)(*sj + 0x51));  // mov rax,[rip+disp] at +0x4E
        }
    if (auto cs = find("0F B6 54 24 78 4C 8B C7 49 8B CE E8")) {
        LPVOID ps = (LPVOID)(*cs + 16 + *(int32_t*)(*cs + 12));
        if (MH_CreateHook(ps, (LPVOID)&peer_send_hook, (LPVOID*)&g_peer_send_orig) == MH_OK) MH_EnableHook(ps);
    }
    auto at = find("40 53 48 83 EC 30 80 3D ? ? ? ? 00 48 8B D9 75");
    if (at && MH_CreateHook((LPVOID)*at, (LPVOID)&netframe_hook, (LPVOID*)&g_netframe_orig) == MH_OK)
        MH_EnableHook((LPVOID)*at);
    auto sv = find("48 89 5C 24 08 44 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC A0 04 00 00 45 33 ED");
    if (sv && MH_CreateHook((LPVOID)*sv, (LPVOID)&service_hook, (LPVOID*)&g_service_orig) == MH_OK)
        MH_EnableHook((LPVOID)*sv);
    LOG_INFO(std::string("engine: enet event log ") + (sv ? "on" : "NOT FOUND"));
}

// *(g_level+0x38) = the entity this instance controls (a client's send job only sends while it is set, RVA
// 0x6B4D00). Id at +0x108 (+0xA0+0x68); position guessed as the transform translation at +0xA0+0x50.
static void ctl_str(char* b, size_t n) {
    uint8_t* level = g_level_signal ? *(uint8_t**)((uint8_t*)g_level_signal + 0x21 + *(int32_t*)((uint8_t*)g_level_signal + 0x1D)) : nullptr;
    __try {
        uint8_t* e = level ? *(uint8_t**)(level + 0x38) : nullptr;
        if (e) snprintf(b, n, " ctl=%X@(%.1f,%.1f,%.1f)", *(uint16_t*)(e + 0x108), *(float*)(e + 0xF0),
                        *(float*)(e + 0xF4), *(float*)(e + 0xF8));
        else snprintf(b, n, " ctl=none");
        // host's networked entities (send candidates, FUN_140747630): net_core+0x168 {u16 id; pad; handle} x count(+0x172)
        if (g_players) {
            uint8_t* nc = (uint8_t*)g_players - 0x18;
            int cnt = *(uint16_t*)(nc + 0x172);
            size_t used = strlen(b);
            used += snprintf(b + used, n - used, " net=%d", cnt);
            for (int i = 0; i < cnt && i < 6 && used < n; ++i)
                used += snprintf(b + used, n - used, "%c%X", i ? ',' : ':', *(uint16_t*)(*(uint8_t**)(nc + 0x168) + i * 16));
        }
        for (uint8_t* a : g_seen_actors) {  // actors the aim hook saw, same layout
            size_t used = strlen(b);
            if (a && used < n) snprintf(b + used, n - used, " a%X@(%.1f,%.1f)", *(uint16_t*)(a + 0x108),
                                        *(float*)(a + 0xF0), *(float*)(a + 0xF8));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(b, n, " ctl=?"); }
}

// per net player: actor id (+0x64), state bits (+0x68 >> 16 & 7; 4 = waiting for actor, 5 = linked), net pos (+0x48)
static std::string slots_str() {
    std::string s;
    for (int i = 0; g_players && i < g_players->size && i < 4; ++i) {
        uint8_t* p = (uint8_t*)g_players->array + i * 0x70;
        char b[96];
        snprintf(b, sizeof(b), " s%d=%X/%u/(%.1f,%.1f,%.1f)", i, *(uint16_t*)(p + 0x64), (*(uint32_t*)(p + 0x68) >> 16) & 7,
                 *(float*)(p + 0x48), *(float*)(p + 0x4C), *(float*)(p + 0x50));
        s += b;
    }
    char b[256];
    ctl_str(b, sizeof(b));
    return s + b;
}

std::string engine_stats() {
    return "engine_mp=" + std::to_string(g_mp_flag ? *g_mp_flag : -1) +
           " netframes=" + std::to_string(g_netframes.load()) +
           " players=" + std::to_string(g_players ? g_players->size : -1) +
           " service_calls=" + std::to_string(g_service_calls.load()) + " sends=" + std::to_string(g_sends.load()) +
           " msg_tx=" + std::to_string(g_msg_tx.load()) + " msg_rx=" + std::to_string(g_msg_rx.load()) +
           " send_jobs=" + std::to_string(g_send_jobs.load()) + " next_send=" + std::to_string(g_last_next_send.load()) +
           " net_time=" + std::to_string(g_net_time ? *g_net_time : 0) +
           (g_loader ? " paused=" + std::to_string(((uint8_t*)g_loader)[0x3244]) + "/" + std::to_string(((uint8_t*)g_loader)[0x3245]) +
                       " pstack=" + std::to_string(*(uint16_t*)((uint8_t*)g_loader + 0x16fe0 + 8)) + "/" + std::to_string(*(uint16_t*)((uint8_t*)g_loader + 0x16fe0 + 10)) : std::string()) +
           " local_idx=" + std::to_string(g_players ? *(int32_t*)((uint8_t*)g_players - 0x18 + 8) : -2) + slots_str();
}

bool engine_host_dead() {
    uint8_t* e = controlled();
    return g_mp_flag && *g_mp_flag && local_slot() == 0 && e && *(float*)(e + 0x308) <= 0;
}
bool engine_client_in_session() {
    return !g_host_gone && g_mp_flag && (*g_mp_flag || g_save_since) && local_slot() >= 1 && g_players->size >= 2 && controlled();
}
bool engine_checkpoint_saved() { return g_checkpoint_saved; }
bool engine_level_following() { return g_level_follow; }
bool engine_level_ready() { return g_level_ready.exchange(false); }
bool engine_hosting() {  // net_core.host is a real ENet host (1 = local-only marker) and we are slot 0
    return g_players && *(uintptr_t*)((uint8_t*)g_players - 0x18) > 1 && local_slot() == 0;
}

bool engine_exec(const std::string& cmd) {
    if (!g_get_console || !g_peek_orig) return false;
    std::lock_guard<std::mutex> lock(g_cmd_mutex);
    g_cmd_queue.push_back(cmd);
    g_cmd_pending = true;
    return true;
}

} // namespace metro_coop
