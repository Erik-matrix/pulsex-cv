// pulse_cv — runtime\genie_pipeline.cpp
#include <pcore/runtime/genie_pipeline.hpp>
#include <pcore/runtime/mem_util.hpp>   // reclaim standby RAM when a model is freed/switched

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <Genie/GenieCommon.h>
#include <Genie/GenieDialog.h>
#include <Genie/GenieProfile.h>
#include <Genie/GenieSampler.h>

namespace pcore::runtime {

// Function-pointer types matching the Genie C API (we GetProcAddress them so we
// never link Genie.lib and control the DLL search path explicitly).
using Fn_CfgCreate = Genie_Status_t (*)(const char*, GenieDialogConfig_Handle_t*);
using Fn_DlgCreate = Genie_Status_t (*)(GenieDialogConfig_Handle_t, GenieDialog_Handle_t*);
using Fn_Query     = Genie_Status_t (*)(GenieDialog_Handle_t, const char*,
                                        GenieDialog_SentenceCode_t,
                                        GenieDialog_QueryCallback_t, const void*);
using Fn_DlgFree   = Genie_Status_t (*)(GenieDialog_Handle_t);
using Fn_CfgFree   = Genie_Status_t (*)(GenieDialogConfig_Handle_t);
using Fn_Reset     = Genie_Status_t (*)(GenieDialog_Handle_t);
using Fn_ProfCfgCreate = Genie_Status_t (*)(const char*, GenieProfileConfig_Handle_t*);
using Fn_ProfCreate    = Genie_Status_t (*)(GenieProfileConfig_Handle_t, GenieProfile_Handle_t*);
using Fn_BindProfiler  = Genie_Status_t (*)(GenieDialogConfig_Handle_t, GenieProfile_Handle_t);
using Fn_ProfGetJson   = Genie_Status_t (*)(GenieProfile_Handle_t, Genie_AllocCallback_t, const char**);
using Fn_ProfFree      = Genie_Status_t (*)(GenieProfile_Handle_t);
using Fn_ProfCfgFree   = Genie_Status_t (*)(GenieProfileConfig_Handle_t);
using Fn_GetSampler    = Genie_Status_t (*)(GenieDialog_Handle_t, GenieSampler_Handle_t*);
using Fn_SmpCfgCreate  = Genie_Status_t (*)(const char*, GenieSamplerConfig_Handle_t*);
using Fn_SmpApply      = Genie_Status_t (*)(GenieSampler_Handle_t, GenieSamplerConfig_Handle_t);
using Fn_SmpCfgFree    = Genie_Status_t (*)(GenieSamplerConfig_Handle_t);

namespace {
// Genie's Rust tokenizer (oniguruma) ABORTS the whole process on invalid UTF-8
// (e.g. a multibyte char split by web-context truncation). Strip any invalid bytes
// so the prompt is always well-formed before it reaches the tokenizer.
std::string sanitize_utf8(const std::string& s) {
    std::string out; out.reserve(s.size());
    std::size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        std::size_t len;
        if      (c < 0x80)        len = 1;
        else if ((c >> 5) == 0x6) len = 2;
        else if ((c >> 4) == 0xE) len = 3;
        else if ((c >> 3) == 0x1E) len = 4;
        else { ++i; continue; }               // invalid lead byte → drop
        if (i + len > n) break;               // truncated multibyte at end → drop
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k)
            if (((unsigned char)s[i + k] >> 6) != 0x2) { ok = false; break; }
        if (ok) { out.append(s, i, len); i += len; }
        else { ++i; }                         // bad continuation → drop lead byte
    }
    return out;
}

struct QueryCtx {
    std::string text;
    std::function<void(const std::string&)>* cb;
};
// Genie streams response chunks here as they are generated.
void query_callback(const char* response,
                    const GenieDialog_SentenceCode_t /*code*/,
                    const void* userData) {
    if (!response || !userData) return;
    auto* ctx = const_cast<QueryCtx*>(static_cast<const QueryCtx*>(userData));
    ctx->text += response;
    if (ctx->cb && *ctx->cb) (*ctx->cb)(response);
}
} // namespace

GeniePipeline::~GeniePipeline() {
    if (profile_ && p_prof_free_)
        reinterpret_cast<Fn_ProfFree>(p_prof_free_)(static_cast<GenieProfile_Handle_t>(profile_));
    if (prof_cfg_ && p_prof_cfg_free_)
        reinterpret_cast<Fn_ProfCfgFree>(p_prof_cfg_free_)(static_cast<GenieProfileConfig_Handle_t>(prof_cfg_));
    if (dialog_ && p_dlg_free_)
        reinterpret_cast<Fn_DlgFree>(p_dlg_free_)(static_cast<GenieDialog_Handle_t>(dialog_));
    if (config_ && p_cfg_free_)
        reinterpret_cast<Fn_CfgFree>(p_cfg_free_)(static_cast<GenieDialogConfig_Handle_t>(config_));
    if (lib_) FreeLibrary(static_cast<HMODULE>(lib_));
}

bool GeniePipeline::load(const std::string& genie_dll_path,
                         const std::string& config_json_path) {
    // Load Genie.dll with its bundle directory on the search path so its sibling
    // QNN DLLs (QnnGenAiTransformer, QnnHtp, the V73 skel) resolve from there.
    lib_ = LoadLibraryExA(genie_dll_path.c_str(), nullptr,
                          LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib_) {
        std::printf("[Genie] LoadLibrary failed for %s (err=%lu)\n",
                    genie_dll_path.c_str(), GetLastError());
        return false;
    }
    auto H = static_cast<HMODULE>(lib_);
    p_cfg_create_ = (void*)GetProcAddress(H, "GenieDialogConfig_createFromJson");
    p_dlg_create_ = (void*)GetProcAddress(H, "GenieDialog_create");
    p_query_      = (void*)GetProcAddress(H, "GenieDialog_query");
    p_reset_      = (void*)GetProcAddress(H, "GenieDialog_reset");
    p_dlg_free_   = (void*)GetProcAddress(H, "GenieDialog_free");
    p_cfg_free_   = (void*)GetProcAddress(H, "GenieDialogConfig_free");
    // runtime sampler API (Genie 2.47+) — optional; null on older runtimes
    p_get_sampler_    = (void*)GetProcAddress(H, "GenieDialog_getSampler");
    p_smp_cfg_create_ = (void*)GetProcAddress(H, "GenieSamplerConfig_createFromJson");
    p_smp_apply_      = (void*)GetProcAddress(H, "GenieSampler_applyConfig");
    p_smp_cfg_free_   = (void*)GetProcAddress(H, "GenieSamplerConfig_free");
    if (!p_cfg_create_ || !p_dlg_create_ || !p_query_) {
        std::printf("[Genie] missing entry points in Genie.dll\n");
        return false;
    }

    std::ifstream f(config_json_path, std::ios::binary);
    if (!f) { std::printf("[Genie] cannot open %s\n", config_json_path.c_str()); return false; }
    std::stringstream ss; ss << f.rdbuf();
    std::string cfg = ss.str();

    GenieDialogConfig_Handle_t cfgh = nullptr;
    if (reinterpret_cast<Fn_CfgCreate>(p_cfg_create_)(cfg.c_str(), &cfgh) != GENIE_STATUS_SUCCESS) {
        std::printf("[Genie] createFromJson failed\n"); return false;
    }
    config_ = cfgh;

    // OPTIMISATION DIAGNOSTIC: optional Genie profiler (set PCORE_GENIE_PROFILE=1). Must bind to the
    // config BEFORE the dialog is created. The profile-config JSON format is undocumented, so we try a
    // few candidates and skip gracefully if none is accepted — never blocks the chat path.
    if (const char* pf = std::getenv("PCORE_GENIE_PROFILE"); pf && pf[0] == '1') {
        auto p_pc_create = (void*)GetProcAddress(H, "GenieProfileConfig_createFromJson");
        auto p_p_create  = (void*)GetProcAddress(H, "GenieProfile_create");
        auto p_bind      = (void*)GetProcAddress(H, "GenieDialogConfig_bindProfiler");
        p_prof_getjson_  = (void*)GetProcAddress(H, "GenieProfile_getJsonData");
        p_prof_free_     = (void*)GetProcAddress(H, "GenieProfile_free");
        p_prof_cfg_free_ = (void*)GetProcAddress(H, "GenieProfileConfig_free");
        // documented 2.47 format first (profile/json.html), then the old guesses as fallback
        const char* candidates[] = {
            "{\"profile\":{\"version\":1,\"trace\":{\"version\":1,\"enable\":true}}}",
            "{\"profile\":\"detailed\"}", "{\"profile\":\"basic\"}", "{}" };
        if (p_pc_create && p_p_create && p_bind) {
            for (const char* cj : candidates) {
                GenieProfileConfig_Handle_t pch = nullptr;
                if (reinterpret_cast<Fn_ProfCfgCreate>(p_pc_create)(cj, &pch) != GENIE_STATUS_SUCCESS) continue;
                GenieProfile_Handle_t ph = nullptr;
                if (reinterpret_cast<Fn_ProfCreate>(p_p_create)(pch, &ph) != GENIE_STATUS_SUCCESS) {
                    reinterpret_cast<Fn_ProfCfgFree>(p_prof_cfg_free_)(pch); continue;
                }
                if (reinterpret_cast<Fn_BindProfiler>(p_bind)(cfgh, ph) != GENIE_STATUS_SUCCESS) {
                    reinterpret_cast<Fn_ProfFree>(p_prof_free_)(ph);
                    reinterpret_cast<Fn_ProfCfgFree>(p_prof_cfg_free_)(pch); continue;
                }
                prof_cfg_ = pch; profile_ = ph;
                std::printf("[Genie] profiler bound (config=%s) — per-query timing → stdout\n", cj);
                break;
            }
            if (!profile_)
                std::printf("[Genie] profiler unavailable (no config string accepted) — continuing without\n");
        }
    }

    // AUTO-FREE ON MODEL SWITCH: before mmap'ing this model's context, reclaim any pages a
    // previously-closed model app left in the standby list. Covers the real workflow (close app A,
    // open app B) — B's load() cleans up A's leftovers so peak RAM is B alone. (Elevation needed for
    // the standby purge; no-ops otherwise.)
    {
        const bool r = reclaim_system_cache();
        std::printf("[Genie] pre-load cache reclaim %s\n",
                    r ? "OK (previous model's standby pages freed)"
                      : "(skipped — run PulseCore elevated to purge standby)");
        std::fflush(stdout);
    }

    GenieDialog_Handle_t dlgh = nullptr;
    if (reinterpret_cast<Fn_DlgCreate>(p_dlg_create_)(cfgh, &dlgh) != GENIE_STATUS_SUCCESS) {
        std::printf("[Genie] dialog create failed\n"); return false;
    }
    dialog_ = dlgh;

    // OPTIMISATION: force the HTP into BURST performance policy (peak tok/s — highest clocks,
    // ideal for short interactive chat replies on a plugged-in laptop). Optional entry point:
    // if the DLL lacks it (older Genie) we silently keep the default policy. See genie_dll analysis.
    if (auto p_set_perf = (void*)GetProcAddress(H, "GenieDialog_setPerformancePolicy")) {
        using Fn_SetPerf = Genie_Status_t (*)(GenieDialog_Handle_t, Genie_PerformancePolicy_t);
        auto pst = reinterpret_cast<Fn_SetPerf>(p_set_perf)(dlgh, GENIE_PERFORMANCE_BURST);
        std::printf(pst == GENIE_STATUS_SUCCESS
                        ? "[Genie] performance policy = BURST (peak tok/s)\n"
                        : "[Genie] setPerformancePolicy failed (status=%d) — default policy kept\n",
                    (int)pst);
    }

    std::printf("[Genie] dialog ready (resident, all-on-NPU)\n");
    return true;
}

void GeniePipeline::unload() {
    if (dialog_ && p_dlg_free_) {
        reinterpret_cast<Fn_DlgFree>(p_dlg_free_)(static_cast<GenieDialog_Handle_t>(dialog_));
        dialog_ = nullptr;
        // Reclaim the freed model's mmap'd context pages so the next user (SD / another model)
        // isn't crowded by them lingering in the standby list.
        const bool r = reclaim_system_cache();
        std::printf("[Genie] dialog unloaded (HTP freed); cache reclaim %s\n",
                    r ? "OK" : "(skipped — run PulseCore elevated to purge standby)");
        std::fflush(stdout);
    }
}

bool GeniePipeline::reload() {
    if (dialog_) return true;                       // already resident
    if (!config_ || !p_dlg_create_) return false;
    GenieDialog_Handle_t dlgh = nullptr;
    if (reinterpret_cast<Fn_DlgCreate>(p_dlg_create_)(
            static_cast<GenieDialogConfig_Handle_t>(config_), &dlgh) != GENIE_STATUS_SUCCESS) {
        std::printf("[Genie] dialog reload failed\n"); return false;
    }
    dialog_ = dlgh;
    if (lib_)                                       // re-apply BURST after recreate
        if (auto p = (void*)GetProcAddress(static_cast<HMODULE>(lib_), "GenieDialog_setPerformancePolicy")) {
            using Fn_SetPerf = Genie_Status_t (*)(GenieDialog_Handle_t, Genie_PerformancePolicy_t);
            reinterpret_cast<Fn_SetPerf>(p)(dlgh, GENIE_PERFORMANCE_BURST);
        }
    std::printf("[Genie] dialog reloaded (resident, BURST)\n"); std::fflush(stdout);
    return true;
}

bool GeniePipeline::switch_to(const std::string& config_json_path) {
    // free current dialog + config (keep lib_ + entry points)
    if (dialog_ && p_dlg_free_) {
        reinterpret_cast<Fn_DlgFree>(p_dlg_free_)(static_cast<GenieDialog_Handle_t>(dialog_));
        dialog_ = nullptr;
    }
    if (config_ && p_cfg_free_) {
        reinterpret_cast<Fn_CfgFree>(p_cfg_free_)(static_cast<GenieDialogConfig_Handle_t>(config_));
        config_ = nullptr;
    }
    // Auto-free: reclaim the OLD model's unmapped pages BEFORE the new one allocates, so peak RAM
    // is the new model alone (not old-in-standby + new-committed). This is the "overload på modellbyte" fix.
    {
        const bool r = reclaim_system_cache();
        std::printf("[Genie] switch: old context freed; cache reclaim %s\n",
                    r ? "OK" : "(skipped — run PulseCore elevated to purge standby)");
        std::fflush(stdout);
    }
    std::ifstream f(config_json_path, std::ios::binary);
    if (!f) { std::printf("[Genie] switch: cannot open %s\n", config_json_path.c_str()); return false; }
    std::stringstream ss; ss << f.rdbuf();
    const std::string cfg = ss.str();
    GenieDialogConfig_Handle_t cfgh = nullptr;
    if (reinterpret_cast<Fn_CfgCreate>(p_cfg_create_)(cfg.c_str(), &cfgh) != GENIE_STATUS_SUCCESS) {
        std::printf("[Genie] switch: createFromJson failed\n"); return false;
    }
    config_ = cfgh;
    GenieDialog_Handle_t dlgh = nullptr;
    if (reinterpret_cast<Fn_DlgCreate>(p_dlg_create_)(cfgh, &dlgh) != GENIE_STATUS_SUCCESS) {
        std::printf("[Genie] switch: dialog create failed\n"); return false;
    }
    dialog_ = dlgh;
    if (lib_)
        if (auto p = (void*)GetProcAddress(static_cast<HMODULE>(lib_), "GenieDialog_setPerformancePolicy")) {
            using Fn_SetPerf = Genie_Status_t (*)(GenieDialog_Handle_t, Genie_PerformancePolicy_t);
            reinterpret_cast<Fn_SetPerf>(p)(dlgh, GENIE_PERFORMANCE_BURST);
        }
    std::printf("[Genie] switched to %s (BURST)\n", config_json_path.c_str()); std::fflush(stdout);
    return true;
}

bool GeniePipeline::set_sampler(float temp, float top_p, int top_k, float rep_penalty) {
    if (!dialog_ || !p_get_sampler_ || !p_smp_cfg_create_ || !p_smp_apply_) return false;
    GenieSampler_Handle_t smp = nullptr;
    if (reinterpret_cast<Fn_GetSampler>(p_get_sampler_)(
            static_cast<GenieDialog_Handle_t>(dialog_), &smp) != GENIE_STATUS_SUCCESS || !smp)
        return false;
    char js[256];
    std::snprintf(js, sizeof(js),
                  "{\"version\":1,\"temp\":%.3f,\"top-k\":%d,\"top-p\":%.3f,"
                  "\"token-penalty\":{\"version\":1,\"penalize-last-n\":64,\"repetition-penalty\":%.3f}}",
                  temp, top_k, top_p, rep_penalty);
    GenieSamplerConfig_Handle_t cfg = nullptr;
    if (reinterpret_cast<Fn_SmpCfgCreate>(p_smp_cfg_create_)(js, &cfg) != GENIE_STATUS_SUCCESS || !cfg)
        return false;
    auto st = reinterpret_cast<Fn_SmpApply>(p_smp_apply_)(smp, cfg);
    if (p_smp_cfg_free_) reinterpret_cast<Fn_SmpCfgFree>(p_smp_cfg_free_)(cfg);
    return st == GENIE_STATUS_SUCCESS;
}

std::string GeniePipeline::generate(const std::string& prompt,
                                    std::function<void(const std::string&)> token_cb) {
    if (!dialog_ || !p_query_) return {};
    // Reset conversation state so each request is independent (stateless server):
    // stops context from accumulating across requests (which grew prefill → slower
    // each call) and prevents one user's history bleeding into the next.
    if (p_reset_)
        reinterpret_cast<Fn_Reset>(p_reset_)(static_cast<GenieDialog_Handle_t>(dialog_));
    const std::string clean = sanitize_utf8(prompt);
    QueryCtx ctx; ctx.cb = &token_cb;
    auto st = reinterpret_cast<Fn_Query>(p_query_)(
        static_cast<GenieDialog_Handle_t>(dialog_), clean.c_str(),
        GENIE_DIALOG_SENTENCE_COMPLETE, &query_callback, &ctx);
    if (st != GENIE_STATUS_SUCCESS)
        std::printf("[Genie] query failed (status=%d)\n", (int)st);
    // If the profiler is active, dump Genie's per-stage timing JSON (prefill vs decode, TTFT).
    if (profile_ && p_prof_getjson_) {
        const char* js = nullptr;
        if (reinterpret_cast<Fn_ProfGetJson>(p_prof_getjson_)(
                static_cast<GenieProfile_Handle_t>(profile_), nullptr, &js) == GENIE_STATUS_SUCCESS && js)
            std::printf("[Genie][profile] %s\n", js);
    }
    return ctx.text;
}

} // namespace pcore::runtime
