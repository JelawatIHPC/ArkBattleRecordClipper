#include "crashguard.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cwchar>

namespace {

/* 防重入标志: 崩溃处理期间再次进入时直接放弃本次处理 */
volatile LONG g_crash_handling = 0;

/* 日志文件句柄: 安装时预打开 (追加模式, 共享读写删除), 崩溃时直接写 */
HANDLE g_log_handle = INVALID_HANDLE_VALUE;

/* exe 所在目录, 安装时缓存, 崩溃时用于生成日志与 dump 路径 */
wchar_t g_exe_dir[MAX_PATH] = { 0 };

/* 文件大小是否为 0 (判断是否首次创建, 决定是否写入 UTF-8 BOM) */
bool IsFileEmpty(HANDLE h) {
    LARGE_INTEGER size = { 0 };
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
    {
        /* 栈地址 (仅原始地址, 不符号化: 零依赖, 配合 PDB 事后解析) */
        void* frames[16];
        USHORT count = CaptureStackBackTrace(0, 16, frames, nullptr);
        char line[512];
        int n = sprintf_s(line, sizeof(line), "stack(%u):", count);
        WriteText(line, (size_t)n);
        for (USHORT i = 0; i < count; ++i) {
            n = sprintf_s(line, sizeof(line), " 0x%p", frames[i]);
            WriteText(line, (size_t)n);
        }
        WriteLine("\n");
    }
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
}
