#include "updater.h"
#include "../boot/boot.h"
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>

#if defined(_MSC_VER)
#pragma comment(lib, "winhttp.lib")
#endif

namespace Updater {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Splits https://host/path into host + path. http also works.
bool split_url(const std::string& url, bool& secure, std::wstring& host, std::wstring& path) {
    std::string u = url;
    secure = true;
    const char* p = nullptr;
    if (u.rfind("https://", 0) == 0) { secure = true; p = u.c_str() + 8; }
    else if (u.rfind("http://", 0) == 0) { secure = false; p = u.c_str() + 7; }
    else return false;
    std::string rest(p);
    auto slash = rest.find('/');
    std::string h = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string q = (slash == std::string::npos) ? "/" : rest.substr(slash);
    host.assign(h.begin(), h.end());
    path.assign(q.begin(), q.end());
    return !h.empty();
}

bool http_get(const std::string& url, std::vector<char>& out, DWORD timeout_ms = 8000) {
    out.clear();
    bool secure = true;
    std::wstring host, path;
    if (!split_url(url, secure, host, path))
        return false;
    HINTERNET hS = WinHttpOpen(L"violet.lol/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hS) return false;
    WinHttpSetTimeouts(hS, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    HINTERNET hC = WinHttpConnect(hS, host.c_str(), secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); return false; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return false; }
    bool ok = false;
    if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hR, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        if (WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                nullptr, &status, &len, nullptr) && (status == 200 || status == 302 || status == 301)) {
            // follow one redirect (github releases/latest/download redirects to the asset)
            if (status == 301 || status == 302) {
                wchar_t loc[2048]{};
                DWORD lloc = sizeof(loc);
                if (WinHttpQueryHeaders(hR, WINHTTP_QUERY_LOCATION, nullptr, loc, &lloc, nullptr)) {
                    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
                    char narrow[2048]{};
                    WideCharToMultiByte(CP_UTF8, 0, loc, -1, narrow, sizeof(narrow), nullptr, nullptr);
                    return http_get(narrow, out, timeout_ms);
                }
            } else {
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(hR, &avail) || !avail) break;
                    size_t old = out.size();
                    out.resize(old + avail);
                    DWORD rd = 0;
                    if (!WinHttpReadData(hR, out.data() + old, avail, &rd) || !rd) { out.resize(old); break; }
                    out.resize(old + rd);
                }
                ok = !out.empty();
            }
        }
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return ok;
}

bool self_swap_and_relaunch(const std::vector<char>& exe) {
    char self[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, self, MAX_PATH)) return false;
    std::string me(self);
    std::string next = me + ".new";
    std::string bat = me + ".upd.bat";

    HANDLE f = CreateFileA(next.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(f, exe.data(), (DWORD)exe.size(), &w, nullptr);
    CloseHandle(f);
    if (w != exe.size()) { DeleteFileA(next.c_str()); return false; }

    // waiter script: dead-loop until old exe exits, swap, relaunch, self-delete
    char script[2048];
    std::snprintf(script, sizeof(script),
        "@echo off\r\n"
        ":loop\r\n"
        "tasklist /FI \"PID eq %u\" 2>nul | find \"%u\" >nul && (timeout /t 1 /nobreak >nul & goto loop)\r\n"
        "move /y \"%s\" \"%s\" >nul\r\n"
        "start \"\" \"%s\"\r\n"
        "del \"%%~f0\"\r\n",
        GetCurrentProcessId(), GetCurrentProcessId(), next.c_str(), me.c_str(), me.c_str());
    f = CreateFileA(bat.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { DeleteFileA(next.c_str()); return false; }
    WriteFile(f, script, (DWORD)strlen(script), &w, nullptr);
    CloseHandle(f);

    SHELLEXECUTEINFOA sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = "cmd.exe";
    char args[512];
    std::snprintf(args, sizeof(args), "/c \"%s\"", bat.c_str());
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    return ShellExecuteExA(&sei) != FALSE;
}

} // namespace

bool CheckAndUpdate() {
    std::vector<char> body;
    if (!http_get(kVersionUrl, body))
        return false; // offline or github down — just run
    std::string remote = trim(std::string(body.begin(), body.end()));
    std::string local = trim(kLocalVersion);
    if (remote.empty() || remote == local) {
        Boot::ok("version", ("up to date - v" + local).c_str());
        return false;
    }

    Boot::warn("update", ("you're using old version v" + local + " - updating to latest v" + remote + "...").c_str());

    // show what's new
    std::vector<char> log;
    if (http_get(kChangelogUrl, log) && !log.empty()) {
        std::string notes = trim(std::string(log.begin(), log.end()));
        if (notes.size() > 1500) notes.resize(1500);
        Boot::ok("whats-new", notes.c_str());
    }

    Boot::warn("update", "downloading latest Violet.exe...");
    std::vector<char> exe;
    if (!http_get(kExeUrl, exe) || exe.size() < 1024) {
        Boot::fail("update", "download failed - running current build");
        return false;
    }
    // sanity: must look like a PE
    if (exe.size() < 2 || exe[0] != 'M' || exe[1] != 'Z') {
        Boot::fail("update", "bad payload - running current build");
        return false;
    }
    if (!self_swap_and_relaunch(exe)) {
        Boot::fail("update", "relaunch failed - running current build");
        return false;
    }
    Boot::ok("update", "updated - restarting...");
    return true;
}

} // namespace Updater
