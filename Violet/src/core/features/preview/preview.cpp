#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "preview.h"
#include "tung_model.h"
#include "imgui_internal.h"
#include "core/variables/variables.h"
#include "core/functions/visual/visual.h"
#include "core/globals/globals.h"
#include "core/cache/cache.h"
#include "memory/memory.h"
#include "sdk/offsets.h"
#include "render/menu/library.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")


#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../../../ext/stb/stb_image.h"

namespace Preview {
namespace {

struct Vert {
    float x, y, z;
    float u, v;
    float cr, cg, cb;
};

constexpr int kMaxVerts = 32768;
constexpr float kPanelW = 196.0f;
constexpr float kTitleH = 22.0f;

std::vector<Vert> g_verts;
std::vector<std::uint32_t> g_idx;
std::vector<char> g_triUV;
ID3D11ShaderResourceView* g_tex = nullptr;
int g_texW = 0, g_texH = 0;
bool g_ready = false;
const char* g_fail = "not initialized";

float g_rx = 0.0f, g_ry = 0.0f, g_rw = 0.0f, g_rh = 0.0f;
float s_yawAuto = 0.0f, s_yawDrag = 0.0f, s_pitch = 0.0f, s_zoom = 1.0f;
float g_floorY = -0.55f;
std::string g_texPathOut;
static bool g_texEmbedded = false; // true when the texture lives in the binary
static ID3D11ShaderResourceView* g_fileTex = nullptr; // stashed file-mesh texture

// Tung Tung swap: the meme log lives in its own slot so the OBJ file mesh
// is never clobbered. Entering tung mode stashes the file mesh, leaving
// restores it.
static bool g_showingTung = false;
static std::vector<Vert> g_fileVerts;
static std::vector<std::uint32_t> g_fileIdx;
static std::vector<char> g_fileUV;

// ---- Client avatar image (user_id -> fetch avatar, like the reference
// ESP-preview renderer). Full-body PNG from the thumbnail service, decoded
// with stb and cached per user id. A worker thread fetches + decodes, the
// panel thread only ever reads the finished texture.

static ID3D11Device* g_dev = nullptr;
struct AvTex {
    ID3D11ShaderResourceView* tex = nullptr;
    int w = 0, h = 0;
    bool loading = false;
    bool failed = false;
    // Roblox renders busts on demand: first calls answer Pending with no
    // URL. That is NOT failure — retry on a timer instead of giving up.
    bool pending = false;
    unsigned long long retryMs = 0;
    int tries = 0;
};
static std::unordered_map<int, AvTex> g_av;
static std::mutex g_avMtx;
static std::unordered_set<int> g_avPend;

static std::string AvHttpGet(const std::wstring& host, const std::wstring& path) {
    std::string out;
    HINTERNET hS =
        WinHttpOpen(L"violet.lol/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hS)
        return out;
    HINTERNET hC = WinHttpConnect(hS, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) {
        WinHttpCloseHandle(hS);
        return out;
    }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) {
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
        return out;
    }
    if (!WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0)) {
        WinHttpCloseHandle(hR);
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
        return out;
    }
    if (!WinHttpReceiveResponse(hR, nullptr)) {
        WinHttpCloseHandle(hR);
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
        return out;
    }
    std::vector<char> buf;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hR, &avail))
            break;
        if (!avail)
            break;
        size_t old = buf.size();
        buf.resize(old + avail);
        DWORD rd = 0;
        if (!WinHttpReadData(hR, buf.data() + old, avail, &rd))
            break;
        buf.resize(old + rd);
        if (rd == 0)
            break;
    }
    out.assign(buf.begin(), buf.end());
    WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
    return out;
}

static std::string AvImageUrl(const std::string& json) {
    auto p = json.find("imageUrl");
    if (p == std::string::npos)
        return {};
    auto q = json.find("http", p);
    if (q == std::string::npos)
        return {};
    auto e = json.find('"', q);
    if (e == std::string::npos)
        e = json.size();
    std::string url = json.substr(q, e - q);
    size_t pos = url.find("\\u0026");
    while (pos != std::string::npos) {
        url.replace(pos, 6, "&");
        pos = url.find("\\u0026", pos + 1);
    }
    return url;
}

static std::string AvHttpPost(const std::wstring& host, const std::wstring& path,
                              const std::string& body) {
    std::string out;
    HINTERNET hS =
        WinHttpOpen(L"violet.lol/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hS)
        return out;
    HINTERNET hC = WinHttpConnect(hS, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) {
        WinHttpCloseHandle(hS);
        return out;
    }
    HINTERNET hR = WinHttpOpenRequest(hC, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) {
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
        return out;
    }
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!WinHttpSendRequest(hR, headers.c_str(), (DWORD)headers.size(), (LPVOID)body.data(),
                            (DWORD)body.size(), (DWORD)body.size(), 0)) {
        WinHttpCloseHandle(hR);
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
        return out;
    }
    if (!WinHttpReceiveResponse(hR, nullptr)) {
        WinHttpCloseHandle(hR);
        WinHttpCloseHandle(hC);
        WinHttpCloseHandle(hS);
        return out;
    }
    std::vector<char> buf;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hR, &avail))
            break;
        if (!avail)
            break;
        size_t old = buf.size();
        buf.resize(old + avail);
        DWORD rd = 0;
        if (!WinHttpReadData(hR, buf.data() + old, avail, &rd))
            break;
        buf.resize(old + rd);
        if (rd == 0)
            break;
    }
    out.assign(buf.begin(), buf.end());
    WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
    return out;
}

static std::unordered_map<std::string, int> g_nameUid;

static int WebUidForName(const std::string& name) {
    if (name.empty())
        return 0;
    {
        std::lock_guard<std::mutex> lk(g_avMtx);
        auto it = g_nameUid.find(name);
        if (it != g_nameUid.end())
            return it->second;
    }
    std::string body = "{\"usernames\":[\"" + name + "\"]}";
    std::string resp = AvHttpPost(L"users.roblox.com", L"/v1/usernames/users", body);
    int id = 0;
    auto p = resp.find("\"id\"");
    if (p != std::string::npos) {
        p = resp.find(':', p);
        if (p != std::string::npos)
            id = std::atoi(resp.c_str() + p + 1);
    }
    if (id <= 0)
        id = 0;
    {
        std::lock_guard<std::mutex> lk(g_avMtx);
        g_nameUid[name] = id;
    }
    return id;
}

static int LocalUid() {
    // Path 1: direct read (fast path when offsets are fresh).
    if (Globals::localPlayer.Addr) {
        int uid = memory->read<int>(Globals::localPlayer.Addr + Offsets::Player::UserId);
        if (uid > 0)
            return uid;
        long long uid64 =
            memory->read<long long>(Globals::localPlayer.Addr + Offsets::Player::UserId);
        if (uid64 > 0 && uid64 < 10000000000LL)
            return (int)uid64;
    }
    // Path 2: our Player object looked up by name (survives a moved field
    // on the cached instance).
    std::string me = Globals::localPlayer.GetName();
    if (!me.empty() && Globals::players.Addr) {
        auto plr = Globals::players.FindChild(me);
        if (plr.Addr) {
            int uid = memory->read<int>(plr.Addr + Offsets::Player::UserId);
            if (uid > 0)
                return uid;
            long long uid64 = memory->read<long long>(plr.Addr + Offsets::Player::UserId);
            if (uid64 > 0 && uid64 < 10000000000LL)
                return (int)uid64;
        }
        // Path 3: ask the web API by username (works even with fully
        // stale offsets, like the reference renderer does).
        return WebUidForName(me);
    }
    return 0;
}

static void RequestAvatar(int uid) {
    if (uid <= 0)
        return;
    unsigned long long nowMs = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(g_avMtx);
        auto it = g_av.find(uid);
        if (it != g_av.end()) {
            if (it->second.tex || it->second.failed)
                return;
            if (it->second.loading)
                return;
            // Pending render-queue entry: only refire once its timer lapses.
            if (it->second.pending && nowMs < it->second.retryMs)
                return;
        }
        if (g_avPend.find(uid) != g_avPend.end())
            return;
        g_avPend.insert(uid);
        // Keep tries/retry state across attempts; fresh entries start clean.
        if (g_av.find(uid) == g_av.end())
            g_av[uid] = AvTex{};
        g_av[uid].loading = true;
    }
    std::thread([uid] {
        auto markFail = [uid] {
            std::lock_guard<std::mutex> lk(g_avMtx);
            g_avPend.erase(uid);
            auto it = g_av.find(uid);
            if (it != g_av.end()) {
                it->second.loading = false;
                it->second.failed = true;
            }
        };
        auto markPending = [uid] {
            std::lock_guard<std::mutex> lk(g_avMtx);
            g_avPend.erase(uid);
            auto it = g_av.find(uid);
            if (it != g_av.end()) {
                it->second.loading = false;
                if (it->second.tries >= 10) {
                    it->second.failed = true;
                    it->second.pending = false;
                } else {
                    it->second.tries++;
                    it->second.pending = true;
                    it->second.retryMs = GetTickCount64() + 3000;
                }
            }
        };
        std::string json = AvHttpGet(L"thumbnails.roblox.com",
                                     L"/v1/users/avatar?userIds=" + std::to_wstring(uid) +
                                         L"&size=420x420&format=Png&isCircular=false");
        std::string imgUrl = AvImageUrl(json);
        if (imgUrl.empty()) {
            // No URL yet: "Pending"/"InReview" means the renderer hasn't
            // finished — queue a retry, don't burn the slot as failed.
            if (json.find("Pending") != std::string::npos ||
                json.find("InReview") != std::string::npos) {
                markPending();
            } else {
                markFail();
            }
            return;
        }
        std::string host, path, tmp = imgUrl;
        if (tmp.rfind("https://", 0) == 0)
            tmp = tmp.substr(8);
        else if (tmp.rfind("http://", 0) == 0)
            tmp = tmp.substr(7);
        auto sl = tmp.find('/');
        if (sl != std::string::npos) {
            host = tmp.substr(0, sl);
            path = tmp.substr(sl);
        } else {
            host = tmp;
            path = "/";
        }
        std::wstring whost(host.begin(), host.end()), wpath(path.begin(), path.end());
        std::string img = AvHttpGet(whost, wpath);
        if (img.empty() || img.size() < 64) {
            markFail();
            return;
        }
        int w = 0, h = 0, ch = 0;
        unsigned char* pix = stbi_load_from_memory((const unsigned char*)img.data(), (int)img.size(),
                                                   &w, &h, &ch, 4);
        ID3D11Device* dev = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_avMtx);
            dev = g_dev;
        }
        if (!pix || w <= 0 || h <= 0 || !dev) {
            if (pix)
                stbi_image_free(pix);
            markFail();
            return;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = (UINT)w;
        desc.Height = (UINT)h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = pix;
        sd.SysMemPitch = (UINT)(w * 4);
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&desc, &sd, &tex))) {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = desc.Format;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = 1;
            if (SUCCEEDED(dev->CreateShaderResourceView(tex, &vd, &srv))) {
                std::lock_guard<std::mutex> lk(g_avMtx);
                auto& a = g_av[uid];
                if (a.tex)
                    a.tex->Release();
                a.tex = srv;
                a.w = w;
                a.h = h;
                a.loading = false;
                a.failed = false;
            }
            if (tex)
                tex->Release();
        }
        stbi_image_free(pix);
        std::lock_guard<std::mutex> lk(g_avMtx);
        g_avPend.erase(uid);
        auto it = g_av.find(uid);
        if (it != g_av.end() && !it->second.tex) {
            it->second.loading = false;
            it->second.failed = true;
        }
    }).detach();
}

std::string ExeDir() {
    char path[MAX_PATH]{};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path, path + (n ? n : 0));
    const auto p = s.find_last_of("\\/");
    if (p != std::string::npos)
        s.resize(p);
    return s;
}

bool FileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return (bool)f;
}

struct Mtl {
    float r = 1.0f, g = 1.0f, b = 1.0f;
    std::string mapKd;
};

bool ParseObjText(const std::string& text, const std::string& objDir, bool memMode);
bool LoadObj(const std::string& objPath, const std::string& objDir) {
    std::ifstream rf(objPath, std::ios::binary);
    if (!rf) {
        g_fail = "player.obj missing";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        g_fail = "obj parse failed";
        return false;
    }
    return ParseObjText(text, objDir, false);
}

bool ParseObjText(const std::string& text, const std::string& objDir, bool memMode) {
    std::istringstream f(text);
    std::vector<float> pos, uv;
    std::unordered_map<std::string, Mtl> mtls;
    std::string curMtl;
    std::string line;
    struct RawTri {
        int v[3], t[3];
        std::string mat;
    };
    std::vector<RawTri> rawTris;
    std::string mtlFile;
    auto parseTriple = [](const std::string& tok, int& vi, int& vti) {
        vi = vti = -1;
        int a = 0, b = 0, c = 0;
        if (std::sscanf(tok.c_str(), "%d/%d/%d", &a, &b, &c) == 3) {
            vi = a - 1;
            vti = b - 1;
        } else if (std::sscanf(tok.c_str(), "%d//%d", &a, &c) == 2) {
            vi = a - 1;
        } else if (std::sscanf(tok.c_str(), "%d/%d", &a, &b) == 2) {
            vi = a - 1;
            vti = b - 1;
        } else if (std::sscanf(tok.c_str(), "%d", &a) == 1) {
            vi = a - 1;
        }
    };
    while (std::getline(f, line)) {
        if (line.compare(0, 7, "mtllib ") == 0) {
            mtlFile = line.substr(7);
            while (!mtlFile.empty() && (mtlFile.back() == '\r' || mtlFile.back() == ' ' || mtlFile.back() == '\t'))
                mtlFile.pop_back();
        } else if (line.compare(0, 2, "v ") == 0) {
            float x = 0, y = 0, z = 0;
            std::sscanf(line.c_str() + 2, "%f %f %f", &x, &y, &z);
            pos.push_back(x);
            pos.push_back(y);
            pos.push_back(z);
        } else if (line.compare(0, 3, "vt ") == 0) {
            float u = 0, v = 0;
            std::sscanf(line.c_str() + 3, "%f %f", &u, &v);
            uv.push_back(u);
            uv.push_back(v);
        } else if (line.compare(0, 7, "usemtl ") == 0) {
            curMtl = line.substr(7);
            while (!curMtl.empty() &&
                   (curMtl.back() == '\r' || curMtl.back() == ' ' || curMtl.back() == '\t'))
                curMtl.pop_back();
        } else if (line.compare(0, 2, "f ") == 0) {
            std::istringstream ss(line.substr(2));
            std::string tok;
            std::vector<int> vis, vts;
            while (ss >> tok) {
                int vi, vti;
                parseTriple(tok, vi, vti);
                if (vi < 0 || (std::size_t)vi * 3 + 2 >= pos.size())
                    continue;
                if (vti >= 0 && (std::size_t)vti * 2 + 1 >= uv.size())
                    vti = -1;
                vis.push_back(vi);
                vts.push_back(vti);
            }
            for (std::size_t i = 1; i + 1 < vis.size(); ++i) {
                RawTri rt{};
                rt.v[0] = vis[0];
                rt.v[1] = vis[i];
                rt.v[2] = vis[i + 1];
                rt.t[0] = vts[0];
                rt.t[1] = vts[i];
                rt.t[2] = vts[i + 1];
                rt.mat = curMtl;
                rawTris.push_back(rt);
            }
        }
    }
    if (rawTris.empty()) {
        g_fail = "obj parse failed";
        return false;
    }
    std::unordered_map<std::string, int> matIds;
    std::vector<Mtl> matList(1);
    if (!mtlFile.empty()) {
        // Embedded tung model carries its own material; disk files load theirs.
        std::string mtlText;
        if (memMode && mtlFile == "tung.mtl") {
            mtlText = "newmtl tung\nKd 1 1 1\nmap_Kd shaded.png\n";
        } else {
            std::ifstream rf(objDir + "\\" + mtlFile, std::ios::binary);
            if (rf)
                mtlText.assign((std::istreambuf_iterator<char>(rf)),
                               std::istreambuf_iterator<char>());
        }
        std::istringstream mf(mtlText);
        std::string ml, cur;
        while (std::getline(mf, ml)) {
            if (ml.compare(0, 7, "newmtl ") == 0) {
                cur = ml.substr(7);
                while (!cur.empty() && (cur.back() == '\r' || cur.back() == ' ' || cur.back() == '\t'))
                    cur.pop_back();
                if (mtls.find(cur) == mtls.end())
                    mtls[cur] = Mtl{};
            } else if (ml.compare(0, 3, "Kd ") == 0 && !cur.empty()) {
                float r = 1, g = 1, b = 1;
                std::sscanf(ml.c_str() + 3, "%f %f %f", &r, &g, &b);
                mtls[cur].r = r;
                mtls[cur].g = g;
                mtls[cur].b = b;
            } else if (ml.compare(0, 7, "map_Kd ") == 0 && !cur.empty()) {
                std::string t = ml.substr(7);
                while (!t.empty() && (t.back() == '\r' || t.back() == ' ' || t.back() == '\t'))
                    t.pop_back();
                mtls[cur].mapKd = t;
            }
        }
    }
    std::string firstTex;
    for (auto& kv : mtls) {
        if (!kv.second.mapKd.empty() && firstTex.empty())
            firstTex = kv.second.mapKd;
    }
    struct Key {
        int v, t, m;
        bool operator==(const Key& o) const { return v == o.v && t == o.t && m == o.m; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return ((std::size_t)k.v * 73856093ull) ^ ((std::size_t)(k.t + 1) * 19349663ull) ^
                   ((std::size_t)(k.m + 1) * 83492791ull);
        }
    };
    std::unordered_map<Key, std::uint32_t, KeyHash> dedup;
    std::vector<Vert> verts;
    std::vector<std::uint32_t> idx;
    std::vector<char> triUV;
    verts.reserve(4096);
    idx.reserve(8192);
    int triNo = 0;
    for (auto& rt : rawTris) {
        ++triNo;
        (void)triNo;
        Mtl mt;
        int mi = 0;
        if (!rt.mat.empty()) {
            auto mii = matIds.find(rt.mat);
            if (mii == matIds.end()) {
                mi = (int)matIds.size() + 1;
                matIds[rt.mat] = mi;
            } else {
                mi = mii->second;
            }
            auto mit = mtls.find(rt.mat);
            if (mit != mtls.end())
                mt = mit->second;
        }
        std::uint32_t out[3]{};
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            Key kk{rt.v[k], rt.t[k], mi};
            auto it = dedup.find(kk);
            if (it != dedup.end()) {
                out[k] = it->second;
                continue;
            }
            if ((int)verts.size() >= kMaxVerts) {
                ok = false;
                break;
            }
            Vert w{};
            w.x = pos[(std::size_t)rt.v[k] * 3 + 0];
            w.y = pos[(std::size_t)rt.v[k] * 3 + 1];
            w.z = pos[(std::size_t)rt.v[k] * 3 + 2];
            if (rt.t[k] >= 0) {
                w.u = uv[(std::size_t)rt.t[k] * 2 + 0];
                w.v = 1.0f - uv[(std::size_t)rt.t[k] * 2 + 1];
            }
            w.cr = mt.r;
            w.cg = mt.g;
            w.cb = mt.b;
            out[k] = (std::uint32_t)verts.size();
            verts.push_back(w);
            dedup[kk] = out[k];
        }
        if (!ok)
            continue;
        idx.push_back(out[0]);
        idx.push_back(out[1]);
        idx.push_back(out[2]);
        triUV.push_back((rt.t[0] >= 0 && rt.t[1] >= 0 && rt.t[2] >= 0) ? 1 : 0);
    }
    if (verts.empty() || idx.empty()) {
        g_fail = "obj parse failed";
        return false;
    }
    float mnx = 1e9f, mny = 1e9f, mnz = 1e9f, mxx = -1e9f, mxy = -1e9f, mxz = -1e9f;
    for (auto& w : verts) {
        if (w.x < mnx) mnx = w.x;
        if (w.y < mny) mny = w.y;
        if (w.z < mnz) mnz = w.z;
        if (w.x > mxx) mxx = w.x;
        if (w.y > mxy) mxy = w.y;
        if (w.z > mxz) mxz = w.z;
    }
    const float cx = (mnx + mxx) * 0.5f, cy = (mny + mxy) * 0.5f, cz = (mnz + mxz) * 0.5f;
    const float ext = (std::max)((mxx - mnx), (std::max)((mxy - mny), (mxz - mnz)));
    if (ext < 1e-6f) {
        g_fail = "obj parse failed";
        return false;
    }
    const float s = 1.15f / ext;
    float floor_y = 1e9f;
    for (auto& w : verts) {
        w.x = (w.x - cx) * s;
        w.y = (w.y - cy) * s;
        w.z = (w.z - cz) * s;
        if (w.y < floor_y)
            floor_y = w.y;
    }
    g_floorY = floor_y;
    g_verts = std::move(verts);
    g_idx = std::move(idx);
    g_triUV = std::move(triUV);
    if (!firstTex.empty()) {
        if (memMode) {
            g_texPathOut = "embedded:shaded.png";
            g_texEmbedded = true;
        } else {
            g_texPathOut = objDir + "\\" + firstTex;
            g_texEmbedded = false;
        }
    }
    return true;
}

bool LoadTexture(ID3D11Device* dev, const std::string& texPath) {
    std::ifstream tf(texPath, std::ios::binary);
    if (!tf) {
        g_fail = "texture file missing";
        return false;
    }
    tf.seekg(0, std::ios::end);
    const std::size_t tlen = (std::size_t)tf.tellg();
    tf.seekg(0, std::ios::beg);
    if (tlen == 0 || tlen > 32 * 1024 * 1024) {
        g_fail = "texture decode failed";
        return false;
    }
    std::vector<unsigned char> tbuf(tlen);
    tf.read((char*)tbuf.data(), tlen);
    tf.close();
    int tw = 0, th = 0, tc = 0;
    unsigned char* img = stbi_load_from_memory(tbuf.data(), (int)tbuf.size(), &tw, &th, &tc, 4);
    if (!img || tw <= 0 || th <= 0) {
        g_fail = "texture decode failed";
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)tw;
    td.Height = (UINT)th;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = img;
    sub.SysMemPitch = (UINT)(tw * 4);
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sub, &tex)) || !tex) {
        stbi_image_free(img);
        g_fail = "gpu init failed";
        return false;
    }
    stbi_image_free(img);
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(tex, &svd, &g_tex)) || !g_tex) {
        tex->Release();
        g_fail = "gpu init failed";
        return false;
    }
    tex->Release();
    g_texW = tw;
    g_texH = th;
    return true;
}

bool LoadTextureMem(ID3D11Device* dev, const unsigned char* data, std::size_t len) {
    if (!dev || !data || !len || len > 32 * 1024 * 1024) {
        g_fail = "texture decode failed";
        return false;
    }
    int tw = 0, th = 0, tc = 0;
    unsigned char* img = stbi_load_from_memory(data, (int)len, &tw, &th, &tc, 4);
    if (!img || tw <= 0 || th <= 0) {
        g_fail = "texture decode failed";
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)tw;
    td.Height = (UINT)th;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = img;
    sub.SysMemPitch = (UINT)(tw * 4);
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sub, &tex)) || !tex) {
        stbi_image_free(img);
        g_fail = "gpu init failed";
        return false;
    }
    stbi_image_free(img);
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(tex, &svd, &g_tex)) || !g_tex) {
        tex->Release();
        g_fail = "gpu init failed";
        return false;
    }
    tex->Release();
    g_texW = tw;
    g_texH = th;
    return true;
}

bool LoadTungEmbedded() {
    g_texPathOut.clear();
    g_texEmbedded = false;
    if (!ParseObjText(std::string((const char*)tung_obj_data, (size_t)tung_obj_data_len), "", true))
        return false;
    // Baked shaded.png straight out of the binary — no files needed.
    if (g_texEmbedded) {
        if (!LoadTextureMem(g_dev, tung_png_data, (size_t)tung_png_data_len))
            g_fail = "texture fallback";
        else
            g_fail = "ok (tung tung)";
    } else {
        g_fail = "ok (tung tung)";
    }
    return true;
}

}

void BuildFallbackAvatar() {
    // No player.obj on disk? Build a blocky R6 rig in-code so preview never says "missing".
    g_verts.clear(); g_idx.clear(); g_triUV.clear();
    auto pushBox = [&](float cx, float cy, float cz, float sx, float sy, float sz, float r, float g, float b) {
        float hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
        uint32_t base = (uint32_t)g_verts.size();
        Vert v{}; v.u = 0; v.v = 0; v.cr = r; v.cg = g; v.cb = b;
        float px[8] = {cx-hx,cx+hx,cx+hx,cx-hx,cx-hx,cx+hx,cx+hx,cx-hx};
        float py[8] = {cy-hy,cy-hy,cy+hy,cy+hy,cy-hy,cy-hy,cy+hy,cy+hy};
        float pz[8] = {cz-hz,cz-hz,cz-hz,cz-hz,cz+hz,cz+hz,cz+hz,cz+hz};
        for (int i = 0; i < 8; ++i) { v.x = px[i]; v.y = py[i]; v.z = pz[i]; g_verts.push_back(v); }
        uint32_t q[6][4] = {{0,1,2,3},{4,5,6,7},{0,1,5,4},{2,3,7,6},{0,3,7,4},{1,2,6,5}};
        for (auto& f : q) {
            g_idx.push_back(base+f[0]); g_idx.push_back(base+f[1]); g_idx.push_back(base+f[2]);
            g_idx.push_back(base+f[0]); g_idx.push_back(base+f[2]); g_idx.push_back(base+f[3]);
            g_triUV.push_back(0); g_triUV.push_back(0);
        }
    };
    // normalize space ~1.2 units tall like the obj path
    pushBox(0.00f, 0.42f, 0.00f, 0.26f, 0.26f, 0.26f, 1.0f, 0.85f, 0.30f); // head
    pushBox(0.00f, 0.05f, 0.00f, 0.44f, 0.48f, 0.24f, 0.20f, 0.55f, 1.00f); // torso
    pushBox(-0.32f, 0.05f, 0.00f, 0.20f, 0.48f, 0.22f, 1.00f, 0.85f, 0.30f); // L arm
    pushBox(0.32f, 0.05f, 0.00f, 0.20f, 0.48f, 0.22f, 1.00f, 0.85f, 0.30f); // R arm
    pushBox(-0.12f, -0.42f, 0.00f, 0.20f, 0.44f, 0.24f, 0.25f, 0.80f, 0.30f); // L leg
    pushBox(0.12f, -0.42f, 0.00f, 0.20f, 0.44f, 0.24f, 0.25f, 0.80f, 0.30f); // R leg
    float mn = 1e9f, mx = -1e9f;
    for (auto& w : g_verts) { if (w.y < mn) mn = w.y; if (w.y > mx) mx = w.y; }
    g_floorY = mn;
    g_texPathOut.clear();
}

bool Init(ID3D11Device* dev) {
    if (dev)
        g_dev = dev;
    if (g_ready || !dev)
        return g_ready;
    std::string objPath;
    std::string objDir;
    {
        char cwd[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, cwd);
        std::string exe = ExeDir();
        std::string candidates[5] = {
            exe + "\\player.obj",
            exe + "\\assets\\player.obj",
            std::string(cwd) + "\\player.obj",
            std::string(cwd) + "\\assets\\player.obj",
            exe + "\\Violet\\assets\\player.obj"
        };
        for (auto& c : candidates) {
            if (FileExists(c)) {
                objPath = c;
                objDir = c.substr(0, c.find_last_of("\\/"));
                break;
            }
        }
        if (objPath.empty()) {
            // FIX: was hardcoded to C:\Users\admin\Desktop\Ny mappe (2)\player.obj
            // (dev machine leftover) -> every other PC got "player.obj missing".
            // Fall back to built-in rig so preview always works.
            BuildFallbackAvatar();
            g_ready = true;
            g_fail = "ok (built-in avatar - drop player.obj next to Violet.exe to override)";
            return true;
        }
    }
    g_texPathOut.clear();
    if (!LoadObj(objPath, objDir)) {
        // Corrupt obj? Don't brick the panel — use built-in rig.
        BuildFallbackAvatar();
        g_ready = true;
        g_fail = "ok (built-in avatar - player.obj parse failed)";
        return true;
    }
    if (g_texPathOut.empty() || !LoadTexture(dev, g_texPathOut)) {
        g_fail = "texture fallback";
    }
    g_ready = true;
    g_fail = "ok";
    return true;
}

void Shutdown() {
    if (g_tex) {
        g_tex->Release();
        g_tex = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g_avMtx);
        for (auto& kv : g_av)
            if (kv.second.tex)
                kv.second.tex->Release();
        g_av.clear();
        g_avPend.clear();
    }
    g_dev = nullptr;
    g_verts.clear();
    g_verts.shrink_to_fit();
    g_idx.clear();
    g_idx.shrink_to_fit();
    g_triUV.clear();
    g_triUV.shrink_to_fit();
    g_texW = g_texH = 0;
    g_ready = false;
}

bool Ready() {
    // Live-player source needs no OBJ on disk — panel is always servable.
    if (variables::Aimbot::previewSource == 1)
        return true;
    return g_ready && !g_verts.empty() && !g_idx.empty();
}

void BuildTungTung() {
    // Procedural Tung Tung Sahur: wooden log body, bands, angry eyes,
    // mouth, arms, bat and feet. Vertex-colored boxes straight through
    // the existing painter pipeline (no texture needed).
    std::vector<Vert> verts;
    std::vector<std::uint32_t> idx;
    std::vector<char> tuv;
    auto box = [&](float cx, float cy, float cz, float sx, float sy, float sz, float r, float g,
                   float b) {
        uint32_t base = (uint32_t)verts.size();
        Vert v{};
        v.u = 0;
        v.v = 0;
        v.cr = r;
        v.cg = g;
        v.cb = b;
        float hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
        float px[8] = {cx - hx, cx + hx, cx + hx, cx - hx, cx - hx, cx + hx, cx + hx, cx - hx};
        float py[8] = {cy - hy, cy - hy, cy + hy, cy + hy, cy - hy, cy - hy, cy + hy, cy + hy};
        float pz[8] = {cz - hz, cz - hz, cz - hz, cz - hz, cz + hz, cz + hz, cz + hz, cz + hz};
        for (int i = 0; i < 8; ++i) {
            v.x = px[i];
            v.y = py[i];
            v.z = pz[i];
            verts.push_back(v);
        }
        uint32_t q[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
                            {2, 3, 7, 6}, {0, 3, 7, 4}, {1, 2, 6, 5}};
        for (auto& f : q) {
            idx.push_back(base + f[0]);
            idx.push_back(base + f[1]);
            idx.push_back(base + f[2]);
            idx.push_back(base + f[0]);
            idx.push_back(base + f[2]);
            idx.push_back(base + f[3]);
            tuv.push_back(0);
            tuv.push_back(0);
        }
    };
    box(0.00f, 0.03f, 0.00f, 0.34f, 0.95f, 0.30f, 0.42f, 0.26f, 0.12f); // log body
    box(0.00f, 0.28f, 0.00f, 0.36f, 0.07f, 0.32f, 0.29f, 0.17f, 0.07f); // bands
    box(0.00f, -0.18f, 0.00f, 0.36f, 0.07f, 0.32f, 0.29f, 0.17f, 0.07f);
    box(-0.085f, 0.33f, 0.155f, 0.10f, 0.12f, 0.03f, 0.95f, 0.95f, 0.92f); // eyes
    box(0.085f, 0.33f, 0.155f, 0.10f, 0.12f, 0.03f, 0.95f, 0.95f, 0.92f);
    box(-0.085f, 0.315f, 0.175f, 0.045f, 0.065f, 0.02f, 0.05f, 0.04f, 0.04f); // pupils
    box(0.085f, 0.315f, 0.175f, 0.045f, 0.065f, 0.02f, 0.05f, 0.04f, 0.04f);
    box(-0.085f, 0.42f, 0.16f, 0.11f, 0.03f, 0.02f, 0.15f, 0.09f, 0.04f); // angry brows
    box(0.085f, 0.42f, 0.16f, 0.11f, 0.03f, 0.02f, 0.15f, 0.09f, 0.04f);
    box(0.00f, 0.13f, 0.16f, 0.17f, 0.035f, 0.02f, 0.10f, 0.06f, 0.03f); // mouth
    box(-0.25f, 0.02f, 0.00f, 0.10f, 0.44f, 0.10f, 0.35f, 0.21f, 0.10f); // arms
    box(0.25f, 0.02f, 0.00f, 0.10f, 0.44f, 0.10f, 0.35f, 0.21f, 0.10f);
    box(0.40f, -0.02f, 0.06f, 0.07f, 0.42f, 0.07f, 0.72f, 0.55f, 0.32f); // bat handle
    box(0.40f, 0.32f, 0.06f, 0.12f, 0.34f, 0.12f, 0.78f, 0.62f, 0.38f); // bat barrel
    box(-0.10f, -0.50f, 0.03f, 0.15f, 0.10f, 0.24f, 0.20f, 0.12f, 0.06f); // feet
    box(0.10f, -0.50f, 0.03f, 0.15f, 0.10f, 0.24f, 0.20f, 0.12f, 0.06f);
    float floor_y = 1e9f;
    for (auto& w : verts)
        if (w.y < floor_y)
            floor_y = w.y;
    g_floorY = floor_y;
    g_verts = std::move(verts);
    g_idx = std::move(idx);
    g_triUV = std::move(tuv);
    g_ready = true;
    g_fail = "ok (tung tung)";
}

void EnsureTungTung() {
    if (g_showingTung)
        return;
    if (g_ready && !g_verts.empty()) {
        g_fileVerts = g_verts;
        g_fileIdx = g_idx;
        g_fileUV = g_triUV;
    }
    // Stash the file-mesh texture so the tung texture doesn't leak into it.
    g_fileTex = g_tex;
    g_tex = nullptr;
    g_texW = g_texH = 0;
    // Real baked model first, procedural log only if the bake ever breaks.
    if (!LoadTungEmbedded())
        BuildTungTung();
    g_showingTung = true;
}

void RestoreFileMesh() {
    if (!g_showingTung)
        return;
    if (g_tex) {
        g_tex->Release();
        g_tex = nullptr;
    }
    g_verts = std::move(g_fileVerts);
    g_idx = std::move(g_fileIdx);
    g_triUV = std::move(g_fileUV);
    g_fileVerts.clear();
    g_fileIdx.clear();
    g_fileUV.clear();
    g_tex = g_fileTex;
    g_fileTex = nullptr;
    g_texEmbedded = false;
    g_showingTung = false;
    g_ready = !g_verts.empty() && !g_idx.empty();
    if (!g_ready)
        g_fail = "player.obj missing";
}

static RBX::Vec3 LivePartPos(std::uintptr_t partAddr) {
    if (!partAddr)
        return {};
    const uintptr_t prim = memory->read<uintptr_t>(partAddr + Offsets::BasePart::Primitive);
    if (!prim)
        return {};
    RBX::Vec3 p = memory->read<RBX::Vec3>(prim + Offsets::Primitive::Position);
    if (!std::isfinite(p.X) || !std::isfinite(p.Y) || !std::isfinite(p.Z))
        return {};
    return p;
}

void DrawPanel() {
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.1f)
        dt = 0.016f;
    static float previewFade = 0.0f;
    if (variables::menuOpen && variables::Aimbot::playerPreview) {
        previewFade += dt * 8.0f;
        if (previewFade > 1.0f)
            previewFade = 1.0f;
    } else {
        previewFade -= dt * 8.0f;
        if (previewFade < 0.0f)
            previewFade = 0.0f;
    }
    if (previewFade <= 0.001f)
        return;

    static float ax = 690.0f, ay = 80.0f, ah = 390.0f;
    static float sw = 0.0f;
    if (sw <= 0.0f)
        sw = (float)GetSystemMetrics(SM_CXSCREEN);
    if (ImGuiWindow* mw = ImGui::FindWindowByName("violet.lol")) {
        if (!mw->Collapsed) {
            const float nx = std::floor(mw->Pos.x) + std::floor(mw->Size.x) + 8.0f;
            if (nx + kPanelW <= sw - 8.0f) {
                ax = nx;
            } else {
                ax = std::floor(mw->Pos.x) - kPanelW - 8.0f;
            }
            ay = std::floor(mw->Pos.y);
            if (mw->Size.y > 100.0f)
                ah = std::floor(mw->Size.y);
        }
    }
    const float x = std::floor(ax);
    const float y = std::floor(ay);
    const float h = std::floor(ah);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, previewFade);
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kPanelW, h), ImGuiCond_Always);
    if (!ImGui::Begin("##player_preview", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    const imGuiCustom::Theme& theme = imGuiCustom::GetTheme();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pmin = ImVec2(std::floor(ImGui::GetWindowPos().x), std::floor(ImGui::GetWindowPos().y));
    const ImVec2 pmax = pmin + ImVec2(kPanelW, h);

    dl->AddRectFilled(pmin, pmax, imGuiCustom::ColorU32(theme.WindowBg), 0.0f);
    {
        const float ga = previewFade * previewFade;
        if (ga > 0.02f)
            imGuiCustom::AddGlowRect(dl, pmin, pmax, imGuiCustom::ColorU32(theme.Accent, ga), ga, 5, 8.0f, 0.0f);
    }
    dl->AddRect(pmin, pmax, imGuiCustom::OutlineBlack(), 0.0f, 0, 1.0f);
    dl->AddRect(pmin + ImVec2(1.0f, 1.0f), pmax - ImVec2(1.0f, 1.0f), imGuiCustom::OutlineInner(), 0.0f, 0, 1.0f);
    dl->AddRectFilled(pmin, pmin + ImVec2(kPanelW, 1.5f), imGuiCustom::ColorU32(theme.Accent));

    const imGuiCustom::Fonts& fonts = imGuiCustom::GetFonts();
    ImFont* title_font = fonts.CascadiaMonoBL ? fonts.CascadiaMonoBL : ImGui::GetFont();
    const float title_fs = 12.0f * imGuiCustom::g_fontScale;
    const ImVec2 title_sz = title_font->CalcTextSizeA(title_fs, FLT_MAX, 0.0f, "player preview");
    dl->AddText(title_font, title_fs, pmin + ImVec2(std::floor((kPanelW - title_sz.x) * 0.5f), 3.5f),
                imGuiCustom::ColorU32(theme.TextBright), "player preview");

    const ImVec2 rmin(std::floor(pmin.x + 6.0f), std::floor(pmin.y + 22.0f));
    const ImVec2 rmax(std::floor(pmin.x + kPanelW - 6.0f), std::floor(pmin.y + h - 10.0f));
    g_rx = rmin.x;
    g_ry = rmin.y;
    g_rw = rmax.x - rmin.x;
    g_rh = rmax.y - rmin.y;
    if (g_rh < 40.0f)
        g_rh = 40.0f;

    // Tung Tung slot resolves before the ready gate so the log shows
    // even when no OBJ file was ever loaded.
    if (variables::Aimbot::previewSource == 2)
        EnsureTungTung();
    else
        RestoreFileMesh();

    if (!g_ready) {
        dl->AddText(title_font, 12.0f * imGuiCustom::g_fontScale, ImVec2(std::floor(pmin.x + 8.0f), std::floor(g_ry + 8.0f)),
                    IM_COL32(255, 90, 90, 255), g_fail);
        g_rw = 0.0f;
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    dl->AddRectFilled(rmin, rmax, imGuiCustom::ColorU32(theme.CardBg), 0.0f);
    {
        const float ga = previewFade * previewFade * 0.6f;
        if (ga > 0.02f)
            imGuiCustom::AddGlowRect(dl, rmin, rmax, imGuiCustom::ColorU32(theme.Accent, ga), ga, 4, 6.0f, 0.0f);
    }
    dl->AddRect(rmin, rmax, imGuiCustom::OutlineBlack(), 0.0f, 0, 1.0f);
    dl->AddRect(rmin + ImVec2(1.0f, 1.0f), rmax - ImVec2(1.0f, 1.0f), imGuiCustom::OutlineInner(), 0.0f, 0, 1.0f);

    const bool menuOpen = variables::menuOpen;
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool isHovered = menuOpen && (mp.x >= rmin.x && mp.x <= rmax.x && mp.y >= rmin.y && mp.y <= rmax.y);
    static bool s_isDragging = false;
    if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s_isDragging = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_isDragging = false;
    }
    if (s_isDragging) {
        s_yawDrag += ImGui::GetIO().MouseDelta.x * 0.015f;
        s_pitch += -ImGui::GetIO().MouseDelta.y * 0.015f;
        if (s_pitch < -1.0f)
            s_pitch = -1.0f;
        if (s_pitch > 1.0f)
            s_pitch = 1.0f;
    }
    if (isHovered) {
        if (ImGui::GetIO().MouseWheel != 0.0f) {
            s_zoom += ImGui::GetIO().MouseWheel * 0.10f;
            if (s_zoom < 0.4f)
                s_zoom = 0.4f;
            if (s_zoom > 2.5f)
                s_zoom = 2.5f;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            s_yawDrag = 0.0f;
            s_pitch = 0.0f;
            s_zoom = 1.0f;
        }
    }
    const float yaw = s_yawAuto + s_yawDrag;
    if (!s_isDragging)
        s_yawAuto += dt * 0.35f; // slow turntable when hands off, like the reference renderer
    const float ca = cosf(yaw), sa = sinf(yaw);
    const float cp = cosf(s_pitch), sp = sinf(s_pitch);
    const float sw2 = g_rw, sh2 = g_rh;
    float ext = 0.0f;
    for (auto& w : g_verts) {
        const float dx = w.x >= 0 ? w.x : -w.x;
        const float dy = w.y >= 0 ? w.y : -w.y;
        const float dz = w.z >= 0 ? w.z : -w.z;
        const float m = dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
        if (m > ext)
            ext = m;
    }
    if (ext < 1e-6f)
        ext = 1.0f;
    const float scale = (std::min)(sw2, sh2) * 0.44f / ext * s_zoom;
    const float cx = rmin.x + sw2 * 0.5f;
    const float cy = rmin.y + sh2 * 0.47f;

    const ImVec2 clipMin = rmin + ImVec2(2.0f, 2.0f);
    const ImVec2 clipMax = rmax - ImVec2(2.0f, 2.0f);
    dl->PushClipRect(clipMin, clipMax, true);

    // ---- Live-player source: the client's own rig, posed in real time ----
    // Same normalize -> orbit-project pipeline as the OBJ path, but the
    // joints are read live from the local character, so the panel mirrors
    // your actual pose (run / jump / arms) instead of a static file.
    if (variables::Aimbot::previewSource == 1) {
        // Client avatar first: full-body render fetched by user id.
        // Live rig below is only the offline fallback.
        int uid = LocalUid();
        if (uid <= 0) {
            dl->PopClipRect();
            dl->AddText(title_font, 12.0f * imGuiCustom::g_fontScale,
                        ImVec2(std::floor(pmin.x + 8.0f), std::floor(g_ry + 8.0f)),
                        IM_COL32(255, 200, 90, 255), "join a game first");
            g_rw = 0.0f;
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        RequestAvatar(uid);
        ID3D11ShaderResourceView* avTex = nullptr;
        int avW = 0, avH = 0;
        bool avLoading = false, avFailed = false, avPending = false;
        {
            std::lock_guard<std::mutex> lk(g_avMtx);
            auto it = g_av.find(uid);
            if (it != g_av.end()) {
                avTex = it->second.tex;
                avW = it->second.w;
                avH = it->second.h;
                avLoading = it->second.loading;
                avFailed = it->second.failed;
                avPending = it->second.pending;
            }
        }
        if (avTex && avW > 0 && avH > 0) {
            float bw = rmax.x - rmin.x, bh = rmax.y - rmin.y;
            float s = (std::min)(bw / (float)avW, bh / (float)avH);
            float dw = (float)avW * s, dh = (float)avH * s;
            ImVec2 i0(rmin.x + (bw - dw) * 0.5f, rmin.y + (bh - dh) * 0.5f);
            ImVec2 i1(i0.x + dw, i0.y + dh);
            dl->AddImage((ImTextureID)avTex, i0, i1);
            dl->AddRect(i0, i1, IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.0f);
            std::string nm = Globals::localPlayer.GetName();
            if (!nm.empty()) {
                ImFont* espF = Visuals::EspFont();
                const float espSz = variables::Misc::espFontSize;
                const ImVec2 ts = espF->CalcTextSizeA(espSz, FLT_MAX, 0.0f, nm.c_str());
                Visuals::DrawOutlinedText(
                    dl, ImVec2(i0.x + (dw - ts.x) * 0.5f, i1.y - ts.y - 3.0f), nm,
                    imGuiCustom::ColorU32(variables::ESP::nameColor));
            }
            dl->PopClipRect();
            dl->AddRect(rmin, rmax, imGuiCustom::OutlineBlack(), 0.0f, 0, 1.0f);
            dl->AddRect(rmin + ImVec2(1.0f, 1.0f), rmax - ImVec2(1.0f, 1.0f),
                        imGuiCustom::OutlineInner(), 0.0f, 0, 1.0f);
            dl->AddRectFilled(pmin, pmin + ImVec2(kPanelW, 1.5f),
                              imGuiCustom::ColorU32(theme.Accent));
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        if (avLoading && !avFailed && !avPending) {
            dl->PopClipRect();
            dl->AddText(title_font, 12.0f * imGuiCustom::g_fontScale,
                        ImVec2(std::floor(pmin.x + 8.0f), std::floor(g_ry + 8.0f)),
                        IM_COL32(255, 200, 90, 255), "fetching avatar...");
            g_rw = 0.0f;
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        if (avPending && !avFailed && !avTex) {
            dl->PopClipRect();
            dl->AddText(title_font, 12.0f * imGuiCustom::g_fontScale,
                        ImVec2(std::floor(pmin.x + 8.0f), std::floor(g_ry + 8.0f)),
                        IM_COL32(255, 200, 90, 255), "rendering avatar...");
            g_rw = 0.0f;
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        // Fetch failed (offline / blocked) — fall through to the live rig.
        RBX::RbxInstance ch = Globals::localPlayer.GetModelRef();
        PlayerCache::LimbAddrs limbs = ch.Addr ? PlayerCache::GetLimbs(ch.Addr, false)
                                               : PlayerCache::LimbAddrs{};
        RBX::Vec3 hp = LivePartPos(limbs.head);
        RBX::Vec3 rp = LivePartPos(limbs.hrp);
        if (!ch.Addr || (hp.X == 0 && hp.Y == 0 && hp.Z == 0) ||
            (rp.X == 0 && rp.Y == 0 && rp.Z == 0)) {
            dl->PopClipRect();
            dl->AddText(title_font, 12.0f * imGuiCustom::g_fontScale,
                        ImVec2(std::floor(pmin.x + 8.0f), std::floor(g_ry + 8.0f)),
                        IM_COL32(255, 200, 90, 255),
                        ch.Addr ? "waiting for rig..." : "join a game first");
            g_rw = 0.0f;
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        std::vector<RBX::Vec3> jpts;
        struct LBone {
            int a, b;
            float w; // full width in preview units — renders as solid body mass
        };
        std::vector<LBone> bones;
        auto addPt = [&](const RBX::Vec3& p) -> int {
            jpts.push_back(p);
            return (int)jpts.size() - 1;
        };
        auto addPart = [&](std::uintptr_t a) -> int {
            RBX::Vec3 p = LivePartPos(a);
            if (p.X == 0 && p.Y == 0 && p.Z == 0)
                return -1;
            return addPt(p);
        };
        int iHead = addPt(hp);
        int iRoot = addPt(rp);
        if (limbs.r6) {
            int iTorso = addPart(limbs.torso);
            int iLA = addPart(limbs.lArm), iRA = addPart(limbs.rArm);
            int iLL = addPart(limbs.lLeg), iRL = addPart(limbs.rLeg);
            int spine = (iTorso >= 0) ? iTorso : iRoot;
            bones.push_back({iHead, spine, 0.12f});
            if (iLA >= 0)
                bones.push_back({spine, iLA, 0.16f});
            if (iRA >= 0)
                bones.push_back({spine, iRA, 0.16f});
            if (spine != iRoot)
                bones.push_back({spine, iRoot, 0.40f});
            if (iLL >= 0)
                bones.push_back({iRoot, iLL, 0.20f});
            if (iRL >= 0)
                bones.push_back({iRoot, iRL, 0.20f});
        } else {
            int iUT = addPart(limbs.upperTorso), iLT = addPart(limbs.lowerTorso);
            int top = (iUT >= 0) ? iUT : iRoot;
            int pelvis = iRoot;
            bones.push_back({iHead, top, 0.12f});
            if (iUT >= 0 && iLT >= 0) {
                bones.push_back({iUT, iLT, 0.38f});
                pelvis = iLT;
            } else if (iLT >= 0) {
                bones.push_back({top, iLT, 0.36f});
                pelvis = iLT;
            } else if (iUT >= 0) {
                bones.push_back({iUT, iRoot, 0.36f});
            }
            auto chain3 = [&](std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, int from, float w1,
                              float w2, float w3) {
                int p1 = addPart(a), p2 = addPart(b), p3 = addPart(c);
                int prev = from;
                float pw = w1;
                if (p1 >= 0) {
                    bones.push_back({prev, p1, w1});
                    prev = p1;
                    pw = w2;
                }
                if (p2 >= 0) {
                    bones.push_back({prev, p2, pw});
                    prev = p2;
                    pw = w3;
                }
                if (p3 >= 0)
                    bones.push_back({prev, p3, pw});
            };
            chain3(limbs.lUpperArm, limbs.lLowerArm, limbs.lHand, top, 0.15f, 0.13f, 0.12f);
            chain3(limbs.rUpperArm, limbs.rLowerArm, limbs.rHand, top, 0.15f, 0.13f, 0.12f);
            chain3(limbs.lUpperLeg, limbs.lLowerLeg, limbs.lFoot, pelvis, 0.19f, 0.16f, 0.15f);
            chain3(limbs.rUpperLeg, limbs.rLowerLeg, limbs.rFoot, pelvis, 0.19f, 0.16f, 0.15f);
        }
        float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f, mnz = 1e9f, mxz = -1e9f;
        for (auto& p : jpts) {
            if (p.X < mnx)
                mnx = p.X;
            if (p.X > mxx)
                mxx = p.X;
            if (p.Y < mny)
                mny = p.Y;
            if (p.Y > mxy)
                mxy = p.Y;
            if (p.Z < mnz)
                mnz = p.Z;
            if (p.Z > mxz)
                mxz = p.Z;
        }
        float ex = mxx - mnx, ey = mxy - mny, ez = mxz - mnz;
        float span = ex > ey ? (ex > ez ? ex : ez) : (ey > ez ? ey : ez);
        if (!(span > 0.5f))
            span = 5.0f;
        const float ccx = (mnx + mxx) * 0.5f, ccy = (mny + mxy) * 0.5f, ccz = (mnz + mxz) * 0.5f;
        const float ns = 1.15f / span;
        struct LP {
            float x, y, d;
        };
        std::vector<LP> lproj;
        lproj.reserve(jpts.size());
        float bMinX = 1e9f, bMaxX = -1e9f, bMinY = 1e9f, bMaxY = -1e9f;
        for (auto& p : jpts) {
            float nx = (p.X - ccx) * ns, ny = (p.Y - ccy) * ns, nz = (p.Z - ccz) * ns;
            const float rx = nx * ca + nz * sa;
            const float rz = -nx * sa + nz * ca;
            const float ry = ny * cp + rz * sp;
            const float rz2 = -ny * sp + rz * cp;
            const float sx = cx + rx * scale;
            const float sy = cy - ry * scale;
            lproj.push_back({sx, sy, rz2});
            if (sx < bMinX)
                bMinX = sx;
            if (sx > bMaxX)
                bMaxX = sx;
            if (sy < bMinY)
                bMinY = sy;
            if (sy > bMaxY)
                bMaxY = sy;
        }
        if (variables::ESP::boxes && bMinX <= bMaxX) {
            Visuals::DrawBoxFill(dl, bMinX, bMinY, bMaxX, bMaxY);
            Visuals::DrawBoxCol(dl, bMinX, bMinY, bMaxX, bMaxY,
                                imGuiCustom::ColorU32(variables::ESP::boxColor));
        }
        // Solid-body render: each bone draws as an outlined slab so the
        // panel shows the actual model mass, not a stick skeleton.
        const ImU32 body = IM_COL32(232, 232, 238, 255);
        const ImU32 bodyDark = IM_COL32(178, 178, 188, 255);
        for (auto& b : bones) {
            if (b.a < 0 || b.b < 0 || (size_t)b.a >= lproj.size() || (size_t)b.b >= lproj.size())
                continue;
            const LP& a = lproj[(size_t)b.a];
            const LP& c = lproj[(size_t)b.b];
            ImVec2 p1(a.x, a.y), p2(c.x, c.y);
            float wpx = b.w * scale;
            if (wpx < 2.0f)
                wpx = 2.0f;
            if (wpx > 120.0f)
                wpx = 120.0f;
            dl->AddLine(p1, p2, IM_COL32(0, 0, 0, 255), wpx + 2.5f);
            dl->AddLine(p1, p2, bodyDark, wpx);
            dl->AddLine(p1, p2, body, (std::max)(1.0f, wpx - 2.0f));
        }
        if ((size_t)iHead < lproj.size()) {
            const LP& hd = lproj[(size_t)iHead];
            float hr = 0.15f * scale;
            if (hr < 4.0f)
                hr = 4.0f;
            if (hr > 60.0f)
                hr = 60.0f;
            dl->AddCircleFilled(ImVec2(hd.x, hd.y), hr + 1.5f, IM_COL32(0, 0, 0, 255), 20);
            dl->AddCircleFilled(ImVec2(hd.x, hd.y), hr, body, 20);
            dl->AddCircleFilled(ImVec2(hd.x, hd.y), hr * 0.55f, bodyDark, 20);
        }
        dl->PopClipRect();
        dl->AddRect(rmin, rmax, imGuiCustom::OutlineBlack(), 0.0f, 0, 1.0f);
        dl->AddRect(rmin + ImVec2(1.0f, 1.0f), rmax - ImVec2(1.0f, 1.0f),
                    imGuiCustom::OutlineInner(), 0.0f, 0, 1.0f);
        dl->AddRectFilled(pmin, pmin + ImVec2(kPanelW, 1.5f),
                          imGuiCustom::ColorU32(theme.Accent));
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    struct PT {
        float x, y, d;
    };
    static std::vector<PT> proj;
    proj.clear();
    if (proj.capacity() < g_verts.size())
        proj.reserve(g_verts.size());
    for (auto& w : g_verts) {
        const float rx = w.x * ca + w.z * sa;
        const float rz = -w.x * sa + w.z * ca;
        const float ry = w.y * cp + rz * sp;
        const float rz2 = -w.y * sp + rz * cp;
        proj.push_back({cx + rx * scale, cy - ry * scale, rz2});
    }

    struct RT {
        float d;
        std::uint32_t t;
    };
    static std::vector<RT> order;
    order.clear();
    const std::size_t ntris = g_idx.size() / 3;
    if (order.capacity() < ntris)
        order.reserve(ntris);
    for (std::size_t ti = 0; ti < ntris; ++ti) {
        const std::uint32_t i0 = g_idx[ti * 3 + 0];
        const std::uint32_t i1 = g_idx[ti * 3 + 1];
        const std::uint32_t i2 = g_idx[ti * 3 + 2];
        if (i0 >= proj.size() || i1 >= proj.size() || i2 >= proj.size())
            continue;
        const PT& a = proj[i0];
        const PT& b = proj[i1];
        const PT& c = proj[i2];
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (area > -0.01f && area < 0.01f)
            continue;
        order.push_back({(a.d + b.d + c.d) * 0.333333f, (std::uint32_t)ti});
    }

    std::sort(order.begin(), order.end(), [](const RT& a, const RT& b) { return a.d > b.d; });

    const bool useChams = variables::ESP::meshChams;
    const bool hasTex = (g_tex != nullptr) && !useChams;
    if (hasTex)
        dl->PushTexture((ImTextureID)g_tex);
    const ImVec2 whiteUv = ImGui::GetFontTexUvWhitePixel();

    for (auto& r : order) {
        const std::uint32_t i0 = g_idx[(std::size_t)r.t * 3 + 0];
        const std::uint32_t i1 = g_idx[(std::size_t)r.t * 3 + 1];
        const std::uint32_t i2 = g_idx[(std::size_t)r.t * 3 + 2];
        const Vert& v0 = g_verts[i0];
        const Vert& v1 = g_verts[i1];
        const Vert& v2 = g_verts[i2];
        const PT& a = proj[i0];
        const PT& b = proj[i1];
        const PT& c = proj[i2];

        const float nx = (v1.y - v0.y) * (v2.z - v0.z) - (v1.z - v0.z) * (v2.y - v0.y);
        const float ny = (v1.z - v0.z) * (v2.x - v0.x) - (v1.x - v0.x) * (v2.z - v0.z);
        const float nz = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
        float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        float lambert = 0.85f;
        if (nl > 1e-9f) {
            const float lx = nx / nl, ly = ny / nl, lz = nz / nl;
            const float lrx = lx * ca + lz * sa;
            const float lrz = -lx * sa + lz * ca;
            const float lry = ly * cp + lrz * sp;
            const float lrz2 = -ly * sp + lrz * cp;
            float ndl = lrx * 0.25f + lry * 0.45f + (-lrz2) * 0.75f;
            // Two-sided: FBX winding varies per export, so a facing-dependent
            // term shades half the model near-black. abs() keeps it lit.
            if (ndl < 0.0f)
                ndl = -ndl;
            if (ndl > 1.0f)
                ndl = 1.0f;
            lambert = 0.72f + 0.28f * ndl;
        }

        const bool triHasUV = (g_triUV[r.t] != 0 && hasTex);
        ImVec2 uv0 = whiteUv, uv1 = whiteUv, uv2 = whiteUv;
        ImU32 triCol0, triCol1, triCol2;

        if (useChams) {
            const ImVec4& cc = variables::ESP::chamsFillColor;
            const float cr = (std::min)(cc.x * lambert, 1.0f);
            const float cg = (std::min)(cc.y * lambert, 1.0f);
            const float cb = (std::min)(cc.z * lambert, 1.0f);
            const int caVal = (int)(cc.w * 255.0f);
            triCol0 = triCol1 = triCol2 = IM_COL32((int)(cr * 255.0f), (int)(cg * 255.0f), (int)(cb * 255.0f), caVal > 0 ? caVal : 255);
        } else if (triHasUV) {
            uv0 = ImVec2(v0.u, v0.v);
            uv1 = ImVec2(v1.u, v1.v);
            uv2 = ImVec2(v2.u, v2.v);
            const int lVal = (int)(std::clamp(lambert, 0.0f, 1.0f) * 255.0f);
            triCol0 = triCol1 = triCol2 = IM_COL32(lVal, lVal, lVal, 255);
        } else {
            const float lr = (std::min)((v0.cr + v1.cr + v2.cr) * 0.333333f * lambert + 0.08f, 1.0f);
            const float lg = (std::min)((v0.cg + v1.cg + v2.cg) * 0.333333f * lambert + 0.08f, 1.0f);
            const float lb = (std::min)((v0.cb + v1.cb + v2.cb) * 0.333333f * lambert + 0.08f, 1.0f);
            triCol0 = triCol1 = triCol2 = IM_COL32((int)(lr * 255.0f), (int)(lg * 255.0f), (int)(lb * 255.0f), 255);
        }

        dl->PrimReserve(3, 3);
        dl->PrimVtx(ImVec2(a.x, a.y), uv0, triCol0);
        dl->PrimVtx(ImVec2(b.x, b.y), uv1, triCol1);
        dl->PrimVtx(ImVec2(c.x, c.y), uv2, triCol2);
    }

    if (hasTex)
        dl->PopTexture();

    float bMinX = 1e9f, bMaxX = -1e9f, bMinY = 1e9f, bMaxY = -1e9f;
    for (const auto& pt : proj) {
        if (pt.x < bMinX) bMinX = pt.x;
        if (pt.x > bMaxX) bMaxX = pt.x;
        if (pt.y < bMinY) bMinY = pt.y;
        if (pt.y > bMaxY) bMaxY = pt.y;
    }
    const float pad = 4.0f;
    const float bx0 = bMinX - pad;
    const float bx1 = bMaxX + pad;
    const float by0 = bMinY - pad;
    const float by1 = bMaxY + pad;

    if (variables::ESP::boxes) {
        Visuals::DrawBoxFill(dl, bx0, by0, bx1, by1);
        if (variables::ESP::boxMode == 0) {
            Visuals::DrawBoxCol(dl, bx0, by0, bx1, by1, imGuiCustom::ColorU32(variables::ESP::boxColor));
        } else {
            const float cw = (bx1 - bx0) * 0.25f;
            const float ch = (by1 - by0) * 0.20f;
            const ImU32 col = imGuiCustom::ColorU32(variables::ESP::boxColor);
            auto drawCorner = [&](ImVec2 p1, ImVec2 corner, ImVec2 p2) {
                dl->AddLine(p1, corner, IM_COL32(0, 0, 0, 255), 3.0f);
                dl->AddLine(corner, p2, IM_COL32(0, 0, 0, 255), 3.0f);
                dl->AddLine(p1, corner, col, 1.5f);
                dl->AddLine(corner, p2, col, 1.5f);
            };
            drawCorner(ImVec2(bx0, by0 + ch), ImVec2(bx0, by0), ImVec2(bx0 + cw, by0));
            drawCorner(ImVec2(bx1 - cw, by0), ImVec2(bx1, by0), ImVec2(bx1, by0 + ch));
            drawCorner(ImVec2(bx0, by1 - ch), ImVec2(bx0, by1), ImVec2(bx0 + cw, by1));
            drawCorner(ImVec2(bx1 - cw, by1), ImVec2(bx1, by1), ImVec2(bx1, by1 - ch));
        }
    }

    if (variables::ESP::healthBar) {
        const float hx0 = std::floor(bx0 - 5.0f);
        const float hx1 = hx0 + 2.0f;
        Visuals::DrawHealthBar(dl, hx0, hx1, by0, by1, 1.0f, imGuiCustom::ColorU32(variables::ESP::healthColor));
    }

    if (variables::ESP::skeleton) {
        struct Joint { float x, y, z; };
        // FIX: joints were hardcoded for a 1.0-tall unit rig. Any real player.obj
        // (wide T-pose, big head, fallback avatar) made them float off the body
        // like in your screenshot. Fit them to the actual mesh AABB instead.
        // Joints from the model's width profile, not fixed height fractions.
        // Fixed fractions break on big heads / held items (arms spawn inside
        // the head, legs collapse into a box). We slice the mesh into rows,
        // find the head->shoulder width jump, the arm band and the leg split.
        float bMinX = 1e9f, bMaxX = -1e9f, bMinY = 1e9f, bMaxY = -1e9f;
        for (auto& w : g_verts) {
            if (w.x < bMinX) bMinX = w.x; if (w.x > bMaxX) bMaxX = w.x;
            if (w.y < bMinY) bMinY = w.y; if (w.y > bMaxY) bMaxY = w.y;
        }
        if (bMinX > bMaxX) { bMinX = -0.4f; bMaxX = 0.4f; bMinY = -0.6f; bMaxY = 0.6f; }
        const float bCx = (bMinX + bMaxX) * 0.5f;
        const float bTop = bMaxY, bBot = bMinY, bH = (bMaxY - bMinY) > 1e-6f ? (bMaxY - bMinY) : 1.0f;
        const float bW = (bMaxX - bMinX) > 1e-6f ? (bMaxX - bMinX) : 1.0f;
        constexpr int NB = 28;
        float binMin[NB], binMax[NB];
        int binCnt[NB] = {};
        for (int i = 0; i < NB; ++i) { binMin[i] = 1e9f; binMax[i] = -1e9f; }
        for (auto& w : g_verts) {
            int b = (int)((w.y - bMinY) / bH * (float)NB);
            if (b < 0) b = 0; if (b >= NB) b = NB - 1;
            if (w.x < binMin[b]) binMin[b] = w.x;
            if (w.x > binMax[b]) binMax[b] = w.x;
            binCnt[b]++;
        }
        auto rowW = [&](int i) -> float {
            if (i < 0) i = 0; if (i >= NB) i = NB - 1;
            return binCnt[i] ? (binMax[i] - binMin[i]) : 0.0f;
        };
        float sw[NB];
        for (int i = 0; i < NB; ++i) sw[i] = (rowW(i - 1) + rowW(i) + rowW(i + 1)) / 3.0f;
        float maxW = 0.0f;
        for (int i = 0; i < NB; ++i) if (sw[i] > maxW) maxW = sw[i];
        if (maxW <= 1e-6f) maxW = bW;
        const float rowH = bH / (float)NB;
        auto rowY = [&](int i) { return bMinY + (float)i * rowH; };
        // head bottom: first wide row scanning down from the top
        int hb = NB - 1;
        while (hb > 0 && sw[hb] < 0.62f * maxW) --hb;
        const float headBotY = rowY(hb + 1);
        // head center: mean of verts above the neck line
        double hx = 0, hy = 0; int hn = 0;
        for (auto& w : g_verts) if (w.y >= headBotY) { hx += w.x; hy += w.y; ++hn; }
        const float headX = hn ? (float)(hx / hn) : bCx;
        const float headY = hn ? (float)(hy / hn) : bTop - bH * 0.07f;
        const float neckY = headBotY - bH * 0.015f;
        const float shoulderY = headBotY - bH * 0.055f;
        const float torsoW = sw[hb] > 1e-6f ? sw[hb] : maxW;
        const float shX = torsoW * 0.40f;
        // arms: wide band below the shoulders (T-pose hands stick out)
        int armTop = -1, armBot = -1;
        for (int i = hb - 1; i >= 0 && rowY(i) > bMinY + bH * 0.30f; --i) {
            if (sw[i] > torsoW * 1.12f) { if (armTop < 0) armTop = i; armBot = i; }
        }
        float handLX = bCx - torsoW * 0.5f, handRX = bCx + torsoW * 0.5f, handY = shoulderY - bH * 0.16f;
        bool haveHands = false;
        if (armTop >= 0) {
            float mn = 1e9f, mx = -1e9f, ys = 0; int yn = 0;
            for (auto& w : g_verts) {
                if (w.y <= rowY(armBot) || w.y >= rowY(armTop + 1)) continue;
                if (w.x < mn) mn = w.x; if (w.x > mx) mx = w.x; ys += w.y; ++yn;
            }
            if (yn > 4 && mx > mn) { handLX = mn; handRX = mx; handY = ys / yn; haveHands = true; }
        }
        // legs: bottom-up cluster split (one blob -> two legs)
        int crotch = -1;
        for (int i = 0; i < NB && rowY(i) < bMinY + bH * 0.55f; ++i) {
            int L = 0, R = 0, M = 0;
            for (auto& w : g_verts) {
                if (w.y < rowY(i) || w.y >= rowY(i + 1)) continue;
                if (w.x < bCx - bW * 0.06f) ++L;
                else if (w.x > bCx + bW * 0.06f) ++R;
                else ++M;
            }
            if (L >= 4 && R >= 4 && M < (L < R ? L : R)) crotch = i;
            else if (crotch >= 0) break;
        }
        float hipY, kneeY, ankleY, hipLX, hipRX;
        if (crotch >= 0) {
            hipY = rowY(crotch + 1) + bH * 0.02f;
            double lx = 0, rx = 0; int ln = 0, rn = 0;
            for (auto& w : g_verts) {
                if (w.y >= rowY(crotch + 1)) continue;
                if (w.x < bCx) { lx += w.x; ++ln; } else { rx += w.x; ++rn; }
            }
            hipLX = ln ? (float)(lx / ln) : bCx - bW * 0.10f;
            hipRX = rn ? (float)(rx / rn) : bCx + bW * 0.10f;
            kneeY = bBot + (hipY - bBot) * 0.52f;
            ankleY = bBot + (hipY - bBot) * 0.06f;
        } else {
            hipY = bBot + bH * 0.42f;
            kneeY = bBot + bH * 0.22f;
            ankleY = bBot + bH * 0.03f;
            hipLX = bCx - (std::min)(bW * 0.14f, 0.14f);
            hipRX = bCx + (std::min)(bW * 0.14f, 0.14f);
        }
        const float elbLX = haveHands ? (bCx - shX + handLX) * 0.5f : bCx - shX * 1.05f;
        const float elbRX = haveHands ? (bCx + shX + handRX) * 0.5f : bCx + shX * 1.05f;
        const float elbY = haveHands ? (shoulderY + handY) * 0.5f : shoulderY - bH * 0.13f;
        const float wriLX = haveHands ? handLX : bCx - shX * 1.05f;
        const float wriRX = haveHands ? handRX : bCx + shX * 1.05f;
        const float wriY = haveHands ? handY : shoulderY - bH * 0.25f;
        const Joint jHead   = { headX,      headY,     0.00f };
        const Joint jNeck   = { headX,      neckY,     0.00f };
        const Joint jPelvis = { bCx,        hipY + bH * 0.01f, 0.00f };
        const Joint jLSh    = { bCx - shX,  shoulderY, 0.00f };
        const Joint jLElb   = { elbLX,      elbY,      0.00f };
        const Joint jLWrist = { wriLX,      wriY,      0.00f };
        const Joint jRSh    = { bCx + shX,  shoulderY, 0.00f };
        const Joint jRElb   = { elbRX,      elbY,      0.00f };
        const Joint jRWrist = { wriRX,      wriY,      0.00f };
        const Joint jLHip   = { hipLX,      hipY,      0.00f };
        const Joint jLKnee  = { hipLX,      kneeY,     0.00f };
        const Joint jLAnkle = { hipLX,      ankleY,    0.00f };
        const Joint jRHip   = { hipRX,      hipY,      0.00f };
        const Joint jRKnee  = { hipRX,      kneeY,     0.00f };
        const Joint jRAnkle = { hipRX,      ankleY,    0.00f };

        auto projJoint = [&](const Joint& j) -> ImVec2 {
            const float rx = j.x * ca + j.z * sa;
            const float rz = -j.x * sa + j.z * ca;
            const float ry = j.y * cp + rz * sp;
            return ImVec2(cx + rx * scale, cy - ry * scale);
        };

        auto drawBone = [&](const Joint& j1, const Joint& j2) {
            ImVec2 p1 = projJoint(j1);
            ImVec2 p2 = projJoint(j2);
            if (variables::ESP::skeletonOutline)
                dl->AddLine(p1, p2, IM_COL32(0, 0, 0, 255), variables::ESP::skeletonThickness + 1.0f);
            dl->AddLine(p1, p2, imGuiCustom::ColorU32(variables::ESP::skeletonColor), variables::ESP::skeletonThickness);
        };

        drawBone(jHead, jNeck);
        drawBone(jNeck, jPelvis);
        drawBone(jNeck, jLSh);
        drawBone(jLSh, jLElb);
        drawBone(jLElb, jLWrist);
        drawBone(jNeck, jRSh);
        drawBone(jRSh, jRElb);
        drawBone(jRElb, jRWrist);
        drawBone(jPelvis, jLHip);
        drawBone(jLHip, jLKnee);
        drawBone(jLKnee, jLAnkle);
        drawBone(jPelvis, jRHip);
        drawBone(jRHip, jRKnee);
        drawBone(jRKnee, jRAnkle);
    }

    // Fitted head Y shared by dot + view line (same AABB fit as skeleton).
    float fitHeadX = 0.0f, fitHeadY = 0.38f;
    {
        float fMinX = 1e9f, fMaxX = -1e9f, fMinY = 1e9f, fMaxY = -1e9f;
        for (auto& w : g_verts) {
            if (w.x < fMinX) fMinX = w.x; if (w.x > fMaxX) fMaxX = w.x;
            if (w.y < fMinY) fMinY = w.y; if (w.y > fMaxY) fMaxY = w.y;
        }
        if (fMinX <= fMaxX && fMinY <= fMaxY) {
            fitHeadX = (fMinX + fMaxX) * 0.5f;
            fitHeadY = fMaxY - (fMaxY - fMinY) * 0.07f;
        }
    }

    if (variables::ESP::headDot) {
        const float rx = fitHeadX * ca + 0.00f * sa;
        const float rz = -fitHeadX * sa + 0.00f * ca;
        const float ry = fitHeadY * cp + rz * sp;
        const ImVec2 hp = ImVec2(cx + rx * scale, cy - ry * scale);
        const float r = (std::max)(2.0f, variables::ESP::headDotSize);
        dl->AddCircleFilled(hp, r, imGuiCustom::ColorU32(variables::ESP::headDotColor), 16);
        dl->AddCircle(hp, r + 1.0f, IM_COL32(0, 0, 0, 200), 16, 1.0f);
    }

    if (variables::ESP::viewDirection) {
        const float rx = fitHeadX * ca + 0.00f * sa;
        const float rz = -fitHeadX * sa + 0.00f * ca;
        const float ry = fitHeadY * cp + rz * sp;
        const ImVec2 hp = ImVec2(cx + rx * scale, cy - ry * scale);

        const float lookX = 0.0f, lookY = 0.0f, lookZ = -0.45f;
        const float lrx = lookX * ca + lookZ * sa;
        const float lrz = -lookX * sa + lookZ * ca;
        const float lry = lookY * cp + lrz * sp;
        const ImVec2 endP = ImVec2(hp.x + lrx * scale, hp.y - lry * scale);

        const ImU32 vdCol = imGuiCustom::ColorU32(variables::ESP::viewDirColor);
        dl->AddLine(hp, endP, IM_COL32(0, 0, 0, 255), 3.0f);
        dl->AddLine(hp, endP, vdCol, 2.0f);
        dl->AddCircleFilled(endP, 2.5f, vdCol, 8);
    }

    if (variables::ESP::names) {
        ImFont* espF = Visuals::EspFont();
        const float espSz = variables::Misc::espFontSize;
        const ImVec2 ts = espF->CalcTextSizeA(espSz, FLT_MAX, 0.0f, "Player");
        Visuals::DrawOutlinedText(dl, ImVec2((bx0 + bx1) * 0.5f - ts.x * 0.5f, by0 - ts.y - 2.0f), "Player", imGuiCustom::ColorU32(variables::ESP::nameColor));
    }

    if (variables::ESP::distance) {
        ImFont* espF = Visuals::EspFont();
        const float espSz = variables::Misc::espFontSize;
        const ImVec2 ts = espF->CalcTextSizeA(espSz, FLT_MAX, 0.0f, "15m");
        Visuals::DrawOutlinedText(dl, ImVec2((bx0 + bx1) * 0.5f - ts.x * 0.5f, by1 + 2.0f), "15m", imGuiCustom::ColorU32(variables::ESP::distanceColor));
    }

    if (variables::ESP::tool) {
        ImFont* espF = Visuals::EspFont();
        const float espSz = variables::Misc::espFontSize;
        const ImVec2 ts = espF->CalcTextSizeA(espSz, FLT_MAX, 0.0f, "Sword");
        float ty = by1 + 2.0f;
        if (variables::ESP::distance)
            ty += espSz + 3.0f;
        Visuals::DrawOutlinedText(dl, ImVec2((bx0 + bx1) * 0.5f - ts.x * 0.5f, ty), "Sword", imGuiCustom::ColorU32(variables::ESP::toolColor));
    }

    if (variables::ESP::flags) {
        const ImU32 flagsCol = imGuiCustom::ColorU32(variables::ESP::flagsColor);
        const float espSz = variables::Misc::espFontSize;
        float fx = bx1 + 4.0f;
        float fy = by0;
        auto flagText = [&](const std::string& t) {
            Visuals::DrawOutlinedText(dl, ImVec2(fx, fy), t, flagsCol);
            fy += espSz + 2.0f;
        };
        if (variables::ESP::flagSel[0]) flagText("Idle");
        if (variables::ESP::flagSel[1]) flagText("R6");
        if (variables::ESP::flagSel[2]) flagText("100% HP");
        if (variables::ESP::flagSel[3] && variables::ESP::tool) flagText("Sword");
        if (variables::ESP::flagSel[4]) flagText("15m");
    }

    dl->PopClipRect();

    dl->AddRect(rmin, rmax, imGuiCustom::OutlineBlack(), 0.0f, 0, 1.0f);
    dl->AddRect(rmin + ImVec2(1.0f, 1.0f), rmax - ImVec2(1.0f, 1.0f), imGuiCustom::OutlineInner(), 0.0f, 0, 1.0f);

    dl->AddRectFilled(pmin, pmin + ImVec2(kPanelW, 1.5f), imGuiCustom::ColorU32(theme.Accent));

    ImGui::End();
    ImGui::PopStyleVar();
}

}
