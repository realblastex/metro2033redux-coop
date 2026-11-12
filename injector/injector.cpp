// Simple DLL injector for Metro 2033 Redux co-op mod
// Just performs standard LoadLibrary injection - DLL's DllMain creates its own thread
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>
#include <vector>
#include <filesystem>

#include "inject.h"

DWORD get_process_id(const std::wstring& process_name) {
    DWORD process_id = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, process_name.c_str()) == 0) {
                process_id = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return process_id;
}

DWORD get_process_id_by_path(const std::wstring& process_path) {
    DWORD process_id = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    // Get full path of target executable
    wchar_t target_full_path[MAX_PATH];
    GetFullPathNameW(process_path.c_str(), MAX_PATH, target_full_path, nullptr);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            // Get full path of this process
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (proc) {
                wchar_t proc_path[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(proc, 0, proc_path, &size)) {
                    if (_wcsicmp(proc_path, target_full_path) == 0) {
                        process_id = entry.th32ProcessID;
                        CloseHandle(proc);
                        break;
                    }
                }
                CloseHandle(proc);
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return process_id;
}

int wmain(int argc, wchar_t* argv[]) {
    // injector.exe --launch <exe path> [game args...] : start the game with the single-instance check disabled (prints pid)
    if (argc >= 3 && std::wstring(argv[1]) == L"--launch") {
        std::wstring args;
        for (int i = 3; i < argc; ++i) args += (i > 3 ? L" " : L"") + std::wstring(argv[i]);
        DWORD pid = launch_patched(argv[2], args);
        if (pid) std::wcout << L"Launched pid " << pid << std::endl;
        return pid ? 0 : 1;
    }
    if (argc < 3) {
        std::wcout << L"Usage: injector.exe <process_name_or_path> <dll_path>" << std::endl;
        std::wcout << L"Example: injector.exe metro.exe E:\\path\\to\\metro_coop.dll" << std::endl;
        std::wcout << L"Example: injector.exe \"C:\\Games\\Metro\\metro.exe\" E:\\path\\to\\metro_coop.dll" << std::endl;
        return 1;
    }

    std::wstring process_arg = argv[1];
    std::wstring dll_path = argv[2];

    // Check if first argument is a path (contains backslash or forward slash)
    bool is_path = process_arg.find(L'\\') != std::wstring::npos ||
                   process_arg.find(L'/') != std::wstring::npos;

    DWORD process_id = 0;
    if (is_path) {
        std::wcout << L"Looking for process by path: " << process_arg << std::endl;
        process_id = get_process_id_by_path(process_arg);
    } else {
        std::wcout << L"Looking for process by name: " << process_arg << std::endl;
        process_id = get_process_id(process_arg);
    }

    if (process_id == 0) {
        std::wcerr << L"Process not found: " << process_arg << std::endl;
        return 1;
    }

    std::wcout << L"Found process ID: " << process_id << std::endl;
    std::wcout << L"Injecting DLL: " << dll_path << std::endl;

    if (inject_dll(process_id, dll_path)) {
        std::wcout << L"Injection successful!" << std::endl;
        return 0;
    } else {
        std::wcerr << L"Injection failed!" << std::endl;
        return 1;
    }
}