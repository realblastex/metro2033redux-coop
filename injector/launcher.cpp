// MetroCoopLauncher.exe: one window with "Host" and "Join <address>". Lives in <game dir>\metro_coop\ next to
// metro_coop.dll. It writes the mode into metro_coop.ini, starts metro.exe (single-instance check patched, so two
// copies can run on one PC) and injects the DLL. The DLL then hosts once a level is loaded, or joins from the menu.
// Command line (tests): MetroCoopLauncher.exe --host | --join <address>  [--second]   (exit code 0 = started)
#include <windows.h>
#include <filesystem>
#include <string>

#include "inject.h"

namespace fs = std::filesystem;

static fs::path g_dir;  // folder of this exe

// metro.exe next to us or one folder up (the normal place: <game dir>\metro_coop\MetroCoopLauncher.exe)
static fs::path find_game() {
    for (fs::path p : {g_dir / L"metro.exe", g_dir.parent_path() / L"metro.exe"})
        if (fs::exists(p)) return p;
    return {};
}

// Hostname / IPv4 characters only: the address ends up in the DLL's ';'-separated command list.
static bool valid_address(const std::wstring& a) {
    if (a.empty() || a.size() > 253) return false;
    for (wchar_t c : a)
        if (!iswalnum(c) && c != L'.' && c != L'-') return false;
    return true;
}

// Returns an error message, or empty on success.
static std::wstring start(bool host, const std::wstring& address, bool second) {
    fs::path exe = find_game(), dll = g_dir / L"metro_coop.dll", ini = g_dir / L"metro_coop.ini";
    if (exe.empty()) return L"metro.exe not found. Put the metro_coop folder into the game folder.";
    if (!fs::exists(dll)) return L"metro_coop.dll not found next to the launcher.";
    if (!host && !valid_address(address)) return L"Enter the host's IP address (for example 192.168.1.20).";
    const wchar_t* s = L"coop";
    if (!WritePrivateProfileStringW(s, L"mode", host ? L"host" : L"join", ini.c_str()) ||
        (!host && !WritePrivateProfileStringW(s, L"join_address", address.c_str(), ini.c_str())) ||
        !WritePrivateProfileStringW(s, L"engine_bind_port", second ? L"1001" : L"0", ini.c_str()))
        return L"Cannot write " + ini.wstring() + L" (is the game folder read-only?)";
    DWORD pid = launch_patched(exe.wstring(), L"");
    if (!pid) return L"Could not start " + exe.wstring();
    Sleep(3000);  // as run_test.ps1: let the process initialise before LoadLibrary
    if (!inject_dll(pid, dll.wstring())) return L"Game started, but loading metro_coop.dll failed.";
    return L"";
}

enum { ID_ADDR = 101, ID_SECOND, ID_HOST, ID_JOIN };

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COMMAND && (LOWORD(wp) == ID_HOST || LOWORD(wp) == ID_JOIN)) {
        bool host = LOWORD(wp) == ID_HOST;
        wchar_t addr[256] = {};
        GetDlgItemTextW(w, ID_ADDR, addr, 256);
        bool second = IsDlgButtonChecked(w, ID_SECOND) == BST_CHECKED;
        EnableWindow(w, FALSE);
        std::wstring err = start(host, addr, second);
        EnableWindow(w, TRUE);
        if (!err.empty()) { MessageBoxW(w, err.c_str(), L"Metro co-op", MB_ICONERROR); return 0; }
        MessageBoxW(w, host ? L"The game is starting.\n\nLoad a save or start a new game. Once you are in the level the "
                              L"game hosts by itself (window title: [host]). Your friend can join then."
                            : L"The game is starting.\n\nIt joins the host from the main menu by itself and gets its own "
                              L"body about 15 s later. If joining fails, press F11 in the menu to retry.",
                    L"Metro co-op", MB_ICONINFORMATION);
        DestroyWindow(w);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_dir = fs::path(self).parent_path();

    if (__argc >= 2) {  // command line mode
        std::wstring a1 = __wargv[1], addr;
        bool second = false;
        for (int i = 2; i < __argc; ++i) {
            if (std::wstring(__wargv[i]) == L"--second") second = true;
            else addr = __wargv[i];
        }
        if (a1 != L"--host" && a1 != L"--join") return 2;
        return start(a1 == L"--host", addr, second).empty() ? 0 : 1;
    }

    wchar_t last[256] = {};  // last address used
    GetPrivateProfileStringW(L"coop", L"join_address", L"", last, 256, (g_dir / L"metro_coop.ini").c_str());

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MetroCoopLauncher";
    RegisterClassW(&wc);
    HWND w = CreateWindowW(wc.lpszClassName, L"Metro 2033 Redux co-op", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                           CW_USEDEFAULT, CW_USEDEFAULT, 380, 210, nullptr, nullptr, inst, nullptr);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int cx, int cy, int id) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, w, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    };
    add(L"BUTTON", L"Host a game", BS_PUSHBUTTON | WS_TABSTOP, 15, 15, 335, 32, ID_HOST);
    add(L"STATIC", L"Host's IP address:", 0, 15, 66, 100, 20, 0);
    add(L"EDIT", last, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 120, 63, 140, 22, ID_ADDR);
    add(L"BUTTON", L"Join", BS_PUSHBUTTON | WS_TABSTOP, 270, 61, 80, 26, ID_JOIN);
    add(L"BUTTON", L"Second copy on this PC (testing)", BS_AUTOCHECKBOX | WS_TABSTOP, 15, 105, 335, 20, ID_SECOND);
    add(L"STATIC", L"Host: allow metro.exe in Windows Firewall (UDP port 1000).", 0, 15, 135, 335, 20, 0);
    ShowWindow(w, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
