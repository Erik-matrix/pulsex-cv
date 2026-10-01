// pulse_cv_gui.cpp — PulseX CV: the window. Dear ImGui + DX11.
// Spawns pulse_cv_tailor.exe (next to this exe) for the writing and the PDFs. Paths are app-relative or per-user.
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>     // GET_X_LPARAM / GET_Y_LPARAM (borderless hit-test)
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>       // rounded corners, dark frame
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"comdlg32.lib")
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <array>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <nlohmann/json.hpp>
#include "cloud_llm.hpp"                 // the engine list (Models tab) - shared with the tailor
#include "ad_fetch.hpp"                  // a link to the ad -> role, company, text
#include "lang.hpp"                      // the language files lang\<code>.json: what an ad asks for -> sentences to tick

// ── paths. The manifest makes the ANSI code page UTF-8, so every narrow string here is UTF-8. ──
static std::string narrow(const wchar_t* w){ int n=WideCharToMultiByte(CP_UTF8,0,w,-1,nullptr,0,nullptr,nullptr);
    std::string s(n>0?n-1:0,'\0'); if(n>0) WideCharToMultiByte(CP_UTF8,0,w,-1,&s[0],n,nullptr,nullptr); return s; }
static std::string app_dir(){ wchar_t b[MAX_PATH]={0}; GetModuleFileNameW(nullptr,b,MAX_PATH);
    std::string p=narrow(b); auto s=p.find_last_of("\\/"); return s==std::string::npos?std::string("."):p.substr(0,s); }
static std::string P(const std::string& sub){ return app_dir()+"\\"+sub; }
static std::string known_folder(REFKNOWNFOLDERID id){ PWSTR wp=nullptr; std::string out;
    if(SUCCEEDED(SHGetKnownFolderPath(id,0,nullptr,&wp)) && wp) out=narrow(wp);
    if(wp) CoTaskMemFree(wp); return out; }
// Where the finished PDFs land: the user's real Downloads folder (PULSE_CV_OUT = another folder).
static std::string downloads_dir(){
    if(const char* o=std::getenv("PULSE_CV_OUT")) if(*o) return o;
    std::string out=known_folder(FOLDERID_Downloads);
    if(out.empty()){ const char* up=std::getenv("USERPROFILE"); out = up? std::string(up)+"\\Downloads" : app_dir()+"\\output"; }
    return out;
}
// Documents\pulse_cv — the user's details, vault, models and settings (PULSE_CV_DATA = another folder).
static std::string docs_dir(){
    if(const char* o=std::getenv("PULSE_CV_DATA")) if(*o){ std::error_code oec; std::filesystem::create_directories(o,oec); return o; }
    std::string out=known_folder(FOLDERID_Documents);
    if(out.empty()){ const char* up=std::getenv("USERPROFILE"); out = up? std::string(up)+"\\Documents" : app_dir(); }
    out += "\\pulse_cv"; std::error_code ec; std::filesystem::create_directories(out,ec); return out;
}
static std::wstring to_w(const std::string& s){ return cloud_llm::widen(s); }
static void shell_open(const std::string& path){ ShellExecuteW(nullptr,L"open",to_w(path).c_str(),nullptr,nullptr,SW_SHOWNORMAL); }
// Run a command line hidden and wait; returns the exit code, 100 when it could not start.
static DWORD run_wait(const std::string& cmd, DWORD timeout_ms, const std::string& wd=std::string()){
    std::wstring w=to_w(cmd), wwd=to_w(wd.empty()?app_dir():wd);
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{}; DWORD rc=100;
    if(CreateProcessW(nullptr,&w[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,wwd.c_str(),&si,&pi)){
        WaitForSingleObject(pi.hProcess,timeout_ms); GetExitCodeProcess(pi.hProcess,&rc);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return rc;
}

// ── DX11 plumbing (trimmed from the official ImGui example) ──
static ID3D11Device*           g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain        = nullptr;
static ID3D11RenderTargetView* g_mainRTV           = nullptr;
static void CreateRenderTarget(){ ID3D11Texture2D* back=nullptr; g_pSwapChain->GetBuffer(0,IID_PPV_ARGS(&back));
    if(back){ g_pd3dDevice->CreateRenderTargetView(back,nullptr,&g_mainRTV); back->Release(); } }
static void CleanupRenderTarget(){ if(g_mainRTV){ g_mainRTV->Release(); g_mainRTV=nullptr; } }
static bool CreateDeviceD3D(HWND hWnd){
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=2; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator=60; sd.BufferDesc.RefreshRate.Denominator=1;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=hWnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
    sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL lvls[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
    if(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,lvls,2,D3D11_SDK_VERSION,
        &sd,&g_pSwapChain,&g_pd3dDevice,&fl,&g_pd3dDeviceContext)!=S_OK) return false;
    CreateRenderTarget(); return true; }
static void CleanupDeviceD3D(){ CleanupRenderTarget();
    if(g_pSwapChain){g_pSwapChain->Release();g_pSwapChain=nullptr;}
    if(g_pd3dDeviceContext){g_pd3dDeviceContext->Release();g_pd3dDeviceContext=nullptr;}
    if(g_pd3dDevice){g_pd3dDevice->Release();g_pd3dDevice=nullptr;} }

// ── window: no native frame. The title bar is ours; Windows still moves, snaps and maximises it (HTCAPTION). ──
static float g_scale    = 1.0f;          // display scale (DPI / 96)
static UINT  g_new_dpi  = 0;             // set by WM_DPICHANGED, applied by the main loop
static int   g_cap_h    = 0;             // title-bar height in client pixels
static RECT  g_nodrag[2]= {};            // title-bar areas that hold controls (tabs, window buttons)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
static LRESULT WINAPI WndProc(HWND hWnd,UINT msg,WPARAM wP,LPARAM lP){
    if(ImGui_ImplWin32_WndProcHandler(hWnd,msg,wP,lP)) return true;
    switch(msg){
        case WM_SIZE: if(g_pd3dDevice && wP!=SIZE_MINIMIZED){ CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0,(UINT)LOWORD(lP),(UINT)HIWORD(lP),DXGI_FORMAT_UNKNOWN,0); CreateRenderTarget(); } return 0;
        case WM_NCCALCSIZE: if(wP) return 0; break;        // remove the native frame
        case WM_NCHITTEST: {
            const LONG b=(LONG)(8*g_scale); POINT pt={ GET_X_LPARAM(lP), GET_Y_LPARAM(lP) }; RECT r; GetWindowRect(hWnd,&r);
            if(!IsZoomed(hWnd)){
                const bool L=pt.x<r.left+b, R=pt.x>=r.right-b, T=pt.y<r.top+b, B=pt.y>=r.bottom-b;
                if(T&&L)return HTTOPLEFT; if(T&&R)return HTTOPRIGHT; if(B&&L)return HTBOTTOMLEFT; if(B&&R)return HTBOTTOMRIGHT;
                if(L)return HTLEFT; if(R)return HTRIGHT; if(T)return HTTOP; if(B)return HTBOTTOM; }
            POINT c=pt; ScreenToClient(hWnd,&c);
            if(c.y<g_cap_h && !PtInRect(&g_nodrag[0],c) && !PtInRect(&g_nodrag[1],c)) return HTCAPTION;
            return HTCLIENT; }
        case WM_GETMINMAXINFO: {                            // maximise inside the work area; a sensible minimum
            MINMAXINFO* mmi=(MINMAXINFO*)lP; HMONITOR mon=MonitorFromWindow(hWnd,MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if(GetMonitorInfo(mon,&mi)){ mmi->ptMaxPosition.x=mi.rcWork.left-mi.rcMonitor.left; mmi->ptMaxPosition.y=mi.rcWork.top-mi.rcMonitor.top;
                mmi->ptMaxSize.x=mi.rcWork.right-mi.rcWork.left; mmi->ptMaxSize.y=mi.rcWork.bottom-mi.rcWork.top; }
            mmi->ptMinTrackSize.x=(LONG)(760*g_scale); mmi->ptMinTrackSize.y=(LONG)(600*g_scale); return 0; }
        case WM_DPICHANGED: { g_new_dpi=HIWORD(wP); const RECT* r=(const RECT*)lP;
            SetWindowPos(hWnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE); return 0; }
        case WM_SYSCOMMAND: if((wP&0xfff0)==SC_KEYMENU) return 0; break;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd,msg,wP,lP);
}

// ════════════════════════ look ════════════════════════
static const float FS = 21.0f;                              // body text size before display scaling
static ImFont *g_font=nullptr, *g_semi=nullptr;
static float px(float v){ return v*g_scale; }
static ImVec4 rgb(int r,int g,int b,float a=1.0f){ return ImVec4(r/255.0f,g/255.0f,b/255.0f,a); }
static const ImVec4 C_BG    = rgb( 58, 63, 71);            // window: soft slate, not black
static const ImVec4 C_CARD  = rgb( 71, 77, 87);            // cards lift off the window
static const ImVec4 C_INPUT = rgb( 47, 51, 59);            // fields sink into the card
static const ImVec4 C_BTN   = rgb( 92, 99,112);
static const ImVec4 C_BTN_H = rgb(106,114,128);
static const ImVec4 C_TEXT  = rgb(236,239,243);
static const ImVec4 C_DIM   = rgb(170,178,190);
static const ImVec4 C_ACC   = rgb( 42,166,154);            // teal
static const ImVec4 C_ACC_H = rgb( 62,190,177);
static const ImVec4 C_ACC_L = rgb( 34,134,125);
static const ImVec4 C_OK    = rgb(110,205,140);
static const ImVec4 C_WARN  = rgb(232,184, 92);
static const ImVec4 C_ERR   = rgb(236,120,110);

static void apply_style(){
    ImGuiStyle st; ImGui::StyleColorsDark(&st);
    st.WindowRounding=0; st.ChildRounding=20; st.FrameRounding=12; st.PopupRounding=12; st.ScrollbarRounding=10; st.GrabRounding=10;
    st.WindowPadding=ImVec2(14,12); st.FramePadding=ImVec2(11,8); st.ItemSpacing=ImVec2(10,9); st.ItemInnerSpacing=ImVec2(8,6);
    st.ScrollbarSize=11; st.WindowBorderSize=0; st.ChildBorderSize=0; st.PopupBorderSize=1; st.FrameBorderSize=1;
    ImVec4* c=st.Colors;
    c[ImGuiCol_WindowBg]=C_BG; c[ImGuiCol_ChildBg]=ImVec4(0,0,0,0); c[ImGuiCol_PopupBg]=rgb(50,55,63);
    c[ImGuiCol_Text]=C_TEXT; c[ImGuiCol_TextDisabled]=C_DIM; c[ImGuiCol_Border]=ImVec4(1,1,1,0.07f);
    c[ImGuiCol_FrameBg]=C_INPUT; c[ImGuiCol_FrameBgHovered]=rgb(52,57,66); c[ImGuiCol_FrameBgActive]=rgb(52,57,66);
    c[ImGuiCol_Button]=C_BTN; c[ImGuiCol_ButtonHovered]=C_BTN_H; c[ImGuiCol_ButtonActive]=rgb(82,89,101);
    c[ImGuiCol_Header]=ImVec4(C_ACC.x,C_ACC.y,C_ACC.z,0.30f); c[ImGuiCol_HeaderHovered]=ImVec4(1,1,1,0.07f); c[ImGuiCol_HeaderActive]=ImVec4(C_ACC.x,C_ACC.y,C_ACC.z,0.45f);
    c[ImGuiCol_CheckMark]=C_ACC_H; c[ImGuiCol_TextSelectedBg]=ImVec4(C_ACC.x,C_ACC.y,C_ACC.z,0.45f); c[ImGuiCol_InputTextCursor]=C_TEXT;
    c[ImGuiCol_ScrollbarBg]=ImVec4(0,0,0,0); c[ImGuiCol_ScrollbarGrab]=ImVec4(1,1,1,0.16f);
    c[ImGuiCol_ScrollbarGrabHovered]=ImVec4(1,1,1,0.26f); c[ImGuiCol_ScrollbarGrabActive]=ImVec4(1,1,1,0.34f);
    c[ImGuiCol_Separator]=ImVec4(1,1,1,0.08f); c[ImGuiCol_NavCursor]=C_ACC_H;
    st.ScaleAllSizes(g_scale); st.FontSizeBase=FS; st.FontScaleDpi=g_scale;
    ImGui::GetStyle()=st;
}

// A rounded panel. size.y == 0 with ImGuiChildFlags_AutoResizeY grows with its content.
static bool begin_card(const char* id, ImVec2 size, const char* title=nullptr, ImGuiChildFlags cf=0, ImGuiWindowFlags wf=0){
    ImGui::PushStyleColor(ImGuiCol_ChildBg,C_CARD); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(px(20),px(16)));
    bool v=ImGui::BeginChild(id,size,ImGuiChildFlags_AlwaysUseWindowPadding|cf,wf);
    ImGui::PopStyleVar(); ImGui::PopStyleColor();
    if(title){ ImGui::PushFont(g_semi,FS*1.08f); ImGui::TextUnformatted(title); ImGui::PopFont(); }
    return v;
}
static void end_card(){ ImGui::EndChild(); }
// Small dim caption above a field.
static void label(const char* t){
    ImGui::PushFont(nullptr,FS*0.84f); ImGui::PushStyleColor(ImGuiCol_Text,C_DIM);
    ImGui::TextUnformatted(t); ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()-px(5));
}
// The next item starts `gap` below the previous one (instead of the default item spacing).
static void vgap(float gap){ ImGui::SetCursorPosY(ImGui::GetCursorPosY()+gap-ImGui::GetStyle().ItemSpacing.y); }
static float label_h(){ return FS*0.84f*g_scale + ImGui::GetStyle().ItemSpacing.y - px(5); }
static float lines_h(int n){ return n*ImGui::GetTextLineHeight() + 2*ImGui::GetStyle().FramePadding.y + px(2); }   // a text box of n lines
static void hint(const char* t){
    ImGui::PushFont(nullptr,FS*0.84f); ImGui::PushStyleColor(ImGuiCol_Text,C_DIM);
    ImGui::TextWrapped("%s",t); ImGui::PopStyleColor(); ImGui::PopFont();
}
static float hint_h(const char* t, float wrap_w){
    ImGui::PushFont(nullptr,FS*0.84f); float h=ImGui::CalcTextSize(t,nullptr,false,wrap_w).y; ImGui::PopFont(); return h;
}
// Fields side by side: each column starts at the row's top (a group after SameLine() would inherit the previous
// group's text baseline and sit 8 px lower).
struct Row { float x,y; Row(){ x=ImGui::GetCursorPosX(); y=ImGui::GetCursorPosY(); }
    void next(float w){ x+=w+ImGui::GetStyle().ItemSpacing.x; ImGui::SetCursorPos(ImVec2(x,y)); } };
// A drop-down whose arrow sits in the field instead of on a button.
static void push_combo(){ ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(1,1,1,0.06f)); }
static void pop_combo(){ ImGui::PopStyleColor(2); }
static bool primary_button(const char* t, ImVec2 size=ImVec2(0,0)){
    ImGui::PushStyleColor(ImGuiCol_Button,C_ACC); ImGui::PushStyleColor(ImGuiCol_ButtonHovered,C_ACC_H); ImGui::PushStyleColor(ImGuiCol_ButtonActive,C_ACC_L);
    ImGui::PushFont(g_semi,0.0f); bool r=ImGui::Button(t,size); ImGui::PopFont(); ImGui::PopStyleColor(3); return r;
}
static bool text_box(const char* id, char* buf, size_t n, float h){
    return ImGui::InputTextMultiline(id,buf,n,ImVec2(-1,h),ImGuiInputTextFlags_WordWrap);
}
static std::string host_of(const std::string& url){
    size_t a=url.find("://"); a = a==std::string::npos? 0 : a+3; size_t b=url.find_first_of("/:",a);
    return url.substr(a, b==std::string::npos? std::string::npos : b-a);
}
static bool is_local_host(const std::string& url){ std::string h=host_of(url); return h=="127.0.0.1"||h=="localhost"||h=="[::1]"; }

// ════════════════════════ app state + logic ════════════════════════
enum { ST_IDLE=0, ST_BUSY=1, ST_OK=2, ST_ERR=3, ST_WARN=4 };
struct Preset { const char* label; const char* kind; const char* url; const char* model; };
static const Preset PRESETS[] = {
    {"A model file on this PC (GGUF)",              "llama",     "",                                          ""},
    {"Anthropic (Claude)",                          "anthropic", "https://api.anthropic.com",                 "claude-opus-5-5"},
    {"Google Gemini",                               "gemini",    "https://generativelanguage.googleapis.com", "gemini-2.5-pro"},
    {"OpenAI",                                      "openai",    "https://api.openai.com/v1",                 ""},
    {"Mistral",                                     "openai",    "https://api.mistral.ai/v1",                 ""},
    {"OpenRouter",                                  "openai",    "https://openrouter.ai/api/v1",              ""},
    {"Local server (llama.cpp, Ollama, LM Studio)", "openai",    "http://127.0.0.1:8080/v1",                  ""},
    {"Other (OpenAI-compatible)",                   "openai",    "",                                          ""},
};
static const char* KINDS[]      = {"openai","anthropic","gemini","llama"};
static const char* KIND_LABEL[] = {"OpenAI-compatible","Anthropic Messages","Google Gemini","llama.cpp on this PC"};
enum { KIND_COUNT=4, KIND_LLAMA=3, PRESET_FILE=0, PRESET_ANTHROPIC=1 };
static bool engine_is_local(const cloud_llm::Engine& e){ return e.kind=="llama" || is_local_host(e.base_url); }
static std::string file_name_of(const std::string& path){ size_t k=path.find_last_of("\\/"); return k==std::string::npos? path : path.substr(k+1); }

struct PulseCvApp {
    char cv_role[128]="", cv_company[128]="";
    char cv_ad[16000]="";
    char cv_extra[1024]="";
    char cv_opening[600]=""; bool cv_opening_loaded=false;   // remembered in <data>\letter_opening.txt
    char cv_url[512]="";
    char cv_ingress[4000]="", cv_brev[8000]="";
    char cv_interests[512]="", cv_strengths[512]="";
    std::vector<std::string> cv_lead;     // titles of the jobs/education this letter is built on, most important first; empty = automatic
    std::vector<std::string> confirmed;   // ids (lang "asks") of what the user has ticked as true of them; remembered between ads
    std::vector<lang::Pack> packs;        // lang\sv.json, lang\en.json, ... (Swedish first: a tie in detection goes to it)
    void lang_load(){
        std::vector<std::string> dirs{P("lang"), P("..\\lang")}; if(const char* e=std::getenv("PULSE_CV_LANG")) if(*e) dirs.insert(dirs.begin(), std::string(e));
        std::error_code ec;
        for(auto& d: dirs){ if(!std::filesystem::exists(std::filesystem::u8path(d),ec)) continue;
            std::vector<std::string> codes;
            for(auto& e: std::filesystem::directory_iterator(std::filesystem::u8path(d),ec)) if(e.path().extension()==".json") codes.push_back(e.path().stem().u8string());
            std::sort(codes.begin(),codes.end(),[](const std::string& x, const std::string& y){ return (x=="sv")!=(y=="sv")? x=="sv" : x<y; });
            for(auto& c: codes){ lang::Pack pk=lang::load(d,c); if(pk.ok) packs.push_back(pk); }
            if(!packs.empty()) break; }
    }
    const lang::Pack* pack_for(const std::string& text) const { return packs.empty()? nullptr : &packs[lang::detect(text,packs)]; }   // the language of an ad
    float cv_acc[3]={0.20f,0.10f,0.25f};
    int cv_profile=0;                 // 0=Auto, 1=Practical, 2=Academic
    std::string engine_sel="local";   // "local" | "claude" | "cloud:<name>"
    std::atomic<bool> cv_busy{false};
    std::thread cv_worker, cv_fetch_worker, test_worker;

    // shared with the worker threads
    std::mutex mu;
    std::string status; int status_kind=ST_IDLE;
    bool fetched=false; std::string f_role, f_company, f_ad;
    std::atomic<bool> drafts_ready{false};
    void set_status(int kind, const std::string& s){ std::lock_guard<std::mutex> g(mu); status_kind=kind; status=s; }

    // ── settings (engine, profile, colour) ──
    std::string settings_path = docs_dir()+"\\settings.json";
    void settings_load(){
        std::ifstream f(std::filesystem::u8path(settings_path)); if(!f) return;
        try{ nlohmann::json j; f>>j; engine_sel=j.value("engine",std::string("local")); cv_profile=j.value("profile",0);
            if(j.contains("accent") && j["accent"].is_array() && j["accent"].size()==3) for(int k=0;k<3;k++) cv_acc[k]=j["accent"][k].get<float>();
            if(j.contains("confirmed") && j["confirmed"].is_array()){ confirmed.clear(); for(auto& x: j["confirmed"]) if(x.is_string()) confirmed.push_back(x.get<std::string>()); }
        }catch(...){}
    }
    void settings_save(){
        nlohmann::json j;                      // keys this window does not own (e.g. "qwen_bundle") are kept
        { std::ifstream f(std::filesystem::u8path(settings_path)); try{ if(f) f>>j; }catch(...){ j=nlohmann::json::object(); } if(!j.is_object()) j=nlohmann::json::object(); }
        j["engine"]=engine_sel; j["profile"]=cv_profile; j["accent"]={cv_acc[0],cv_acc[1],cv_acc[2]};
        j["confirmed"]=confirmed;
        std::ofstream o(std::filesystem::u8path(settings_path)); o<<j.dump(2);
    }

    // ── cloud models ──
    std::string engines_path = docs_dir()+"\\engines.json";
    std::vector<cloud_llm::Engine> engines;
    int  ed_idx=-1;                   // -1 nothing, -2 a new model, >=0 engines[ed_idx]
    int  ed_builtin=0;                // which built-in the right card describes when ed_idx==-1 (0 local, 1 Claude Code)
    int  ed_preset=0, ed_kind=0;
    char ed_name[64]="", ed_url[256]="", ed_model[128]="", ed_key[512]="";
    char ed_gguf[520]="", ed_server[520]=""; int ed_device=0;   // kind "llama": the model file, llama-server.exe, 0 = processor, 1 = NPU
    std::string ed_msg; int ed_msg_kind=ST_IDLE;
    std::atomic<bool> test_busy{false};
    void engines_load(){ engines=cloud_llm::load(engines_path); }
    bool engine_exists(const std::string& sel){
        if(sel=="local"||sel=="claude") return true;
        if(sel.rfind("cloud:",0)==0) for(auto& e: engines) if(e.name==sel.substr(6)) return true;
        return false; }
    // Is the built-in Qwen model really here? Next to the program, or where settings.json says ("qwen_bundle").
    // Looked up every few seconds. A first-time user has no bundle, and the window must then ask for a model instead
    // of offering one that cannot run.
    bool qwen_here(){
        static ULONGLONG at=0; static bool here=false; const ULONGLONG now=GetTickCount64();
        if(at && now-at<3000) return here;
        at=now; std::error_code ec; std::string b=P("models\\qwen3-4b-4k\\genie_bundle");
        if(!std::filesystem::exists(std::filesystem::u8path(b+"\\Genie.dll"),ec)){
            std::ifstream f(std::filesystem::u8path(settings_path));
            try{ nlohmann::json j; if(f) f>>j; b=j.value("qwen_bundle",std::string()); }catch(...){ b.clear(); } }
        here = !b.empty() && std::filesystem::exists(std::filesystem::u8path(b+"\\Genie.dll"),ec);
        return here; }
    bool no_model(){ return engine_sel=="local" && !qwen_here(); }   // nothing is chosen that can write
    // The built-in model chosen but not installed: a model the user has added on this PC is taken instead, if there
    // is one. Never a cloud model by itself - that sends the merits away, and is the user's own choice to make.
    void engine_settle(){
        if(!engine_exists(engine_sel)) engine_sel="local";
        if(no_model()) for(auto& e: engines) if(engine_is_local(e)){ engine_sel="cloud:"+e.name; break; } }
    std::string engine_label(const std::string& sel){
        if(sel=="local")  return qwen_here()? "Qwen3-4B on this PC" : "Choose a model\xE2\x80\xA6";
        if(sel=="claude") return "Claude Code";
        return sel.substr(6); }
    void ed_open(int idx){
        ed_idx=idx; ed_msg.clear(); ed_msg_kind=ST_IDLE; SecureZeroMemory(ed_key,sizeof(ed_key));
        if(idx>=0){ auto& e=engines[idx];
            std::snprintf(ed_name,sizeof(ed_name),"%s",e.name.c_str()); std::snprintf(ed_url,sizeof(ed_url),"%s",e.base_url.c_str());
            std::snprintf(ed_model,sizeof(ed_model),"%s",e.model.c_str());
            ed_kind=0; for(int k=0;k<KIND_COUNT;k++) if(e.kind==KINDS[k]) ed_kind=k;
            std::snprintf(ed_gguf,sizeof(ed_gguf),"%s",e.gguf.c_str()); std::snprintf(ed_server,sizeof(ed_server),"%s",e.server.c_str());
            ed_device = e.device=="npu";
            ed_preset=(int)(sizeof(PRESETS)/sizeof(PRESETS[0]))-1;
            for(int k=0;k<(int)(sizeof(PRESETS)/sizeof(PRESETS[0]));k++) if(*PRESETS[k].url && e.base_url==PRESETS[k].url){ ed_preset=k; break; }
            if(ed_kind==KIND_LLAMA) ed_preset=PRESET_FILE;
        } else if(idx==-2){ ed_preset=PRESET_FILE; ed_gguf[0]=0; ed_device=0;
            std::snprintf(ed_server,sizeof(ed_server),"%s",default_server().c_str()); ed_apply_preset(true); }
    }
    // llama-server.exe: next to this program, or on PATH.
    static std::string default_server(){
        std::error_code ec; std::string a=P("llama-server.exe"); if(std::filesystem::exists(std::filesystem::u8path(a),ec)) return a;
        wchar_t buf[MAX_PATH]={0}; if(SearchPathW(nullptr,L"llama-server.exe",nullptr,MAX_PATH,buf,nullptr)) return narrow(buf);
        return {}; }
    static bool browse(char* buf, size_t n, const wchar_t* filter){
        wchar_t fn[1040]={}; OPENFILENAMEW ofn{}; ofn.lStructSize=sizeof(ofn); ofn.lpstrFilter=filter;
        ofn.lpstrFile=fn; ofn.nMaxFile=1040; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(!GetOpenFileNameW(&ofn)) return false;
        std::snprintf(buf,n,"%s",narrow(fn).c_str()); return true; }
    void ed_apply_preset(bool name_too){
        const Preset& p=PRESETS[ed_preset];
        for(int k=0;k<KIND_COUNT;k++) if(std::string(p.kind)==KINDS[k]) ed_kind=k;
        std::snprintf(ed_url,sizeof(ed_url),"%s",p.url); std::snprintf(ed_model,sizeof(ed_model),"%s",p.model);
        if(name_too){ std::string n=p.label; size_t par=n.find(" ("); if(par!=std::string::npos && ed_preset>=6) n=n.substr(0,par);
            if(ed_preset==PRESET_FILE) n = *ed_gguf? file_name_of(ed_gguf) : "Local model";
            std::snprintf(ed_name,sizeof(ed_name),"%s",n.c_str()); }
    }
    // Writes the edited model to engines.json. The key field is encrypted and wiped; an empty field keeps the stored key.
    bool ed_save(){
        std::string name=ed_name; for(char& c: name) if(c=='"') c='\'';
        { size_t a=name.find_first_not_of(' '), b=name.find_last_not_of(' '); name = a==std::string::npos? std::string() : name.substr(a,b-a+1); }
        if(name.empty()){ ed_msg="Give the model a name."; ed_msg_kind=ST_ERR; return false; }
        if(name=="local"||name=="claude"){ ed_msg="That name is taken by a built-in model."; ed_msg_kind=ST_ERR; return false; }
        for(int k=0;k<(int)engines.size();k++) if(k!=ed_idx && engines[k].name==name){ ed_msg="Another model already has that name."; ed_msg_kind=ST_ERR; return false; }
        std::string url=ed_url; std::error_code fec;
        if(ed_kind==KIND_LLAMA){
            if(!*ed_gguf || !std::filesystem::exists(std::filesystem::u8path(ed_gguf),fec)){ ed_msg="Choose the model file (.gguf)."; ed_msg_kind=ST_ERR; return false; }
            if(!*ed_server || !std::filesystem::exists(std::filesystem::u8path(ed_server),fec)){ ed_msg="Choose llama-server.exe (it comes with llama.cpp)."; ed_msg_kind=ST_ERR; return false; }
        } else {
            if(url.rfind("http://",0)!=0 && url.rfind("https://",0)!=0){ ed_msg="The address must start with https:// (or http:// for a local server)."; ed_msg_kind=ST_ERR; return false; }
            if(!*ed_model){ ed_msg="Fill in the model id, as the provider writes it."; ed_msg_kind=ST_ERR; return false; }
        }
        cloud_llm::Engine e; std::string old_name;
        if(ed_idx>=0){ e=engines[ed_idx]; old_name=e.name; }
        e.name=name; e.kind=KINDS[ed_kind]; e.base_url=url; e.model=ed_model;
        e.gguf=ed_gguf; e.server=ed_server; e.device= ed_device? "npu" : "cpu";
        if(ed_kind==KIND_LLAMA){ e.base_url.clear(); e.model.clear(); e.key_enc.clear(); SecureZeroMemory(ed_key,sizeof(ed_key)); }
        if(*ed_key){ e.key_enc=cloud_llm::encrypt_key(ed_key); SecureZeroMemory(ed_key,sizeof(ed_key));
            if(e.key_enc.empty()){ ed_msg="Windows could not encrypt the key."; ed_msg_kind=ST_ERR; return false; } }
        if(ed_idx>=0) engines[ed_idx]=e; else { engines.push_back(e); ed_idx=(int)engines.size()-1; }
        if(!cloud_llm::save(engines_path,engines)){ ed_msg="Could not write engines.json."; ed_msg_kind=ST_ERR; return false; }
        if(!old_name.empty() && engine_sel=="cloud:"+old_name){ engine_sel="cloud:"+name; settings_save(); }
        std::snprintf(ed_name,sizeof(ed_name),"%s",name.c_str());
        ed_msg="Saved."; ed_msg_kind=ST_OK; return true;
    }
    void ed_remove(){
        if(ed_idx<0) return; std::string name=engines[ed_idx].name;
        engines.erase(engines.begin()+ed_idx); cloud_llm::save(engines_path,engines);
        if(engine_sel=="cloud:"+name){ engine_sel="local"; settings_save(); }
        ed_idx=-1; ed_msg.clear();
    }
    // One tiny request ("Reply with the single word OK") through the tailor - no personal data leaves.
    void ed_test(){
        if(test_busy.load() || ed_idx<0) return; if(test_worker.joinable()) test_worker.join();
        test_busy=true; ed_msg_kind=ST_BUSY;
        ed_msg = engines[ed_idx].kind=="llama"? "Loading the model and asking it\xE2\x80\xA6 this can take a minute." : "Asking the model\xE2\x80\xA6";
        std::string name=engines[ed_idx].name;
        test_worker=std::thread([this,name](){
            std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(P("work")),ec);
            std::filesystem::remove(std::filesystem::u8path(P("work\\cloud_test.txt")),ec);
            DWORD rc=run_wait("\""+P("pulse_cv_tailor.exe")+"\" --cloud-test \""+name+"\"",600000);
            std::string r; { std::ifstream f(std::filesystem::u8path(P("work\\cloud_test.txt"))); std::getline(f,r,'\0'); }
            bool ok = r.rfind("OK:",0)==0;
            if(r.empty()) r = rc==100? "pulse_cv_tailor.exe could not be started." : "No answer.";
            else r = ok? "It works \xE2\x80\x94 the model answered: "+r.substr(4) : r.substr(r.rfind("ERROR: ",0)==0?7:0);
            if(!r.empty() && !ok) r[0]=(char)toupper((unsigned char)r[0]);
            { std::lock_guard<std::mutex> g(mu); ed_msg=r; ed_msg_kind= ok? ST_OK : ST_ERR; }
            test_busy=false;
        });
    }

    // ── vault (merits) ──
    std::string vault_dir = docs_dir()+"\\vault";
    char vault_name[96]="", vault_doc[64000]="";
    std::vector<std::pair<std::string,uintmax_t>> vault_files;
    std::string vault_status; bool vault_loaded=false;
    void vault_refresh(){
        vault_loaded=true; vault_files.clear(); std::error_code ec; auto dir=std::filesystem::u8path(vault_dir);
        if(std::filesystem::exists(dir,ec))
            for(auto& e: std::filesystem::directory_iterator(dir,ec))
                if(e.path().extension()==".txt") vault_files.push_back({narrow(e.path().filename().wstring().c_str()), e.file_size(ec)});
        std::sort(vault_files.begin(),vault_files.end());
    }
    void vault_add(){
        std::string name=vault_name; if(name.empty()){ vault_status="Give the document a name first."; return; }
        for(char&c:name) if(c=='/'||c=='\\'||c==':') c='_';
        if(name.size()<4 || name.substr(name.size()-4)!=".txt") name+=".txt";
        std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(vault_dir),ec);
        std::string body=vault_doc; if(body.empty()){ vault_status="Nothing to save \xE2\x80\x94 paste the document's text."; return; }
        { std::ofstream(std::filesystem::u8path(vault_dir)/std::filesystem::u8path(name),std::ios::binary)<<body; }
        vault_status="Saved '"+name+"'. It is read at the next Generate.";
        vault_name[0]=0; vault_doc[0]=0; vault_refresh();
    }
    void vault_del(const std::string& name){ std::error_code ec; std::filesystem::remove(std::filesystem::u8path(vault_dir)/std::filesystem::u8path(name),ec);
        vault_status="Removed '"+name+"'."; vault_refresh(); }
    // pdftotext: next to the app, on PATH, or the copy that comes with Git for Windows.
    static std::string find_pdftotext(){
        std::error_code ec; std::string a=P("pdftotext.exe"); if(std::filesystem::exists(std::filesystem::u8path(a),ec)) return a;
        wchar_t buf[MAX_PATH]={0}; if(SearchPathW(nullptr,L"pdftotext.exe",nullptr,MAX_PATH,buf,nullptr)) return narrow(buf);
        const char* pf=std::getenv("ProgramFiles");
        for(const char* sub: {"\\Git\\clangarm64\\bin\\pdftotext.exe","\\Git\\mingw64\\bin\\pdftotext.exe"}){
            std::string c=std::string(pf?pf:"C:\\Program Files")+sub; if(std::filesystem::exists(std::filesystem::u8path(c),ec)) return c; }
        return {};
    }
    void vault_add_file(){
        wchar_t fnbuf[1040]={}; OPENFILENAMEW ofn{}; ofn.lStructSize=sizeof(ofn);
        ofn.lpstrFilter=L"CV documents (PDF/DOCX/TXT)\0*.pdf;*.docx;*.txt\0All files\0*.*\0";
        ofn.lpstrFile=fnbuf; ofn.nMaxFile=1040; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(!GetOpenFileNameW(&ofn)) return;
        std::filesystem::path p(fnbuf); std::string ext=narrow(p.extension().wstring().c_str()); for(char&c:ext) c=(char)tolower((unsigned char)c);
        std::string in=narrow(p.wstring().c_str()), fname=narrow(p.filename().wstring().c_str()), text; std::error_code ec;
        std::filesystem::path tmpp=std::filesystem::temp_directory_path()/L"pulse_cv_ingest.txt"; std::string tmp=narrow(tmpp.wstring().c_str());
        std::filesystem::remove(tmpp,ec);
        auto slurp=[&](const std::filesystem::path& f){ std::ifstream s(f,std::ios::binary); text.assign((std::istreambuf_iterator<char>(s)),{}); };
        if(ext==".txt") slurp(p);
        else if(ext==".pdf"){
            std::string tool=find_pdftotext();
            if(tool.empty()){ vault_status="PDF import needs pdftotext.exe (it comes with Git for Windows; or put it next to the app). Paste the text instead."; return; }
            run_wait("\""+tool+"\" -enc UTF-8 -nopgbrk \""+in+"\" \""+tmp+"\"",120000); slurp(tmpp);
        } else if(ext==".docx"){
            std::string scr=P("work\\docx2txt.ps1"); std::filesystem::create_directories(std::filesystem::u8path(P("work")),ec);
            { std::ofstream s(std::filesystem::u8path(scr)); s<<
R"PS(param([string]$in,[string]$out)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[System.IO.Compression.ZipFile]::OpenRead($in)
$entry=$zip.GetEntry('word/document.xml')
$sr=New-Object System.IO.StreamReader($entry.Open())
$xml=$sr.ReadToEnd(); $sr.Close(); $zip.Dispose()
$xml=$xml -replace '</w:p>',[Environment]::NewLine
$xml=$xml -replace '<[^>]+>',''
$xml=$xml -replace '&amp;','&' -replace '&lt;','<' -replace '&gt;','>' -replace '&quot;','"' -replace '&apos;',"'"
$enc=New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($out,$xml,$enc)
)PS"; }
            run_wait("powershell -NoProfile -ExecutionPolicy Bypass -File \""+scr+"\" \""+in+"\" \""+tmp+"\"",120000); slurp(tmpp);
        } else { vault_status="Unsupported file type: "+ext; return; }
        std::filesystem::remove(tmpp,ec);   // the extracted text holds the user's merits - no copy left in %TEMP%
        if(text.size()>=3 && (unsigned char)text[0]==0xEF&&(unsigned char)text[1]==0xBB&&(unsigned char)text[2]==0xBF) text.erase(0,3);
        std::snprintf(vault_name,sizeof(vault_name),"%s",narrow(p.stem().wstring().c_str()).c_str());
        std::snprintf(vault_doc,sizeof(vault_doc),"%s",text.c_str());
        vault_status = text.empty()? "No text found (a scanned, image-only PDF?). Paste the text instead."
                     : "Read '"+fname+"'. Look it through, then press Add to vault.";
    }

    // ── a link to the ad → role, company, ad text (ad_fetch.hpp: Platsbanken, or any site's JobPosting data) ──
    void cv_fetch(){
        if(cv_busy.load()) return; if(cv_fetch_worker.joinable()) cv_fetch_worker.join();
        cv_busy=true; set_status(ST_BUSY,"Reading the ad\xE2\x80\xA6");
        std::string url=cv_url;
        cv_fetch_worker=std::thread([this,url](){
            ad_fetch::Ad ad; std::string err;
            if(!ad_fetch::fetch(url,ad,err)){ set_status(ST_ERR,err); cv_busy=false; return; }
            { std::lock_guard<std::mutex> g(mu); f_role=ad.role; f_company=ad.company; f_ad=ad.text; fetched=true; }
            set_status(ad.how=="page"? ST_WARN : ST_OK,
                       ad.how=="page"? "That page has no structured ad, so its text was read as it stands \xE2\x80\x94 trim what is not the ad, and fill in role and company."
                                     : "Ad read. Check role, company and text, then Generate.");
            cv_busy=false;
        });
    }
    void cv_load_drafts(){
        auto rd=[](const std::string& p, char* buf, size_t n){ std::ifstream f(std::filesystem::u8path(p)); std::string s((std::istreambuf_iterator<char>(f)),{});
            std::string t; for(char c:s) if(c!='\r') t+=c; std::snprintf(buf,n,"%s",t.c_str()); };
        rd(P("work\\ingress.txt"), cv_ingress, sizeof(cv_ingress));
        rd(P("work\\brev.txt"),    cv_brev,    sizeof(cv_brev));
        rd(P("work\\interests.txt"), cv_interests, sizeof(cv_interests));
        rd(P("work\\strengths.txt"), cv_strengths, sizeof(cv_strengths));
    }
    // Called on the UI thread every frame: takes over what the workers left.
    void pump(){
        bool f=false; { std::lock_guard<std::mutex> g(mu); if(fetched){ fetched=false; f=true;
            if(!f_role.empty())    std::snprintf(cv_role,   sizeof(cv_role),   "%s", f_role.c_str());
            if(!f_company.empty()) std::snprintf(cv_company,sizeof(cv_company),"%s", f_company.c_str());
            if(!f_ad.empty())      std::snprintf(cv_ad,     sizeof(cv_ad),     "%s", f_ad.c_str()); } }
        (void)f;
        if(drafts_ready.exchange(false)) cv_load_drafts();
    }
    void cv_run(bool rerender){
        if(cv_busy.load()) return; if(cv_worker.joinable()) cv_worker.join();
        engine_settle();
        std::string role=cv_role, company=cv_company, ad=cv_ad, extra=cv_extra, ing=cv_ingress, brev=cv_brev, opening=cv_opening;
        std::string intr=cv_interests, str=cv_strengths, engine=engine_sel, elabel=engine_label(engine_sel);
        std::string lead; for(auto& t: cv_lead) lead+=t+"\n";
        std::string confirm;   // the sentences this ad asks for AND the user has ticked
        if(const lang::Pack* lp=pack_for(role+" "+ad)) for(auto& a: lang::asks_for(*lp,ad)) if(std::find(confirmed.begin(),confirmed.end(),a.id)!=confirmed.end()) confirm+=a.text+"\n";
        bool on_pc=false; if(engine.rfind("cloud:",0)==0){ cloud_llm::Engine ce; if(cloud_llm::find(engines,engine.substr(6),ce)) on_pc=ce.kind=="llama"; }
        cv_busy=true;
        set_status(ST_BUSY, rerender? "Rendering the PDFs again\xE2\x80\xA6"
                          : engine=="local"? "Writing on this PC\xE2\x80\xA6 this takes a minute or two."
                          : on_pc? "Writing on this PC with "+elabel+"\xE2\x80\xA6 the model is loaded first; this takes a few minutes."
                                           : "Writing with "+elabel+"\xE2\x80\xA6");
        settings_save();
        char ac[64]; std::snprintf(ac,sizeof(ac),"%.3f %.3f %.3f", cv_acc[0],cv_acc[1],cv_acc[2]); std::string accent=ac;
        std::string profile = cv_profile==1?"praktisk":cv_profile==2?"akademisk":"auto";
        cv_worker = std::thread([this,rerender,role,company,ad,extra,ing,brev,intr,str,engine,elabel,opening,accent,profile,on_pc,lead,confirm](){
            auto W=[](const std::string& p){ return std::filesystem::u8path(p); };
            std::error_code ec; std::filesystem::create_directories(W(P("work")),ec);
            auto q=[&](std::string s){ for(char&c:s) if(c=='"')c='\''; return s; };
            std::string cmd = "\""+P("pulse_cv_tailor.exe")+"\" \""+q(role)+"\" \""+q(company)+"\" --accent "+accent+" --profile "+profile+" --layout creative";
            if(rerender){
                { std::ofstream(W(P("work\\ingress.txt")))<<ing; std::ofstream(W(P("work\\brev.txt")))<<brev;
                  std::ofstream(W(P("work\\interests.txt")))<<intr; std::ofstream(W(P("work\\strengths.txt")))<<str; }
                cmd += " --from-drafts";
            } else {
                { std::ofstream(W(P("work\\career_ad.txt")))<<ad; }
                cmd += " --adfile \""+P("work\\career_ad.txt")+"\"";
                if(!extra.empty()) cmd += " --extra \""+q(extra)+"\"";
                { std::ofstream(W(P("work\\opening.txt")))<<opening;                 // this run
                  std::ofstream(W(docs_dir()+"\\letter_opening.txt"))<<opening; }     // remembered for next time
                if(!opening.empty()) cmd += " --opening \""+P("work\\opening.txt")+"\"";
                if(!lead.empty()){ std::ofstream(W(P("work\\lead.txt")),std::ios::binary)<<lead; cmd += " --lead \""+P("work\\lead.txt")+"\""; }
                if(!confirm.empty()){ std::ofstream(W(P("work\\confirmed.txt")),std::ios::binary)<<confirm; cmd += " --confirm \""+P("work\\confirmed.txt")+"\""; }
                if(engine=="claude") cmd += " --claude";
                else if(engine.rfind("cloud:",0)==0) cmd += " --cloud \""+q(engine.substr(6))+"\"";
            }
            std::filesystem::remove(W(P("work\\last_error.txt")),ec);   // fresh error slot per run
            DWORD exitCode = run_wait(cmd,on_pc?1500000:300000);         // 100 = the tailor did not even start
            if(exitCode==0){
                drafts_ready=true;   // only pull the fresh drafts on a real success - never show stale text as done
                std::string msg = rerender ? "Rendered again. The PDFs are in your Downloads folder."
                                           : "Done. The PDFs are in your Downloads folder \xE2\x80\x94 edit the texts and press Re-render if you like.";
                { std::string cl; std::ifstream cf(W(P("work\\cliches.txt"))); std::getline(cf,cl,'\0');
                  if(!cl.empty()) msg += " Clich\xC3\xA9s worth rewriting: "+cl; }
                { std::string ub; std::ifstream uf(W(P("work\\claims.txt"))); std::getline(uf,ub,'\0');
                  if(!ub.empty()) msg = "Check before you send: the texts mention "+ub+" \xE2\x80\x94 not found in your details or merits. "+msg; }
                std::string miss; { std::ifstream mf(W(P("work\\missing.txt"))); std::getline(mf,miss,'\0'); }
                if(!miss.empty()) msg = "The letter does not mention "+miss+", which you chose to build it on \xE2\x80\x94 add it by hand, or Generate again. "+msg;
                std::string spell; { std::ifstream sf(W(P("work\\spelling.txt"))); std::getline(sf,spell,'\0'); }
                if(!spell.empty()) msg = "Check the spelling under Your details: "+spell+" \xE2\x80\x94 it is printed on the CV. "+msg;
                { std::string ub; std::ifstream uf(W(P("work\\claims.txt"))); std::getline(uf,ub,'\0'); set_status(ub.empty() && miss.empty() && spell.empty()? ST_OK : ST_WARN,msg); }
            } else {
                std::string why; { std::ifstream ef(W(P("work\\last_error.txt"))); std::getline(ef,why,'\0'); }
                std::string msg;
                if(exitCode==100) msg="pulse_cv_tailor.exe could not be started (it belongs next to this program).";
                else if(exitCode==3) msg = why.empty()? "Claude Code is not available (not installed, or not logged in)." : why;
                else if(engine!="local" && !rerender) msg = elabel+" failed"+(why.empty()?std::string("."):": "+why);
                else msg = !why.empty()? why : exitCode==2
                    ? "Nothing was written \xE2\x80\x94 the NPU is probably busy. Close other AI apps and try again."
                    : "Writing failed \xE2\x80\x94 the NPU may be in use by another app. Close it and try again.";
                set_status(ST_ERR,msg+" No new PDF was written.");
            }
            cv_busy=false;
        });
    }

    // ── Your details: ONE CV. The practical/academic angle is chosen at generation. ──
    char pd_name[128]="", pd_addr[256]="", pd_phone[64]="", pd_email[128]="", pd_place[64]="";
    char pd_sum[2000]="", pd_exp[16000]="", pd_edu[8000]="", pd_qual[256]="", pd_int[512]="", pd_str[512]="", pd_pers[512]="";
    std::string pd_status; bool pd_loaded=false, pd_exists=false;
    std::string pd_snap, pd_saved_at;      // what is on disk (all fields joined) and when it was last saved here
    std::string pd_cat() const { std::string c; for(const char* b: {pd_name,pd_addr,pd_phone,pd_email,pd_place,pd_sum,pd_exp,pd_edu,pd_qual,pd_int,pd_str,pd_pers}){ c+=b; c+='\x1f'; } return c; }
    bool pd_dirty() const { return pd_loaded && pd_cat()!=pd_snap; }
    static std::string pd_trim(std::string s){ size_t a=s.find_first_not_of(" \t\r"); size_t b=s.find_last_not_of(" \t\r");
        return a==std::string::npos?std::string():s.substr(a,b-a+1); }
    static std::string place_from_address(const std::string& a){   // "Storgatan 1, 123 45 Stad" -> "Stad" (same rule as the tailor)
        size_t d=std::string::npos; for(size_t k=0;k<a.size();++k) if(a[k]>='0'&&a[k]<='9') d=k;
        if(d==std::string::npos) return {};
        std::string r=a.substr(d+1); size_t x=r.find_first_not_of(" ,\t"), y=r.find_last_not_of(" ,\t");
        return x==std::string::npos? std::string() : r.substr(x,y-x+1); }
    void pd_load(){ pd_load_file(); pd_snap=pd_cat(); pd_saved_at.clear(); }
    void pd_load_file(){
        pd_loaded=true;
        std::ifstream f(std::filesystem::u8path(docs_dir()+"\\cv_data.json"));
        if(!f){ pd_exists=false; pd_status="Fill in your details and press Save."; return; }
        pd_exists=true;
        try{ nlohmann::json j; f>>j;
            auto cp=[&](const char* k,char* b,size_t n){ std::snprintf(b,n,"%s", j.value(k,std::string()).c_str()); };
            cp("name",pd_name,sizeof(pd_name)); cp("address",pd_addr,sizeof(pd_addr)); cp("phone",pd_phone,sizeof(pd_phone)); cp("email",pd_email,sizeof(pd_email));
            cp("place",pd_place,sizeof(pd_place));
            if(!*pd_place) std::snprintf(pd_place,sizeof(pd_place),"%s",place_from_address(pd_addr).c_str());
            pd_sum[0]=pd_exp[0]=pd_edu[0]=pd_qual[0]=pd_int[0]=pd_str[0]=pd_pers[0]=0;
            nlohmann::json pr;
            if(j.contains("profiles")){ if(j["profiles"].contains("praktisk")) pr=j["profiles"]["praktisk"];
                                        else if(!j["profiles"].empty()) pr=j["profiles"].begin().value(); }
            if(pr.is_object()){
                std::snprintf(pd_sum,sizeof(pd_sum),"%s",pr.value("summary",std::string()).c_str());
                auto row=[](const nlohmann::json& e){ std::string d=e.value("date",std::string()), t=e.value("title",std::string()), o=e.value("org",std::string()), n=e.value("note",std::string());
                    if(o==t) o.clear();
                    std::string l=(d.empty() && o.empty() && n.empty())? t : d+" | "+t; if(!o.empty() || !n.empty()) l+=" | "+o; if(!n.empty()) l+=" | "+n; return l+"\n"; };
                std::string ex; for(auto& e:pr.value("experience",nlohmann::json::array())) ex+=row(e);
                std::snprintf(pd_exp,sizeof(pd_exp),"%s",ex.c_str());
                std::string ed; for(auto& e:pr.value("education",nlohmann::json::array())) ed+=row(e);
                std::snprintf(pd_edu,sizeof(pd_edu),"%s",ed.c_str());
                auto join=[&](const char* fld){ std::string o; for(auto& s:pr.value(fld,nlohmann::json::array())){ if(!o.empty())o+=", "; o+=s.get<std::string>(); } return o; };
                std::snprintf(pd_qual,sizeof(pd_qual),"%s",join("qualifications").c_str());
                std::snprintf(pd_int, sizeof(pd_int), "%s",join("interests").c_str());
                std::snprintf(pd_str, sizeof(pd_str), "%s",join("strengths").c_str());
                std::string ps; for(auto& t:pr.value("personality",nlohmann::json::array())) if(t.is_object()){ char b[48]; std::snprintf(b,sizeof(b),"%g",t.value("value",0.0)); ps+=t.value("label",std::string())+": "+b+"\n"; }
                std::snprintf(pd_pers,sizeof(pd_pers),"%s",ps.c_str());
            }
            pd_status.clear();
        }catch(...){ pd_status="cv_data.json could not be read."; }
    }
    // A line of Jobs/Education:  years | role | employer | what it involved   (the later fields may be left out;
    // one field alone is the role).
    static std::vector<std::array<std::string,4>> parse_rows(const char* buf){
        std::vector<std::array<std::string,4>> out; std::stringstream ss(buf); std::string line;
        while(std::getline(ss,line)){ line=pd_trim(line); if(line.empty()) continue;
            std::vector<std::string> f; std::string cur; for(char c: line){ if(c=='|' && f.size()<3){ f.push_back(pd_trim(cur)); cur.clear(); } else cur+=c; } f.push_back(pd_trim(cur));
            std::array<std::string,4> r;   // date, title, org, note
            if(f.size()==1) r[1]=f[0]; else { r[0]=f[0]; r[1]=f[1]; if(f.size()>2) r[2]=f[2]; if(f.size()>3) r[3]=f[3]; }
            if(!r[1].empty() || !r[0].empty()) out.push_back(r); }
        return out; }
    void pd_save(){
        auto rows=[&](const char* buf){ nlohmann::json arr=nlohmann::json::array();
            for(auto& f: parse_rows(buf)){ nlohmann::json r; r["date"]=f[0]; r["title"]=f[1]; r["org"]=f[2]; if(!f[3].empty()) r["note"]=f[3]; arr.push_back(r); }
            return arr; };
        auto csv=[&](const char* buf){ nlohmann::json arr=nlohmann::json::array(); std::string s=buf,cur;
            for(size_t i=0;i<=s.size();++i){ char c=(i<s.size())?s[i]:','; if(c==','){ std::string t=pd_trim(cur); if(!t.empty())arr.push_back(t); cur.clear(); } else cur+=c; } return arr; };
        nlohmann::json j;
        j["_note"]="Written by PulseX CV (Your details).";
        j["name"]=pd_name; j["address"]=pd_addr; j["phone"]=pd_phone; j["email"]=pd_email; j["place"]=pd_place;
        auto pers=[&](const char* buf){ nlohmann::json arr=nlohmann::json::array(); std::stringstream ss(buf); std::string line;
            while(std::getline(ss,line)){ line=pd_trim(line); if(line.empty()) continue;
                auto c=line.find_last_of(':'); if(c==std::string::npos) continue;
                std::string lab=pd_trim(line.substr(0,c)); double val=std::atof(pd_trim(line.substr(c+1)).c_str());
                if(!lab.empty()){ nlohmann::json t; t["label"]=lab; t["value"]=val; arr.push_back(t); } } return arr; };
        nlohmann::json pr;
        pr["summary"]=pd_sum; pr["experience"]=rows(pd_exp); pr["education"]=rows(pd_edu);
        pr["qualifications"]=csv(pd_qual); pr["interests"]=csv(pd_int); pr["strengths"]=csv(pd_str);
        pr["personality"]=pers(pd_pers);
        // Same data for both profiles; the model angles the wording at generation (--profile praktisk/akademisk/auto).
        { nlohmann::json a=pr; a["subtitle"]="Praktiskt CV"; j["profiles"]["praktisk"]=a; }
        { nlohmann::json a=pr; a["subtitle"]="Akademiskt CV"; j["profiles"]["akademisk"]=a; }
        std::ofstream o(std::filesystem::u8path(docs_dir()+"\\cv_data.json")); o<<j.dump(2);
        o.flush(); pd_exists=(bool)o;
        if(o){ pd_snap=pd_cat(); SYSTEMTIME t; GetLocalTime(&t); char b[16]; std::snprintf(b,sizeof(b),"%02d:%02d",t.wHour,t.wMinute); pd_saved_at=b; pd_status.clear(); }
        else pd_status="Could not write cv_data.json \xE2\x80\x94 nothing was saved.";
    }
    ~PulseCvApp(){ for(std::thread* t: {&cv_worker,&cv_fetch_worker,&test_worker}) if(t->joinable()) t->join();
        SecureZeroMemory(ed_key,sizeof(ed_key)); }
};

// ════════════════════════ pages ════════════════════════
enum { TAB_APPLY=0, TAB_DETAILS, TAB_MERITS, TAB_MODELS, TAB_COUNT };
static int g_tab = TAB_APPLY;
static bool g_details_dirty = false;   // unsaved changes on Your details (a dot on its tab)

static void status_dot(int kind){
    float h=ImGui::GetTextLineHeight(); ImVec2 p=ImGui::GetCursorScreenPos(); ImVec2 c(p.x+h*0.42f,p.y+h*0.54f); float r=h*0.24f;
    ImDrawList* dl=ImGui::GetWindowDrawList();
    if(kind==ST_BUSY){ float a=(float)ImGui::GetTime()*5.5f; dl->PathArcTo(c,r*1.25f,a,a+4.3f,24); dl->PathStroke(ImGui::GetColorU32(C_WARN),0,px(2.2f)); }
    else dl->AddCircleFilled(c,r,ImGui::GetColorU32(kind==ST_OK?C_OK:kind==ST_ERR?C_ERR:kind==ST_WARN?C_WARN:C_DIM),20);
    ImGui::Dummy(ImVec2(h*0.9f,h)); ImGui::SameLine(0,px(4));
}
static void status_text(int kind, const std::string& s){
    status_dot(kind);
    ImGui::PushStyleColor(ImGuiCol_Text, kind==ST_ERR? C_ERR : kind==ST_IDLE? C_DIM : C_TEXT);
    ImGui::TextWrapped("%s",s.c_str()); ImGui::PopStyleColor();
}
static void open_newest(const char* tag){
    std::filesystem::path best; std::filesystem::file_time_type bt{}; std::error_code ec; bool got=false;
    auto dir=std::filesystem::u8path(downloads_dir());
    if(std::filesystem::exists(dir,ec))
        for(auto& e: std::filesystem::directory_iterator(dir,ec)){
            std::wstring fn=e.path().filename().wstring(); std::wstring wt(tag,tag+strlen(tag));
            if(fn.find(wt)!=std::wstring::npos && e.path().extension()==L".pdf"){ auto t=e.last_write_time(ec);
                if(!got||t>bt){ bt=t; best=e.path(); got=true; } } }
    if(got) ShellExecuteW(nullptr,L"open",best.c_str(),nullptr,nullptr,SW_SHOW);
}

// "Build the letter on": which of the user's jobs and education this letter leads with. A title does not say what a
// job was, and the ad's words cannot rank it - the user can, in a few seconds.
static void draw_lead_picker(PulseCvApp& app){
    const auto jobs=PulseCvApp::parse_rows(app.pd_exp), edus=PulseCvApp::parse_rows(app.pd_edu);
    auto exists=[&](const std::string& t){ for(auto& r: jobs) if(r[1]==t) return true; for(auto& r: edus) if(r[1]==t) return true; return false; };
    for(size_t k=0;k<app.cv_lead.size();) if(!exists(app.cv_lead[k])) app.cv_lead.erase(app.cv_lead.begin()+k); else ++k;
    label("Build the letter on \xE2\x80\x94 choose the jobs and education it should lead with");
    std::string shown;
    if(app.cv_lead.empty()) shown="Automatic (click to choose yourself)";
    else for(size_t k=0;k<app.cv_lead.size();++k){ if(k) shown+="  \xC2\xB7  "; shown+=std::to_string(k+1)+" "+app.cv_lead[k]; }
    // a drop-down like the others on the page (with its arrow) - as a plain field nobody saw that it could be clicked
    ImGui::SetNextItemWidth(-1); push_combo();
    const bool open=ImGui::BeginCombo("##lead",shown.c_str(),ImGuiComboFlags_HeightLargest);
    pop_combo();
    if(open){
        hint("Tick what this letter should lead with, most important first. Nothing ticked: the app picks what matches the ad's words.");
        if(jobs.empty() && edus.empty()) ImGui::TextDisabled("Your jobs and education are listed here once they are filled in under Your details.");
        int id=0;
        ImGui::PushStyleColor(ImGuiCol_FrameBg,C_BTN); ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,C_BTN_H); ImGui::PushStyleColor(ImGuiCol_CheckMark,C_TEXT);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,px(6)); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(px(5),px(5)));
        auto list=[&](const char* head, const std::vector<std::array<std::string,4>>& rows){
            if(rows.empty()) return; ImGui::Dummy(ImVec2(0,px(2))); ImGui::PushFont(g_semi,0.0f); ImGui::TextUnformatted(head); ImGui::PopFont();
            for(auto& r: rows){ ImGui::PushID(id++);
                int at=-1; for(size_t k=0;k<app.cv_lead.size();++k) if(app.cv_lead[k]==r[1]) at=(int)k;
                bool on=at>=0;
                std::string text=r[1]+((r[2].empty()||r[2]==r[1])? std::string() : "  \xE2\x80\x94  "+r[2])+(r[0].empty()? std::string() : "  ("+r[0]+")");
                if(on) text=std::to_string(at+1)+".  "+text;
                if(ImGui::Checkbox(text.c_str(),&on)){ if(on) app.cv_lead.push_back(r[1]); else app.cv_lead.erase(app.cv_lead.begin()+at); }
                ImGui::PopID(); } };
        list("Jobs",jobs); list("Education",edus);
        ImGui::PopStyleVar(2); ImGui::PopStyleColor(3);
        ImGui::Dummy(ImVec2(0,px(2)));
        if(ImGui::Button("Automatic")){ app.cv_lead.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if(ImGui::Button("Done")) ImGui::CloseCurrentPopup();
        ImGui::EndCombo(); }
}

// "The ad asks for": the practical requirements of the ad (lang "asks", in the ad's own language), each as the sentence that would go into the
// letter. Only what the user ticks is written - the app does not turn the ad's wishes into claims about the user.
static void draw_asks_picker(PulseCvApp& app){
    static std::string seen_ad, seen_role; static std::vector<lang::Ask> asks;
    if(seen_ad!=app.cv_ad || seen_role!=app.cv_role){ seen_ad=app.cv_ad; seen_role=app.cv_role; asks.clear();
        if(const lang::Pack* lp=app.pack_for(seen_role+" "+seen_ad)) asks=lang::asks_for(*lp,seen_ad); }
    auto at=[&](const std::string& id){ for(size_t k=0;k<app.confirmed.size();++k) if(app.confirmed[k]==id) return (int)k; return -1; };
    int n=0; for(auto& a: asks) if(at(a.id)>=0) ++n;
    label("The ad asks for \xE2\x80\x94 tick what is true of you");
    const std::string shown = app.packs.empty()? std::string("The language files (lang\\sv.json, lang\\en.json) are missing next to the program")
                            : asks.empty()? std::string(*app.cv_ad? "Nothing practical found in this ad" : "Fetch or paste the ad first")
                            : n==0? std::to_string(asks.size())+" things found \xC2\xB7 nothing ticked (click to choose)"
                                  : std::to_string(n)+" of "+std::to_string(asks.size())+" ticked";
    ImGui::SetNextItemWidth(-1); push_combo();
    const bool open=ImGui::BeginCombo("##asks",shown.c_str(),ImGuiComboFlags_HeightLargest);
    pop_combo();
    if(open){
        hint("Each line is a sentence for your letter. Tick the ones you can stand for; the rest are left out. Your ticks are remembered for the next ad.");
        if(asks.empty()) ImGui::TextDisabled("Hours, languages, licences and the like are listed here when the ad names them.");
        ImGui::PushStyleColor(ImGuiCol_FrameBg,C_BTN); ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,C_BTN_H); ImGui::PushStyleColor(ImGuiCol_CheckMark,C_TEXT);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,px(6)); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(px(5),px(5)));
        int id=0;
        for(auto& a: asks){ ImGui::PushID(id++);
            const int k=at(a.id); bool on=k>=0;
            if(ImGui::Checkbox(a.text.c_str(),&on)){ if(on) app.confirmed.push_back(a.id); else app.confirmed.erase(app.confirmed.begin()+k); app.settings_save(); }
            ImGui::PopID(); }
        ImGui::PopStyleVar(2); ImGui::PopStyleColor(3);
        ImGui::Dummy(ImVec2(0,px(2)));
        if(ImGui::Button("Done")) ImGui::CloseCurrentPopup();
        ImGui::EndCombo(); }
}

static void draw_job(PulseCvApp& app){
    const ImGuiStyle& st=ImGui::GetStyle();
    ImGui::BeginDisabled(app.cv_busy.load());
    label("Link to the ad");
    const float bw=px(92);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-bw-st.ItemSpacing.x);
    ImGui::InputTextWithHint("##cvurl","Platsbanken or another job site \xE2\x80\x94 role, company and text fill in by themselves",app.cv_url,sizeof(app.cv_url));
    ImGui::SameLine(); if(ImGui::Button("Fetch",ImVec2(bw,0)) && *app.cv_url) app.cv_fetch();
    const float half=(ImGui::GetContentRegionAvail().x-st.ItemSpacing.x)*0.5f;
    Row rw;
    ImGui::BeginGroup(); label("Role");    ImGui::SetNextItemWidth(half); ImGui::InputText("##role",app.cv_role,sizeof(app.cv_role)); ImGui::EndGroup();
    rw.next(half);
    ImGui::BeginGroup(); label("Company"); ImGui::SetNextItemWidth(half); ImGui::InputText("##company",app.cv_company,sizeof(app.cv_company)); ImGui::EndGroup();
    if(!app.cv_opening_loaded){ app.cv_opening_loaded=true;
        std::ifstream of(std::filesystem::u8path(docs_dir()+"\\letter_opening.txt")); std::string s((std::istreambuf_iterator<char>(of)),{});
        if(s.empty() || s=="Anledningen till att jag skriver till er \xC3\xA4r att jag s\xC3\xA5g er annons om {roll} som v\xC3\xA4" "ckte mitt intresse.") s = "Anledningen till att jag skriver till er \xC3\xA4r att jag s\xC3\xA5g er annons om {roll}, {detalj}.";   // old default -> detail slot (09-29)
        std::snprintf(app.cv_opening,sizeof(app.cv_opening),"%s",s.c_str()); }
    static const char* OPEN_HINT="{roll} = the role \xC2\xB7 {f\xC3\xB6retag} = the company \xC2\xB7 {detalj} = one real detail from the ad \xC2\xB7 leave empty for a free opening";
    label("Job ad");
    const float below = 2*(label_h()+lines_h(2)+st.ItemSpacing.y) + 2*(label_h()+ImGui::GetFrameHeight()+st.ItemSpacing.y)
                      + hint_h(OPEN_HINT,ImGui::GetContentRegionAvail().x) + px(4);
    text_box("##cvad",app.cv_ad,sizeof(app.cv_ad),std::max(px(110),ImGui::GetContentRegionAvail().y-below-st.ItemSpacing.y));
    draw_lead_picker(app);
    draw_asks_picker(app);
    label("Also worth mentioning (optional)");
    text_box("##cvextra",app.cv_extra,sizeof(app.cv_extra),lines_h(2));
    label("How the letter opens");
    text_box("##cvopening",app.cv_opening,sizeof(app.cv_opening),lines_h(2));
    hint(OPEN_HINT);
    ImGui::EndDisabled();
}

static void draw_texts(PulseCvApp& app){
    const ImGuiStyle& st=ImGui::GetStyle();
    const bool busy=app.cv_busy.load();
    const bool empty = !*app.cv_ingress && !*app.cv_brev && !*app.cv_interests && !*app.cv_strengths;
    const float row_h=ImGui::GetFrameHeight();
    if(empty){
        const char* a="Nothing written yet";
        const char* b="Fill in the job on the left and press Generate. The CV summary and the cover letter land here \xE2\x80\x94 "
                      "change anything you like, then Re-render to get new PDFs.";
        ImVec2 av=ImGui::GetContentRegionAvail(); float w=std::min(av.x,px(420));
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX()+(av.x-w)*0.5f, ImGui::GetCursorPosY()+av.y*0.30f));
        ImGui::BeginGroup();
        ImGui::PushFont(g_semi,0.0f); ImGui::TextUnformatted(a); ImGui::PopFont();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+w); ImGui::PushStyleColor(ImGuiCol_Text,C_DIM); ImGui::TextUnformatted(b); ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        return;
    }
    label("CV summary");
    text_box("##cving",app.cv_ingress,sizeof(app.cv_ingress),lines_h(4));
    label("Cover letter");
    const float below = 2*(label_h()+row_h+st.ItemSpacing.y) + row_h + st.ItemSpacing.y + px(4);
    text_box("##cvbrev",app.cv_brev,sizeof(app.cv_brev),std::max(px(140),ImGui::GetContentRegionAvail().y-below));
    label("Interests on the CV (comma-separated)");
    ImGui::SetNextItemWidth(-1); ImGui::InputText("##cvint",app.cv_interests,sizeof(app.cv_interests));
    label("Strengths on the CV (comma-separated)");
    ImGui::SetNextItemWidth(-1); ImGui::InputText("##cvstr",app.cv_strengths,sizeof(app.cv_strengths));
    ImGui::BeginDisabled(busy);
    if(ImGui::Button("Re-render PDFs")) app.cv_run(true);
    ImGui::EndDisabled();
    const float w3 = ImGui::CalcTextSize("Open CV").x+ImGui::CalcTextSize("Open letter").x+ImGui::CalcTextSize("Folder").x + 6*st.FramePadding.x + 2*st.ItemSpacing.x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x-w3);
    if(ImGui::Button("Open CV")) open_newest("_CV");
    ImGui::SameLine(); if(ImGui::Button("Open letter")) open_newest("_PB");
    ImGui::SameLine(); if(ImGui::Button("Folder")) shell_open(downloads_dir());
}

// Bottom bar of the Application page: what is going on, and the choices that shape a generation.
static void draw_action_bar(PulseCvApp& app, bool wide){
    const ImGuiStyle& st=ImGui::GetStyle();
    const bool busy=app.cv_busy.load();
    std::string s; int kind; { std::lock_guard<std::mutex> g(app.mu); s=app.status; kind=app.status_kind; }
    app.engine_settle();
    const bool nomodel=app.no_model();
    if(kind==ST_IDLE){   // nothing has happened yet: say where the text will be written
        if(nomodel) s="No model is chosen yet. Have a look around \xE2\x80\x94 when you want a letter, pick who writes it under Written by, or add a model under Models.";
        else if(app.engine_sel=="local") s="Ready. The texts are written on this PC \xE2\x80\x94 nothing leaves it.";
        else if(app.engine_sel=="claude") s="Ready. Claude Code writes with your own Claude login: your merits and the ad are sent to Anthropic.";
        else { cloud_llm::Engine e; cloud_llm::find(app.engines,app.engine_sel.substr(6),e);
            s = engine_is_local(e) ? "Ready. "+e.name+" runs on this PC \xE2\x80\x94 nothing leaves it."
                                          : "Ready. "+e.name+" is a cloud model: your merits and the ad are sent to "+host_of(e.base_url)+"."; } }
    const float wProf=px(138), wEng=px(250), wGen=px(214), sw=ImGui::GetFrameHeight();
    const float controls = wProf+sw+wEng+wGen+3*st.ItemSpacing.x;
    if(wide){
        ImGui::BeginChild("##status",ImVec2(ImGui::GetContentRegionAvail().x-controls-px(18),0),0,ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY()+px(3)); status_text(kind,s);
        ImGui::EndChild(); ImGui::SameLine(0,px(18));
    } else { status_text(kind,s); }
    ImGui::BeginDisabled(busy);
    Row rw; push_combo();
    ImGui::BeginGroup(); label("CV angle"); ImGui::SetNextItemWidth(wProf);
    if(ImGui::Combo("##cvprofile",&app.cv_profile,"Auto\0Practical\0Academic\0")) app.settings_save();
    ImGui::EndGroup(); rw.next(wProf);
    ImGui::BeginGroup(); label("Colour");
    if(ImGui::ColorEdit3("##color",app.cv_acc,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel)) app.settings_save();
    ImGui::EndGroup(); rw.next(sw);
    ImGui::BeginGroup(); label("Written by"); ImGui::SetNextItemWidth(wEng);
    if(ImGui::BeginCombo("##cvengine",app.engine_label(app.engine_sel).c_str())){
        auto item=[&](const std::string& sel, const std::string& text, const char* note){
            bool on=app.engine_sel==sel;
            if(ImGui::Selectable((text+"##"+sel).c_str(),on)){ app.engine_sel=sel; app.settings_save(); { std::lock_guard<std::mutex> g(app.mu); if(app.status_kind!=ST_BUSY){ app.status_kind=ST_IDLE; app.status.clear(); } } }
            const ImVec2 r0=ImGui::GetItemRectMin(), r1=ImGui::GetItemRectMax(); const float fs=FS*0.84f*g_scale;   // the note, right-aligned in the row
            const float tw=g_font->CalcTextSizeA(fs,FLT_MAX,0.0f,note).x;
            ImGui::GetWindowDrawList()->AddText(g_font,fs,ImVec2(r1.x-tw-px(4),r0.y+(r1.y-r0.y-fs)*0.5f),ImGui::GetColorU32(C_DIM),note); };
        if(app.qwen_here()) item("local","Qwen3-4B on this PC","private");
        else { ImGui::BeginDisabled(); item("local","Qwen3-4B on this PC","not installed"); ImGui::EndDisabled(); }
        item("claude","Claude Code","your login");
        for(auto& e: app.engines) item("cloud:"+e.name, e.name, engine_is_local(e)?"this PC":"cloud");
        ImGui::Separator();
        if(ImGui::Selectable("Add a model\xE2\x80\xA6")){ g_tab=TAB_MODELS; app.ed_open(-2); }
        ImGui::EndCombo(); }
    ImGui::EndGroup(); pop_combo();
    if(wide) rw.next(wEng);
    ImGui::BeginGroup(); if(wide) ImGui::Dummy(ImVec2(0,label_h()-st.ItemSpacing.y));
    if(primary_button("Generate CV + letter",ImVec2(wide? wGen : ImGui::GetContentRegionAvail().x,0))){
        // without a model no run can work: say where one is chosen, and leave the user where they are
        if(nomodel) app.set_status(ST_WARN,"A letter needs a model to write it. Pick one under Written by, or add your own under Models \xE2\x80\x94 a model file on this PC, Claude Code or a cloud model.");
        else app.cv_run(false); }
    ImGui::EndGroup();
    ImGui::EndDisabled();
}

static void draw_apply(PulseCvApp& app){
    const ImGuiStyle& st=ImGui::GetStyle(); const float gap=px(14);
    ImVec2 av=ImGui::GetContentRegionAvail();
    const bool wide = av.x >= px(980);
    // wide: status (up to three lines) beside the controls; narrow: status above them
    const float bar_h = (wide? std::max(label_h()+ImGui::GetFrameHeight(), ImGui::GetTextLineHeight()*3+px(4))
                             : ImGui::GetTextLineHeight()*2+label_h()+2*ImGui::GetFrameHeight()+2*st.ItemSpacing.y) + px(32);
    float top=0;
    if(!app.vault_loaded) app.vault_refresh();
    if(!app.pd_loaded) app.pd_load();
    const bool nomodel=app.no_model();
    if(app.vault_files.empty() || !app.pd_exists || nomodel){   // first run: say what is missing, once, with a way there
        begin_card("##first",ImVec2(0,0),nullptr,ImGuiChildFlags_AutoResizeY);
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(g_semi,0.0f); ImGui::TextUnformatted("Before the first letter"); ImGui::PopFont();
        ImGui::SameLine(0,px(14)); ImGui::AlignTextToFramePadding();
        { std::vector<const char*> todo;
          if(!app.pd_exists) todo.push_back("fill in who you are");
          if(app.vault_files.empty()) todo.push_back("add your old CV as merits");
          if(nomodel) todo.push_back("choose the model that writes");
          std::string t; for(size_t k=0;k<todo.size();++k){ if(k) t += (k+1==todo.size()? " and " : ", "); t+=todo[k]; } t+='.';
          ImGui::TextDisabled("%s", t.c_str()); }
        if(!app.pd_exists){ ImGui::SameLine(); if(ImGui::Button("Your details")) g_tab=TAB_DETAILS; }
        if(app.vault_files.empty()){ ImGui::SameLine(); if(ImGui::Button("Add merits")) g_tab=TAB_MERITS; }
        if(nomodel){ ImGui::SameLine(); if(ImGui::Button("Choose a model")){ g_tab=TAB_MODELS; app.ed_open(-2); } }
        end_card();
        top = ImGui::GetItemRectSize().y + gap; vgap(gap);
    }
    const float body_h = std::max(px(300), av.y-bar_h-gap-top);
    if(wide){
        const float cw=(av.x-gap)*0.5f;
        // everything fits from about 690 px of card height; below that the card scrolls instead of cutting fields off
        const ImGuiWindowFlags cf = body_h>=px(745)? (ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse) : 0;
        begin_card("##job",ImVec2(cw,body_h),"The job",0,cf); draw_job(app); end_card();
        ImGui::SameLine(0,gap);
        begin_card("##texts",ImVec2(cw,body_h),"Your texts",0,cf); draw_texts(app); end_card();
    } else {   // narrow window: the two cards stack and the page scrolls
        ImGui::BeginChild("##stack",ImVec2(0,body_h));
        begin_card("##job",ImVec2(0,px(640)),"The job",0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse); draw_job(app); end_card();
        vgap(gap);
        begin_card("##texts",ImVec2(0,px(680)),"Your texts",0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse); draw_texts(app); end_card();
        ImGui::EndChild();
    }
    vgap(gap);
    begin_card("##bar",ImVec2(0,bar_h),nullptr,0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse); draw_action_bar(app,wide); end_card();
}

static void draw_details(PulseCvApp& app){
    const ImGuiStyle& st=ImGui::GetStyle(); const float gap=px(14);
    if(!app.pd_loaded) app.pd_load();
    ImVec2 av=ImGui::GetContentRegionAvail();
    const bool wide = av.x >= px(980);
    const float bar_h = ImGui::GetFrameHeight()+px(30)+px(2);
    const float body_h = av.y-bar_h-gap;
    auto field=[&](const char* cap, const char* id, char* buf, size_t n, float w=-1){ ImGui::BeginGroup(); label(cap); ImGui::SetNextItemWidth(w); ImGui::InputText(id,buf,n); ImGui::EndGroup(); };
    auto about=[&](){
        field("Name","##pdname",app.pd_name,sizeof(app.pd_name));
        field("Address","##pdaddr",app.pd_addr,sizeof(app.pd_addr));
        const float third=(ImGui::GetContentRegionAvail().x-2*st.ItemSpacing.x)/3;
        Row rw;
        field("Phone","##pdphone",app.pd_phone,sizeof(app.pd_phone),third); rw.next(third);
        field("Email","##pdemail",app.pd_email,sizeof(app.pd_email),third); rw.next(third);
        field("Town (dates the letter)","##pdplace",app.pd_place,sizeof(app.pd_place),third);
        label("Summary"); text_box("##pdsum",app.pd_sum,sizeof(app.pd_sum),lines_h(4));
        field("Licences and permits (comma-separated)","##pdqual",app.pd_qual,sizeof(app.pd_qual));
        field("Interests (comma-separated)","##pdint",app.pd_int,sizeof(app.pd_int));
        field("Strengths (comma-separated \xE2\x80\x94 best taken from a personality test)","##pdstr",app.pd_str,sizeof(app.pd_str));
        label("Personality, drawn as bars \xE2\x80\x94 one per line:  trait: 1\xE2\x80\x93" "10");
        ImGui::InputTextMultiline("##pdpers",app.pd_pers,sizeof(app.pd_pers),ImVec2(-1,std::max(lines_h(3),ImGui::GetContentRegionAvail().y-px(2))));
    };
    auto history=[&](float h){
        static const char* NOTE_HINT="The last part is for the model, not for the printed CV: what the job really was. "
            "A title hides it \xE2\x80\x94 budget, people, results, what you had to be good at.";
        const float hh=hint_h(NOTE_HINT,ImGui::GetContentRegionAvail().x)+st.ItemSpacing.y;
        const float each=std::max(lines_h(4),(h - 2*label_h() - st.ItemSpacing.y - hh)*0.5f);
        label("Jobs \xE2\x80\x94 one per line:  years | role | employer | what it involved");
        ImGui::InputTextMultiline("##pdexp",app.pd_exp,sizeof(app.pd_exp),ImVec2(-1,each*1.25f),ImGuiInputTextFlags_WordWrap);
        hint(NOTE_HINT);
        label("Education \xE2\x80\x94 one per line:  years | qualification | school | what it covered");
        ImGui::InputTextMultiline("##pdedu",app.pd_edu,sizeof(app.pd_edu),ImVec2(-1,std::max(lines_h(3),ImGui::GetContentRegionAvail().y-px(2))),ImGuiInputTextFlags_WordWrap);
    };
    if(wide){
        const float cw=(av.x-gap)*0.5f;
        begin_card("##about",ImVec2(cw,body_h),"About you"); about(); end_card();
        ImGui::SameLine(0,gap);
        begin_card("##hist",ImVec2(cw,body_h),"Work and education"); history(ImGui::GetContentRegionAvail().y); end_card();
    } else {
        ImGui::BeginChild("##stack",ImVec2(0,body_h));
        begin_card("##about",ImVec2(0,px(720)),"About you"); about(); end_card();
        vgap(gap);
        begin_card("##hist",ImVec2(0,px(560)),"Work and education"); history(ImGui::GetContentRegionAvail().y); end_card();
        ImGui::EndChild();
    }
    vgap(gap);
    begin_card("##bar",ImVec2(0,bar_h),nullptr,0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    const bool dirty=app.pd_dirty();
    if(dirty){ if(primary_button("Save",ImVec2(px(120),0))) app.pd_save(); }
    else { ImGui::BeginDisabled(); ImGui::Button("Saved",ImVec2(px(120),0)); ImGui::EndDisabled(); }   // the button itself says it
    ImGui::SameLine(); ImGui::BeginDisabled(!dirty); if(ImGui::Button("Undo changes")) app.pd_load(); ImGui::EndDisabled();
    ImGui::SameLine(0,px(16)); ImGui::SetCursorPosY(ImGui::GetCursorPosY()+ImGui::GetStyle().FramePadding.y);
    if(!app.pd_status.empty()) status_text(ST_ERR,app.pd_status);
    else if(dirty) status_text(ST_WARN,"Unsaved changes \xE2\x80\x94 press Save to put them on the CV.");
    else if(!app.pd_saved_at.empty()) status_text(ST_OK,"Saved "+app.pd_saved_at+". This is what the CV prints.");
    else status_text(ST_IDLE, app.pd_exists? "Everything here is saved. The CV prints this; the wording is angled for each ad."
                                           : "Fill in your details and press Save.");
    end_card();
}

static void draw_merits(PulseCvApp& app){
    const ImGuiStyle& st=ImGui::GetStyle(); const float gap=px(14);
    if(!app.vault_loaded) app.vault_refresh();
    ImVec2 av=ImGui::GetContentRegionAvail();
    const bool wide = av.x >= px(980);
    const float row_h=ImGui::GetFrameHeight();
    auto list=[&](){
        hint("Old CVs, certificates, references, a personality test. For each ad only the passages that fit are picked out. "
             "The documents stay on this PC.");
        ImGui::Dummy(ImVec2(0,px(2)));
        ImGui::BeginChild("##files",ImVec2(0,ImGui::GetContentRegionAvail().y-row_h-st.ItemSpacing.y));
        if(app.vault_files.empty()){ ImGui::Dummy(ImVec2(0,px(8))); ImGui::TextDisabled("Nothing here yet. Import a file, or paste a text on the right."); }
        for(auto& f: app.vault_files){
            ImGui::PushID(f.first.c_str());
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(f.first.c_str());
            char sz[32]; std::snprintf(sz,sizeof(sz),"%.1f kB",f.second/1024.0);
            const float rw=ImGui::CalcTextSize("Remove").x+2*st.FramePadding.x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x-rw-st.ItemSpacing.x-ImGui::CalcTextSize(sz).x); ImGui::TextDisabled("%s",sz);
            ImGui::SameLine(ImGui::GetContentRegionMax().x-rw);
            bool del=ImGui::Button("Remove");
            ImGui::PopID();
            if(del){ app.vault_del(f.first); break; }
        }
        ImGui::EndChild();
        if(primary_button("Import a file\xE2\x80\xA6")) app.vault_add_file();
        ImGui::SameLine(); ImGui::TextDisabled("PDF, DOCX or TXT");
        const float ow=ImGui::CalcTextSize("Open folder").x+2*st.FramePadding.x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x-ow); if(ImGui::Button("Open folder")){ std::error_code ec; std::filesystem::create_directories(std::filesystem::u8path(app.vault_dir),ec); shell_open(app.vault_dir); }
    };
    auto add=[&](){
        label("Name"); ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##vname","for example cv_2024",app.vault_name,sizeof(app.vault_name));
        label("Text");
        const float sh = app.vault_status.empty()? 0 : hint_h(app.vault_status.c_str(),ImGui::GetContentRegionAvail().x)+st.ItemSpacing.y;
        ImGui::InputTextMultiline("##vdoc",app.vault_doc,sizeof(app.vault_doc),ImVec2(-1,std::max(lines_h(4),ImGui::GetContentRegionAvail().y-row_h-st.ItemSpacing.y-sh)),ImGuiInputTextFlags_WordWrap);
        if(!app.vault_status.empty()) hint(app.vault_status.c_str());
        ImGui::BeginDisabled(!*app.vault_doc);
        if(ImGui::Button("Add to vault")) app.vault_add();
        ImGui::EndDisabled();
    };
    if(wide){
        const float cw=(av.x-gap)*0.5f;
        begin_card("##vault",ImVec2(cw,av.y),"Your merits"); list(); end_card();
        ImGui::SameLine(0,gap);
        begin_card("##vadd",ImVec2(cw,av.y),"Add a document"); add(); end_card();
    } else {
        ImGui::BeginChild("##stack",ImVec2(0,av.y));
        begin_card("##vault",ImVec2(0,px(420)),"Your merits"); list(); end_card();
        vgap(gap);
        begin_card("##vadd",ImVec2(0,px(520)),"Add a document"); add(); end_card();
        ImGui::EndChild();
    }
}

static void draw_models(PulseCvApp& app){
    const ImGuiStyle& st=ImGui::GetStyle(); const float gap=px(14);
    ImVec2 av=ImGui::GetContentRegionAvail();
    const bool wide = av.x >= px(980);
    const float row_h=ImGui::GetFrameHeight();
    auto row=[&](const char* id, const std::string& name, const std::string& note, bool on)->bool{
        ImGui::PushID(id);
        const float h=ImGui::GetTextLineHeight()+FS*0.84f*g_scale+px(14);
        ImVec2 p=ImGui::GetCursorScreenPos();
        bool hit=ImGui::InvisibleButton("##row",ImVec2(std::max(px(40),ImGui::GetContentRegionAvail().x),h));
        ImDrawList* dl=ImGui::GetWindowDrawList();
        if(on || ImGui::IsItemHovered())
            dl->AddRectFilled(p,ImVec2(p.x+ImGui::GetItemRectSize().x,p.y+h),
                              ImGui::GetColorU32(on? ImVec4(C_ACC.x,C_ACC.y,C_ACC.z,0.30f) : ImVec4(1,1,1,0.05f)),px(12));
        dl->AddText(g_semi,FS*g_scale,ImVec2(p.x+px(12),p.y+px(5)),ImGui::GetColorU32(C_TEXT),name.c_str());
        dl->AddText(g_font,FS*0.84f*g_scale,ImVec2(p.x+px(12),p.y+px(5)+ImGui::GetTextLineHeight()+px(1)),ImGui::GetColorU32(C_DIM),note.c_str());
        ImGui::PopID(); return hit; };
    auto list=[&](){
        hint("Who writes the summary and the letter. Pick one per application, at the bottom of the Application page.");
        ImGui::Dummy(ImVec2(0,px(2)));
        ImGui::BeginChild("##models",ImVec2(0,ImGui::GetContentRegionAvail().y-row_h-st.ItemSpacing.y));
        if(row("local","Qwen3-4B on this PC",app.qwen_here()? "Built in \xC2\xB7 runs on the NPU \xC2\xB7 nothing leaves the PC" : "Not installed \xC2\xB7 the model bundle is not on this PC",
               app.ed_idx==-1&&app.ed_builtin==0)){ app.ed_idx=-1; app.ed_builtin=0; }
        if(row("claude","Claude Code","Built in \xC2\xB7 your own Claude login \xC2\xB7 no API key",app.ed_idx==-1&&app.ed_builtin==1)){ app.ed_idx=-1; app.ed_builtin=1; }
        for(int k=0;k<(int)app.engines.size();k++){ auto& e=app.engines[k];
            std::string note = e.kind=="llama"? "This PC \xC2\xB7 "+file_name_of(e.gguf)+(e.device=="npu"?" \xC2\xB7 NPU":" \xC2\xB7 processor")
                : (is_local_host(e.base_url)? std::string("This PC") : host_of(e.base_url))+" \xC2\xB7 "+e.model+(e.key_enc.empty()&&!is_local_host(e.base_url)?" \xC2\xB7 no key yet":"");
            if(row(("c"+std::to_string(k)).c_str(),e.name,note,app.ed_idx==k)) app.ed_open(k); }
        ImGui::EndChild();
        if(primary_button("Add a model\xE2\x80\xA6")) app.ed_open(-2);
    };
    auto editor=[&](){
        if(app.ed_idx==-1){
            if(app.ed_builtin==0 && !app.qwen_here()){
                ImGui::TextWrapped("This engine needs a Qwen3-4B model bundle for Qualcomm's Genie runtime, and it is not on this PC. "
                                   "Put the bundle in models\\qwen3-4b-4k\\genie_bundle next to the program, or use another model.");
                ImGui::Dummy(ImVec2(0,px(4)));
                hint("The quickest way to a model on this PC: press Add a model and point at a GGUF file and llama-server.exe.");
            } else if(app.ed_builtin==0){
                ImGui::TextWrapped("The model that comes with PulseX CV. It runs on the Snapdragon NPU of this PC, so your merits and "
                                   "the ad never leave it. It reads about 4,000 tokens at a time: long ads and long CVs are trimmed to fit.");
                ImGui::Dummy(ImVec2(0,px(4)));
                hint("Nothing to set up. Close other AI apps that use the NPU while it writes.");
            } else {
                ImGui::TextWrapped("Claude Sonnet through the Claude Code program on this PC, with your own Claude login \xE2\x80\x94 no API key is "
                                   "stored here. Your merits and the ad are sent to Anthropic.");
                ImGui::Dummy(ImVec2(0,px(4)));
                hint("Needs Claude Code installed and logged in: open a terminal, run claude and type /login once.");
            }
            ImGui::Dummy(ImVec2(0,px(10)));
            ImGui::TextDisabled("Any other model that writes text can be added: press Add a model.");
            return;
        }
        const bool busy=app.test_busy.load();
        ImGui::BeginDisabled(busy);
        label("Provider"); ImGui::SetNextItemWidth(-1); push_combo();
        if(ImGui::BeginCombo("##preset",PRESETS[app.ed_preset].label)){
            for(int k=0;k<(int)(sizeof(PRESETS)/sizeof(PRESETS[0]));k++)
                if(ImGui::Selectable(PRESETS[k].label,k==app.ed_preset)){ app.ed_preset=k; app.ed_apply_preset(app.ed_idx==-2); }
            ImGui::EndCombo(); }
        pop_combo();
        const float half=(ImGui::GetContentRegionAvail().x-st.ItemSpacing.x)*0.5f;
        Row rw;
        ImGui::BeginGroup(); label("Name in the list"); ImGui::SetNextItemWidth(half); ImGui::InputText("##edname",app.ed_name,sizeof(app.ed_name)); ImGui::EndGroup();
        rw.next(half);
        ImGui::BeginGroup(); label("Speaks"); ImGui::SetNextItemWidth(half); push_combo(); ImGui::Combo("##edkind",&app.ed_kind,KIND_LABEL,KIND_COUNT); pop_combo(); ImGui::EndGroup();
        if(app.ed_kind==KIND_LLAMA){
            const float bw=px(104);
            label("Model file (.gguf)"); ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-bw-st.ItemSpacing.x);
            ImGui::InputTextWithHint("##edgguf","a chat model in GGUF format",app.ed_gguf,sizeof(app.ed_gguf),ImGuiInputTextFlags_ElideLeft);
            ImGui::SameLine(); if(ImGui::Button("Browse\xE2\x80\xA6##g",ImVec2(bw,0)) && PulseCvApp::browse(app.ed_gguf,sizeof(app.ed_gguf),L"GGUF models\0*.gguf\0All files\0*.*\0")
                                  && (!*app.ed_name || !strcmp(app.ed_name,"Local model"))) std::snprintf(app.ed_name,sizeof(app.ed_name),"%s",file_name_of(app.ed_gguf).c_str());
            label("llama-server.exe (from llama.cpp)"); ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-bw-st.ItemSpacing.x);
            ImGui::InputTextWithHint("##edsrv","the program that runs the model",app.ed_server,sizeof(app.ed_server),ImGuiInputTextFlags_ElideLeft);
            ImGui::SameLine(); if(ImGui::Button("Browse\xE2\x80\xA6##s",ImVec2(bw,0))) PulseCvApp::browse(app.ed_server,sizeof(app.ed_server),L"llama-server\0llama-server.exe\0Programs\0*.exe\0");
            label("Runs on"); ImGui::SetNextItemWidth(half); push_combo();
            ImGui::Combo("##eddev",&app.ed_device,"The processor (works everywhere)\0The Snapdragon NPU (needs an NPU build of llama.cpp)\0"); pop_combo();
        } else {
        label("Address"); ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##edurl","https://\xE2\x80\xA6  (the provider's API address)",app.ed_url,sizeof(app.ed_url));
        label("Model"); ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##edmodel","the model id, exactly as the provider writes it",app.ed_model,sizeof(app.ed_model));
        const bool has_key = app.ed_idx>=0 && !app.engines[app.ed_idx].key_enc.empty();
        label("API key"); ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##edkey", has_key? "A key is stored \xE2\x80\x94 type a new one to replace it"
                                 : is_local_host(app.ed_url)? "Not needed for a server on this PC" : "Paste the key from the provider",
                                 app.ed_key,sizeof(app.ed_key),ImGuiInputTextFlags_Password);
        }
        ImGui::Dummy(ImVec2(0,px(2)));
        if(primary_button("Save",ImVec2(px(110),0))) app.ed_save();
        ImGui::SameLine(); if(ImGui::Button("Test",ImVec2(px(90),0))){ if(app.ed_save()) app.ed_test(); }
        if(app.ed_idx>=0){ const float rw=ImGui::CalcTextSize("Remove").x+2*st.FramePadding.x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x-rw); if(ImGui::Button("Remove")) app.ed_remove(); }
        ImGui::EndDisabled();
        std::string m; int mk; { std::lock_guard<std::mutex> g(app.mu); m=app.ed_msg; mk=app.ed_msg_kind; }
        if(!m.empty()){ ImGui::Dummy(ImVec2(0,px(2))); status_text(mk,m); }
        ImGui::Dummy(ImVec2(0,px(8)));
        hint(app.ed_kind==KIND_LLAMA
             ? "The model runs on this PC and nothing leaves it. It is loaded for each letter and unloaded again, so it takes "
               "no memory in between. An 8-billion-parameter model in 4-bit form needs about 6 GB of free memory."
             : is_local_host(app.ed_url)
             ? "A server on this PC: nothing leaves it. Start the server yourself before you generate."
             : "A cloud model receives your merits and the job ad each time it writes. The key is stored encrypted for your "
               "Windows account (engines.json in Documents\\pulse_cv) and is sent only to the address above. "
               "Test sends a single word, nothing of yours.");
    };
    const char* etitle = app.ed_idx==-2? "New model" : app.ed_idx>=0? app.engines[app.ed_idx].name.c_str()
                       : app.ed_builtin==0? "Qwen3-4B on this PC" : "Claude Code";
    if(wide){
        const float cw=(av.x-gap)*0.5f;
        begin_card("##mlist",ImVec2(cw,av.y),"Models"); list(); end_card();
        ImGui::SameLine(0,gap);
        begin_card("##medit",ImVec2(cw,av.y),etitle); editor(); end_card();
    } else {
        ImGui::BeginChild("##stack",ImVec2(0,av.y));
        begin_card("##mlist",ImVec2(0,px(380)),"Models"); list(); end_card();
        vgap(gap);
        begin_card("##medit",ImVec2(0,px(620)),etitle); editor(); end_card();
        ImGui::EndChild();
    }
}

// Title bar: name, pages, window buttons. Dragging is done by Windows (WM_NCHITTEST) outside the two control areas.
static void draw_titlebar(HWND hWnd, bool& done){
    ImDrawList* dl=ImGui::GetWindowDrawList();
    const ImVec2 o=ImGui::GetWindowPos(); const float W=ImGui::GetWindowSize().x, H=px(54);
    g_cap_h=(int)H;
    const float cy=o.y+H*0.5f;
    dl->AddCircleFilled(ImVec2(o.x+px(30),cy),px(11),ImGui::GetColorU32(ImVec4(C_ACC.x,C_ACC.y,C_ACC.z,0.28f)),28);
    dl->AddCircleFilled(ImVec2(o.x+px(30),cy),px(6),ImGui::GetColorU32(C_ACC_H),24);
    ImGui::PushFont(g_semi,FS*1.05f);
    const float th=ImGui::GetTextLineHeight();
    ImGui::SetCursorScreenPos(ImVec2(o.x+px(50),cy-th*0.5f)); ImGui::TextUnformatted("PulseX CV");
    const float title_r=o.x+px(50)+ImGui::CalcTextSize("PulseX CV").x;
    ImGui::PopFont();
    // pages
    const char* NAMES[TAB_COUNT]={"Application", g_details_dirty? "Your details \xE2\x80\xA2###details" : "Your details###details","Merits","Models"};
    const float ph=ImGui::GetFrameHeight();
    float x=title_r+px(30); const float tabs_l=x;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,ph*0.5f); ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(px(16),ImGui::GetStyle().FramePadding.y));
    for(int k=0;k<TAB_COUNT;k++){
        const bool on=g_tab==k;
        ImGui::PushStyleColor(ImGuiCol_Button, on? C_CARD : ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on? C_CARD : ImVec4(1,1,1,0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, C_CARD);
        ImGui::PushStyleColor(ImGuiCol_Text, on? C_TEXT : C_DIM);
        ImGui::SetCursorScreenPos(ImVec2(x,cy-ph*0.5f));
        if(ImGui::Button(NAMES[k])) g_tab=k;
        x+=ImGui::GetItemRectSize().x+px(4);
        ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar(3);
    g_nodrag[0]={ (LONG)(tabs_l-o.x), (LONG)(cy-ph*0.5f-o.y), (LONG)(x-o.x), (LONG)(cy+ph*0.5f-o.y) };
    // window buttons
    const float bw=px(46); const float bx=o.x+W-3*bw;
    g_nodrag[1]={ (LONG)(bx-o.x), 0, (LONG)W, (LONG)H };
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0.0f); ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(1,1,1,0.09f)); ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(1,1,1,0.16f));
    const ImU32 ic=ImGui::GetColorU32(C_TEXT); const float k=px(5), lw=px(1.2f);
    ImGui::SetCursorScreenPos(ImVec2(bx,o.y));
    if(ImGui::Button("##min",ImVec2(bw,H))) ShowWindow(hWnd,SW_MINIMIZE);
    { ImVec2 c(bx+bw*0.5f,cy); dl->AddLine(ImVec2(c.x-k,c.y),ImVec2(c.x+k,c.y),ic,lw); }
    ImGui::SetCursorScreenPos(ImVec2(bx+bw,o.y));
    { const bool mx=IsZoomed(hWnd)!=0; if(ImGui::Button("##max",ImVec2(bw,H))) ShowWindow(hWnd, mx?SW_RESTORE:SW_MAXIMIZE);
      ImVec2 c(bx+bw*1.5f,cy);
      if(mx){ dl->AddRect(ImVec2(c.x-k+px(2),c.y-k),ImVec2(c.x+k,c.y+k-px(2)),ic,px(1.5f),0,lw);
              dl->AddRectFilled(ImVec2(c.x-k,c.y-k+px(2)),ImVec2(c.x+k-px(2),c.y+k),ImGui::GetColorU32(C_BG),px(1.5f));
              dl->AddRect(ImVec2(c.x-k,c.y-k+px(2)),ImVec2(c.x+k-px(2),c.y+k),ic,px(1.5f),0,lw); }
      else dl->AddRect(ImVec2(c.x-k,c.y-k),ImVec2(c.x+k,c.y+k),ic,px(1.5f),0,lw); }
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.80f,0.24f,0.24f,0.90f));
    ImGui::SetCursorScreenPos(ImVec2(bx+2*bw,o.y));
    if(ImGui::Button("##close",ImVec2(bw,H))) done=true;
    { ImVec2 c(bx+bw*2.5f,cy); dl->AddLine(ImVec2(c.x-k,c.y-k),ImVec2(c.x+k,c.y+k),ic,lw); dl->AddLine(ImVec2(c.x-k,c.y+k),ImVec2(c.x+k,c.y-k),ic,lw); }
    ImGui::PopStyleColor(4); ImGui::PopStyleVar(2);
}

// ════════════════════════ main ════════════════════════
int main(int, char**){
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0,0, GetModuleHandle(nullptr), nullptr, LoadCursor(nullptr,IDC_ARROW), nullptr, nullptr, L"pulsex_cv", nullptr };
    wc.hIcon=(HICON)LoadImageW(nullptr,to_w(P("pulse_cv.ico")).c_str(),IMAGE_ICON,0,0,LR_LOADFROMFILE|LR_DEFAULTSIZE);
    RegisterClassExW(&wc);
    g_scale = GetDpiForSystem()/96.0f;
    RECT wa{0,0,1280,1024}; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
    const int waW=wa.right-wa.left, waH=wa.bottom-wa.top;
    const int winW=std::min((int)(1340*g_scale),waW-(int)(24*g_scale)), winH=std::min((int)(900*g_scale),waH-(int)(24*g_scale));
    const int winX=wa.left+(waW-winW)/2, winY=wa.top+(waH-winH)/2;
    HWND hWnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"PulseX CV",
                              WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,   // no native caption
                              winX, winY, winW, winH, nullptr, nullptr, wc.hInstance, nullptr);
    { const DWORD round=2 /*DWMWCP_ROUND*/; DwmSetWindowAttribute(hWnd,33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,&round,sizeof(round));
      const BOOL dark=TRUE; DwmSetWindowAttribute(hWnd,20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,&dark,sizeof(dark));
      const COLORREF edge=RGB(84,90,101); DwmSetWindowAttribute(hWnd,34 /*DWMWA_BORDER_COLOR*/,&edge,sizeof(edge)); }
    g_scale = GetDpiForWindow(hWnd)/96.0f;
    if(!CreateDeviceD3D(hWnd)){ CleanupDeviceD3D(); UnregisterClassW(wc.lpszClassName,wc.hInstance); return 1; }
    ShowWindow(hWnd, SW_SHOWDEFAULT); UpdateWindow(hWnd);

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO(); io.IniFilename=nullptr;
    { std::string fonts=known_folder(FOLDERID_Fonts); if(fonts.empty()) fonts="C:\\Windows\\Fonts";
      g_font=io.Fonts->AddFontFromFileTTF((fonts+"\\segoeui.ttf").c_str(),FS);
      if(!g_font) g_font=io.Fonts->AddFontDefault();
      g_semi=io.Fonts->AddFontFromFileTTF((fonts+"\\seguisb.ttf").c_str(),FS);
      if(!g_semi) g_semi=g_font; }
    apply_style();
    ImGui_ImplWin32_Init(hWnd); ImGui_ImplDX11_Init(g_pd3dDevice,g_pd3dDeviceContext);

    PulseCvApp app;
    app.engines_load(); app.settings_load(); app.lang_load();
    app.engine_settle();
    bool done=false;
    while(!done){
        MSG msg;
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){ TranslateMessage(&msg); DispatchMessageW(&msg); if(msg.message==WM_QUIT) done=true; }
        if(done) break;
        // Throttle when minimised or not in focus: no busy loop, and the Windows IME is not nudged each frame.
        if(IsIconic(hWnd)){ Sleep(120); continue; }
        if(GetForegroundWindow()!=hWnd) Sleep(30);
        if(g_new_dpi){ g_scale=g_new_dpi/96.0f; g_new_dpi=0; apply_style(); }
        app.pump(); g_details_dirty=app.pd_dirty();
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
        ImGui::Begin("PulseX CV", nullptr, ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse
                                          |ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        draw_titlebar(hWnd,done);
        const float m=px(18);
        ImGui::SetCursorPos(ImVec2(m,(float)g_cap_h+px(2)));
        ImGui::BeginChild("##page",ImVec2(ImGui::GetWindowSize().x-2*m,ImGui::GetWindowSize().y-g_cap_h-px(2)-m),0,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        switch(g_tab){
            case TAB_APPLY:   draw_apply(app);   break;
            case TAB_DETAILS: draw_details(app); break;
            case TAB_MERITS:  draw_merits(app);  break;
            case TAB_MODELS:  draw_models(app);  break;
        }
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        const float clr[4]={C_BG.x,C_BG.y,C_BG.z,1.0f};
        g_pd3dDeviceContext->OMSetRenderTargets(1,&g_mainRTV,nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV,clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1,0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanupDeviceD3D(); DestroyWindow(hWnd); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    return 0;
}
