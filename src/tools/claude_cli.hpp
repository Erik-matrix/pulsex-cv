// claude_cli.hpp — optional cloud engine (--claude): Claude Sonnet through the user's OWN Claude Code login
// (`claude -p`, counts against their Claude subscription; no API key in pulse_cv). Personal use on the user's machine.
// The prompt goes in through stdin (no 32k command-line limit), JSON comes back on stdout.
// Tools, MCP servers, slash commands and session history are switched off: it only writes text.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace claude_cli {

inline std::wstring widen(const std::string& s){
    int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
    std::wstring w(n>0?n-1:0,L'\0'); if(n>0) MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&w[0],n); return w;
}

// newest <root>\<version>\claude.exe
inline void newest_in(const std::filesystem::path& root, std::wstring& best, std::filesystem::file_time_type& bt){
    std::error_code ec;
    if(!std::filesystem::exists(root,ec)) return;
    for(auto& d: std::filesystem::directory_iterator(root,ec)){
        auto x=d.path()/L"claude.exe";
        if(!std::filesystem::exists(x,ec)) continue;
        auto t=std::filesystem::last_write_time(x,ec);
        if(best.empty()||t>bt){ best=x.wstring(); bt=t; } }
}

// PULSE_CV_CLAUDE (full path) > claude.exe on PATH > newest Claude Code bundled with the Claude desktop app.
// The desktop app is an MSIX package: its %APPDATA% writes are virtualised, so outside the package the bundled CLI
// lives in %LOCALAPPDATA%\Packages\Claude_<id>\LocalCache\Roaming\Claude\claude-code\<version>\claude.exe
// (a plain %APPDATA%\Claude\claude-code is only visible to processes inside the package). Version folders change
// with every app update, so take the newest.
inline std::wstring find_exe(){
    if(const wchar_t* e=_wgetenv(L"PULSE_CV_CLAUDE")) if(*e && std::filesystem::exists(e)) return e;
    wchar_t buf[MAX_PATH]={0};
    if(SearchPathW(nullptr,L"claude.exe",nullptr,MAX_PATH,buf,nullptr)) return buf;
    std::wstring best; std::filesystem::file_time_type bt{}; std::error_code ec;
    if(const wchar_t* la=_wgetenv(L"LOCALAPPDATA")){
        std::filesystem::path pk=std::filesystem::path(la)/L"Packages";
        if(std::filesystem::exists(pk,ec))
            for(auto& d: std::filesystem::directory_iterator(pk,ec))
                if(d.path().filename().wstring().rfind(L"Claude_",0)==0)
                    newest_in(d.path()/L"LocalCache"/L"Roaming"/L"Claude"/L"claude-code", best, bt); }
    if(const wchar_t* ad=_wgetenv(L"APPDATA"))
        newest_in(std::filesystem::path(ad)/L"Claude"/L"claude-code", best, bt);
    return best;
}

// Returns the model's text; on failure returns "" and sets err. not_logged_in is set when the CLI runs but has no login.
inline std::string generate(const std::string& prompt, const std::string& system, const std::string& workdir,
                            std::string& err, bool& not_logged_in, const std::string& model="sonnet", DWORD timeout_ms=280000){
    not_logged_in=false;
    std::wstring exe=find_exe();
    if(exe.empty()){ err="Claude Code (claude.exe) not found: install Claude Code or set PULSE_CV_CLAUDE"; return {}; }
    std::string pf=workdir+"\\claude_prompt.txt";
    { std::ofstream f(pf,std::ios::binary); f<<prompt; }
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE hIn=CreateFileW(widen(pf).c_str(),GENERIC_READ,FILE_SHARE_READ,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE rd=nullptr, wr=nullptr;
    if(hIn==INVALID_HANDLE_VALUE || !CreatePipe(&rd,&wr,&sa,0)){
        err="could not open prompt file / pipe"; if(hIn!=INVALID_HANDLE_VALUE) CloseHandle(hIn); return {}; }
    SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);
    auto q=[](const std::string& s){ std::string o="\""; for(char c: s){ if(c=='"') o+="\\\""; else o+=c; } return o+"\""; };
    std::string args=" -p --model "+model+" --output-format json --tools \"\" --no-session-persistence"
                     " --strict-mcp-config --disable-slash-commands --system-prompt "+q(system);
    std::wstring cmd=L"\""+exe+L"\""+widen(args);
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=hIn; si.hStdOutput=wr; si.hStdError=wr;
    PROCESS_INFORMATION pi{};
    BOOL ok=CreateProcessW(nullptr,&cmd[0],nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,widen(workdir).c_str(),&si,&pi);
    CloseHandle(wr); CloseHandle(hIn);
    std::string out;
    if(!ok){ CloseHandle(rd); err="could not start claude.exe"; std::filesystem::remove(pf); return {}; }
    DWORD t0=GetTickCount();
    for(;;){
        DWORD avail=0;
        if(PeekNamedPipe(rd,nullptr,0,nullptr,&avail,nullptr) && avail){
            std::vector<char> b(avail); DWORD got=0;
            if(ReadFile(rd,b.data(),avail,&got,nullptr)) out.append(b.data(),got);
            continue; }
        if(WaitForSingleObject(pi.hProcess,50)==WAIT_OBJECT_0){
            char b[4096]; DWORD got=0; while(ReadFile(rd,b,sizeof(b),&got,nullptr)&&got) out.append(b,got);
            break; }
        if(GetTickCount()-t0>timeout_ms){ TerminateProcess(pi.hProcess,1); err="Claude timed out"; break; }
    }
    CloseHandle(rd); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    std::error_code ec; std::filesystem::remove(pf,ec);   // the prompt holds the user's merits — don't leave a copy
    if(!err.empty()) return {};
    size_t j0=out.find('{'), j1=out.rfind('}');
    if(j0==std::string::npos||j1==std::string::npos){ err="unexpected output from claude: "+out.substr(0,300); return {}; }
    try{
        auto j=nlohmann::json::parse(out.substr(j0,j1-j0+1));
        std::string r=j.value("result",std::string());
        if(j.value("is_error",false)){
            if(r.find("Not logged in")!=std::string::npos||r.find("/login")!=std::string::npos) not_logged_in=true;
            err="Claude: "+r; return {}; }
        if(r.empty()) err="Claude returned an empty reply";
        return r;
    } catch(const std::exception& e){ err=std::string("could not parse claude JSON: ")+e.what(); return {}; }
}

} // namespace claude_cli
