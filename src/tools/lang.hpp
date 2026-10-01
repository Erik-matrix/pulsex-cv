// lang.hpp — the language pack (lang\sv.json, lang\en.json, ...): everything the program knows about a language.
//
//   prioritising   what work demands, and the words by which an ad or the user's own facts show it
//   spelling       which of Windows' spell-check dictionaries to ask; ordinary word endings and filler words
//   suggestions    what an ad asks for, as sentences the user can tick; the sentences the program writes itself
//   prompts        the questions to a model file on this PC
//   labels         the headings on the CV and the letter
//
// The window and the tailor read the same files. A language is added by copying a file - no code changes.
#pragma once
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace lang {

struct Demand     { std::string say, shown_by, occupations; };
struct Variant    { std::string when, say; };
struct Part       { std::string when, word; };
struct AskRule    { std::string id, when, say; std::vector<Variant> variants; std::vector<Part> parts; };
struct Lead       { std::string find, say; };
struct Covered    { std::string when, id; };
struct CourseVerb { std::string when, verb; };
struct Ask        { std::string id, text; };   // what the window shows: a stable id and the sentence for the letter

struct Pack {
    bool ok=false; std::string code, file, error;
    std::vector<std::string> detect;
    std::vector<Demand> demands; std::vector<std::string> joints;
    std::vector<AskRule> asks; std::vector<Lead> exp_leads; std::vector<std::string> exp_stops; std::vector<Covered> exp_covered;
    size_t exp_max_words=8, exp_max_finds=5;
    std::vector<std::string> merge_prefixes, endings, fill_words, spell_tags;
    std::vector<std::string> i_words, job_markers, cut_tails, grade_words, noun_endings;
    std::vector<CourseVerb> course_verbs;
    std::map<std::string,std::string> s;      // words.*, sentences.*, prompts.* ("p.<name>") and labels.* ("l.<name>")
    const std::string& str(const std::string& k) const { static const std::string none; auto it=s.find(k); return it==s.end()? none : it->second; }
};

// lower case, ASCII and the Latin-1 letters in UTF-8 (Å Ä Ö É ...); byte length is kept
inline std::string lower(const std::string& in){
    std::string r; r.reserve(in.size());
    for(size_t i=0;i<in.size();++i){ unsigned char c=(unsigned char)in[i];
        if(c==0xC3 && i+1<in.size()){ unsigned char d=(unsigned char)in[i+1]; if(d>=0x80 && d<=0x9E && d!=0x97) d=(unsigned char)(d+0x20);
            r+=(char)c; r+=(char)d; ++i; continue; }
        r += (c>='A'&&c<='Z')? char(c+32) : (char)c; }
    return r;
}
inline bool is_letter(unsigned char b){ return (b>='a'&&b<='z')||(b>='A'&&b<='Z')||b>=0x80; }

// is one of "a|*b|c" in a lower-case text?   a: at the start of a word;   *b: anywhere, also inside a word
inline bool has(const std::string& text, const std::string& keys){
    size_t k0=0;
    while(k0<keys.size()){ size_t k1=keys.find('|',k0); if(k1==std::string::npos) k1=keys.size();
        std::string key=keys.substr(k0,k1-k0); k0=k1+1;
        const bool inside = !key.empty() && key[0]=='*'; if(inside) key.erase(0,1);
        if(key.empty()) continue;
        for(size_t at=text.find(key); at!=std::string::npos; at=text.find(key,at+1))
            if(inside || at==0 || !is_letter((unsigned char)text[at-1])) return true; }
    return false;
}

// "a, b och c"
inline std::string join_and(const std::vector<std::string>& v, const std::string& and_word){
    std::string o; for(size_t k=0;k<v.size();++k){ if(k) o += (k+1==v.size()? " "+and_word+" " : std::string(", ")); o+=v[k]; } return o;
}

// Fill a sentence: {name} is the value; {a:name} is "a" or "an" for it; a part in [[ ]] is left out when a value in it is empty.
inline std::string fmt(std::string t, const std::map<std::string,std::string>& v){
    auto value=[&](const std::string& name, bool& known){ auto it=v.find(name); known = it!=v.end(); return known? it->second : std::string(); };
    auto fill=[&](const std::string& in, bool* any_empty){
        std::string out; size_t i=0;
        while(i<in.size()){
            if(in[i]=='{'){ const size_t e=in.find('}',i);
                if(e!=std::string::npos){ std::string name=in.substr(i+1,e-i-1); const bool art = name.rfind("a:",0)==0; if(art) name.erase(0,2);
                    bool known=false; const std::string val=value(name,known);
                    if(known){ if(val.empty() && any_empty) *any_empty=true;
                        if(!art) out+=val;
                        else if(!val.empty()) out += std::strchr("aeiouAEIOU",val[0])? "an" : "a";
                        i=e+1; continue; } } }
            out+=in[i]; ++i; }
        return out; };
    std::string r; size_t i=0;
    while(i<t.size()){ const size_t a=t.find("[[",i);
        if(a==std::string::npos){ r+=fill(t.substr(i),nullptr); break; }
        const size_t b=t.find("]]",a); if(b==std::string::npos){ r+=fill(t.substr(i),nullptr); break; }
        r+=fill(t.substr(i,a-i),nullptr);
        bool empty=false; const std::string seg=fill(t.substr(a+2,b-a-2),&empty); if(!empty) r+=seg;
        i=b+2; }
    return r;
}

inline Pack load(const std::string& dir, const std::string& code){
    Pack p; p.code=code; p.file=dir+"\\"+code+".json";
    std::ifstream f(std::filesystem::u8path(p.file));
    if(!f){ p.error="The language file "+p.file+" was not found."; return p; }
    nlohmann::json j;
    try{ f>>j; }catch(const std::exception& e){ p.error="The language file "+p.file+" cannot be read: "+e.what(); return p; }
    auto strs=[&](const nlohmann::json& a, std::vector<std::string>& out){ if(a.is_array()) for(auto& x: a) if(x.is_string()) out.push_back(x.get<std::string>()); };
    auto S=[](const nlohmann::json& o, const char* k){ return o.is_object()&&o.contains(k)&&o[k].is_string()? o[k].get<std::string>() : std::string(); };
    try{
        strs(j.value("detect",nlohmann::json::array()), p.detect);
        for(auto& d: j.value("demands",nlohmann::json::array())) p.demands.push_back({S(d,"say"),S(d,"shown_by"),S(d,"occupations")});
        strs(j.value("joints",nlohmann::json::array()), p.joints);
        for(auto& a: j.value("asks",nlohmann::json::array())){ AskRule r{S(a,"id"),S(a,"when"),S(a,"say"),{},{}};
            if(a.contains("variants")) for(auto& v: a["variants"]) r.variants.push_back({S(v,"when"),S(v,"say")});
            if(a.contains("parts")) for(auto& v: a["parts"]) r.parts.push_back({S(v,"when"),S(v,"word")});
            p.asks.push_back(r); }
        if(j.contains("experience")){ auto& e=j["experience"];
            if(e.contains("leads")) for(auto& l: e["leads"]) p.exp_leads.push_back({S(l,"find"),S(l,"say")});
            strs(e.value("stops",nlohmann::json::array()), p.exp_stops);
            if(e.contains("covered")) for(auto& c: e["covered"]) p.exp_covered.push_back({S(c,"when"),S(c,"id")});
            p.exp_max_words=e.value("max_words",8); p.exp_max_finds=e.value("max_finds",5); }
        strs(j.value("merge_prefixes",nlohmann::json::array()), p.merge_prefixes);
        if(j.contains("spelling")){ auto& sp=j["spelling"];
            strs(sp.value("endings",nlohmann::json::array()), p.endings); strs(sp.value("fill_words",nlohmann::json::array()), p.fill_words);
            strs(sp.value("windows",nlohmann::json::array()), p.spell_tags); }
        if(j.contains("words")){ auto& w=j["words"];
            strs(w.value("i_words",nlohmann::json::array()), p.i_words); strs(w.value("job_markers",nlohmann::json::array()), p.job_markers);
            strs(w.value("cut_tails",nlohmann::json::array()), p.cut_tails); strs(w.value("grade_words",nlohmann::json::array()), p.grade_words);
            strs(w.value("noun_endings",nlohmann::json::array()), p.noun_endings);
            if(w.contains("course_verbs")) for(auto& c: w["course_verbs"]) p.course_verbs.push_back({S(c,"when"),S(c,"verb")});
            for(auto it=w.begin(); it!=w.end(); ++it) if(it.value().is_string() && it.key()[0]!='_') p.s[it.key()]=it.value().get<std::string>(); }
        for(const char* sec: {"sentences","prompts","labels"}) if(j.contains(sec)){ const std::string pre = std::string(sec)=="prompts"? "p." : std::string(sec)=="labels"? "l." : "";
            for(auto it=j[sec].begin(); it!=j[sec].end(); ++it) if(it.value().is_string() && it.key()[0]!='_') p.s[pre+it.key()]=it.value().get<std::string>(); }
    }catch(const std::exception& e){ p.error="The language file "+p.file+" has an entry of the wrong kind: "+e.what(); return p; }
    // what the program cannot do without
    std::string missing;
    for(const char* k: {"and","and_also","I","have_job","have_job_head","have_job_place","have_edu","have_course","i_have","also_have","start_job","start_edu",
                        "start_course","detail","job_fallback","as_person","strengths_are","licences","references","summary_relevant","summary_jobs","summary_edu",
                        "p.system","p.ad","p.job","p.job_line","p.facts","p.kind_job","p.kind_edu","p.documents","p.item_two","p.item_one","p.item_tail","p.traits",
                        "l.summary","l.experience","l.other_experience","l.education","l.education_courses","l.other","l.qualifications","l.personal_interests",
                        "l.personal_info","l.traits","l.personality","l.interests","l.page","l.page_single","l.tailored_cv","l.application","l.letter","l.regards"})
        if(p.s.find(k)==p.s.end()) missing+=std::string(missing.empty()? "" : ", ")+k;
    if(p.demands.empty()) missing+=std::string(missing.empty()? "" : ", ")+"demands";
    if(p.joints.size()<3) missing+=std::string(missing.empty()? "" : ", ")+"joints (three)";
    if(!missing.empty()){ p.error="The language file "+p.file+" lacks: "+missing; return p; }
    p.ok=true; return p;
}

// Which language is a text in? The pack whose small words ("och", "att" / "the", "and") occur most; ties go to the first.
inline size_t detect(const std::string& text, const std::vector<Pack>& packs){
    const std::string lt=" "+lower(text)+" "; size_t best=0, bi=0;
    for(size_t i=0;i<packs.size();++i){ size_t n=0;
        for(auto& w: packs[i].detect) for(size_t at=lt.find(w); at!=std::string::npos; at=lt.find(w,at+1)) ++n;
        if(n>best){ best=n; bi=i; } }
    return bi;
}

// A word without one ordinary ending (plockning -> plock). At least four bytes stay. Lower case in, lower case out.
inline std::string stem(const Pack& p, std::string w){
    for(auto& suf: p.endings) if(w.size()>=suf.size()+4 && w.compare(w.size()-suf.size(),suf.size(),suf)==0){ w.erase(w.size()-suf.size()); break; }
    return w;
}

// What an ad asks for in practical terms, as sentences the applicant can put their name to. Nothing here is a claim
// until the user has ticked it in the window.
inline std::vector<Ask> asks_for(const Pack& p, const std::string& ad){
    std::vector<Ask> out; const std::string la=lower(ad); if(la.size()<20) return out;
    const std::string and_word=p.str("and");
    for(auto& r: p.asks){
        if(!has(la,r.when)) continue;
        std::string say=r.say; for(auto& v: r.variants) if(has(la,v.when)){ say=v.say; break; }
        std::vector<std::string> parts; for(auto& pt: r.parts) if(has(la,pt.when)) parts.push_back(pt.word);
        out.push_back({r.id, fmt(say, {{"parts", join_and(parts,and_word)}})}); }
    // "erfarenhet av <x>": the ad's own words
    size_t n=0;
    for(auto& L: p.exp_leads) for(size_t at=la.find(L.find); at!=std::string::npos && n<p.exp_max_finds; at=la.find(L.find,at+1)){
        if(at>0 && is_letter((unsigned char)la[at-1])) continue;                      // "ovan vid"
        const size_t b=at+L.find.size(); size_t e=la.find_first_of(".,;:!?\n\r()/", b); if(e==std::string::npos) e=la.size();
        std::string lx=la.substr(b,e-b);
        for(auto& stop: p.exp_stops){ const size_t c=lx.find(stop); if(c!=std::string::npos) lx.erase(c); }
        while(!lx.empty() && lx.back()==' ') lx.pop_back();
        size_t words=0; { bool in=false; for(char c: lx){ if(c==' ') in=false; else if(!in){ in=true; ++words; } } }
        if(lx.size()<4 || words>p.exp_max_words) continue;
        bool covered=false;                                                            // the list above says it already
        for(auto& c: p.exp_covered) if(has(lx,c.when)) for(auto& a: out) if(a.id==c.id) covered=true;
        const std::string id="exp:"+lx;
        for(auto& a: out) if(a.id==id) covered=true;
        if(covered) continue;
        out.push_back({id, fmt(L.say, {{"x", ad.substr(b,lx.size())}})}); ++n; }       // the ad's own capitals ("Excel")
    return out;
}

} // namespace lang
