#include "crashguard.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>

namespace {

/* 防重入标志: 崩溃处理期间再次进入时直接放弃本次处理 */
volatile LONG g_crash_handling = 0;

/* 日志文件句柄: 安装时预打开 (追加模式, 共享读写删除), 崩溃时直接写 */
HANDLE g_log_handle = INVALID_HANDLE_VALUE;

/* exe 所在目录, 安装时缓存, 崩溃时用于生成日志与 dump 路径 */
wchar_t g_exe_dir[MAX_PATH] = { 0 };

/* 业务上下文回调 (由 core 等模块注册), 崩溃时同步调用一次 */
CrashContextFn g_context_provider = nullptr;

/* 文件大小是否为 0 (判断是否首次创建, 决定是否写入 UTF-8 BOM) */
bool IsFileEmpty(HANDLE h) {
    LARGE_INTEGER size = { {0, 0} };
    return GetFileSizeEx(h, &size) && size.QuadPart == 0;
}

/* 打开/创建 exe 目录下的 crash.log (追加模式), 首次创建时写入 UTF-8 BOM */
void OpenLogFile() {
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, L"%s\\crash.log", g_exe_dir) < 0) {
        return;
    }
    g_log_handle = CreateFileW(
        path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log_handle == INVALID_HANDLE_VALUE) {
        return;
    }
    if (IsFileEmpty(g_log_handle)) {
        static const char kBom[] = "\xEF\xBB\xBF";
        DWORD written = 0;
        WriteFile(g_log_handle, kBom, sizeof(kBom) - 1, &written, nullptr);
    }
}

/* 向日志文件追加写入一段文本 (不换行) */
void WriteText(const char* text, size_t len) {
    if (g_log_handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(g_log_handle, text, (DWORD)len, &written, nullptr);
}

void WriteLine(const char* text) {
    WriteText(text, strlen(text));
}

/* 把地址格式化为 "短模块名+0xRVA" (崩溃日志标准形态, 配合归档 PDB 事后还原);
   地址不属于任何已加载模块时退化为原始十六进制地址 */
void FormatAddress(void* addr, char* out, size_t out_len) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)addr, &mod)) {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(mod, path, MAX_PATH) > 0) {
            const wchar_t* slash = wcsrchr(path, L'\\');
            slash = slash != nullptr ? slash + 1 : path;
            char utf8[MAX_PATH * 2];
            if (WideCharToMultiByte(CP_UTF8, 0, slash, -1, utf8,
                                    sizeof(utf8), nullptr, nullptr) > 0) {
                sprintf_s(out, out_len, "%s+0x%zX", utf8,
                          (uintptr_t)addr - (uintptr_t)mod);
                return;
            }
        }
    }
    sprintf_s(out, out_len, "0x%p", addr);
}

/* 解析主模块 PE 头的 CodeView 调试目录, 取 PDB GUID 与 Age (写入 pdb_guid/age);
   同时给出镜像时间戳与大小。任何一步失败都返回 false */
bool GetBuildFingerprint(char* pdb_guid, size_t guid_len, DWORD* age,
                         DWORD* image_ts, DWORD* image_size) {
    *pdb_guid = '\0';
    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe == nullptr) {
        return false;
    }
    const auto* dos = (const IMAGE_DOS_HEADER*)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nth = (const IMAGE_NT_HEADERS*)((const char*)exe + dos->e_lfanew);
    if (nth->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    *image_ts = nth->FileHeader.TimeDateStamp;
    *image_size = nth->OptionalHeader.SizeOfImage;

    const IMAGE_DATA_DIRECTORY& dd =
        nth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const auto* dbg =
        (const IMAGE_DEBUG_DIRECTORY*)((const char*)exe + dd.VirtualAddress);
    const DWORD count = dd.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (DWORD i = 0; i < count; ++i) {
        if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
            dbg[i].AddressOfRawData == 0 || dbg[i].SizeOfData < 24) {
            continue;
        }
        const char* cv = (const char*)exe + dbg[i].AddressOfRawData;
        if (memcmp(cv, "RSDS", 4) != 0) {
            continue;
        }
        const GUID* g = (const GUID*)(cv + 4);
        sprintf_s(pdb_guid, guid_len,
                  "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX"
                  "%02hhX%02hhX",
                  g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
                  g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5],
                  g->Data4[6], g->Data4[7]);
        *age = *(const DWORD*)(cv + 20);
        return true;
    }
    return false;
}

/* 把当前线程调用栈写入日志 (每帧 短模块名+0xRVA), 供 L1/L3 共用。
   注意: 在异常过滤器/terminate 处理器内调用时, 首帧是处理器自身而非崩溃点,
   精确帧序以随附 minidump 为准 */
void WriteCurrentStack() {
    void* frames[16];
    USHORT count = CaptureStackBackTrace(0, 16, frames, nullptr);
    char line[512];
    int n = sprintf_s(line, sizeof(line), "stack(%u):", count);
    WriteText(line, (size_t)n);
    for (USHORT i = 0; i < count; ++i) {
        char where[MAX_PATH * 2];
        FormatAddress(frames[i], where, sizeof(where));
        n = sprintf_s(line, sizeof(line), " %s", where);
        WriteText(line, (size_t)n);
    }
    WriteLine("\n");
}

/* 写入构建指纹行 (PDB GUID/Age + 镜像时间戳/大小), 供事后配对归档 PDB */
void WriteBuildFingerprint() {
    char pdb_guid[48] = "";
    DWORD age = 0, image_ts = 0, image_size = 0;
    const bool ok = GetBuildFingerprint(pdb_guid, sizeof(pdb_guid), &age,
                                        &image_ts, &image_size);
    char line[256];
    int n = sprintf_s(
        line, sizeof(line), "build pdb-guid=%s age=%lu image-ts=0x%08lX size=0x%08X\n",
        ok ? pdb_guid : "<none>", age, image_ts, image_size);
    WriteText(line, (size_t)n);
}

/* 调用已注册的业务上下文回调, 以 "context=" 行写入日志。
   未注册或回调未产出内容时不写该行。回调自身承诺不抛异常/不分配/不加锁 */
void WriteCrashContext() {
    if (g_context_provider == nullptr) {
        return;
    }
    char buf[512] = "";
    g_context_provider(buf, sizeof(buf));
    if (buf[0] == '\0') {
        return;
    }
    WriteLine("context=");
    WriteLine(buf);
    WriteLine("\n");
}

/* 崩溃处理: 返回 true 表示已完整记录并应退出进程 */
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    /* 防重入: 处理过程中再抛异常 (例如内存已损坏) 时放弃, 交给系统默认处理 */
    if (InterlockedExchange(&g_crash_handling, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (g_log_handle == INVALID_HANDLE_VALUE) {
        /* 崩溃时才打开日志文件, 正常运行时 crash.log 不应存在 */
        OpenLogFile();
    }
    if (g_log_handle == INVALID_HANDLE_VALUE) {
        /* 日志都开不出来, 没有任何可做的事, 直接退出 */
        ExitProcess(0xAC000001);
    }

    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const DWORD code = er->ExceptionCode;
    const void* addr = er->ExceptionAddress;

    if (code == EXCEPTION_STACK_OVERFLOW) {
        /* 栈已耗尽: 只做最小操作 (WriteFile 常量串), 不做深栈调用, 不写 dump */
        WriteLine("===== STACK_OVERFLOW =====\n");
        FlushFileBuffers(g_log_handle);
        CloseHandle(g_log_handle);
        ExitProcess(0xAC000001);
    }

    {
        char line[512];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = sprintf_s(
            line, sizeof(line),
            "===== CRASH %04u-%02u-%02u %02u:%02u:%02u.%03u PID=%lu TID=%lu =====\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());
        WriteText(line, (size_t)n);
    }
    {
        char line[512];
        int n = sprintf_s(line, sizeof(line), "code=0x%08lX addr=0x%p\n", code, addr);
        WriteText(line, (size_t)n);
    }
    {
        /* 异常地址的 模块+RVA 形态, 与下方栈帧同构 */
        char where[MAX_PATH * 2];
        FormatAddress((void*)addr, where, sizeof(where));
        char line[sizeof(where) + 8];
        int n = sprintf_s(line, sizeof(line), "at=%s\n", where);
        WriteText(line, (size_t)n);
    }
    {
        /* 异常地址所属模块名 (UTF-8), 用于区分崩溃在 exe 内部还是系统 DLL */
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)addr, &mod)) {
            wchar_t mod_path[MAX_PATH];
            if (GetModuleFileNameW(mod, mod_path, MAX_PATH) > 0) {
                char utf8[MAX_PATH * 2];
                int n = WideCharToMultiByte(CP_UTF8, 0, mod_path, -1, utf8,
                                            sizeof(utf8), nullptr, nullptr);
                if (n > 0) {
                    char line[sizeof(utf8) + 16];
                    int m = sprintf_s(line, sizeof(line), "module=%s\n", utf8);
                    WriteText(line, (size_t)m);
                }
            }
        }
    }
    WriteCrashContext();
    {
        /* 调用栈: 每帧格式化为 短模块名+0xRVA (首帧是本过滤器而非异常点,
           异常点见上方 at=/addr=) */
        WriteCurrentStack();
    }
    WriteBuildFingerprint();
    {
        /* minidump: 延迟加载 dbghelp.dll, 避免启动期硬依赖 */
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        if (dbghelp != nullptr) {
            typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(
                HANDLE, DWORD, HANDLE, DWORD,
                PMINIDUMP_EXCEPTION_INFORMATION,
                PMINIDUMP_USER_STREAM_INFORMATION,
                PMINIDUMP_CALLBACK_INFORMATION);
            MiniDumpWriteDumpFn dump_fn =
                (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump");
            if (dump_fn != nullptr) {
                wchar_t dmp[MAX_PATH];
                SYSTEMTIME st;
                GetLocalTime(&st);
                if (swprintf_s(dmp, L"%s\\crash-%04u%02u%02u-%02u%02u%02u.dmp",
                               g_exe_dir, st.wYear, st.wMonth, st.wDay,
                               st.wHour, st.wMinute, st.wSecond) >= 0) {
                    HANDLE h = CreateFileW(
                        dmp, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (h != INVALID_HANDLE_VALUE) {
                        MINIDUMP_EXCEPTION_INFORMATION info = {
                            GetCurrentThreadId(), ep, FALSE };
                        dump_fn(GetCurrentProcess(), GetCurrentProcessId(), h,
                                MiniDumpNormal | MiniDumpWithThreadInfo |
                                    MiniDumpWithProcessThreadData,
                                &info, nullptr, nullptr);
                        CloseHandle(h);
                    }
                }
            }
        }
    }

    FlushFileBuffers(g_log_handle);
    CloseHandle(g_log_handle);
    ExitProcess(0xAC000001);

    /* 实际不可达, 仅满足函数签名 */
    return EXCEPTION_EXECUTE_HANDLER;
}

/* L3 终止处理: 未捕获的 C++ 异常从任意线程逃逸时由 std::terminate 调用。
   该路径走 abort->fastfail, 完全绕过用户态异常分发, SEH 过滤器收不到,
   必须单独挂钩。计划文档 docs/CRASH_GUARD_PLAN.md §1 */
[[noreturn]] void TerminateFilter() {
    if (InterlockedExchange(&g_crash_handling, 1) != 0) {
        /* 已在其它层处理中: 不叠加日志, 直接退出 */
        ExitProcess(0xAC000002);
    }

    if (g_log_handle == INVALID_HANDLE_VALUE) {
        OpenLogFile();
    }
    if (g_log_handle == INVALID_HANDLE_VALUE) {
        ExitProcess(0xAC000002);
    }

    {
        char line[512];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = sprintf_s(
            line, sizeof(line),
            "===== TERMINATE %04u-%02u-%02u %02u:%02u:%02u.%03u PID=%lu TID=%lu =====\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());
        WriteText(line, (size_t)n);
    }
    {
        /* 取在逃异常描述: current_exception/rethrow 在 terminate 处理器内是
           MSVC 支持的事实做法; 若再次失控会被入口防重入拦住直接退出。
           有微小堆分配, 属于对"零分配铁律"的受控偏离 (此时堆通常完好) */
        char what[256] = "<unknown>";
        if (auto ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                strncpy_s(what, sizeof(what), e.what(), _TRUNCATE);
            } catch (...) {
                /* 非 std::exception 派生: 保持 unknown */
            }
        }
        char line[sizeof(what) + 16];
        int n = sprintf_s(line, sizeof(line), "exception=%s\n", what);
        WriteText(line, (size_t)n);
    }
    WriteCrashContext();
    WriteCurrentStack();
    WriteBuildFingerprint();

    FlushFileBuffers(g_log_handle);
    CloseHandle(g_log_handle);
    ExitProcess(0xAC000002);
}

} // namespace

void InstallCrashGuard() {
    /* 关闭 Windows 默认的崩溃弹窗, 避免崩溃后 UI 卡死等待用户 */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    /* 只缓存 exe 目录; crash.log 延迟到真正崩溃时再打开, 正常运行不产生日志文件 */
    if (GetModuleFileNameW(nullptr, g_exe_dir, MAX_PATH) > 0) {
        wchar_t* slash = wcsrchr(g_exe_dir, L'\\');
        if (slash != nullptr) {
            *slash = L'\0';
        }
    }

    SetUnhandledExceptionFilter(CrashFilter);

    /* L3: 未捕获 C++ 异常兜底 (进程级, 自动覆盖所有线程) */
    std::set_terminate(TerminateFilter);
}

void SetCrashContextProvider(CrashContextFn fn) {
    g_context_provider = fn;
}
