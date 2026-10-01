// cv_render.hpp — shared CV + cover-letter rendering (classic Swedish template, vector, multi-page).
// Used by pulsecore_cv (static data) and pulsecore_tailor (Qwen-tailored data).
#pragma once
#include "pcore_pdf.hpp"
#include <string>
#include <vector>
using namespace pcore_pdf;

struct Row { std::string date, title, org, note; };   // note = what the job involved: for the model; printed only for the jobs chosen for an application
// The headings and fixed lines, in the language of the application (lang\<code>.json, "labels"); Swedish by default.
struct CvLabels {
    std::string summary="Sammanfattning", experience="Yrkeserfarenhet", other_experience="\xC3\x96vrig yrkeserfarenhet", education="Utbildning",
                education_courses="Utbildning & kurser", other="\xC3\x96vrigt", qualifications="Beh\xC3\xB6righeter",
                personal_interests="Personliga intressen", personal_info="Personlig info", traits="Egenskaper", personality="Personlighet",
                interests="Intressen", page="Sida {n} av {m}", page_single="Sida {n}", application="Ans\xC3\xB6kan: {role}",
                letter="Personligt brev", regards="Med v\xC3\xA4nliga h\xC3\xA4lsningar,";
};
inline std::string cv_put(std::string t, const std::string& key, const std::string& val){ size_t p=t.find(key); if(p!=std::string::npos) t.replace(p,key.size(),val); return t; }
inline std::string cv_page(const CvLabels& lb, int n, int m){ return m>0? cv_put(cv_put(lb.page,"{n}",std::to_string(n)),"{m}",std::to_string(m)) : cv_put(lb.page_single,"{n}",std::to_string(n)); }
struct CvData {
    CvLabels lb;
    std::string name, subtitle, address, phone, email, linkedin, summary;   // linkedin empty = hidden (opt-in)
    std::vector<Row> experience, education;
    std::vector<std::string> qualifications, interests, strengths;          // strengths = traits (e.g. from a personality test)
    std::vector<std::pair<std::string,double>> personality;                 // trait bars (label, value) — e.g. Big Five scores
    double aR=0.09, aG=0.16, aB=0.33;          // accent colour (icons, headings, divider lines) — default deep navy
    // The first lead_exp jobs / lead_edu educations are the ones the user chose for THIS application: they are shown
    // in full, with their note; the other jobs follow as one line each. 0 = no choice made, everything as usual.
    size_t lead_exp=0, lead_edu=0;
};
struct LetterData {
    CvLabels lb;
    std::string name, address, phone, email;   // sender
    std::string place, date, company, role;    // meta + recipient
    std::string body;                          // paragraphs separated by a blank line
    double aR=0.09, aG=0.16, aB=0.33;          // accent colour
};

inline constexpr double NR=0.09, NG=0.16, NB=0.33;      // deep marine navy
inline constexpr double GR=0.42;
inline constexpr double ORGr=0.34, ORGg=0.42, ORGb=0.55;

inline std::string cv_upper(const std::string& s){ std::string r;
    for(size_t i=0;i<s.size();++i){ unsigned char c=s[i];
        if(c==0xC3 && i+1<s.size()){ unsigned char d=s[i+1];   // UTF-8 å/ä/ö → Å/Ä/Ö
            if(d==0xA5||d==0xA4||d==0xB6){ r+=(char)0xC3; r+=(char)(d-0x20); ++i; continue; } r+=(char)c; continue; }
        r += (c>='a'&&c<='z')?char(c-32):(char)c; }
    return r; }
inline std::vector<std::string> cv_wrap(Pdf& d, double maxw, double size, Font f, const std::string& s){
    std::vector<std::string> words; { std::string w; for(char c:s){ if(c==' '){ if(!w.empty()){words.push_back(w);w.clear();} } else w+=c; } if(!w.empty())words.push_back(w); }
    std::vector<std::string> lines; std::string line;
    for(auto& w:words){ std::string t=line.empty()?w:line+" "+w; if(d.textWidth(t,size,f)>maxw && !line.empty()){ lines.push_back(line); line=w; } else line=t; }
    if(!line.empty()) lines.push_back(line); return lines;
}
inline std::string cv_cap1(std::string s){   // a note is typed in lower case; on the CV it starts with a capital
    if(s.empty()) return s; unsigned char c=(unsigned char)s[0];
    if(c>='a'&&c<='z') s[0]=char(c-32);
    else if(c==0xC3 && s.size()>1){ unsigned char d=(unsigned char)s[1]; if(d==0xA5||d==0xA4||d==0xB6) s[1]=char(d-0x20); }
    return s; }
inline double cv_heading(Pdf& d, double x, double y, double xr, const std::string& t, double r=NR,double g=NG,double b=NB){
    d.text(x,y,11.5,HELV_B,cv_upper(t),r,g,b);
    // section underline in a soft tint of the accent colour (tasteful colour, not a collage). line(x0,y0,x1,y1,w,r,g,b)
    d.line(x,y+6,xr,y+6, 0.9, r+(1-r)*0.35, g+(1-g)*0.35, b+(1-b)*0.35); return y+22;
}

inline int cv_render(Pdf& d, const CvData& cv, int total){
    const double W=d.width(), H=d.height(), M=44, XR=W-M, BOT=H-46; int page=1;
    const double ar=cv.aR, ag=cv.aG, ab=cv.aB;                 // accent (icons, headings, lines)
    auto footer=[&](){ std::string s=cv_page(cv.lb,page,total);
        d.textRight(XR,H-28,8,HELV,s,0.55,0.55,0.58); };
    auto brk=[&](double& y,double need){ if(y+need>BOT){ footer(); d.newPage(); ++page; y=52; } };
    d.text(M,54,22,HELV_B,cv_upper(cv.name),ar,ag,ab); d.text(M,72,11,HELV,cv.subtitle,GR,GR,GR);
    double is=11;
    auto cline=[&](double cy,int kind,const std::string& txt){ double tw=d.textWidth(txt,9.5,HELV), ix=XR-tw-is-6;
        if(kind==0)d.icoEmail(ix,cy-is+2,is,ar,ag,ab); else if(kind==1)d.icoPhone(ix,cy-is+2,is,ar,ag,ab);
        else if(kind==2)d.icoPin(ix,cy-is+2,is,ar,ag,ab); else d.icoLinkedIn(ix,cy-is+2,is);
        d.text(XR-tw,cy,9.5,HELV,txt,0.25,0.25,0.25); };
    double hy=46; cline(hy,2,cv.address); cline(hy+16,1,cv.phone); cline(hy+32,0,cv.email);
    double ruleY=hy+44; if(!cv.linkedin.empty()){ cline(hy+48,3,cv.linkedin); ruleY=hy+60; }
    d.line(M,ruleY,XR,ruleY,1.5,ar,ag,ab); double y=ruleY+24;
    y=cv_heading(d,M,y,XR,cv.lb.summary,ar,ag,ab);
    { double pad=10, tw=XR-M-2*pad; auto lines=cv_wrap(d,tw,9.5,HELV,cv.summary); double lh=13, bh=lines.size()*lh+2*pad;
      d.rect(M,y-4,XR-M,bh,0.945,0.96,0.99); d.rect(M,y-4,3,bh,ar,ag,ab);
      double ty=y-4+pad+9; for(auto& L:lines){ d.text(M+pad+4,ty,9.5,HELV,L,0.2,0.2,0.22); ty+=lh; } y=y-4+bh+18; }
    // a row in full; with_note: the chosen ones also say what the job involved
    auto full=[&](const Row& r, bool with_note){
        std::vector<std::string> nl; if(with_note && !r.note.empty()) nl=cv_wrap(d,XR-(M+96),9,HELV,cv_cap1(r.note));
        brk(y,30+12.0*nl.size()); d.text(M,y,9.5,HELV_B,r.date,ar,ag,ab);
        d.text(M+96,y,10.5,HELV_B,r.title,0.12,0.12,0.12); d.text(M+96,y+13,9.5,HELV,r.org,ORGr,ORGg,ORGb);
        if(nl.empty()){ y+=32; return; }
        double ny=y+26; for(auto& L:nl){ d.text(M+96,ny,9,HELV,L,0.25,0.25,0.27); ny+=12; } y=ny+8; };
    // a row on one line (the jobs that were not chosen for this application)
    // (one string: the writer has no font metrics, so a second piece of text cannot be placed after the first)
    auto compact=[&](const Row& r){ brk(y,16); d.text(M,y,9,HELV_B,r.date,ar,ag,ab);
        const std::string t = r.title+((r.org.empty()||r.org==r.title)? std::string() : "  \xC2\xB7  "+r.org);
        bool first=true; for(auto& L:cv_wrap(d,XR-(M+96),9.5,HELV,t)){ if(!first) y+=12; first=false; d.text(M+96,y,9.5,HELV,L,0.15,0.15,0.17); }
        y+=17; };
    brk(y,40); y=cv_heading(d,M,y,XR,cv.lb.experience,ar,ag,ab);
    if(cv.lead_exp>0 && cv.lead_exp<=cv.experience.size()){
        for(size_t i=0;i<cv.lead_exp;++i) full(cv.experience[i],true);
        if(cv.lead_exp<cv.experience.size()){ y+=6; brk(y,40); y=cv_heading(d,M,y,XR,cv.lb.other_experience,ar,ag,ab);
            for(size_t i=cv.lead_exp;i<cv.experience.size();++i) compact(cv.experience[i]); }
    } else for(auto& r:cv.experience) full(r,false);
    y+=6;
    brk(y,40); y=cv_heading(d,M,y,XR,cv.lb.education_courses,ar,ag,ab);
    for(size_t i=0;i<cv.education.size();++i) full(cv.education[i], i<cv.lead_edu);
    y+=6;
    if(cv.qualifications.size()||cv.interests.size()){
      brk(y,60); y=cv_heading(d,M,y,XR,cv.lb.other,ar,ag,ab); double colX2=M+(XR-M)/2, cy=y;
      d.text(M,cy,10,HELV_B,cv.lb.qualifications,0.12,0.12,0.12); d.text(colX2,cy,10,HELV_B,cv.lb.personal_interests,0.12,0.12,0.12); cy+=16;
      size_t n=cv.qualifications.size()>cv.interests.size()?cv.qualifications.size():cv.interests.size();
      for(size_t i=0;i<n;i++){ if(i<cv.qualifications.size()){ d.icoDiamond(M,cy-5,3,ar,ag,ab); d.text(M+9,cy,9.5,HELV,cv.qualifications[i],0.2,0.2,0.2); }
        if(i<cv.interests.size()){ d.icoDiamond(colX2,cy-5,3,ar,ag,ab); d.text(colX2+9,cy,9.5,HELV,cv.interests[i],0.2,0.2,0.2); } cy+=15; } }
    footer(); return page;
}

// ── "Creative" layout: full-height accent sidebar (contact + skills) + right column with sections.
//    Inspired by the classic two-column CV templates. Single page (v1). Works best with a DARK accent colour. ──
inline int cv_render_creative(Pdf& d, const CvData& cv, int total){
    const double W=d.width(), H=d.height(); const double ar=cv.aR, ag=cv.aG, ab=cv.aB;
    const double SW=200;                                   // sidebar width
    d.rect(0,0,SW,H, ar,ag,ab);                            // full-height accent sidebar
    // right-column header
    const double RX=SW+30, RXR=W-42;
    d.text(RX,60,24,HELV_B, cv.name, ar,ag,ab);
    d.text(RX,82,11,HELV, cv.subtitle, GR,GR,GR);
    // sidebar (white text on accent)
    const double sx=20, sxr=SW-16; double sy=56;
    auto sHead=[&](const std::string& t){ d.text(sx,sy,11,HELV_B,cv_upper(t),1,1,1); d.line(sx,sy+6,sxr,sy+6,0.8,1,1,1); sy+=20; };
    auto sWrap=[&](const std::string& t,double sz){ for(auto& L:cv_wrap(d,sxr-sx,sz,HELV,t)){ d.text(sx,sy,sz,HELV,L,0.92,0.94,0.98); sy+=sz+3; } };
    sHead(cv.lb.personal_info);
    if(!cv.address.empty()) { sWrap(cv.address,9); sy+=3; }
    if(!cv.phone.empty())   { sWrap(cv.phone,9);   sy+=3; }
    if(!cv.email.empty())   { sWrap(cv.email,9);   sy+=3; }
    if(!cv.linkedin.empty()){ sWrap(cv.linkedin,9);sy+=3; }
    sy+=8;
    if(cv.strengths.size()){ sHead(cv.lb.traits); for(auto& s:cv.strengths){ sWrap(s,9); } sy+=8; }
    if(cv.personality.size()){ sHead(cv.lb.personality);   // trait bars (resumaker-style), e.g. Big Five
        double maxv=10.0; for(auto& t:cv.personality) if(t.second>maxv) maxv=t.second;
        for(auto& t:cv.personality){
            d.text(sx,sy,8.5,HELV,t.first,0.92,0.94,0.98); sy+=11;
            double bw=sxr-sx, bh=4.5, rad=bh*0.5;
            d.rectRound(sx,sy,bw,bh,rad, ar+(1-ar)*0.32, ag+(1-ag)*0.32, ab+(1-ab)*0.32, true);   // track (lightened accent)
            double fr=(t.second<=0)?0.0:(t.second/maxv); if(fr>1)fr=1; double fw=bw*fr; if(fw>0&&fw<bh)fw=bh;
            d.rectRound(sx,sy,fw,bh,rad, 0.92,0.94,0.99, true);   // fill (bright)
            sy+=bh+8;
        } sy+=4;
    }
    if(cv.qualifications.size()){ sHead(cv.lb.qualifications); for(auto& q:cv.qualifications){ sWrap(q,9); } sy+=8; }
    if(cv.interests.size()){ sHead(cv.lb.interests); for(auto& it:cv.interests){ sWrap(it,9); } }
    // right column body
    double y=112;
    auto rHead=[&](const std::string& t){ d.text(RX,y,12,HELV_B,cv_upper(t),ar,ag,ab);
        d.line(RX,y+6,RXR,y+6,1.0, ar+(1-ar)*0.4,ag+(1-ag)*0.4,ab+(1-ab)*0.4); y+=22; };
    // a row in full; with_note: the chosen ones also say what the job involved
    auto rowFull=[&](const Row& r, bool with_note){ d.text(RX,y,10.5,HELV_B,r.title,0.12,0.12,0.12);
        d.text(RX,y+13,9,HELV, r.org+"   \xC2\xB7   "+r.date, ORGr,ORGg,ORGb);
        if(!with_note || r.note.empty()){ y+=30; return; }
        double ny=y+26; for(auto& L:cv_wrap(d,RXR-RX,9,HELV,cv_cap1(r.note))){ d.text(RX,ny,9,HELV,L,0.25,0.25,0.27); ny+=12; } y=ny+8; };
    // a row on one line (the jobs that were not chosen for this application)
    // (one string: the writer has no font metrics, so a second piece of text cannot be placed after the first)
    auto rowCompact=[&](const Row& r){
        const std::string t = r.title+((r.org.empty()||r.org==r.title)? std::string() : "  \xC2\xB7  "+r.org)+(r.date.empty()? std::string() : "  \xC2\xB7  "+r.date);
        bool first=true; for(auto& L:cv_wrap(d,RXR-RX,9,HELV,t)){ if(!first) y+=11; first=false; d.text(RX,y,9,HELV,L,0.15,0.15,0.17); }
        y+=16; };
    rHead(cv.lb.summary);
    for(auto& L:cv_wrap(d,RXR-RX,9.5,HELV,cv.summary)){ d.text(RX,y,9.5,HELV,L,0.2,0.2,0.22); y+=13; } y+=14;
    rHead(cv.lb.experience);
    if(cv.lead_exp>0 && cv.lead_exp<=cv.experience.size()){
        for(size_t i=0;i<cv.lead_exp;++i) rowFull(cv.experience[i],true);
        if(cv.lead_exp<cv.experience.size()){ y+=6; rHead(cv.lb.other_experience);
            for(size_t i=cv.lead_exp;i<cv.experience.size();++i) rowCompact(cv.experience[i]); y+=4; }
    } else for(auto& r:cv.experience) rowFull(r,false);
    y+=6;
    rHead(cv.lb.education);
    for(size_t i=0;i<cv.education.size();++i) rowFull(cv.education[i], i<cv.lead_edu);
    if(total>0) d.textRight(RXR,H-28,8,HELV,cv_page(cv.lb,1,total),0.55,0.55,0.58);
    return 1;
}

// personligt brev — single clean page
inline void cv_render_letter(Pdf& d, const LetterData& L){
    const double W=d.width(), M=58, XR=W-M; const double ar=L.aR, ag=L.aG, ab=L.aB;
    d.textRight(XR,58,11,HELV_B,L.name,ar,ag,ab);
    d.textRight(XR,73,9,HELV,L.address,0.3,0.3,0.3);
    d.textRight(XR,86,9,HELV,L.phone,0.3,0.3,0.3);
    d.textRight(XR,99,9,HELV,L.email,0.3,0.3,0.3);
    double y=136; d.text(M,y,10,HELV,(L.place.empty()? L.date : L.place+", "+L.date),0.3,0.3,0.3); y+=30;
    d.text(M,y,11,HELV_B,L.company,0.12,0.12,0.12); y+=14;
    if(!L.role.empty()){ d.text(M,y,10,HELV,cv_put(L.lb.application,"{role}",L.role),ORGr,ORGg,ORGb); y+=8; }
    y+=18; y=cv_heading(d,M,y,XR,L.lb.letter,ar,ag,ab); y+=2;
    // body paragraphs (blank-line separated). Robust to Windows CRLF AND to accumulated blank lines: strip \r,
    // then ANY run of >=2 newlines = ONE paragraph break; a single newline = a space (joins wrapped lines).
    std::vector<std::string> paras; { std::string p; std::string s; for(char c:L.body) if(c!='\r') s+=c;
        for(size_t i=0;i<s.size();){
            if(s[i]=='\n'){ size_t j=i; while(j<s.size()&&s[j]=='\n') ++j;
                if(j-i>=2){ if(!p.empty()){ paras.push_back(p); p.clear(); } } else p+=' ';
                i=j; }
            else { p+=s[i]; ++i; } }
        if(!p.empty())paras.push_back(p); }
    for(auto& para:paras){ auto lines=cv_wrap(d,XR-M,10.5,TIMES,para);
        for(auto& ln:lines){ d.text(M,y,10.5,TIMES,ln,0.15,0.15,0.15); y+=15; } y+=8; }
    y+=14; d.text(M,y,10.5,TIMES,L.lb.regards,0.15,0.15,0.15); y+=26;
    d.text(M,y,12,HELV_B,L.name,ar,ag,ab);
}
