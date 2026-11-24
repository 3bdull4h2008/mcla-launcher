// mcla_launcher.cpp - standalone WebView2 launcher host for MCLA PC (Windows x64).
//
// TRUE per-pixel-alpha transparency via DirectComposition + WS_EX_NOREDIRECTIONBITMAP.
// This is the only path that gives WebView2 both see-through pixels AND working input:
//  - WinForms TransparencyKey : magenta renders visible (WebView2 child HWND ignores parent key).
//  - WPF AllowsTransparency   : transparent but WebView2 input is dead (measured).
//  - Here: the top-level window has no redirection bitmap; DWM composes a DirectComposition
//    visual tree whose content comes from WebView2's own swap chain with transparent clear color.
//
// Build:  build.bat   (clang-cl + user32/ole32/dcomp/shlwapi + WebView2Loader.dll.lib)
// Output: mcla_launcher.exe   (copy to <mcla-pc>\build\mcla_launcher.exe)
// Self-check: mcla_launcher.exe --selftest   (exit code 0 = ini/toml writer ok)
// NEVER touches build/mcla.exe.
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <dcomp.h>
#include <wrl.h>
#include <wrl/client.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "wv2/WebView2.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

// ------------------------------- config -------------------------------------------------------
static const int WIN_W = 650, WIN_H = 446;
static int    g_w = WIN_W, g_h = WIN_H;   // DPI-scaled window size in physical pixels
static UINT   g_dpi = 96;
static double g_zoom = 1.0;               // CSS layout is already 650x446; do not double-scale it
static int    g_exit = 0;
static HWND       g_hwnd = nullptr;
static std::wstring g_dir, g_uiDir, g_gameDir, g_cacheDir, g_mclaExe, g_ini;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2>           g_web;
static const wchar_t kOrigin[] = L"https://mcla.launcher/";
static const wchar_t kRuntimeUrl[] = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";

// ------------------------------- helpers -------------------------------------------------------
static std::wstring dirOf(const std::wstring& p) {
    auto i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}
static bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
// Minimal JSON string escape (for pushing mcla.toml into JS).
static std::string jesc(const std::string& s) {
    std::string o; for (char c : s) {
        switch (c) { case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
                     case '\n': o += "\\n"; break; case '\r': o += "\\r"; break;
                     default: if ((unsigned char)c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; } else o += c; }
    } return o;
}

// WebView2 missing or broken: offer the runtime download and quit instead of a blank window.
static void wv2Fail(HRESULT hr) {
    wchar_t m[512];
    swprintf_s(m, L"The WebView2 Runtime could not be started (0x%08X).\nDownload it from:\n%s\n\nOpen the download page now?", (unsigned)hr, kRuntimeUrl);
    if (MessageBoxW(g_hwnd, m, L"MCLA Launcher", MB_YESNO | MB_ICONERROR) == IDYES)
        ShellExecuteW(nullptr, L"open", kRuntimeUrl, nullptr, nullptr, SW_SHOWNORMAL);
    g_exit = 1; PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// ------------------------------- mcla.toml <-> JS ---------------------------------------------
static std::string tomlUnquote(const std::string& v) {
    std::string o; for (size_t i = 1; i + 1 < v.size(); ++i) { if (v[i] == '\\' && i + 2 < v.size()) i++; o += v[i]; } return o;
}
static std::string tomlQuote(const std::string& v) {
    std::string o = "\""; for (char c : v) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o + "\"";
}
// Read cache/mcla.toml into a flat JSON object with key: value pairs for loadConfig().
static std::string tomlToJson() {
    std::ifstream f(g_cacheDir + L"\\mcla.toml");
    if (!f.good()) return "{}";
    std::string line, out = "{"; bool first = true;
    while (std::getline(f, line)) {
        size_t s = line.find_first_not_of(" \t\r\n");
        if (s == std::string::npos || line[s] == '#' || line[s] == '[') continue;
        size_t eq = line.find('=', s); if (eq == std::string::npos) continue;
        std::string k = line.substr(s, eq - s); while (!k.empty() && k.back() == ' ') k.pop_back();
        std::string v = line.substr(eq + 1);
        v.erase(0, v.find_first_not_of(" \t")); while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
        bool quoted = v.size() >= 2 && v.front() == '"' && v.back() == '"';
        if (quoted) v = tomlUnquote(v);
        if (!first) out += ",";
        out += "\"" + jesc(k) + "\":";
        // number only if strtod consumes everything; x/n/i reject hex, nan and inf
        char* end = nullptr; double d = strtod(v.c_str(), &end);
        bool isNum = !quoted && !v.empty() && !isspace((unsigned char)v[0]) && v.find_first_of("xXnNiI") == std::string::npos && *end == 0 && std::isfinite(d);
        if (isNum) { char b[40]; snprintf(b, sizeof b, "%.15g", d); out += b; }
        else if (!quoted && (v == "true" || v == "false")) out += v;
        else out += "\"" + jesc(v) + "\"";
        first = false;
    }
    out += "}"; return out;
}

// Minimal JSON parser for the launcher message (objects, arrays, strings, scalars).
struct JVal { enum T{STR,NUM,BOOL,OBJ,ARR,NUL} type=NUL; std::string s; std::vector<std::pair<std::string,JVal>> obj; };
static size_t jskip(const std::string& j,size_t i){ while(i<j.size()&&isspace((unsigned char)j[i]))i++; return i; }
static void putUtf8(std::string& o,unsigned c){
    if(c<0x80)o+=(char)c;
    else if(c<0x800){o+=(char)(0xC0|c>>6);o+=(char)(0x80|(c&63));}
    else if(c<0x10000){o+=(char)(0xE0|c>>12);o+=(char)(0x80|((c>>6)&63));o+=(char)(0x80|(c&63));}
    else{o+=(char)(0xF0|c>>18);o+=(char)(0x80|((c>>12)&63));o+=(char)(0x80|((c>>6)&63));o+=(char)(0x80|(c&63));}
}
static bool hex4(const std::string& j,size_t i,unsigned& v){
    if(i+4>j.size())return false; v=0;
    for(size_t k=0;k<4;k++){ char c=j[i+k]; v<<=4;
        if(c>='0'&&c<='9')v|=c-'0'; else if(c>='a'&&c<='f')v|=c-'a'+10; else if(c>='A'&&c<='F')v|=c-'A'+10; else return false; }
    return true;
}
static std::string jstr(const std::string& j,size_t& i){
    i++; std::string o;   // j[i]==quote on entry
    while(i<j.size()&&j[i]!='"'){
        if(j[i]!='\\'||i+1>=j.size()){ o+=j[i++]; continue; }
        char c=j[++i]; i++;
        switch(c){
        case 'n':o+='\n';break; case 'r':o+='\r';break; case 't':o+='\t';break; case 'b':o+='\b';break; case 'f':o+='\f';break;
        case 'u':{ unsigned u; if(!hex4(j,i,u))break; i+=4;
            if(u>=0xD800&&u<0xDC00&&i+6<=j.size()&&j[i]=='\\'&&j[i+1]=='u'){
                unsigned lo; if(hex4(j,i+2,lo)&&lo>=0xDC00&&lo<0xE000){u=0x10000+((u-0xD800)<<10)+(lo-0xDC00);i+=6;} }
            putUtf8(o,u);break; }
        default:o+=c; }
    }
    if(i<j.size())i++; return o;
}
static JVal jparse(const std::string& j,size_t& i,int depth=0){
    JVal v; i=jskip(j,i);
    if(i>=j.size()||depth>16)return v;
    if(j[i]=='"'){ v.type=JVal::STR; v.s=jstr(j,i); return v; }
    if(j[i]=='{'||j[i]=='['){
        bool isObj=j[i]=='{'; char close=isObj?'}':']';
        v.type=isObj?JVal::OBJ:JVal::ARR; i++;
        while(true){ i=jskip(j,i); if(i>=j.size()||j[i]==close){if(i<j.size())i++;break;}
            std::string key;
            if(isObj){
                if(j[i]!='"'){i++;continue;}
                key=jstr(j,i);
                i=jskip(j,i); if(i<j.size()&&j[i]==':')i++;
                // map UI key -> real cvar/env name
                if(key=="prompts")key="mcla_button_prompts";
                else if(key=="vibration")key="mcla_vibration";
                else if(key=="deadzone")key="mcla_left_stick_deadzone";
            }
            JVal val=jparse(j,i,depth+1);
            v.obj.push_back({key,val});
            i=jskip(j,i); if(i<j.size()&&j[i]==',')i++;
        }
        return v;
    }
    size_t st=i; while(i<j.size()&&j[i]!=','&&j[i]!='}'&&j[i]!=']'&&!isspace((unsigned char)j[i]))i++;
    if(i==st)i++;
    std::string tok=j.substr(st,i-st); v.s=tok;
    v.type=(tok=="true"||tok=="false")?JVal::BOOL:(tok=="null"?JVal::NUL:JVal::NUM);
    return v;
}
static const JVal* jget(const JVal& o,const char* k){
    if(o.type!=JVal::OBJ)return nullptr;
    for(auto& kv:o.obj) if(kv.first==k)return &kv.second;
    return nullptr;
}

// Keys: [A-Za-z0-9_.-] only. Values: no CR, LF, '=' or NUL (they would split or forge lines).
static bool okKey(const std::string& k){
    if(k.empty())return false;
    for(char c:k) if(!isalnum((unsigned char)c)&&c!='_'&&c!='-'&&c!='.')return false;
    return true;
}
static bool badVal(const std::string& v){ return v.find_first_of(std::string("\n\r=\0",4))!=std::string::npos; }
static std::string tomlKeyOf(const std::string& line){
    size_t s=line.find_first_not_of(" \t"); if(s==std::string::npos||line[s]=='#'||line[s]=='[')return "";
    size_t eq=line.find('=',s); if(eq==std::string::npos)return "";
    std::string k=line.substr(s,eq-s); while(!k.empty()&&(k.back()==' '||k.back()=='\t'))k.pop_back();
    return k;
}

// payload.extra.toml (section -> key -> value): _env keys go to the ini [env] section (allowlist),
// everything else is merged into mcla.toml keeping the other lines.
static void writeToml(const JVal& root,const std::wstring& ini,const std::wstring& tomlPath){
    if(root.type!=JVal::OBJ)return;
    static const std::set<std::string> kEnvOk={
        "MCLA_PROFILE_NAME","MCLA_WINDOW_W","MCLA_WINDOW_H","MCLA_RENDER_SCALE","MCLA_DISPLAY_MODE","MCLA_VSYNC",
        "MCLA_ASPECT_FIX","MCLA_ASPECT_WIDTH","MCLA_MOTION_BLUR","MCLA_LOD_SCALE","MCLA_FPS_CAP","MCLA_NO_GAMMA_LUT",
        "MCLA_NO_SIGNIN","MCLA_NO_RENDER_2STAGE","MCLA_FLOW_LOD","MCLA_FIXED_SEED","MCLA_FIXED_TOD","MCLA_LOG_LEVEL",
        "MCLA_SMALL_FS_SWAP","MCLA_BTN_PROMPTS_FIX","MCLA_BTN_PROMPTS_PS","MCLA_NO_RESOLVED_MIPS",
        "MCLA_NO_CPUEXPAND_MIPS","MCLA_NO_OCCLUSION_QUERY","MCLA_NO_MT_BURSTS","MCLA_MT_PREP",
        "MCLA_AUDIO_GAIN","MCLA_NO_ADAPTIVE_AUDIO","MCLA_AMBIENT_OCCLUSION","MCLA_SHADOWS",
        "MCLA_FLOW_LOD"};
    std::vector<std::pair<std::string,std::string>> sets, envSets;   // sets: key + full toml line
    for(auto& sec:root.obj){
        if(sec.second.type!=JVal::OBJ)continue;
        for(auto& kv:sec.second.obj){
            if(kv.second.type==JVal::OBJ||kv.second.type==JVal::ARR)continue;
            std::string val=kv.second.s; bool str=kv.second.type==JVal::STR;
            if(val=="true")val="1"; else if(val=="false")val="0";
            if(kv.first=="mcla_adaptive_triggers")continue;  // setting does not exist in the game
            if(val=="null"||val.empty()){
                // UI produced a null string for a slider: use the CVar default.
                static const std::map<std::string,std::string> defs={
                    {"mcla_left_stick_deadzone","24"},{"mcla_right_stick_deadzone","24"},
                    {"mcla_script_pad_latch_ms","100"},{"mcla_dt_max","0.125"},
                    {"mcla_poll_spin_us","40"},{"mcla_poll_tail_us","400"},
                    {"mcla_pump_wake_pause_s","60"},{"mcla_pump_sleep_max_ms","-1"},
                    {"MCLA_LOD_SCALE","1"},{"MCLA_FLOW_LOD","1"}
                };
                auto it=defs.find(kv.first);
                if(it==defs.end())continue;
                val=it->second; str=false;
            }
            if(!okKey(kv.first)||badVal(val))continue;
            if(sec.first=="_env"){
                std::string k=kv.first;
                if(k=="resolution_scale")k="MCLA_RENDER_SCALE";
                else if(k=="vsync")k="MCLA_VSYNC";
                if(kEnvOk.count(k))envSets.push_back({k,val});
            } else {
                // AO/shadows are startup env bridges in the game; mirroring the
                // launcher choice there also persists it in its normal CVar file.
                if(kv.first=="mcla_ambient_occlusion")
                    envSets.push_back({"MCLA_AMBIENT_OCCLUSION",val});
                else if(kv.first=="mcla_shadows")
                    envSets.push_back({"MCLA_SHADOWS",val});
                // The game's CVar reader is deliberately flat, so TOML table
                // headers in the launcher schema are grouping labels only.
                if(kv.first=="mcla_fps"){
                    if(val=="-1")val="0";
                    envSets.push_back({"MCLA_FPS_CAP",val});
                }
                if(kv.first=="fullscreen")envSets.push_back({"MCLA_DISPLAY_MODE",val=="1"?"borderless_fullscreen":"windowed"});
                sets.push_back({kv.first,kv.first+" = "+(str?tomlQuote(val):val)});
            }
        }
    }
    // WritePrivateProfileString keeps other lines and comments and never duplicates the section.
    for(auto& kv:envSets)WritePrivateProfileStringW(L"env",widen(kv.first).c_str(),widen(kv.second).c_str(),ini.c_str());
    if(sets.empty())return;
    std::ifstream in(tomlPath); std::vector<std::string> lines; std::string l;
    while(std::getline(in,l)){ if(!l.empty()&&l.back()=='\r')l.pop_back(); lines.push_back(l); }
    in.close();
    // new keys must stay before the first table header, or they would belong to that table
    size_t hdr=0; while(hdr<lines.size()){ size_t s=lines[hdr].find_first_not_of(" \t"); if(s!=std::string::npos&&lines[hdr][s]=='[')break; hdr++; }
    for(auto& kv:sets){
        bool found=false;
        for(size_t n=0;n<hdr;n++) if(tomlKeyOf(lines[n])==kv.first){lines[n]=kv.second;found=true;break;}
        if(!found)lines.insert(lines.begin()+hdr++,kv.second);
    }
    std::ofstream out(tomlPath,std::ios::binary); for(auto& line:lines)out<<line<<"\r\n";
}

// writeToml twice into temp files: one [env] header, allowlisted env only, quoted strings, keys before the table.
static std::string slurp(const std::wstring& p){ std::ifstream f(p,std::ios::binary); std::stringstream ss; ss<<f.rdbuf(); return ss.str(); }
static size_t countOf(const std::string& h,const std::string& n){ size_t c=0,p=0; while((p=h.find(n,p))!=std::string::npos){c++;p+=n.size();} return c; }
static int selfTest(){
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH,tmp);
    std::wstring ini=std::wstring(tmp)+L"mcla_selftest.ini", toml=std::wstring(tmp)+L"mcla_selftest.toml";
    DeleteFileW(ini.c_str());
    { std::ofstream o(toml,std::ios::binary); o<<"a = 1\r\n[t]\r\nx = 2\r\n"; }
    const std::string js=R"({"display":{"name":"a\"b\\cé😀","fullscreen":true,"bad":"x=y","mcla_fps":120},"renderer":{"mcla_shadows":false,"mcla_ambient_occlusion":true},"_env":{"resolution_scale":2,"MCLA_EVIL":1,"vsync":false,"MCLA_SMALL_FS_SWAP":1,"MCLA_AUDIO_GAIN":1.2,"MCLA_FLOW_LOD":1.5,"MCLA_PROFILE_NAME":"TestTag"}})";
    for(int n=0;n<2;n++){ size_t i=0; JVal r=jparse(js,i); writeToml(r,ini,toml); }
    std::string a=slurp(ini), b=slurp(toml);
    if(countOf(a,"[env]")!=1)return 2;
    if(countOf(a,"MCLA_RENDER_SCALE=2")!=1||countOf(a,"MCLA_VSYNC=0")!=1||a.find("EVIL")!=std::string::npos)return 3;
    if(countOf(a,"MCLA_SMALL_FS_SWAP=1")!=1||countOf(a,"MCLA_AUDIO_GAIN=1.2")!=1||countOf(a,"MCLA_PROFILE_NAME=TestTag")!=1)return 8;
    if(countOf(a,"MCLA_FLOW_LOD=1.5")!=1)return 10;
    if(countOf(a,"MCLA_DISPLAY_MODE=borderless_fullscreen")!=1)return 4;
    if(countOf(a,"MCLA_FPS_CAP=120")!=1)return 11;
    if(countOf(a,"MCLA_AMBIENT_OCCLUSION=1")!=1||countOf(a,"MCLA_SHADOWS=0")!=1)return 14;
    if(countOf(b,"name = \"a\\\"b\\\\c\xC3\xA9\xF0\x9F\x98\x80\"")!=1)return 5;
    if(countOf(b,"fullscreen = 1")!=1||b.find("bad")!=std::string::npos)return 6;
    if(countOf(b,"mcla_fps = 120")!=1||countOf(b,"mcla_shadows = 0")!=1||countOf(b,"mcla_ambient_occlusion = 1")!=1)return 13;
    if(b.find("name = ")>b.find("[t]")||countOf(b,"[t]")!=1)return 7;
    DeleteFileW(ini.c_str()); DeleteFileW(toml.c_str());
    return 0;
}

// ------------------------------- action dispatch ----------------------------------------------
static void eval(const std::string& js) {
    if (g_web) g_web->ExecuteScript(widen(js).c_str(), nullptr);
}
// Data-driven child env from mcla_launcher.ini at LAUNCH time (no rebuild for new settings).
static std::wstring buildChildEnv() {
    std::vector<std::wstring> kv;
    wchar_t* cur = GetEnvironmentStringsW();
    for (wchar_t* p = cur; *p; p += wcslen(p) + 1) if (*p != L'=') kv.push_back(p);
    FreeEnvironmentStringsW(cur);
    auto setEnv = [&](const std::wstring& e) {
        size_t eq = e.find(L'='); if (eq == std::wstring::npos || eq == 0) return;
        for (auto& o : kv)
            if (o.size() > eq && o[eq] == L'=' && _wcsnicmp(o.c_str(), e.c_str(), eq) == 0) { o = e; return; }
        kv.push_back(e);
    };
    wchar_t buf[256];
    auto get = [&](const wchar_t* k, const wchar_t* d)->std::wstring{ GetPrivateProfileStringW(L"launcher",k,d,buf,256,g_ini.c_str()); return std::wstring(buf); };
    std::wstring res=get(L"res",L"2560x1440"); size_t x=res.find(L'x');
    if(x!=std::wstring::npos){
        int w=_wtoi(res.substr(0,x).c_str()), h=_wtoi(res.substr(x+1).c_str());
        if(w>=640&&w<=7680&&h>=480&&h<=4320){ setEnv(L"MCLA_WINDOW_W="+std::to_wstring(w)); setEnv(L"MCLA_WINDOW_H="+std::to_wstring(h)); }
        else x=std::wstring::npos;
    }
    int sc=_wtoi(get(L"scale",L"2").c_str()); if(sc>=1&&sc<=3)setEnv(L"MCLA_RENDER_SCALE="+std::to_wstring(sc));
    std::wstring fs=get(L"fullscreen",L"1");
    setEnv(std::wstring(L"MCLA_DISPLAY_MODE=")+(fs==L"1"?L"borderless_fullscreen":L"windowed"));
    // profile name: printable ASCII, max 15 (same rule as the game)
    std::wstring name; for(wchar_t c:get(L"name",L"SWA")) if(c>=0x20&&c<0x7F&&name.size()<15)name+=c;
    if(!name.empty())setEnv(L"MCLA_PROFILE_NAME="+name);
    int ai=_wtoi(get(L"aspect",L"0").c_str()),aw=0;
    if(ai==2)aw=1680; else if(ai==3)aw=2560;
    else if(ai==0&&x!=std::wstring::npos)aw=(int)(720.0*_wtoi(res.substr(0,x).c_str())/std::max(1,_wtoi(res.substr(x+1).c_str()))+0.5);
    if(aw>1280){ setEnv(L"MCLA_ASPECT_FIX=1"); setEnv(L"MCLA_ASPECT_WIDTH="+std::to_wstring(std::min(aw,4095))); }
    if(_wtoi(get(L"blur",L"0").c_str())==1)setEnv(L"MCLA_MOTION_BLUR=0");
    static const wchar_t* kLod[]={L"0.75",L"1.0",L"1.5",L"2.0"}; int lod=_wtoi(get(L"lod",L"1").c_str());
    if(lod>=0&&lod<4&&lod!=1)setEnv(std::wstring(L"MCLA_LOD_SCALE=")+kLod[lod]);
    // MCLA_FPS_CAP=0 means "no override" in the game; uncapped is the literal word. Key absent = no override.
    static const wchar_t* kFps[]={L"30",L"60",L"120",L"uncapped"}; std::wstring fpsS=get(L"fps",L"");
    int fps=_wtoi(fpsS.c_str()); if(!fpsS.empty()&&fps>=0&&fps<4)setEnv(std::wstring(L"MCLA_FPS_CAP=")+kFps[fps]);
    if(_wtoi(get(L"vsync",L"0").c_str())==1)setEnv(L"MCLA_VSYNC=1");
    if(_wtoi(get(L"gamma",L"0").c_str())==1)setEnv(L"MCLA_NO_GAMMA_LUT=1");
    std::vector<wchar_t> sec(32768);
    GetPrivateProfileSectionW(L"env",sec.data(),(DWORD)sec.size(),g_ini.c_str());
    for(wchar_t* p=sec.data();*p;p+=wcslen(p)+1){
        std::wstring e=p; size_t c=e.find(L" ;"); if(c!=std::wstring::npos)e.resize(c);
        while(!e.empty()&&e.back()==L' ')e.pop_back();
        if(e.empty()||e[0]==L';')continue;
        setEnv(e);
    }
    std::wstring block; for(auto& e:kv){block+=e;block+=L'\0';} block+=L'\0';
    return block;
}

static bool gameRunning() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) }; bool r = false;
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
        if (_wcsicmp(pe.szExeFile, L"mcla.exe") == 0) { r = true; break; }
    CloseHandle(s); return r;
}
static void launchGame() {
    if (!exists(g_mclaExe)) { MessageBoxW(g_hwnd, (L"mcla.exe not found:\n"+g_mclaExe).c_str(), L"MCLA Launcher", MB_ICONERROR); return; }
    if (gameRunning()) { MessageBoxW(g_hwnd, L"mcla.exe is already running.", L"MCLA Launcher", MB_ICONINFORMATION); return; }
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + g_mclaExe + L"\"";
    std::wstring env = buildChildEnv();
    if (CreateProcessW(g_mclaExe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, (void*)env.c_str(), g_gameDir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else MessageBoxW(g_hwnd, L"Cannot start mcla.exe", L"MCLA Launcher", MB_ICONERROR);
}
static void openFolder(const std::wstring& p) {
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// WebMessageReceived handler (via ICoreWebView2WebMessageReceivedEventHandler)
struct MsgHandler : public ICoreWebView2WebMessageReceivedEventHandler {
    ULONG ref = 1;
    STDMETHOD(QueryInterface)(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == IID_ICoreWebView2WebMessageReceivedEventHandler) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++ref; }
    STDMETHOD_(ULONG, Release)() override { ULONG r = --ref; if (!r) delete this; return r; }
    STDMETHOD(Invoke)(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        (void)sender;
        LPWSTR src = nullptr; bool trusted = false;
        if (SUCCEEDED(args->get_Source(&src)) && src) { trusted = wcsncmp(src, kOrigin, _countof(kOrigin) - 1) == 0; CoTaskMemFree(src); }
        if (!trusted) return S_OK;
        LPWSTR raw = nullptr; if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
        std::string msg = narrow(raw); CoTaskMemFree(raw);
        size_t pos = 0; JVal root = jparse(msg, pos);
        const JVal* a = jget(root, "action");
        std::string action = (a && a->type == JVal::STR) ? a->s : "";
        const JVal* ex = jget(root, "extra");
        const JVal* tm = ex ? jget(*ex, "toml") : nullptr;
        JVal none; const JVal& extraToml = tm ? *tm : none;

        if (action == "ready") {
            std::string js = "if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadConfig){window.MCLALauncherAPI.loadConfig(" + tomlToJson() + ");}";
            eval(js);
        } else if (action == "dragWindow") {
            ReleaseCapture(); SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } else if (action == "launchGame") {
            writeToml(extraToml, g_ini, g_cacheDir + L"\\mcla.toml"); launchGame();
        } else if (action == "saveConfig") {
            writeToml(extraToml, g_ini, g_cacheDir + L"\\mcla.toml");
        } else if (action == "browseFiles") {
            openFolder(g_gameDir);
        } else if (action == "openLogs") {
            openFolder(g_cacheDir);
        } else if (action == "minimizeWindow") {
            ShowWindow(g_hwnd, SW_MINIMIZE);
        } else if (action == "exitLauncher") {
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
        return S_OK;
    }
};

// NavigationCompleted -> add transparent-window class on html+body (v3 contract).
struct NavHandler : public ICoreWebView2NavigationCompletedEventHandler {
    ULONG ref = 1;
    STDMETHOD(QueryInterface)(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == IID_ICoreWebView2NavigationCompletedEventHandler) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++ref; }
    STDMETHOD_(ULONG, Release)() override { ULONG r = --ref; if (!r) delete this; return r; }
    STDMETHOD(Invoke)(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) override {
        eval("document.documentElement.classList.add('transparent-window');document.body.classList.add('transparent-window');");
        if (g_controller) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return S_OK;
    }
};

// Created-controller callback: set background, hook events, map virtual host, navigate.
struct ControllerHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    ULONG ref = 1;
    STDMETHOD(QueryInterface)(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++ref; }
    STDMETHOD_(ULONG, Release)() override { ULONG r = --ref; if (!r) delete this; return r; }
    STDMETHOD(Invoke)(HRESULT hr, ICoreWebView2Controller* controller) override {
        if (FAILED(hr) || !controller) { wv2Fail(FAILED(hr) ? hr : E_FAIL); return S_OK; }
        g_controller = controller;
        g_controller->get_CoreWebView2(&g_web);
        if (!g_web) { wv2Fail(E_FAIL); return S_OK; }
        // TRUE transparent background (ARGB 0) and raw-pixel bounds keep the
        // fixed CSS layout stable across per-monitor DPI changes.
        ComPtr<ICoreWebView2Controller3> c3;
        if (SUCCEEDED(g_controller.As(&c3))) {
            c3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
            c3->put_ShouldDetectMonitorScaleChanges(TRUE);
            c3->put_DefaultBackgroundColor({ 0, 0, 0, 0 });
        }
        ComPtr<ICoreWebView2Settings> st;
        if (SUCCEEDED(g_web->get_Settings(&st))) {
#ifndef MCLA_DEBUG
            st->put_AreDevToolsEnabled(FALSE);
#endif
            st->put_AreDefaultContextMenusEnabled(FALSE);
        }
        // Handlers start at refcount 1; the event source AddRefs, so drop ours (no leak).
        MsgHandler* mh = new MsgHandler(); g_web->add_WebMessageReceived(mh, nullptr); mh->Release();
        NavHandler* nh = new NavHandler(); g_web->add_NavigationCompleted(nh, nullptr); nh->Release();
        // The page may only ever show our own virtual host; everything else is cancelled or blocked.
        g_web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                LPWSTR u = nullptr; bool ok = false;
                if (SUCCEEDED(a->get_Uri(&u)) && u) { ok = wcsncmp(u, kOrigin, _countof(kOrigin) - 1) == 0; CoTaskMemFree(u); }
                if (!ok) a->put_Cancel(TRUE);
                return S_OK;
            }).Get(), nullptr);
        g_web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT { a->put_Handled(TRUE); return S_OK; }).Get(), nullptr);
        ComPtr<ICoreWebView2_3> w3;
        if (SUCCEEDED(g_web.As(&w3))) w3->SetVirtualHostNameToFolderMapping(L"mcla.launcher", g_uiDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        RECT r; GetClientRect(g_hwnd, &r); g_controller->SetBoundsAndZoomFactor(r, g_zoom);
        std::wstring url=L"https://mcla.launcher/index.html?transparent=1";
    wchar_t tabName[64];
    if (GetEnvironmentVariableW(L"MCLA_LAUNCHER_TAB",tabName,64)>0) url+=L"&tab="+std::wstring(tabName);
    g_web->Navigate(url.c_str());
        return S_OK;
    }
};

// Environment created -> create controller on our window.
struct EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    ULONG ref = 1;
    STDMETHOD(QueryInterface)(REFIID iid, void** pp) override {
        if (iid == IID_IUnknown || iid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++ref; }
    STDMETHOD_(ULONG, Release)() override { ULONG r = --ref; if (!r) delete this; return r; }
    STDMETHOD(Invoke)(HRESULT hr, ICoreWebView2Environment* env) override {
        if (FAILED(hr) || !env) { wv2Fail(FAILED(hr) ? hr : E_FAIL); return S_OK; }
        ControllerHandler* ch = new ControllerHandler();
        HRESULT h2 = env->CreateCoreWebView2Controller(g_hwnd, ch); ch->Release();
        if (FAILED(h2)) wv2Fail(h2);
        return S_OK;
    }
};

// ------------------------------- paths --------------------------------------------------------
static bool hasGame(const std::wstring& d) { return !d.empty() && exists(d + L"\\mcla.exe"); }
// gameDir: ini key, else launcher folder, else ..\..\mcla pc\build, else old repo layout, else ask.
static bool resolveGameDir() {
    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"launcher", L"gameDir", L"", buf, MAX_PATH, g_ini.c_str());
    std::wstring cand[] = { buf, g_dir, g_dir + L"\\..\\..\\mcla pc\\build", dirOf(g_dir) + L"\\build" };
    for (auto& c : cand) if (hasGame(c)) {
        wchar_t full[MAX_PATH]; GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr); g_gameDir = full; return true;
    }
    for (;;) {
        BROWSEINFOW bi = {}; bi.lpszTitle = L"Select the folder that contains mcla.exe"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi); if (!pidl) return false;
        wchar_t pth[MAX_PATH]; BOOL ok = SHGetPathFromIDListW(pidl, pth); CoTaskMemFree(pidl);
        if (ok && hasGame(pth)) { g_gameDir = pth; WritePrivateProfileStringW(L"launcher", L"gameDir", pth, g_ini.c_str()); return true; }
        MessageBoxW(nullptr, L"mcla.exe was not found in that folder.", L"MCLA Launcher", MB_ICONWARNING);
    }
}

// ------------------------------- window -------------------------------------------------------
static void LayoutLauncher(HWND h, UINT dpi, HMONITOR monitor, const RECT* suggested = nullptr) {
    g_dpi = dpi ? dpi : GetDpiForWindow(h);
    const int wantedW = MulDiv(WIN_W, (int)g_dpi, 96);
    const int wantedH = MulDiv(WIN_H, (int)g_dpi, 96);

    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    RECT work{};
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        work = mi.rcWork;
    } else {
        work.left = work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int workLeft = (int)work.left, workTop = (int)work.top;
    const int availW = std::max(1, (int)work.right - workLeft);
    const int availH = std::max(1, (int)work.bottom - workTop);
    // Normally 1.0. If the work area is smaller than the DPI-scaled layout,
    // shrink host and content together so the complete CSS layout remains visible.
    g_zoom = std::min(1.0, std::min(availW / (double)wantedW, availH / (double)wantedH));
    g_w = std::max(1, (int)std::lround(wantedW * g_zoom));
    g_h = std::max(1, (int)std::lround(wantedH * g_zoom));
    int x = suggested ? (int)suggested->left : workLeft + (availW - g_w) / 2;
    int y = suggested ? (int)suggested->top : workTop + (availH - g_h) / 2;
    x = std::max(workLeft, std::min(x, workLeft + availW - g_w));
    y = std::max(workTop, std::min(y, workTop + availH - g_h));
    SetWindowPos(h, nullptr, x, y, g_w, g_h, SWP_NOZORDER | SWP_NOACTIVATE);

    const RECT bounds{0, 0, g_w, g_h};
    if (g_controller) g_controller->SetBoundsAndZoomFactor(bounds, g_zoom);
    HRGN region = CreateRectRgn(0, 0, g_w, g_h);
    if (region) SetWindowRgn(h, region, TRUE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && g_controller) {
            RECT r; GetClientRect(h, &r); g_controller->SetBoundsAndZoomFactor(r, g_zoom);
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(l);
        HMONITOR target = MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST);
        LayoutLauncher(h, HIWORD(w), target, suggested);
        return 0;
    }
    case WM_NCHITTEST: {
        // Top 24 CSS px strip (left of the window controls) drags the borderless window.
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) }; ScreenToClient(h, &p);
        const double cssToPhysical = g_dpi / 96.0 * g_zoom;
        const int dragH = (int)std::lround(24 * cssToPhysical);
        const int controlsW = (int)std::lround(80 * cssToPhysical);
        if (p.y >= 0 && p.y < dragH && p.x < g_w - controlsW) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    // The manifest declares PerMonitorV2. The runtime call is best-effort only;
    // calling SetProcessDPIAware as a fallback can downgrade an already-aware process.
    if (wcsstr(GetCommandLineW(), L"--selftest")) return selfTest();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    g_dir = dirOf(p);
    std::wstring repo = dirOf(g_dir);
    g_ini = g_dir + L"\\mcla_launcher.ini";
    if (!resolveGameDir()) return 0;
    if (!exists(g_ini) && exists(g_gameDir + L"\\mcla_launcher.ini")) g_ini = g_gameDir + L"\\mcla_launcher.ini";
    g_mclaExe = g_gameDir + L"\\mcla.exe";
    g_uiDir = g_dir + L"\\launcher\\ui";
    if (!exists(g_uiDir + L"\\index.html")) g_uiDir = repo + L"\\tools\\launcher\\ui";
    if (!exists(g_uiDir + L"\\index.html")) g_uiDir = g_gameDir + L"\\launcher\\ui";
    if (!exists(g_uiDir + L"\\index.html")) { MessageBoxW(nullptr, L"launcher\\ui\\index.html not found.", L"MCLA Launcher", MB_ICONERROR); return 1; }
    g_cacheDir = g_gameDir + L"\\cache"; CreateDirectoryW(g_cacheDir.c_str(), nullptr);

    WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"MclaLauncherWv2";
    wc.hIcon = (HICON)LoadImageW(hi, L"launcher.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    // Open on the foreground window's monitor (the user's active display), not
    // necessarily the primary display. Create hidden first, then query that
    // monitor's actual DPI; Windows does not send WM_DPICHANGED on creation.
    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST)
                                  : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO target{}; target.cbSize = sizeof(target);
    POINT probe{0, 0};
    if (GetMonitorInfoW(monitor, &target)) probe = { target.rcWork.left + 8, target.rcWork.top + 8 };
    g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED, wc.lpszClassName,
                             L"Midnight Club: Los Angeles — Launcher", WS_POPUP,
                             probe.x, probe.y, 1, 1, nullptr, nullptr, hi, nullptr);
    if (!g_hwnd) return (int)GetLastError();
    LayoutLauncher(g_hwnd, GetDpiForWindow(g_hwnd), monitor);
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);

    // WebView2 user-data folder: %LOCALAPPDATA%\MCLA Launcher\WebView2 (falls back to next to the exe).
    std::wstring ud = g_dir + L"\\WebView2UserData";
    wchar_t la[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) {
        std::wstring d = std::wstring(la) + L"\\MCLA Launcher\\WebView2";
        if (SHCreateDirectoryExW(nullptr, d.c_str(), nullptr) == ERROR_SUCCESS || exists(d)) ud = d;
    }
    if (ud == g_dir + L"\\WebView2UserData") CreateDirectoryW(ud.c_str(), nullptr);
    EnvHandler* eh = new EnvHandler();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, ud.c_str(), nullptr, eh);
    eh->Release();
    if (FAILED(hr)) wv2Fail(hr);

    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_controller) g_controller->Close();
    g_web.Reset(); g_controller.Reset();
    CoUninitialize();
    return g_exit;
}
