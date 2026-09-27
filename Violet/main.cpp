#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <chrono>
#include <windows.h>
#include <shellapi.h>
#include "src/core/app/app.h"
#include "src/core/logger/logger.h"
#include "src/core/boot/boot.h"
#include "src/core/launcher/launcher.h"
#include "src/core/games/games.h"
#include "src/memory/memory.h"
#include "src/core/globals/globals.h"
#include "src/sdk/offsets.h"

namespace {
constexpr WORD C_RED    = FOREGROUND_RED | FOREGROUND_INTENSITY;
constexpr WORD C_YELLOW = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;

void badge(const char* label, WORD labelColor, const char* msg) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO old{};
    GetConsoleScreenBufferInfo(h, &old);
    std::cout << "  ";
    SetConsoleTextAttribute(h, 15 | FOREGROUND_INTENSITY);
    std::cout << "[";
    SetConsoleTextAttribute(h, labelColor);
    std::cout << label;
    SetConsoleTextAttribute(h, 15 | FOREGROUND_INTENSITY);
    std::cout << "] ";
    SetConsoleTextAttribute(h, 7);
    std::cout << msg << "\n";
    SetConsoleTextAttribute(h, old.wAttributes);
}

bool is_admin() {
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &admin);
        FreeSid(group);
    }
    return admin != FALSE;
}

void relaunch_as_admin() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
        badge("priv", C_RED, "elevation declined - run Violet.exe as administrator");
}

void stage_watcher(std::atomic<bool>& stop) {
    Boot::Spinner spin;
    spin.start("waiting for RobloxPlayerBeta.exe");
    int shown = 0;
    while (!stop.load()) {
        const int s = App::stage.load();
        if (s >= 1 && shown < 1) {
            shown = 1;
            spin.stop();
            char buf[64];
            snprintf(buf, sizeof(buf), "pid=%u", (unsigned)memory->get_process_id());
            Boot::ok("attached", buf);
            Boot::bar_animated(0.45f);
        }
        if (s >= 2 && shown < 2) {
            shown = 2;
            Boot::ok("overlay", "DirectX ready");
            Boot::bar_animated(0.8f);
            // Game line: name + profile + place. Nothing else.
            const auto& gi = Game::Get(Globals::game);
            Boot::game_line(gi.name, gi.tag, Globals::placeId);
            Boot::bar_animated(1.0f);
        }
        if (s >= 3 && shown < 3) {
            shown = 3;
            Boot::ok("ready", "press INSERT for menu - have fun");
            Boot::rule();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    spin.stop();
}
}

std::int32_t main() {
    SetUnhandledExceptionFilter(CrashHandler);
    AddVectoredExceptionHandler(1, [](PEXCEPTION_POINTERS ep)->LONG {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code==0x406D1388 || code==0x40010006 || code==0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;
        static long long lastMs=0; static int count=0;
        long long now = GetTickCount64();
        if (now - lastMs < 500) return EXCEPTION_CONTINUE_SEARCH;
        if (count++ > 20) return EXCEPTION_CONTINUE_SEARCH;
        lastMs = now;
        Logger::write_crash(ep, "VEH");
        return EXCEPTION_CONTINUE_SEARCH;
    });
    Logger::quiet = true;
    Logger::init();
    Boot::set_title("violet.lol");
    Boot::banner();
    if (!is_admin()) {
        Boot::warn("priv", "administrator required - relaunching elevated...");
        relaunch_as_admin();
        return 0;
    }
    Boot::ok("priv", "running as administrator");
    if (!Launcher::Run())
        return 0; // updated (new exe took over) or quit from the launcher
    std::atomic<bool> stopWatcher{false};
    std::thread watcher(stage_watcher, std::ref(stopWatcher));
    const std::int32_t code = App::Run();
    stopWatcher.store(true);
    if (watcher.joinable()) watcher.join();
    if (code == 0) {
        Boot::ok("done", "session ended cleanly");
    } else {
        char buf[128]; snprintf(buf, sizeof(buf), "exited with code %d - see violet.lol_log.txt", code);
        Boot::fail("fail", buf);
        system("pause >nul");
    }
    return code;
}
