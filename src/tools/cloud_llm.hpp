// cloud_llm.hpp — any cloud (or local-server) text model as the writing engine.
// Three wire protocols cover practically every provider:
//   "openai"    POST <base>/chat/completions   Authorization: Bearer <key>   (OpenAI, Mistral, OpenRouter, Groq,
//               DeepSeek, xAI ... and local servers: llama.cpp llama-server, Ollama, LM Studio - key optional)
//   "anthropic" POST <base>/v1/messages        x-api-key + anthropic-version (Claude API)
//   "gemini"    POST <base>/v1beta/models/<model>:generateContent   x-goog-api-key
// The engines live in <data>\engines.json. API keys are stored encrypted with Windows DPAPI: only the same Windows
// account on the same PC can decrypt them. Request and reply bodies are never logged (they hold the user's merits).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"crypt32.lib")

namespace cloud_llm {

struct Engine {
    std::string name;       // shown in the engine list, unique
    std::string kind;       // "openai" | "anthropic" | "gemini"
    std::string base_url;   // e.g. https://api.openai.com/v1
    std::string model;      // the provider's model id
    std::string key_enc;    // DPAPI-encrypted API key, base64 ("" = no key, e.g. a local server)
    // kind "llama": a model file on this PC, served by llama.cpp's llama-server for the length of one request
    std::string gguf, server, device;   // the .gguf, llama-server.exe, "cpu" | "npu"
    int max_tokens = 0;     // openai kind only, not stored: a reply cap for a server we start ourselves (0 = none)
};

inline std::wstring widen(const std::string& s){
    int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
    std::wstring w(n>0?n-1:0,L'\0'); if(n>0) MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&w[0],n); return w;
}

// ── base64 + DPAPI ──
inline std::string b64_encode(const unsigned char* d, size_t n){
    static const char* t="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((n+2)/3*4);
    for(size_t i=0;i<n;i+=3){ unsigned v=(unsigned)d[i]<<16; if(i+1<n) v|=(unsigned)d[i+1]<<8; if(i+2<n) v|=d[i+2];
        o+=t[(v>>18)&63]; o+=t[(v>>12)&63]; o+=(i+1<n)?t[(v>>6)&63]:'='; o+=(i+2<n)?t[v&63]:'='; }
    return o;
}
inline std::vector<unsigned char> b64_decode(const std::string& s){
    auto val=[](char c)->int{ if(c>='A'&&c<='Z')return c-'A'; if(c>='a'&&c<='z')return c-'a'+26;
        if(c>='0'&&c<='9')return c-'0'+52; if(c=='+')return 62; if(c=='/')return 63; return -1; };
    std::vector<unsigned char> o; int buf=0,bits=0;
    for(char c:s){ if(c=='=')break; int v=val(c); if(v<0)continue;
        buf=(buf<<6)|v; bits+=6; if(bits>=8){ bits-=8; o.push_back((unsigned char)((buf>>bits)&0xFF)); } }
    return o;
}
inline std::string encrypt_key(const std::string& plain){
    if(plain.empty()) return {};
    DATA_BLOB in{(DWORD)plain.size(),(BYTE*)plain.data()}, out{};
    if(!CryptProtectData(&in,L"pulsex-cv",nullptr,nullptr,nullptr,0,&out)) return {};
    std::string r=b64_encode(out.pbData,out.cbData); LocalFree(out.pbData); return r;
}
inline std::string decrypt_key(const std::string& b64){
    auto blob=b64_decode(b64); if(blob.empty()) return {};
    DATA_BLOB in{(DWORD)blob.size(),blob.data()}, out{};
    if(!CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,0,&out)) return {};
    std::string p((char*)out.pbData,out.cbData); SecureZeroMemory(out.pbData,out.cbData); LocalFree(out.pbData); return p;
}

// ── engines.json ──
inline std::vector<Engine> load(const std::string& path){
    std::vector<Engine> v; std::ifstream f(std::filesystem::u8path(path)); if(!f) return v;
    try{ nlohmann::json j; f>>j;
        for(auto& e: j.value("engines",nlohmann::json::array())){
            Engine x; x.name=e.value("name",std::string()); x.kind=e.value("kind",std::string("openai"));
            x.base_url=e.value("base_url",std::string()); x.model=e.value("model",std::string());
            x.key_enc=e.value("key_enc",std::string());
            x.gguf=e.value("gguf",std::string()); x.server=e.value("server",std::string()); x.device=e.value("device",std::string("cpu"));
            if(!x.name.empty()) v.push_back(x); }
    }catch(...){ v.clear(); }
    return v;
}
inline bool save(const std::string& path, const std::vector<Engine>& v){
    nlohmann::json j; j["engines"]=nlohmann::json::array();
    for(auto& x: v){
        if(x.kind=="llama") j["engines"].push_back({{"name",x.name},{"kind",x.kind},{"gguf",x.gguf},{"server",x.server},{"device",x.device}});
        else j["engines"].push_back({{"name",x.name},{"kind",x.kind},{"base_url",x.base_url},{"model",x.model},{"key_enc",x.key_enc}}); }
    std::ofstream o(std::filesystem::u8path(path),std::ios::binary); if(!o) return false;
    o<<j.dump(2); return (bool)o;
}
inline bool find(const std::vector<Engine>& v, const std::string& name, Engine& out){
    for(auto& x: v) if(x.name==name){ out=x; return true; } return false;
}

// ── one HTTPS/HTTP request (a JSON body when there is one); returns the body, status in *status ──
inline std::string http(const wchar_t* method, const std::string& url, const std::vector<std::wstring>& headers,
                        const std::string& body, int* status, std::string& err, DWORD recv_timeout_ms, DWORD connect_timeout_ms=15000){
    *status=0;
    std::wstring wurl=widen(url);
    wchar_t host[256]={0}; std::vector<wchar_t> path(wurl.size()+8,L'\0');
    URL_COMPONENTS uc{}; uc.dwStructSize=sizeof(uc);
    uc.lpszHostName=host; uc.dwHostNameLength=255; uc.lpszUrlPath=path.data(); uc.dwUrlPathLength=(DWORD)path.size()-1;
    if(!WinHttpCrackUrl(wurl.c_str(),0,0,&uc)){ err="the address is not a valid URL: "+url; return {}; }
    const bool tls = uc.nScheme==INTERNET_SCHEME_HTTPS;
    HINTERNET hS=WinHttpOpen(L"PulseX-CV/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!hS){ err="WinHttpOpen failed"; return {}; }
    HINTERNET hC=WinHttpConnect(hS,host,uc.nPort,0);
    HINTERNET hR=hC? WinHttpOpenRequest(hC,method,path.data(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,tls?WINHTTP_FLAG_SECURE:0) : nullptr;
    std::string out;
    if(hR){
        DWORD tc=connect_timeout_ms, ts=60000, tr=recv_timeout_ms;
        WinHttpSetOption(hR,WINHTTP_OPTION_CONNECT_TIMEOUT,&tc,sizeof(tc));
        WinHttpSetOption(hR,WINHTTP_OPTION_SEND_TIMEOUT,&ts,sizeof(ts));
        WinHttpSetOption(hR,WINHTTP_OPTION_RECEIVE_TIMEOUT,&tr,sizeof(tr));
        if(!body.empty()) WinHttpAddRequestHeaders(hR,L"Content-Type: application/json",(DWORD)-1,WINHTTP_ADDREQ_FLAG_ADD);
        for(auto& h: headers) WinHttpAddRequestHeaders(hR,h.c_str(),(DWORD)-1,WINHTTP_ADDREQ_FLAG_ADD);
        if(WinHttpSendRequest(hR,WINHTTP_NO_ADDITIONAL_HEADERS,0,body.empty()?WINHTTP_NO_REQUEST_DATA:(LPVOID)body.data(),(DWORD)body.size(),(DWORD)body.size(),0)
           && WinHttpReceiveResponse(hR,nullptr)){
            DWORD st=0, sz=sizeof(st);
            WinHttpQueryHeaders(hR,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&st,&sz,nullptr); *status=(int)st;
            DWORD got=0; char buf[8192];
            while(WinHttpReadData(hR,buf,sizeof(buf),&got) && got>0) out.append(buf,got);
        } else {
            DWORD e=GetLastError();
            err = e==ERROR_WINHTTP_TIMEOUT ? "the model did not answer in time"
                : (e==ERROR_WINHTTP_CANNOT_CONNECT||e==ERROR_WINHTTP_NAME_NOT_RESOLVED) ? "could not reach "+url.substr(0,url.find('/',8))
                : "network error "+std::to_string(e);
        }
        WinHttpCloseHandle(hR);
    } else err="could not open the request";
    if(hC) WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
    return out;
}

inline std::string post_json(const std::string& url, const std::vector<std::wstring>& headers, const std::string& body,
                             int* status, std::string& err, DWORD recv_timeout_ms){
    return http(L"POST",url,headers,body,status,err,recv_timeout_ms);
}

inline std::string trim_slash(std::string s){ while(!s.empty() && (s.back()=='/'||s.back()==' ')) s.pop_back(); return s; }

// The provider's own error text, whatever shape it comes in: {"error":{"message":..}}, {"error":".."}, {"message":..}.
inline std::string error_text(const std::string& body){
    try{ auto j=nlohmann::json::parse(body);
        if(j.is_array() && !j.empty()) j=j[0];
        if(j.contains("error")){ auto& e=j["error"];
            if(e.is_object()) return e.value("message",e.dump());
            if(e.is_string()) return e.get<std::string>(); }
        if(j.contains("message") && j["message"].is_string()) return j["message"].get<std::string>();
    }catch(...){}
    return body.substr(0,300);
}

// System + user text in, the model's text out. On failure returns "" and sets err (a sentence the GUI can show).
inline std::string generate(const Engine& e, const std::string& system, const std::string& user, std::string& err,
                            DWORD recv_timeout_ms=240000){
    using nlohmann::json;
    err.clear();
    if(e.base_url.empty()){ err="'"+e.name+"' has no address (base URL)."; return {}; }
    if(e.model.empty()){ err="'"+e.name+"' has no model name."; return {}; }
    std::string key;
    if(!e.key_enc.empty()){ key=decrypt_key(e.key_enc);
        if(key.empty()){ err="The API key for '"+e.name+"' cannot be decrypted on this Windows account. Enter it again under Models."; return {}; } }
    const std::string base=trim_slash(e.base_url);
    std::string url; std::vector<std::wstring> hd; json body;
    if(e.kind=="anthropic"){
        url=base+"/v1/messages";
        hd.push_back(L"x-api-key: "+widen(key)); hd.push_back(L"anthropic-version: 2023-06-01");
        body={{"model",e.model},{"max_tokens",16000},{"messages",json::array({{{"role","user"},{"content",user}}})}};
        if(!system.empty()) body["system"]=system;
    } else if(e.kind=="gemini"){
        url=base+"/v1beta/models/"+e.model+":generateContent";
        hd.push_back(L"x-goog-api-key: "+widen(key));
        body={{"contents",json::array({{{"role","user"},{"parts",json::array({{{"text",user}}})}}})},
              {"generationConfig",{{"maxOutputTokens",8192}}}};
        if(!system.empty()) body["systemInstruction"]={{"parts",json::array({{{"text",system}}})}};
    } else {   // openai-compatible
        url=base+"/chat/completions";
        if(!key.empty()) hd.push_back(L"Authorization: Bearer "+widen(key));
        json msgs=json::array();
        if(!system.empty()) msgs.push_back({{"role","system"},{"content",system}});
        msgs.push_back({{"role","user"},{"content",user}});
        body={{"model",e.model},{"messages",msgs}};   // no token cap / temperature: providers disagree on the names
        if(e.max_tokens>0) body["max_tokens"]=e.max_tokens;
    }
    int status=0;
    std::string resp=post_json(url,hd,body.dump(),&status,err,recv_timeout_ms);
    SecureZeroMemory(key.data(),key.size());
    for(auto& h: hd) SecureZeroMemory(h.data(),h.size()*sizeof(wchar_t));
    if(!err.empty()) return {};
    if(status<200||status>=300){
        std::string why=error_text(resp);
        err = status==401||status==403 ? "the provider rejected the API key (HTTP "+std::to_string(status)+"): "+why
            : status==404 ? "address or model not found (HTTP 404): "+why
            : status==429 ? "rate limit or no credit left (HTTP 429): "+why
            : "HTTP "+std::to_string(status)+": "+why;
        return {}; }
    std::string text;
    try{
        json j=json::parse(resp);
        if(e.kind=="anthropic"){
            if(j.value("stop_reason",std::string())=="refusal"){ err="the model declined to write this text"; return {}; }
            for(auto& b: j.value("content",json::array())) if(b.value("type",std::string())=="text") text+=b.value("text",std::string());
            if(text.empty() && j.value("stop_reason",std::string())=="max_tokens") err="the reply was cut off before any text came";
        } else if(e.kind=="gemini"){
            auto cands=j.value("candidates",json::array());
            if(cands.empty()){ err="the model returned no text"; if(j.contains("promptFeedback")) err+=" ("+j["promptFeedback"].dump()+")"; return {}; }
            if(cands[0].contains("content"))
                for(auto& p: cands[0]["content"].value("parts",json::array()))
                    if(p.contains("text") && !p.value("thought",false)) text+=p["text"].get<std::string>();
            if(text.empty()) err="the model returned no text (finish reason: "+cands[0].value("finishReason",std::string("?"))+")";
        } else {
            auto ch=j.value("choices",json::array());
            if(!ch.empty() && ch[0].contains("message")){ auto& c=ch[0]["message"]["content"];
                if(c.is_string()) text=c.get<std::string>();
                else if(c.is_array()) for(auto& p: c) if(p.is_object() && p.value("type",std::string())=="text") text+=p.value("text",std::string()); }
        }
    }catch(const std::exception& ex){ err=std::string("could not read the reply: ")+ex.what(); return {}; }
    // a reasoning model behind a local server writes its thinking inline - it is not part of the answer
    for(;;){ size_t a=text.find("<think>"); if(a==std::string::npos) break; size_t b=text.find("</think>",a);
        text.erase(a, b==std::string::npos? std::string::npos : b+8-a); }
    { size_t a=text.find_first_not_of(" \t\r\n"); text = a==std::string::npos? std::string() : text.substr(a); }
    if(text.empty() && err.empty()) err="the model returned an empty reply";
    return text;
}

} // namespace cloud_llm
