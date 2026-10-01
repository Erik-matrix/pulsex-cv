// pcore_pdf.hpp — minimal, dependency-free PDF writer for CVs / cover letters.
// Text uses the 14 standard PDF fonts (no embedding); icons are drawn as VECTOR PATHS (no PNG/JPEG, near-zero I/O).
// Coordinates are TOP-LEFT (y grows downward), points (72/inch). A4 = 595.28 x 841.89.
#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <cmath>
#include <cstdint>

namespace pcore_pdf {

enum Font { HELV=1, HELV_B=2, TIMES=3, TIMES_B=4, HELV_I=5 };   // maps to /F1../F5

class Pdf {
public:
    Pdf(double w=595.28, double h=841.89) : W(w), H(h) {}
    double width() const { return W; } double height() const { return H; }
    void newPage(){ pages_.push_back(cs); cs.clear(); }   // start a fresh page (multi-page support)

    // ── text ── (x,y = top-left baseline-ish; y is the text baseline from top)
    void text(double x, double y, double size, Font f, const std::string& s, double r=0, double g=0, double b=0) {
        std::ostringstream o; o.setf(std::ios::fixed); o.precision(2);
        o << "BT /F" << (int)f << " " << size << " Tf " << r << " " << g << " " << b << " rg "
          << x << " " << (H - y) << " Td (" << esc(s) << ") Tj ET\n";
        cs += o.str();
    }
    // crude proportional width (avg glyph); good enough for right-align / columns
    double textWidth(const std::string& s, double size, Font f) const {
        double k = (f==TIMES||f==TIMES_B) ? 0.48 : 0.52;   // avg advance / size
        return cp1252(s).size() * size * k;
    }
    void textRight(double xr, double y, double size, Font f, const std::string& s, double r=0,double g=0,double b=0){
        text(xr - textWidth(s,size,f), y, size, f, s, r,g,b);
    }

    void line(double x0,double y0,double x1,double y1,double w,double r,double g,double b){
        p2(w,r,g,b); ln(x0,y0); o("m "); ln(x1,y1); o("l S\n");
    }
    void rect(double x,double y,double w,double h,double r,double g,double b){       // filled (top-left origin)
        std::ostringstream s; s.setf(std::ios::fixed); s.precision(2);
        s << r<<" "<<g<<" "<<b<<" rg "<<x<<" "<<(H-y-h)<<" "<<w<<" "<<h<<" re f\n"; cs+=s.str();
    }
    void rectRound(double x,double y,double w,double h,double rad,double r,double g,double b,bool fill=true){
        double k=0.5523*rad; begin();
        mv(x+rad,y); lp(x+w-rad,y); cv(x+w-rad+k,y, x+w,y+rad-k, x+w,y+rad);
        lp(x+w,y+h-rad); cv(x+w,y+h-rad+k, x+w-rad+k,y+h, x+w-rad,y+h);
        lp(x+rad,y+h); cv(x+rad-k,y+h, x,y+h-rad+k, x,y+h-rad);
        lp(x,y+rad); cv(x,y+rad-k, x+rad-k,y, x+rad,y);
        if(fill){ std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<r<<" "<<g<<" "<<b<<" rg h f\n"; cs+=s.str(); }
        else    { std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<"1 w "<<r<<" "<<g<<" "<<b<<" RG h S\n"; cs+=s.str(); }
    }

    // ── "chip" ICONS: a filled rounded square (accent colour) with a crisp WHITE vector glyph — solid, not flat. ──
    void icoEmail(double x,double y,double s,double r,double g,double b){
        rectRound(x,y,s,s,s*0.24,r,g,b,true);
        double gx=x+s*0.24, gy=y+s*0.30, gw=s*0.52, gh=s*0.40, lw=s*0.075;
        strokeRect(gx,gy,gw,gh,lw,1,1,1);
        p2(lw,1,1,1); ln(gx,gy); o("m "); ln(gx+gw/2,gy+gh*0.55); o("l "); ln(gx+gw,gy); o("l S\n");   // white flap
    }
    void icoPhone(double x,double y,double s,double r,double g,double b){
        rectRound(x,y,s,s,s*0.24,r,g,b,true);
        double bw=s*0.34, bx=x+(s-bw)/2, by=y+s*0.20, bh=s*0.60, lw=s*0.07;
        strokeRoundRect(bx,by,bw,bh,s*0.06,lw,1,1,1);
        p2(lw,1,1,1); ln(bx+bw*0.32,by+bh*0.06); o("m "); ln(bx+bw*0.68,by+bh*0.06); o("l S\n");        // speaker
        dot(bx+bw/2,by+bh*0.86,s*0.045,1,1,1);                                                          // home dot
    }
    void icoPin(double x,double y,double s,double r,double g,double b){
        rectRound(x,y,s,s,s*0.24,r,g,b,true);
        double cx=x+s/2, cy=y+s*0.40, rad=s*0.20;                                                       // white filled pin
        fillCircle(cx,cy,rad,1,1,1);
        begin(); mv(cx-rad,cy); lp(cx,y+s*0.78); lp(cx+rad,cy);
        { std::ostringstream o2; o2.setf(std::ios::fixed); o2.precision(2); o2<<"1 1 1 rg h f\n"; cs+=o2.str(); }
        dot(cx,cy,s*0.06,r,g,b);
    }
    void icoWeb(double x,double y,double s,double r,double g,double b){
        rectRound(x,y,s,s,s*0.24,r,g,b,true);
        double cx=x+s/2, cy=y+s/2, rad=s*0.30, lw=s*0.06;
        strokeCircle(cx,cy,rad,lw,1,1,1);
        p2(lw,1,1,1); ln(cx-rad,cy); o("m "); ln(cx+rad,cy); o("l S\n");
        strokeEllipse(cx,cy,rad*0.5,rad,lw,1,1,1);
    }
    void icoLinkedIn(double x,double y,double s){     // LinkedIn-blue chip + white "in" (optional / opt-in)
        rectRound(x,y,s,s,s*0.22,0.05,0.47,0.71,true);
        text(x+s*0.16, y+s*0.72, s*0.60, HELV_B, "in", 1,1,1);
    }
    void icoDiamond(double x,double y,double s,double r,double g,double b){   // skill bullet
        begin(); mv(x+s/2,y); lp(x+s,y+s/2); lp(x+s/2,y+s); lp(x,y+s/2);
        std::ostringstream o2; o2.setf(std::ios::fixed); o2.precision(2); o2<<r<<" "<<g<<" "<<b<<" rg h f\n"; cs+=o2.str();
    }

    void save(const std::string& path){
        pages_.push_back(cs);                                 // finalise the current page
        int N=(int)pages_.size(), fb=3+2*N;                   // font objects start at fb
        std::vector<std::string> obj;
        obj.push_back("<< /Type /Catalog /Pages 2 0 R >>");
        std::string kids; for(int i=0;i<N;i++) kids += std::to_string(3+2*i)+" 0 R ";
        obj.push_back("<< /Type /Pages /Kids ["+kids+"] /Count "+std::to_string(N)+" >>");
        for(int i=0;i<N;i++){
            obj.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "+num(W)+" "+num(H)+"] /Contents "+std::to_string(4+2*i)
                +" 0 R /Resources << /Font << /F1 "+std::to_string(fb)+" 0 R /F2 "+std::to_string(fb+1)+" 0 R /F3 "
                +std::to_string(fb+2)+" 0 R /F4 "+std::to_string(fb+3)+" 0 R /F5 "+std::to_string(fb+4)+" 0 R >> >> >>");
            obj.push_back("<< /Length "+std::to_string(pages_[i].size())+" >>\nstream\n"+pages_[i]+"endstream");
        }
        obj.push_back(fontObj("Helvetica")); obj.push_back(fontObj("Helvetica-Bold")); obj.push_back(fontObj("Times-Roman"));
        obj.push_back(fontObj("Times-Bold")); obj.push_back(fontObj("Helvetica-Oblique"));
        std::string out="%PDF-1.4\n"; std::vector<size_t> off(obj.size());
        for(size_t i=0;i<obj.size();++i){ off[i]=out.size(); out+=std::to_string(i+1)+" 0 obj\n"+obj[i]+"\nendobj\n"; }
        size_t xref=out.size(); out+="xref\n0 "+std::to_string(obj.size()+1)+"\n0000000000 65535 f \n";
        for(size_t i=0;i<obj.size();++i){ char b[24]; std::snprintf(b,sizeof(b),"%010zu 00000 n \n",off[i]); out+=b; }
        out+="trailer\n<< /Size "+std::to_string(obj.size()+1)+" /Root 1 0 R >>\nstartxref\n"+std::to_string(xref)+"\n%%EOF\n";
        std::ofstream f(path,std::ios::binary); f.write(out.data(),(std::streamsize)out.size());
    }
private:
    double W,H; std::string cs; std::vector<std::string> pages_;
    void o(const char* s){ cs+=s; }
    static std::string num(double v){ std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<v; return s.str(); }
    void ln(double x,double y){ std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<x<<" "<<(H-y)<<" "; cs+=s.str(); }
    void p2(double w,double r,double g,double b){ std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<w<<" w "<<r<<" "<<g<<" "<<b<<" RG "; cs+=s.str(); }
    void begin(){}
    void mv(double x,double y){ ln(x,y); o("m "); } void lp(double x,double y){ ln(x,y); o("l "); }
    void cv(double x1,double y1,double x2,double y2,double x3,double y3){ ln(x1,y1);ln(x2,y2);ln(x3,y3);o("c "); }
    void dot(double cx,double cy,double rad,double r,double g,double b){ fillCircle(cx,cy,rad,r,g,b); }
    void strokeRect(double x,double y,double w,double h,double lw,double r,double g,double b){
        std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<lw<<" w "<<r<<" "<<g<<" "<<b<<" RG "<<x<<" "<<(H-y-h)<<" "<<w<<" "<<h<<" re S\n"; cs+=s.str(); }
    void strokeRoundRect(double x,double y,double w,double h,double rad,double lw,double r,double g,double b){
        double k=0.5523*rad; begin();
        mv(x+rad,y); lp(x+w-rad,y); cv(x+w-rad+k,y,x+w,y+rad-k,x+w,y+rad); lp(x+w,y+h-rad); cv(x+w,y+h-rad+k,x+w-rad+k,y+h,x+w-rad,y+h);
        lp(x+rad,y+h); cv(x+rad-k,y+h,x,y+h-rad+k,x,y+h-rad); lp(x,y+rad); cv(x,y+rad-k,x+rad-k,y,x+rad,y);
        std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<lw<<" w "<<r<<" "<<g<<" "<<b<<" RG h S\n"; cs+=s.str(); }
    void circPath(double cx,double cy,double rad){ double k=0.5523*rad; begin();
        mv(cx+rad,cy); cv(cx+rad,cy+k,cx+k,cy+rad,cx,cy+rad); cv(cx-k,cy+rad,cx-rad,cy+k,cx-rad,cy);
        cv(cx-rad,cy-k,cx-k,cy-rad,cx,cy-rad); cv(cx+k,cy-rad,cx+rad,cy-k,cx+rad,cy); }
    void strokeCircle(double cx,double cy,double rad,double lw,double r,double g,double b){ circPath(cx,cy,rad);
        std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<lw<<" w "<<r<<" "<<g<<" "<<b<<" RG h S\n"; cs+=s.str(); }
    void fillCircle(double cx,double cy,double rad,double r,double g,double b){ circPath(cx,cy,rad);
        std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<r<<" "<<g<<" "<<b<<" rg h f\n"; cs+=s.str(); }
    void strokeEllipse(double cx,double cy,double rx,double ry,double lw,double r,double g,double b){ double kx=0.5523*rx,ky=0.5523*ry; begin();
        mv(cx+rx,cy); cv(cx+rx,cy+ky,cx+kx,cy+ry,cx,cy+ry); cv(cx-kx,cy+ry,cx-rx,cy+ky,cx-rx,cy);
        cv(cx-rx,cy-ky,cx-kx,cy-ry,cx,cy-ry); cv(cx+kx,cy-ry,cx+rx,cy-ky,cx+rx,cy);
        std::ostringstream s; s.setf(std::ios::fixed); s.precision(2); s<<lw<<" w "<<r<<" "<<g<<" "<<b<<" RG h S\n"; cs+=s.str(); }
    static std::string fontObj(const char* base){ return std::string("<< /Type /Font /Subtype /Type1 /BaseFont /")+base+" /Encoding /WinAnsiEncoding >>"; }
    // UTF-8 → CP1252 (WinAnsi) bytes for the common Latin range (åäöÅÄÖé… ); unknown → '?'
    static std::string cp1252(const std::string& u){ std::string r; for(size_t i=0;i<u.size();){ unsigned char c=u[i];
        if(c<0x80){ r+=(char)c; i++; }
        else if((c&0xE0)==0xC0 && i+1<u.size()){ unsigned cp=((c&0x1F)<<6)|(u[i+1]&0x3F); i+=2;
            r += (cp<=0xFF) ? (char)cp : '?'; }
        else if((c&0xF0)==0xE0 && i+2<u.size()){ unsigned cp=((c&0x0F)<<12)|((u[i+1]&0x3F)<<6)|(u[i+2]&0x3F); i+=3;
            // common WinAnsi punctuation in the 0x80-0x9F "smart" range
            switch(cp){ case 0x2013:r+=(char)0x96;break; case 0x2014:r+=(char)0x97;break; case 0x2018:r+=(char)0x91;break;
                case 0x2019:r+=(char)0x92;break; case 0x201C:r+=(char)0x93;break; case 0x201D:r+=(char)0x94;break;
                case 0x2022:r+=(char)0x95;break; case 0x2026:r+=(char)0x85;break; case 0x20AC:r+=(char)0x80;break; default:r+='?'; } }
        else { i++; r+='?'; } } return r; }
    static std::string esc(const std::string& s){ std::string u=cp1252(s), o; for(char c:u){ if(c=='('||c==')'||c=='\\'){o+='\\';} o+=c; } return o; }
};

} // namespace pcore_pdf
