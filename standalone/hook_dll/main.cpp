// Better File Sizes - Standalone Hook DLL
// Converted from Windhawk module by m417z
// Original: https://github.com/m417z/my-windhawk-mods

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>
#include <comutil.h>
#include <shlwapi.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "MinHook.h"
#include "config.h"
#include "symbol_resolver.h"

using namespace std::string_view_literals;

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

// Global settings
static Settings g_settings;
static bool g_isEverything = false;
static HMODULE g_propsysModule = nullptr;
static HMODULE g_thisModule = nullptr;
static std::atomic<int> g_hookRefCount{0};

// Thread-local state
thread_local bool g_inCRecursiveFolderOperation_Prepare = false;
thread_local bool g_inCRecursiveFolderOperation_Do = false;

// Hook reference count scope helper
auto hookRefCountScope() {
    g_hookRefCount++;
    return std::unique_ptr<decltype(g_hookRefCount),
                           void (*)(decltype(g_hookRefCount)*)>{
        &g_hookRefCount, [](auto hookRefCount) { (*hookRefCount)--; }};
}

// Forward declarations
void SetModuleHandle(HMODULE hModule);

//==============================================================================
// PROPERTYKEY for Size
//==============================================================================
static const PROPERTYKEY kPKEY_Size = { { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 12 };

//==============================================================================
// Everything SDK (partial implementation)
//==============================================================================
#pragma region everything_sdk

#define EVERYTHING3_OK                          0
#define EVERYTHING3_ERROR_OUT_OF_MEMORY         0xE0000001
#define EVERYTHING3_ERROR_IPC_PIPE_NOT_FOUND    0xE0000002

#define _EVERYTHING3_COMMAND_GET_FOLDER_SIZE    18
#define _EVERYTHING3_RESPONSE_OK                200
#define _EVERYTHING3_RESPONSE_ERROR_NOT_FOUND   404

typedef struct _everything3_message_s {
    DWORD code;
    DWORD size;
} _everything3_message_t;

typedef struct _everything3_client_s {
    CRITICAL_SECTION cs;
    HANDLE pipe_handle;
    HANDLE send_event;
    HANDLE recv_event;
    HANDLE shutdown_event;
} _everything3_client_t;

static void _everything3_Lock(_everything3_client_t* client) {
    EnterCriticalSection(&client->cs);
}

static void _everything3_Unlock(_everything3_client_t* client) {
    LeaveCriticalSection(&client->cs);
}

static BOOL _everything3_send(_everything3_client_t* client, DWORD code,
                              const void* in_data, SIZE_T in_size) {
    OVERLAPPED ol = {};
    ol.hEvent = client->send_event;

    _everything3_message_t msg;
    msg.code = code;
    msg.size = (DWORD)in_size;

    HANDLE events[2] = { client->shutdown_event, client->send_event };

    // Send header
    DWORD written = 0;
    ResetEvent(client->send_event);
    if (!WriteFile(client->pipe_handle, &msg, sizeof(msg), &written, &ol)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, 10000);
            if (waitResult != WAIT_OBJECT_0 + 1) return FALSE;
            GetOverlappedResult(client->pipe_handle, &ol, &written, FALSE);
        } else {
            return FALSE;
        }
    }

    // Send data
    if (in_size > 0 && in_data) {
        ResetEvent(client->send_event);
        if (!WriteFile(client->pipe_handle, in_data, (DWORD)in_size, &written, &ol)) {
            if (GetLastError() == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, 10000);
                if (waitResult != WAIT_OBJECT_0 + 1) return FALSE;
                GetOverlappedResult(client->pipe_handle, &ol, &written, FALSE);
            } else {
                return FALSE;
            }
        }
    }

    return TRUE;
}

static BOOL _everything3_recv_header(_everything3_client_t* client,
                                     _everything3_message_t* recv_header) {
    OVERLAPPED ol = {};
    ol.hEvent = client->recv_event;

    HANDLE events[2] = { client->shutdown_event, client->recv_event };

    DWORD read = 0;
    ResetEvent(client->recv_event);
    if (!ReadFile(client->pipe_handle, recv_header, sizeof(*recv_header), &read, &ol)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, 10000);
            if (waitResult != WAIT_OBJECT_0 + 1) return FALSE;
            GetOverlappedResult(client->pipe_handle, &ol, &read, FALSE);
        } else {
            return FALSE;
        }
    }

    return read == sizeof(*recv_header);
}

static BOOL _everything3_recv_data(_everything3_client_t* client, void* buf, SIZE_T buf_size) {
    OVERLAPPED ol = {};
    ol.hEvent = client->recv_event;

    HANDLE events[2] = { client->shutdown_event, client->recv_event };

    DWORD read = 0;
    ResetEvent(client->recv_event);
    if (!ReadFile(client->pipe_handle, buf, (DWORD)buf_size, &read, &ol)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, 10000);
            if (waitResult != WAIT_OBJECT_0 + 1) return FALSE;
            GetOverlappedResult(client->pipe_handle, &ol, &read, FALSE);
        } else {
            return FALSE;
        }
    }

    return read == buf_size;
}

static _everything3_client_t* Everything3_ConnectW(const WCHAR* instance_name) {
    std::wstring pipe_name = L"\\\\.\\PIPE\\Everything IPC";
    if (instance_name && *instance_name) {
        pipe_name += L" (";
        pipe_name += instance_name;
        pipe_name += L")";
    }

    HANDLE pipe_handle = CreateFileW(pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

    if (pipe_handle == INVALID_HANDLE_VALUE) {
        SetLastError(EVERYTHING3_ERROR_IPC_PIPE_NOT_FOUND);
        return nullptr;
    }

    auto client = new _everything3_client_t();
    InitializeCriticalSection(&client->cs);
    client->pipe_handle = pipe_handle;
    client->shutdown_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client->send_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    client->recv_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    return client;
}

static BOOL Everything3_ShutdownClient(_everything3_client_t* client) {
    if (client && client->shutdown_event) {
        SetEvent(client->shutdown_event);
    }
    return TRUE;
}

static BOOL Everything3_DestroyClient(_everything3_client_t* client) {
    if (!client) return FALSE;

    if (client->pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(client->pipe_handle);
    }
    if (client->shutdown_event) CloseHandle(client->shutdown_event);
    if (client->send_event) CloseHandle(client->send_event);
    if (client->recv_event) CloseHandle(client->recv_event);

    DeleteCriticalSection(&client->cs);
    delete client;
    return TRUE;
}

static UINT64 Everything3_GetFolderSizeFromFilenameW(_everything3_client_t* client,
                                                      const WCHAR* lpFilename) {
    if (!client || !lpFilename) return UINT64_MAX;

    _everything3_Lock(client);

    // Convert filename to UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, lpFilename, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Path(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, lpFilename, -1, &utf8Path[0], utf8Len, nullptr, nullptr);

    UINT64 result = UINT64_MAX;

    if (_everything3_send(client, _EVERYTHING3_COMMAND_GET_FOLDER_SIZE,
                          utf8Path.c_str(), utf8Path.size())) {
        _everything3_message_t response;
        if (_everything3_recv_header(client, &response)) {
            if (response.code == _EVERYTHING3_RESPONSE_OK && response.size == sizeof(UINT64)) {
                _everything3_recv_data(client, &result, sizeof(result));
            }
        }
    }

    _everything3_Unlock(client);
    return result;
}

#pragma endregion everything_sdk

//==============================================================================
// Everything Query Implementation
//==============================================================================

enum ES_QUERY_STATUS {
    ES_QUERY_OK,
    ES_QUERY_NO_EVERYTHING,
    ES_QUERY_NO_INDEX,
    ES_QUERY_ZERO_SIZE_REPARSE_POINT,
};

static const wchar_t* g_gsQueryStatus[] = {
    L"OK",
    L"NO_EVERYTHING",
    L"NO_INDEX",
    L"ZERO_SIZE_REPARSE_POINT",
};

static std::atomic<HANDLE> g_everything4Wh_Thread{nullptr};
static HWND g_everything4Wh_ReplyWindow = nullptr;
static _everything3_client_t* g_everything3_client = nullptr;
static std::mutex g_everything_mutex;

static DWORD WINAPI Everything4Wh_ThreadProc(LPVOID) {
    // Try to connect to Everything 1.5a first (SDK3)
    g_everything3_client = Everything3_ConnectW(L"1.5a");
    if (!g_everything3_client) {
        g_everything3_client = Everything3_ConnectW(nullptr);
    }

    if (g_everything3_client) {
        LogMessage(L"Connected to Everything via SDK3");
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_APP) break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_everything3_client) {
        Everything3_ShutdownClient(g_everything3_client);
        Everything3_DestroyClient(g_everything3_client);
        g_everything3_client = nullptr;
    }

    return 0;
}

static void EnsureEverythingThread() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        HANDLE thread = CreateThread(nullptr, 0, Everything4Wh_ThreadProc, nullptr, 0, nullptr);
        g_everything4Wh_Thread.store(thread);
        Sleep(100); // Give thread time to connect
    });
}

static unsigned Everything4Wh_GetFileSize(const wchar_t* path, int64_t* size) {
    std::lock_guard<std::mutex> lock(g_everything_mutex);

    EnsureEverythingThread();

    if (!g_everything3_client) {
        return ES_QUERY_NO_EVERYTHING;
    }

    UINT64 result = Everything3_GetFolderSizeFromFilenameW(g_everything3_client, path);

    if (result == UINT64_MAX) {
        return ES_QUERY_NO_INDEX;
    }

    if (result == 0) {
        // Check if it's a reparse point
        DWORD attrs = GetFileAttributesW(path);
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return ES_QUERY_ZERO_SIZE_REPARSE_POINT;
        }
    }

    *size = static_cast<int64_t>(result);
    return ES_QUERY_OK;
}

//==============================================================================
// Utility Functions
//==============================================================================

static bool IsUncPath(const wchar_t* path) {
    return path && path[0] == L'\\' && path[1] == L'\\';
}

static std::wstring ResolvePath(const wchar_t* path) {
    WCHAR resolved[MAX_PATH * 2];
    DWORD len = GetFinalPathNameByHandleW(
        CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr),
        resolved, _countof(resolved), FILE_NAME_NORMALIZED);

    if (len == 0 || len >= _countof(resolved)) {
        return {};
    }

    std::wstring result(resolved);

    // Remove \\?\ prefix
    if (result.starts_with(L"\\\\?\\")) {
        result = result.substr(4);
    }

    return result;
}

using PIDLVector = std::vector<BYTE>;

static PIDLVector PIDLToVector(LPCITEMIDLIST pidl) {
    if (!pidl) return {};
    UINT size = ILGetSize(pidl);
    return PIDLVector(reinterpret_cast<const BYTE*>(pidl),
                      reinterpret_cast<const BYTE*>(pidl) + size);
}

static PIDLVector GetVectorFromIShellFolder(IShellFolder2* shellFolder) {
    LPITEMIDLIST pidl = nullptr;
    if (SUCCEEDED(SHGetIDListFromObject(shellFolder, &pidl)) && pidl) {
        auto result = PIDLToVector(pidl);
        CoTaskMemFree(pidl);
        return result;
    }
    return {};
}

// Cache for folder sizes
static PIDLVector g_cacheShellFolder;
static DWORD g_cacheShellFolderLastUsedTickCount = 0;
static std::map<PIDLVector, std::optional<ULONGLONG>> g_cacheShellFolderSizes;

//==============================================================================
// Size Calculator (for manual calculation)
//==============================================================================

class SizeCalculator : public INamespaceWalkCB2 {
public:
    SizeCalculator() : m_refCount(1), m_totalSize(0) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_INamespaceWalkCB ||
            riid == IID_INamespaceWalkCB2) {
            *ppv = static_cast<INamespaceWalkCB2*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = --m_refCount;
        if (ref == 0) delete this;
        return ref;
    }

    // INamespaceWalkCB
    HRESULT STDMETHODCALLTYPE FoundItem(IShellFolder* psf, LPCITEMIDLIST pidl) override {
        VARIANT var;
        VariantInit(&var);

        IShellFolder2* psf2 = nullptr;
        if (SUCCEEDED(psf->QueryInterface(IID_IShellFolder2, (void**)&psf2))) {
            if (SUCCEEDED(psf2->GetDetailsEx(pidl, &kPKEY_Size, &var))) {
                if (var.vt == VT_UI8) {
                    m_totalSize += var.ullVal;
                }
                VariantClear(&var);
            }
            psf2->Release();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnterFolder(IShellFolder*, LPCITEMIDLIST) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LeaveFolder(IShellFolder*, LPCITEMIDLIST) override { return S_OK; }

    // INamespaceWalkCB2
    HRESULT STDMETHODCALLTYPE WalkComplete(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE InitializeProgressDialog(LPWSTR*, LPWSTR*) override { return E_NOTIMPL; }

    ULONGLONG GetTotalSize() const { return m_totalSize; }

private:
    ULONG m_refCount;
    ULONGLONG m_totalSize;
};

static std::optional<ULONGLONG> CalculateFolderSize(IShellFolder2* shellFolder) {
    INamespaceWalk* nsWalk = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_NamespaceWalker, nullptr, CLSCTX_INPROC,
                                  IID_INamespaceWalk, (void**)&nsWalk);
    if (FAILED(hr)) return std::nullopt;

    SizeCalculator* calc = new SizeCalculator();

    hr = nsWalk->Walk(shellFolder,
        NSWF_DONT_ACCUMULATE_RESULT | NSWF_DONT_TRAVERSE_LINKS, 255, calc);

    ULONGLONG size = calc->GetTotalSize();
    calc->Release();
    nsWalk->Release();

    if (FAILED(hr)) return std::nullopt;
    return size;
}

static std::wstring GetFolderPathFromIShellFolder(IShellFolder2* shellFolder) {
    LPITEMIDLIST pidl = nullptr;
    if (FAILED(SHGetIDListFromObject(shellFolder, &pidl)) || !pidl) {
        return {};
    }

    WCHAR path[MAX_PATH * 2];
    BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);

    if (!ok) return {};

    std::wstring result(path);

    // Remove \\?\ prefix if present
    if (result.starts_with(L"\\\\?\\")) {
        result = result.substr(4);
    }

    return result;
}

//==============================================================================
// Hook Functions
//==============================================================================

// CFSFolder::_GetSize
using CFSFolder__GetSize_t = HRESULT(WINAPI*)(void* pCFSFolder,
                                               const ITEMID_CHILD* itemidChild,
                                               const void* idFolder,
                                               PROPVARIANT* propVariant);
static CFSFolder__GetSize_t CFSFolder__GetSize_Original = nullptr;

static HRESULT WINAPI CFSFolder__GetSize_Hook(void* pCFSFolder,
                                               const ITEMID_CHILD* itemidChild,
                                               const void* idFolder,
                                               PROPVARIANT* propVariant) {
    auto hookScope = hookRefCountScope();

    HRESULT ret = CFSFolder__GetSize_Original(pCFSFolder, itemidChild, idFolder, propVariant);

    if (g_inCRecursiveFolderOperation_Prepare ||
        g_inCRecursiveFolderOperation_Do ||
        ret != S_OK || propVariant->vt != VT_EMPTY) {
        return ret;
    }

    switch (g_settings.calculateFolderSizes) {
        case CalculateFolderSizes::disabled:
            return ret;
        case CalculateFolderSizes::withShiftKey:
            if (GetAsyncKeyState(VK_SHIFT) >= 0) {
                return ret;
            }
            break;
        case CalculateFolderSizes::everything:
        case CalculateFolderSizes::always:
            break;
    }

    LogMessage(L"CFSFolder__GetSize_Hook called");

    IShellFolder2* shellFolder2 = nullptr;
    HRESULT hr = ((IUnknown*)pCFSFolder)->QueryInterface(IID_IShellFolder2, (void**)&shellFolder2);
    if (FAILED(hr) || !shellFolder2) {
        return S_OK;
    }

    auto shellFolder2Vector = GetVectorFromIShellFolder(shellFolder2);
    if (shellFolder2Vector.empty() ||
        shellFolder2Vector != g_cacheShellFolder ||
        GetTickCount() - g_cacheShellFolderLastUsedTickCount > 1000) {
        g_cacheShellFolderSizes.clear();
    }

    g_cacheShellFolder = std::move(shellFolder2Vector);

    auto [cacheIt, cacheMissing] = g_cacheShellFolderSizes.try_emplace(
        PIDLToVector(itemidChild), std::nullopt);

    if (cacheMissing) {
        IShellFolder2* childFolder = nullptr;
        hr = shellFolder2->BindToObject(itemidChild, nullptr,
                                         IID_IShellFolder2, (void**)&childFolder);

        if (SUCCEEDED(hr) && childFolder) {
            if (g_settings.calculateFolderSizes == CalculateFolderSizes::everything) {
                auto path = GetFolderPathFromIShellFolder(childFolder);
                if (!path.empty()) {
                    int64_t size;
                    unsigned result = Everything4Wh_GetFileSize(path.c_str(), &size);

                    if (result == ES_QUERY_ZERO_SIZE_REPARSE_POINT ||
                        (result == ES_QUERY_NO_INDEX && !IsUncPath(path.c_str()))) {
                        auto resolved = ResolvePath(path.c_str());
                        if (!resolved.empty() && resolved != path) {
                            result = Everything4Wh_GetFileSize(resolved.c_str(), &size);
                        }
                    }

                    if (result == ES_QUERY_OK) {
                        cacheIt->second = size;
                    } else {
                        LogMessage(L"Failed to get size: %s", g_gsQueryStatus[result]);
                    }
                }
            } else {
                cacheIt->second = CalculateFolderSize(childFolder);
            }
            childFolder->Release();
        }
    }

    g_cacheShellFolderLastUsedTickCount = GetTickCount();
    shellFolder2->Release();

    if (cacheIt->second.has_value()) {
        propVariant->uhVal.QuadPart = *cacheIt->second;
        propVariant->vt = VT_UI8;
    }

    return S_OK;
}

// CRecursiveFolderOperation::Prepare
using CRecursiveFolderOperation_Prepare_t = HRESULT(__thiscall*)(void* pThis);
static CRecursiveFolderOperation_Prepare_t CRecursiveFolderOperation_Prepare_Original = nullptr;

static HRESULT __thiscall CRecursiveFolderOperation_Prepare_Hook(void* pThis) {
    auto hookScope = hookRefCountScope();
    g_inCRecursiveFolderOperation_Prepare = true;
    HRESULT ret = CRecursiveFolderOperation_Prepare_Original(pThis);
    g_inCRecursiveFolderOperation_Prepare = false;
    return ret;
}

// CRecursiveFolderOperation::Do
using CRecursiveFolderOperation_Do_t = HRESULT(__thiscall*)(void* pThis);
static CRecursiveFolderOperation_Do_t CRecursiveFolderOperation_Do_Original = nullptr;

static HRESULT __thiscall CRecursiveFolderOperation_Do_Hook(void* pThis) {
    auto hookScope = hookRefCountScope();
    g_inCRecursiveFolderOperation_Do = true;
    HRESULT ret = CRecursiveFolderOperation_Do_Original(pThis);
    g_inCRecursiveFolderOperation_Do = false;
    return ret;
}

// CFSFolder::MapColumnToSCID
using CFSFolder_MapColumnToSCID_t = HRESULT(WINAPI*)(void* pCFSFolder,
                                                      int column,
                                                      PROPERTYKEY* scid);
static CFSFolder_MapColumnToSCID_t CFSFolder_MapColumnToSCID_Original = nullptr;

// CFSFolder::GetDetailsEx
using CFSFolder_GetDetailsEx_t = HRESULT(WINAPI*)(void* pCFSFolder,
                                                   const ITEMID_CHILD* itemid,
                                                   const PROPERTYKEY* scid,
                                                   VARIANT* value);
static CFSFolder_GetDetailsEx_t CFSFolder_GetDetailsEx_Original = nullptr;

// CFSFolder::CompareIDs
using CFSFolder_CompareIDs_t = HRESULT(WINAPI*)(void* pCFSFolder,
                                                 int column,
                                                 const ITEMIDLIST_RELATIVE* itemid1,
                                                 const ITEMIDLIST_RELATIVE* itemid2);
static CFSFolder_CompareIDs_t CFSFolder_CompareIDs_Original = nullptr;

static HRESULT WINAPI CFSFolder_CompareIDs_Hook(void* pCFSFolder,
                                                 int column,
                                                 const ITEMIDLIST_RELATIVE* itemid1,
                                                 const ITEMIDLIST_RELATIVE* itemid2) {
    auto hookScope = hookRefCountScope();

    if (!itemid1 || !itemid2 || !g_settings.sortSizesMixFolders) {
        return CFSFolder_CompareIDs_Original(pCFSFolder, column, itemid1, itemid2);
    }

    PROPERTYKEY columnSCID;
    if (FAILED(CFSFolder_MapColumnToSCID_Original(pCFSFolder, column, &columnSCID)) ||
        !IsEqualPropertyKey(columnSCID, kPKEY_Size)) {
        return CFSFolder_CompareIDs_Original(pCFSFolder, column, itemid1, itemid2);
    }

    VARIANT value1 = {}, value2 = {};
    bool succeeded =
        SUCCEEDED(CFSFolder_GetDetailsEx_Original(pCFSFolder, itemid1, &columnSCID, &value1)) &&
        value1.vt == VT_UI8 &&
        SUCCEEDED(CFSFolder_GetDetailsEx_Original(pCFSFolder, itemid2, &columnSCID, &value2)) &&
        value2.vt == VT_UI8;

    if (!succeeded) {
        VariantClear(&value1);
        VariantClear(&value2);
        return CFSFolder_CompareIDs_Original(pCFSFolder, column, itemid1, itemid2);
    }

    ULONGLONG size1 = value1.ullVal;
    ULONGLONG size2 = value2.ullVal;

    VariantClear(&value1);
    VariantClear(&value2);

    if (size1 > size2) return 1;
    if (size1 < size2) return 0xFFFF;
    return 0;
}

// PSFormatForDisplayAlloc
using PSFormatForDisplayAlloc_t = HRESULT(WINAPI*)(const PROPERTYKEY& key,
                                                    const PROPVARIANT& propvar,
                                                    PROPDESC_FORMAT_FLAGS pdff,
                                                    PWSTR* ppszDisplay);
static PSFormatForDisplayAlloc_t PSFormatForDisplayAlloc_Original = nullptr;

static HRESULT WINAPI PSFormatForDisplayAlloc_Hook(const PROPERTYKEY& key,
                                                    const PROPVARIANT& propvar,
                                                    PROPDESC_FORMAT_FLAGS pdff,
                                                    PWSTR* ppszDisplay) {
    PROPDESC_FORMAT_FLAGS pdffNew = (PROPDESC_FORMAT_FLAGS)(pdff & ~PDFF_ALWAYSKB);
    if (pdffNew == pdff) {
        return PSFormatForDisplayAlloc_Original(key, propvar, pdff, ppszDisplay);
    }

    void* retAddress = _ReturnAddress();

    HMODULE explorerFrame = GetModuleHandleW(L"explorerframe.dll");
    if (!explorerFrame) {
        return PSFormatForDisplayAlloc_Original(key, propvar, pdff, ppszDisplay);
    }

    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (PCWSTR)retAddress, &module) ||
        module != explorerFrame) {
        return PSFormatForDisplayAlloc_Original(key, propvar, pdff, ppszDisplay);
    }

    return PSFormatForDisplayAlloc_Original(key, propvar, pdffNew, ppszDisplay);
}

// PSFormatForDisplay
using PSFormatForDisplay_t = HRESULT(WINAPI*)(const PROPERTYKEY& propkey,
                                               const PROPVARIANT& propvar,
                                               PROPDESC_FORMAT_FLAGS pdfFlags,
                                               LPWSTR pwszText,
                                               DWORD cchText);
static PSFormatForDisplay_t PSFormatForDisplay_Original = nullptr;

static HRESULT WINAPI PSFormatForDisplay_Hook(const PROPERTYKEY& propkey,
                                               const PROPVARIANT& propvar,
                                               PROPDESC_FORMAT_FLAGS pdfFlags,
                                               LPWSTR pwszText,
                                               DWORD cchText) {
    PROPDESC_FORMAT_FLAGS pdfFlagsNew = (PROPDESC_FORMAT_FLAGS)(pdfFlags & ~PDFF_ALWAYSKB);
    if (pdfFlagsNew == pdfFlags) {
        return PSFormatForDisplay_Original(propkey, propvar, pdfFlags, pwszText, cchText);
    }

    void* retAddress = _ReturnAddress();

    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32) {
        return PSFormatForDisplay_Original(propkey, propvar, pdfFlags, pwszText, cchText);
    }

    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (PCWSTR)retAddress, &module) ||
        module != shell32) {
        return PSFormatForDisplay_Original(propkey, propvar, pdfFlags, pwszText, cchText);
    }

    return PSFormatForDisplay_Original(propkey, propvar, pdfFlagsNew, pwszText, cchText);
}

// PSStrFormatKBSizeW (for IEC terms)
using PSStrFormatKBSizeW_t = void*(WINAPI*)(ULONGLONG size, LPWSTR pwszText, DWORD cchText);
static PSStrFormatKBSizeW_t PSStrFormatKBSizeW_Original = nullptr;

static void* WINAPI PSStrFormatKBSizeW_Hook(ULONGLONG size, LPWSTR pwszText, DWORD cchText) {
    void* ret = PSStrFormatKBSizeW_Original(size, pwszText, cchText);

    if (!pwszText || cchText == 0) return ret;

    int len = (int)wcslen(pwszText);

    // Replace KB with KiB, MB with MiB, etc.
    struct {
        const wchar_t* from;
        const wchar_t* to;
    } replacements[] = {
        { L" KB", L" KiB" },
        { L" MB", L" MiB" },
        { L" GB", L" GiB" },
        { L" TB", L" TiB" },
        { L" PB", L" PiB" },
    };

    for (const auto& r : replacements) {
        int fromLen = (int)wcslen(r.from);
        if (len >= fromLen && wcscmp(pwszText + len - fromLen, r.from) == 0) {
            int toLen = (int)wcslen(r.to);
            if ((DWORD)(len - fromLen + toLen) < cchText) {
                wcscpy_s(pwszText + len - fromLen, cchText - (len - fromLen), r.to);
            }
            break;
        }
    }

    return ret;
}

// LoadStringW (for IEC terms in string resources)
using LoadStringW_t = int(WINAPI*)(HINSTANCE hInstance, UINT uID, LPWSTR lpBuffer, int cchBufferMax);
static LoadStringW_t LoadStringW_Original = nullptr;

static int WINAPI LoadStringW_Hook(HINSTANCE hInstance, UINT uID, LPWSTR lpBuffer, int cchBufferMax) {
    int ret = LoadStringW_Original(hInstance, uID, lpBuffer, cchBufferMax);

    if (ret <= 0 || !lpBuffer || cchBufferMax <= 0) return ret;

    // Check if this is from propsys.dll
    if (hInstance != g_propsysModule) return ret;

    // Replace size unit strings
    struct {
        const wchar_t* from;
        const wchar_t* to;
    } replacements[] = {
        { L"KB", L"KiB" },
        { L"MB", L"MiB" },
        { L"GB", L"GiB" },
        { L"TB", L"TiB" },
        { L"PB", L"PiB" },
    };

    std::wstring str(lpBuffer, ret);

    for (const auto& r : replacements) {
        size_t pos = str.find(r.from);
        if (pos != std::wstring::npos) {
            str.replace(pos, wcslen(r.from), r.to);
            if ((int)str.length() < cchBufferMax) {
                wcscpy_s(lpBuffer, cchBufferMax, str.c_str());
                return (int)str.length();
            }
        }
    }

    return ret;
}

//==============================================================================
// Hook Installation
//==============================================================================

static bool HookWindowsStorageSymbols() {
    HMODULE windowsStorageModule = GetModuleHandleW(L"windows.storage.dll");
    if (!windowsStorageModule) {
        LogMessage(L"windows.storage.dll not loaded");
        return false;
    }

    if (!InitSymbolResolver()) {
        LogMessage(L"Failed to initialize symbol resolver");
        return false;
    }

    // Symbol names for x64
#ifdef _WIN64
    const char* getSizeSymbol =
        "private: long __cdecl CFSFolder::_GetSize(struct _ITEMID_CHILD const __unaligned *,struct tagFOLDERIDLIST const *,struct tagPROPVARIANT *)";
    const char* prepareSymbol =
        "public: long __cdecl CRecursiveFolderOperation::Prepare(void)";
    const char* doSymbol =
        "public: long __cdecl CRecursiveFolderOperation::Do(void)";
    const char* mapColumnSymbol =
        "public: virtual long __cdecl CFSFolder::MapColumnToSCID(unsigned int,struct _tagpropertykey *)";
    const char* getDetailsExSymbol =
        "public: virtual long __cdecl CFSFolder::GetDetailsEx(struct _ITEMID_CHILD const __unaligned *,struct _tagpropertykey const *,struct tagVARIANT *)";
    const char* compareIDsSymbol =
        "public: virtual long __cdecl CFSFolder::CompareIDs(__int64,struct _ITEMIDLIST_RELATIVE const __unaligned *,struct _ITEMIDLIST_RELATIVE const __unaligned *)";
#else
    const char* getSizeSymbol =
        "private: long __thiscall CFSFolder::_GetSize(struct _ITEMID_CHILD const *,struct tagFOLDERIDLIST const *,struct tagPROPVARIANT *)";
    const char* prepareSymbol =
        "public: long __thiscall CRecursiveFolderOperation::Prepare(void)";
    const char* doSymbol =
        "public: long __thiscall CRecursiveFolderOperation::Do(void)";
    const char* mapColumnSymbol =
        "public: virtual long __stdcall CFSFolder::MapColumnToSCID(unsigned int,struct _tagpropertykey *)";
    const char* getDetailsExSymbol =
        "public: virtual long __stdcall CFSFolder::GetDetailsEx(struct _ITEMID_CHILD const *,struct _tagpropertykey const *,struct tagVARIANT *)";
    const char* compareIDsSymbol =
        "public: virtual long __stdcall CFSFolder::CompareIDs(long,struct _ITEMIDLIST_RELATIVE const *,struct _ITEMIDLIST_RELATIVE const *)";
#endif

    void* pGetSize = FindFunctionBySymbol(windowsStorageModule, getSizeSymbol);
    void* pPrepare = FindFunctionBySymbol(windowsStorageModule, prepareSymbol);
    void* pDo = FindFunctionBySymbol(windowsStorageModule, doSymbol);
    void* pMapColumn = FindFunctionBySymbol(windowsStorageModule, mapColumnSymbol);
    void* pGetDetailsEx = FindFunctionBySymbol(windowsStorageModule, getDetailsExSymbol);
    void* pCompareIDs = FindFunctionBySymbol(windowsStorageModule, compareIDsSymbol);

    if (!pGetSize || !pPrepare || !pDo || !pMapColumn || !pGetDetailsEx || !pCompareIDs) {
        LogMessage(L"Failed to find some symbols");
        return false;
    }

    // Install hooks using MinHook
    if (MH_CreateHook(pGetSize, CFSFolder__GetSize_Hook, (LPVOID*)&CFSFolder__GetSize_Original) != MH_OK ||
        MH_CreateHook(pPrepare, CRecursiveFolderOperation_Prepare_Hook, (LPVOID*)&CRecursiveFolderOperation_Prepare_Original) != MH_OK ||
        MH_CreateHook(pDo, CRecursiveFolderOperation_Do_Hook, (LPVOID*)&CRecursiveFolderOperation_Do_Original) != MH_OK ||
        MH_CreateHook(pCompareIDs, CFSFolder_CompareIDs_Hook, (LPVOID*)&CFSFolder_CompareIDs_Original) != MH_OK) {
        LogMessage(L"Failed to create hooks");
        return false;
    }

    // Store original functions that we need but don't hook
    CFSFolder_MapColumnToSCID_Original = (CFSFolder_MapColumnToSCID_t)pMapColumn;
    CFSFolder_GetDetailsEx_Original = (CFSFolder_GetDetailsEx_t)pGetDetailsEx;

    if (MH_EnableHook(pGetSize) != MH_OK ||
        MH_EnableHook(pPrepare) != MH_OK ||
        MH_EnableHook(pDo) != MH_OK ||
        MH_EnableHook(pCompareIDs) != MH_OK) {
        LogMessage(L"Failed to enable hooks");
        return false;
    }

    LogMessage(L"Windows Storage hooks installed successfully");
    return true;
}

static bool InstallHooks() {
    LogMessage(L"Installing hooks...");

    if (MH_Initialize() != MH_OK) {
        LogMessage(L"MH_Initialize failed");
        return false;
    }

    // Load settings
    g_settings = LoadSettings();

    // Hook Windows Storage functions for folder size calculation
    if (g_settings.calculateFolderSizes != CalculateFolderSizes::disabled) {
        if (!HookWindowsStorageSymbols()) {
            LogMessage(L"Failed to hook Windows Storage symbols");
            return false;
        }
    }

    // Hook propsys functions for size formatting
    if (g_settings.disableKbOnlySizes) {
        HMODULE propsys = GetModuleHandleW(L"propsys.dll");
        if (propsys) {
            auto pPSFormatForDisplayAlloc = (PSFormatForDisplayAlloc_t)GetProcAddress(propsys, "PSFormatForDisplayAlloc");
            auto pPSFormatForDisplay = (PSFormatForDisplay_t)GetProcAddress(propsys, "PSFormatForDisplay");

            if (pPSFormatForDisplayAlloc) {
                MH_CreateHook(pPSFormatForDisplayAlloc, PSFormatForDisplayAlloc_Hook,
                              (LPVOID*)&PSFormatForDisplayAlloc_Original);
                MH_EnableHook(pPSFormatForDisplayAlloc);
            }

            if (pPSFormatForDisplay) {
                MH_CreateHook(pPSFormatForDisplay, PSFormatForDisplay_Hook,
                              (LPVOID*)&PSFormatForDisplay_Original);
                MH_EnableHook(pPSFormatForDisplay);
            }
        }
    }

    // Hook for IEC terms
    if (g_settings.useIecTerms) {
        HMODULE propsys = GetModuleHandleW(L"propsys.dll");
        if (propsys) {
            g_propsysModule = propsys;

            if (!g_settings.disableKbOnlySizes) {
                auto pPSStrFormatKBSizeW = (PSStrFormatKBSizeW_t)GetProcAddress(propsys, (LPCSTR)422);
                if (pPSStrFormatKBSizeW) {
                    MH_CreateHook(pPSStrFormatKBSizeW, PSStrFormatKBSizeW_Hook,
                                  (LPVOID*)&PSStrFormatKBSizeW_Original);
                    MH_EnableHook(pPSStrFormatKBSizeW);
                }
            }

            HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
            if (kernelBase) {
                auto pLoadStringW = (LoadStringW_t)GetProcAddress(kernelBase, "LoadStringW");
                if (pLoadStringW) {
                    MH_CreateHook(pLoadStringW, LoadStringW_Hook, (LPVOID*)&LoadStringW_Original);
                    MH_EnableHook(pLoadStringW);
                }
            }
        }
    }

    LogMessage(L"All hooks installed successfully");
    return true;
}

static void RemoveHooks() {
    LogMessage(L"Removing hooks...");

    // Wait for hook ref count to reach 0
    while (g_hookRefCount > 0) {
        Sleep(100);
    }

    // Shutdown Everything thread
    if (HANDLE thread = g_everything4Wh_Thread.exchange(nullptr)) {
        PostThreadMessage(GetThreadId(thread), WM_APP, 0, 0);
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    CleanupSymbolResolver();

    LogMessage(L"Hooks removed");
}

//==============================================================================
// DLL Entry Point
//==============================================================================

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            g_thisModule = hinstDLL;
            SetModuleHandle(hinstDLL);
            DisableThreadLibraryCalls(hinstDLL);

            LogMessage(L"BetterFileSizes DLL loaded");

            if (!InstallHooks()) {
                LogMessage(L"Failed to install hooks");
                return FALSE;
            }
            break;

        case DLL_PROCESS_DETACH:
            if (lpReserved == nullptr) { // Not process termination
                RemoveHooks();
            }
            break;
    }

    return TRUE;
}

// Export for injector to verify DLL is loaded
extern "C" __declspec(dllexport) BOOL IsLoaded() {
    return TRUE;
}
