// winspell.hpp — spelling by Windows' own spell checker (ISpellChecker, Windows 8 and later) and its dictionaries.
//
// The dictionaries come with Windows' language features ("Basic typing" for a language, under Settings > Time &
// language > Language & region). Nothing is shipped with this program, and a language Windows has no dictionary for
// is simply not checked: check() then returns false and says so in `note`.
#pragma once
#include <windows.h>
#include <objbase.h>
#include <spellcheck.h>
#include <string>
#include <vector>

namespace winspell {

struct Finding { std::string word, suggestion; };   // UTF-8; suggestion may be empty

inline std::wstring widen(const std::string& s){
    if(s.empty()) return {}; int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);
    std::wstring w(n,L'\0'); MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),&w[0],n); return w; }
inline std::string narrow(const std::wstring& w){
    if(w.empty()) return {}; int n=WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),nullptr,0,nullptr,nullptr);
    std::string s(n,'\0'); WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),&s[0],n,nullptr,nullptr); return s; }

// Check `text` with the first of `tags` ("sv-SE", "en-GB", ...) that Windows has a dictionary for.
// Returns false when none is installed or the spell checker cannot be reached; `note` then says which.
inline bool check(const std::vector<std::string>& tags, const std::string& text, std::vector<Finding>& out, std::string& note){
    const HRESULT ci=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(ci);
    bool done=false;
    ISpellCheckerFactory* f=nullptr;
    if(FAILED(CoCreateInstance(__uuidof(SpellCheckerFactory),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f))) || !f){
        note="Windows' spell checker is not available"; if(uninit) CoUninitialize(); return false; }
    std::string wanted;
    for(auto& tag: tags){
        wanted += (wanted.empty()? "" : ", ")+tag;
        const std::wstring wt=widen(tag); BOOL ok=FALSE;
        if(FAILED(f->IsSupported(wt.c_str(),&ok)) || !ok) continue;
        ISpellChecker* sc=nullptr; if(FAILED(f->CreateSpellChecker(wt.c_str(),&sc)) || !sc) continue;
        const std::wstring wtext=widen(text);
        IEnumSpellingError* en=nullptr;
        if(SUCCEEDED(sc->Check(wtext.c_str(),&en)) && en){
            ISpellingError* e=nullptr;
            while(en->Next(&e)==S_OK && e){
                ULONG start=0, len=0; e->get_StartIndex(&start); e->get_Length(&len);
                if(start+len<=wtext.size() && len>0){
                    const std::wstring w=wtext.substr(start,len); Finding fd; fd.word=narrow(w);
                    IEnumString* sg=nullptr;
                    if(SUCCEEDED(sc->Suggest(w.c_str(),&sg)) && sg){ LPOLESTR s=nullptr; ULONG got=0;
                        if(sg->Next(1,&s,&got)==S_OK && s){ fd.suggestion=narrow(s); CoTaskMemFree(s); }
                        sg->Release(); }
                    out.push_back(fd); }
                e->Release(); e=nullptr; }
            en->Release(); }
        sc->Release(); note=tag; done=true; break; }
    f->Release();
    if(!done) note="Windows has no spell-check dictionary for "+wanted;
    if(uninit) CoUninitialize();
    return done;
}

} // namespace winspell
