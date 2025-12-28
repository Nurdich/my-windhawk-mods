#include "config.h"
#include <shlwapi.h>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>

#pragma comment(lib, "shlwapi.lib")

static HMODULE g_hModule = nullptr;
static bool g_consoleLogging = false;
static std::mutex g_logMutex;

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved);

void SetModuleHandle(HMODULE hModule) {
    g_hModule = hModule;
}

HMODULE GetCurrentModuleHandle() {
    if (!g_hModule) {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetCurrentModuleHandle),
            &g_hModule
        );
    }
    return g_hModule;
}

std::wstring GetConfigPath() {
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(GetCurrentModuleHandle(), dllPath, MAX_PATH);

    PathRemoveFileSpecW(dllPath);
    PathAppendW(dllPath, L"BetterFileSizes.ini");

    return dllPath;
}

std::wstring GetLogPath() {
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(GetCurrentModuleHandle(), dllPath, MAX_PATH);

    PathRemoveFileSpecW(dllPath);
    PathAppendW(dllPath, L"BetterFileSizes.log");

    return dllPath;
}

void SetConsoleLogging(bool enable) {
    g_consoleLogging = enable;
}

void LogMessage(const wchar_t* format, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    WCHAR buffer[2048];
    va_list args;
    va_start(args, format);
    vswprintf_s(buffer, _countof(buffer), format, args);
    va_end(args);

    // Get current time
    SYSTEMTIME st;
    GetLocalTime(&st);

    WCHAR timeBuffer[64];
    swprintf_s(timeBuffer, L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::wstring logLine = timeBuffer;
    logLine += buffer;
    logLine += L"\n";

    // Output to debugger
    OutputDebugStringW(logLine.c_str());

    // Output to console if enabled
    if (g_consoleLogging) {
        wprintf(L"%s", logLine.c_str());
    }

    // Append to log file
    static std::wstring logPath = GetLogPath();
    std::wofstream logFile(logPath, std::ios::app);
    if (logFile.is_open()) {
        logFile << logLine;
    }
}

Settings LoadSettings() {
    Settings settings;
    std::wstring configPath = GetConfigPath();

    // Check if config file exists, if not create default
    if (!PathFileExistsW(configPath.c_str())) {
        SaveDefaultSettings();
    }

    WCHAR buffer[256];

    // calculateFolderSizes
    GetPrivateProfileStringW(L"Settings", L"calculateFolderSizes", L"disabled",
                             buffer, _countof(buffer), configPath.c_str());

    if (wcscmp(buffer, L"everything") == 0) {
        settings.calculateFolderSizes = CalculateFolderSizes::everything;
    } else if (wcscmp(buffer, L"withShiftKey") == 0) {
        settings.calculateFolderSizes = CalculateFolderSizes::withShiftKey;
    } else if (wcscmp(buffer, L"always") == 0) {
        settings.calculateFolderSizes = CalculateFolderSizes::always;
    } else {
        settings.calculateFolderSizes = CalculateFolderSizes::disabled;
    }

    // sortSizesMixFolders
    settings.sortSizesMixFolders = GetPrivateProfileIntW(
        L"Settings", L"sortSizesMixFolders", 1, configPath.c_str()) != 0;

    // disableKbOnlySizes
    settings.disableKbOnlySizes = GetPrivateProfileIntW(
        L"Settings", L"disableKbOnlySizes", 1, configPath.c_str()) != 0;

    // useIecTerms
    settings.useIecTerms = GetPrivateProfileIntW(
        L"Settings", L"useIecTerms", 0, configPath.c_str()) != 0;

    LogMessage(L"Settings loaded from: %s", configPath.c_str());
    LogMessage(L"  calculateFolderSizes: %s", buffer);
    LogMessage(L"  sortSizesMixFolders: %d", settings.sortSizesMixFolders);
    LogMessage(L"  disableKbOnlySizes: %d", settings.disableKbOnlySizes);
    LogMessage(L"  useIecTerms: %d", settings.useIecTerms);

    return settings;
}

void SaveDefaultSettings() {
    std::wstring configPath = GetConfigPath();

    // Create config file with default values and comments
    std::wofstream file(configPath);
    if (file.is_open()) {
        file << L"; Better File Sizes Configuration\n";
        file << L"; Place this file next to BetterFileSizesHook.dll\n";
        file << L"\n";
        file << L"[Settings]\n";
        file << L"\n";
        file << L"; Show folder sizes\n";
        file << L"; Options: disabled, everything, withShiftKey, always\n";
        file << L";   disabled    - Don't show folder sizes\n";
        file << L";   everything  - Use Everything search for folder sizes (recommended, requires Everything to be running)\n";
        file << L";   withShiftKey - Calculate manually only when Shift is held\n";
        file << L";   always      - Always calculate manually (can be slow)\n";
        file << L"calculateFolderSizes=disabled\n";
        file << L"\n";
        file << L"; Mix files and folders when sorting by size\n";
        file << L"; 1 = enabled, 0 = disabled\n";
        file << L"sortSizesMixFolders=1\n";
        file << L"\n";
        file << L"; Use MB/GB for large files instead of KB only\n";
        file << L"; 1 = enabled, 0 = disabled\n";
        file << L"disableKbOnlySizes=1\n";
        file << L"\n";
        file << L"; Use IEC terms (KiB, MiB, GiB instead of KB, MB, GB)\n";
        file << L"; 1 = enabled, 0 = disabled\n";
        file << L"useIecTerms=0\n";

        file.close();
        LogMessage(L"Created default config file: %s", configPath.c_str());
    }
}
