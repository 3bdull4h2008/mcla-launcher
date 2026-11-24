// mcla_launcher.cpp ΓÇö standalone WebView2 launcher host for MCLA PC (Windows x64).
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
// NEVER touches build/mcla.exe.
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <dcomp.h>
#include <wrl.h>
#include <wrl/client.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <map>
#include <codecvt>
#include "wv2/WebView2.h"

using Microsoft::WRL::ComPtr;

// ------------------------------- config -------------------------------------------------------
static const int WIN_W = 650, WIN_H = 446;
static HWND       g_hwnd = nullptr;
static std::wstring g_dir, g_uiDir, g_gameDir, g_cacheDir, g_mclaExe, g_ini;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2>           g_web;
static bool g_ready = false;

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

// ------------------------------- mcla.toml <-> JS ---------------------------------------------
// Read cache/mcla.toml into a flat JSON object { key: value } for loadConfig().
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
        if (!v.empty() && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (!first) out += ",";
        out += "\"" + jesc(k) + "\":";
        bool isNum = !v.empty(); for (char c : v) if (!isdigit((unsigned char)c) && c != '.' && c != '-' && c != '+') isNum = false;
        if (isNum) out += v;
        else if (v == "true" || v == "false") out += v;
        else out += "\"" + jesc(v) + "\"";
        first = false;
    }
    out += "}"; return out;
}
// Write payload.extra.toml (section -> key -> value) to cache/mcla.toml, preserving other lines.
// Minimal JSON object parser for payload.extra.toml: { section: { key: scalar, ... }, ... }
// Values are only strings/numbers/bools here. Writes flat, line-preserving mcla.toml.
struct JVal { enum T{STR,NUM,BOOL,OBJ,NUL} type=NUL; std::string s; std::vector<std::pair<std::string,JVal>> obj; };
static size_t jskip(const std::string& j,size_t i){ while(i<j.size()&&isspace((unsigned char)j[i]))i++; return i; }
static std::string jstr(const std::string& j,size_t& i){
    // j[i]=='"'
    i++; std::string o;
    while(i<j.size()&&j[i]!='"'){ if(j[i]=='\\'&&i+1<j.size()){i++; char c=j[i]; o+= (c=='n'?'\n':(c=='r'?'\r':(c=='t'?'\t':c)));} else o+=j[i]; i++; }
    if(i<j.size())i++; return o;
}
static JVal jparse(const std::string& j,size_t& i){
    JVal v; i=jskip(j,i);
    if(i>=j.size())return v;
    if(j[i]=='"'){ v.type=JVal::STR; v.s=jstr(j,i); return v; }
    if(j[i]=='{'){
        v.type=JVal::OBJ; i++;
        while(true){ i=jskip(j,i); if(i>=j.size()||j[i]=='}'){if(i<j.size())i++;break;}
            std::string k = (j[i]=='"')?jstr(j,i):"";
            i=jskip(j,i); if(i<j.size()&&j[i]==':')i++;
            std::string key=k; JVal val=jparse(j,i);
            // map UI key -> real cvar/env name
            if(key=="prompts")key="mcla_button_prompts";
            else if(key=="vibration")key="mcla_vibration";
            else if(key=="deadzone")key="mcla_left_stick_deadzone";
            v.obj.push_back({key,val});
            i=jskip(j,i); if(i<j.size()&&j[i]==',')i++;
        }
        return v;
    }
    size_t st=i; while(i<j.size()&&j[i]!=','&&j[i]!='}'&&!isspace((unsigned char)j[i]))i++;
    std::string tok=j.substr(st,i-st);
    if(tok=="true"||tok=="false"){v.type=JVal::BOOL;v.s=tok;}
    else {v.type=JVal::NUM;v.s=tok;}
    return v;
}
static void writeToml(const std::string& json){
    size_t i=0; JVal root=jparse(json,i);
    if(root.type!=JVal::OBJ)return;
    std::vector<std::pair<std::string,std::string>> sets;
    std::vector<std::pair<std::string,std::string>> envSets;
    for(auto& sec:root.obj){
        if(sec.second.type!=JVal::OBJ)continue;
        for(auto& kv:sec.second.obj){
            std::string val;
            val=kv.second.s;
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
                val=it->second;
            }
            if(sec.first=="_env"){
                std::string k=kv.first;
                if(k=="resolution_scale")k="MCLA_RENDER_SCALE";
                else if(k=="vsync")k="MCLA_VSYNC";
                envSets.push_back({k,k+"="+val});
            } else {
                sets.push_back({kv.first,kv.first+" = "+val});
            }
        }
    }
    // Route _env settings into the ini [env] section (merged with existing, preserving others)
    if(!envSets.empty()){
        std::wstring ini=g_dir+L"\\mcla_launcher.ini";
        std::ifstream in(ini); std::vector<std::string> lines; std::string l;
        bool inEnv=false;
        while(std::getline(in,l)){
            if(l.size()&&l[0]=='['){ inEnv = (l=="[env]"); }
            if(inEnv){
                for(auto& kv:envSets){
                    size_t s=l.find_first_not_of(" \t");
                    if(s==std::string::npos||l[s]==';'||l[s]=='[')continue;
                    size_t eq=l.find('=',s); if(eq==std::string::npos)continue;
                    std::string k=l.substr(s,eq-s); while(!k.empty()&&k.back()==' ')k.pop_back();
                    if(k==kv.first){ l=kv.second; break; }
                }
            }
            lines.push_back(l);
        }
        // append any not found
        for(auto& kv:envSets){
            bool found=false;
            for(auto& line:lines){ size_t s=line.find_first_not_of(" \t"); if(s==std::string::npos||line[s]==';'||line[s]=='[')continue; size_t eq=line.find('=',s); if(eq==std::string::npos)continue; std::string k=line.substr(s,eq-s); while(!k.empty()&&k.back()==' ')k.pop_back(); if(k==kv.first){found=true;break;} }
            if(!found){
                if(lines.empty() || lines.back()!="[env]") lines.push_back("[env]");
                lines.push_back(kv.second);
            }
        }
        std::ofstream out(ini,std::ios::binary); for(auto& line:lines)out<<line<<"\r\n";
    }
    if(sets.empty())return;
    std::wstring path=g_cacheDir+L"\\mcla.toml";
    std::ifstream in(path); std::vector<std::string> lines; std::string l;
    while(std::getline(in,l))lines.push_back(l);
    for(auto& kv:sets){
        bool found=false;
        for(auto& line:lines){
            size_t s=line.find_first_not_of(" \t"); if(s==std::string::npos||line[s]=='#'||line[s]=='[')continue;
            size_t eq=line.find('=',s); if(eq==std::string::npos)continue;
            std::string k=line.substr(s,eq-s); while(!k.empty()&&k.back()==' ')k.pop_back();
            if(k==kv.first){line=kv.second;found=true;break;}
        }
        if(!found)lines.push_back(kv.second);
    }
    std::ofstream out(path,std::ios::binary); for(auto& line:lines)out<<line<<"\r\n";
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
    if(x!=std::wstring::npos){ setEnv(L"MCLA_WINDOW_W="+res.substr(0,x)); setEnv(L"MCLA_WINDOW_H="+res.substr(x+1)); }
    setEnv(L"MCLA_RENDER_SCALE="+get(L"scale",L"2"));
    std::wstring fs=get(L"fullscreen",L"1");
    setEnv(std::wstring(L"MCLA_DISPLAY_MODE=")+(fs==L"1"?L"borderless_fullscreen":L"windowed"));
    std::wstring name=get(L"name",L"SWA"); if(!name.empty())setEnv(L"MCLA_PROFILE_NAME="+name);
    int ai=_wtoi(get(L"aspect",L"0").c_str()),aw=0;
    if(ai==2)aw=1680; else if(ai==3)aw=2560;
    else if(ai==0&&x!=std::wstring::npos)aw=(int)(720.0*_wtoi(res.substr(0,x).c_str())/std::max(1,_wtoi(res.substr(x+1).c_str()))+0.5);
    if(aw>1280){ setEnv(L"MCLA_ASPECT_FIX=1"); setEnv(L"MCLA_ASPECT_WIDTH="+std::to_wstring(std::min(aw,4095))); }
    if(_wtoi(get(L"blur",L"0").c_str())==1)setEnv(L"MCLA_MOTION_BLUR=0");
    static const wchar_t* kLod[]={L"0.75",L"1.0",L"1.5",L"2.0"}; int lod=_wtoi(get(L"lod",L"1").c_str());
    if(lod>=0&&lod<4&&lod!=1)setEnv(std::wstring(L"MCLA_LOD_SCALE=")+kLod[lod]);
    static const wchar_t* kFps[]={L"30",L"60",L"120",L"0"}; int fps=_wtoi(get(L"fps",L"3").c_str());
    if(fps>=0&&fps<4&&fps!=3)setEnv(std::wstring(L"MCLA_FPS_CAP=")+kFps[fps]);
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

static void launchGame() {
    if (!exists(g_mclaExe)) { MessageBoxW(g_hwnd, (L"mcla.exe not found:\n"+g_mclaExe).c_str(), L"MCLA Launcher", MB_ICONERROR); return; }
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
        LPWSTR raw = nullptr; if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
        std::string msg = narrow(raw); CoTaskMemFree(raw);
        std::string action, extraToml, cliArgs;
        auto getStr = [&](const std::string& key, std::string& dst) {
            size_t p = msg.find("\"" + key + "\"");
            if (p == std::string::npos) return;
            p = msg.find(':', p + key.size() + 2); if (p == std::string::npos) return;
            p++; while (p < msg.size() && (msg[p] == ' ' || msg[p] == '"')) p++;
            size_t e = msg.find('"', p); if (e != std::string::npos) dst = msg.substr(p, e - p);
        };
        // action is the FIRST string after "action"
        size_t ap = msg.find("\"action\"");
        if (ap != std::string::npos) {
            ap = msg.find(':', ap + 8) + 1; while (ap < msg.size() && msg[ap] == ' ') ap++;
            if (ap < msg.size() && msg[ap] == '"') { size_t e = msg.find('"', ap + 1); if (e != std::string::npos) action = msg.substr(ap + 1, e - ap - 1); }
        }
        // find the "toml" object and "cliArgs" array inside "extra" via brace/bracket matching
        auto grabBlock = [&](const std::string& key, char open, char close, std::string& dst) {
            size_t p = msg.find("\"" + key + "\"");
            if (p == std::string::npos) return;
            p = msg.find(open, p); if (p == std::string::npos) return;
            int depth = 0; size_t start = p;
            for (; p < msg.size(); ++p) { if (msg[p] == open) depth++; else if (msg[p] == close) { depth--; if (!depth) { dst = msg.substr(start, p - start + 1); return; } } }
        };
        grabBlock("toml", '{', '}', extraToml);
        grabBlock("cliArgs", '[', ']', cliArgs);

        if (action == "ready") {
            std::string js = "if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadConfig){window.MCLALauncherAPI.loadConfig(" + tomlToJson() + ");}";
            eval(js);
        } else if (action == "dragWindow") {
            ReleaseCapture(); SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } else if (action == "launchGame") {
            writeToml(extraToml); launchGame();
        } else if (action == "saveConfig") {
            writeToml(extraToml);
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
        if (FAILED(hr) || !controller) { MessageBoxW(g_hwnd, L"WebView2 controller creation failed", L"MCLA Launcher", MB_ICONERROR); return S_OK; }
        g_controller = controller;
        g_controller->get_CoreWebView2(&g_web);
        // TRUE transparent background (ARGB 0) ΓÇö the core of per-pixel alpha.
        ComPtr<ICoreWebView2Controller3> c3;
        if (SUCCEEDED(g_controller.As(&c3))) c3->put_DefaultBackgroundColor({ 0, 0, 0, 0 });
        g_web->add_WebMessageReceived(new MsgHandler(), nullptr);
        g_web->add_NavigationCompleted(new NavHandler(), nullptr);
        ComPtr<ICoreWebView2_3> w3;
        if (SUCCEEDED(g_web.As(&w3))) w3->SetVirtualHostNameToFolderMapping(L"mcla.launcher", g_uiDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        RECT r; GetClientRect(g_hwnd, &r); g_controller->put_Bounds(r);
        g_web->Navigate(L"https://mcla.launcher/index.html?transparent=1");
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
        if (FAILED(hr) || !env) { MessageBoxW(g_hwnd, L"WebView2 environment failed (install WebView2 Runtime)", L"MCLA Launcher", MB_ICONERROR); return S_OK; }
        env->CreateCoreWebView2Controller(g_hwnd, new ControllerHandler());
        return S_OK;
    }
};

// ------------------------------- window -------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (g_controller) { RECT r; GetClientRect(h, &r); g_controller->put_Bounds(r); }
        return 0;
    case WM_NCHITTEST: {
        // Top 24px strip (left of the window controls) drags the borderless window.
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) }; ScreenToClient(h, &p);
        if (p.y >= 0 && p.y < 24 && p.x < WIN_W - 80) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    g_dir = dirOf(p);
    // Repo layout: <repo>\build\mcla_launcher.exe -> gameDir = g_dir, repo = dirOf(g_dir)
    std::wstring repo = dirOf(g_dir);
    g_gameDir = g_dir;
    g_mclaExe = g_dir + L"\\mcla.exe";
    if (!exists(g_mclaExe)) { g_gameDir = repo + L"\\build"; g_mclaExe = g_gameDir + L"\\mcla.exe"; }
    g_uiDir = g_dir + L"\\launcher\\ui";
    if (!exists(g_uiDir + L"\\index.html")) g_uiDir = repo + L"\\tools\\launcher\\ui";
    g_cacheDir = g_dir + L"\\cache"; if (!exists(g_cacheDir)) g_cacheDir = repo + L"\\build\\cache";
    g_ini = g_dir + L"\\mcla_launcher.ini";

    WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"MclaLauncherWv2";
    wc.hIcon = (HICON)LoadImageW(hi, L"launcher.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // WS_EX_NOREDIRECTIONBITMAP: DWM does not give the window a normal redirection surface;
    // WebView2's DirectComposition swap chain supplies the pixels, with per-pixel alpha.
    g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED, wc.lpszClassName,
                             L"Midnight Club: Los Angeles ΓÇö Launcher", WS_POPUP | WS_VISIBLE,
                             (GetSystemMetrics(SM_CXSCREEN) - WIN_W) / 2,
                             (GetSystemMetrics(SM_CYSCREEN) - WIN_H) / 2, WIN_W, WIN_H,
                             nullptr, nullptr, hi, nullptr);
    // WS_EX_NOREDIRECTIONBITMAP windows have no redirection surface, so Windows
    // won't hit-test them. Give the window an explicit region so clicks land here.
    { HRGN rgn = CreateRectRgn(0,0,WIN_W,WIN_H); SetWindowRgn(g_hwnd,rgn,TRUE); }
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);

    // WebView2 with user-data folder next to exe.
    std::wstring ud = g_dir + L"\\WebView2UserData";
    CreateDirectoryW(ud.c_str(), nullptr);
    CreateCoreWebView2EnvironmentWithOptions(nullptr, ud.c_str(), nullptr, new EnvHandler());

    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_controller) g_controller->Close();
    CoUninitialize();
    return 0;
}
