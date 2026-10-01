// mem_util.cpp — see mem_util.hpp. Native standby-list purge via NtSetSystemInformation,
// the same mechanism RAMMap / "system booster" tools use, wired to run automatically on model switch.
#include "pcore/runtime/mem_util.hpp"

#include <windows.h>
#include <cstdio>

namespace {

using Fn_NtSetSystemInformation = LONG (WINAPI*)(int, PVOID, ULONG);

// SYSTEM_MEMORY_LIST_INFORMATION class + commands (undocumented but stable).
constexpr int SystemMemoryListInformation = 80;
constexpr int MemoryPurgeStandbyList      = 4;

bool enable_priv(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    LUID luid;
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, name, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        ok = (GetLastError() == ERROR_SUCCESS);   // ERROR_NOT_ALL_ASSIGNED if not held
    }
    CloseHandle(tok);
    return ok;
}

} // namespace

namespace pcore { namespace runtime {

bool reclaim_system_cache() {
    // 1) Trim our own working set — always works, no privilege needed. Returns this process's
    //    trimmable resident pages; the just-unmapped context pages drop out of our set here.
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);

    // 2) Purge the system standby list — this is what actually reclaims the old model's unmapped
    //    pages so they don't crowd the next model. Needs elevation; no-ops otherwise.
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto NtSet = nt ? reinterpret_cast<Fn_NtSetSystemInformation>(
                          GetProcAddress(nt, "NtSetSystemInformation"))
                    : nullptr;
    if (!NtSet) return false;

    if (!enable_priv(L"SeProfileSingleProcessPrivilege")) return false;

    int cmd = MemoryPurgeStandbyList;
    LONG st = NtSet(SystemMemoryListInformation, &cmd, sizeof(cmd));
    return st == 0;   // STATUS_SUCCESS
}

} } // namespace pcore::runtime
