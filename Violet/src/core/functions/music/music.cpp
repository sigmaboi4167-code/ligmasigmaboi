// Violet music deck — system media (Spotify first) + LRCLIB synced lyrics.
//
// Real C++/WinRT consumption (SDK's cppwinrt headers): session manager,
// polled snapshots, fire-and-forget transport. A dead media service only
// ever trips a caught hresult — the worker thread cannot die.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "music.h"
#include "../../variables/variables.h"
#include "../../../render/menu/library.h"
#include "../../../memory/memory.h"

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winhttp.h>
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")


#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>




// stb_image is implemented in preview.cpp as static; this TU gets its own
// global copy for cover decoding (internal + external linkage coexist fine).
#define STB_IMAGE_IMPLEMENTATION
#include "../../../../ext/stb/stb_image.h"

namespace Music {
namespace {

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using winrt::Windows::Storage::Streams::Buffer;
using winrt::Windows::Storage::Streams::InputStreamOptions;

// ---- snapshot (worker writes, panel reads) ----
struct Snap {
    bool hasSession = false;
    bool playing = false;
    std::string title, artist, album;
    long long posMs = 0, durMs = 0;
    bool canSeek = false;
};
static Snap g_snap;
static std::mutex g_snapMtx;

struct LyricLine {
    long long ms = 0;
    std::string text;
};
static std::vector<LyricLine> g_lyrics;
static std::string g_lyricKey; // title\1artist — refetch on change
static std::mutex g_lyrMtx;

static ID3D11ShaderResourceView* g_art = nullptr;
static int g_artW = 0, g_artH = 0;
static std::string g_artKey;
static std::mutex g_artMtx;
static ID3D11Device* g_dev = nullptr;

static std::atomic<bool> g_run{false};
static std::thread g_th;
static long long g_lastManualMs = 0; // lyric click override (5s follow delay)

// Transport requests from the panel thread; the worker owns all COM.
static std::atomic<int> g_cmd{0}; // 1 play, 2 pause, 3 next, 4 prev
static std::atomic<long long> g_seekReq{-1};

static GlobalSystemMediaTransportControlsSessionManager g_mgr{nullptr};
static GlobalSystemMediaTransportControlsSession g_sess{nullptr};

inline void DropArt() {
    std::lock_guard<std::mutex> lk(g_artMtx);
    if (g_art) {
        g_art->Release();
        g_art = nullptr;
    }
    g_artW = g_artH = 0;
    g_artKey.clear();
}

inline bool ContainsCi(const std::wstring& hay, const wchar_t* needle) {
    if (hay.empty() || !needle || !*needle)
        return false;
    size_t nl = wcslen(needle);
    if (nl > hay.size())
        return false;
    for (size_t i = 0; i + nl <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < nl; ++j) {
            if (towupper(hay[i + j]) != towupper(needle[j]))
                break;
        }
        if (j == nl)
            return true;
    }
    return false;
}

// ---- tiny https GET over WinHTTP ----
static std::string HttpsGet(const std::wstring& host, const std::wstring& path) {
    std::string out;
    HINTERNET hS = WinHttpOpen(L"violet.lol/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hS)
        return out;
    DWORD to = 8000;
    WinHttpSetOption(hS, WINHTTP_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    WinHttpSetOption(hS, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
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
    if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hR, nullptr)) {
        std::vector<char> buf;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hR, &avail) || !avail)
                break;
            size_t old = buf.size();
            buf.resize(old + avail);
            DWORD rd = 0;
            if (!WinHttpReadData(hR, buf.data() + old, avail, &rd))
                break;
            buf.resize(old + rd);
            if (!rd)
                break;
        }
        out.assign(buf.begin(), buf.end());
    }
    WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
    return out;
}

static std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
            o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// LRCLIB synced-lyrics fetch: "[mm:ss.xx] line" -> ms + text.
static void FetchLyrics(const std::string& artist, const std::string& title, const std::string& album) {
    std::vector<LyricLine> lines;
    auto enc = [](const std::string& s) {
        std::string e = UrlEncode(s);
        return std::wstring(e.begin(), e.end());
    };
    std::wstring path = L"/api/get?artist_name=" + enc(artist) + L"&track_name=" + enc(title);
    if (!album.empty())
        path += L"&album_name=" + enc(album);
    std::string body = HttpsGet(L"lrclib.net", path);
    auto p = body.find("syncedLyrics");
    if (p != std::string::npos) {
        auto q = body.find('"', body.find(':', p) + 1);
        std::string raw;
        if (q != std::string::npos) {
            for (size_t i = q + 1; i < body.size(); ++i) {
                char c = body[i];
                if (c == '\\' && i + 1 < body.size()) {
                    char n = body[i + 1];
                    if (n == 'n') {
                        raw += '\n';
                        i++;
                    } else if (n == '"') {
                        raw += '"';
                        i++;
                    } else if (n == 'r') {
                        i++;
                    } else {
                        raw += c;
                    }
                } else if (c == '"') {
                    break;
                } else {
                    raw += c;
                }
            }
        }
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t nl = raw.find('\n', pos);
            std::string ln = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (!ln.empty() && ln[0] == '[') {
                auto rb = ln.find(']');
                int mm = 0;
                float ss = 0;
                if (rb != std::string::npos && sscanf_s(ln.c_str() + 1, "%d:%f", &mm, &ss) == 2) {
                    LyricLine l;
                    l.ms = (long long)mm * 60000LL + (long long)(ss * 1000.0f);
                    l.text = ln.substr(rb + 1);
                    while (!l.text.empty() && (l.text[0] == ' ' || l.text[0] == '\t'))
                        l.text.erase(l.text.begin());
                    if (!l.text.empty())
                        lines.push_back(l);
                }
            }
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
    }
    std::lock_guard<std::mutex> lk(g_lyrMtx);
    g_lyrics = std::move(lines);
}

static bool UploadArt(const unsigned char* data, size_t len, const std::string& key) {
    if (!data || !len || len > 16 * 1024 * 1024)
        return false;
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
    ID3D11Device* dev = g_dev;
    if (!px || w <= 0 || h <= 0 || !dev) {
        if (px)
            stbi_image_free(px);
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = px;
    sd.SysMemPitch = (UINT)(w * 4);
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, &sd, &tex))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = d.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        if (SUCCEEDED(dev->CreateShaderResourceView(tex, &vd, &srv))) {
            std::lock_guard<std::mutex> lk(g_artMtx);
            if (g_art)
                g_art->Release();
            g_art = srv;
            g_artW = w;
            g_artH = h;
            g_artKey = key;
        }
        if (tex)
            tex->Release();
    }
    stbi_image_free(px);
    return srv != nullptr;
}

// GSMTC thumbnail stream -> bytes. No art == no failure, just empty.
static std::vector<unsigned char> PullArtBytes(GlobalSystemMediaTransportControlsSession s) {
    std::vector<unsigned char> out;
    try {
        auto props = s.TryGetMediaPropertiesAsync().get();
        if (!props)
            return out;
        auto ref = props.Thumbnail();
        if (!ref)
            return out;
        auto stream = ref.OpenReadAsync().get();
        if (!stream)
            return out;
        uint64_t total = stream.Size();
        if (!total || total > 16 * 1024 * 1024)
            return out;
        Buffer buf((uint32_t)total);
        auto got = stream.ReadAsync(buf, (uint32_t)total, winrt::Windows::Storage::Streams::InputStreamOptions::None).get();
        uint32_t n = got.Length();
        if (!n)
            return out;
        uint8_t* p = got.data();
        if (p)
            out.assign(p, p + n);
    } catch (...) {
    }
    return out;
}

static void Worker() {
    winrt::init_apartment();
    try {
        g_mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    } catch (...) {
        g_mgr = nullptr;
    }
    std::string lastKey;
    while (g_run.load()) {
        try {
            if (!g_mgr) {
                try {
                    g_mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                } catch (...) {
                }
            }
            GlobalSystemMediaTransportControlsSession s{nullptr};
            if (g_mgr) {
                // Spotify Desktop first, else current.
                try {
                    auto sessions = g_mgr.GetSessions();
                    for (auto cand : sessions) {
                        if (!s) {
                            s = cand;
                            continue;
                        }
                        try {
                            if (ContainsCi(cand.SourceAppUserModelId().c_str(), L"SPOTIFY")) {
                                s = cand;
                                break;
                            }
                        } catch (...) {
                        }
                    }
                } catch (...) {
                }
                if (!s) {
                    try {
                        s = g_mgr.GetCurrentSession();
                    } catch (...) {
                    }
                }
            }
            Snap snap;
            if (s) {
                snap.hasSession = true;
                try {
                    snap.playing = (s.GetPlaybackInfo().PlaybackStatus() ==
                                    GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                } catch (...) {
                }
                try {
                    auto mp = s.TryGetMediaPropertiesAsync().get();
                    if (mp) {
                        snap.title = winrt::to_string(mp.Title());
                        snap.artist = winrt::to_string(mp.Artist());
                        snap.album = winrt::to_string(mp.AlbumTitle());
                    }
                } catch (...) {
                }
                try {
                    auto tl = s.GetTimelineProperties();
                    snap.posMs = (long long)(tl.Position().count() / 10000LL);
                    snap.durMs = (long long)(tl.EndTime().count() / 10000LL);
                    snap.canSeek = (tl.MaxSeekTime().count() > tl.MinSeekTime().count());
                } catch (...) {
                }
                // Drained panel-thread requests (transport + seek).
                int cmd = g_cmd.exchange(0);
                if (cmd) {
                    try {
                        if (cmd == 1)
                            s.TryPlayAsync().get();
                        else if (cmd == 2)
                            s.TryPauseAsync().get();
                        else if (cmd == 3)
                            s.TrySkipNextAsync().get();
                        else if (cmd == 4)
                            s.TrySkipPreviousAsync().get();
                    } catch (...) {
                    }
                }
                long long wantSeek = g_seekReq.exchange(-1);
                if (wantSeek >= 0 && snap.canSeek) {
                    try {
                        s.TryChangePlaybackPositionAsync(wantSeek * 10000LL).get();
                    } catch (...) {
                    }
                }
                std::string key = snap.title + "\x01" + snap.artist;
                if (!snap.title.empty() && key != lastKey) {
                    lastKey = key;
                    DropArt();
                    {
                        std::lock_guard<std::mutex> lk(g_lyrMtx);
                        g_lyrics.clear();
                        g_lyricKey = key;
                    }
                    auto art = PullArtBytes(s);
                    if (!art.empty())
                        UploadArt(art.data(), art.size(), key);
                    if (!snap.artist.empty())
                        FetchLyrics(snap.artist, snap.title, snap.album);
                }
            } else if (!lastKey.empty()) {
                lastKey.clear();
            }
            {
                std::lock_guard<std::mutex> lk(g_snapMtx);
                g_snap = snap;
            }
        } catch (...) {
        }
        for (int i = 0; i < 10 && g_run.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    g_mgr = nullptr;
    g_sess = nullptr;
}

} // namespace

void Loop() {
    bool expected = false;
    if (!g_run.compare_exchange_strong(expected, true))
        return;
    std::thread(Worker).detach();
}

void Stop() {
    g_run.store(false);
}

static std::string FmtTime(long long ms) {
    if (ms < 0)
        ms = 0;
    long long s = ms / 1000;
    char b[16];
    std::snprintf(b, sizeof(b), "%lld:%02lld", s / 60, s % 60);
    return b;
}

static void Transport(int what) {
    g_cmd.store(what);
}

static void SeekTo(long long ms) {
    if (ms < 0)
        ms = 0;
    g_seekReq.store(ms);
}

void DrawPanel(ID3D11Device* dev) {
    if (dev)
        g_dev = dev;
    if (!variables::Music::enabled)
        return;
    Snap snap;
    {
        std::lock_guard<std::mutex> lk(g_snapMtx);
        snap = g_snap;
    }
    const imGuiCustom::Theme& theme = imGuiCustom::GetTheme();
    ImFont* font = imGuiCustom::GetFonts().CascadiaMonoBL
                       ? imGuiCustom::GetFonts().CascadiaMonoBL
                       : ImGui::GetFont();
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, imGuiCustom::ColorU32(theme.CardBg));
    ImGui::PushStyleColor(ImGuiCol_Border, imGuiCustom::ColorU32(ImVec4(0, 0, 0, 0)));
    ImGui::PushStyleColor(ImGuiCol_Text, imGuiCustom::ColorU32(theme.TextBright));
    ImGui::PushStyleColor(ImGuiCol_Button, imGuiCustom::ColorU32(theme.ControlBg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, imGuiCustom::ColorU32(theme.ControlInactive));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, imGuiCustom::ColorU32(theme.Accent));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, imGuiCustom::ColorU32(theme.ControlBg));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, imGuiCustom::ColorU32(theme.Accent));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, imGuiCustom::ColorU32(theme.Accent));
    ImGui::PushStyleColor(ImGuiCol_Header, imGuiCustom::ColorU32(theme.Accent, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, imGuiCustom::ColorU32(theme.Accent, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, imGuiCustom::ColorU32(theme.Accent, 0.7f));

    const float PW = 300.0f;
    const float PH = variables::Music::showLyrics ? 380.0f : 168.0f;
    ImGui::SetNextWindowPos(ImVec2(80.0f, 480.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(PW, PH), ImGuiCond_Always);
    const bool open =
        ImGui::Begin("##music", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos();
        dl->AddRectFilled(origin, origin + ImVec2(PW, 2.0f), imGuiCustom::ColorU32(theme.Accent));
        if (!snap.hasSession || snap.title.empty()) {
            ImGui::SetCursorPos(ImVec2(12.0f, 60.0f));
            ImGui::TextUnformatted("no media playing");
            ImGui::SetCursorPos(ImVec2(12.0f, 80.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, imGuiCustom::ColorU32(theme.Text));
            ImGui::TextUnformatted("start Spotify, boss man");
            ImGui::PopStyleColor();
        } else {
            // cover
            ID3D11ShaderResourceView* art = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_artMtx);
                art = g_art;
            }
            ImVec2 cur = ImGui::GetCursorPos();
            if (art)
                ImGui::Image((ImTextureID)art, ImVec2(84.0f, 84.0f));
            else {
                ImVec2 p = ImGui::GetWindowPos() + cur;
                dl->AddRectFilled(p, p + ImVec2(84.0f, 84.0f),
                                  imGuiCustom::ColorU32(theme.ControlBg));
                dl->AddRect(p, p + ImVec2(84.0f, 84.0f), imGuiCustom::OutlineBlack());
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 190.0f);
            ImGui::TextUnformatted(snap.title.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, imGuiCustom::ColorU32(theme.Text));
            ImGui::TextUnformatted(snap.artist.empty() ? snap.album.c_str() : snap.artist.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            // seek
            static float seekFrac = 0.0f;
            float frac = (snap.durMs > 0) ? (float)snap.posMs / (float)snap.durMs : 0.0f;
            if (frac < 0)
                frac = 0;
            if (frac > 1)
                frac = 1;
            ImGui::PushItemWidth(276.0f);
            if (!ImGui::IsItemActive())
                seekFrac = frac;
            ImGui::SliderFloat("##seek", &seekFrac, 0.0f, 1.0f, "");
            if (ImGui::IsItemDeactivatedAfterEdit() && snap.durMs > 0)
                SeekTo((long long)(seekFrac * (float)snap.durMs));
            ImGui::PopItemWidth();
            ImGui::TextUnformatted(FmtTime(snap.posMs).c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(PW - 48.0f);
            ImGui::TextUnformatted(FmtTime(snap.durMs).c_str());
            // transport
            const float bw = 88.0f;
            if (ImGui::Button("|<", ImVec2(bw, 22.0f)))
                Transport(4);
            ImGui::SameLine();
            if (ImGui::Button(snap.playing ? "||" : ">", ImVec2(bw, 22.0f)))
                Transport(snap.playing ? 2 : 1);
            ImGui::SameLine();
            if (ImGui::Button(">|", ImVec2(bw, 22.0f)))
                Transport(3);
            ImGui::SameLine();
            bool wantLyr = variables::Music::showLyrics;
            if (ImGui::Button(wantLyr ? "Aa*" : "Aa", ImVec2(0.0f, 22.0f)))
                variables::Music::showLyrics = !wantLyr;
            // lyrics
            if (variables::Music::showLyrics) {
                std::vector<LyricLine> lines;
                {
                    std::lock_guard<std::mutex> lk(g_lyrMtx);
                    lines = g_lyrics;
                }
                ImGui::BeginChild("##lyr", ImVec2(0.0f, 0.0f), true);
                if (lines.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, imGuiCustom::ColorU32(theme.Text));
                    ImGui::TextUnformatted("no synced lyrics for this one");
                    ImGui::PopStyleColor();
                } else {
                    int active = -1;
                    for (size_t i = 0; i < lines.size(); ++i)
                        if (lines[i].ms <= snap.posMs)
                            active = (int)i;
                    unsigned long long nowMs = GetTickCount64();
                    bool follow = (nowMs - (unsigned long long)g_lastManualMs) > 5000;
                    for (size_t i = 0; i < lines.size(); ++i) {
                        bool sel = ((int)i == active);
                        if (sel)
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  imGuiCustom::ColorU32(theme.Accent));
                        if (ImGui::Selectable(lines[i].text.c_str(), sel)) {
                            SeekTo(lines[i].ms);
                            g_lastManualMs = (long long)GetTickCount64();
                        }
                        if (sel) {
                            ImGui::PopStyleColor();
                            if (follow)
                                ImGui::SetScrollHereY(0.5f);
                        }
                    }
                }
                ImGui::EndChild();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(11);
    ImGui::PopFont();
}

} // namespace Music
