// ad_fetch.hpp — a link to a job ad -> role, employer, ad text.
//   1. Platsbanken (arbetsformedlingen.se): the open JobTech API, by the ad's number.
//   2. Any other site: the page's schema.org "JobPosting" data (JSON-LD), which most job sites embed for search engines.
//   3. Failing that: the page's own text, as it is - the user trims what is not the ad.
// One GET of the page the user pasted; nothing else is fetched and nothing is sent.
#pragma once
#include "cloud_llm.hpp"

namespace ad_fetch {

struct Ad { std::string role, company, text; std::string how; };   // how: "platsbanken" | "jobposting" | "page"

inline void replace_all(std::string& s, const std::string& a, const std::string& b){
    for(size_t p=0;(p=s.find(a,p))!=std::string::npos;p+=b.size()) s.replace(p,a.size(),b);
}
inline void put_utf8(std::string& o, unsigned cp){
    if(cp<0x80) o+=(char)cp;
    else if(cp<0x800){ o+=(char)(0xC0|(cp>>6)); o+=(char)(0x80|(cp&0x3F)); }
    else if(cp<0x10000){ o+=(char)(0xE0|(cp>>12)); o+=(char)(0x80|((cp>>6)&0x3F)); o+=(char)(0x80|(cp&0x3F)); }
    else { o+=(char)(0xF0|(cp>>18)); o+=(char)(0x80|((cp>>12)&0x3F)); o+=(char)(0x80|((cp>>6)&0x3F)); o+=(char)(0x80|(cp&0x3F)); }
}
inline std::string decode_entities(const std::string& s){
    static const std::pair<const char*,unsigned> NAMED[] = {{"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",' '},
        {"aring",0xE5},{"auml",0xE4},{"ouml",0xF6},{"Aring",0xC5},{"Auml",0xC4},{"Ouml",0xD6},{"eacute",0xE9},{"Eacute",0xC9},{"uuml",0xFC},
        {"ndash",0x2013},{"mdash",0x2014},{"bull",0x2022},{"hellip",0x2026},{"rsquo",0x2019},{"lsquo",0x2018},{"rdquo",0x201D},{"ldquo",0x201C}};
    std::string o; o.reserve(s.size());
    for(size_t i=0;i<s.size();++i){
        if(s[i]!='&'){ o+=s[i]; continue; }
        size_t e=s.find(';',i); if(e==std::string::npos || e-i>10){ o+='&'; continue; }
        const std::string name=s.substr(i+1,e-i-1); unsigned cp=0; bool ok=false;
        if(name.size()>1 && name[0]=='#'){ try{ cp = (name[1]=='x'||name[1]=='X')? (unsigned)std::stoul(name.substr(2),nullptr,16) : (unsigned)std::stoul(name.substr(1)); ok=cp>0; }catch(...){} }
        else for(auto& n: NAMED) if(name==n.first){ cp=n.second; ok=true; break; }
        if(ok){ put_utf8(o, cp==0xA0? ' ' : cp); i=e; } else o+='&';
    }
    return o;
}
inline std::string lower_ascii(std::string s){ for(char& c: s) if(c>='A'&&c<='Z') c=char(c+32); return s; }
// HTML -> readable text: scripts and styles go, block ends become line breaks, list items get a dash.
inline std::string html_to_text(const std::string& html){
    std::string o; const std::string low=lower_ascii(html);
    for(size_t i=0;i<html.size();){
        if(html[i]!='<'){ o+=html[i++]; continue; }
        size_t e=html.find('>',i); if(e==std::string::npos) break;
        std::string tag=low.substr(i+1,e-i-1); size_t sp=tag.find_first_of(" \t\r\n/",tag[0]=='/'?1:0); std::string name=tag.substr(0,sp);
        if(name=="script"||name=="style"||name=="noscript"||name=="svg"||name=="head"){ size_t c=low.find("</"+name,e); if(c==std::string::npos) break; e=low.find('>',c); if(e==std::string::npos) break; }
        else if(name=="br"||name=="/p"||name=="/div"||name=="/li"||name=="/tr"||name=="/ul"||name=="/ol"||name=="/section"
             ||(name.size()==3&&name[0]=='/'&&name[1]=='h')) o+='\n';
        else if(name=="li") o+="- ";
        else if(name=="p"||(name.size()==2&&name[0]=='h'&&name[1]>='1'&&name[1]<='6')) o+='\n';
        i=e+1;
    }
    o=decode_entities(o);
    std::string t; int nl=0; bool sp=false;                 // collapse runs of spaces and of blank lines
    for(char c: o){
        if(c=='\r') continue;
        if(c=='\n'){ while(!t.empty()&&t.back()==' ') t.pop_back(); if(++nl<=2 && !t.empty()) t+='\n'; sp=false; continue; }
        if(c==' '||c=='\t'){ sp=true; continue; }
        if(sp && !t.empty() && t.back()!='\n') t+=' ';
        sp=false; nl=0; t+=c;
    }
    while(!t.empty()&&(t.back()=='\n'||t.back()==' ')) t.pop_back();
    return t;
}
inline bool valid_utf8(const std::string& s){
    for(size_t i=0;i<s.size();){ unsigned char c=(unsigned char)s[i]; int n = c<0x80?0 : (c&0xE0)==0xC0?1 : (c&0xF0)==0xE0?2 : (c&0xF8)==0xF0?3 : -1;
        if(n<0 || i+n>=s.size()+ (n==0?1:0)) return false; for(int k=1;k<=n;k++) if(((unsigned char)s[i+k]&0xC0)!=0x80) return false; i+=n+1; }
    return true;
}
inline std::string from_cp1252(const std::string& s){
    int n=MultiByteToWideChar(1252,0,s.data(),(int)s.size(),nullptr,0); std::wstring w(n,L'\0'); MultiByteToWideChar(1252,0,s.data(),(int)s.size(),&w[0],n);
    int m=WideCharToMultiByte(CP_UTF8,0,w.data(),n,nullptr,0,nullptr,nullptr); std::string o(m,'\0'); WideCharToMultiByte(CP_UTF8,0,w.data(),n,&o[0],m,nullptr,nullptr); return o;
}
// The first JobPosting in a JSON-LD value (an object, an array, or an @graph).
inline bool find_posting(const nlohmann::json& j, nlohmann::json& out){
    if(j.is_array()){ for(auto& x: j) if(find_posting(x,out)) return true; return false; }
    if(!j.is_object()) return false;
    if(j.contains("@type")){ auto& t=j["@type"]; bool is=false;
        if(t.is_string()) is = t.get<std::string>()=="JobPosting";
        else if(t.is_array()) for(auto& x: t) if(x.is_string() && x.get<std::string>()=="JobPosting") is=true;
        if(is){ out=j; return true; } }
    if(j.contains("@graph")) return find_posting(j["@graph"],out);
    return false;
}
inline std::string str_of(const nlohmann::json& j, const char* key){
    if(!j.contains(key)) return {}; auto& v=j[key];
    if(v.is_string()) return v.get<std::string>();
    if(v.is_object() && v.contains("name") && v["name"].is_string()) return v["name"].get<std::string>();
    return {};
}

inline bool fetch(const std::string& url_in, Ad& out, std::string& err){
    std::string url=url_in; { size_t a=url.find_first_not_of(" \t\r\n"), b=url.find_last_not_of(" \t\r\n"); url = a==std::string::npos? std::string() : url.substr(a,b-a+1); }
    if(url.empty()){ err="Paste a link first."; return false; }
    bool digits=true; for(char c: url) if(c<'0'||c>'9') digits=false;
    if(digits || url.find("arbetsformedlingen.se")!=std::string::npos){
        std::string id; for(size_t i=url.size();i-- >0;){ if(url[i]>='0'&&url[i]<='9') id=std::string(1,url[i])+id; else if(!id.empty()) break; }
        if(id.empty()){ err="That Platsbanken link has no ad number in it."; return false; }
        int st=0; std::string body=cloud_llm::http(L"GET","https://jobsearch.api.jobtechdev.se/ad/"+id,{L"Accept: application/json"},"",&st,err,30000);
        if(!err.empty()){ err="Could not reach Platsbanken ("+err+")."; return false; }
        if(st!=200){ err = st==404? "Platsbanken has no ad with that number (it may have been taken down)." : "Platsbanken answered HTTP "+std::to_string(st)+"."; return false; }
        try{ auto j=nlohmann::json::parse(body);
            out.role=j.value("headline",std::string());
            if(j.contains("employer") && j["employer"].is_object()) out.company=j["employer"].value("name",std::string());
            if(j.contains("description") && j["description"].is_object()) out.text=j["description"].value("text",std::string());
        }catch(...){ err="Platsbanken's answer could not be read."; return false; }
        out.how="platsbanken";
        if(out.text.empty()){ err="The ad came without text \xE2\x80\x94 paste it by hand."; return false; }
        return true;
    }
    if(url.rfind("http://",0)!=0 && url.rfind("https://",0)!=0) url="https://"+url;
    int st=0; std::string page=cloud_llm::http(L"GET",url,{L"Accept: text/html,application/xhtml+xml",L"Accept-Language: sv,en;q=0.8"},"",&st,err,30000);
    if(!err.empty()){ err="Could not read the page ("+err+"). Paste the ad's text instead."; return false; }
    if(st<200||st>=300){ err="The site answered HTTP "+std::to_string(st)+(st==403||st==401||st==429? " \xE2\x80\x94 it does not let an app read the page" : "")+". Paste the ad's text instead."; return false; }
    if(page.size()>4000000) page.resize(4000000);
    if(!valid_utf8(page)) page=from_cp1252(page);
    const std::string low=lower_ascii(page);
    // 2. JobPosting data
    for(size_t p=0;(p=low.find("application/ld+json",p))!=std::string::npos;){
        size_t a=page.find('>',p), b = a==std::string::npos? a : low.find("</script",a); if(b==std::string::npos) break; p=b;
        nlohmann::json j=nlohmann::json::parse(page.substr(a+1,b-a-1),nullptr,false), jp;
        if(j.is_discarded() || !find_posting(j,jp)) continue;
        out.role=decode_entities(str_of(jp,"title")); out.company=decode_entities(str_of(jp,"hiringOrganization"));
        std::string d=str_of(jp,"description"); d=html_to_text(d); if(d.find('<')!=std::string::npos && d.find('>')!=std::string::npos) d=html_to_text(d);   // escaped twice on some sites
        out.text=d; out.how="jobposting";
        if(!out.text.empty()) return true;
    }
    // 3. the page's own text: <main> or <article> when there is one, else the body
    size_t a=low.find("<main"); if(a==std::string::npos) a=low.find("<article"); if(a==std::string::npos) a=low.find("<body"); if(a==std::string::npos) a=0;
    size_t b=low.find(low.compare(a,5,"<main")==0? "</main" : low.compare(a,8,"<article")==0? "</article" : "</body", a); if(b==std::string::npos) b=page.size();
    out.text=html_to_text(page.substr(a,b-a));
    { size_t t0=low.find("<title"), t1 = t0==std::string::npos? t0 : low.find("</title",t0);
      if(t1!=std::string::npos){ size_t g=page.find('>',t0); out.role=html_to_text(page.substr(g+1,t1-g-1)); } }
    out.how="page";
    if(out.text.size()<300){ err="That page shows its ad only inside a browser, so there was nothing to read. Paste the ad's text instead."; return false; }
    return true;
}

} // namespace ad_fetch
