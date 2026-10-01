// genie_pipeline.hpp
// PulseCore — wrapper around Qualcomm Genie (GenieDialog C API) for fast,
// all-on-NPU LLM inference using AI-Hub-compiled QAIRT context binaries.
//
// Loads Genie.dll dynamically from the model's genie_bundle/ (so its sibling
// QNN DLLs resolve there, isolated from PulseCore's own QNN stack) and drives a
// resident GenieDialog. ~19 tok/s for Llama 3.2 3B (W4, KV-cache).
#pragma once

#include <functional>
#include <string>

namespace pcore::runtime {

class GeniePipeline {
public:
    GeniePipeline() = default;
    ~GeniePipeline();

    GeniePipeline(const GeniePipeline&)            = delete;
    GeniePipeline& operator=(const GeniePipeline&) = delete;

    // genie_dll_path: full path to genie_bundle/Genie.dll
    // config_json_path: full path to genie_config.json (absolute ctx-bin/tokenizer paths)
    bool load(const std::string& genie_dll_path, const std::string& config_json_path);

    // Generate a full response for an already chat-templated prompt.
    // token_cb (optional) is called with each incremental text chunk for streaming.
    std::string generate(const std::string& prompt,
                         std::function<void(const std::string&)> token_cb = nullptr);

    bool is_loaded() const noexcept { return dialog_ != nullptr; }

    // HTP memory swap: unload() frees the dialog (releases ~the model's HTP footprint) but keeps the
    // loaded DLL + parsed config so reload() can recreate the dialog cheaply (re-applies BURST).
    // Used to let the chat model and the image model take turns on the HTP. The app keeps the
    // conversation transcript, so it is re-sent on the next query \xE2\x80\x94 no Genie save/restore needed.
    void unload();
    bool reload();

    // Swap to a different bundle's genie_config.json at runtime (e.g. 1024 ↔ 2048 ↔ 4096 context).
    // Frees the current dialog+config, parses the new config, recreates the dialog. Reuses the already-
    // loaded Genie.dll (all bundles ship the same one). Returns false on failure (status left unloaded).
    bool switch_to(const std::string& config_json_path);

    // Live sampler control (Genie 2.47+): change temperature / top-p / top-k without
    // recreating the dialog. Applied to the dialog's sampler; the next generate() uses it.
    // Returns false if the runtime sampler API isn't available (older Genie).
    bool set_sampler(float temp, float top_p, int top_k, float rep_penalty = 1.1f);

private:
    void*       lib_    = nullptr;   // HMODULE for Genie.dll
    const void* config_ = nullptr;   // GenieDialogConfig_Handle_t
    const void* dialog_ = nullptr;   // GenieDialog_Handle_t

    // Resolved entry points (GetProcAddress)
    void* p_cfg_create_ = nullptr;
    void* p_dlg_create_ = nullptr;
    void* p_query_      = nullptr;
    void* p_reset_      = nullptr;
    void* p_dlg_free_   = nullptr;
    void* p_cfg_free_   = nullptr;

    // Runtime sampler API (Genie 2.47+) — null on older runtimes
    void* p_get_sampler_     = nullptr;
    void* p_smp_cfg_create_  = nullptr;
    void* p_smp_apply_       = nullptr;
    void* p_smp_cfg_free_    = nullptr;

    // Optional profiler (enabled by setting env PCORE_GENIE_PROFILE=1). When active, each
    // generate() dumps Genie's per-stage timing JSON (prefill vs decode tok/s, TTFT) to stdout.
    const void* prof_cfg_      = nullptr;   // GenieProfileConfig_Handle_t
    const void* profile_       = nullptr;   // GenieProfile_Handle_t
    void* p_prof_getjson_      = nullptr;
    void* p_prof_free_         = nullptr;
    void* p_prof_cfg_free_     = nullptr;
};

} // namespace pcore::runtime
