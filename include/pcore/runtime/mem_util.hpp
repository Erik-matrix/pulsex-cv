#pragma once
// mem_util — reclaim leftover RAM after unloading/switching an NPU model.
// When a Genie dialog is freed its mmap'd context pages linger in the Windows *standby list*
// (they read as "available" but pressure the next model's allocation → the "overload on switch").
// reclaim_system_cache() trims our working set and purges the standby list so the next model
// gets clean room. The standby purge needs elevation (SeProfileSingleProcessPrivilege); when not
// elevated it no-ops harmlessly and the function returns false.
namespace pcore { namespace runtime {

// Best-effort. Returns true only if the privileged standby purge actually ran (process elevated).
bool reclaim_system_cache();

} } // namespace pcore::runtime
