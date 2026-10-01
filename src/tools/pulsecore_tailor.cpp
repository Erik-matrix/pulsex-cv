// pulsecore_tailor.cpp — the litmus test: job ad + CV → Qwen (LOCAL) writes a tailored ingress + cover letter → PDFs.
// Nothing private leaves the device; only the (public) ad is external. Renders to <exe>\output.
#include <pcore/runtime/genie_pipeline.hpp>   // 4096-context Genie path (handles KV/context internally)
#include "local_llama.hpp"                 // optional engines (--cloud <name>): any cloud provider, or a model file on this PC
#include "ad_fetch.hpp"
#include "lang.hpp"                      // the language pack lang\<code>.json: prioritising, suggestions, prompts, labels
#include "winspell.hpp"                  // spelling by Windows' own spell checker
#include "claude_cli.hpp"                  // optional cloud engine (--claude): Claude Sonnet via the user's own Claude Code login
#include "cv_render.hpp"
#include <nlohmann/json.hpp>                   // load the editable CV history (cv_data.json)
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>       // SHGetKnownFolderPath (user's Downloads folder)
#include <shellapi.h>     // CommandLineToArgvW — read args as Unicode so åäö in role/company survive
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <ctime>
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
using namespace pcore::runtime;

// All paths resolve relative to the executable → pulse_cv is portable, no personal/hardcoded paths.
static std::string app_dir(){
    char buf[MAX_PATH]={0}; GetModuleFileNameA(nullptr,buf,MAX_PATH);
    std::string p(buf); auto sl=p.find_last_of("\\/"); return sl==std::string::npos?std::string("."):p.substr(0,sl);
}
static std::string P(const std::string& sub){ return app_dir()+"\\"+sub; }   // <exe>\<sub>
static lang::Pack g_L;   // the language of this run: the ad's own, or --lang (see main)
static CvLabels labels_of(const lang::Pack& p){ CvLabels lb;
    lb.summary=p.str("l.summary"); lb.experience=p.str("l.experience"); lb.other_experience=p.str("l.other_experience"); lb.education=p.str("l.education");
    lb.education_courses=p.str("l.education_courses"); lb.other=p.str("l.other"); lb.qualifications=p.str("l.qualifications");
    lb.personal_interests=p.str("l.personal_interests"); lb.personal_info=p.str("l.personal_info"); lb.traits=p.str("l.traits");
    lb.personality=p.str("l.personality"); lb.interests=p.str("l.interests"); lb.page=p.str("l.page"); lb.page_single=p.str("l.page_single");
    lb.application=p.str("l.application"); lb.letter=p.str("l.letter"); lb.regards=p.str("l.regards"); return lb; }
// The user's real Downloads folder (respects a moved/redirected Downloads); falls back to %USERPROFILE%\Downloads.
static std::string downloads_dir(){
    if(const char* o=std::getenv("PULSE_CV_OUT")) if(*o) return o;   // another output folder
    PWSTR wp=nullptr; std::string out;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads,0,nullptr,&wp)) && wp){
        int n=WideCharToMultiByte(CP_ACP,0,wp,-1,nullptr,0,nullptr,nullptr);
        if(n>1){ out.resize(n-1); WideCharToMultiByte(CP_ACP,0,wp,-1,&out[0],n,nullptr,nullptr); } }
    if(wp) CoTaskMemFree(wp);
    if(out.empty()){ const char* up=std::getenv("USERPROFILE"); out = up? std::string(up)+"\\Downloads" : app_dir()+"\\output"; }
    return out;
}
// The user's Documents\pulse_cv folder — where cv_data.json + the vault live (discoverable, per-user, survives app moves).
static std::string docs_dir(){
    if(const char* o=std::getenv("PULSE_CV_DATA")) if(*o){ std::error_code oec; std::filesystem::create_directories(o,oec); return o; }   // another data folder (tests, portable use)
    PWSTR wp=nullptr; std::string out;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&wp)) && wp){
        int n=WideCharToMultiByte(CP_ACP,0,wp,-1,nullptr,0,nullptr,nullptr);
        if(n>1){ out.resize(n-1); WideCharToMultiByte(CP_ACP,0,wp,-1,&out[0],n,nullptr,nullptr); } }
    if(wp) CoTaskMemFree(wp);
    if(out.empty()){ const char* up=std::getenv("USERPROFILE"); out = up? std::string(up)+"\\Documents" : app_dir(); }
    out += "\\pulse_cv"; std::error_code ec; std::filesystem::create_directories(out,ec); return out;
}
// Args come from the OS as UTF-16; convert to UTF-8 so they match the file/JSON pipeline (cp1252() expects UTF-8).
static std::vector<std::string> utf8_args(){
    std::vector<std::string> A; int wc=0; LPWSTR* wv=CommandLineToArgvW(GetCommandLineW(),&wc);
    for(int i=0;i<wc;i++){ int n=WideCharToMultiByte(CP_UTF8,0,wv[i],-1,nullptr,0,nullptr,nullptr);
        std::string s(n>0?n-1:0,'\0'); if(n>0) WideCharToMultiByte(CP_UTF8,0,wv[i],-1,&s[0],n,nullptr,nullptr); A.push_back(s); }
    if(wv) LocalFree(wv); return A;
}


// Generic fallback CV — real data lives in data/cv_data.json (edit that; no recompile needed)
CvData builtin_cv(bool academic);   // defined below — two strictly separate profiles (practical vs academic)
static bool load_cv_json(const std::string& path, const std::string& profkey, CvData& out);   // defined below
// The town printed before the date in the letter: "place" in cv_data.json, else the part of the address after the
// postal code ("Storgatan 1, 123 45 Stad" -> "Stad").
static std::string g_place;
static std::string place_from_address(const std::string& a){
    size_t d=std::string::npos; for(size_t k=0;k<a.size();++k) if(a[k]>='0'&&a[k]<='9') d=k;
    if(d==std::string::npos) return {};
    std::string r=a.substr(d+1); size_t x=r.find_first_not_of(" ,\t"), y=r.find_last_not_of(" ,\t");
    return x==std::string::npos? std::string() : r.substr(x,y-x+1);
}

static std::string lc(std::string s){ for(char& c:s) if(c>='A'&&c<='Z') c=char(c+32); return s; }
static std::string between(const std::string& s, const std::string& a, const std::string& b){
    std::string ls=lc(s); auto i=ls.find(lc(a)); if(i==std::string::npos) return ""; i+=a.size(); auto j=ls.find(lc(b),i);
    std::string r = (j==std::string::npos)? s.substr(i) : s.substr(i,j-i);
    while(!r.empty()&&(r.front()=='\n'||r.front()==' '||r.front()=='\r')) r.erase(r.begin());
    while(!r.empty()&&(r.back()=='\n'||r.back()==' '||r.back()=='\r')) r.pop_back(); return r;
}
// Robust section extractor: from marker `a` up to the EARLIEST of any `ends` marker (handles a missing close-tag,
// e.g. the model forgetting [/INGRESS] and running straight into [BREV]).
static std::string section(const std::string& s, const std::string& a, std::initializer_list<std::string> ends){
    std::string ls=lc(s); auto i=ls.find(lc(a)); if(i==std::string::npos) return ""; i+=a.size();
    size_t j=std::string::npos;
    for(const auto& e: ends){ auto p=ls.find(lc(e),i); if(p!=std::string::npos && p<j) j=p; }
    std::string r=(j==std::string::npos)? s.substr(i) : s.substr(i,j-i);
    while(!r.empty()&&(r.front()=='\n'||r.front()==' '||r.front()=='\r')) r.erase(r.begin());
    while(!r.empty()&&(r.back()=='\n'||r.back()==' '||r.back()=='\r')) r.pop_back(); return r;
}

// Section markers as the engines actually write them -> the bracket form section() looks for. A marker counts when
// it starts a line: "[BREV]", "BREV:", "**Brev:**", "### BREV" (with or without text after it). INGÅNG is what
// Llama-3 makes of INGRESS; SAMMANFATTNING is the name the local prompt uses for it.
static std::string normalize_markers(const std::string& s){
    static const std::pair<const char*,const char*> M[] = {
        {"SAMMANFATTNING","[INGRESS]"},{"INGRESS","[INGRESS]"},{"ING\xC3\x85NG","[INGRESS]"},{"INGANG","[INGRESS]"},
        {"DETALJ","[DETALJ]"},{"BREV","[BREV]"},{"PERSONLIGT BREV","[BREV]"},{"INTRESSEN","[INTRESSEN]"},{"EGENSKAPER","[EGENSKAPER]"} };
    auto upper=[](std::string x){ for(size_t k=0;k<x.size();++k){ unsigned char c=(unsigned char)x[k];
        if(c>='a'&&c<='z') x[k]=char(c-32);
        else if(c==0xC3&&k+1<x.size()){ unsigned char d=(unsigned char)x[k+1]; if(d==0xA5||d==0xA4||d==0xB6) x[k+1]=char(d-0x20); ++k; } } return x; };
    std::string out; size_t pos=0;
    while(pos<=s.size()){
        size_t nl=s.find('\n',pos); std::string line=s.substr(pos, nl==std::string::npos? std::string::npos : nl-pos);
        size_t a=0; while(a<line.size() && (line[a]==' '||line[a]=='\t'||line[a]=='#'||line[a]=='*'||line[a]=='[')) ++a;
        const bool closing = a<line.size() && line[a]=='/';
        if(!closing){
            std::string up=upper(line.substr(a));
            for(const auto& m: M){ const size_t n=std::strlen(m.first);
                if(up.compare(0,n,m.first)!=0) continue;
                size_t b=a+n; bool sep=false;
                while(b<line.size() && (line[b]==']'||line[b]==':'||line[b]=='*'||line[b]==' '||line[b]=='\t'||line[b]=='\r')){ if(line[b]==']'||line[b]==':') sep=true; ++b; }
                if(!sep && b<line.size()) continue;            // "Brevet ska ...": a word that merely starts like a marker
                line=std::string(m.second)+(b<line.size()? " "+line.substr(b) : std::string()); break; }
        }
        out+=line; if(nl==std::string::npos) break; out+='\n'; pos=nl+1;
    }
    return out;
}

// Deterministic Swedish cleanup: a curated table of SAFE substitutions for the recurring 4B slips
// (böjnings-/prepositionsfel, särskrivningar, a couple of nonsense words). Extend as new quirks appear.
static std::string svefix(std::string t){
    static const std::pair<const char*,const char*> R[] = {
        {"målade att förstå","strävade efter att förstå"},
        {"beslutsskattar","beslutsunderlag"},
        {"organisationella","organisatoriska"},
        {"intresserad att ","intresserad av att "},
        {"hög krav","höga krav"},
        {"starkt uppmärksamhet","stark uppmärksamhet"},
        {"arbeta noggrann","arbeta noggrant"},
        {"noggrannsätt","noggrant sätt"},
        {"ansvarslig","ansvarsfull"},
        {"ansvarlig","ansvarsfull"},
        {"mitt förmåga","min förmåga"},
        {"jobbades jag","jobbade jag"},
        {"van att arbeta","van vid att arbeta"},
        {"t.ex. i som ","t.ex. som "},
        {"systemkommunisk","systematisk"},
        {"myndheter","myndigheter"},
        {"stått in på","stött på"},
        {"stött in på","stött på"},
        {" i praktik."," i praktiken."},
        {"gärna att berättar","berättar gärna"},
        {"är jag berättar gärna","berättar jag gärna"},
        {"försäkras om att","säkerställa att"},
        {"lamnas på begäran","lämnas på begäran"},
        {"lamnas pa begaran","lämnas på begäran"},
        {"nder mina ar som","nder mina år som"},
    };
    for(const auto& kv: R){ std::string a=kv.first,b=kv.second; size_t p=0;
        while((p=t.find(a,p))!=std::string::npos){ t.replace(p,a.size(),b); p+=b.size(); } }
    return t;
}

// A model that loops repeats whole paragraphs; a reply cut by the token cap ends mid-sentence. Neither belongs in a
// letter: a paragraph equal to an earlier one is dropped, and so is a last paragraph without a full stop.
static std::string tidy_paragraphs(const std::string& t, int* dropped){
    std::vector<std::string> ps; std::string cur; *dropped=0;
    auto push=[&]{ size_t a=cur.find_first_not_of(" \t\r\n"), b=cur.find_last_not_of(" \t\r\n");
        if(a!=std::string::npos){ std::string x=cur.substr(a,b-a+1); bool dup=false; for(auto& q: ps) if(q==x) dup=true;
            if(dup) ++*dropped; else ps.push_back(x); } cur.clear(); };
    std::string s; for(char c: t) if(c!='\r') s+=c;
    for(size_t k=0;k<s.size();++k){
        if(s[k]=='\n'){ size_t m=k+1; while(m<s.size() && (s[m]==' '||s[m]=='\t')) ++m;
            if(m<s.size() && s[m]=='\n'){ push(); k=m; continue; } }
        cur+=s[k]; }
    push();
    if(ps.size()>1){ char e=ps.back().back(); if(e!='.'&&e!='!'&&e!='?'&&e!='"'&&e!=')'){ ps.pop_back(); ++*dropped; } }
    std::string r; for(size_t k=0;k<ps.size();++k){ if(k) r+="\n\n"; r+=ps[k]; } return r;
}

// Letter opening (GUI field): fill {roll}/{role} and {f\xC3\xB6retag}/{company}; the role goes in lower-case when it
// reads as an ordinary noun ("om Lagerarbetare" -> "om lagerarbetare", but "om IT-tekniker" stays).
static std::string fill_opening(std::string op, const std::string& role, const std::string& company){
    std::string r=role;
    if(r.size()>1 && r[0]>='A'&&r[0]<='Z' && r[1]>='a'&&r[1]<='z') r[0]=char(r[0]+32);
    auto rep=[&](const std::string& k, const std::string& v){ size_t p; while((p=op.find(k))!=std::string::npos) op.replace(p,k.size(),v); };
    rep("{roll}",r); rep("{role}",r); rep("{f\xC3\xB6retag}",company); rep("{foretag}",company); rep("{company}",company);
    size_t a=op.find_first_not_of(" \t\r\n"), b=op.find_last_not_of(" \t\r\n");
    return a==std::string::npos? std::string() : op.substr(a,b-a+1);
}
// Guarantee the letter starts with the opening, whatever the engine wrote. Kept as is when the letter already starts
// with it (also when the model extended the sentence, e.g. '...mitt intresse, eftersom ...'). Otherwise the model's
// first sentence is replaced when it is an opening attempt (mentions the ad / starts like ours), else prepended.
static std::string enforce_opening(std::string brev, const std::string& op, const char** how){
    *how="ingen inledning"; if(op.empty()) return brev;
    size_t a=brev.find_first_not_of(" \t\r\n"); brev = a==std::string::npos? std::string() : brev.substr(a);
    std::string core=op; while(!core.empty() && (core.back()=='.'||core.back()=='!'||core.back()==' ')) core.pop_back();
    if(brev.compare(0,core.size(),core)==0){ *how="modellen skrev den ordagrant"; return brev; }
    size_t e=std::string::npos;
    for(size_t k=10;k+1<brev.size();++k) if((brev[k]=='.'||brev[k]=='!'||brev[k]=='?') && (brev[k+1]==' '||brev[k+1]=='\n'||brev[k+1]=='\r')){ e=k; break; }
    std::string first = e==std::string::npos? brev : brev.substr(0,e+1);
    std::string w0=op.substr(0,op.find(' '));
    bool attempt = first.find("annons")!=std::string::npos || (w0.size()>=5 && first.compare(0,w0.size(),w0)==0);   // "I" / "Jag" open every other sentence too
    if(attempt && e!=std::string::npos){ *how="modellens forsta mening ersatt"; return op+brev.substr(e+1); }
    if(attempt){ *how="hela texten var en inledning - ersatt"; return op; }
    *how="inledningen lagd forst"; return op+" "+brev;
}

// Lower-case ASCII + \xC3\x85/\xC3\x84/\xC3\x96/\xC3\x89 (UTF-8) for phrase and word matching.
static std::string lower_sv(std::string s){
    for(size_t i=0;i<s.size();++i){ unsigned char c=(unsigned char)s[i];
        if(c>='A'&&c<='Z') s[i]=char(c+32);
        else if(c==0xC3&&i+1<s.size()){ unsigned char d=(unsigned char)s[i+1]; if(d==0x85||d==0x84||d==0x96||d==0x89) s[i+1]=char(d+0x20); ++i; } }
    return s;
}
// ── merits: which passages of the vault documents go into the prompt (by words; see patch_vault_words_1001.py) ──
struct Passage { std::string doc; int n=0; std::string text; std::vector<std::string> words; double score=0; std::string why; };
static size_t cp_len(const std::string& s){ size_t n=0; for(unsigned char c: s) if((c&0xC0)!=0x80) ++n; return n; }
static std::string stem6(const std::string& w){ size_t n=0, k=0; for(; k<w.size(); ++k){ if(((unsigned char)w[k]&0xC0)!=0x80){ if(n==6) break; ++n; } } return w.substr(0,k); }
// Content words: lower case, at least three letters, no numbers, no function words and no words every ad contains.
static std::vector<std::string> sv_words(const std::string& text){
    static const char* STOP[] = {"och","att","det","som","för","med","den","till","är","har","inte","jag","ett","han","hon","ni","de","du","en","av","på","om","så","men","var","sig",
        "från","eller","när","kan","ska","skulle","ha","hade","vara","blir","blev","bli","vid","under","över","efter","innan","sedan","där","här","detta","denna","dessa",
        "vår","våra","vårt","din","dina","ditt","er","era","ert","min","mina","mitt","sin","sina","sitt","dig","mig","oss","dem","deras","hans","hennes","alla","allt",
        "också","även","samt","genom","mot","utan","inom","hos","mycket","mer","mest","många","några","något","någon","andra","annan","annat","än","bara","nu",
        "gärna","kommer","får","fått","göra","gör","gjort","hela","hel","varje","både","dock","redan","snart","samma","sådan","sådana","vilket","vilka","vilken",
        "söker","sökes","erbjuder","erbjuda","ansökan","ansök","välkommen","frågor","tjänsten","tjänst","annons","annonsen","urval","intervjuer",
        "arbeta","arbetar","arbete","jobba","jobbar","jobb","vill","ser","fram","emot","idag","senast","möjligt","möjligheter",
        // filler every ad is written with
        "bästa","bra","komma","skapa","hitta","sker","därför","hög","högt","höga","viktig","viktiga","viktigt","nyckel","nyckeln","stor","stort","stora",
        "del","delar","behov","krav","plus","person","personer","personlig","personliga","personligt","individer","företag","företaget","medarbetare",
        "roll","rollen","miljö","arbetsmiljö","brev","cv","skicka","kontakta","tar","ta","ger","ge","nå","nu","dag","år","tid"};
    std::vector<std::string> out; std::string lt=lower_sv(text), w;
    auto flush=[&]{ if(cp_len(w)>=3){ bool num=true; for(unsigned char c: w) if(c<'0'||c>'9') num=false;
            bool stop=false; for(const char* s: STOP) if(w==s){ stop=true; break; }
            if(!num && !stop) out.push_back(w); } w.clear(); };
    for(unsigned char c: lt){ if((c>='a'&&c<='z')||(c>='0'&&c<='9')||c>=0x80) w+=(char)c; else flush(); }
    flush(); return out;
}
// Paragraphs (blank line, or a line that starts a heading); a long one is split at a sentence end near 70 words.
static void vault_passages(const std::string& dir, std::vector<Passage>& out, std::vector<std::pair<std::string,std::string>>& docs){
    namespace fsx=std::filesystem; std::error_code ec; std::vector<fsx::path> files;
    if(fsx::exists(fsx::u8path(dir),ec)) for(auto& e: fsx::directory_iterator(fsx::u8path(dir),ec)) if(e.path().extension()==".txt") files.push_back(e.path());
    std::sort(files.begin(),files.end());
    for(auto& f: files){
        std::ifstream in(f,std::ios::binary); std::string t((std::istreambuf_iterator<char>(in)),{});
        if(t.size()>=3 && (unsigned char)t[0]==0xEF && (unsigned char)t[1]==0xBB && (unsigned char)t[2]==0xBF) t.erase(0,3);
        { std::string u; for(char c: t) if(c!='\r') u+=c; t.swap(u); }
        if(t.find_first_not_of(" \t\n")==std::string::npos) continue;
        const std::string name=f.filename().u8string();
        docs.push_back({name,t});
        std::vector<std::string> paras; std::string cur;
        for(size_t k=0;k<=t.size();){
            size_t nl=t.find('\n',k); if(nl==std::string::npos) nl=t.size();
            std::string line=t.substr(k,nl-k); k=nl+1;
            const bool blank=line.find_first_not_of(" \t")==std::string::npos, head=!line.empty()&&line[0]=='#';
            if(blank||head){ if(!cur.empty()){ paras.push_back(cur); cur.clear(); } if(blank) continue; }
            if(!cur.empty()) cur+=' '; cur+=line;
        }
        if(!cur.empty()) paras.push_back(cur);
        int n=0;
        for(auto& para: paras){
            std::vector<std::string> ws; { std::string w; for(char c: para){ if(c==' '||c=='\t'){ if(!w.empty()){ ws.push_back(w); w.clear(); } } else w+=c; } if(!w.empty()) ws.push_back(w); }
            size_t a=0;
            while(a<ws.size()){
                size_t b=std::min(ws.size(),a+70);
                if(b<ws.size()) for(size_t k=b;k>a+35;--k){ char e=ws[k-1].back(); if(e=='.'||e=='!'||e=='?'||e==':'||e==';'){ b=k; break; } }
                if(b-a>=4){ Passage ps; ps.doc=name; ps.n=++n; for(size_t k=a;k<b;++k){ if(k>a) ps.text+=' '; ps.text+=ws[k]; } ps.words=sv_words(ps.text); out.push_back(ps); }
                a=b;
            }
        }
    }
}
// The ad's word stems (weight 1) and the role's (weight 3); the role's compound parts = shorter ad words that sit
// inside a role word ("bil" and "sälja" in "bilförsäljare").
static void ad_terms(const std::string& ad, const std::string& role, std::vector<std::pair<std::string,double>>& q, std::vector<std::string>& parts){
    auto setq=[&](const std::string& st, double w){ for(auto& x: q) if(x.first==st){ if(w>x.second) x.second=w; return; } q.push_back({st,w}); };
    const std::vector<std::string> adw=sv_words(ad), rolew=sv_words(role);
    for(auto& w: adw) setq(stem6(w),1.0);
    for(size_t k=0;k<rolew.size();++k) setq(stem6(rolew[k]), k==0? 3.0 : 1.0);   // the occupation; then employer and town
    std::vector<std::string> all=adw; all.insert(all.end(),rolew.begin(),rolew.end());
    if(!rolew.empty()) for(auto& v: all){ const std::string& w=rolew[0]; size_t lv=cp_len(v); if(lv<3 || lv>=cp_len(w)) continue;
        size_t keep = lv>5? lv-2 : 3; size_t n=0,k=0; for(; k<v.size(); ++k){ if(((unsigned char)v[k]&0xC0)!=0x80){ if(n==keep) break; ++n; } }
        std::string part=v.substr(0,k); if(w.find(part)==std::string::npos) continue;
        bool have=false; for(auto& x: parts) if(x==part) have=true; if(!have) parts.push_back(part); }
}
// Inside a word a part counts at its start, after a common prefix, or after at least four letters when no Latin
// ending follows ("bil" in lastbilsmekaniker and personbil, "sälja" in försäljare; not "bil" in stabil, flexibilitet).
static bool part_inside(const std::string& w, const std::string& part){
    size_t at=w.find(part); if(at==std::string::npos) return false;
    if(at==0) return true;
    const std::string pre=w.substr(0,at); bool prefix=false; for(const char* x: {"för","ut","in","be","an","av","el","upp","åter","över","under"}) if(pre==x) prefix=true;
    if(!prefix && cp_len(pre)<4) return false;
    std::string rest=w.substr(at+part.size()); return rest.compare(0,2,"it")!=0 && rest.compare(0,2,"is")!=0;
}
// How strongly one line of the candidate's facts (a job, an education, an interest) answers the ad: the summed
// weights of the distinct ad stems and role parts in it.
static double line_match(const std::string& line, const std::vector<std::pair<std::string,double>>& q, const std::vector<std::string>& parts){
    std::vector<std::string> seen; double sc=0;
    auto add=[&](const std::string& k, double w){ for(auto& x: seen) if(x==k) return; seen.push_back(k); sc+=w; };
    for(auto& w: sv_words(line)){ std::string st=stem6(w); for(auto& x: q) if(x.first==st){ add(st,x.second); break; }
        for(auto& part: parts) if(part_inside(w,part)) add("~"+part,3.0); }
    return sc;
}
// The passages that are ABOUT one job or education: those that contain the first word of its title. At most `cap` bytes.
// allkeys = the first word (stem) of every title the user has: a passage naming three or more of them is a summary of
// everything and says nothing about this one.
static std::string vault_about(const std::vector<Passage>& ps, const std::string& title, size_t cap,
                               const std::vector<std::string>& allkeys=std::vector<std::string>()){
    const std::vector<std::string> tw=sv_words(title); if(tw.empty()) return {};
    const std::string key=stem6(tw[0]); std::string out;
    for(auto& x: ps){ bool hit=false; for(auto& w: x.words) if(stem6(w)==key){ hit=true; break; }
        if(!hit) continue;
        size_t named=0; for(auto& k: allkeys) for(auto& w: x.words) if(stem6(w)==k){ ++named; break; }
        if(named>2 || out.size()+x.text.size()+1>cap) continue;
        if(!out.empty()) out+='\n'; out+=x.text; }
    return out;
}
// The merit text for the prompt, at most `cap` bytes. whole_if_fits: an engine with room gets every document as it is.
static std::string vault_select(const std::string& ad, const std::string& role, size_t cap, bool whole_if_fits, std::string& report,
                                const std::string& lead_titles=std::string(), const std::string& lead_notes=std::string()){
    std::vector<Passage> ps; std::vector<std::pair<std::string,std::string>> docs;
    vault_passages(docs_dir()+"\\vault", ps, docs);
    report.clear();
    if(docs.empty()){ report="valvet ar tomt\n"; return {}; }
    size_t total=0; for(auto& d: docs) total+=d.first.size()+d.second.size()+8;
    if(whole_if_fits && total<=cap){
        std::string out; for(auto& d: docs){ if(!out.empty()) out+="\n\n"; out+="["+d.first+"]\n"+d.second; }
        report="hela valvet ("+std::to_string(docs.size())+" dokument, "+std::to_string(total)+" tecken) - ingen rankning\n"; return out; }
    std::vector<std::pair<std::string,double>> q; std::vector<std::string> parts; ad_terms(ad, role, q, parts);
    { auto setq=[&](const std::string& st, double w){ for(auto& x: q) if(x.first==st){ if(w>x.second) x.second=w; return; } q.push_back({st,w}); };
      for(auto& w: sv_words(lead_notes))  setq(stem6(w),2.0);       // what the user chose to build the letter on:
      for(auto& w: sv_words(lead_titles)) setq(stem6(w),4.0); }     // passages about those jobs come first
    auto inside=[](const std::string& w, const std::string& part){ return part_inside(w,part); };
    const double N=(double)ps.size();
    std::vector<std::vector<std::pair<std::string,double>>> hits(ps.size());
    std::vector<std::pair<std::string,int>> df; auto bump=[&](const std::string& s){ for(auto& x: df) if(x.first==s){ ++x.second; return; } df.push_back({s,1}); };
    for(size_t i=0;i<ps.size();++i){ auto& h=hits[i];
        auto add=[&](const std::string& s, double w){ for(auto& x: h) if(x.first==s) return; h.push_back({s,w}); };
        for(auto& w: ps[i].words){ std::string s=stem6(w); for(auto& x: q) if(x.first==s){ add(s,x.second); break; }
            for(auto& part: parts) if(inside(w,part)) add("~"+part,3.0); }
        for(auto& x: h) bump(x.first); }
    for(size_t i=0;i<ps.size();++i){ double sc=0; std::vector<std::pair<double,std::string>> top;
        for(auto& x: hits[i]){ int d=1; for(auto& y: df) if(y.first==x.first) d=y.second; double v=x.second*std::log(1.0+N/d); sc+=v; top.push_back({v,x.first}); }
        std::sort(top.begin(),top.end(),[](auto& a, auto& b){ return a.first>b.first; });
        ps[i].score=sc/std::sqrt(8.0+(double)ps[i].words.size());
        for(size_t k=0;k<top.size()&&k<6;++k){ if(k) ps[i].why+=' '; ps[i].why+=top[k].second; } }
    std::vector<size_t> order(ps.size()); for(size_t i=0;i<order.size();++i) order[i]=i;
    std::stable_sort(order.begin(),order.end(),[&](size_t a, size_t b){ return ps[a].score>ps[b].score; });
    std::vector<size_t> chosen; size_t used=0; std::vector<std::pair<std::string,size_t>> perdoc; int rank=0;
    for(size_t i: order){ ++rank;
        if(ps[i].score<=0) break;
        size_t* pd=nullptr; for(auto& x: perdoc) if(x.first==ps[i].doc) pd=&x.second; if(!pd){ perdoc.push_back({ps[i].doc,0}); pd=&perdoc.back().second; }
        const size_t need=ps[i].text.size()+2+(*pd==0? ps[i].doc.size()+4 : 0);
        const bool fits = used+need<=cap, share = *pd+need<=cap/2 || cap<1200;      // a small budget is one passage anyway
        char b[64]; std::snprintf(b,sizeof(b),"%2d  %5.2f  ",rank,ps[i].score);
        if(rank<=14) report+=std::string(b)+(fits&&share? "MED   " : !fits? "ryms ej " : "dok-tak ")+ps[i].doc+" #"+std::to_string(ps[i].n)+"  ["+ps[i].why+"]\n";
        if(fits&&share){ chosen.push_back(i); used+=need; *pd+=need; } }
    std::sort(chosen.begin(),chosen.end());                       // document order, passage order
    std::string out, last;
    for(size_t i: chosen){ if(ps[i].doc!=last){ if(!out.empty()) out+="\n\n"; out+="["+ps[i].doc+"]"; last=ps[i].doc; } out+="\n"+ps[i].text; }
    report="ordrankning: "+std::to_string(ps.size())+" stycken ur "+std::to_string(docs.size())+" dokument, "+std::to_string(chosen.size())+" valda ("+std::to_string(out.size())+" av "+std::to_string(cap)+" tecken)\n"+report;
    return out;
}


// The ticked answers to what the ad asks for, as a paragraph. Sentences that open alike are joined:
// "Jag har B-körkort. Jag har truckkort." -> "Jag har B-körkort och truckkort."  (the openings: lang "merge_prefixes")
static std::string merge_confirmed(const std::vector<std::string>& v){
    const std::string AND=" "+g_L.str("and")+" ", ALSO=" "+g_L.str("and_also")+" ";
    std::string out; std::vector<bool> used(v.size(),false);
    for(size_t i=0;i<v.size();++i){ if(used[i]) continue; used[i]=true;
        const std::string* pre=nullptr; for(auto& p: g_L.merge_prefixes) if(v[i].rfind(p,0)==0) pre=&p;
        std::string s=v[i];
        if(pre){ const size_t n=pre->size();
            auto tail=[&](const std::string& x){ std::string t=x.substr(n); while(!t.empty()&&(t.back()=='.'||t.back()==' ')) t.pop_back(); return t; };
            std::vector<std::string> tails{tail(v[i])};
            for(size_t j=i+1;j<v.size();++j) if(!used[j] && v[j].rfind(*pre,0)==0){ tails.push_back(tail(v[j])); used[j]=true; }
            bool compound=false; for(auto& t: tails) if(t.find(AND)!=std::string::npos || t.find(',')!=std::string::npos) compound=true;
            s=*pre; for(size_t k=0;k<tails.size();++k){ if(k) s += (k+1==tails.size()? (compound? ALSO : AND) : std::string(", ")); s+=tails[k]; }
            s+='.'; }
        if(!out.empty()) out+=' '; out+=s; }
    return out;
}
// Two words that differ by one letter (missing, added or changed), eight letters or more: "lagerarbetere" / "lagerarbetare".
static bool one_letter_apart(const std::string& a, const std::string& b){
    if(a==b || a.size()<8 || b.size()<8 || a.size()+1<b.size() || b.size()+1<a.size()) return false;
    size_t x=0; while(x<a.size()&&x<b.size()&&a[x]==b[x]) ++x;
    size_t y=0; while(y<a.size()-x&&y<b.size()-x&&a[a.size()-1-y]==b[b.size()-1-y]) ++y;
    return x+y+1>=std::max(a.size(),b.size());
}
// The fewest single-letter changes that turn one word into the other (for "is this suggestion the same word, misspelt?").
static size_t edit_distance(const std::string& a, const std::string& b){
    std::vector<size_t> prev(b.size()+1), cur(b.size()+1);
    for(size_t j=0;j<=b.size();++j) prev[j]=j;
    for(size_t i=1;i<=a.size();++i){ cur[0]=i;
        for(size_t j=1;j<=b.size();++j) cur[j]=std::min({prev[j]+1, cur[j-1]+1, prev[j-1]+(a[i-1]==b[j-1]? 0 : 1)});
        prev.swap(cur); }
    return prev[b.size()];
}
// A passage without the parts that list school grades ("Ämnesbetyg: Svenska 3, Engelska 3, Idrott 3"): asked what
// an education gave, the model otherwise recites the certificate.  (the words that mark grades: lang "grade_words")
static std::string drop_grades(std::string t){
    for(;;){
        const std::string lt=lower_sv(t); size_t at=std::string::npos;
        for(auto& g: g_L.grade_words){ const size_t p=lt.find(g); if(p<at) at=p; }
        if(at==std::string::npos) break;
        size_t b=0; for(const char* sep: {". "," - ","\n","=> "}){ const size_t p=lt.rfind(sep,at); if(p!=std::string::npos && p+std::strlen(sep)>b) b=p+std::strlen(sep); }
        size_t e=lt.size(); for(const char* sep: {". ","\n"," - "}){ const size_t p=lt.find(sep,at); if(p!=std::string::npos && p<e) e=p+(sep[0]=='.'? 1 : 0); }
        t.erase(b,e-b); }
    return t;
}
// A word without one ordinary ending: plockning -> plock, leveranser -> leverans, varmköket -> varmkök
// (the endings: lang "spelling.endings"). At least four bytes stay. Lower case in, lower case out.
static std::string sv_stem(std::string w){ return lang::stem(g_L, w); }
// {detalj} in the opening: fill it with the model's [DETALJ] phrase ONLY if at least half of its content words
// (>= 5 letters) occur in the ad; otherwise drop the slot (and the comma/dash before it). Never an invented detail.
static std::string finalize_opening(std::string op, std::string d, const std::string& ad, std::string* how){
    size_t p=op.find("{detalj}"); if(p==std::string::npos) p=op.find("{detail}");
    if(p==std::string::npos){ *how=""; return op; }
    size_t nlp=d.find('\n'); if(nlp!=std::string::npos) d.erase(nlp);
    auto junk=[](char c){ return c==' '||c=='\t'||c=='\r'||c=='"'||c=='\''||c==','||c=='.'||c=='-'; };
    while(!d.empty()&&junk(d.front())) d.erase(d.begin());
    while(!d.empty()&&junk(d.back())) d.pop_back();
    // the phrase continues the opening sentence: a conjunction the model capitalised goes back to lower case
    for(const char* w: {"D\xC3\xA4r ","Eftersom ","D\xC3\xA5 ","Som ","Vilket ","N\xC3\xA4r ","Med ","I ","P\xC3\xA5 "})
        if(d.compare(0,std::strlen(w),w)==0){ d[0]=char(d[0]+32); break; }
    bool ok = !d.empty() && d.size()<=160 && d.find('[')==std::string::npos;
    { int nw=0; bool in=false; for(char c: d){ bool sp=(c==' '); if(!sp&&!in) ++nw; in=!sp; } if(nw>18) ok=false; }   // a clause, not a paragraph
    if(ok){ size_t e=d.find(' '); const std::string w0=d.substr(0,e);     // first word: lower case unless the ad has it as a name
        bool name=false; for(size_t at=ad.find(w0); at!=std::string::npos && !name; at=ad.find(w0,at+1)){
            size_t k=at; while(k>0 && ad[k-1]==' ') --k; if(k>0 && ad[k-1]!='.'&&ad[k-1]!='!'&&ad[k-1]!='?'&&ad[k-1]!=':'&&ad[k-1]!='\n'&&ad[k-1]!='\r') name=true; }
        if(!name){ unsigned char c0=(unsigned char)d[0];
            if(c0>='A'&&c0<='Z') d[0]=char(c0+32);
            else if(c0==0xC3 && d.size()>1){ unsigned char c1=(unsigned char)d[1]; if(c1==0x85||c1==0x84||c1==0x96) d[1]=char(c1+0x20); } } }
    { const std::string ldd=lower_sv(" "+d+" "); for(const char* c: {" jag har "," har arbetat"," arbetat som"," min erfarenhet"," mina erfarenheter"," jag \xC3\xA4r "}) if(ldd.find(c)!=std::string::npos) ok=false; }   // a claim about me, not a thing in the ad
    std::string hit;
    if(ok){ std::string ld=lower_sv(d), la=lower_sv(ad), w; ok=false;
        static const char* DETAIL_STOP[] = { "arbetat", "arbetade", "jobbat", "hj\xC3\xA4lpa", "deras", "behov", "dagliga", "uppgifter", "inneb\xC3\xA4r", "tidigare", "personlig", "passar", "passa", "arbete", "arbeta", "arbetar", "jobbet", "jobba", "tj\xC3\xA4nsten", "tj\xC3\xA4nst", "annonsen", "annons", "eftersom", "mycket", "g\xC3\xA4rna", "bredvid", "ocks\xC3\xA5", "sj\xC3\xA4lv", "b\xC3\xA4ttre", "detta", "denna", "andra", "genom", "under", "efter", "sedan", "innan", "utvecklas", "utveckla", "m\xC3\xB6jlighet", "erfarenhet", "f\xC3\xB6retaget", "f\xC3\xB6retag", "intresse", "v\xC3\xA4lkommen", "s\xC3\xB6ker", "letar", "kunna", "vilja", "bland", "kring" };
        auto stop=[&](const std::string& x){ for(const char* s: DETAIL_STOP) if(x==s) return true; return false; };
        int seen=0, found=0;
        auto test=[&]{
            if(w.size()>=5 && !stop(w)){
                ++seen; if(la.find(sv_stem(w))!=std::string::npos){ ++found; if(hit.empty()) hit=w; } }   // the ending may differ, the word may not
            w.clear(); };
        for(unsigned char c: ld){ if((c>='a'&&c<='z')||c>=0x80) w+=(char)c; else test(); } test();
        ok = found>0 && found*2>=seen; }   // most content words are the ad's own; one shared stem ("leveran-") is not enough
    if(ok){ op.replace(p,8,d); *how="detalj ur annonsen (ordet '"+hit+"' finns i annonsen): "+d; }
    else {
        op.erase(p,8);
        while(p>0 && (op[p-1]==' '||op[p-1]==','||op[p-1]=='-')){ op.erase(p-1,1); --p; }
        if(p>=3 && (unsigned char)op[p-3]==0xE2 && (unsigned char)op[p-2]==0x80 && ((unsigned char)op[p-1]==0x93||(unsigned char)op[p-1]==0x94)){
            op.erase(p-3,3); p-=3; while(p>0 && op[p-1]==' '){ op.erase(p-1,1); --p; } }
        *how = d.empty()? "ingen detalj skriven - luckan borttagen" : "detaljen fanns inte i annonsen - luckan borttagen: "+d; }
    while(!op.empty()&&op.back()==' ') op.pop_back();
    if(!op.empty() && op.back()!='.' && op.back()!='!' && op.back()!='?') op+='.';
    return op;
}
// Canned phrases that give away a template or an AI. Skipped: the user's own opening, and phrases the
// ad itself uses (mirroring the ad's words is what we want). Reported, never auto-rewritten.
static std::string find_cliches(const std::string& text, const std::string& ad){
    static const char* PH[] = {
        "v\xC3\xA4" "ckte genast mitt intresse",
        "v\xC3\xA4" "ckte direkt mitt intresse",
        "v\xC3\xA4" "ckte mitt intresse",
        "f\xC3\xA5ngade mitt intresse",
        "f\xC3\xA5ngade genast",
        "blev jag genast intresserad",
        "blev genast intresserad",
        "blev direkt intresserad",
        "genast intresserad",
        "brinner f\xC3\xB6r",
        "passionerad",
        "min passion",
        "passion f\xC3\xB6r",
        "f\xC3\xB6r\xC3\xA4nderlig v\xC3\xA4rld",
        "snabbf\xC3\xB6r\xC3\xA4nderlig",
        "i en v\xC3\xA4rld d\xC3\xA4r",
        "jag \xC3\xA4r \xC3\xB6vertygad om att",
        "\xC3\xA4r \xC3\xB6vertygad om att",
        "ser fram emot m\xC3\xB6jligheten",
        "nya utmaningar",
        "perfekt match",
        "skapa merv\xC3\xA4rde",
        "tillf\xC3\xB6ra merv\xC3\xA4rde",
        "bidra med merv\xC3\xA4rde",
        "dynamisk milj\xC3\xB6",
        "driven och engagerad",
        "stark arbetsmoral",
        "utanf\xC3\xB6r boxen",
        "lagspelare",
        "den extra milen",
        "g\xC3\xB6ra skillnad",
        "unik m\xC3\xB6jlighet",
        "sp\xC3\xA4nnande m\xC3\xB6jlighet",
        "med stor entusiasm",
        "genuint intresse",
        "genuin passion",
        "ta n\xC3\xA4sta steg i min karri\xC3\xA4r",
        "b\xC3\xA5" "de personligt och professionellt",
        "ser fram emot ett samtal",
        "ser fram emot att tr\xC3\xA4" "ffa",
        "ser fram emot att h\xC3\xB6ra",
        "ser fram emot att f\xC3\xA5",
        "ser fram emot er \xC3\xA5terkoppling",
        "hoppas f\xC3\xA5 h\xC3\xB6ra",
        "hoppas att h\xC3\xB6ra",
        "hoppas p\xC3\xA5 att f\xC3\xA5 h\xC3\xB6ra",
        "tack f\xC3\xB6r att ni tog er tid",
        "tack f\xC3\xB6r att du tog dig tid",
        "tack f\xC3\xB6r er tid",
        "ber\xC3\xA4ttar g\xC3\xA4rna mer",
        "g\xC3\xA4rna ber\xC3\xA4tta mer",
        "i ett personligt m\xC3\xB6te",
        "vore glad att f\xC3\xA5",
        "skulle vara en \xC3\xA4ra",
    };
    std::string lt=lower_sv(text), la=lower_sv(ad); std::vector<std::string> hits;
    for(const char* ph: PH){ std::string s=ph; if(lt.find(s)==std::string::npos || la.find(s)!=std::string::npos) continue;
        bool dup=false; for(auto& h: hits) if(h.find(s)!=std::string::npos||s.find(h)!=std::string::npos) dup=true;
        if(!dup) hits.push_back(s); }
    std::string r; for(size_t k=0;k<hits.size();++k){ if(k) r+="; "; r+="'"+hits[k]+"'"; } return r;
}

// Names from the AD that the texts repeat although the candidate's own facts never mention them (a system, a tool,
// a certificate...). A name = a word capitalised in the middle of a sentence, or written in capitals. The role and
// the company are the ad's own subject and are skipped.
static std::string find_unbacked(const std::string& text, const std::string& ad, const std::string& facts,
                                 const std::string& role, const std::string& company){
    const std::string lt=lower_sv(text), lf=lower_sv(facts), lskip=lower_sv(role+" "+company);
    auto wordch=[](unsigned char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c>=0x80; };
    std::vector<std::string> hits;
    for(size_t i=0;i<ad.size();){
        if(!wordch((unsigned char)ad[i])){ ++i; continue; }
        size_t j=i; while(j<ad.size() && (wordch((unsigned char)ad[j]) || (ad[j]=='-' && j+1<ad.size() && wordch((unsigned char)ad[j+1])))) ++j;
        std::string w=ad.substr(i,j-i); const size_t start=i; i=j;
        const unsigned char c0=(unsigned char)w[0];
        const bool cap = (c0>='A'&&c0<='Z') || (c0==0xC3 && w.size()>1 && ((unsigned char)w[1]==0x85||(unsigned char)w[1]==0x84||(unsigned char)w[1]==0x96));
        if(!cap || w.size()<3) continue;
        int up=0, lo=0; for(unsigned char c: w){ if(c>='A'&&c<='Z') ++up; else if(c>='a'&&c<='z') ++lo; else if(c=='-') break; }
        bool allcaps = up>=2 && lo==0;
        if(allcaps){ bool mark=false; for(unsigned char c: w) if((c>='0'&&c<='9')||c=='-') mark=true;      // SAP, WMS, B2B - not a heading like "OM ROLLEN"
            if(cp_len(w)>5 && !mark) continue; }
        size_t k=start; while(k>0 && (ad[k-1]==' '||ad[k-1]=='\t')) --k;
        const bool first = k==0 || ad[k-1]=='.'||ad[k-1]=='!'||ad[k-1]=='?'||ad[k-1]==':'||ad[k-1]=='\n'||ad[k-1]=='\r'||ad[k-1]=='-'||ad[k-1]=='*'
                           || (k>=3 && (unsigned char)ad[k-3]==0xE2 && (unsigned char)ad[k-2]==0x80);   // a bullet or a dash
        if(first && !allcaps) continue;
        const std::string lw=lower_sv(w);
        if(lskip.find(lw)!=std::string::npos) continue;
        if(lt.find(lw)==std::string::npos || lf.find(lw)!=std::string::npos) continue;
        bool dup=false; for(auto& h: hits) if(lower_sv(h)==lw) dup=true;
        if(!dup) hits.push_back(w);
    }
    // years: a year in the texts that is in neither the candidate's facts nor the ad
    for(size_t i=0;i+3<text.size();++i){
        if(!((text[i]=='1'&&text[i+1]=='9')||(text[i]=='2'&&text[i+1]=='0'))) continue;
        if(!(text[i+2]>='0'&&text[i+2]<='9'&&text[i+3]>='0'&&text[i+3]<='9')) continue;
        if(i>0 && text[i-1]>='0'&&text[i-1]<='9') continue;
        if(i+4<text.size() && text[i+4]>='0'&&text[i+4]<='9') continue;
        const std::string y=text.substr(i,4);
        if(facts.find(y)!=std::string::npos || ad.find(y)!=std::string::npos) continue;
        bool dup=false; for(auto& h: hits) if(h==y) dup=true;
        if(!dup) hits.push_back(y);
    }
    std::string r; for(size_t k=0;k<hits.size();++k){ if(k) r+=", "; r+=hits[k]; } return r;
}

int main(int argc,char**argv){
    // ad: title, employer, and the description text (from Platsbanken/JobTech; passed in or the built-in Kock test)
    std::string role="Kock till Uppsala", company="Compass Group AB", ad, extra; int pos=0; bool fromdrafts=false; bool use_claude=false; bool vault_only=false; std::vector<std::string> lead_titles; std::string cloud_name, cloud_test; std::string opening;
    std::vector<std::string> confirmed;   // sentences answering the ad's practical requirements that the user has ticked as true
    std::string lang_arg="auto", ad_asks_file;   // --lang sv|en|auto;  --ad-asks <file>: print what an ad asks for (tests)
    std::string profile="auto";                                            // praktisk | akademisk | auto (pick from the ad)
    std::string layout="classic";                                          // classic | creative (accent sidebar)
    double acc[3]={0.09,0.16,0.33};
    std::vector<std::string> Aw=utf8_args(); int argN=(int)Aw.size(); (void)argc; (void)argv;
    for(int i=1;i<argN;i++){ std::string a=Aw[i];
        if(a=="--extra"&&i+1<argN){ extra=Aw[++i]; }                       // undocumented merits/interests (from the GUI questions)
        else if(a=="--adfile"&&i+1<argN){ std::ifstream f(Aw[++i]); ad.assign((std::istreambuf_iterator<char>(f)),{}); }
        else if(a=="--profile"&&i+1<argN){ profile=Aw[++i]; }              // which CV profile to use (praktisk/akademisk/auto)
        else if(a=="--from-drafts"){ fromdrafts=true; }                    // skip Qwen, render from edited ingress.txt/brev.txt
        else if(a=="--cloud"&&i+1<argN){ cloud_name=Aw[++i]; }             // cloud engine by its name in engines.json (same grounding)
        else if(a=="--cloud-test"&&i+1<argN){ cloud_test=Aw[++i]; }        // the GUI's Test button: one tiny request, no personal data
        else if(a=="--claude"){ use_claude=true; }                         // cloud engine: Claude Sonnet via `claude -p` (user's own login)
        else if(a=="--lead"&&i+1<argN){ std::ifstream f(Aw[++i]); std::string l;                 // what the user wants the letter built on, one title per line
            while(std::getline(f,l)){ while(!l.empty()&&(l.back()=='\r'||l.back()==' ')) l.pop_back(); if(!l.empty()) lead_titles.push_back(l); } }
        else if(a=="--fetch-ad"&&i+1<argN){ ad_fetch::Ad fa; std::string ferr; bool ok=ad_fetch::fetch(Aw[++i],fa,ferr);
            std::error_code fec; std::filesystem::create_directories(P("work"),fec);
            { std::ofstream(P("work\\fetched_ad.txt"),std::ios::binary) << (ok? "OK " + fa.how : "ERROR " + ferr) << "\n" << fa.role << "\n" << fa.company << "\n---\n" << fa.text; }
            std::printf("[tailor] fetch-ad: %s\n", ok? fa.how.c_str() : ferr.c_str()); return ok? 0 : 1; }
        else if(a=="--confirm"&&i+1<argN){ std::ifstream f(Aw[++i]); std::string l;            // one ticked sentence per line
            while(std::getline(f,l)){ while(!l.empty()&&(l.back()=='\r'||l.back()==' ')) l.pop_back(); if(!l.empty()) confirmed.push_back(l); } }
        else if(a=="--ad-asks"&&i+1<argN){ ad_asks_file=Aw[++i]; }
        else if(a=="--lang"&&i+1<argN){ lang_arg=Aw[++i]; }               // the language pack to use; default: the ad's own language
        else if(a=="--vault-only"){ vault_only=true; }                     // only choose the merit passages and report
        else if(a=="--opening"&&i+1<argN){ std::ifstream f(Aw[++i]); opening.assign((std::istreambuf_iterator<char>(f)),{}); }   // letter opening (GUI field)
        else if(a=="--accent"&&i+3<argN){ acc[0]=atof(Aw[i+1].c_str()); acc[1]=atof(Aw[i+2].c_str()); acc[2]=atof(Aw[i+3].c_str()); i+=3; }
        else if(a=="--layout"&&i+1<argN){ layout=Aw[++i]; }                 // classic | creative
        else { if(pos==0)role=a; else if(pos==1)company=a; ++pos; } }
    if(!cloud_test.empty()){
        cloud_llm::Engine ce; std::string terr, res;
        std::error_code tec; std::filesystem::create_directories(P("work"),tec);
        if(!cloud_llm::find(cloud_llm::load(docs_dir()+"\\engines.json"), cloud_test, ce)) terr="No model named '"+cloud_test+"' under Models.";
        else res=local_llama::generate_any(ce, "", "Reply with the single word OK.", terr, P("work"));
        for(char& c: res) if(c=='\n'||c=='\r') c=' ';
        std::error_code wec; std::filesystem::create_directories(P("work"),wec);
        { std::ofstream(P("work\\cloud_test.txt")) << (terr.empty()? "OK: "+res.substr(0,160) : "ERROR: "+terr); }
        std::printf("[tailor] cloud-test %s: %s\n", cloud_test.c_str(), terr.empty()? res.c_str() : terr.c_str());
        return terr.empty()? 0 : 1;
    }
    if(ad.empty()) ad =
        "Kock till Uppsala, Compass Group AB. Tillsvidare heltid, dagtid mandag-fredag. Tillagningskok som lagar skolmat, "
        "team om ca 5 personer. I tjansten ingar dagligt arbete med egenkontroll, god kannedom om allergener och ingredienser, "
        "bestallning och mottagning av varor, svinnhantering, samt hjalp med menyplanering och inventering. "
        "Krav: hotell- och restaurangutbildning med kockinriktning, tidigare restaurangerfarenhet, svenska i tal och skrift, "
        "formaga att planera och strukturera arbetet, lagarbete och kommunikation, initiativ och flexibilitet.";

    // The language pack: the files in lang\ next to the program (sv.json, en.json, ...). The ad's own language decides
    // which one is used unless --lang names it; a re-render takes the language of the last Generate.
    { std::error_code lec; std::filesystem::create_directories(P("work"),lec);
      std::vector<std::string> dirs{P("lang"), P("..\\lang")}; if(const char* e=std::getenv("PULSE_CV_LANG")) if(*e) dirs.insert(dirs.begin(), std::string(e));
      std::vector<lang::Pack> packs; std::string lerr;
      for(auto& d: dirs){ if(!std::filesystem::exists(std::filesystem::u8path(d),lec)) continue;
          std::vector<std::string> codes;
          for(auto& e: std::filesystem::directory_iterator(std::filesystem::u8path(d),lec)) if(e.path().extension()==".json") codes.push_back(e.path().stem().u8string());
          std::sort(codes.begin(),codes.end(),[](const std::string& x, const std::string& y){ return (x=="sv")!=(y=="sv")? x=="sv" : x<y; });   // Swedish first: a tie goes to it
          for(auto& c: codes){ lang::Pack p=lang::load(d,c); if(p.ok) packs.push_back(p); else lerr+=p.error+" "; }
          if(!packs.empty()) break; }
      if(packs.empty()){ if(lerr.empty()) lerr="The language files (lang\\sv.json, lang\\en.json) were not found next to the program.";
          std::printf("[tailor] %s\n", lerr.c_str()); { std::ofstream(P("work\\last_error.txt")) << lerr; } return 1; }
      if(!lerr.empty()) std::printf("[tailor] sprakfil: %s\n", lerr.c_str());
      if(!ad_asks_file.empty()){ std::ifstream f(std::filesystem::u8path(ad_asks_file)); std::string t((std::istreambuf_iterator<char>(f)),{});
          const lang::Pack& ap=packs[lang::detect(t,packs)];
          for(auto& q: lang::asks_for(ap,t)) std::printf("%s\t%s\n", q.id.c_str(), q.text.c_str()); return 0; }
      size_t pick=0; std::string want=lang_arg;
      if(want=="auto" && fromdrafts){ std::ifstream lf(P("work\\lang_used.txt")); std::getline(lf,want); if(want.empty()) want="auto"; }
      if(want=="auto") pick=lang::detect(role+" "+ad, packs);
      else { bool got=false; for(size_t i=0;i<packs.size();++i) if(packs[i].code==want){ pick=i; got=true; }
             if(!got) std::printf("[tailor] sprak %s finns inte i lang\\ - anvander %s\n", want.c_str(), packs[0].code.c_str()); }
      g_L=packs[pick];
      if(!fromdrafts){ std::ofstream(P("work\\lang_used.txt")) << g_L.code; }
      std::printf("[tailor] sprak: %s (%s)\n", g_L.code.c_str(), g_L.file.c_str()); }

    // Decide which CV profile to use. Explicit --profile wins; "auto" guesses from the ad/role keywords.
    bool academic;
    if(profile=="akademisk"||profile=="academic") academic=true;
    else if(profile=="praktisk"||profile=="practical") academic=false;
    else {   // auto
        std::string la=lc(role+" "+ad); academic=false;
        for(const char* k : {"analytiker","utredare","sekreterare","handlägg","samordnare","lärare","pedagog",
                              "projektled","koordinator","akademi","högskola","universitet","utredning","förvaltning",
                              "strateg","kommunikatör","analys","rapport","beslutsunderlag"})
            if(la.find(k)!=std::string::npos){ academic=true; break; }
    }
    std::printf("[tailor] profil: %s%s\n", academic?"akademisk":"praktisk", profile=="auto"?" (auto)":"");
    CvData base = builtin_cv(academic);                        // built-in fallback
    { const std::string CVJSON=docs_dir()+"\\cv_data.json", pk=academic?"akademisk":"praktisk";
      if(load_cv_json(CVJSON,pk,base)) std::printf("[tailor] CV-data: %s (%s)\n", CVJSON.c_str(), pk.c_str());
      else std::printf("[tailor] CV-data: inbyggd fallback (JSON saknas/ogiltig)\n"); }
    // Spelling of what is printed on the CV - the titles and notes under Your details:
    //  * two titles one letter apart are almost always one title and a typo;
    //  * Windows' own spell checker is asked about every title and note, in the language of the application. A word is
    //    reported when Windows offers ONE word close to it (two letters at most): a misspelling, not a word it does not know.
    { std::vector<std::pair<std::string,std::string>> ks; std::string sp;
      auto report=[&](const std::string& x){ if(sp.find(x)==std::string::npos) sp+=std::string(sp.empty()? "" : "; ")+x; };
      auto add=[&](const Row& r){ std::string t=lower_sv(r.title); const size_t c=t.find_first_of(" ,("); if(c!=std::string::npos) t.erase(c); ks.push_back({t,r.title}); };
      for(auto& e: base.experience) add(e); for(auto& e: base.education) add(e);
      for(size_t i=0;i<ks.size();++i) for(size_t j=i+1;j<ks.size();++j) if(one_letter_apart(ks[i].first,ks[j].first)) report(ks[i].second+" / "+ks[j].second);
      std::string text;
      auto feed=[&](const Row& r){ std::string t=r.title; size_t c=t.find_first_of(",("); if(c!=std::string::npos) t.erase(c);
          c=t.find(" \xE2\x80\x93 "); if(c!=std::string::npos) t.erase(c);
          text += lower_sv(t)+". "; if(!r.note.empty()) text += r.note+". "; };
      for(auto& e: base.experience) feed(e); for(auto& e: base.education) feed(e);
      std::vector<winspell::Finding> fs; std::string wnote;
      if(!g_L.spell_tags.empty() && winspell::check(g_L.spell_tags, text, fs, wnote)){
          size_t n=0;
          for(auto& f: fs){ const std::string lw=lower_sv(f.word), lg=lower_sv(f.suggestion);
              bool plain=cp_len(f.word)>=5; for(unsigned char ch: f.word) if((ch>='0'&&ch<='9')||ch=='-') plain=false;
              if(!plain || lg.empty() || lg.find(' ')!=std::string::npos || lg==lw || edit_distance(lw,lg)>2 || sp.find(f.word)!=std::string::npos) continue;
              bool dup=false; for(auto& k: ks) if(sp.find(k.second)!=std::string::npos && lower_sv(k.second).find(lw)==0) dup=true;   // said as a pair already
              if(dup) continue;
              report(f.word+" ("+f.suggestion+"?)"); if(++n>=6) break; }
          std::printf("[tailor] stavning: Windows ordlista %s, %zu fynd\n", wnote.c_str(), n);
      } else std::printf("[tailor] stavning: %s\n", wnote.empty()? "ingen ordlista angiven for spraket" : wnote.c_str());
      std::error_code sec; std::filesystem::create_directories(P("work"),sec);
      std::ofstream(P("work\\spelling.txt")) << sp;
      if(!sp.empty()) std::printf("[tailor] stavning: %s\n", sp.c_str()); }
    std::string ingress, brev, intr, egen, op_used;   // op_used = the opening forced onto the letter (cliche scan skips it)
    if(fromdrafts){
        std::ifstream fi(P("work\\ingress.txt")); ingress.assign((std::istreambuf_iterator<char>(fi)),{});
        std::ifstream fb(P("work\\brev.txt"));    brev.assign((std::istreambuf_iterator<char>(fb)),{});
        std::printf("[tailor] renderar från redigerade utkast (ingen Qwen)\n");
    } else {
    // What the engine has room for: the local Qwen reads 4096 tokens in all (and so may a server the user runs on
    // this PC); a model file served by us has 8192 and a short prompt; Claude Code and cloud models have room to spare.
    bool is_llama=false, roomy=use_claude;
    if(!use_claude && !cloud_name.empty()){ cloud_llm::Engine pe;
        if(cloud_llm::find(cloud_llm::load(docs_dir()+"\\engines.json"), cloud_name, pe)){
            const bool own_server = pe.base_url.find("127.0.0.1")!=std::string::npos || pe.base_url.find("localhost")!=std::string::npos;
            is_llama = pe.kind=="llama"; roomy = !is_llama && !own_server; } }
    // The user's own choice of what the letter is built on (Application page), in the order given.
    std::vector<const Row*> lead; std::vector<bool> lead_edu;
    for(auto& t: lead_titles){ bool got=false;
        auto have=[&](const Row* r){ for(auto* q: lead) if(q==r) return true; return false; };
        // every row with that title: the picker ticks two "Lagerarbetare" together
        for(auto& e: base.experience) if(e.title==t && !have(&e)){ lead.push_back(&e); lead_edu.push_back(false); got=true; }
        if(!got) for(auto& e: base.education) if(e.title==t && !have(&e)){ lead.push_back(&e); lead_edu.push_back(true); got=true; } }
    auto is_lead=[&](const Row& r){ for(auto* p: lead) if(p==&r) return true; return false; };
    { std::ofstream lu(P("work\\lead_used.txt"),std::ios::binary); for(auto* p: lead) lu << p->title << "\n"; }   // checked against the letter afterwards
    // "role (employer, years)"; what the job involved follows where there is room for it, and always for a lead
    auto fact=[&](const Row& e, bool with_note){ std::string x=e.title+" ("+((e.org.empty()||e.org==e.title)? std::string() : e.org+", ")+e.date+")";
        if(e.date.empty() && (e.org.empty()||e.org==e.title)) x=e.title;
        if(with_note && !e.note.empty()) x+=": "+e.note; return x; };
    std::string cand = "Namn: "+base.name+". Egenskaper: "+base.summary+"\nErfarenhet: ";
    for(auto& e:base.experience) cand += fact(e, roomy||is_lead(e))+"; ";
    cand += "\nUtbildning: "; for(auto& e:base.education) cand += fact(e, roomy||is_lead(e))+"; ";
    if(!lead.empty()){ cand += "\nLYFT FRAM I BREVET (kandidatens eget val, viktigast forst - bygg brevet pa dessa): ";
        for(auto* p: lead) cand += fact(*p,true)+"; "; }
    cand += "\nBehorigheter och korkort: "; for(auto& q:base.qualifications) cand += q+"; ";
    cand += "\nNuvarande intressen: "; for(auto& in:base.interests) cand += in+"; ";
    cand += "\nMina egenskaper (fran personlighetstest): "; for(auto& s:base.strengths) cand += s+"; ";
    if(!extra.empty()) cand += "\nYtterligare (uppgett av kandidaten): "+extra;
    if(!confirmed.empty()){ cand += "\nSvar pa annonsens krav (intygat av kandidaten sjalv - ta med dem i brevet, ordagrant eller med egna ord): ";
        for(auto& c: confirmed) cand += c+" "; }

    // The vault budget follows the engine: the local Qwen reads 4096 tokens in all (and so may a server the user runs
    // on this PC); a model file served by us has 8192 and a short prompt; a cloud model takes the whole vault if it fits.
    size_t vcap=700; bool vwhole=false;
    if(roomy){ vcap=30000; vwhole=true; } else if(is_llama) vcap=3500;
    std::string lead_t, lead_n; for(auto* r: lead){ lead_t+=r->title+" "; lead_n+=r->note+" "; }
    std::string vrep; std::string merits = vault_select(ad, role, vcap, vwhole, vrep, lead_t, lead_n);
    { std::ofstream(P("work\\vault_ctx.txt")) << merits; std::ofstream(P("work\\vault_rank.txt")) << vrep; }
    std::printf("[tailor] valvet (hogst %zu tecken): %s", vcap, vrep.substr(0,vrep.find('\n')+1).c_str());
    if(vault_only){ std::printf("%s", vrep.c_str()); return 0; }
    { std::ofstream(P("work\\facts.txt")) << cand << "\n" << merits; }   // what the texts may claim (checked after writing and on re-render)
    // Running on the 4096-token Genie bundle → room for a full ad. Still keep a generous cap so a very long ad
    // + merits + generation stays comfortably under 4096 (Swedish ≈ 3 chars/token).
    if(ad.size()>4500){ ad = ad.substr(0,4500); ad += "\n[annonstext forkortad]"; }
    std::printf("[tailor] prompt ~%zu tecken (ad %zu, meriter %zu)\n", cand.size()+merits.size()+ad.size(), ad.size(), merits.size());

    // The [BREV] instruction. With a letter opening (GUI field) the letter starts with it verbatim and is built from
    // what fits the ad: experience/merits, then education, interests and strengths; without one, the old 3-part letter.
    // an ad in another language with the Swedish default opening still in the field: that language's own default
    if(g_L.code!="sv" && opening.rfind("Anledningen till att jag skriver till er",0)==0 && !g_L.str("opening_default").empty()) opening=g_L.str("opening_default");
    opening = fill_opening(opening, role, company);
    const bool want_detail = opening.find("{detalj}")!=std::string::npos || opening.find("{detail}")!=std::string::npos;
    std::string op_shown=opening;
    for(const char* k: {"{detalj}","{detail}"}){ size_t p=op_shown.find(k); if(p!=std::string::npos) op_shown.replace(p,8,"<DETALJ>"); }
    std::string detail_instr = !want_detail ? std::string() : std::string(
        "[DETALJ] En kort bisats (hogst 15 ord) som fortsatter inledningsmeningen efter kommatecknet, med EN konkret "
        "detalj ur ANNONSEN som hanger ihop med mig - t.ex. arbetstiden, arbetsplatsen, en arbetsuppgift eller branschen. "
        "Anvand annonsens egna ord. Exempel: 'dar arbetstiden pa sondagar passar val bredvid mina studier'. Inga klicheer "
        "som 'vackte mitt intresse' eller 'blev genast intresserad'. [/DETALJ]\n");
    std::string brev_instr = opening.empty() ? std::string(
            "[BREV] Ett personligt brev pa ca 130-180 ord i 3 stycken (tom rad mellan styckena): (1) en naturlig inledning som visar "
            "intresse for TJANSTEN och foretaget - INTE en tackfras, INTE som om jag vore arbetsgivaren; (2) 1-2 stycken dar jag "
            "kopplar mina egenskaper och min erfarenhet DIREKT till annonsens efterfragade egenskaper/krav - namn dem med annonsens "
            "egna ord dar de passar mig (t.ex. korvana, punktlighet, lugn under press, noggrann struktur/packning, serviceinriktad) "
            "och ge ett konkret exempel ur mina faktiska jobb, och lyft dar tydligt fram DEN ENA utmarkande fordelen (se FORDELEN ovan); "
            "(3) en kort, saklig avslutning pa EN mening utan artighetsfraser, t.ex. 'Referenser lamnas pa begaran.' Skriv INTE 'ser fram emot', 'hoppas fa hora fran er', 'tack for att ni tog er tid' eller liknande. Hela brevet: saklig och rak ton - lat meriterna tala, inga overtrevliga eller installsamma formuleringar, och lat arbetsgivaren sjalv dra slutsatserna. [/BREV]\n"
        ) : std::string(
            "[BREV] Ett personligt brev pa ca 170-230 ord i 4 stycken (tom rad mellan styckena). Brevet ska borja EXAKT med "
            "denna mening, ordagrant och utan andringar: \"") + op_shown + "\" " + std::string(want_detail ? "dar <DETALJ> ar exakt samma text som du skrev i [DETALJ]. " : "") +
            "Fortsatt sedan i samma forsta stycke med en eller tva meningar om varfor just TJANSTEN och foretaget intresserar "
            "mig (inte en tackfras). (2) Ett stycke om de ERFARENHETER och MERITER ur mina faktiska jobb som passar annonsen, "
            "med konkreta exempel. (3) Ett stycke om den UTBILDNING, de INTRESSEN och de EGENSKAPER hos mig som passar "
            "annonsen - namn dem med annonsens egna ord dar de stammer in pa mig, och lyft dar tydligt fram DEN ENA "
            "utmarkande fordelen (se FORDELEN ovan). (4) En kort, saklig avslutning pa EN mening utan artighetsfraser, t.ex. 'Referenser lamnas pa begaran.' Skriv INTE 'ser fram emot', 'hoppas fa hora fran er', 'tack for att ni tog er tid' eller liknande. Hela brevet: saklig och rak ton - lat meriterna tala, inga overtrevliga eller installsamma formuleringar, och lat arbetsgivaren sjalv dra slutsatserna. "
            "Ta bara med det som verkligen passar annonsen OCH finns i KANDIDAT/MERITER - hitta aldrig pa. [/BREV]\n";
    std::string prompt =
        "<|im_start|>user\nDu ar en erfaren svensk rekryterare som skriver at en kandidat. Skriv KORREKT, enkel och naturlig "
        "svenska i FORSTA PERSON (jag).\n\n"
        "GRUNDREGEL (viktigast av allt - gar fore alla andra regler):\n"
        "Nedan finns TVA olika saker. Hall dem strikt isar:\n"
        "  * ANNONSEN = vad arbetsgivaren VILL HA (deras onskelista - INTE vad jag kan).\n"
        "  * KANDIDAT / MERITER = vad jag FAKTISKT har gjort och kan.\n"
        "Allt du skriver om MIG maste ga att hitta i KANDIDAT/MERITER. Att annonsen namner eller kraver nagot ger dig "
        "ALDRIG ratt att pasta att jag har det. Saknas ett krav bland mina meriter: utelamna det, eller lyft arligt en "
        "angransande erfarenhet jag VERKLIGEN har. Hitta ALDRIG pa erfarenhet, verktyg eller kunskap.\n"
        "EXEMPEL (gor INTE sa har):\n"
        "  Annonsen: 'vi soker erfarenhet av bokforing.'  Mina meriter: ingen bokforing.\n"
        "  FEL:  'Jag har erfarenhet av bokforing.'   (pahittat - star inte i mina meriter)\n"
        "  RATT: 'Jag har arbetat strukturerat och ansvarsfullt i flera roller och lar mig nya rutiner snabbt.'\n\n"
        "SPRAKREGLER (viktiga - foljs strikt):\n"
        "- Skriv KORTA, FULLSTANDIGA meningar. Varje mening ska vara grammatiskt korrekt och ga att forsta.\n"
        "- ALLRA VIKTIGAST - borja INTE meningar med ordet 'Jag'. Inled istallet meningen med ett ADVERB eller en "
        "PREPOSITIONSFRAS, sa hamnar verbet pa andra plats (svensk V2-ordfoljd) och 'jag' kommer senare. HOGST EN "
        "(1) mening i HELA brevet far borja med 'Jag'. Vaxla mellan olika sorters adverb:\n"
        "    * SATTSADVERB (hur?):  Noggrant..., Strukturerat..., Sjalvstandigt..., Effektivt...\n"
        "    * TIDSADVERB (nar?):   Tidigare..., Numera..., Ofta..., Under mina ar som...\n"
        "    * RUMSADVERB (var?):   Pa lagret..., I teamet..., Vid utleverans...\n"
        "    * GRADADVERB (grad?):  Alltid..., Sarskilt..., Framfor allt...\n"
        "  Skriv INTE: 'Jag har arbetat med orderplock. Jag ar noggrann.'\n"
        "  Skriv SA HAR: 'Tidigare har jag arbetat med orderplock. Noggrant kontrollerade jag varje leverans.'\n"
        "- PRONOMEN (kontrollera fore leverans):\n"
        "    * Reflexiv possessiv: agande som syftar tillbaka pa satsens subjekt = sin/sitt/sina, annars hans/hennes/deras. "
        "RATT: 'Foretaget varnar sina medarbetare.'  FEL: 'Foretaget varnar deras medarbetare.'\n"
        "    * Entydig syftning: varje pronomen far peka pa EN sak - annars upprepa substantivet. "
        "RATT: 'Rollen lockar mig; den kraver initiativ.'  FEL: 'Jag talade med chefen om projektet, och det var svart.' (vad ar 'det'?)\n"
        "    * Balans jag<->ni: flytta fokus fran jag/min/mitt till ni/er/er verksamhet nar det gar. "
        "RATT: 'Er satsning pa hallbarhet lockar mig.'  FEL: 'Jag soker jobbet for min egen utvecklings skull.'\n"
        "    * Kongruens: boj pronomenet efter huvudordets genus/numerus (den/det, denna/detta/dessa, vilken/vilket/vilka). "
        "RATT: 'denna tjanst', 'detta uppdrag'  FEL: 'detta tjanst', 'denna uppdrag'\n"
        "    * som vs vilket: 'som' knyter till ett substantiv, 'vilket' syftar pa HELA foregaende sats. "
        "RATT: 'Jag ledde teamet, vilket starkte min ledarformaga.'  FEL: 'Jag ledde teamet, som starkte min ledarformaga.'\n"
        "- Var KONKRET: utga fran kandidatens faktiska erfarenhet nedan och namn garna riktiga roller (t.ex. personlig assistent, "
        "kock, chauffor). Inga vaga metaforer, inga halvfardiga uttryck, inga ord du ar osaker pa.\n"
        "- Undvik floskler, engelska ord, overdrifter och krystade formuleringar. Hitta INTE pa meriter eller intressen.\n"
        "- VIKTIGT: en rad markerad (Praktik) eller en UTBILDNING/kurs betyder att jag praktiserat eller utbildat mig - "
        "skriv ALDRIG att jag har 'arbetat som' det. Skilj pa anstallning och utbildning/praktik.\n"
        "- Pasta ALDRIG att jag beharskar specifika verktyg, system eller plattformar som namns i ANNONSEN men som INTE "
        "finns i mina meriter (t.ex. SAP, PRIO, interna/slutna system). Skriv bara om system/verktyg jag faktiskt har "
        "erfarenhet av. Saknar jag nagot som kravs - lyft istallet arligt min angransande erfarenhet (t.ex. IT-system, "
        "systemnara programmering, analys) och min formaga att lara nytt snabbt.\n"
        "- Presentera ALDRIG ett personligt INTRESSE som professionell yrkeserfarenhet. Min IT/teknik (bygga datorer, "
        "natverk hemma, fixa datorer, hemsidor) ar ett starkt INTRESSE - inte en yrkesroll. Namn det som intresse/engagemang, "
        "aldrig som anstallning eller professionell natverks-/IT-kompetens.\n"
        "- Om annonsens yrke/omrade ar nagot jag bara har UTBILDNING i (och inte finns bland mina jobb under Erfarenhet), "
        "skriv att jag har UTBILDNING inom det (ev. med praktik kvar) och lyft overforbar erfarenhet fran mina faktiska jobb. "
        "Skriv ALDRIG 'jag har arbetat som/med' det yrket eller det faltet om det inte star bland mina jobb. Hitta inte pa "
        "arbetsuppgifter (t.ex. diagnostik, felsokning) jag inte har i mina meriter.\n"
        "- Las igenom sjalv och ratta allt som later konstigt innan du svarar.\n\n"
        "MATCHA MOT ANNONSEN (DET VIKTIGASTE - annars ar brevet vardelost):\n"
        "- Annonsen beskriver oftast de egenskaper och krav den soker i PUNKTFORM (t.ex. under rubriker som 'Vem du ar' "
        "eller 'Vad du tar med dig'). LAS de punkterna noga - de ar facit for vad brevet ska handla om.\n"
        "- Koppla KONKRET mina egenskaper och min faktiska erfarenhet till annonsens efterfragade egenskaper, en for en. "
        "Anvand garna annonsens EGNA nyckelord (t.ex. sjalvgaende, talmodig, trygg, ordningsam, punktlig, anpassningsbar, "
        "serviceinriktad, korvana, god fysik) DAR de verkligen stammer in pa mig - med ett konkret exempel ur mina jobb.\n"
        "- Skriv INTE en generisk 'empatisk / hjalpa manniskor'-vinkel som skulle passa vilket jobb som helst. Spegla det "
        " just DENNA annons faktiskt ber om. Tva olika annonser ska ge tydligt olika brev.\n\n"
        "BYGG BREVET SOM EN ARGUMENTATION (sa har resonerar du innan du skriver - sjalva analysen ska du INTE skriva ut, "
        "den STYR bara brevet):\n"
        "- TES: vad annonsen EGENTLIGEN soker (behovet bakom orden).\n"
        "- Stod tesen med mina argument, i denna ordning dar de finns tackning for: (1) mina ERFARENHETER, (2) mina "
        "EGENSKAPER, (3) min UTBILDNING, (4) min personliga OVERTYGELSE/drivkraft for just denna roll. Varje argument "
        "maste ga att hitta i KANDIDAT/MERITER och backas av ett konkret exempel - hitta ALDRIG pa.\n"
        "- FORDELEN: valj EN utmarkande fordel - det som bast skiljer mig fran mangden for just detta jobb - och lyft fram "
        "den tydligt i brevet (garna i mittstycket). Overdriv inte, men lat den synas som brevets skarpaste poang.\n"
        "- SYNTES: se till att brevets delar hanger ihop och tillsammans staller tesen - att jag passar rollen.\n\n"
        "Skriv exakt " + std::string(want_detail ? "fem" : "fyra") + " delar med dessa markorer:\n"
        "[INGRESS] En kort CV-sammanfattning, hogst 3 meningar, som lyfter det mest relevanta for tjansten. [/INGRESS]\n"
        + detail_instr + brev_instr +
        "[INTRESSEN] En kommaseparerad lista med de av mina intressen som ar relevanta for jobbet (uteslut irrelevanta). [/INTRESSEN]\n"
        "[EGENSKAPER] En kommaseparerad lista med de av MINA EGENSKAPER (listade ovan) som bast motsvarar annonsens efterfragade "
        "egenskaper (t.ex. min 'Lugn under press' motsvarar annonsens 'talmodig/trygg'; 'Strukturerad' motsvarar 'ordningsam/"
        "punktlig'; 'Oppen/losningsorienterad' motsvarar 'anpassningsbar'). Valj ENBART bland mina egenskaper ovan - hitta INTE pa nya. [/EGENSKAPER]\n\n"
        "KANDIDAT:\n"+cand+"\n\nRELEVANTA MERITER (fran valvet):\n"+merits+"\n\nANNONS:\n"+ad+"\n\n"
        "PAMINNELSE innan du svarar: koppla mina egenskaper och min erfarenhet till annonsens efterfragade egenskaper ovan "
        "(rubriker som 'Vem du ar' / krav), anvand annonsens egna nyckelord dar de matchar mig, och undvik en generisk "
        "empati-vinkel.<|im_end|>\n<|im_start|>assistant\n";

    std::string outtxt;
    // Cloud engines get the SAME grounding prompt, minus the ChatML wrappers. The prompt text is ASCII-folded for the
    // local Qwen; the system prompt asks for proper Swedish (\xC3\xA5 \xC3\xA4 \xC3\xB6) anyway.
    std::string cp = prompt;
    { const std::string h="<|im_start|>user\n"; if(cp.rfind(h,0)==0) cp.erase(0,h.size());
      size_t e=cp.find("<|im_end|>"); if(e!=std::string::npos) cp.erase(e); }
    cp += g_L.str("p.cloud_language");   // nothing for Swedish; for another language: write the texts in it
    const std::string sys = g_L.str("p.cloud_language") +
        "You write CV introductions and cover letters in Swedish for the candidate described in the user message. "
        "Follow the instructions and the tagged output format ([INGRESS], [BREV], [INTRESSEN], [EGENSKAPER]) exactly, "
        "and reply with nothing but those tagged sections. Use only facts given about the candidate (KANDIDAT, the "
        "merits and any extra notes) - never invent merits, employers, dates or qualifications. Write natural, "
        "correct Swedish with the letters \xC3\xA5, \xC3\xA4 and \xC3\xB6, even where the instructions are written without them.";
    if (use_claude) {
        std::printf("[tailor] anvander Claude Sonnet (claude -p, samma grundnings-prompt)...\n"); std::fflush(stdout);
        std::string cerr; bool nologin=false;
        std::error_code wec; std::filesystem::create_directories(P("work"),wec);
        outtxt = claude_cli::generate(cp, sys, P("work"), cerr, nologin, "sonnet");
        if(outtxt.empty()){
            std::printf("[tailor] Claude-fel: %s\n", cerr.c_str());
            { std::ofstream(P("work\\last_error.txt")) << (nologin
                ? std::string("Claude Code is not logged in. Open a terminal, run claude and type /login once (your Claude account).")
                : cerr); }
            return (nologin || cerr.find("not found")!=std::string::npos) ? 3 : 1; }
        std::printf("%s\n", outtxt.c_str());
    } else if (!cloud_name.empty()) {
        std::error_code wec; std::filesystem::create_directories(P("work"),wec);
        cloud_llm::Engine ce;
        if(!cloud_llm::find(cloud_llm::load(docs_dir()+"\\engines.json"), cloud_name, ce)){
            std::printf("[tailor] ingen motor med namnet %s\n", cloud_name.c_str());
            { std::ofstream(P("work\\last_error.txt")) << "No model named '"+cloud_name+"' under Models."; } return 1; }
        std::printf("[tailor] anvander %s (%s, %s; samma grundnings-prompt)...\n", ce.name.c_str(), ce.kind.c_str(), ce.model.c_str()); std::fflush(stdout);
        std::string gerr;
        if(ce.kind!="llama") outtxt = cloud_llm::generate(ce, sys, cp, gerr);
        else {
            // A model file on this PC writes ONE PARAGRAPH AT A TIME and sees, each time, only what that paragraph is
            // about (see tools/patch_local_stepwise_1001.py). The unchosen jobs never reach it.
            std::vector<std::pair<std::string,double>> tq; std::vector<std::string> tparts; ad_terms(ad, role, tq, tparts);
            auto item=[](const Row& e){
                return e.title+((e.org.empty()||e.org==e.title)? std::string() : ", "+e.org)+(e.date.empty()? std::string() : " ("+e.date+")")
                       +(e.note.empty()? std::string() : " \xE2\x80\x94 "+e.note); };
            struct Pick { const Row* r; bool edu; double m; };
            std::vector<Pick> plan;
            for(size_t k=0;k<lead.size();++k) plan.push_back({lead[k],(bool)lead_edu[k],0.0});
            const bool auto_pick=plan.empty();          // nothing ticked by the user: the best word matches, at most three
            if(auto_pick){
                for(auto& e: base.experience){ double m=line_match(e.title+" "+e.note,tq,tparts); if(m>=3.0) plan.push_back({&e,false,m}); }
                for(auto& e: base.education){  double m=line_match(e.title+" "+e.note,tq,tparts); if(m>=3.0) plan.push_back({&e,true,m}); }
                std::stable_sort(plan.begin(),plan.end(),[](const Pick& x, const Pick& y){ return x.m>y.m; });
                if(plan.size()>3) plan.resize(3);
                if(plan.empty() && !base.experience.empty()) plan.push_back({&base.experience[0],false,0.0});   // nothing matches: the latest job
            }
            std::printf("[tailor] lokal modell, ett stycke i taget: %zu poster (%s)\n", plan.size(), auto_pick? "orden i annonsen" : "anvandarens val"); std::fflush(stdout);
            const lang::Pack& L=g_L;
            auto F=[&](const char* key, const std::map<std::string,std::string>& v){ return lang::fmt(L.str(key), v); };
            const std::string AND=L.str("and"), ALSO=L.str("and_also");
            // one paragraph out of a reply: the first block of text, no line breaks or quotes, at most n sentences, and
            // nothing after the last full stop (a reply cut by the token cap ends mid-sentence)
            auto clean=[](std::string t, int n){
                size_t a0=t.find_first_not_of(" \t\r\n\"'"); if(a0==std::string::npos) return std::string(); t=t.substr(a0);
                { std::string u; for(char c: t) if(c!='\r') u+=c; t.swap(u); }
                size_t bl=t.find("\n\n"); if(bl!=std::string::npos) t.erase(bl);
                for(char& c: t) if(c=='\n') c=' ';
                if(n==0){ while(!t.empty() && (t.back()==' '||t.back()=='"'||t.back()=='.')) t.pop_back(); return t; }   // a clause, not a sentence
                int cnt=0; size_t last=std::string::npos;
                for(size_t k=0;k<t.size();++k){
                    if(t[k]!='.'&&t[k]!='!'&&t[k]!='?') continue;
                    const bool end = k+1==t.size() || (t[k+1]==' ' && (k+2>=t.size() || (t[k+2]>='A'&&t[k+2]<='Z') || (unsigned char)t[k+2]==0xC3));
                    if(!end) continue;
                    last=k; if(++cnt==n) break; }
                if(last==std::string::npos) return std::string();
                t.erase(last+1); return t; };
            const std::string LSYS = L.str("p.system");
            const std::string AD = F("p.ad", {{"role",role},{"company",company},{"ad",ad}});
            local_llama::Session ses;
            if(!ses.start(ce, gerr, P("work\\llama_server.log"))){ std::printf("[tailor] lokal modell: %s\n", gerr.c_str());
                { std::ofstream(P("work\\last_error.txt")) << gerr; } return 1; }
            bool failed=false;
            std::ofstream steps(P("work\\steps.txt"),std::ios::binary);   // what each step answered and decided, to read afterwards
            auto say=[&](const std::string& line){ std::printf("[tailor]   %s\n", line.c_str()); std::fflush(stdout); steps << line << "\n"; steps.flush(); };
            auto ask=[&](const char* what, const std::string& user, int max_tokens, int sentences){
                if(failed) return std::string();
                std::string e2, t=ses.ask(LSYS, user, e2, max_tokens);
                if(t.empty() && !e2.empty()){ gerr=e2; failed=true; return std::string(); }
                t=clean(t,sentences);
                say(std::string(what)+": "+t);
                return t; };
            // step 0: the job in one sentence. With the whole ad in front of it for every paragraph the model wrote
            // about the ad instead of about the candidate; one sentence of tasks leaves the facts in charge.
            std::string job=ask("tjansten", AD+L.str("p.job"), 70, 1);
            if(job.empty()) job=F("job_fallback", {{"role",role},{"company",company}});
            const std::string TJ=F("p.job_line", {{"role",role},{"company",company},{"job",job}});
            auto lc1=[](std::string t){   // "Truckförare" -> "truckförare", but "B-körkort" and "IT" stay
                if(t.size()>1 && t[0]>='A'&&t[0]<='Z' && !(t[1]>='A'&&t[1]<='Z') && t[1]!='-' && !(t[1]>='0'&&t[1]<='9')) t[0]=char(t[0]+32);
                else if(t.size()>1 && (unsigned char)t[0]==0xC3 && ((unsigned char)t[1]==0x85||(unsigned char)t[1]==0x84||(unsigned char)t[1]==0x96)) t[1]=char((unsigned char)t[1]+0x20);
                return t; };
            auto nwords=[](const std::string& t){ size_t n=0; bool in=false; for(char c: t){ if(c==' ') in=false; else if(!in){ in=true; ++n; } } return n; };
            // a short phrase out of a text: what follows a colon if there is one, no full stop, no "jag", at most maxw words
            auto phrase=[&](std::string t, size_t maxw){
                size_t c=t.rfind(": "); if(c!=std::string::npos) t=t.substr(c+2);
                c=t.find(". "); if(c!=std::string::npos) t.erase(c);
                while(!t.empty() && (t.back()=='.'||t.back()==' '||t.back()==',')) t.pop_back();
                t=lc1(t);
                if(t.empty() || nwords(t)>maxw) return std::string();
                for(auto& iw: L.i_words) if(t.rfind(iw+" ",0)==0 || t.find(" "+iw+" ")!=std::string::npos) return std::string();
                return t; };
            // The detail for the opening sentence is the first task in the job summary, built into a clause by the program
            // (asked for a detail of its own the model answered with the ad's headline, or with the example in the
            // question). It is checked against the ad afterwards, as for every engine.
            std::string l_det;
            if(want_detail) for(auto& m: L.job_markers){
                size_t c=job.find(m); if(c==std::string::npos) continue;
                const std::string also=" "+ALSO+" ", vp=L.str("verb_prefix"), inc=L.str("including");
                std::string x=job.substr(c+m.size()), second;
                size_t e=x.find_first_of(",.");
                if(e!=std::string::npos && x[e]==',' && e+2<x.size()){ second=x.substr(e+2); size_t e2=second.find_first_of(",."); if(e2!=std::string::npos) second.erase(e2);
                    e2=second.find(also); if(e2!=std::string::npos) second.erase(e2); }
                if(e!=std::string::npos) x.erase(e);
                e=x.find(also); if(e!=std::string::npos){ x.erase(e); second.clear(); }
                const bool verb = !vp.empty() && x.rfind(vp,0)==0;   // "att vägleda kunder ..." -> "där det ingår att vägleda kunder ..."
                if(!verb && nwords(x)==1 && !second.empty() && (vp.empty() || second.rfind(vp,0)!=0) && (inc.empty() || second.rfind(inc,0)!=0) && nwords(second)<=4) x+=", "+second;
                x=phrase(x,10); if(x.empty()) break;
                l_det = F(verb? "detail_verb" : "detail", {{"x",x}});
                std::printf("[tailor]   detalj (ur sammanfattningen av tj\xC3\xA4nsten): %s\n", l_det.c_str()); break; }
            auto short_title=[&](const Row& rw){ std::string t0=rw.title;
                size_t c=t0.find_first_of(",("); if(c!=std::string::npos && c>3) t0.erase(c);
                c=t0.find(" \xE2\x80\x93 "); if(c!=std::string::npos && c>3) t0.erase(c);
                while(!t0.empty()&&t0.back()==' ') t0.pop_back();
                return lc1(t0); };   // "lastbilsmekaniker", not the whole title line
            // an education named as a course takes a verb ("gått handelsprogrammet"); one named as an occupation does not
            // ("utbildning som lastbilsmekaniker")
            auto edu_verb=[&](const std::string& t0){
                for(auto& cv: L.course_verbs){ size_t k0=0;
                    while(k0<cv.when.size()){ size_t k1=cv.when.find('|',k0); if(k1==std::string::npos) k1=cv.when.size();
                        if(k1>k0 && t0.find(cv.when.substr(k0,k1-k0))!=std::string::npos) return cv.verb;
                        k0=k1+1; } }
                return std::string(); };
            auto is_course=[&](const std::string& t0){ return !edu_verb(t0).empty(); };
            // what follows "Jag har" about one item, by the program: title, place and years exactly as the user wrote them
            auto have=[&](const Row& rw, bool edu){
                const std::string t0=short_title(rw);
                const std::map<std::string,std::string> v{{"title",t0},{"employer",(rw.org.empty()||rw.org==rw.title)? std::string() : rw.org},{"years",rw.date},{"verb",edu_verb(t0)}};
                return F(!edu? "have_job" : is_course(t0)? "have_course" : "have_edu", v); };
            // the model sometimes misspells the title it was given ("lastbilmekaniker"): a word one letter away from
            // the title's first word is put right. An ending added to the title ("...mekanikern") is left alone.
            auto fix_title=[](const std::string& t, const std::string& t0){
                const std::string key=t0.substr(0,t0.find(' ')); if(key.size()<8) return t;
                auto one_off=[&](const std::string& w){   // ("near" is a macro in the Windows headers)
                    if(w==key || w.size()+1<key.size() || w.size()>key.size()+1) return false;
                    if(w.size()>key.size() && w.compare(0,key.size(),key)==0) return false;
                    size_t x=0; while(x<w.size()&&x<key.size()&&w[x]==key[x]) ++x;
                    size_t y=0; while(y<w.size()-x&&y<key.size()-x&&w[w.size()-1-y]==key[key.size()-1-y]) ++y;
                    return x+y+1>=std::max(w.size(),key.size()); };
                std::string out; size_t i=0;
                while(i<t.size()){ size_t j=t.find(' ',i); if(j==std::string::npos) j=t.size();
                    std::string w=t.substr(i,j-i), tail;
                    while(!w.empty() && (w.back()=='.'||w.back()==','||w.back()==';'||w.back()==':')){ tail.insert(tail.begin(),w.back()); w.pop_back(); }
                    std::string lw=w; const bool cap=!lw.empty()&&lw[0]>='A'&&lw[0]<='Z'; if(cap) lw[0]=char(lw[0]+32);
                    if(one_off(lw)){ w=key; if(cap && w[0]>='a'&&w[0]<='z') w[0]=char(w[0]-32); }
                    out+=w+tail; if(j<t.size()) out+=' '; i=j+1; }
                return out; };
            std::vector<std::string> paras;
            // The common denominator with the job. Asked to name it in a phrase, an 8B writes a paragraph or repeats the
            // facts - so it is found in a fixed list of what work demands (lang "demands"), and the program writes the
            // sentence (lang "joints"). Each demand has the words that SHOW it in a text - the ad, or the user's own
            // facts - and the occupations (in the user's title) that show it without saying so.
            const size_t NCOMMON=L.demands.size();
            std::vector<bool> taken(NCOMMON, false);   // a thing said about one job is not said about the next
            // What the job demands is READ from the ad, not asked: asked, the 8B names the top of the list whatever the job.
            std::vector<bool> jobneeds(NCOMMON, false);
            { const std::string la=lower_sv(role+" "+ad); std::string seen;
              for(size_t i=0;i<NCOMMON;++i) if(lang::has(la,L.demands[i].shown_by)){ jobneeds[i]=true; seen+=std::string(seen.empty()? "" : ", ")+L.demands[i].say; }
              say("annonsen kr\xC3\xA4ver: "+(seen.empty()? std::string("-") : seen)); }
            // The common denominator of one job and the job applied for: something the ad shows AND
            //   1. that the user's own facts show in words, or
            //   2. that the facts show although it was said about another job already, or
            //   3. that the occupation in the title shows.
            // Nothing found: no sentence. (The model's own choice is not used: a wrong denominator is worse than none.)
            auto common=[&](bool edu, const std::string& title, const std::string& facts){
                const std::string lf=lower_sv(facts), lt=lower_sv(title);
                auto take=[&](size_t i, const char* how){ say(std::string(edu? "utbildning" : "jobb")+" (gemensamt, "+how+"): "+L.demands[i].say); taken[i]=true; return L.demands[i].say; };
                for(size_t i=0;i<NCOMMON;++i) if(!taken[i] && jobneeds[i] && lang::has(lf,L.demands[i].shown_by)) return take(i,"ur meriterna");
                for(size_t i=0;i<NCOMMON;++i) if(jobneeds[i] && lang::has(lf,L.demands[i].shown_by)) return take(i,"ur meriterna, igen");
                for(size_t i=0;i<NCOMMON;++i) if(!taken[i] && jobneeds[i] && lang::has(lt,L.demands[i].occupations)) return take(i,"ur yrket");
                say(std::string(edu? "utbildning" : "jobb")+" (gemensamt): inget");
                return std::string(); };
            // names and years must be the user's own: a capitalised word inside a sentence, or a year, that the facts do not have
            auto names_ok=[&](const std::string& t, const std::string& facts){
                const std::string I=L.str("I");
                for(size_t i=0;i+4<=t.size();++i){ bool y=true; for(size_t k=0;k<4;++k) if(!(t[i+k]>='0'&&t[i+k]<='9')) y=false;
                    if(!y || (i>0 && t[i-1]>='0'&&t[i-1]<='9') || (i+4<t.size() && t[i+4]>='0'&&t[i+4]<='9')) continue;
                    if(facts.find(t.substr(i,4))==std::string::npos) return false; }
                bool start=true; size_t i=0;
                while(i<t.size()){ size_t j=t.find(' ',i); if(j==std::string::npos) j=t.size();
                    std::string w=t.substr(i,j-i); const bool ends = !w.empty() && (w.back()=='.'||w.back()=='!'||w.back()=='?');
                    while(!w.empty() && std::strchr(".,;:!?()\"", w.back())) w.pop_back();
                    while(!w.empty() && std::strchr("(\"", w.front())) w.erase(w.begin());
                    if(!w.empty() && !start && w!=I && w.rfind(I+"'",0)!=0 && w.rfind(I+"\xE2\x80\x99",0)!=0){
                        const bool cap = (w[0]>='A'&&w[0]<='Z') || (w.size()>1 && (unsigned char)w[0]==0xC3 && ((unsigned char)w[1]==0x85||(unsigned char)w[1]==0x84||(unsigned char)w[1]==0x96));
                        if(cap && facts.find(w)==std::string::npos) return false; }
                    start=ends; i=j+1; }
                return true; };
            // "..., vilket visade min förmåga att ..." - the tail where the conclusions live (lang "cut_tails")
            auto cut_tail=[&](std::string t){
                for(auto& tail: L.cut_tails) for(size_t c=t.find(tail); c!=std::string::npos; c=t.find(tail)){
                    const size_t e=t.find(". ",c); if(e==std::string::npos){ t.erase(c); t+='.'; } else t.erase(c,e-c); }
                return t; };
            // is a sentence carried by the facts? At least half of its content words (five letters or more) must be found there.
            // skip: words that are in this text (the title/employer line) do not count either way
            auto backed=[&](const std::string& sent, const std::string& facts, const std::string& skip=std::string()){
                const std::string ls=lower_sv(sent), lf=lower_sv(facts), lk=lower_sv(skip); std::string w; int seen=0, found=0;
                auto test=[&]{ bool fill=false; for(auto& f: L.fill_words) if(w==f) fill=true;
                    if(cp_len(w)>=5 && !fill && (lk.empty() || lk.find(sv_stem(w))==std::string::npos)){ /* letters, not bytes: "från" is four */ ++seen; if(lf.find(sv_stem(w))!=std::string::npos) ++found; }
                    w.clear(); };
                for(unsigned char c: ls){ if((c>='a'&&c<='z')||c>=0x80) w+=(char)c; else test(); } test();
                return seen==0 || found*2>=seen; };
            std::vector<Passage> vps; { std::vector<std::pair<std::string,std::string>> vdocs; vault_passages(docs_dir()+"\\vault", vps, vdocs); }
            std::vector<std::string> allkeys;   // the first word of every title the user has (see vault_about)
            { auto add=[&](const Row& e){ const std::vector<std::string> tw=sv_words(e.title); if(!tw.empty()) allkeys.push_back(stem6(tw[0])); };
              for(auto& e: base.experience) add(e); for(auto& e: base.education) add(e);
              std::sort(allkeys.begin(),allkeys.end()); allkeys.erase(std::unique(allkeys.begin(),allkeys.end()),allkeys.end()); }
            // The chosen items in groups: jobs with the same title and nothing but title, place and year ("lagerarbetare"
            // at two employers) are ONE paragraph, not two that say the same thing.
            struct Grp { std::vector<size_t> ix; std::string about; bool thin; };
            std::vector<Grp> grp;
            for(size_t k=0;k<plan.size();++k){
                const Row& rw=*plan[k].r; const std::string about=drop_grades(vault_about(vps, rw.title, 600, allkeys));   // what the documents say about this one
                const bool thin = rw.note.empty() && about.empty();
                bool joined=false;
                if(thin && !plan[k].edu) for(auto& g: grp) if(g.thin && !plan[g.ix[0]].edu && lower_sv(short_title(*plan[g.ix[0]].r))==lower_sv(short_title(rw))){ g.ix.push_back(k); joined=true; break; }
                if(!joined) grp.push_back(Grp{{k},about,thin}); }
            auto have_grp=[&](const Grp& g){   // what follows "Jag har" about a group
                if(g.ix.size()==1) return have(*plan[g.ix[0]].r, plan[g.ix[0]].edu);
                std::string o=F("have_job_head", {{"title",short_title(*plan[g.ix[0]].r)}});
                for(size_t m=0;m<g.ix.size();++m){ const Row& rw=*plan[g.ix[m]].r;
                    if(m) o += (m+1==g.ix.size()? " "+AND : std::string(","));
                    o += F("have_job_place", {{"employer",(rw.org.empty()||rw.org==rw.title)? std::string() : rw.org},{"years",rw.date}}); }
                return o; };
            for(size_t gi=0;gi<grp.size() && gi<3;++gi){
                const Grp& g=grp[gi]; const Row& rw=*plan[g.ix[0]].r; const bool edu=plan[g.ix[0]].edu;
                const std::string& about=g.about;
                const std::string t0=short_title(rw);
                std::string s1;
                if(g.thin) s1=F("i_have", {{"x",have_grp(g)}});   // nothing says what this was: the program states it
                else {
                    // what I did there - the job applied for is NOT in front of the model here, so it cannot leak in
                    const std::string start = !edu? F("start_job", {{"title",t0}}) : is_course(t0)? F("start_course", {{"verb",edu_verb(t0)},{"title",t0}}) : F("start_edu", {{"title",t0}});
                    const std::string facts=item(rw)+" "+about;
                    const bool two = facts.size()>160;   // one line of facts carries one sentence; asked for two, the model makes the second one up
                    auto first_end=[](const std::string& x){ for(size_t e=0;e+2<x.size();++e)
                        if((x[e]=='.'||x[e]=='!'||x[e]=='?') && x[e+1]==' ' && ((x[e+2]>='A'&&x[e+2]<='Z')||(unsigned char)x[e+2]==0xC3)) return e;
                        return std::string::npos; };
                    for(int attempt=0; attempt<2; ++attempt){
                        s1=cut_tail(fix_title(ask(edu? "utbildning" : "jobb",
                            F("p.facts", {{"kind",L.str(edu? "p.kind_edu" : "p.kind_job")},{"item",item(rw)}})
                            +(about.empty()? std::string() : F("p.documents", {{"about",about}}))
                            +F(two? "p.item_two" : "p.item_one", {{"start",start}})
                            +L.str("p.item_tail"), 150, two? 2 : 1), t0));
                        if(s1.empty()) continue;
                        const size_t e=first_end(s1);
                        const std::string first1 = lower_sv(e==std::string::npos? s1 : s1.substr(0,e+1)), own = lower_sv(t0.substr(0,t0.find(' ')));
                        if(first1.find(own)==std::string::npos){ say("titeln omd\xC3\xB6pt ("+own+" saknas)"); }   // "livsmedelsprogrammet" for the user's "livsmedelsteknisk utbildning"
                        else if(backed(e==std::string::npos? s1 : s1.substr(0,e+1), rw.note+" "+about, rw.title+" "+rw.org) && names_ok(s1,facts)) break;
                        say(std::string("inte ur meriterna")+(attempt? " - programmets egen mening i st\xC3\xA4llet" : " - ett f\xC3\xB6rs\xC3\xB6k till")); s1.clear(); }
                    // a second sentence stays only if the facts carry it
                    for(size_t e=0;e+2<s1.size();++e){
                        if((s1[e]!='.'&&s1[e]!='!'&&s1[e]!='?') || s1[e+1]!=' ' || !((s1[e+2]>='A'&&s1[e+2]<='Z')||(unsigned char)s1[e+2]==0xC3)) continue;
                        const std::string rest=s1.substr(e+2);
                        if(!backed(rest,facts)){ say("struket, st\xC3\xA5r inte i meriterna: "+rest); s1.erase(e+1); }
                        break; }
                    if(s1.empty()) s1=F("i_have", {{"x",have(rw,edu)}});
                }
                std::string gfacts; for(size_t m: g.ix) gfacts+=item(*plan[m].r)+" "; gfacts+=about;
                const std::string link=common(edu, rw.title+" "+rw.note, gfacts);
                if(!link.empty()) s1+=" "+lang::fmt(L.joints[gi], {{"x",link}});
                paras.push_back(s1);
            }
            if(grp.size()>3){ std::string t;   // chosen, but beyond the three that get a paragraph: one sentence by the program
                const std::string drop=L.str("have_job_drop");
                for(size_t gi=3;gi<grp.size();++gi){ if(gi>3) t += (gi+1==grp.size()? " "+AND+" " : std::string(", "));
                    std::string h=have_grp(grp[gi]);
                    if(gi>3 && !plan[grp[gi].ix[0]].edu && !plan[grp[gi-1].ix[0]].edu && !drop.empty() && h.rfind(drop,0)==0) h.erase(0,drop.size());   // "... och som kock hos ..."
                    t+=h; }
                paras.push_back(F("also_have", {{"x",t}})); }
            // what the ad asks for and the user has ticked as true: the program writes it, word for word
            std::string conf_all;
            if(!confirmed.empty()){ const std::string t=merge_confirmed(confirmed); paras.push_back(t); conf_all=lower_sv(t);
                say("annonsens krav, ikryssade: "+std::to_string(confirmed.size())); }
            { std::string t;   // traits: the model chooses three of the user's own, the program writes the sentence
              std::vector<std::string> pick;
              if(base.strengths.size()>3){
                  std::string opts; for(auto& x: base.strengths) opts+="- "+x+"\n";
                  const std::string r=lower_sv(ask("egenskaper", TJ+F("p.traits", {{"list",opts}}), 40, 0));
                  std::vector<std::pair<size_t,size_t>> hits;
                  for(size_t i=0;i<base.strengths.size();++i){ size_t at=r.find(lower_sv(base.strengths[i])); if(at!=std::string::npos) hits.push_back({at,i}); }
                  std::sort(hits.begin(),hits.end()); if(hits.size()>3) hits.resize(3);
                  if(hits.size()>=2) for(auto& h: hits) pick.push_back(base.strengths[h.second]);
              }
              if(pick.empty()) for(size_t i=0;i<base.strengths.size() && i<3;++i) pick.push_back(base.strengths[i]);
              bool nouns=false;   // "god samarbetsförmåga" cannot follow "Som person är jag"
              for(auto& x: pick){ const std::string lx=lower_sv(x)+" ";
                  for(auto& w: L.noun_endings) if(lx.find(w)!=std::string::npos) nouns=true; }
              std::vector<std::string> pl; for(auto& x: pick) pl.push_back(lc1(x));
              if(!pl.empty()) t=F(nouns? "strengths_are" : "as_person", {{"x",lang::join_and(pl,AND)}});
              // a licence the ad asks for is stated by the program
              std::vector<std::string> ql; for(auto& q: base.qualifications){ const std::string lq=lower_sv(q);
                  if(conf_all.find(lq.substr(0,lq.find(' ')))!=std::string::npos) continue;   // said among the ticked sentences already
                  if(line_match(q,tq,tparts)>0.0) ql.push_back(lc1(q)); }
              if(!ql.empty()) t+=std::string(t.empty()? "" : " ")+F("licences", {{"x",lang::join_and(ql,AND)}});
              paras.push_back(t+std::string(t.empty()? "" : " ")+L.str("references")); }
            // the CV summary: the user's own, plus one sentence by the program naming what was chosen for this job
            std::string l_sum=base.summary;
            { while(!l_sum.empty() && (l_sum.back()==' '||l_sum.back()=='\n')) l_sum.pop_back();
              if(!l_sum.empty() && l_sum.back()!='.' && l_sum.back()!='!') l_sum+='.';
              // the ones that have a paragraph of their own, each title once: "arbete som lagerarbetare och kock samt utbildning som ..."
              std::vector<std::string> js, es;
              for(size_t gi=0;gi<grp.size() && gi<3;++gi){ const size_t k=grp[gi].ix[0]; const std::string t0=short_title(*plan[k].r);
                  std::vector<std::string>& v = plan[k].edu? es : js; const std::string x = !plan[k].edu? t0 : is_course(t0)? t0 : F("summary_edu", {{"title",t0}});
                  if(std::find(v.begin(),v.end(),x)==v.end()) v.push_back(x); }
              std::string rel = js.empty()? std::string() : F("summary_jobs", {{"x",lang::join_and(js,AND)}});
              if(!es.empty()) rel += std::string(rel.empty()? "" : " "+ALSO+" ")+lang::join_and(es,AND);
              if(!rel.empty()) l_sum += std::string(l_sum.empty()? "" : " ")+F("summary_relevant", {{"x",rel}}); }
            ses.stop();
            if(failed){ std::printf("[tailor] lokal modell: %s\n", gerr.c_str()); { std::ofstream(P("work\\last_error.txt")) << gerr; } return 1; }
            std::string l_int; for(auto& x: base.interests) if(line_match(x,tq,tparts)>=1.0){ if(!l_int.empty()) l_int+=", "; l_int+=x; }   // interests by the word match
            outtxt="[INGRESS]\n"+l_sum+"\n[/INGRESS]\n";
            if(want_detail) outtxt+="[DETALJ]\n"+l_det+"\n[/DETALJ]\n";
            outtxt+="[BREV]\n"; for(size_t k=0;k<paras.size();++k){ if(k) outtxt+="\n\n"; outtxt+=paras[k]; }
            outtxt+="\n[/BREV]\n[INTRESSEN]\n"+l_int+"\n[/INTRESSEN]\n[EGENSKAPER]\n\n[/EGENSKAPER]\n";
        }
        if(outtxt.empty()){ std::printf("[tailor] molnfel: %s\n", gerr.c_str());
            { std::ofstream(P("work\\last_error.txt")) << gerr; } return 1; }
        std::printf("%s\n", outtxt.c_str());
    } else {
        std::printf("[tailor] laddar Qwen lokalt (Genie, 4096-kontext)...\n"); std::fflush(stdout);
        // the bundle: next to the program, or where settings.json says ("qwen_bundle") when the models live elsewhere
        std::string B4K=P("models\\qwen3-4b-4k\\genie_bundle"); std::error_code bec;
        if(!std::filesystem::exists(std::filesystem::u8path(B4K+"\\Genie.dll"),bec)){
            std::ifstream sf(std::filesystem::u8path(docs_dir()+"\\settings.json"));
            try{ nlohmann::json sj; if(sf) sf>>sj; std::string alt=sj.value("qwen_bundle",std::string()); if(!alt.empty()) B4K=alt; }catch(...){}
        }
        if(!std::filesystem::exists(std::filesystem::u8path(B4K+"\\Genie.dll"),bec)){
            std::printf("[tailor] Qwen-bundlen saknas: %s\n", B4K.c_str());
            { std::ofstream(P("work\\last_error.txt")) << "The built-in Qwen model was not found (" << B4K << "). Put it in models\\qwen3-4b-4k next to the program, "
                 "or give its folder as \"qwen_bundle\" in settings.json."; }
            return 1; }
        std::printf("[tailor] Qwen-bundle: %s\n", B4K.c_str());
        GeniePipeline eng;
        if(!eng.load(B4K+"\\Genie.dll", B4K+"\\genie_config.json")){ std::printf("[tailor] Qwen-load fail\n"); return 1; }
        eng.set_sampler(/*temp=*/0.2f, /*top_p=*/0.85f, /*top_k=*/20, /*rep=*/1.1f);   // low temp = factual; (no-op on 4k → genie_config temp)
        std::printf("[tailor] genererar (riktar mot: %s)...\n", role.c_str()); std::fflush(stdout);
        outtxt = eng.generate(prompt, [&](const std::string& c){ std::printf("%s",c.c_str()); std::fflush(stdout); });
    }

    { std::ofstream(P("work\\last_raw.txt")) << outtxt; }   // debug: raw model output
    // Empty output = the NPU produced nothing (usually HTP contention: another app holds the Genie/NPU).
    // Abort with a DISTINCT code (2) BEFORE overwriting drafts/PDFs, so the GUI can show an honest error
    // instead of a false "Done!" over stale drafts.
    if(outtxt.find_first_not_of(" \t\r\n")==std::string::npos){
        std::printf("[tailor] TOM generering \xE2\x80\x94 NPU upptagen? (inget skrivet)\n"); return 2; }
    outtxt = normalize_markers(outtxt);
    ingress = section(outtxt,"[INGRESS]",  {"[/INGRESS]","[DETALJ]","[BREV]","[INTRESSEN]"});
    brev    = section(outtxt,"[BREV]",     {"[/BREV]","[INTRESSEN]","[INGRESS]"});
    intr    = section(outtxt,"[INTRESSEN]",{"[/INTRESSEN]","[EGENSKAPER]","[BREV]","[INGRESS]"});
    egen = section(outtxt,"[EGENSKAPER]",{"[/EGENSKAPER]","[INTRESSEN]","[BREV]","[INGRESS]"});
    std::string detalj = section(outtxt,"[DETALJ]",{"[/DETALJ]","[BREV]","[INTRESSEN]","[INGRESS]","[EGENSKAPER]"});
    // The 4B sometimes MISSPELLS a tag (e.g. "[interessen]" with an extra e) so section() can't use it as a
    // boundary and it — plus its text — leaks into the letter. A cover letter never contains '[', so truncate there.
    { auto cut=[](std::string& x){ auto b=x.find('['); if(b!=std::string::npos){ x.erase(b);
        while(!x.empty()&&(x.back()=='\n'||x.back()==' '||x.back()=='\r')) x.pop_back(); } };
      cut(ingress); cut(brev); }
    if(ingress.empty()) ingress = base.summary;                       // fallback
    if(brev.empty())    brev = outtxt;                                // fallback: whatever it wrote
    { int nd=0; brev=tidy_paragraphs(brev,&nd); if(nd) std::printf("[tailor] %d upprepade/avbrutna stycken borttagna ur brevet\n", nd); }

    // Proofreading pass: DETERMINISTIC Swedish fixer. (An LLM self-proofread with the same 4B proved unreliable — it
    // partially echoed and even degraded text, e.g. dropping the å in "målade". A curated safe-substitution table fixes
    // the recurring mechanical slips predictably without touching meaning. Easy to extend as new quirks show up.)
    std::printf("[tailor] korrekturläser (deterministiskt)...\n"); std::fflush(stdout);
    ingress = svefix(ingress);
    brev    = svefix(brev);
    { std::string dhow; std::string op_final = finalize_opening(opening, detalj, ad, &dhow);
      if(!dhow.empty()) std::printf("[tailor] detalj: %s\n", dhow.c_str());
      // A detail that was turned down must not survive inside the model's own first sentence: when the letter starts
      // with the opening and runs on, that whole sentence becomes the opening as it stands.
      if(dhow.find("luckan borttagen")!=std::string::npos && !op_final.empty()){
          std::string core=op_final; while(!core.empty() && (core.back()=='.'||core.back()=='!'||core.back()==' ')) core.pop_back();
          size_t a=brev.find_first_not_of(" \t\r\n");
          if(a!=std::string::npos && brev.compare(a,core.size(),core)==0){
              size_t e=std::string::npos;
              for(size_t k=a+core.size(); k+1<brev.size(); ++k) if((brev[k]=='.'||brev[k]=='!'||brev[k]=='?') && (brev[k+1]==' '||brev[k+1]=='\n'||brev[k+1]=='\r')){ e=k; break; }
              if(e!=std::string::npos && e>a+core.size()){ brev = op_final + brev.substr(e+1); std::printf("[tailor] modellens egen fortsattning pa inledningen struken\n"); } } }
      const char* how=""; brev = enforce_opening(brev, op_final, &how);
      if(!op_final.empty()) std::printf("[tailor] inledning: %s\n", how);
      op_used = op_final; std::ofstream(P("work\\opening_used.txt")) << op_used; }
    }   // end !fromdrafts
    // Cliche filter (both on a generate and on a re-render of edited drafts). work\cliches.txt is always rewritten
    // so the GUI never shows a stale list.
    { std::string adscan=ad;
      if(fromdrafts){ std::ifstream fa(P("work\\career_ad.txt")); std::string s((std::istreambuf_iterator<char>(fa)),{}); if(!s.empty()) adscan=s;
                      std::ifstream fo(P("work\\opening_used.txt")); std::getline(fo,op_used,'\0'); }
      std::string body=brev; size_t a=body.find_first_not_of(" \t\r\n"); if(a!=std::string::npos) body.erase(0,a);
      if(!op_used.empty() && body.compare(0,op_used.size(),op_used)==0) body.erase(0,op_used.size());
      std::string cl=find_cliches(ingress+"\n"+body, adscan);
      std::error_code wec; std::filesystem::create_directories(P("work"),wec);
      std::ofstream(P("work\\cliches.txt")) << cl;
      std::printf("[tailor] klicheer: %s\n", cl.empty()? "inga" : cl.c_str());
      std::string facts; { std::ifstream ff(P("work\\facts.txt")); std::getline(ff,facts,'\0'); }
      std::string ub = facts.empty()? std::string() : find_unbacked(ingress+"\n"+brev, adscan, facts, role, company);
      std::ofstream(P("work\\claims.txt")) << ub;
      std::printf("[tailor] ur annonsen men inte ur meriterna: %s\n", ub.empty()? "inget" : ub.c_str());
      // what the user chose to build the letter on must be IN the letter: a title counts as mentioned when one of its
      // words of five letters or more (by its first six) is there
      std::string miss; { std::ifstream lf(P("work\\lead_used.txt")); std::string t; const std::string lb=lower_sv(brev);
        while(std::getline(lf,t)){ while(!t.empty()&&(t.back()=='\r'||t.back()==' ')) t.pop_back(); if(t.empty()) continue;
            bool found=false, any=false; for(auto& w: sv_words(t)) if(cp_len(w)>=5){ any=true; if(lb.find(stem6(w))!=std::string::npos) found=true; }
            if(!any) found = lb.find(lower_sv(t))!=std::string::npos;
            if(!found){ if(!miss.empty()) miss+=", "; miss+=t; } } }
      std::ofstream(P("work\\missing.txt")) << miss;
      std::printf("[tailor] valt men inte i brevet: %s\n", miss.empty()? "inget" : miss.c_str()); }

    std::error_code ec; std::string DOWN=downloads_dir();
    std::filesystem::create_directories(DOWN, ec); std::filesystem::create_directories(P("work"), ec);
    // Numbered per-application output: 01_CV.pdf + 01_PB.pdf, 02_… for the next job.
    // A full generate takes the next free number; --from-drafts reuses the last one (so edits overwrite the same pair).
    std::string NSTATE=P("work\\cv_n.txt");
    int idx=1;
    if(fromdrafts){ std::ifstream sf(NSTATE); sf>>idx; if(idx<=0) idx=1; }
    else {
        namespace fsx=std::filesystem;
        if(fsx::exists(DOWN,ec)) for(auto& e: fsx::directory_iterator(DOWN,ec)){
            std::string fn=e.path().filename().string();
            if(fn.find("_CV")!=std::string::npos && fn.size()>4 && fn.substr(fn.size()-4)==".pdf"){ int n=std::atoi(fn.c_str()); if(n>=idx) idx=n+1; }
        }
        std::ofstream(NSTATE)<<idx;
    }
    char pfx[8]; std::snprintf(pfx,sizeof(pfx),"%02d",idx);
    // surname from the candidate's name → 01_CV_<Efternamn>.pdf / 01_PB_<Efternamn>.pdf
    std::string sur; { auto sp=base.name.find_last_of(' '); sur = (sp==std::string::npos)? base.name : base.name.substr(sp+1);
        std::string clean; for(unsigned char c:sur){ if(c=='\\'||c=='/'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|'||c==' ') continue; clean+=(char)c; } sur=clean; }
    std::string suf = sur.empty()? "" : "_"+sur;
    std::string CVPATH=DOWN+"\\"+pfx+"_CV"+suf+".pdf";
    std::string PBPATH=DOWN+"\\"+pfx+"_PB"+suf+".pdf";
    // editable drafts — tweak these and re-render (the GUI does this in text fields)
    { std::ofstream(P("work\\ingress.txt")) << ingress;
      std::ofstream(P("work\\brev.txt")) << brev;
      std::printf("[tailor] redigerbara utkast: work\\ingress.txt, work\\brev.txt\n"); }

    // tailored CV. INTERESTS: honesty first — keep only the user's REAL interests, but let Qwen EXCLUDE the ones
    // it judged irrelevant (it may only filter, never invent; unmatched Qwen items are dropped). Fallback = all real.
    CvData cv=base; cv.summary=ingress; cv.subtitle=lang::fmt(g_L.str("l.tailored_cv"), {{"role",role}}); cv.lb=labels_of(g_L);
    cv.aR=acc[0]; cv.aG=acc[1]; cv.aB=acc[2];
    // The user's ticks ("Build the letter on") order the CV too: the chosen jobs first and in full, the others compact.
    // A re-render has no --lead: it takes the choice the last Generate was made with.
    { std::vector<std::string> lt=lead_titles;
      if(lt.empty() && fromdrafts){ std::ifstream lf(P("work\\lead_used.txt")); std::string l;
          while(std::getline(lf,l)){ while(!l.empty()&&(l.back()=='\r'||l.back()==' ')) l.pop_back(); if(!l.empty()) lt.push_back(l); } }
      auto chosen_first=[&](std::vector<Row>& v){ std::vector<Row> out; std::vector<bool> used(v.size(),false);
          for(auto& t: lt) for(size_t i=0;i<v.size();++i) if(!used[i] && v[i].title==t){ out.push_back(v[i]); used[i]=true; }
          const size_t n=out.size(); for(size_t i=0;i<v.size();++i) if(!used[i]) out.push_back(v[i]);
          v.swap(out); return n; };
      cv.lead_exp=chosen_first(cv.experience); cv.lead_edu=chosen_first(cv.education); }
    // Editable interests/strengths: on --from-drafts take them VERBATIM from the drafts the GUI wrote
    // (the user typed exactly what should appear on the CV — human-in-the-loop; this bypasses the Qwen
    // honesty filter below, which only runs on a fresh generate). Comma- or newline-separated.
    if(fromdrafts){
        auto split_list=[](const std::string& s){ std::vector<std::string> v; std::string cur;
            auto push=[&]{ size_t a=cur.find_first_not_of(" \t\r\n"); size_t b=cur.find_last_not_of(" \t\r\n");
                if(a!=std::string::npos) v.push_back(cur.substr(a,b-a+1)); cur.clear(); };
            for(char c:s){ if(c==','||c=='\n') push(); else cur+=c; } push(); return v; };
        { std::ifstream f(P("work\\interests.txt")); std::string s((std::istreambuf_iterator<char>(f)),{});
          if(!s.empty()){ auto v=split_list(s); if(!v.empty()) cv.interests=v; } }
        { std::ifstream f(P("work\\strengths.txt")); std::string s((std::istreambuf_iterator<char>(f)),{});
          if(!s.empty()){ auto v=split_list(s); if(!v.empty()) cv.strengths=v; } }
    }
    if(!intr.empty()){ std::string lin=intr; for(char&c:lin) if(c>='A'&&c<='Z')c=char(c+32);
        std::vector<std::string> kept;
        for(auto& real:base.interests){ std::string w; for(char c:real){ if(c==' '||c=='&'){ if(w.size()>=4){ std::string lw=w; for(char&c2:lw)if(c2>='A'&&c2<='Z')c2=char(c2+32);
                if(lin.find(lw)!=std::string::npos){ kept.push_back(real); w.clear(); break; } } w.clear(); } else w+=c; }
            if(!w.empty()&&w.size()>=4){ std::string lw=w; for(char&c2:lw)if(c2>='A'&&c2<='Z')c2=char(c2+32);
                if(lin.find(lw)!=std::string::npos && (kept.empty()||kept.back()!=real)) kept.push_back(real); } }
        if(!kept.empty()) cv.interests=kept; }   // else keep all real interests
    // STRENGTHS matching (same honest rule): keep only my REAL strengths that Qwen picked as relevant; never invent. Fallback = all.
    if(!egen.empty()){ std::string leg=egen; for(char&c:leg) if(c>='A'&&c<='Z')c=char(c+32);
        std::vector<std::string> kept;
        for(auto& real:base.strengths){ std::string w; for(char c:real){ if(c==' '||c=='('){ if(w.size()>=4){ std::string lw=w; for(char&c2:lw)if(c2>='A'&&c2<='Z')c2=char(c2+32);
                if(leg.find(lw)!=std::string::npos){ kept.push_back(real); w.clear(); break; } } w.clear(); } else w+=c; }
            if(!w.empty()&&w.size()>=4){ std::string lw=w; for(char&c2:lw)if(c2>='A'&&c2<='Z')c2=char(c2+32);
                if(leg.find(lw)!=std::string::npos && (kept.empty()||kept.back()!=real)) kept.push_back(real); } }
        if(!kept.empty()) cv.strengths=kept; }   // else keep all real strengths
    // Sync the editable interests/strengths drafts to what's actually on the CV, so after a generate
    // the GUI shows them and the user can freely edit → Re-render (e.g. swap in ad-matching interests).
    { std::string si; for(size_t i=0;i<cv.interests.size();++i){ if(i)si+=", "; si+=cv.interests[i]; }
      std::ofstream(P("work\\interests.txt"))<<si;
      std::string ss; for(size_t i=0;i<cv.strengths.size();++i){ if(i)ss+=", "; ss+=cv.strengths[i]; }
      std::ofstream(P("work\\strengths.txt"))<<ss; }
    { Pdf d; int total;
      if(layout=="creative"){ total=cv_render_creative(d,cv,1); }
      else { Pdf tmp; total=cv_render(tmp,cv,0); cv_render(d,cv,total); }
      d.save(CVPATH); std::printf("[tailor] CV (%s) -> %s (%d sidor)\n",layout.c_str(),CVPATH.c_str(),total); }
    // cover letter
    std::time_t t=std::time(nullptr); char ds[16]; std::strftime(ds,sizeof(ds),"%Y-%m-%d",std::localtime(&t));
    LetterData L; L.lb=labels_of(g_L); L.name=base.name; L.address=base.address; L.phone=base.phone; L.email=base.email;
    L.place=g_place; L.date=ds; L.company=company; L.role=role; L.body=brev; L.aR=acc[0]; L.aG=acc[1]; L.aB=acc[2];
    { Pdf d; cv_render_letter(d,L); d.save(PBPATH); std::printf("[tailor] Brev -> %s\n",PBPATH.c_str()); }
    std::fflush(stdout); return 0;
}

// Generic fallback CV (used only if data/cv_data.json is missing — real data lives in that editable file).
CvData builtin_cv(bool academic){
    CvData cv;
    cv.name="Ditt Namn";
    cv.address="Gatuadress, Postnr Ort"; cv.phone="070-000 00 00"; cv.email="din.epost@exempel.se";
    cv.qualifications={"B-körkort"};
    cv.subtitle = academic ? "Akademiskt CV" : "Praktiskt CV";
    cv.summary  = academic ? "Kort sammanfattning som lyfter din analytiska/akademiska profil."
                           : "Kort sammanfattning som lyfter din praktiska profil.";
    cv.experience={ {"20XX – 20XX","Roll","Arbetsgivare, Ort"} };
    cv.education ={ {"20XX – 20XX", academic?"Utbildning (hp)":"Utbildning", academic?"Lärosäte":"Skola/Ort"} };
    cv.interests={"Intresse 1","Intresse 2"};
    cv.strengths={"Egenskap 1","Egenskap 2","Egenskap 3"};
    return cv;
}

// Load the chosen profile from the editable cv_data.json (so the user maintains their history without a recompile).
// Fills `out` only on full success; returns false (leaving the built-in fallback untouched) on any error.
static bool load_cv_json(const std::string& path, const std::string& profkey, CvData& out){
    std::ifstream f(path); if(!f) return false;
    try{
        nlohmann::json j; f>>j;
        if(!j.contains("profiles") || !j["profiles"].contains(profkey)) return false;
        auto& p=j["profiles"][profkey];
        CvData cv;
        cv.name=j.value("name","Ditt Namn"); cv.address=j.value("address","");
        cv.phone=j.value("phone",""); cv.email=j.value("email","");
        g_place=j.value("place",std::string()); if(g_place.empty()) g_place=place_from_address(cv.address);
        cv.subtitle=p.value("subtitle","CV"); cv.summary=p.value("summary","");
        for(auto& e:p.value("experience",nlohmann::json::array()))
            cv.experience.push_back({e.value("date",std::string()),e.value("title",std::string()),e.value("org",std::string()),e.value("note",std::string())});
        for(auto& e:p.value("education",nlohmann::json::array()))
            cv.education.push_back({e.value("date",std::string()),e.value("title",std::string()),e.value("org",std::string()),e.value("note",std::string())});
        for(auto& q:p.value("qualifications",nlohmann::json::array())) cv.qualifications.push_back(q.get<std::string>());
        for(auto& in:p.value("interests",nlohmann::json::array()))     cv.interests.push_back(in.get<std::string>());
        for(auto& s:p.value("strengths",nlohmann::json::array()))      cv.strengths.push_back(s.get<std::string>());
        for(auto& t:p.value("personality",nlohmann::json::array())) if(t.is_object()) cv.personality.push_back({t.value("label",std::string()), t.value("value",0.0)});
        if(cv.experience.empty()) return false;
        out=cv; return true;
    }catch(...){ return false; }
}
