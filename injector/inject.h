// LoadLibrary injection and "launch metro.exe with the single-instance check patched", shared by injector.exe and
// MetroCoopLauncher.exe. Errors go to std::wcerr (invisible in the launcher, which checks the return values).
#pragma once
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

inline bool inject_dll(DWORD process_id, const std::wstring& dll_path) {
    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, process_id);
    if (!process) {
        std::wcerr << L"Failed to open process. Error: " << GetLastError() << std::endl;
        return false;
    }

    // Allocate memory in target process for DLL path
    SIZE_T path_size = (dll_path.length() + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, NULL, path_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        std::wcerr << L"Failed to allocate memory in target process. Error: " << GetLastError() << std::endl;
        CloseHandle(process);
        return false;
    }

    // Write DLL path to target process
    if (!WriteProcessMemory(process, remote_path, dll_path.c_str(), path_size, NULL)) {
        std::wcerr << L"Failed to write DLL path. Error: " << GetLastError() << std::endl;
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // Get LoadLibraryW address
    LPVOID load_library_addr = (LPVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!load_library_addr) {
        std::wcerr << L"Failed to get LoadLibraryW address. Error: " << GetLastError() << std::endl;
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // Create remote thread to call LoadLibraryW
    HANDLE thread = CreateRemoteThread(process, NULL, 0,
        (LPTHREAD_START_ROUTINE)load_library_addr, remote_path, 0, NULL);
    if (!thread) {
        std::wcerr << L"Failed to create remote thread. Error: " << GetLastError() << std::endl;
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // Wait for thread to finish
    WaitForSingleObject(thread, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);

    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(thread);

    if (exit_code == 0) {
        std::wcerr << L"LoadLibraryW returned 0 (failed)" << std::endl;
        CloseHandle(process);
        return false;
    }

    // DLL loaded successfully - DllMain will create its own initialization thread
    // No need to call any export function
    std::wcout << L"DLL injected successfully! Base address: 0x" << std::hex << (uintptr_t)exit_code << std::endl;
    CloseHandle(process);
    return true;
}

// The game's single-instance check: CreateMutexW(...); if (GetLastError() == ERROR_ALREADY_EXISTS) MessageBox + exit.
// Bytes: cmp edi,0B7h / jne skip / mov r9d,10h (MB_ICONERROR). We flip jne (75) to jmp (EB) before the game runs.
// Found at RVA 0x655CAC in metro_nextgen_patched.exe (Steam Next-Gen build); matched by pattern so other builds still work.
inline constexpr int kMutexPatternLen = 14;
inline bool mutex_pattern_at(const unsigned char* p) {
    static const unsigned char pat[kMutexPatternLen] = {0x81,0xFF,0xB7,0,0,0,0x75,0,0x41,0xB9,0x10,0,0,0};
    for (int i = 0; i < kMutexPatternLen; ++i)
        if (i != 7 && p[i] != pat[i]) return false;
    return true;
}

typedef LONG(NTAPI* NtQIP)(HANDLE, int, PVOID, ULONG, PULONG);

// Start the exe suspended, patch the single-instance check in memory, resume. Returns the new pid or 0.
inline DWORD launch_patched(const std::wstring& exe, const std::wstring& args) {
    std::wstring dir = std::filesystem::path(exe).parent_path().wstring();
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        dir.c_str(), &si, &pi)) {
        std::wcerr << L"CreateProcess failed. Error: " << GetLastError() << std::endl;
        return 0;
    }
    bool patched = false;
    // PEB64 + 0x10 = ImageBaseAddress; the exe image is mapped even while the process is suspended at start.
    struct { PVOID r0; PVOID peb; PVOID r[4]; } pbi{};
    auto qip = (NtQIP)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    uintptr_t base = 0;
    if (qip && qip(pi.hProcess, 0, &pbi, sizeof(pbi), nullptr) == 0 &&
        ReadProcessMemory(pi.hProcess, (char*)pbi.peb + 0x10, &base, sizeof(base), nullptr)) {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        ReadProcessMemory(pi.hProcess, (LPCVOID)base, &dos, sizeof(dos), nullptr);
        ReadProcessMemory(pi.hProcess, (LPCVOID)(base + dos.e_lfanew), &nt, sizeof(nt), nullptr);
        std::vector<unsigned char> img(nt.OptionalHeader.SizeOfCode + nt.OptionalHeader.BaseOfCode);
        SIZE_T got = 0;
        uintptr_t code = base + nt.OptionalHeader.BaseOfCode;
        ReadProcessMemory(pi.hProcess, (LPCVOID)code, img.data(), nt.OptionalHeader.SizeOfCode, &got);
        for (SIZE_T i = 0; got >= kMutexPatternLen && i + kMutexPatternLen <= got; ++i) {
            if (!mutex_pattern_at(&img[i])) continue;
            LPVOID at = (LPVOID)(code + i + 6);
            DWORD old;
            unsigned char jmp = 0xEB;
            patched = VirtualProtectEx(pi.hProcess, at, 1, PAGE_EXECUTE_READWRITE, &old) &&
                      WriteProcessMemory(pi.hProcess, at, &jmp, 1, nullptr);
            VirtualProtectEx(pi.hProcess, at, 1, old, &old);
            FlushInstructionCache(pi.hProcess, at, 1);
            std::wcout << L"Single-instance check at RVA 0x" << std::hex << (code + i - base) << std::dec
                       << (patched ? L" patched" : L" FAILED to patch") << std::endl;
            break;
        }
    }
    if (!patched) std::wcerr << L"Single-instance check not patched (pattern not found?)" << std::endl;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pi.dwProcessId;
}

