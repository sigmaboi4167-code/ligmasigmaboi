#include "launcher.h"
#include "../updater/updater.h"
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

} // namespace

bool Run() {
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE); // our UI now, not the prompt

    const int W = 560, H = 480;
    const int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                   GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"violet.launcher", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"violet.lol — launcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sx - W) / 2, (sy - H) / 2, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return true; // fall back to console flow

    D3D d3d;
    if (!d3d.init(hwnd, W, H)) { DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance); return true; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImVec4 accent(0.62f, 0.24f, 0.93f, 1.0f); // violet
    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Button] = ImVec4(0.28f, 0.13f, 0.42f, 1.0f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.38f, 0.18f, 0.56f, 1.0f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.48f, 0.24f, 0.68f, 1.0f);
    st.Colors[ImGuiCol_PlotHistogram] = accent;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(d3d.dev, d3d.ctx);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    Shared sh;
    Phase phase = Phase::Checking;
    bool launch = false, quit = false;
    auto doneAt = std::chrono::steady_clock::now();

    // version check up front (fast, one shot)
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

        // download finished on worker?
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
        // hold the done screen briefly, then swap + relaunch
        if (phase == Phase::Done &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - doneAt).count() > 1200) {
            std::vector<char> exe;
            { std::lock_guard<std::mutex> lk(sh.mtx); exe = sh.exe; }
            Updater::InstallAndRelaunch(exe);
            quit = true; // old binary exits, new one takes over
            break;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##launcher", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextColored(accent, "violet.lol");
        ImGui::SameLine();
        ImGui::TextDisabled("launcher");
        ImGui::Separator();

        {
            std::lock_guard<std::mutex> lk(sh.mtx);
            ImGui::Text("build: v%s", Updater::kLocalVersion);
            if (!sh.remote.empty()) ImGui::Text("latest: v%s", sh.remote.c_str());
            ImGui::TextDisabled("%s", sh.status.c_str());
        }
        ImGui::Spacing();

        if (phase == Phase::HasUpdate || phase == Phase::Error || phase == Phase::Ready) {
            std::lock_guard<std::mutex> lk(sh.mtx);
            ImGui::Text("whats new:");
            ImGui::BeginChild("##notes", ImVec2(0, 150), true);
            ImGui::TextWrapped("%s", sh.notes.empty() ? "(no notes)" : sh.notes.c_str());
            ImGui::EndChild();
            ImGui::Spacing();
        }

        if (phase == Phase::Checking) {
            ImGui::Text("checking for updates...");
        } else if (phase == Phase::NoUpdate || phase == Phase::Ready) {
            if (ImGui::Button("Launch Violet", ImVec2(-1, 40))) launch = true;
        } else if (phase == Phase::HasUpdate || phase == Phase::Error) {
            std::string label = phase == Phase::Error ? "Retry update" : "Update & Launch";
            if (!sh.remote.empty() && phase == Phase::HasUpdate)
                label = "Update to v" + sh.remote + " & Launch";
            if (ImGui::Button(label.c_str(), ImVec2(-1, 40))) {
                phase = Phase::Downloading;
                sh.dlDone.store(false);
                sh.dlOk.store(false);
                sh.progress.store(0.0f);
                { std::lock_guard<std::mutex> lk(sh.mtx); sh.status = "downloading..."; }
                if (dlThread.joinable()) dlThread.join();
                dlThread = std::thread([&sh]() {
                    std::vector<char> exe;
                    bool ok = Updater::DownloadLatest(exe, [&sh](float p) {
                        sh.progress.store(p);
                    });
                    std::lock_guard<std::mutex> lk(sh.mtx);
                    if (ok) sh.exe = std::move(exe);
                    sh.dlOk.store(ok);
                    sh.dlDone.store(true);
                });
                dlThread.detach();
            }
            ImGui::Spacing();
            if (ImGui::Button("Skip - launch current build", ImVec2(-1, 0))) launch = true;
        } else if (phase == Phase::Downloading) {
            float p = sh.progress.load();
            if (p < 0) {
                ImGui::Text("downloading... (size unknown)");
                ImGui::ProgressBar(0.0f, ImVec2(-1, 0));
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%d%%", (int)(p * 100.0f));
                ImGui::ProgressBar(p, ImVec2(-1, 0), buf);
            }
        } else if (phase == Phase::Done) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "restarting with the new build...");
        }

        ImGui::End();
        ImGui::Render();
        const float clear[4] = {0.07f, 0.05f, 0.10f, 1.0f};
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
    return launch; // false = updated (new exe took over) or quit
}

} // namespace Launcher
