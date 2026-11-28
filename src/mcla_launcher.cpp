

#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <dcomp.h>
#include <wrl.h>
#include <wrl/client.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <urlmon.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include "wv2/WebView2.h"
#include "generated_settings_contract.h"
#include "iso_manifest.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
typedef HRESULT (WINAPI* CreateWebView2EnvironmentFn)(REFIID, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

static const int WIN_W = 650, WIN_H = 446;
static int    g_w = WIN_W, g_h = WIN_H;
static UINT   g_dpi = 96;
static double g_zoom = 1.0;
static int    g_exit = 0;
static HWND       g_hwnd = nullptr;
static std::wstring g_dir, g_uiDir, g_gameDir, g_cacheDir, g_mclaExe, g_ini;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2>           g_web;
static const wchar_t kOrigin[] = L"https://mcla.launcher/";
static const wchar_t kRuntimeUrl[] = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";
static const char kInstallVersion[] = "1.0.0-rc1";
static const wchar_t kEngineUrl[] = L"https://github.com/3bdull4h2008/MCLArecomp_native/releases/download/engine-v1/mcla.exe";
static const char kEngineSha256[] = "9abb0b72d5627edd6f982f49c5637a0ea25b18a50ef00be01f96c2cdd1869386";
struct EngineFile { const wchar_t* name; const wchar_t* url; const char* sha; };
static const EngineFile kEngineFiles[] = {
    { L"mcla.exe", kEngineUrl, kEngineSha256 },
    { L"SDL3.dll", L"https://github.com/3bdull4h2008/MCLArecomp_native/releases/download/engine-v1/SDL3.dll", "771b416c3cb61e1bc6ab6513f257ef85e908a051a1790d49f7381c89f7e5bd25" },
    { L"dxcompiler.dll", L"https://github.com/3bdull4h2008/MCLArecomp_native/releases/download/engine-v1/dxcompiler.dll", "1220478c87af97551d2342906c5a08d888ee9fa26f6f99b6072d674801f492d1" },
    { L"dxil.dll", L"https://github.com/3bdull4h2008/MCLArecomp_native/releases/download/engine-v1/dxil.dll", "e038a9755387fc396a9a5f393d292b9318cf6ac7e520e9283bbc3088c5f8ac93" },
};
static const wchar_t kReadyRel[] = L"\\cache\\mcla_ready.json";
static bool g_installMode = false;
static std::wstring g_installSource;
static std::wstring g_isoPath;
static std::string slurp(const std::wstring& p);
static void eval(const std::string& js);
static void cleanupDevFiles(const std::wstring& gameDir);
static std::string normalizeLang(const std::string& v) {
    if (v == "ar" || v == "fr" || v == "es" || v == "de" || v == "it") return v;
    return "en";
}

static std::wstring dirOf(const std::wstring& p) {
    auto i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}
static bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool writeResource(HMODULE mod, int id, const std::wstring& path) {
    HRSRC r = FindResourceW(mod, MAKEINTRESOURCE(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(mod, r);
    if (!g) return false;
    DWORD size = SizeofResource(mod, r);
    void* data = LockResource(g);
    if (!data || !size) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, size, &written, nullptr) && written == size;
    CloseHandle(h);
    return ok != FALSE;
}

static std::wstring ensureBundledResources(HMODULE mod) {
    wchar_t la[MAX_PATH];
    std::wstring base;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH))
        base = std::wstring(la) + L"\\MCLA_Launcher";
    else
        base = L"C:\\MCLA_Launcher";
    const std::wstring webview = base + L"\\WebView2";
    const std::wstring ui = webview + L"\\ui";
    SHCreateDirectoryExW(nullptr, (ui + L"\\fonts").c_str(), nullptr);
    writeResource(mod, 1, ui + L"\\index.html");
    writeResource(mod, 2, ui + L"\\launcher-bridge.js");
    writeResource(mod, 3, ui + L"\\launcher.schema.json");
    writeResource(mod, 4, ui + L"\\logo.png");
    writeResource(mod, 5, ui + L"\\fonts\\rajdhani-600.woff2");
    writeResource(mod, 6, ui + L"\\fonts\\rajdhani-700.woff2");
    writeResource(mod, 7, ui + L"\\fonts\\saira-600.woff2");
    writeResource(mod, 8, ui + L"\\fonts\\saira-700.woff2");
    writeResource(mod, 10, ui + L"\\fonts\\noto-sans-arabic-700.woff2");
    writeResource(mod, 11, ui + L"\\fonts\\OFL-NotoSansArabic.txt");
    writeResource(mod, 9, webview + L"\\WebView2Loader.dll");
    return webview;
}

static std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}

static std::string jesc(const std::string& s) {
    std::string o; for (char c : s) {
        switch (c) { case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
                     case '\n': o += "\\n"; break; case '\r': o += "\\r"; break;
                     default: if ((unsigned char)c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; } else o += c; }
    } return o;
}

static void wv2Fail(HRESULT hr) {
    wchar_t m[512];
    swprintf_s(m, L"The WebView2 Runtime could not be started (0x%08X).\nDownload it from:\n%s\n\nOpen the download page now?", (unsigned)hr, kRuntimeUrl);
    if (MessageBoxW(g_hwnd, m, L"MCLA Launcher", MB_YESNO | MB_ICONERROR) == IDYES)
        ShellExecuteW(nullptr, L"open", kRuntimeUrl, nullptr, nullptr, SW_SHOWNORMAL);
    g_exit = 1; PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

static std::string tomlUnquote(const std::string& v) {
    std::string o; for (size_t i = 1; i + 1 < v.size(); ++i) { if (v[i] == '\\' && i + 2 < v.size()) i++; o += v[i]; } return o;
}
static std::string tomlQuote(const std::string& v) {
    std::string o = "\""; for (char c : v) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o + "\"";
}

static void parseTomlIntoMap(const std::wstring& path, std::map<std::string, std::string>& jsonMap) {
    std::ifstream f(path);
    if (!f.good()) return;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos || line[s] == '#' || line[s] == '[') continue;
        size_t eq = line.find('=', s); if (eq == std::string::npos) continue;
        std::string k = line.substr(s, eq - s);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        std::string v = line.substr(eq + 1);
        size_t vs = v.find_first_not_of(" \t");
        if (vs == std::string::npos) v.clear(); else v.erase(0, vs);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        bool quoted = v.size() >= 2 && v.front() == '"' && v.back() == '"';
        if (quoted) v = tomlUnquote(v);

        char* end = nullptr; double d = strtod(v.c_str(), &end);
        bool isNum = !quoted && !v.empty() && !isspace((unsigned char)v[0]) &&
                     v.find_first_of("xXnNiI") == std::string::npos && *end == 0 && std::isfinite(d);
        std::string valStr;
        if (isNum) { char b[40]; snprintf(b, sizeof b, "%.15g", d); valStr = b; }
        else if (!quoted && (v == "true" || v == "false")) valStr = v;
        else valStr = "\"" + jesc(v) + "\"";
        jsonMap[k] = valStr;
    }
}

static std::string tomlToJson(const std::wstring& gameDir = g_gameDir, const std::wstring& cacheDir = g_cacheDir, const std::wstring& iniPath = g_ini) {
    std::map<std::string, std::string> jsonMap;

    if (!gameDir.empty()) parseTomlIntoMap(gameDir + L"\\mcla.toml", jsonMap);
    if (!cacheDir.empty()) parseTomlIntoMap(cacheDir + L"\\mcla.toml", jsonMap);

    std::string winW, winH;
    if (!iniPath.empty() && exists(iniPath)) {
        std::vector<wchar_t> sec(32768);
        GetPrivateProfileSectionW(L"env", sec.data(), (DWORD)sec.size(), iniPath.c_str());
        for (wchar_t* p = sec.data(); *p; p += wcslen(p) + 1) {
            std::wstring we = p;
            size_t c = we.find(L" ;"); if (c != std::wstring::npos) we.resize(c);
            while (!we.empty() && (we.back() == L' ' || we.back() == L'\r')) we.pop_back();
            if (we.empty() || we[0] == L';') continue;
            size_t eq = we.find(L'='); if (eq == std::wstring::npos || eq == 0) continue;
            std::string k = narrow(we.substr(0, eq));
            std::string v = narrow(we.substr(eq + 1));
            while (!k.empty() && k.back() == ' ') k.pop_back();
            size_t vs = v.find_first_not_of(" \t");
            if (vs == std::string::npos) v.clear(); else v.erase(0, vs);

            if (k == "MCLA_WINDOW_W") winW = v;
            else if (k == "MCLA_WINDOW_H") winH = v;

            std::string cvar;
            for (const auto& m : kGeneratedEnvToCvarMap) {
                if (m.first == k) { cvar = m.second; break; }
            }
            if (cvar.empty() && k == "MCLA_ASPECT_RATIO") cvar = "mcla_aspect_ratio";

            char* end = nullptr; double d = strtod(v.c_str(), &end);
            bool isNum = !v.empty() && !isspace((unsigned char)v[0]) &&
                         v.find_first_of("xXnNiI") == std::string::npos && *end == 0 && std::isfinite(d);
            std::string valStr;
            if (isNum) { char b[40]; snprintf(b, sizeof b, "%.15g", d); valStr = b; }
            else if (v == "true" || v == "false") valStr = v;
            else valStr = "\"" + jesc(v) + "\"";

            if (jsonMap.find(k) == jsonMap.end()) jsonMap[k] = valStr;
            if (!cvar.empty() && jsonMap.find(cvar) == jsonMap.end()) jsonMap[cvar] = valStr;
        }
        if (!winW.empty() && !winH.empty() && jsonMap.find("resolution_preset") == jsonMap.end()) {
            jsonMap["resolution_preset"] = "\"" + jesc(winW + "x" + winH) + "\"";
        }
        wchar_t resBuf[64] = {};
        GetPrivateProfileStringW(L"launcher", L"res", L"", resBuf, 64, iniPath.c_str());
        if (resBuf[0] && jsonMap.find("resolution_preset") == jsonMap.end()) {
            jsonMap["resolution_preset"] = "\"" + jesc(narrow(resBuf)) + "\"";
        }
    }
    if (jsonMap.find("display_mode") == jsonMap.end()) {
        auto it = jsonMap.find("fullscreen");
        if (it != jsonMap.end()) {
            const bool isFs = (it->second == "true" || it->second == "1");
            jsonMap["display_mode"] = isFs ? "\"fullscreen\"" : "\"windowed\"";
        }
    }

    std::string out = "{"; bool first = true;
    for (const auto& kv : jsonMap) {
        if (!first) out += ",";
        out += "\"" + jesc(kv.first) + "\":" + kv.second;
        first = false;
    }
    out += "}";
    return out;
}

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
    i++; std::string o;
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

static bool writeFileAtomic(const std::wstring& path, const std::string& content) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = content.empty() ? TRUE : WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
    const BOOL flushed = ok ? FlushFileBuffers(h) : FALSE;
    CloseHandle(h);
    if (!ok || !flushed || written != (DWORD)content.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

static bool sha256OfFile(const std::wstring& path, std::string& hex) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) { CloseHandle(h); return false; }
    HCRYPTHASH hash = 0;
    BOOL ok = CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash);
    BYTE buf[65536]; DWORD n = 0;
    while (ok && ReadFile(h, buf, sizeof buf, &n, nullptr) && n) ok = CryptHashData(hash, buf, n, 0);
    BYTE dig[32]; DWORD dl = sizeof dig;
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, dig, &dl, 0);
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(h);
    if (!ok || dl != 32) return false;
    static const char* hx = "0123456789abcdef";
    char out[65];
    for (int i = 0; i < 32; ++i) { out[i * 2] = hx[dig[i] >> 4]; out[i * 2 + 1] = hx[dig[i] & 15]; }
    out[64] = 0; hex = out; return true;
}

static void dirFingerprint(const std::wstring& dir, unsigned long long& count, unsigned long long& bytes,
                           bool excludeMods = false) {
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (excludeMods && !_wcsicmp(fd.cFileName, L"mods") &&
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirFingerprint(p, count, bytes);
        else { ++count; bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

struct InstallCheck {
    bool complete = false;
    bool markerValid = false;
    bool markerNeedsRefresh = false;
    std::string markerVersion;
    std::vector<std::string> missing;
    std::string hashesJson;
};

static InstallCheck checkInstall(const std::wstring& gameDir, const std::wstring& uiDir) {
    InstallCheck r;
    (void)uiDir;
    std::string hashes = "{";
    size_t hashCount = 0;
    bool allFiles = true;
    std::string exeHash;
    if (sha256OfFile(gameDir + L"\\mcla.exe", exeHash)) {
        hashes += "\"mcla.exe\":\"" + exeHash + "\"";
        ++hashCount;
    } else {
        r.missing.push_back("mcla.exe");
        allFiles = false;
    }
    const DWORD configAttributes = GetFileAttributesW((gameDir + L"\\cache\\mcla.toml").c_str());
    if (configAttributes == INVALID_FILE_ATTRIBUTES || (configAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        r.missing.push_back("cache/mcla.toml");
        allFiles = false;
    }
    const std::wstring dirs[] = { gameDir + L"\\game_data" };
    const char* dirNames[] = { "game_data" };
    for (size_t i = 0; i < 1; ++i) {
        unsigned long long n = 0, b = 0;
        dirFingerprint(dirs[i], n, b, true);
        if (!n) { r.missing.push_back(dirNames[i]); allFiles = false; }
        if (hashCount++) hashes += ",";
        hashes += "\"" + std::string(dirNames[i]) + "\":\"" + std::to_string(n) + ":" + std::to_string(b) + "\"";
    }
    hashes += "}";
    r.hashesJson = hashes;
    r.complete = allFiles;
    const std::wstring marker = gameDir + kReadyRel;
    if (!exists(marker)) { r.missing.push_back("cache/mcla_ready.json"); return r; }
    size_t p = 0; JVal root = jparse(slurp(marker), p);
    const JVal* v = jget(root, "version");
    const JVal* hs = jget(root, "hashes");
    if (!v || v->type != JVal::STR || v->s != kInstallVersion) { r.missing.push_back("version mismatch"); return r; }
    r.markerVersion = v->s;
    bool same = true;
    size_t hp = 0; JVal expected = jparse(hashes, hp);
    for (const auto& kv : expected.obj) {
        const JVal* got = hs ? jget(*hs, kv.first.c_str()) : nullptr;
        if (got && got->type == JVal::STR && got->s == kv.second.s) continue;
        if (got && got->type == JVal::STR && kv.first == "game_data" && jget(*hs, "cache/mcla.toml")) {
            unsigned long long files = 0, bytes = 0;
            dirFingerprint(gameDir + L"\\game_data", files, bytes);
            if (got->s == std::to_string(files) + ":" + std::to_string(bytes)) {
                r.markerNeedsRefresh = true;
                continue;
            }
        }
        same = false;
        r.missing.push_back("hash mismatch: " + kv.first);
    }
    r.markerValid = allFiles && same;
    return r;
}

static bool writeReadyMarker(const std::wstring& gameDir, const std::wstring& uiDir) {
    InstallCheck c = checkInstall(gameDir, uiDir);
    if (!c.complete) return false;
    char stamp[32]; time_t now = time(nullptr);
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    const std::string body = "{\"version\":\"" + std::string(kInstallVersion) + "\",\"created\":\"" + stamp + "\",\"hashes\":" + c.hashesJson + "}\r\n";
    return writeFileAtomic(gameDir + kReadyRel, body);
}

static bool adoptExistingInstall(const std::wstring& gameDir, const std::wstring& uiDir) {
    if (exists(gameDir + kReadyRel)) return false;
    for (const auto& entry : kMclaIsoManifest) {
        const std::string_view name = entry.name;
        if (name != "default.xex" && name != "xarchive_cache.rpf" && name != "xarchive_audlo.rpf" &&
            name != "xarchive_audio.rpf" && name != "xarchive_music.rpf") continue;
        const std::wstring path = gameDir + L"\\game_data\\" + widen(entry.name);
        WIN32_FILE_ATTRIBUTE_DATA file{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &file) ||
            (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        const auto size = (static_cast<unsigned long long>(file.nFileSizeHigh) << 32) | file.nFileSizeLow;
        if (size != static_cast<unsigned long long>(entry.size)) return false;
    }
    return writeReadyMarker(gameDir, uiDir);
}

static std::string installStatusJson(const std::wstring& gameDir, const std::wstring& uiDir) {
    InstallCheck c = checkInstall(gameDir, uiDir);
    std::string s = "{\"ready\":" + std::string(c.markerValid ? "true" : "false") + ",\"complete\":" + std::string(c.complete ? "true" : "false") + ",\"version\":\"" + kInstallVersion + "\",\"missing\":[";
    for (size_t i = 0; i < c.missing.size(); ++i) { if (i) s += ","; s += "\"" + jesc(c.missing[i]) + "\""; }
    s += "]}";
    return s;
}

static bool copyTree(const std::wstring& from, const std::wstring& to, std::string& error, int& files) {
    std::wstring pattern = from + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { error = "source folder not found: " + narrow(from); return false; }
    CreateDirectoryW(to.c_str(), nullptr);
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring src = from + L"\\" + fd.cFileName, dst = to + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = copyTree(src, dst, error, files);
        } else {
            if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) { error = "copy failed: " + narrow(src); ok = false; }
            else ++files;
        }
        if (!ok) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static bool ensureEngine(const std::wstring& gameDir, std::string& error) {
    int done = 0;
    for (const auto& f : kEngineFiles) {
        const std::wstring target = gameDir + L"\\" + f.name;
        std::string hash;
        if (exists(target) && sha256OfFile(target, hash) && hash == f.sha) { ++done; continue; }
        const std::wstring tmp = target + L".download";
        DeleteFileW(tmp.c_str());
        eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.setInstallProgress){window.MCLALauncherAPI.setInstallProgress(" +
             std::to_string(10 + done * 15) + ",'copy')}");
        HRESULT hr = URLDownloadToFileW(nullptr, f.url, tmp.c_str(), 0, nullptr);
        if (FAILED(hr) || !exists(tmp)) { error = std::string("download failed: ") + narrow(f.name); DeleteFileW(tmp.c_str()); return false; }
        hash.clear();
        if (!sha256OfFile(tmp, hash) || hash != f.sha) {
            error = std::string("hash mismatch: ") + narrow(f.name);
            DeleteFileW(tmp.c_str());
            return false;
        }
        if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = std::string("install failed: ") + narrow(f.name);
            return false;
        }
        ++done;
    }
    return true;
}

static bool extractIso(const std::wstring& iso, const std::wstring& gameDir, std::string& error) {
    HANDLE h = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error = "cannot open ISO"; return false; }
    LARGE_INTEGER magicPos{}; magicPos.QuadPart = 0x8001;
    char magic[8] = {}; DWORD got = 0;
    if (!SetFilePointerEx(h, magicPos, nullptr, FILE_BEGIN) || !ReadFile(h, magic, 5, &got, nullptr) || got != 5 || memcmp(magic, "CD001", 5) != 0) {
        CloseHandle(h); error = "not an ISO9660 image"; return false;
    }
    LARGE_INTEGER end{}; GetFileSizeEx(h, &end);
    const std::wstring dataDir = gameDir + L"\\game_data";
    SHCreateDirectoryExW(nullptr, dataDir.c_str(), nullptr);
    bool ok = true;
    for (int i = 0; i < kMclaIsoManifestCount && ok; ++i) {
        const MclaIsoEntry& e = kMclaIsoManifest[i];
        if (e.offset < 0 || e.offset + e.size > end.QuadPart) { error = std::string("ISO entry out of range: ") + e.name; ok = false; break; }
        LARGE_INTEGER pos{}; pos.QuadPart = e.offset;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { error = std::string("seek failed: ") + e.name; ok = false; break; }
        std::wstring out = dataDir + L"\\" + widen(e.name);
        HANDLE o = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (o == INVALID_HANDLE_VALUE) { error = std::string("cannot create: ") + e.name; ok = false; break; }
        char buf[64 * 1024];
        long long left = e.size;
        while (left > 0) {
            DWORD want = (DWORD)std::min<long long>(left, sizeof buf);
            DWORD r = 0, w = 0;
            if (!ReadFile(h, buf, want, &r, nullptr) || r == 0 || !WriteFile(o, buf, r, &w, nullptr) || w != r) { ok = false; break; }
            left -= r;
        }
        CloseHandle(o);
        if (!ok) { error = std::string("extract failed: ") + e.name; break; }
    }
    CloseHandle(h);
    if (!ok) return false;
    SHCreateDirectoryExW(nullptr, (gameDir + L"\\cache").c_str(), nullptr);
    writeFileAtomic(gameDir + L"\\cache\\mcla.toml", "mcla_fps = 60\r\nresolution_scale = 1\r\nvsync = true\r\n");
    SHCreateDirectoryExW(nullptr, (gameDir + L"\\dxil_cache").c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, (gameDir + L"\\game_data\\mods").c_str(), nullptr);
    return true;
}

static bool runInstall(const std::wstring& src, const std::wstring& gameDir, const std::wstring& uiDir, std::string& error) {
    if (!g_isoPath.empty()) {
        if (!ensureEngine(gameDir, error)) return false;
        if (!extractIso(g_isoPath, gameDir, error)) return false;
        if (!writeReadyMarker(gameDir, uiDir)) { error = "verification failed; ready marker was not created"; return false; }
        cleanupDevFiles(gameDir);
        return true;
    }
    if (src.empty()) {
        if (!writeReadyMarker(gameDir, uiDir)) { error = "game files are incomplete"; return false; }
        return true;
    }
    if (!exists(src)) { error = "install source folder not found"; return false; }
    std::error_code dirError;
    std::filesystem::create_directories(gameDir, dirError);
    if (dirError) { error = "cannot create game folder: " + dirError.message(); return false; }
    int files = 0;
    const std::wstring filesToCopy[] = { L"mcla.exe", L"mcla.toml" };
    for (const auto& f : filesToCopy) {
        if (exists(src + L"\\" + f) && !CopyFileW((src + L"\\" + f).c_str(), (gameDir + L"\\" + f).c_str(), FALSE)) {
            error = "copy failed: " + narrow(f); return false;
        }
        ++files;
    }
    if (exists(src + L"\\cache\\mcla.toml")) {
        CreateDirectoryW((gameDir + L"\\cache").c_str(), nullptr);
        if (!CopyFileW((src + L"\\cache\\mcla.toml").c_str(), (gameDir + L"\\cache\\mcla.toml").c_str(), FALSE)) { error = "copy failed: cache/mcla.toml"; return false; }
        ++files;
    }
    const std::wstring trees[] = { L"content", L"game_data", L"launcher\\ui" };
    const std::wstring dest[] = { gameDir + L"\\content", gameDir + L"\\game_data", uiDir };
    for (size_t i = 0; i < 3; ++i) {
        const std::wstring source = src + L"\\" + trees[i];
        if (!exists(source)) continue;
        std::filesystem::create_directories(dest[i], dirError);
        if (dirError) { error = "cannot create install folder: " + dirError.message(); return false; }
        if (!copyTree(source, dest[i], error, files)) return false;
    }
    eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.setInstallProgress){window.MCLALauncherAPI.setInstallProgress(100,'verify')}");
    if (!writeReadyMarker(gameDir, uiDir)) { error = "verification failed; ready marker was not created"; return false; }
    cleanupDevFiles(gameDir);
    return true;
}

static void writeIniEnv(const std::wstring& iniPath, const std::vector<std::pair<std::string, std::string>>& envSets) {
    std::vector<std::string> lines;
    if (exists(iniPath)) {
        std::ifstream in(iniPath);
        std::string l;
        while (std::getline(in, l)) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
            lines.push_back(l);
        }
    }
    int envStart = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string s = lines[i];
        size_t non = s.find_first_not_of(" \t");
        if (non != std::string::npos && _stricmp(s.substr(non).c_str(), "[env]") == 0) {
            if (envStart < 0) envStart = (int)i;
            else { lines.erase(lines.begin() + i); --i; }
        }
    }
    if (envStart < 0) {
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back("[env]");
        envStart = (int)lines.size() - 1;
    }
    size_t envEnd = lines.size();
    for (size_t i = envStart + 1; i < lines.size(); ++i) {
        std::string s = lines[i];
        size_t non = s.find_first_not_of(" \t");
        if (non != std::string::npos && s[non] == '[') { envEnd = i; break; }
    }
    for (const auto& kv : envSets) {
        bool found = false;
        for (size_t i = envStart + 1; i < envEnd; ++i) {
            std::string s = lines[i];
            size_t non = s.find_first_not_of(" \t");
            if (non == std::string::npos || s[non] == ';' || s[non] == '#') continue;
            size_t eq = s.find('=', non);
            if (eq == std::string::npos) continue;
            std::string k = s.substr(non, eq - non);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            if (_stricmp(k.c_str(), kv.first.c_str()) == 0) {
                lines[i] = kv.first + "=" + kv.second;
                found = true;
                break;
            }
        }
        if (!found) {
            lines.insert(lines.begin() + envEnd, kv.first + "=" + kv.second);
            envEnd++;
        }
    }
    std::string iniBuf;
    for (const auto& line : lines) { iniBuf += line; iniBuf += "\r\n"; }
    writeFileAtomic(iniPath, iniBuf);
}

static void writeToml(const JVal& root, const std::wstring& ini, const std::wstring& tomlPath) {
    if (root.type != JVal::OBJ) return;
    std::vector<std::pair<std::string, std::string>> sets, envSets;
    for (auto& sec : root.obj) {
        if (sec.second.type != JVal::OBJ) continue;
        for (auto& kv : sec.second.obj) {
            if (kv.second.type == JVal::OBJ || kv.second.type == JVal::ARR) continue;
            std::string val = kv.second.s;
            bool isStr = kv.second.type == JVal::STR;
            if (val == "true") val = "1";
            else if (val == "false") val = "0";
            if (kv.first == "mcla_adaptive_triggers") continue;
            if (val == "null" || val.empty()) {
                static const std::map<std::string, std::string> defs = {
                    {"mcla_left_stick_deadzone","24"},{"mcla_right_stick_deadzone","24"},
                    {"mcla_script_pad_latch_ms","100"},{"mcla_dt_max","0.125"},
                    {"mcla_poll_spin_us","40"},{"mcla_poll_tail_us","400"},
                    {"mcla_pump_wake_pause_s","60"},{"mcla_pump_sleep_max_ms","-1"},
                    {"mcla_lod_scale","1"},{"MCLA_LOD_SCALE","1"},{"MCLA_FLOW_LOD","1"}
                };
                auto it = defs.find(kv.first);
                if (it == defs.end()) continue;
                val = it->second; isStr = false;
            }
            if (!okKey(kv.first) || badVal(val)) continue;

            if (sec.first == "_env") {
                std::string k = kv.first;
                if (k == "resolution_scale") k = "MCLA_RENDER_SCALE";
                else if (k == "vsync") k = "MCLA_VSYNC";
                if (kGeneratedEnvOk.count(k)) envSets.push_back({ k, val });
            } else {
                if (kv.first == "mcla_hud_scale") envSets.push_back({ "MCLA_HUD_SCALE", val });
                if (kv.first == "mcla_ambient_occlusion") envSets.push_back({ "MCLA_AMBIENT_OCCLUSION", val });
                else if (kv.first == "mcla_shadows") envSets.push_back({ "MCLA_SHADOWS", val });
                else if (kv.first == "resolution_scale") envSets.push_back({ "MCLA_RENDER_SCALE", val });
                else if (kv.first == "vsync") envSets.push_back({ "MCLA_VSYNC", (val == "true" || val == "1") ? "1" : "0" });
                if (kv.first == "mcla_fps") {
                    std::string fval = (val == "-1" || val == "uncapped") ? "0" : val;
                    envSets.push_back({ "MCLA_FPS_CAP", fval });
                }
                if (kv.first == "display_mode") {
                    envSets.push_back({ "MCLA_DISPLAY_MODE", val });
                    sets.push_back({ "fullscreen", std::string("fullscreen = ") + (val == "windowed" ? "0" : "1") });
                }
                if (kv.first == "fullscreen") envSets.push_back({ "MCLA_DISPLAY_MODE", val == "1" ? "borderless_fullscreen" : "windowed" });
                if (kv.first == "mcla_aspect_ratio") envSets.push_back({ "MCLA_ASPECT_RATIO", val });
            }

            if (kv.first == "resolution_preset") {
                WritePrivateProfileStringW(L"launcher", L"res", widen(val).c_str(), ini.c_str());
                continue;
            }
            std::string tomlKey = kv.first;
            if (tomlKey.rfind("MCLA_", 0) == 0 && tomlKey != "MCLA_SMALL_FS_SWAP") {
                continue;
            }
            sets.push_back({ tomlKey, tomlKey + " = " + (isStr ? tomlQuote(val) : val) });
        }
    }

    if (!envSets.empty()) writeIniEnv(ini, envSets);

    if (sets.empty()) return;
    std::ifstream in(tomlPath);
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(in, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
        lines.push_back(l);
    }
    in.close();

    size_t hdr = 0;
    while (hdr < lines.size()) {
        size_t s = lines[hdr].find_first_not_of(" \t");
        if (s != std::string::npos && lines[hdr][s] == '[') break;
        hdr++;
    }
    for (auto& kv : sets) {
        bool found = false;
        for (size_t n = 0; n < hdr; ++n) {
            if (tomlKeyOf(lines[n]) == kv.first) {
                lines[n] = kv.second;
                found = true;
                break;
            }
        }
        if (!found) {
            lines.insert(lines.begin() + hdr, kv.second);
            hdr++;
        }
    }
    std::string tomlBuf;
    for (auto& line : lines) { tomlBuf += line; tomlBuf += "\r\n"; }
    writeFileAtomic(tomlPath, tomlBuf);
}

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

    std::wstring gdir = std::wstring(tmp) + L"mcla_st_game", cdir = gdir + L"\\cache";
    CreateDirectoryW(gdir.c_str(), nullptr); CreateDirectoryW(cdir.c_str(), nullptr);
    { std::ofstream o(gdir + L"\\mcla.toml", std::ios::binary); o << "mcla_fps = 90\r\nresolution_scale = 1\r\nvsync = false\r\n"; }
    { std::ofstream o(cdir + L"\\mcla.toml", std::ios::binary); o << "mcla_fps = 120\r\nmcla_aspect_ratio = \"21:9\"\r\n"; }
    std::string rb = tomlToJson(gdir, cdir, L"");
    DeleteFileW((cdir + L"\\mcla.toml").c_str()); DeleteFileW((gdir + L"\\mcla.toml").c_str());
    RemoveDirectoryW(cdir.c_str()); RemoveDirectoryW(gdir.c_str());
    if(rb.find("\"mcla_fps\":120")==std::string::npos) return 15;
    if(rb.find("\"resolution_scale\":1")==std::string::npos) return 16;
    if(rb.find("\"vsync\":false")==std::string::npos) return 17;
    if(rb.find("\"mcla_aspect_ratio\":\"21:9\"")==std::string::npos) return 18;

    std::wstring rtIni = std::wstring(tmp) + L"mcla_rt.ini", rtDir = std::wstring(tmp) + L"mcla_rt_cache";
    CreateDirectoryW(rtDir.c_str(), nullptr);
    DeleteFileW(rtIni.c_str()); DeleteFileW((rtDir + L"\\mcla.toml").c_str());
    const std::string rtJs = R"({"display":{"display_mode":"borderless_fullscreen","resolution_scale":2,"vsync":false,"mcla_aspect_ratio":"32:9","mcla_fps":120},"renderer":{"mcla_shadows":true,"mcla_ambient_occlusion":false},"input":{"mcla_key_throttle":"W","mcla_key_rt":"W"},"profile":{"mcla_profile_name":"Racer"},"_env":{"resolution_preset":"3440x1440","MCLA_WINDOW_W":"3440","MCLA_WINDOW_H":"1440","MCLA_HUD_SCALE":1.25,"MCLA_HDR":1}})";
    { size_t p = 0; JVal r = jparse(rtJs, p); writeToml(r, rtIni, rtDir + L"\\mcla.toml"); }
    std::string rtOut = tomlToJson(L"", rtDir, rtIni);
    DeleteFileW(rtIni.c_str()); DeleteFileW((rtDir + L"\\mcla.toml").c_str()); RemoveDirectoryW(rtDir.c_str());
    if(rtOut.find("\"display_mode\":\"borderless_fullscreen\"")==std::string::npos) return 19;
    if(rtOut.find("\"resolution_scale\":2")==std::string::npos) return 20;
    if(rtOut.find("\"vsync\":0")==std::string::npos) return 21;
    if(rtOut.find("\"mcla_aspect_ratio\":\"32:9\"")==std::string::npos) return 22;
    if(rtOut.find("\"mcla_fps\":120")==std::string::npos) return 23;
    if(rtOut.find("\"mcla_shadows\":1")==std::string::npos || rtOut.find("\"mcla_ambient_occlusion\":0")==std::string::npos) return 24;
    if(rtOut.find("\"mcla_key_throttle\":\"W\"")==std::string::npos || rtOut.find("\"mcla_key_rt\":\"W\"")==std::string::npos) return 25;
    if(rtOut.find("\"mcla_profile_name\":\"Racer\"")==std::string::npos) return 26;
    if(rtOut.find("\"resolution_preset\":\"3440x1440\"")==std::string::npos) return 27;
    if(rtOut.find("\"MCLA_HUD_SCALE\":1.25")==std::string::npos || rtOut.find("\"MCLA_HDR\":1")==std::string::npos) return 28;

    std::wstring mdir = std::wstring(tmp) + L"mcla_st_mods";
    CreateDirectoryW(mdir.c_str(), nullptr);
    CreateDirectoryW((mdir + L"\\alpha_mod").c_str(), nullptr);
    CreateDirectoryW((mdir + L"\\beta_mod").c_str(), nullptr);
    writeFileAtomic(mdir + L"\\modlist.txt", "#alpha_mod\r\nbeta_mod\r\n");
    std::string mj = slurp(mdir + L"\\modlist.txt");
    DeleteFileW((mdir + L"\\modlist.txt").c_str());
    RemoveDirectoryW((mdir + L"\\alpha_mod").c_str());
    RemoveDirectoryW((mdir + L"\\beta_mod").c_str());
    RemoveDirectoryW(mdir.c_str());
    if(mj != "#alpha_mod\r\nbeta_mod\r\n") return 29;
    if(!kGeneratedEnvOk.count("MCLA_HDR") || !kGeneratedChildEnvAllowlist.count("MCLA_NO_CMDBUF_CACHE")) return 30;

    std::wstring idir = std::wstring(tmp) + L"mcla_st_install";
    std::error_code ec;
    std::filesystem::remove_all(idir, ec);
    std::wstring sg = idir + L"\\game", ss = idir + L"\\src", sui = sg + L"\\launcher\\ui";
    std::filesystem::create_directories(ss + L"\\launcher\\ui", ec);
    CreateDirectoryW((ss + L"\\cache").c_str(), nullptr);
    CreateDirectoryW((ss + L"\\content").c_str(), nullptr);
    CreateDirectoryW((ss + L"\\game_data").c_str(), nullptr);
    writeFileAtomic(ss + L"\\mcla.exe", "mock-exe");
    writeFileAtomic(ss + L"\\mcla.toml", "mcla_fps = 60\r\n");
    writeFileAtomic(ss + L"\\cache\\mcla.toml", "resolution_scale = 1\r\n");
    writeFileAtomic(ss + L"\\launcher\\ui\\index.html", "<html>mock</html>");
    writeFileAtomic(ss + L"\\content\\a.bin", "content");
    writeFileAtomic(ss + L"\\game_data\\b.bin", "data");
    if (checkInstall(sg, sui).markerValid) return 31;
    std::string ierr;
    if (!runInstall(ss, sg, sui, ierr)) return 32;
    if (!checkInstall(sg, sui).markerValid) return 33;
    if (slurp(sg + kReadyRel).find(std::string("\"version\":\"") + kInstallVersion + "\"") == std::string::npos) return 34;
    if (slurp(sui + L"\\index.html") != "<html>mock</html>") return 36;
    if (!writeFileAtomic(sg + L"\\cache\\mcla.toml", "resolution_scale = 2\r\n") ||
        !checkInstall(sg, sui).markerValid) return 37;
    if (!CreateDirectoryW((sg + L"\\game_data\\mods").c_str(), nullptr) ||
        !writeFileAtomic(sg + L"\\game_data\\mods\\modlist.txt", "example\r\n") ||
        !checkInstall(sg, sui).markerValid) return 38;
    writeFileAtomic(sg + L"\\mcla.exe", "tampered");
    if (checkInstall(sg, sui).markerValid) return 35;
    std::filesystem::remove_all(idir, ec);
    return 0;
}

static std::string g_cvarJson;
static void dumpCVars() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring out = std::wstring(tmp) + L"mcla_cvars_" + std::to_wstring(GetCurrentProcessId()) + L".json";
    DeleteFileW(out.c_str());
    SetEnvironmentVariableW(L"MCLA_DUMP_CVARS", out.c_str());
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + g_mclaExe + L"\"";
    const BOOL started = CreateProcessW(g_mclaExe.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, g_gameDir.c_str(), &si, &pi);
    SetEnvironmentVariableW(L"MCLA_DUMP_CVARS", nullptr);
    if (!started) return;
    const bool exited = WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0;
    if (!exited) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    std::string js = exited && code == 0 ? slurp(out) : std::string();
    DeleteFileW(out.c_str());
    const size_t a = js.find_first_not_of(" \t\r\n"), b = js.find_last_not_of(" \t\r\n");
    if (a != std::string::npos && js[a] == '[' && js[b] == ']') g_cvarJson = js.substr(a, b - a + 1);
}

static void eval(const std::string& js) {
    if (g_web) g_web->ExecuteScript(widen(js).c_str(), nullptr);
}

static std::wstring resolveModsDir(const std::wstring& gameDir = g_gameDir) {
    const std::wstring gd = gameDir + L"\\game_data";
    const DWORD attr = GetFileAttributesW(gd.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return gd + L"\\mods";
    }
    return gameDir + L"\\mods";
}

static std::string listModsJson(const std::wstring& gameDir = g_gameDir) {
    const std::wstring modsDir = resolveModsDir(gameDir);
    std::vector<std::string> diskMods;
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((modsDir + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            diskMods.push_back(narrow(fd.cFileName));
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    std::sort(diskMods.begin(), diskMods.end());

    struct ModEntry { std::string name; bool enabled; };
    std::vector<ModEntry> ordered;
    std::set<std::string> seen;
    std::set<std::string> diskSet(diskMods.begin(), diskMods.end());

    std::ifstream ml(modsDir + L"\\modlist.txt");
    if (ml.good()) {
        std::string line;
        while (std::getline(ml, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t s = line.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            line = line.substr(s);
            bool enabled = true;
            if (line[0] == '#') {
                enabled = false;
                line.erase(0, 1);
                size_t s2 = line.find_first_not_of(" \t");
                if (s2 == std::string::npos) continue;
                line = line.substr(s2);
            }
            if (!diskSet.count(line) || seen.count(line)) continue;
            seen.insert(line);
            ordered.push_back({ line, enabled });
        }
    }
    for (const auto& m : diskMods) {
        if (!seen.count(m)) ordered.push_back({ m, true });
    }

    std::string out = "{\"modsDir\":\"" + jesc(narrow(modsDir)) + "\",\"mods\":[";
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (i > 0) out += ",";
        out += "{\"name\":\"" + jesc(ordered[i].name) + "\",\"enabled\":" + (ordered[i].enabled ? "true" : "false") + "}";
    }
    out += "]}";
    return out;
}

static bool saveModlistFromJson(const JVal& modsArr, const std::wstring& gameDir = g_gameDir) {
    if (modsArr.type != JVal::ARR) return false;
    const std::wstring modsDir = resolveModsDir(gameDir);
    CreateDirectoryW(modsDir.c_str(), nullptr);
    std::string buf;
    for (const auto& item : modsArr.obj) {
        if (item.second.type != JVal::OBJ) continue;
        const JVal* n = jget(item.second, "name");
        const JVal* e = jget(item.second, "enabled");
        if (!n || n->type != JVal::STR || n->s.empty()) continue;
        if (n->s.find('/') != std::string::npos || n->s.find('\\') != std::string::npos || n->s == "..") continue;
        const bool enabled = !(e && (e->s == "false" || e->s == "0"));
        if (!enabled) buf += "#";
        buf += n->s + "\r\n";
    }
    return writeFileAtomic(modsDir + L"\\modlist.txt", buf);
}

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
    GetPrivateProfileStringW(L"launcher", L"res", L"", buf, 256, g_ini.c_str());
    std::wstring res = buf;
    size_t x = res.find(L'x');
    if (x != std::wstring::npos) {
        int w = _wtoi(res.substr(0, x).c_str()), h = _wtoi(res.substr(x + 1).c_str());
        if (w >= 640 && w <= 7680 && h >= 480 && h <= 4320) {
            setEnv(L"MCLA_WINDOW_W=" + std::to_wstring(w));
            setEnv(L"MCLA_WINDOW_H=" + std::to_wstring(h));
        }
    }

    if (exists(g_ini)) {
        std::vector<wchar_t> sec(32768);
        GetPrivateProfileSectionW(L"env", sec.data(), (DWORD)sec.size(), g_ini.c_str());
        for (wchar_t* p = sec.data(); *p; p += wcslen(p) + 1) {
            std::wstring e = p;
            size_t c = e.find(L" ;"); if (c != std::wstring::npos) e.resize(c);
            while (!e.empty() && (e.back() == L' ' || e.back() == L'\r')) e.pop_back();
            if (e.empty() || e[0] == L';') continue;
            if (e.find(L'\n') != std::wstring::npos || e.find(L'\r') != std::wstring::npos) continue;
            size_t eq = e.find(L'='); if (eq == std::wstring::npos || eq == 0) continue;
            std::string k = narrow(e.substr(0, eq));
            std::string v = narrow(e.substr(eq + 1));
            if (!kGeneratedChildEnvAllowlist.count(k)) continue;
            if (v.find('\0') != std::string::npos || v.find('\r') != std::string::npos || v.find('\n') != std::string::npos) continue;
            setEnv(e);
        }
    }

    std::wstring block;
    for (auto& e : kv) { block += e; block += L'\0'; }
    block += L'\0';
    return block;
}

static bool hasGame(const std::wstring& d) { return !d.empty() && exists(d + L"\\mcla.exe"); }

static bool gameRunning() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) }; bool r = false;
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
        if (_wcsicmp(pe.szExeFile, L"mcla.exe") == 0) { r = true; break; }
    CloseHandle(s); return r;
}
static void warmCaches(const std::wstring& gameDir) {
    const std::wstring exe = gameDir + L"\\mcla.exe";
    if (!exists(exe) || gameRunning()) return;
    SetEnvironmentVariableW(L"MCLA_PSO_PREWARM", L"1");
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    bool started = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, gameDir.c_str(), &si, &pi) != FALSE;
    SetEnvironmentVariableW(L"MCLA_PSO_PREWARM", nullptr);
    if (started) {
        if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    cleanupDevFiles(gameDir);
}

static void launchGame() {
    InstallCheck c = checkInstall(g_gameDir, g_uiDir);
    if (!c.markerValid) {
        eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadInstallStatus){window.MCLALauncherAPI.loadInstallStatus(" + installStatusJson(g_gameDir, g_uiDir) + ")}");
        eval("if(typeof switchTab==='function'){switchTab('main','next')}");
        return;
    }
    if (!exists(g_mclaExe)) { MessageBoxW(g_hwnd, (L"mcla.exe not found:\n"+g_mclaExe).c_str(), L"MCLA Launcher", MB_ICONERROR); return; }
    if (gameRunning()) { MessageBoxW(g_hwnd, L"mcla.exe is already running.", L"MCLA Launcher", MB_ICONINFORMATION); return; }
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + g_mclaExe + L"\"";
    std::wstring env = buildChildEnv();
    wchar_t dbg[8] = {};
    const bool debugConsole = GetEnvironmentVariableW(L"MCLA_DEBUG_CONSOLE", dbg, 8) > 0 && dbg[0] == L'1';
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | (debugConsole ? 0 : CREATE_NO_WINDOW);
    if (CreateProcessW(g_mclaExe.c_str(), &cmd[0], nullptr, nullptr, FALSE, flags, (void*)env.c_str(), g_gameDir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else MessageBoxW(g_hwnd, L"Cannot start mcla.exe", L"MCLA Launcher", MB_ICONERROR);
}
static void openFolder(const std::wstring& p) {
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void cleanupDevFiles(const std::wstring& gameDir) {
    std::wstring pattern = gameDir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            std::wstring ext = name.substr(dot);
            const bool devDump = name.find(L"_toc") != std::wstring::npos || name.find(L"_parsed") != std::wstring::npos ||
                name.find(L"crash_") == 0 || name.find(L"dump_") == 0;
            if (_wcsicmp(ext.c_str(), L".log") == 0 || _wcsicmp(ext.c_str(), L".pdb") == 0 ||
                _wcsicmp(ext.c_str(), L".dmp") == 0 || _wcsicmp(ext.c_str(), L".tmp") == 0 ||
                _wcsicmp(ext.c_str(), L".download") == 0 || devDump)
                DeleteFileW((gameDir + L"\\" + name).c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static bool writeSettingsJson(const std::string& json) {
    size_t p = 0;
    JVal r = jparse(json, p);
    if (r.type != JVal::OBJ) return false;
    writeToml(r, g_ini, g_cacheDir + L"\\mcla.toml");
    return true;
}

static void applyGraphicsPreset(const std::string& preset) {
    std::string shadows = "true", ao = "true", foliage = "true", lod = "1", blur = "true";
    std::string cacheSoft = "3072", cacheHard = "3584";
    if (preset == "LOW") {
        shadows = "false"; ao = "false"; lod = "0.75"; blur = "false";
        cacheSoft = "1024"; cacheHard = "1280";
    } else if (preset == "MEDIUM") {
        ao = "false"; blur = "false"; cacheSoft = "2048"; cacheHard = "2560";
    }
    writeSettingsJson("{\"renderer\":{\"mcla_shadows\":" + shadows + ",\"mcla_ambient_occlusion\":" + ao +
        ",\"mcla_fullscreen_blur\":" + blur + "},\"graphics\":{\"mcla_lod_scale\":" + lod +
        ",\"mcla_foliage_impostors\":" + foliage + "},\"_env\":{\"MCLA_TEXTURE_CACHE_SOFT_MB\":" +
        cacheSoft + ",\"MCLA_TEXTURE_CACHE_HARD_MB\":" + cacheHard + "}}");
}

static void applyControlPreset(const std::string& preset) {
    std::string t = "W", b = "S", l = "A", r = "D", prompts = "pc";
    if (preset == "XBOX") { t = "W"; b = "S"; l = "A"; r = "D"; prompts = "xbox"; }
    if (preset == "CUSTOM") return;
    writeSettingsJson("{\"input\":{\"mcla_button_prompts\":\"" + prompts + "\",\"mcla_key_throttle\":\"" + t +
        "\",\"mcla_key_brake\":\"" + b + "\",\"mcla_key_steer_left\":\"" + l +
        "\",\"mcla_key_steer_right\":\"" + r + "\"}}");
}

static void resetKeymap() {
    writeSettingsJson("{\"input\":{\"mcla_key_throttle\":\"W\",\"mcla_key_brake\":\"S\",\"mcla_key_steer_left\":\"A\",\"mcla_key_steer_right\":\"D\",\"mcla_button_prompts\":\"auto\"}}");
}

static bool backupConfig() {
    const std::wstring src = g_cacheDir + L"\\mcla.toml";
    if (!exists(src)) return false;
    return CopyFileW(src.c_str(), (src + L".bak").c_str(), FALSE) != FALSE;
}

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
        const JVal* pl = jget(root, "payload");
        const JVal* ex = jget(root, "extra");
        if (!ex && pl) ex = jget(*pl, "extra");
        const JVal* tm = ex ? jget(*ex, "toml") : nullptr;
        JVal none; const JVal& extraToml = tm ? *tm : none;

        const bool settingsWrite = action == "optionChanged" || action == "saveConfig" || action == "restoreDefaults" ||
            action == "applyGraphicsPreset" || action == "applyControlPreset" || action == "resetKeymap" ||
            action == "saveModlist" || action == "launchGame";
        if (settingsWrite && !checkInstall(g_gameDir, g_uiDir).markerValid) { eval("if(typeof mclaBadInput==='function'){mclaBadInput('locked-row')}"); return S_OK; }

        if (action == "ready") {
            wchar_t lang[16] = {};
            GetPrivateProfileStringW(L"launcher", L"installLang", L"en", lang, 16, g_ini.c_str());
            eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.setInstallLanguage){window.MCLALauncherAPI.setInstallLanguage('" + normalizeLang(narrow(lang)) + "')}");
            eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.setInstallSource){window.MCLALauncherAPI.setInstallSource('" + jesc(narrow(g_installSource)) + "')}");
            if (!g_installMode) adoptExistingInstall(g_gameDir, g_uiDir);
            const InstallCheck install = checkInstall(g_gameDir, g_uiDir);
            if (install.markerValid && install.markerNeedsRefresh) writeReadyMarker(g_gameDir, g_uiDir);
            eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadInstallStatus){window.MCLALauncherAPI.loadInstallStatus(" + installStatusJson(g_gameDir, g_uiDir) + ")}");
            wchar_t cancelled[8] = {};
            GetPrivateProfileStringW(L"launcher", L"installCancelled", L"", cancelled, 8, g_ini.c_str());
            (void)cancelled;
            const bool installed = install.markerValid;
            if (installed && !g_cvarJson.empty()) eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadCVars){window.MCLALauncherAPI.loadCVars(" + g_cvarJson + ");}");
            if (installed) eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadConfig){window.MCLALauncherAPI.loadConfig(" + tomlToJson() + ");}");
            if (installed) eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadMods){window.MCLALauncherAPI.loadMods(" + listModsJson() + ");}");
        } else if (action == "getInstallStatus") {
            eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadInstallStatus){window.MCLALauncherAPI.loadInstallStatus(" + installStatusJson(g_gameDir, g_uiDir) + ")}");
        } else if (action == "setInstallLanguage") {
            const JVal* lv = jget(root, "language");
            std::string lv2 = (lv && lv->type == JVal::STR) ? lv->s : "en";
            lv2 = normalizeLang(lv2);
            WritePrivateProfileStringW(L"launcher", L"installLang", widen(lv2).c_str(), g_ini.c_str());
        } else if (action == "chooseInstallSource") {
            BROWSEINFOW bi = {};
            bi.hwndOwner = g_hwnd;
            bi.lpszTitle = L"Select the folder that contains the game files";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t pth[MAX_PATH];
                BOOL ok = SHGetPathFromIDListW(pidl, pth);
                CoTaskMemFree(pidl);
                if (ok) {
                    g_installSource = pth;
                    WritePrivateProfileStringW(L"launcher", L"installSource", pth, g_ini.c_str());
                    eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.setInstallSource){window.MCLALauncherAPI.setInstallSource('" + jesc(narrow(pth)) + "')}");
                }
            }
        } else if (action == "chooseIso") {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_hwnd;
            ofn.lpstrFilter = L"Xbox 360 ISO (*.iso)\0*.iso\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            ofn.lpstrTitle = L"Select the game ISO";
            if (GetOpenFileNameW(&ofn)) {
                HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                char magic[8] = {};
                bool valid = false;
                if (h != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER off{}; off.QuadPart = 0x8001;
                    if (SetFilePointerEx(h, off, nullptr, FILE_BEGIN)) {
                        DWORD got = 0; ReadFile(h, magic, 5, &got, nullptr);
                        valid = (got == 5 && memcmp(magic, "CD001", 5) == 0);
                    }
                    CloseHandle(h);
                }
                if (valid) {
                    g_isoPath = file;
                    WritePrivateProfileStringW(L"launcher", L"isoPath", file, g_ini.c_str());
                    eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.setInstallSource){window.MCLALauncherAPI.setInstallSource('" + jesc(narrow(file)) + "')}");
                } else {
                    eval("if(typeof mclaBadInput==='function'){mclaBadInput('bad-iso')}");
                }
            }
        } else if (action == "runInstall") {
            const JVal* sv = jget(root, "source");
            if (sv && sv->type == JVal::STR && !sv->s.empty()) { g_installSource = widen(sv->s); WritePrivateProfileStringW(L"launcher", L"installSource", g_installSource.c_str(), g_ini.c_str()); }
            std::string err;
            const bool ok = runInstall(g_installSource, g_gameDir, g_uiDir, err);
            eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.installFinished){window.MCLALauncherAPI.installFinished(" + std::string(ok ? "true" : "false") + ",'" + jesc(err) + "')}");
            eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadInstallStatus){window.MCLALauncherAPI.loadInstallStatus(" + installStatusJson(g_gameDir, g_uiDir) + ")}");
        } else if (action == "cancelInstall") {
            WritePrivateProfileStringW(L"launcher", L"installCancelled", L"1", g_ini.c_str());
        } else if (action == "logError") {
            const JVal* kv = jget(root, "kind");
            const JVal* dv = jget(root, "detail");
            std::string kind = (kv && kv->type == JVal::STR) ? kv->s : "unknown";
            std::string detail = (dv && dv->type == JVal::STR) ? dv->s : "";
            char stamp[32]; time_t now = time(nullptr);
            strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
            std::string line = std::string(stamp) + "\t" + kind + "\t" + jesc(detail) + "\r\n";
            HANDLE h = CreateFileW((g_dir + L"\\launcher_errors.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) { DWORD n = 0; WriteFile(h, line.data(), (DWORD)line.size(), &n, nullptr); CloseHandle(h); }
        } else if (action == "backupConfig") {
            eval("if(typeof showToast==='function'){showToast('BACKUP " + std::string(backupConfig() ? "SAVED" : "FAILED") + "')}");
        } else if (action == "restoreConfig") {
            const std::wstring bak = g_cacheDir + L"\\mcla.toml.bak";
            const bool ok = exists(bak) && CopyFileW(bak.c_str(), (g_cacheDir + L"\\mcla.toml").c_str(), FALSE) != FALSE;
            eval("if(typeof showToast==='function'){showToast('RESTORE " + std::string(ok ? "DONE" : "FAILED") + "')}");
            if (!ok) eval("if(typeof mclaBadInput==='function'){mclaBadInput('backup-failed')}");
        } else if (action == "checkUpdate") {
            ShellExecuteW(nullptr, L"open", L"https://github.com/3bdull4h2008/mcla-launcher/releases", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (action == "resetKeymap") {
            resetKeymap();
            eval("if(typeof showToast==='function'){showToast('KEYMAP RESET')}");
        } else if (action == "applyGraphicsPreset" || action == "applyControlPreset") {
            const JVal* pv = pl ? jget(*pl, "preset") : nullptr;
            if (!pv || pv->type != JVal::STR) pv = jget(root, "preset");
            const std::string preset = (pv && pv->type == JVal::STR) ? pv->s : "";
            if (action == "applyGraphicsPreset") applyGraphicsPreset(preset); else applyControlPreset(preset);
        } else if (action == "dragWindow") {
            ReleaseCapture(); SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } else if (action == "launchGame") {
            writeToml(extraToml, g_ini, g_cacheDir + L"\\mcla.toml"); launchGame();
        } else if (action == "saveConfig" || action == "optionChanged" || action == "restoreDefaults") {
            if (tm) writeToml(extraToml, g_ini, g_cacheDir + L"\\mcla.toml");
        } else if (action == "listMods") {
            eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadMods){window.MCLALauncherAPI.loadMods(" + listModsJson() + ");}");
        } else if (action == "saveModlist") {
            const JVal* mods = jget(root, "mods");
            if (!mods && pl) mods = jget(*pl, "mods");
            if (mods) saveModlistFromJson(*mods);
        } else if (action == "openModsFolder") {
            const std::wstring md = resolveModsDir();
            CreateDirectoryW(md.c_str(), nullptr);
            openFolder(md);
        } else if (action == "browseFiles") {
            openFolder(g_gameDir);
        } else if (action == "changeGameDir") {
            BROWSEINFOW bi = {};
            bi.hwndOwner = g_hwnd;
            bi.lpszTitle = L"Select the folder that contains mcla.exe";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t pth[MAX_PATH];
                BOOL ok = SHGetPathFromIDListW(pidl, pth);
                CoTaskMemFree(pidl);
                if (ok && hasGame(pth)) {
                    wchar_t full[MAX_PATH];
                    GetFullPathNameW(pth, MAX_PATH, full, nullptr);
                    g_gameDir = full;
                    g_mclaExe = g_gameDir + L"\\mcla.exe";
                    g_cacheDir = g_gameDir + L"\\cache";
                    CreateDirectoryW(g_cacheDir.c_str(), nullptr);
                    WritePrivateProfileStringW(L"launcher", L"gameDir", full, g_ini.c_str());
                    adoptExistingInstall(g_gameDir, g_uiDir);
                    eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadInstallStatus){window.MCLALauncherAPI.loadInstallStatus(" + installStatusJson(g_gameDir, g_uiDir) + ")}");
                    eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadConfig){window.MCLALauncherAPI.loadConfig(" + tomlToJson() + ");}");
                    eval("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadMods){window.MCLALauncherAPI.loadMods(" + listModsJson() + ");}");
                    eval("if(window.showToast)window.showToast('GAME FOLDER UPDATED');");
                } else {
                    MessageBoxW(g_hwnd, L"mcla.exe was not found in the selected folder.", L"MCLA Launcher", MB_ICONWARNING);
                }
            }
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
        g_controller->put_IsVisible(TRUE);
        g_controller->get_CoreWebView2(&g_web);
        if (!g_web) { wv2Fail(E_FAIL); return S_OK; }

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

        MsgHandler* mh = new MsgHandler(); g_web->add_WebMessageReceived(mh, nullptr); mh->Release();
        NavHandler* nh = new NavHandler(); g_web->add_NavigationCompleted(nh, nullptr); nh->Release();

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
        g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return S_OK;
    }
};

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

static bool resolveGameDir() {
    wchar_t buf[MAX_PATH] = {}, src[MAX_PATH] = {}, iso[MAX_PATH] = {}, lang[16] = {};
    GetPrivateProfileStringW(L"launcher", L"gameDir", L"", buf, MAX_PATH, g_ini.c_str());
    GetPrivateProfileStringW(L"launcher", L"installSource", L"", src, MAX_PATH, g_ini.c_str());
    GetPrivateProfileStringW(L"launcher", L"isoPath", L"", iso, MAX_PATH, g_ini.c_str());
    GetPrivateProfileStringW(L"launcher", L"installLang", L"en", lang, 16, g_ini.c_str());
    g_installSource = src;
    g_isoPath = iso;
    std::wstring cand[] = {
        buf,
        g_dir,
        dirOf(g_dir) + L"\\..\\mcla pc\\build",
        g_dir + L"\\..\\..\\mcla pc\\build",
        dirOf(g_dir) + L"\\build",
        dirOf(g_dir)
    };
    for (auto& c : cand) {
        if (!c.empty() && hasGame(c)) {
            wchar_t full[MAX_PATH];
            GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr);
            g_gameDir = full;
            WritePrivateProfileStringW(L"launcher", L"gameDir", full, g_ini.c_str());
            return true;
        }
    }
    g_installMode = true;
    g_gameDir = buf[0] ? buf : g_dir;
    return true;
}

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

    if (wcsstr(GetCommandLineW(), L"--selftest")) return selfTest();
    if (wcsstr(GetCommandLineW(), L"--extract-test")) {
        const wchar_t* iso = _wgetenv(L"MCLA_TEST_ISO");
        const wchar_t* dst = _wgetenv(L"MCLA_TEST_DEST");
        if (!iso || !dst) return 2;
        std::string err;
        bool ok = extractIso(iso, dst, err);
        if (!ok) {
            wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
            std::ofstream o(std::wstring(tmp) + L"mcla_extract_err.txt", std::ios::binary);
            o << err;
            fprintf(stderr, "EXTRACT_ERROR: %s\n", err.c_str());
            return 3;
        }
        return 0;
    }
    if (wcsstr(GetCommandLineW(), L"--install-cli")) {
        const wchar_t* iso = _wgetenv(L"MCLA_ISO");
        const wchar_t* dst = _wgetenv(L"MCLA_GAME_DIR");
        if (!iso || !dst) return 2;
        g_gameDir = dst;
        g_ini = g_gameDir + L"\\mcla_launcher.ini";
        g_cacheDir = g_gameDir + L"\\cache";
        g_isoPath = iso;
        std::string err;
        if (!ensureEngine(g_gameDir, err)) { fprintf(stderr, "ENGINE: %s\n", err.c_str()); return 5; }
        if (!extractIso(iso, g_gameDir, err)) { fprintf(stderr, "ISO: %s\n", err.c_str()); return 3; }
        if (!writeReadyMarker(g_gameDir, L"")) { fprintf(stderr, "MARKER failed\n"); return 4; }
        cleanupDevFiles(g_gameDir);
        warmCaches(g_gameDir);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    g_dir = dirOf(p);
    g_ini = g_dir + L"\\mcla_launcher.ini";
    if (!resolveGameDir()) return 0;
    if (g_installMode) SetEnvironmentVariableW(L"MCLA_INSTALL_MODE", L"1");
    if (!exists(g_ini) && exists(g_gameDir + L"\\mcla_launcher.ini")) g_ini = g_gameDir + L"\\mcla_launcher.ini";
    g_mclaExe = g_gameDir + L"\\mcla.exe";
    dumpCVars();
    const std::wstring ud = ensureBundledResources(hi);
    g_uiDir = ud + L"\\ui";
    g_cacheDir = g_gameDir + L"\\cache"; CreateDirectoryW(g_cacheDir.c_str(), nullptr);

    WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"MclaLauncherWv2";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCE(101));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST)
                                  : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO target{}; target.cbSize = sizeof(target);
    POINT probe{0, 0};
    if (GetMonitorInfoW(monitor, &target)) probe = { target.rcWork.left + 8, target.rcWork.top + 8 };
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
                             L"Midnight Club: Los Angeles — Launcher", WS_POPUP,
                             probe.x, probe.y, 1, 1, nullptr, nullptr, hi, nullptr);
    if (!g_hwnd) return (int)GetLastError();
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);
    LayoutLauncher(g_hwnd, GetDpiForWindow(g_hwnd), monitor);
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);

    HMODULE loader = LoadLibraryW((ud + L"\\WebView2Loader.dll").c_str());
    if (!loader) { MessageBoxW(nullptr, L"Cannot load bundled WebView2Loader.dll", L"MCLA Launcher", MB_ICONERROR); return 1; }
    auto createEnv = (CreateWebView2EnvironmentFn)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!createEnv) { MessageBoxW(nullptr, L"WebView2 loader entry point missing", L"MCLA Launcher", MB_ICONERROR); return 1; }
    EnvHandler* eh = new EnvHandler();
    HRESULT hr = createEnv(GUID_NULL, ud.c_str(), nullptr, eh);
    eh->Release();
    if (FAILED(hr)) wv2Fail(hr);

    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_controller) g_controller->Close();
    g_web.Reset(); g_controller.Reset();
    CoUninitialize();
    return g_exit;
}
