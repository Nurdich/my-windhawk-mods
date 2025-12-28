// Better File Sizes - DLL Injector
// Injects BetterFileSizesHook.dll into explorer.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

// Get process IDs by name
std::vector<DWORD> GetProcessIdsByName(const wchar_t* processName) {
    std::vector<DWORD> pids;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return pids;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(pe32);

    if (Process32FirstW(snapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, processName) == 0) {
                pids.push_back(pe32.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &pe32));
    }

    CloseHandle(snapshot);
    return pids;
}

// Enable debug privilege
bool EnableDebugPrivilege() {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }

    bool success = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
                   GetLastError() == ERROR_SUCCESS;

    CloseHandle(hToken);
    return success;
}

// Inject DLL into a process
bool InjectDll(DWORD processId, const wchar_t* dllPath) {
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
    if (!hProcess) {
        wprintf(L"Failed to open process %d: error %d\n", processId, GetLastError());
        return false;
    }

    // Allocate memory for DLL path in target process
    size_t pathSize = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(hProcess, nullptr, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        wprintf(L"VirtualAllocEx failed: error %d\n", GetLastError());
        CloseHandle(hProcess);
        return false;
    }

    // Write DLL path to target process
    if (!WriteProcessMemory(hProcess, remotePath, dllPath, pathSize, nullptr)) {
        wprintf(L"WriteProcessMemory failed: error %d\n", GetLastError());
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Get LoadLibraryW address
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadLibraryW = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW");

    // Create remote thread to load DLL
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, loadLibraryW, remotePath, 0, nullptr);
    if (!hThread) {
        wprintf(L"CreateRemoteThread failed: error %d\n", GetLastError());
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Wait for DLL to load
    WaitForSingleObject(hThread, 10000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    return exitCode != 0;
}

// Unload DLL from a process
bool UnloadDll(DWORD processId, const wchar_t* dllName) {
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
    if (!hProcess) {
        return false;
    }

    // Find the DLL module in the target process
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE) {
        CloseHandle(hProcess);
        return false;
    }

    MODULEENTRY32W me32;
    me32.dwSize = sizeof(me32);

    HMODULE targetModule = nullptr;
    if (Module32FirstW(snapshot, &me32)) {
        do {
            if (_wcsicmp(me32.szModule, dllName) == 0) {
                targetModule = me32.hModule;
                break;
            }
        } while (Module32NextW(snapshot, &me32));
    }

    CloseHandle(snapshot);

    if (!targetModule) {
        CloseHandle(hProcess);
        return false;
    }

    // Get FreeLibrary address
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE freeLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "FreeLibrary");

    // Create remote thread to unload DLL
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, freeLibrary, targetModule, 0, nullptr);
    if (!hThread) {
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 10000);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    return true;
}

// Get DLL path (next to injector exe)
std::wstring GetDllPath() {
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    PathAppendW(exePath, L"BetterFileSizesHook.dll");
    return exePath;
}

void PrintUsage() {
    wprintf(L"Better File Sizes - DLL Injector\n");
    wprintf(L"================================\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  BetterFileSizesInjector.exe [command]\n\n");
    wprintf(L"Commands:\n");
    wprintf(L"  inject   - Inject DLL into all explorer.exe processes (default)\n");
    wprintf(L"  unload   - Unload DLL from all explorer.exe processes\n");
    wprintf(L"  status   - Check if DLL is loaded in explorer.exe\n");
    wprintf(L"  help     - Show this help message\n\n");
    wprintf(L"Examples:\n");
    wprintf(L"  BetterFileSizesInjector.exe inject\n");
    wprintf(L"  BetterFileSizesInjector.exe unload\n\n");
    wprintf(L"Notes:\n");
    wprintf(L"  - Run as Administrator for best results\n");
    wprintf(L"  - BetterFileSizesHook.dll must be in the same folder\n");
    wprintf(L"  - Edit BetterFileSizes.ini to configure settings\n");
}

int wmain(int argc, wchar_t* argv[]) {
    std::wstring command = L"inject";

    if (argc > 1) {
        command = argv[1];
    }

    if (command == L"help" || command == L"-h" || command == L"--help" || command == L"/?") {
        PrintUsage();
        return 0;
    }

    std::wstring dllPath = GetDllPath();

    if (command == L"inject") {
        wprintf(L"Better File Sizes - Injecting DLL\n");
        wprintf(L"==================================\n\n");

        // Check if DLL exists
        if (!PathFileExistsW(dllPath.c_str())) {
            wprintf(L"Error: DLL not found: %s\n", dllPath.c_str());
            return 1;
        }

        // Enable debug privilege
        if (!EnableDebugPrivilege()) {
            wprintf(L"Warning: Could not enable debug privilege. Run as Administrator.\n");
        }

        // Find explorer.exe processes
        auto pids = GetProcessIdsByName(L"explorer.exe");
        if (pids.empty()) {
            wprintf(L"No explorer.exe processes found.\n");
            return 1;
        }

        wprintf(L"Found %zu explorer.exe process(es)\n", pids.size());
        wprintf(L"DLL path: %s\n\n", dllPath.c_str());

        int successCount = 0;
        for (DWORD pid : pids) {
            wprintf(L"Injecting into PID %d... ", pid);
            if (InjectDll(pid, dllPath.c_str())) {
                wprintf(L"Success\n");
                successCount++;
            } else {
                wprintf(L"Failed\n");
            }
        }

        wprintf(L"\nInjected into %d/%zu processes\n", successCount, pids.size());
        return successCount > 0 ? 0 : 1;

    } else if (command == L"unload") {
        wprintf(L"Better File Sizes - Unloading DLL\n");
        wprintf(L"==================================\n\n");

        // Enable debug privilege
        if (!EnableDebugPrivilege()) {
            wprintf(L"Warning: Could not enable debug privilege. Run as Administrator.\n");
        }

        auto pids = GetProcessIdsByName(L"explorer.exe");
        if (pids.empty()) {
            wprintf(L"No explorer.exe processes found.\n");
            return 0;
        }

        int successCount = 0;
        for (DWORD pid : pids) {
            wprintf(L"Unloading from PID %d... ", pid);
            if (UnloadDll(pid, L"BetterFileSizesHook.dll")) {
                wprintf(L"Success\n");
                successCount++;
            } else {
                wprintf(L"Not loaded or failed\n");
            }
        }

        wprintf(L"\nUnloaded from %d processes\n", successCount);
        return 0;

    } else if (command == L"status") {
        wprintf(L"Better File Sizes - Status\n");
        wprintf(L"===========================\n\n");

        auto pids = GetProcessIdsByName(L"explorer.exe");
        if (pids.empty()) {
            wprintf(L"No explorer.exe processes found.\n");
            return 0;
        }

        for (DWORD pid : pids) {
            wprintf(L"PID %d: ", pid);

            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snapshot == INVALID_HANDLE_VALUE) {
                wprintf(L"Cannot check (access denied?)\n");
                continue;
            }

            MODULEENTRY32W me32;
            me32.dwSize = sizeof(me32);

            bool found = false;
            if (Module32FirstW(snapshot, &me32)) {
                do {
                    if (_wcsicmp(me32.szModule, L"BetterFileSizesHook.dll") == 0) {
                        found = true;
                        break;
                    }
                } while (Module32NextW(snapshot, &me32));
            }

            CloseHandle(snapshot);
            wprintf(L"%s\n", found ? L"DLL loaded" : L"DLL not loaded");
        }

        return 0;

    } else {
        wprintf(L"Unknown command: %s\n\n", command.c_str());
        PrintUsage();
        return 1;
    }
}
