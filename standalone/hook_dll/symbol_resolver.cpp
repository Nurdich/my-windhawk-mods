#include "symbol_resolver.h"
#include "config.h"
#include <dbghelp.h>
#include <mutex>
#include <unordered_map>
#include <string>

#pragma comment(lib, "dbghelp.lib")

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

bool InitSymbolResolver() {
    std::lock_guard<std::mutex> lock(g_symMutex);

    if (g_symInitialized) {
        return true;
    }

    g_hProcess = GetCurrentProcess();

    // Initialize symbol handler
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_DEBUG);

    if (!SymInitialize(g_hProcess, nullptr, FALSE)) {
        LogMessage(L"SymInitialize failed: %d", GetLastError());
        return false;
    }

    g_symInitialized = true;
    LogMessage(L"Symbol resolver initialized");
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

    // Load module symbols
    DWORD64 moduleBase = SymLoadModuleExW(
        g_hProcess,
        nullptr,
        modulePath,
        nullptr,
        reinterpret_cast<DWORD64>(hModule),
        0,
        nullptr,
        0
    );

    if (moduleBase == 0 && GetLastError() != ERROR_SUCCESS) {
        // Module might already be loaded
        moduleBase = reinterpret_cast<DWORD64>(hModule);
    }

    LogMessage(L"Searching for symbol: %S in %s", decoratedName, modulePath);

    // Search for the symbol
    SymbolSearchContext ctx = { decoratedName, nullptr, moduleBase };

    if (!SymEnumSymbols(g_hProcess, moduleBase, "*", EnumSymbolsCallback, &ctx)) {
        LogMessage(L"SymEnumSymbols failed: %d", GetLastError());
    }

    if (ctx.foundAddress) {
        LogMessage(L"Found symbol at: %p", ctx.foundAddress);
        g_symbolCache[moduleKey][symbolKey] = ctx.foundAddress;
    } else {
        LogMessage(L"Symbol not found: %S", decoratedName);
    }

    return ctx.foundAddress;
}
