// local_llama.hpp — a model FILE on this PC as the writing engine (kind "llama" in engines.json).
// llama.cpp's llama-server is started for the length of one request and stopped again: it binds to 127.0.0.1 on a
// free port, the request goes through the same OpenAI-compatible client as a cloud model, nothing leaves the PC.
// The server is tied to a job object, so it cannot outlive this process.
#pragma once
#include "cloud_llm.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib,"ws2_32.lib")

namespace local_llama {

inline int free_port(){
    WSADATA w; if(WSAStartup(MAKEWORD(2,2),&w)!=0) return 0;
    int port=0; SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(s!=INVALID_SOCKET){
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=0;
        int n=sizeof(a);
        if(bind(s,(sockaddr*)&a,sizeof(a))==0 && getsockname(s,(sockaddr*)&a,&n)==0) port=ntohs(a.sin_port);
        closesocket(s); }
    WSACleanup(); return port;
}

// The command line that serves `e`. A llama-server that sits next to ggml-hexagon.dll is the NPU-capable build;
// "npu" asks for all layers on the Snapdragon NPU, anything else keeps the model on the processor.
inline std::string command_line(const cloud_llm::Engine& e, int port){
    std::string c="\""+e.server+"\" -m \""+e.gguf+"\" --host 127.0.0.1 --port "+std::to_string(port)
                 +" -c 8192 -np 1 --no-webui --temp 0.3 --repeat-penalty 1.1"
                 " --dry-multiplier 0.8 --dry-allowed-length 12 --dry-penalty-last-n 1024";   // DRY: no paragraph comes round twice (12 = words and phrases may repeat)
    c += e.device=="npu" ? " -ngl 99 --device HTP0" : " -ngl 0 --device none";
    return c;
}

// The server for one model file: started once, asked several times, stopped (also when this object goes away).
// log_path: the server's own output (startup messages; llama-server does not log prompts or replies there).
struct Session {
    PROCESS_INFORMATION pi{}; HANDLE job=nullptr; std::string base, name; bool up=false;
    bool start(const cloud_llm::Engine& e, std::string& err, const std::string& log_path, DWORD load_timeout_ms=300000){
        err.clear(); std::error_code ec; name=e.name;
        if(e.gguf.empty() || !std::filesystem::exists(std::filesystem::u8path(e.gguf),ec)){ err="The model file of '"+e.name+"' is not there: "+e.gguf; return false; }
        if(e.server.empty() || !std::filesystem::exists(std::filesystem::u8path(e.server),ec)){ err="llama-server.exe of '"+e.name+"' is not there: "+e.server; return false; }
        const int port=free_port(); if(!port){ err="no free port for the local server"; return false; }
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        HANDLE hLog=CreateFileW(cloud_llm::widen(log_path).c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        HANDLE hNul=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
        job=CreateJobObjectW(nullptr,nullptr);
        if(job){ JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{}; li.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job,JobObjectExtendedLimitInformation,&li,sizeof(li)); }
        std::wstring cmd=cloud_llm::widen(command_line(e,port));
        std::wstring wd=std::filesystem::u8path(e.server).parent_path().wstring();
        // NPU: the DSP side loads its library (libggml-htp-*.so, next to the server) through this variable; without it
        // the session fails with 0x80000406. The server's own folder is also its working directory.
        if(e.device=="npu") SetEnvironmentVariableW(L"ADSP_LIBRARY_PATH",wd.c_str());
        STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
        si.hStdInput=hNul; si.hStdOutput=hLog; si.hStdError=hLog;
        BOOL ok=CreateProcessW(nullptr,&cmd[0],nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,wd.c_str(),&si,&pi);
        if(hLog!=INVALID_HANDLE_VALUE) CloseHandle(hLog);
        if(hNul!=INVALID_HANDLE_VALUE) CloseHandle(hNul);
        if(!ok){ if(job){ CloseHandle(job); job=nullptr; } err="could not start "+e.server; return false; }
        if(job) AssignProcessToJobObject(job,pi.hProcess);
        ResumeThread(pi.hThread); up=true;
        // /health answers 503 while the model loads and 200 when it can take a request
        base="http://127.0.0.1:"+std::to_string(port);
        const DWORD t0=GetTickCount();
        for(;;){
            if(WaitForSingleObject(pi.hProcess,400)==WAIT_OBJECT_0){ DWORD rc=0; GetExitCodeProcess(pi.hProcess,&rc);
                err="the local server stopped while loading the model (exit code "+std::to_string(rc)+"; see "+log_path+")"; stop(); return false; }
            int st=0; std::string herr; cloud_llm::http(L"GET",base+"/health",{},"",&st,herr,3000,1000);
            if(st==200) return true;
            if(GetTickCount()-t0>load_timeout_ms){ err="the model did not finish loading in time"; stop(); return false; }
        }
    }
    // max_tokens: a reply cap - a model that will not stop is cut there
    std::string ask(const std::string& system, const std::string& user, std::string& err, int max_tokens, DWORD reply_timeout_ms=900000){
        if(!up){ err="the local server is not running"; return {}; }
        cloud_llm::Engine o; o.name=name; o.kind="openai"; o.base_url=base+"/v1"; o.model="local"; o.max_tokens=max_tokens;
        return cloud_llm::generate(o,system,user,err,reply_timeout_ms);
    }
    void stop(){ if(!up) return; up=false; TerminateProcess(pi.hProcess,0); WaitForSingleObject(pi.hProcess,5000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); if(job){ CloseHandle(job); job=nullptr; } }
    ~Session(){ stop(); }
};

// One request to a model file: start, ask, stop.
inline std::string generate(const cloud_llm::Engine& e, const std::string& system, const std::string& user,
                            std::string& err, const std::string& log_path){
    Session ses; if(!ses.start(e,err,log_path)) return {};
    std::string text=ses.ask(system,user,err,1000);
    ses.stop();
    return text;
}

// Any engine from engines.json: a model file on this PC, or a cloud / local-server model.
inline std::string generate_any(const cloud_llm::Engine& e, const std::string& system, const std::string& user,
                                std::string& err, const std::string& work_dir){
    return e.kind=="llama" ? generate(e,system,user,err,work_dir+"\\llama_server.log") : cloud_llm::generate(e,system,user,err);
}

} // namespace local_llama
