#pragma once
// violet.lol — boot cinema for the console window.
// Plain Win32 colors + pure ASCII only (stock cmd safe). No feature dumps,
// no base addresses — just a quick shimmer, live progress and the game line.
#include <windows.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>

namespace Boot {

inline constexpr int V = 13;
inline constexpr int D = 8;
inline constexpr int W = 15;
inline constexpr int G = 10;
inline constexpr int R = 12;
inline constexpr int Y = 14;
inline constexpr int CY = 11;

inline void set_title(const char* t) { SetConsoleTitleA(t); }

inline void paint(const char* s, int c, bool bright = true) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO old{};
    GetConsoleScreenBufferInfo(h, &old);
    SetConsoleTextAttribute(h, (WORD)c | (bright ? FOREGROUND_INTENSITY : 0));
    std::cout << s;
    SetConsoleTextAttribute(h, old.wAttributes);
}

inline void rule() {
    paint("  - - - - - - - - - - - - - - - - - - - -\n", D, false);
}

// TrueType fonts draw the block glyphs clean. Raster fonts turn them
// into garbage, so those consoles get a plain fallback instead.
inline bool console_truetype() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_FONT_INFOEX fi{};
    fi.cbSize = sizeof(fi);
    if (!GetCurrentConsoleFontEx(h, FALSE, &fi))
        return true;
    return (fi.FontFamily & TMPF_TRUETYPE) != 0;
}

// Wordmark: your VIOLET outline plate with a shine sweep rolling
// left-to-right across each row as it drops in.
inline void banner() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO old{};
    GetConsoleScreenBufferInfo(h, &old);
    std::cout << "\n";
    static const char* art[] = {
        "  ___      ___ ___  ________  ___       _______  _________   ",
        "  |\\  \\    /  /|\\  \\|\\   __  \\|\\  \\     |\\  ___ \\|\\___   ___\\ ",
        "  \\ \\  \\  /  / | \\  \\ \\  \\|\\  \\ \\  \\    \\ \\   __/\\|___ \\  \\_| ",
        "   \\ \\  \\/  / / \\ \\  \\ \\  \\\\\\  \\ \\  \\    \\ \\  \\_|/__  \\ \\  \\  ",
        "    \\ \\    / /   \\ \\  \\ \\  \\\\\\  \\ \\  \\____\\ \\  \\_|\\ \\  \\ \\  \\ ",
        "     \\ \\__/ /     \\ \\__\\ \\_______\\ \\_______\\ \\_______\\  \\ \\__\\",
        "      \\|__|/       \\|__|\\|_______|\\|_______|\\|_______|   \\|__| ",
    };
    for (auto line : art) {
        std::string s(line);
        const int n = (int)s.size();
        // Shine frames: bright band sweeps across, then the row settles violet.
        for (int f = 0; f <= 2; ++f) {
            int head = (n * f) / 2;
            int tail = head - 10;
            if (tail < 0)
                tail = 0;
            std::cout << "\r  ";
            for (int i = 2; i < n; ++i) {
                bool lit = (i >= tail && i <= head);
                SetConsoleTextAttribute(h, lit ? ((WORD)W | FOREGROUND_INTENSITY)
                                              : ((WORD)V | FOREGROUND_INTENSITY));
                std::cout << s[(size_t)i];
            }
            std::cout << "  " << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        std::cout << "\r  ";
        SetConsoleTextAttribute(h, (WORD)V | FOREGROUND_INTENSITY);
        std::cout << s.c_str() + 2 << "  \n" << std::flush;
    }
    SetConsoleTextAttribute(h, old.wAttributes);
    // Short two-tone menu hint.
    std::cout << "  ";
    paint("press ", D, false);
    paint("INSERT", W, true);
    paint(" ingame for menu\n", D, false);
    rule();
}

inline void stage(const char* label, const char* msg, int c) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO old{};
    GetConsoleScreenBufferInfo(h, &old);
    auto t = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count();
    std::cout << "  ";
    SetConsoleTextAttribute(h, (WORD)W | FOREGROUND_INTENSITY);
    std::cout << "[";
    SetConsoleTextAttribute(h, (WORD)c | FOREGROUND_INTENSITY);
    std::cout << label;
    SetConsoleTextAttribute(h, (WORD)W | FOREGROUND_INTENSITY);
    std::cout << "] ";
    SetConsoleTextAttribute(h, (WORD)D);
    std::cout << msg << "\n";
    SetConsoleTextAttribute(h, old.wAttributes);
    (void)t;
}
inline void ok(const char* label, const std::string& msg) { stage(label, msg.c_str(), G); }
inline void warn(const char* label, const std::string& msg) { stage(label, msg.c_str(), Y); }
inline void fail(const char* label, const std::string& msg) { stage(label, msg.c_str(), R); }

inline float& last_bar() {
    static float f = 0.0f;
    return f;
}

// Animated fill: eases from whatever was last shown up to the target.
inline void bar_animated(float target, int width = 26) {
    if (target < 0) target = 0;
    if (target > 1) target = 1;
    float from = last_bar();
    if (target < from) from = 0.0f;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO old{};
    GetConsoleScreenBufferInfo(h, &old);
    const int steps = 5;
    for (int s = 1; s <= steps; ++s) {
        float f = from + (target - from) * ((float)s / (float)steps);
        int fill = (int)(f * (float)width + 0.5f);
        std::cout << "\r  ";
        SetConsoleTextAttribute(h, (WORD)V | FOREGROUND_INTENSITY);
        std::cout << "<";
        for (int i = 0; i < width; ++i)
            std::cout << (i < fill ? "=" : ".");
        std::cout << "> ";
        SetConsoleTextAttribute(h, (WORD)W | FOREGROUND_INTENSITY);
        std::cout << (int)(f * 100.0f) << "%" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cout << "\n";
    SetConsoleTextAttribute(h, old.wAttributes);
    last_bar() = target;
}

// Fire-and-forget waiter with crawling dots + elapsed timer.
class Spinner {
public:
    void start(const char* msg) {
        stop();
        msg_ = msg ? msg : "";
        run_ = true;
        th_ = std::thread([this] {
            int i = 0;
            auto t0 = std::chrono::steady_clock::now();
            while (run_) {
                auto s = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
                int dots = 1 + (i % 3);
                char line[192];
                std::snprintf(line, sizeof(line), "  %s%*s (%llds)      \r", msg_.c_str(), dots, "...",
                              (long long)s);
                std::cout << line << std::flush;
                ++i;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
    }
    void stop() {
        if (run_) {
            run_ = false;
            if (th_.joinable()) th_.join();
            std::cout << "                                                             \r" << std::flush;
        }
    }
    ~Spinner() { stop(); }

private:
    std::atomic<bool> run_{false};
    std::thread th_;
    std::string msg_;
};

// One-liner game line. No feature dump.
inline void game_line(const char* name, const char* tag, std::uint64_t placeId) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO old{};
    GetConsoleScreenBufferInfo(h, &old);
    char idbuf[64];
    std::snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)placeId);
    std::cout << "  ";
    SetConsoleTextAttribute(h, (WORD)V | FOREGROUND_INTENSITY);
    std::cout << ">> ";
    SetConsoleTextAttribute(h, (WORD)W | FOREGROUND_INTENSITY);
    std::cout << name;
    SetConsoleTextAttribute(h, (WORD)D);
    std::cout << "  // " << tag << "  // ";
    SetConsoleTextAttribute(h, (WORD)CY | FOREGROUND_INTENSITY);
    std::cout << idbuf << "\n";
    SetConsoleTextAttribute(h, old.wAttributes);
}

} // namespace Boot
