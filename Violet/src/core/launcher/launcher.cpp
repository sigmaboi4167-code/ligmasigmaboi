#include "launcher.h"
#include "../updater/updater.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include "../../render/menu/library.h"
extern unsigned char CascadiaMonoBL[];
constexpr int kCascadiaMonoBLSize = 290368;
#include "../../../ext/imgui/imgui.h"
#include "../../../ext/imgui/imgui_impl_win32.h"
#include "../../../ext/imgui/imgui_impl_dx11.h"
#include <windows.h>
#include <d3d11.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Launcher {
namespace {

LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

struct D3D {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    bool init(HWND hwnd, int w, int h) {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = (UINT)w;
        sd.BufferDesc.Height = (UINT)h;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL lvls[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                lvls, 2, D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx)))
            return false;
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) return false;
        dev->CreateRenderTargetView(bb, nullptr, &rtv);
        bb->Release();
        return rtv != nullptr;
    }
    void free() {
        if (rtv) { rtv->Release(); rtv = nullptr; }
        if (swap) { swap->Release(); swap = nullptr; }
        if (ctx) { ctx->Release(); ctx = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
    }
};

enum class Phase { Checking, Ready, NoUpdate, HasUpdate, Downloading, Done, Error };

struct Shared {
    std::mutex mtx;
    std::string remote, notes, status = "contacting github...";
    std::vector<char> exe;
    std::atomic<float> progress{0.0f};
    std::atomic<bool> dlDone{false};
    std::atomic<bool> dlOk{false};
};

// Card panel exactly like the menu: CardBg fill + black outline + inner line.
void Panel(ImDrawList* draw, const ImVec2& origin, const ImVec2& pos, const ImVec2& size) {
    const auto& theme = imGuiCustom::GetTheme();
    const ImVec2 min(std::floor(origin.x + pos.x), std::floor(origin.y + pos.y));
    const ImVec2 max(min.x + std::floor(size.x), min.y + std::floor(size.y));
    draw->AddRectFilled(min, max, imGuiCustom::ColorU32(theme.CardBg), 0.0f);
    draw->AddRect(min, max, imGuiCustom::OutlineBlack(), 0.0f, 0, 1.0f);
    draw->AddRect(min + ImVec2(1.0f, 1.0f), max - ImVec2(1.0f, 1.0f), imGuiCustom::OutlineInner(), 0.0f, 0, 1.0f);
}

void ShimmerTitle(ImDrawList* draw, const imGuiCustom::Fonts& fonts, const ImVec2& origin, float width) {
    const auto& theme = imGuiCustom::GetTheme();
    ImFont* font = fonts.CascadiaMonoBL ? fonts.CascadiaMonoBL : ImGui::GetFont();
    const char* title = "violet.lol";
    const float fs = 12.0f * imGuiCustom::g_fontScale;
    const ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, title);
    float cx = origin.x + std::floor((width - tsz.x) * 0.5f);
    const float ty = origin.y + 3.5f;
    const float now = (float)ImGui::GetTime();
    for (const char* p = title; *p; ++p) {
        char ch[2] = {*p, 0};
        const ImVec2 cs = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, ch);
        const float wave = 0.5f + 0.5f * sinf(now * 2.5f - (cx - origin.x) * 0.045f);
        draw->AddText(font, fs, ImVec2(cx, ty),
            imGuiCustom::ColorU32(imGuiCustom::LerpColor(theme.TextBright, theme.Accent, wave * wave)), ch);
        cx += cs.x;
    }
}

void MenuButton(const char* label, const ImVec2& origin, const ImVec2& pos, const ImVec2& size, bool& out) {
    const ImVec2 min(std::floor(origin.x + pos.x), std::floor(origin.y + pos.y));
    ImGui::PushID(label);
    ImGui::SetCursorScreenPos(min);
    const bool pressed = ImGui::InvisibleButton("##lbtn", size);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiID id = ImGui::GetItemID();
    const float hov = imGuiCustom::AnimateFloat(id, hovered, 14.0f);
    const auto& theme = imGuiCustom::GetTheme();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, min + size,
        imGuiCustom::ColorU32(imGuiCustom::LerpColor(theme.ControlBg, theme.ControlInactive, hov * 0.5f)), 0.0f);
    draw->AddRect(min, min + size, imGuiCustom::OutlineBlack(), 0.0f, 0, 1.0f);
    draw->AddRect(min + ImVec2(1.0f, 1.0f), min + size - ImVec2(1.0f, 1.0f), imGuiCustom::OutlineInner(), 0.0f, 0, 1.0f);
    ImFont* font = theme.Accent.x >= 0 && imGuiCustom::GetFonts().CascadiaMonoBL
        ? imGuiCustom::GetFonts().CascadiaMonoBL : ImGui::GetFont();
    const float fs = 12.5f * imGuiCustom::g_fontScale;
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
    draw->AddText(font, fs,
        ImVec2(std::floor(min.x + (size.x - ts.x) * 0.5f), std::floor(min.y + (size.y - ts.y) * 0.5f)),
        imGuiCustom::ColorU32(imGuiCustom::LerpColor(theme.Text, theme.TextBright, hov)), label);
    ImGui::PopID();
    out = pressed;
}

void AccentBar(ImDrawList* draw, const ImVec2& origin, float w, float pillX, float pillW) {
    const float pulse = 0.70f + 0.30f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 2.2f));
    ImVec4 bar = imGuiCustom::GetTheme().Accent;
    bar.w *= pulse;
    imGuiCustom::AddGlowRect(draw, origin, origin + ImVec2(w, 3.0f), imGuiCustom::ColorU32(bar), 0.9f, 6, 10.0f, 0.0f);
    draw->AddRectFilled(origin, origin + ImVec2(w, 3.0f), imGuiCustom::ColorU32(bar), 0.0f);
    if (pillW > 0.0f)
        draw->AddRectFilled(ImVec2(std::floor(origin.x + pillX - pillW), std::floor(origin.y + 3.0f)),
                            ImVec2(std::floor(origin.x + pillX + pillW), std::floor(origin.y + 5.0f)),
                            imGuiCustom::ColorU32(imGuiCustom::GetTheme().Accent), 0.0f);
}

} // namespace

bool Run() {
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);

    constexpr float PW = 601.0f, PH = 460.0f;
    const int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                   GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"violet.launcher", nullptr};
    RegisterClassExW(&wc);
    RECT rc{0, 0, (LONG)PW, (LONG)PH};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"violet.lol", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sx - (rc.right - rc.left)) / 2, (sy - (rc.bottom - rc.top)) / 2,
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return true;

    D3D d3d;
    if (!d3d.init(hwnd, (int)PW, (int)PH)) { DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance); return true; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig fcfg;
    fcfg.FontDataOwnedByAtlas = false;
    fcfg.OversampleH = 3;
    fcfg.OversampleV = 3;
    fcfg.PixelSnapH = true;
    ImFont* menuFont = io.Fonts->AddFontFromMemoryTTF((void*)CascadiaMonoBL, kCascadiaMonoBLSize, 12.0f, &fcfg);
    imGuiCustom::Initialize(menuFont);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(d3d.dev, d3d.ctx);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    Shared sh;
    Phase phase = Phase::Checking;
    bool launch = false, quit = false;
    auto doneAt = std::chrono::steady_clock::now();

    {
        std::string remote;
        if (Updater::FetchRemoteVersion(remote)) {
            std::lock_guard<std::mutex> lk(sh.mtx);
            sh.remote = remote;
            Updater::FetchChangelog(sh.notes);
            phase = (remote == Updater::kLocalVersion) ? Phase::NoUpdate : Phase::HasUpdate;
            sh.status = (phase == Phase::NoUpdate) ? "up to date" : "update available";
        } else {
            phase = Phase::Ready;
            std::lock_guard<std::mutex> lk(sh.mtx);
            sh.status = "offline - launching current build";
        }
    }

    std::thread dlThread;
    const auto pump = [&]() {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    };

    while (!quit && !launch) {
        pump();
        if (quit) break;

        if (phase == Phase::Downloading && sh.dlDone.load()) {
            if (sh.dlOk.load()) {
                phase = Phase::Done;
                std::lock_guard<std::mutex> lk(sh.mtx);
                sh.status = "download complete - restarting...";
                doneAt = std::chrono::steady_clock::now();
            } else {
                phase = Phase::Error;
                std::lock_guard<std::mutex> lk(sh.mtx);
                sh.status = "download failed - check connection and retry";
            }
        }
        if (phase == Phase::Done &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - doneAt).count() > 1200) {
            std::vector<char> exe;
            { std::lock_guard<std::mutex> lk(sh.mtx); exe = sh.exe; }
            Updater::InstallAndRelaunch(exe);
            quit = true;
            break;
        }

        float dt = io.DeltaTime;
        if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(PW, PH));
        ImGui::Begin("violet.lol", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
        const auto& theme = imGuiCustom::GetTheme();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin(std::floor(ImGui::GetWindowPos().x), std::floor(ImGui::GetWindowPos().y));
        const ImVec2 winMax(origin.x + PW, origin.y + PH);

        // plate: WindowBg + accent glow + outlines (same as menu)
        draw->AddRectFilled(origin, winMax, imGuiCustom::ColorU32(theme.WindowBg), 0.0f);
        imGuiCustom::AddGlowRect(draw, origin, winMax, imGuiCustom::ColorU32(theme.Accent, 0.35f), 0.35f, 4, 6.0f, 0.0f);
        draw->AddRect(origin, winMax, imGuiCustom::OutlineBlack(), 0.0f, 0, 1.0f);
        draw->AddRect(origin + ImVec2(1.0f, 1.0f), winMax - ImVec2(1.0f, 1.0f), imGuiCustom::OutlineInner(), 0.0f, 0, 1.0f);
        AccentBar(draw, origin, PW, PW * 0.5f, 22.0f);
        ShimmerTitle(draw, imGuiCustom::GetFonts(), origin, PW);

        ImFont* font = imGuiCustom::GetFonts().CascadiaMonoBL
            ? imGuiCustom::GetFonts().CascadiaMonoBL : ImGui::GetFont();
        const float fs = 12.5f * imGuiCustom::g_fontScale;

        // version panel
        Panel(draw, origin, ImVec2(6.0f, 40.0f), ImVec2(589.0f, 64.0f));
        {
            std::lock_guard<std::mutex> lk(sh.mtx);
            draw->AddText(font, fs, origin + ImVec2(16.0f, 50.0f),
                imGuiCustom::ColorU32(theme.TextBright), ("build  v" + std::string(Updater::kLocalVersion)).c_str());
            if (!sh.remote.empty())
                draw->AddText(font, fs, origin + ImVec2(220.0f, 50.0f),
                    imGuiCustom::ColorU32(theme.Accent), ("latest  v" + sh.remote).c_str());
            else
                draw->AddText(font, fs, origin + ImVec2(220.0f, 50.0f),
                    imGuiCustom::ColorU32(theme.Text), "latest  ...");
            draw->AddText(font, fs, origin + ImVec2(16.0f, 72.0f),
                imGuiCustom::ColorU32(theme.Text), sh.status.c_str());
        }

        // changelog panel
        Panel(draw, origin, ImVec2(6.0f, 112.0f), ImVec2(589.0f, 218.0f));
        draw->AddText(font, fs, origin + ImVec2(16.0f, 120.0f),
            imGuiCustom::ColorU32(theme.TextBright), "whats new");
        {
            std::lock_guard<std::mutex> lk(sh.mtx);
            const char* notes = sh.notes.empty() ? "(no notes)" : sh.notes.c_str();
            ImGui::SetCursorScreenPos(origin + ImVec2(16.0f, 142.0f));
            ImGui::PushTextWrapPos(origin.x + 585.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.Text);
            ImGui::TextWrapped("%s", notes);
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }

        // action row
        bool pressed = false;
        if (phase == Phase::Checking) {
            draw->AddText(font, fs, origin + ImVec2(16.0f, 348.0f),
                imGuiCustom::ColorU32(theme.Text), "checking for updates...");
        } else if (phase == Phase::NoUpdate || phase == Phase::Ready) {
            MenuButton("Launch Violet", origin, ImVec2(6.0f, 342.0f), ImVec2(589.0f, 34.0f), pressed);
            if (pressed) launch = true;
        } else if (phase == Phase::HasUpdate || phase == Phase::Error) {
            std::string label = "Retry update";
            if (phase == Phase::HasUpdate) {
                std::lock_guard<std::mutex> lk(sh.mtx);
                label = sh.remote.empty() ? "Update & Launch" : ("Update to v" + sh.remote + " & Launch");
            }
            MenuButton(label.c_str(), origin, ImVec2(6.0f, 342.0f), ImVec2(589.0f, 34.0f), pressed);
            if (pressed) {
                phase = Phase::Downloading;
                sh.dlDone.store(false);
                sh.dlOk.store(false);
                sh.progress.store(0.0f);
                { std::lock_guard<std::mutex> lk(sh.mtx); sh.status = "downloading..."; }
                if (dlThread.joinable()) dlThread.join();
                dlThread = std::thread([&sh]() {
                    std::vector<char> exe;
                    bool ok = Updater::DownloadLatest(exe, [&sh](float p) { sh.progress.store(p); });
                    std::lock_guard<std::mutex> lk(sh.mtx);
                    if (ok) sh.exe = std::move(exe);
                    sh.dlOk.store(ok);
                    sh.dlDone.store(true);
                });
                dlThread.detach();
            }
            bool skip = false;
            MenuButton("Skip - launch current build", origin, ImVec2(6.0f, 384.0f), ImVec2(589.0f, 26.0f), skip);
            if (skip) launch = true;
        } else if (phase == Phase::Downloading) {
            const float p = sh.progress.load();
            const ImVec2 tmin(origin + ImVec2(6.0f, 348.0f));
            const ImVec2 tmax(origin + ImVec2(595.0f, 368.0f));
            draw->AddRectFilled(tmin, tmax, imGuiCustom::ColorU32(theme.ControlBg), 0.0f);
            if (p >= 0.0f)
                draw->AddRectFilled(tmin, ImVec2(tmin.x + 589.0f * p, tmax.y),
                    imGuiCustom::ColorU32(theme.Accent), 0.0f);
            draw->AddRect(tmin, tmax, imGuiCustom::OutlineBlack(), 0.0f, 0, 1.0f);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d%%", p < 0 ? 0 : (int)(p * 100.0f));
            const ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf);
            draw->AddText(font, fs,
                ImVec2(std::floor(tmin.x + (589.0f - tsz.x) * 0.5f), std::floor(tmin.y + (20.0f - tsz.y) * 0.5f)),
                imGuiCustom::ColorU32(theme.TextBright), buf);
        } else if (phase == Phase::Done) {
            draw->AddText(font, fs, origin + ImVec2(16.0f, 348.0f),
                imGuiCustom::ColorU32(ImVec4(0.47f, 1.0f, 0.47f, 1.0f)), "restarting with the new build...");
        }

        // footer
        draw->AddText(font, 11.0f * imGuiCustom::g_fontScale, origin + ImVec2(16.0f, 438.0f),
            imGuiCustom::ColorU32(ImVec4(0.45f, 0.45f, 0.45f, 1.0f)), "press INSERT ingame for menu");
        {
            const char* v = ("v" + std::string(Updater::kLocalVersion)).c_str();
            const ImVec2 vsz = font->CalcTextSizeA(11.0f * imGuiCustom::g_fontScale, FLT_MAX, 0.0f, v);
            draw->AddText(font, 11.0f * imGuiCustom::g_fontScale, origin + ImVec2(PW - 16.0f - vsz.x, 438.0f),
                imGuiCustom::ColorU32(ImVec4(0.45f, 0.45f, 0.45f, 1.0f)), v);
        }

        ImGui::End();
        ImGui::Render();
        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        d3d.ctx->OMSetRenderTargets(1, &d3d.rtv, nullptr);
        d3d.ctx->ClearRenderTargetView(d3d.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        d3d.swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    d3d.free();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return launch;
}

} // namespace Launcher
