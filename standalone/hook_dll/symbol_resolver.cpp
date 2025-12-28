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
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

static std::mutex g_symMutex;
static bool g_symInitialized = false;
static HANDLE g_hProcess = nullptr;

// Cache for resolved symbols
static std::unordered_map<std::wstring, std::unordered_map<std::wstring, void*>> g_symbolCache;
static std::unordered_map<std::wstring, bool> g_moduleSymbolsLoaded;

struct SymbolSearchContext {
    const char* targetName;
    void* foundAddress;
    DWORD64 moduleBase;
    bool found;
};

static BOOL CALLBACK EnumSymbolsCallback(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext) {
    auto* ctx = reinterpret_cast<SymbolSearchContext*>(UserContext);

    // Compare undecorated names
    if (strcmp(pSymInfo->Name, ctx->targetName) == 0) {
        ctx->foundAddress = reinterpret_cast<void*>(pSymInfo->Address);
        ctx->found = true;
        return FALSE; // Stop enumeration
    }

    return TRUE; // Continue enumeration
}

// Get symbol cache directory
static std::wstring GetSymbolCachePath() {
    WCHAR path[MAX_PATH];

    // Try to use standard Windows symbol cache location
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        PathAppendW(path, L"Temp\\SymbolCache");
        return path;
    }

    // Fallback to %TEMP%
    if (GetTempPathW(MAX_PATH, path)) {
        PathAppendW(path, L"SymbolCache");
        return path;
    }

    return L"C:\\SymbolCache";
}

// Recursively create directory
static void CreateDirectoryRecursive(const std::wstring& path) {
    size_t pos = 0;
    while ((pos = path.find(L'\\', pos + 1)) != std::wstring::npos) {
        CreateDirectoryW(path.substr(0, pos).c_str(), nullptr);
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

bool InitSymbolResolver() {
    std::lock_guard<std::mutex> lock(g_symMutex);

    if (g_symInitialized) {
        return true;
    }

    g_hProcess = GetCurrentProcess();

    // Set symbol options - include SYMOPT_DEBUG for verbose output
    DWORD symOptions = SymGetOptions();
    symOptions |= SYMOPT_UNDNAME;              // Undecorate names
    symOptions |= SYMOPT_DEFERRED_LOADS;       // Defer symbol loading
    symOptions |= SYMOPT_FAVOR_COMPRESSED;     // Prefer compressed PDB
    symOptions |= SYMOPT_ALLOW_ABSOLUTE_SYMBOLS;
    symOptions |= SYMOPT_AUTO_PUBLICS;
    symOptions |= SYMOPT_INCLUDE_32BIT_MODULES;
    symOptions &= ~SYMOPT_IGNORE_NT_SYMPATH;   // Use _NT_SYMBOL_PATH if set
    SymSetOptions(symOptions);

    // Build symbol path with Microsoft Symbol Server
    std::wstring symbolCachePath = GetSymbolCachePath();
    CreateDirectoryRecursive(symbolCachePath);

    // Symbol path format: cache*server
    // Use both HTTP and HTTPS, some corporate networks block one or the other
    std::wstring symbolPath = L"SRV*";
    symbolPath += symbolCachePath;
    symbolPath += L"*https://msdl.microsoft.com/download/symbols";

    LogMessage(L"Symbol cache path: %s", symbolCachePath.c_str());
    LogMessage(L"Symbol path: %s", symbolPath.c_str());

    // Initialize without any path first
    if (!SymInitializeW(g_hProcess, nullptr, FALSE)) {
        LogMessage(L"SymInitializeW failed: %d", GetLastError());
        return false;
    }

    // Then set the search path
    if (!SymSetSearchPathW(g_hProcess, symbolPath.c_str())) {
        LogMessage(L"SymSetSearchPathW failed: %d", GetLastError());
    }

    // Verify the path was set
    WCHAR verifyPath[2048];
    if (SymGetSearchPathW(g_hProcess, verifyPath, _countof(verifyPath))) {
        LogMessage(L"Verified symbol path: %s", verifyPath);
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
    g_moduleSymbolsLoaded.clear();
}

void* FindFunctionBySymbol(HMODULE hModule, const wchar_t* decoratedName) {
    int len = WideCharToMultiByte(CP_UTF8, 0, decoratedName, -1, nullptr, 0, nullptr, nullptr);
    std::string narrowName(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, decoratedName, -1, &narrowName[0], len, nullptr, nullptr);
    return FindFunctionBySymbol(hModule, narrowName.c_str());
}

// Load symbols for a module (with retry logic for network download)
static bool LoadModuleSymbols(HMODULE hModule, const WCHAR* modulePath, DWORD64& outBase) {
    // Check if already loaded
    std::wstring moduleKey(modulePath);
    if (g_moduleSymbolsLoaded.count(moduleKey) && g_moduleSymbolsLoaded[moduleKey]) {
        outBase = reinterpret_cast<DWORD64>(hModule);
        return true;
    }

    LogMessage(L"Loading symbols for: %s", modulePath);

    // Get module info
    MODULEINFO modInfo = {};
    if (!GetModuleInformation(g_hProcess, hModule, &modInfo, sizeof(modInfo))) {
        LogMessage(L"GetModuleInformation failed: %d", GetLastError());
        modInfo.SizeOfImage = 0;
    }

    // Unload first if already partially loaded
    SymUnloadModule64(g_hProcess, reinterpret_cast<DWORD64>(hModule));

    // Load module with explicit size
    DWORD64 moduleBase = SymLoadModuleExW(
        g_hProcess,
        nullptr,
        modulePath,
        nullptr,
        reinterpret_cast<DWORD64>(hModule),
        modInfo.SizeOfImage ? modInfo.SizeOfImage : 0x10000000, // Large default if unknown
        nullptr,
        0
    );

    if (moduleBase == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_SUCCESS) {
            moduleBase = reinterpret_cast<DWORD64>(hModule);
            LogMessage(L"Module already loaded at base: 0x%llX", moduleBase);
        } else {
            LogMessage(L"SymLoadModuleExW failed: %d", err);
            return false;
        }
    } else {
        LogMessage(L"Module loaded at base: 0x%llX", moduleBase);
    }

    // Check if symbols were actually loaded by getting module info
    IMAGEHLP_MODULEW64 moduleInfo = {};
    moduleInfo.SizeOfStruct = sizeof(moduleInfo);

    if (SymGetModuleInfoW64(g_hProcess, moduleBase, &moduleInfo)) {
        LogMessage(L"Module info:");
        LogMessage(L"  ImageName: %s", moduleInfo.ImageName);
        LogMessage(L"  LoadedImageName: %s", moduleInfo.LoadedImageName);
        LogMessage(L"  LoadedPdbName: %s", moduleInfo.LoadedPdbName);
        LogMessage(L"  SymType: %d", moduleInfo.SymType);
        LogMessage(L"  PdbSig70: {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
            moduleInfo.PdbSig70.Data1, moduleInfo.PdbSig70.Data2, moduleInfo.PdbSig70.Data3,
            moduleInfo.PdbSig70.Data4[0], moduleInfo.PdbSig70.Data4[1],
            moduleInfo.PdbSig70.Data4[2], moduleInfo.PdbSig70.Data4[3],
            moduleInfo.PdbSig70.Data4[4], moduleInfo.PdbSig70.Data4[5],
            moduleInfo.PdbSig70.Data4[6], moduleInfo.PdbSig70.Data4[7]);

        // SymType:
        // 0 = SymNone - No symbols
        // 1 = SymCoff
        // 2 = SymCv - CodeView
        // 3 = SymPdb - PDB
        // 4 = SymExport - Export symbols only
        // 5 = SymDeferred
        // 6 = SymSym
        // 7 = SymDia

        if (moduleInfo.SymType == 0 || moduleInfo.SymType == 4) {
            LogMessage(L"WARNING: No PDB symbols loaded! Only export symbols available.");
            LogMessage(L"Symbols may need to be downloaded from Microsoft Symbol Server.");
            LogMessage(L"This can take a few seconds on first run...");

            // Try to force symbol loading by searching for a known symbol
            // This can trigger the symbol server download
            SYMBOL_INFOW* pSymbol = (SYMBOL_INFOW*)malloc(sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR));
            if (pSymbol) {
                pSymbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
                pSymbol->MaxNameLen = MAX_SYM_NAME;

                // Try to look up any symbol to trigger download
                SymFromNameW(g_hProcess, L"DllMain", pSymbol);
                free(pSymbol);

                // Check again
                if (SymGetModuleInfoW64(g_hProcess, moduleBase, &moduleInfo)) {
                    LogMessage(L"After trigger - SymType: %d, PdbName: %s",
                        moduleInfo.SymType, moduleInfo.LoadedPdbName);
                }
            }
        }

        g_moduleSymbolsLoaded[moduleKey] = (moduleInfo.SymType >= 2 && moduleInfo.SymType != 4);
    } else {
        LogMessage(L"SymGetModuleInfoW64 failed: %d", GetLastError());
    }

    outBase = moduleBase;
    return true;
}

void* FindFunctionBySymbol(HMODULE hModule, const char* decoratedName) {
    std::lock_guard<std::mutex> lock(g_symMutex);

    if (!g_symInitialized) {
        LogMessage(L"Symbol resolver not initialized");
        return nullptr;
    }

    // Get module path
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
            LogMessage(L"Cache hit for: %S", decoratedName);
            return symbolIt->second;
        }
    }

    // Load module symbols
    DWORD64 moduleBase = 0;
    if (!LoadModuleSymbols(hModule, modulePath, moduleBase)) {
        return nullptr;
    }

    LogMessage(L"Searching for: %S", decoratedName);

    // Method 1: Try SymFromName directly
    char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(CHAR)];
    PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)symbolBuffer;
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    if (SymFromName(g_hProcess, decoratedName, pSymbol)) {
        void* addr = reinterpret_cast<void*>(pSymbol->Address);
        LogMessage(L"Found via SymFromName at: %p", addr);
        g_symbolCache[moduleKey][symbolKey] = addr;
        return addr;
    }

    LogMessage(L"SymFromName failed: %d, trying enumeration...", GetLastError());

    // Method 2: Enumerate symbols with pattern
    SymbolSearchContext ctx = { decoratedName, nullptr, moduleBase, false };

    // Extract class name for more targeted search
    std::string pattern = "*";
    const char* colonPos = strstr(decoratedName, "::");
    if (colonPos) {
        // Find the class name
        const char* classStart = decoratedName;
        // Skip past "public: " or "private: " etc
        if (strstr(decoratedName, "public: ") == decoratedName) classStart += 8;
        else if (strstr(decoratedName, "private: ") == decoratedName) classStart += 9;
        else if (strstr(decoratedName, "protected: ") == decoratedName) classStart += 11;

        // Skip return type and calling convention to find class name
        const char* classNameStart = nullptr;
        for (const char* p = classStart; p < colonPos; p++) {
            if (*p == ' ') classNameStart = p + 1;
        }

        if (classNameStart && classNameStart < colonPos) {
            std::string className(classNameStart, colonPos - classNameStart);
            pattern = className + "::*";
            LogMessage(L"Using pattern: %S", pattern.c_str());
        }
    }

    if (!SymEnumSymbols(g_hProcess, moduleBase, pattern.c_str(), EnumSymbolsCallback, &ctx)) {
        LogMessage(L"SymEnumSymbols failed: %d", GetLastError());
    }

    if (ctx.found && ctx.foundAddress) {
        LogMessage(L"Found via enumeration at: %p", ctx.foundAddress);
        g_symbolCache[moduleKey][symbolKey] = ctx.foundAddress;
        return ctx.foundAddress;
    }

    // Method 3: Try with wildcard enumeration
    ctx.found = false;
    ctx.foundAddress = nullptr;

    LogMessage(L"Trying full enumeration...");
    if (!SymEnumSymbols(g_hProcess, moduleBase, "*", EnumSymbolsCallback, &ctx)) {
        LogMessage(L"Full SymEnumSymbols failed: %d", GetLastError());
    }

    if (ctx.found && ctx.foundAddress) {
        LogMessage(L"Found via full enumeration at: %p", ctx.foundAddress);
        g_symbolCache[moduleKey][symbolKey] = ctx.foundAddress;
        return ctx.foundAddress;
    }

    LogMessage(L"Symbol not found: %S", decoratedName);
    return nullptr;
}
