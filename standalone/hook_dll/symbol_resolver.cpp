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

#pragma comment(lib, "psapi.lib")

// Dynamically loaded dbghelp module
static HMODULE g_dbghelpModule = nullptr;

// Function pointers for dbghelp
typedef DWORD (WINAPI *SymSetOptions_t)(DWORD);
typedef DWORD (WINAPI *SymGetOptions_t)();
typedef BOOL (WINAPI *SymInitializeW_t)(HANDLE, PCWSTR, BOOL);
typedef BOOL (WINAPI *SymCleanup_t)(HANDLE);
typedef BOOL (WINAPI *SymSetSearchPathW_t)(HANDLE, PCWSTR);
typedef BOOL (WINAPI *SymGetSearchPathW_t)(HANDLE, PWSTR, DWORD);
typedef DWORD64 (WINAPI *SymLoadModuleExW_t)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD, PMODLOAD_DATA, DWORD);
typedef BOOL (WINAPI *SymUnloadModule64_t)(HANDLE, DWORD64);
typedef BOOL (WINAPI *SymGetModuleInfoW64_t)(HANDLE, DWORD64, PIMAGEHLP_MODULEW64);
typedef BOOL (WINAPI *SymFromName_t)(HANDLE, PCSTR, PSYMBOL_INFO);
typedef BOOL (WINAPI *SymFromNameW_t)(HANDLE, PCWSTR, PSYMBOL_INFOW);
typedef BOOL (WINAPI *SymEnumSymbols_t)(HANDLE, ULONG64, PCSTR, PSYM_ENUMERATESYMBOLS_CALLBACK, PVOID);

static SymSetOptions_t pSymSetOptions = nullptr;
static SymGetOptions_t pSymGetOptions = nullptr;
static SymInitializeW_t pSymInitializeW = nullptr;
static SymCleanup_t pSymCleanup = nullptr;
static SymSetSearchPathW_t pSymSetSearchPathW = nullptr;
static SymGetSearchPathW_t pSymGetSearchPathW = nullptr;
static SymLoadModuleExW_t pSymLoadModuleExW = nullptr;
static SymUnloadModule64_t pSymUnloadModule64 = nullptr;
static SymGetModuleInfoW64_t pSymGetModuleInfoW64 = nullptr;
static SymFromName_t pSymFromName = nullptr;
static SymFromNameW_t pSymFromNameW = nullptr;
static SymEnumSymbols_t pSymEnumSymbols = nullptr;

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

    if (strcmp(pSymInfo->Name, ctx->targetName) == 0) {
        ctx->foundAddress = reinterpret_cast<void*>(pSymInfo->Address);
        ctx->found = true;
        return FALSE;
    }

    return TRUE;
}

// Get current module directory
static std::wstring GetModuleDirectory() {
    WCHAR path[MAX_PATH];
    HMODULE hModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&GetModuleDirectory, &hModule);
    GetModuleFileNameW(hModule, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

// Get symbol cache directory
static std::wstring GetSymbolCachePath() {
    WCHAR path[MAX_PATH];

    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        PathAppendW(path, L"Temp\\SymbolCache");
        return path;
    }

    if (GetTempPathW(MAX_PATH, path)) {
        PathAppendW(path, L"SymbolCache");
        return path;
    }

    return L"C:\\SymbolCache";
}

static void CreateDirectoryRecursive(const std::wstring& path) {
    size_t pos = 0;
    while ((pos = path.find(L'\\', pos + 1)) != std::wstring::npos) {
        CreateDirectoryW(path.substr(0, pos).c_str(), nullptr);
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

// Load dbghelp.dll from our directory (newer version with symbol server support)
static bool LoadDbgHelp() {
    if (g_dbghelpModule) return true;

    std::wstring moduleDir = GetModuleDirectory();

    // Try to load from our directory first (should have symsrv.dll too)
    std::wstring dbghelpPath = moduleDir + L"\\dbghelp.dll";

    LogMessage(L"Trying to load dbghelp from: %s", dbghelpPath.c_str());

    g_dbghelpModule = LoadLibraryW(dbghelpPath.c_str());

    if (!g_dbghelpModule) {
        LogMessage(L"Local dbghelp.dll not found, using system version");
        g_dbghelpModule = LoadLibraryW(L"dbghelp.dll");
    }

    if (!g_dbghelpModule) {
        LogMessage(L"Failed to load dbghelp.dll: %d", GetLastError());
        return false;
    }

    // Get function pointers
    pSymSetOptions = (SymSetOptions_t)GetProcAddress(g_dbghelpModule, "SymSetOptions");
    pSymGetOptions = (SymGetOptions_t)GetProcAddress(g_dbghelpModule, "SymGetOptions");
    pSymInitializeW = (SymInitializeW_t)GetProcAddress(g_dbghelpModule, "SymInitializeW");
    pSymCleanup = (SymCleanup_t)GetProcAddress(g_dbghelpModule, "SymCleanup");
    pSymSetSearchPathW = (SymSetSearchPathW_t)GetProcAddress(g_dbghelpModule, "SymSetSearchPathW");
    pSymGetSearchPathW = (SymGetSearchPathW_t)GetProcAddress(g_dbghelpModule, "SymGetSearchPathW");
    pSymLoadModuleExW = (SymLoadModuleExW_t)GetProcAddress(g_dbghelpModule, "SymLoadModuleExW");
    pSymUnloadModule64 = (SymUnloadModule64_t)GetProcAddress(g_dbghelpModule, "SymUnloadModule64");
    pSymGetModuleInfoW64 = (SymGetModuleInfoW64_t)GetProcAddress(g_dbghelpModule, "SymGetModuleInfoW64");
    pSymFromName = (SymFromName_t)GetProcAddress(g_dbghelpModule, "SymFromName");
    pSymFromNameW = (SymFromNameW_t)GetProcAddress(g_dbghelpModule, "SymFromNameW");
    pSymEnumSymbols = (SymEnumSymbols_t)GetProcAddress(g_dbghelpModule, "SymEnumSymbols");

    if (!pSymSetOptions || !pSymGetOptions || !pSymInitializeW || !pSymCleanup ||
        !pSymLoadModuleExW || !pSymFromName || !pSymEnumSymbols) {
        LogMessage(L"Failed to get dbghelp function pointers");
        FreeLibrary(g_dbghelpModule);
        g_dbghelpModule = nullptr;
        return false;
    }

    // Log dbghelp version info
    WCHAR dbghelpLoadedPath[MAX_PATH];
    GetModuleFileNameW(g_dbghelpModule, dbghelpLoadedPath, MAX_PATH);
    LogMessage(L"Loaded dbghelp.dll from: %s", dbghelpLoadedPath);

    return true;
}

bool InitSymbolResolver() {
    std::lock_guard<std::mutex> lock(g_symMutex);

    if (g_symInitialized) {
        return true;
    }

    if (!LoadDbgHelp()) {
        return false;
    }

    g_hProcess = GetCurrentProcess();

    // Set symbol options
    DWORD symOptions = pSymGetOptions();
    symOptions |= SYMOPT_UNDNAME;
    symOptions |= SYMOPT_DEFERRED_LOADS;
    symOptions |= SYMOPT_FAVOR_COMPRESSED;
    symOptions |= SYMOPT_ALLOW_ABSOLUTE_SYMBOLS;
    symOptions |= SYMOPT_AUTO_PUBLICS;
    symOptions |= SYMOPT_INCLUDE_32BIT_MODULES;
    symOptions &= ~SYMOPT_IGNORE_NT_SYMPATH;
    pSymSetOptions(symOptions);

    // Build symbol path
    std::wstring symbolCachePath = GetSymbolCachePath();
    CreateDirectoryRecursive(symbolCachePath);

    std::wstring symbolPath = L"SRV*";
    symbolPath += symbolCachePath;
    symbolPath += L"*https://msdl.microsoft.com/download/symbols";

    LogMessage(L"Symbol cache: %s", symbolCachePath.c_str());
    LogMessage(L"Symbol path: %s", symbolPath.c_str());

    // Initialize
    if (!pSymInitializeW(g_hProcess, nullptr, FALSE)) {
        LogMessage(L"SymInitializeW failed: %d", GetLastError());
        return false;
    }

    // Set search path
    if (pSymSetSearchPathW && !pSymSetSearchPathW(g_hProcess, symbolPath.c_str())) {
        LogMessage(L"SymSetSearchPathW failed: %d", GetLastError());
    }

    // Verify path
    if (pSymGetSearchPathW) {
        WCHAR verifyPath[2048];
        if (pSymGetSearchPathW(g_hProcess, verifyPath, _countof(verifyPath))) {
            LogMessage(L"Verified symbol path: %s", verifyPath);
        }
    }

    g_symInitialized = true;
    LogMessage(L"Symbol resolver initialized");
    return true;
}

void CleanupSymbolResolver() {
    std::lock_guard<std::mutex> lock(g_symMutex);

    if (g_symInitialized && g_hProcess && pSymCleanup) {
        pSymCleanup(g_hProcess);
        g_symInitialized = false;
        g_hProcess = nullptr;
    }

    g_symbolCache.clear();
    g_moduleSymbolsLoaded.clear();

    if (g_dbghelpModule) {
        FreeLibrary(g_dbghelpModule);
        g_dbghelpModule = nullptr;
    }
}

void* FindFunctionBySymbol(HMODULE hModule, const wchar_t* decoratedName) {
    int len = WideCharToMultiByte(CP_UTF8, 0, decoratedName, -1, nullptr, 0, nullptr, nullptr);
    std::string narrowName(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, decoratedName, -1, &narrowName[0], len, nullptr, nullptr);
    return FindFunctionBySymbol(hModule, narrowName.c_str());
}

static bool LoadModuleSymbols(HMODULE hModule, const WCHAR* modulePath, DWORD64& outBase) {
    std::wstring moduleKey(modulePath);
    if (g_moduleSymbolsLoaded.count(moduleKey) && g_moduleSymbolsLoaded[moduleKey]) {
        outBase = reinterpret_cast<DWORD64>(hModule);
        return true;
    }

    LogMessage(L"Loading symbols for: %s", modulePath);

    MODULEINFO modInfo = {};
    if (!GetModuleInformation(g_hProcess, hModule, &modInfo, sizeof(modInfo))) {
        LogMessage(L"GetModuleInformation failed: %d", GetLastError());
        modInfo.SizeOfImage = 0;
    }

    if (pSymUnloadModule64) {
        pSymUnloadModule64(g_hProcess, reinterpret_cast<DWORD64>(hModule));
    }

    DWORD64 moduleBase = pSymLoadModuleExW(
        g_hProcess,
        nullptr,
        modulePath,
        nullptr,
        reinterpret_cast<DWORD64>(hModule),
        modInfo.SizeOfImage ? modInfo.SizeOfImage : 0x10000000,
        nullptr,
        0
    );

    if (moduleBase == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_SUCCESS) {
            moduleBase = reinterpret_cast<DWORD64>(hModule);
            LogMessage(L"Module already loaded at: 0x%llX", moduleBase);
        } else {
            LogMessage(L"SymLoadModuleExW failed: %d", err);
            return false;
        }
    } else {
        LogMessage(L"Module loaded at: 0x%llX", moduleBase);
    }

    // Check symbol status
    if (pSymGetModuleInfoW64) {
        IMAGEHLP_MODULEW64 moduleInfo = {};
        moduleInfo.SizeOfStruct = sizeof(moduleInfo);

        if (pSymGetModuleInfoW64(g_hProcess, moduleBase, &moduleInfo)) {
            LogMessage(L"SymType: %d (%s)",
                moduleInfo.SymType,
                moduleInfo.SymType == 0 ? L"None" :
                moduleInfo.SymType == 3 ? L"PDB" :
                moduleInfo.SymType == 4 ? L"Export only" :
                moduleInfo.SymType == 5 ? L"Deferred" : L"Other");

            if (moduleInfo.LoadedPdbName[0]) {
                LogMessage(L"PDB: %s", moduleInfo.LoadedPdbName);
            }

            if (moduleInfo.SymType == 0 || moduleInfo.SymType == 4) {
                LogMessage(L"WARNING: No PDB symbols loaded!");
                LogMessage(L"Make sure dbghelp.dll and symsrv.dll are in the same folder.");
                LogMessage(L"Run setup_symbols.bat to copy the required files.");
            }

            g_moduleSymbolsLoaded[moduleKey] = (moduleInfo.SymType >= 2 && moduleInfo.SymType != 4);
        }
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

    DWORD64 moduleBase = 0;
    if (!LoadModuleSymbols(hModule, modulePath, moduleBase)) {
        return nullptr;
    }

    LogMessage(L"Searching: %S", decoratedName);

    // Try SymFromName
    char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(CHAR)];
    PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)symbolBuffer;
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    if (pSymFromName(g_hProcess, decoratedName, pSymbol)) {
        void* addr = reinterpret_cast<void*>(pSymbol->Address);
        LogMessage(L"Found at: %p", addr);
        g_symbolCache[moduleKey][symbolKey] = addr;
        return addr;
    }

    LogMessage(L"SymFromName failed: %d", GetLastError());

    // Try enumeration
    SymbolSearchContext ctx = { decoratedName, nullptr, moduleBase, false };

    if (pSymEnumSymbols(g_hProcess, moduleBase, "*", EnumSymbolsCallback, &ctx)) {
        if (ctx.found && ctx.foundAddress) {
            LogMessage(L"Found via enum at: %p", ctx.foundAddress);
            g_symbolCache[moduleKey][symbolKey] = ctx.foundAddress;
            return ctx.foundAddress;
        }
    } else {
        LogMessage(L"SymEnumSymbols failed: %d", GetLastError());
    }

    LogMessage(L"Symbol not found: %S", decoratedName);
    return nullptr;
}
