#include "CrashReport.h"

#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <psapi.h>

#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace amber
{
namespace
{

// Everything the handler needs is resolved at install time and held here, so
// the handler itself allocates nothing: at crash time the heap is one of the
// things that might be broken.
wchar_t g_dir[MAX_PATH] = L"";
bool    g_ready = false;

// A tiny fixed-size formatter. snprintf into a stack buffer is safe here;
// std::string is not, because it allocates.
struct Out
{
    HANDLE h = INVALID_HANDLE_VALUE;
    void Line(const char* fmt, ...)
    {
        if (h == INVALID_HANDLE_VALUE)
            return;
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
        va_end(ap);
        if (n <= 0)
            return;
        int len = n < static_cast<int>(sizeof buf) - 2 ? n : static_cast<int>(sizeof buf) - 2;
        buf[len++] = '\r';
        buf[len++] = '\n';
        DWORD wrote = 0;
        WriteFile(h, buf, static_cast<DWORD>(len), &wrote, nullptr);
    }
};

const char* ExceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
    case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR:         return "in-page error";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case 0xE06D7363:                      return "unhandled C++ exception";
    default:                              return "exception";
    }
}

void ModuleAndOffset(const void* addr, char* out, size_t cap)
{
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(addr), &mod);
    wchar_t path[MAX_PATH] = L"?";
    if (mod)
        GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    char narrow[128] = "?";
    WideCharToMultiByte(CP_UTF8, 0, leaf, -1, narrow, sizeof narrow, nullptr, nullptr);
    const unsigned long long off =
        reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
    snprintf(out, cap, "%s+0x%llx", narrow, off);
}

LONG WINAPI Handler(EXCEPTION_POINTERS* ep)
{
    if (!g_ready || !ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH * 2];
    swprintf(path, MAX_PATH * 2, L"%s\\crash-%04u%02u%02u-%02u%02u%02u-%lu.txt",
             g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             GetCurrentProcessId());

    Out o;
    o.h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o.h == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    char where[192];
    ModuleAndOffset(r->ExceptionAddress, where, sizeof where);

    o.Line("AmberSSH crash report");
    o.Line("This file records WHERE the process died, never WHAT it was holding:");
    o.Line("no memory contents, no registers, no session data. Offsets resolve");
    o.Line("against the matching AmberSSH.pdb or the linker map.");
    o.Line("");
    o.Line("time      %04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute, st.wSecond);
    o.Line("process   %lu", GetCurrentProcessId());
    o.Line("thread    %lu", GetCurrentThreadId());
    o.Line("exception 0x%08lx  %s", r->ExceptionCode, ExceptionName(r->ExceptionCode));
    o.Line("at        %s", where);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
    {
        const char* how = r->ExceptionInformation[0] == 0   ? "read"
                          : r->ExceptionInformation[0] == 1 ? "write"
                                                            : "execute";
        o.Line("%-9s 0x%llx", how,
               static_cast<unsigned long long>(r->ExceptionInformation[1]));
    }

    // The stack, as module+offset. Symbols are not loaded and nothing is
    // resolved to a name: that is done afterwards, off the crashing process.
    if (ep->ContextRecord)
    {
        o.Line("");
        o.Line("stack (innermost first)");
        CONTEXT ctx = *ep->ContextRecord;
        STACKFRAME64 sf = {};
        sf.AddrPC.Offset = ctx.Rip;      sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp;   sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp;   sf.AddrStack.Mode = AddrModeFlat;
        const HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
        SymInitialize(proc, nullptr, TRUE);
        for (int depth = 0; depth < 40; ++depth)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx,
                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (sf.AddrPC.Offset == 0)
                break;
            char frame[192];
            ModuleAndOffset(reinterpret_cast<const void*>(sf.AddrPC.Offset), frame, sizeof frame);
            o.Line("  [%02d] %s", depth, frame);
        }
    }

    // Load addresses, so an offset above can be turned back into a symbol on
    // a machine that has the build's .pdb.
    o.Line("");
    o.Line("modules");
    HMODULE mods[256];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed))
    {
        const DWORD count = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < count && i < 256; ++i)
        {
            wchar_t mp[MAX_PATH] = L"?";
            GetModuleFileNameW(mods[i], mp, MAX_PATH);
            const wchar_t* leaf = wcsrchr(mp, L'\\');
            char narrow[128] = "?";
            WideCharToMultiByte(CP_UTF8, 0, leaf ? leaf + 1 : mp, -1, narrow, sizeof narrow,
                                nullptr, nullptr);
            o.Line("  0x%016llx  %s",
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mods[i])), narrow);
        }
    }

    CloseHandle(o.h);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

std::wstring CrashReportDirectory()
{
    wchar_t* base = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
    {
        out = std::wstring(base) + L"\\AmberSSH\\crashes";
        CoTaskMemFree(base);
    }
    return out;
}

void InstallCrashHandler()
{
    const std::wstring dir = CrashReportDirectory();
    if (dir.empty() || dir.size() >= MAX_PATH)
        return;
    // Created now, not at crash time: SHCreateDirectoryEx allocates and takes
    // locks, and the handler must do neither.
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;
    wcscpy_s(g_dir, MAX_PATH, dir.c_str());
    g_ready = true;
    SetUnhandledExceptionFilter(Handler);
}

} // namespace amber
