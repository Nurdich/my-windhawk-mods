#include "symbol_resolver.h"
#include "config.h"
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <mutex>
#include <unordered_map>
#include <string>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

static std::mutex g_symMutex;
static bool g_symInitialized = false;
static HANDLE g_hProcess = nullptr;

// Cache for resolved symbols
static std::unordered_map<std::wstring, std::unordered_map<std::wstring, void*>> g_symbolCache;

struct SymbolSearchContext {
    const char* targetName;
    void* foundAddress;
    DWORD64 moduleBase;
};

static BOOL CALLBACK EnumSymbolsCallback(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext) {
    auto* ctx = reinterpret_cast<SymbolSearchContext*>(UserContext);

    if (strcmp(pSymInfo->Name, ctx->targetName) == 0) {
        ctx->foundAddress = reinterpret_cast<void*>(pSymInfo->Address);
        return FALSE; // Stop enumeration
    }

    return TRUE; // Continue enumeration
}

// Get symbol cache directory
static std::wstring GetSymbolCachePath() {
    WCHAR path[MAX_PATH];

    // Try %TEMP%\SymbolCache first
    if (GetTempPathW(MAX_PATH, path)) {
        PathAppendW(path, L"SymbolCache");
        return path;
    }

    // Fallback to current directory
    GetCurrentDirectoryW(MAX_PATH, path);
    PathAppendW(path, L"SymbolCache");
    return path;
}

bool InitSymbolResolver() {
    std::lock_guard<std::mutex> lock(g_symMutex);

    if (g_symInitialized) {
        return true;
    }

    g_hProcess = GetCurrentProcess();

    // Set symbol options
    DWORD symOptions = SymGetOptions();
    symOptions |= SYMOPT_UNDNAME;           // Undecorate names
    symOptions |= SYMOPT_DEFERRED_LOADS;    // Defer symbol loading
    symOptions |= SYMOPT_DEBUG;             // Debug output
    symOptions |= SYMOPT_FAVOR_COMPRESSED;  // Prefer compressed PDB
    symOptions &= ~SYMOPT_NO_PROMPTS;       // Allow network access
    SymSetOptions(symOptions);

    // Build symbol path with Microsoft Symbol Server
    std::wstring symbolCachePath = GetSymbolCachePath();

    // Create cache directory if it doesn't exist
    CreateDirectoryW(symbolCachePath.c_str(), nullptr);

    // Symbol path format: srv*<local_cache>*https://msdl.microsoft.com/download/symbols
    std::wstring symbolPath = L"srv*";
    symbolPath += symbolCachePath;
    symbolPath += L"*https://msdl.microsoft.com/download/symbols";

    LogMessage(L"Symbol path: %s", symbolPath.c_str());

    // Convert to ANSI for SymInitialize
    int ansiLen = WideCharToMultiByte(CP_ACP, 0, symbolPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string ansiSymbolPath(ansiLen, '\0');
    WideCharToMultiByte(CP_ACP, 0, symbolPath.c_str(), -1, &ansiSymbolPath[0], ansiLen, nullptr, nullptr);

    if (!SymInitialize(g_hProcess, ansiSymbolPath.c_str(), FALSE)) {
        LogMessage(L"SymInitialize failed: %d", GetLastError());
        return false;
    }

    g_symInitialized = true;
    LogMessage(L"Symbol resolver initialized with Microsoft Symbol Server");
    return true;
}

void CleanupSymbolResolver() {
    std::lock_guard<std::mutex> lock(g_symMutex);

    if (g_symInitialized && g_hProcess) {
        SymCleanup(g_hProcess);
        g_symInitialized = false;
        g_hProcess = nullptr;
    }

    g_symbolCache.clear();
}

void* FindFunctionBySymbol(HMODULE hModule, const wchar_t* decoratedName) {
    // Convert to narrow string
    int len = WideCharToMultiByte(CP_UTF8, 0, decoratedName, -1, nullptr, 0, nullptr, nullptr);
    std::string narrowName(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, decoratedName, -1, &narrowName[0], len, nullptr, nullptr);

    return FindFunctionBySymbol(hModule, narrowName.c_str());
}

void* FindFunctionBySymbol(HMODULE hModule, const char* decoratedName) {
    std::lock_guard<std::mutex> lock(g_symMutex);

    if (!g_symInitialized) {
        LogMessage(L"Symbol resolver not initialized");
        return nullptr;
    }

    // Get module path for cache key
    WCHAR modulePath[MAX_PATH];
    GetModuleFileNameW(hModule, modulePath, MAX_PATH);

    std::wstring moduleKey(modulePath);
    int mbLen = MultiByteToWideChar(CP_UTF8, 0, decoratedName, -1, nullptr, 0);
    std::wstring symbolKey(mbLen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, decoratedName, -1, &symbolKey[0], mbLen);

    // Check cache
    auto moduleIt = g_symbolCache.find(moduleKey);
    if (moduleIt != g_symbolCache.end()) {
        auto symbolIt = moduleIt->second.find(symbolKey);
        if (symbolIt != moduleIt->second.end()) {
            return symbolIt->second;
        }
    }

    LogMessage(L"Loading symbols for: %s", modulePath);

    // Get module info for proper loading
    MODULEINFO modInfo = {};
    if (!GetModuleInformation(g_hProcess, hModule, &modInfo, sizeof(modInfo))) {
        LogMessage(L"GetModuleInformation failed: %d", GetLastError());
    }

    // Load module symbols
    DWORD64 moduleBase = SymLoadModuleExW(
        g_hProcess,
        nullptr,
        modulePath,
        nullptr,
        reinterpret_cast<DWORD64>(hModule),
        modInfo.SizeOfImage,
        nullptr,
        0
    );

    if (moduleBase == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_SUCCESS) {
            // Module already loaded
            moduleBase = reinterpret_cast<DWORD64>(hModule);
            LogMessage(L"Module already loaded at: %p", (void*)moduleBase);
        } else {
            LogMessage(L"SymLoadModuleExW failed: %d", err);
            return nullptr;
        }
    } else {
        LogMessage(L"Module loaded at: %p", (void*)moduleBase);
    }

    // Try to get symbol info directly using SymFromName
    char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)symbolBuffer;
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    LogMessage(L"Searching for symbol: %S", decoratedName);

    if (SymFromName(g_hProcess, decoratedName, pSymbol)) {
        void* addr = reinterpret_cast<void*>(pSymbol->Address);
        LogMessage(L"Found symbol at: %p", addr);
        g_symbolCache[moduleKey][symbolKey] = addr;
        return addr;
    }

    DWORD symError = GetLastError();
    LogMessage(L"SymFromName failed: %d", symError);

    // Fallback: enumerate all symbols (slower but more thorough)
    LogMessage(L"Trying symbol enumeration...");

    SymbolSearchContext ctx = { decoratedName, nullptr, moduleBase };

    if (!SymEnumSymbols(g_hProcess, moduleBase, "*", EnumSymbolsCallback, &ctx)) {
        LogMessage(L"SymEnumSymbols failed: %d", GetLastError());
    }

    if (ctx.foundAddress) {
        LogMessage(L"Found symbol via enumeration at: %p", ctx.foundAddress);
        g_symbolCache[moduleKey][symbolKey] = ctx.foundAddress;
        return ctx.foundAddress;
    }

    LogMessage(L"Symbol not found: %S", decoratedName);
    return nullptr;
}
