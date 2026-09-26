#include "combat.h"
#include "../../globals/globals.h"
#include "../../variables/variables.h"
#include "../../games/games.h"
#include "../../cache/pf_cache.h"
#include "../../cache/cb_cache.h"
#include "../../cache/ops_cache.h"
#include "../../../sdk/sdk.h"

#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace Combat {
namespace {

inline bool valid_ptr(uintptr_t p) {
    return p >= 0x10000 && p <= 0x00007FFFFFFFFFFF;
}

inline std::string lower_str(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline bool name_has_any(const std::string& name, const char* const* keys, int n) {
    std::string ln = lower_str(name);
    for (int i = 0; i < n; ++i) {
        if (ln.find(keys[i]) != std::string::npos)
            return true;
    }
    return false;
}

// Fallen Survival (main game: ores/plants/Boris tools) keeps gun tuning in
// Tool configs + Backpack + PlayerGui weapon frames. PF/CB/Ops use same
// ValueBase pattern. Extra keys cover all four.
constexpr const char* kSpreadKeys[] = {
    "spread", "bloom", "deviation", "dispers", "inaccuracy",
    "spreadangle", "bulletspread", "shotspread", "hipspread", "aimspread",
    "gunspread", "aperture", "crosshairspread", "sspread", "currentbloom"
};
constexpr const char* kRecoilKeys[] = {
    "recoil", "kick", "kickback", "kickup", "camerashake", "camerakick",
    "viewkick", "viewpunch", "punch", "shake", "shakemag", "recoilamount",
    "kickamount", "gunrecoil", "weaponrecoil", "recoilx", "recoily",
    "cameraoffset", "recoilpower", "kickpower"
};

struct Tracked {
    uintptr_t addr = 0;
    float backupFloat = 0.0f;
    int32_t backupInt = 0;
    bool isInt = false;
    bool isSpread = false;
};

inline std::unordered_map<uintptr_t, Tracked>& tracked() {
    static std::unordered_map<uintptr_t, Tracked> m;
    return m;
}
inline std::string lastSig;
inline std::chrono::steady_clock::time_point lastScan{};
inline std::chrono::steady_clock::time_point lastTick{};

// ---- camera recoil trim (works even when stats live in Lua modules) ----
inline bool firing() {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}
inline float look_pitch(const RBX::CFrame& cf) {
    float ly = -cf.data[5]; // look.Y = -rot[5]
    if (ly > 1.0f) ly = 1.0f;
    if (ly < -1.0f) ly = -1.0f;
    return std::asin(ly);
}
inline RBX::CFrame pitch_corrected(const RBX::CFrame& cur, float dPitch) {
    // Rotate look/up around the RIGHT axis by dPitch (negative = pull down).
    float rx = cur.data[0], ry = cur.data[3], rz = cur.data[6]; // right
    float ux = cur.data[1], uy = cur.data[4], uz = cur.data[7]; // up
    float lx = cur.data[2], ly = cur.data[5], lz = cur.data[8]; // look
    float c = std::cos(dPitch), s = std::sin(dPitch);
    RBX::CFrame out = cur;
    out.data[1] = ux * c + lx * s;
    out.data[4] = uy * c + ly * s;
    out.data[7] = uz * c + lz * s;
    out.data[2] = lx * c - ux * s;
    out.data[5] = ly * c - uy * s;
    out.data[8] = lz * c - uz * s;
    (void)rx; (void)ry; (void)rz;
    return out;
}
inline void tick_camera_trim() {
    // OPT-IN ONLY: pure no-recoil never touches the camera. This path runs
    // only when the user explicitly enables Camera Assist.
    if (!variables::Combat::noRecoil || !variables::Combat::recoilCamAssist ||
        !Globals::camera.Addr || !valid_ptr(Globals::camera.Addr))
        return;
    static float basePitch = 0.0f;
    static bool haveBase = false;
    static bool wasFiring = false;
    const bool isFiring = firing();
    // Humanoid.CameraOffset is the other recoil channel (punch/shake vector).
    // Zero it while firing so the kick has nowhere to hide.
    static uintptr_t lastHum = 0;
    static RBX::Vec3 offsetBackup{0, 0, 0};
    static bool offsetZeroed = false;
    auto zero_offset = [&]() {
        auto ch = Globals::localPlayer.GetModelRef();
        uintptr_t hum = 0;
        if (ch.Addr && valid_ptr(ch.Addr)) {
            auto h = ch.FindChildByClass("Humanoid");
            hum = h.Addr;
        }
        if (hum && valid_ptr(hum)) {
            if (!offsetZeroed || hum != lastHum) {
                offsetBackup = memory->read<RBX::Vec3>(hum + Offsets::Humanoid::CameraOffset);
                lastHum = hum;
            }
            RBX::Vec3 cur = memory->read<RBX::Vec3>(hum + Offsets::Humanoid::CameraOffset);
            if (std::isfinite(cur.X) && std::isfinite(cur.Y) && std::isfinite(cur.Z)) {
                if (std::fabs(cur.X) > 0.001f || std::fabs(cur.Y) > 0.001f || std::fabs(cur.Z) > 0.001f)
                    memory->write<RBX::Vec3>(hum + Offsets::Humanoid::CameraOffset, RBX::Vec3{0, 0, 0});
                offsetZeroed = true;
            }
        }
    };
    auto restore_offset = [&]() {
        if (offsetZeroed && lastHum && valid_ptr(lastHum))
            memory->write<RBX::Vec3>(lastHum + Offsets::Humanoid::CameraOffset, offsetBackup);
        offsetZeroed = false;
    };
    if (!isFiring) {
        restore_offset();
    } else {
        zero_offset();
    }
    RBX::CFrame cf = memory->read<RBX::CFrame>(Globals::camera.Addr + Offsets::Camera::Rotation);
    float px = cf.data[0], py = cf.data[5];
    if (!std::isfinite(px) || !std::isfinite(py)) { haveBase = false; wasFiring = false; return; }
    // Validate rotation matrix roughly orthonormal before touching it.
    float rl = std::sqrt(cf.data[0]*cf.data[0] + cf.data[3]*cf.data[3] + cf.data[6]*cf.data[6]);
    if (rl < 0.5f || rl > 2.0f) { haveBase = false; wasFiring = false; return; }
    float pitch = look_pitch(cf);
    if (!std::isfinite(pitch)) return;
    if (!isFiring) {
        basePitch = pitch; // track intention while not shooting
        haveBase = true;
        wasFiring = false;
        return;
    }
    if (!wasFiring) { haveBase = true; wasFiring = true; /* keep pre-fire baseline */ }
    if (!haveBase) { basePitch = pitch; haveBase = true; return; }
    float climb = pitch - basePitch;
    // TRUE ZERO: pull 100% of upward climb back to baseline every tick.
    // Old 88% left visible drift. Downward drag passes through + re-baselines
    // so you can still control spray. Yaw never touched.
    if (climb > 0.0008f) {
        float fix = -climb; // full cancel
        // Per-tick clamp raised so fast full-auto kicks still die in one tick.
        if (fix < -0.10f) fix = -0.10f;
        RBX::CFrame fixed = pitch_corrected(cf, fix);
        memory->write<RBX::CFrame>(Globals::camera.Addr + Offsets::Camera::Rotation, fixed);
    } else if (climb < -0.02f) {
        // You dragged down hard on purpose -> adopt new baseline.
        basePitch = pitch;
    }
}

// Active profile: place-pinned game refined with live cache signals,
// so sub-places and late-replicating PlaceIds still land correctly.
inline Game::Type active_game() {
    return Game::Refine(Globals::game, PfCache::workspacePlayersAddr != 0,
                        CbCache::charactersAddr != 0, OpsCache::viewmodelsAddr != 0);
}

void scan_root(uintptr_t root, size_t& visited, size_t budget) {
    if (!root || !valid_ptr(root) || visited >= budget)
        return;
    std::vector<uintptr_t> stack;
    stack.reserve(256);
    stack.push_back(root);
    size_t head = 0;
    auto& t = tracked();
    while (head < stack.size() && visited < budget) {
        uintptr_t cur = stack[head++];
        ++visited;
        if (!cur || !valid_ptr(cur))
            continue;
        RBX::RbxInstance inst(cur);
        auto kids = inst.GetChildList();
        for (auto& k : kids) {
            if (k.Addr && valid_ptr(k.Addr) && stack.size() < 512)
                stack.push_back(k.Addr);
        }
        // Fast reject: only ValueBase classes can hold tuning numbers.
        std::string cls = inst.GetClass();
        if (cls != "NumberValue" && cls != "IntValue" && cls != "DoubleConstrainedValue")
            continue;
        std::string nm = inst.GetName();
        if (nm.empty() || nm.size() > 64)
            continue;
        bool isSpread = name_has_any(nm, kSpreadKeys, (int)(sizeof(kSpreadKeys) / sizeof(kSpreadKeys[0])));
        bool isRecoil = name_has_any(nm, kRecoilKeys, (int)(sizeof(kRecoilKeys) / sizeof(kRecoilKeys[0])));
        if (!isSpread && !isRecoil)
            continue;
        if (t.find(cur) != t.end())
            continue;
        Tracked tr{};
        tr.addr = cur;
        tr.isSpread = isSpread && !isRecoil ? true : isSpread;
        if (cls == "IntValue") {
            tr.isInt = true;
            tr.backupInt = memory->read<int32_t>(cur + Offsets::Value::Value);
        } else {
            tr.isInt = false;
            tr.backupFloat = memory->read<float>(cur + Offsets::Value::Value);
            if (!std::isfinite(tr.backupFloat))
                tr.backupFloat = 0.0f;
        }
        t.emplace(cur, tr);
    }
}

// GC-style sweep roots: equipped tool + backpack + character + gun UI,
// plus ReplicatedStorage weapon subtrees on deep-sweep profiles.
// Walks every live weapon object reachable from the service roots,
// matches tuning values by name hash, freezes them at 0 with backup.
void rescan_all() {
    auto& t = tracked();
    t.clear();
    const Game::Type g = active_game();
    // GC-style sweep budget: big-tree profiles (Fallen / Rivals) walk the
    // full ReplicatedStorage weapon subtrees; cache-path games (PF/CB/Ops)
    // stay shallow — their trees are huge and hot, and their guns resolve
    // through the character + backpack roots below.
    const size_t budget = Game::DeepSweep(g) ? 2048 : 768;
    size_t visited = 0;
    std::string sig;
    auto ch = Globals::localPlayer.GetModelRef();
    // 1) equipped tool (character child)
    if (ch.Addr && valid_ptr(ch.Addr)) {
        for (auto& c : ch.GetChildList()) {
            if (c.GetClass() == "Tool" && valid_ptr(c.Addr)) {
                sig += std::to_string(c.Addr) + ";";
                scan_root(c.Addr, visited, budget);
            }
        }
        // 2) whole character (Humanoid-side recoil values live here on some games)
        scan_root(ch.Addr, visited, budget);
    }
    // 3) Backpack (Fallen keeps every gun Tool here, stats readable even unequipped)
    if (Globals::localPlayer.Addr && valid_ptr(Globals::localPlayer.Addr)) {
        auto pack = RBX::RbxInstance(Globals::localPlayer.Addr).FindChild("Backpack");
        if (pack.Addr && valid_ptr(pack.Addr)) {
            sig += "p" + std::to_string(pack.Addr) + ";";
            scan_root(pack.Addr, visited, budget);
        }
        // 4) PlayerGui weapon frames (PF Fallen-style bloom bars, CB crosshair state)
        auto gui = RBX::RbxInstance(Globals::localPlayer.Addr).FindChild("PlayerGui");
        if (gui.Addr && valid_ptr(gui.Addr)) {
            for (auto& c : gui.GetChildList()) {
                std::string n = lower_str(c.GetName());
                if (n.find("gun") != std::string::npos || n.find("weapon") != std::string::npos ||
                    n.find("hud") != std::string::npos || n.find("crosshair") != std::string::npos ||
                    n.find("bloom") != std::string::npos) {
                    scan_root(c.Addr, visited, budget);
                }
            }
        }
    }
    // 5) ReplicatedStorage gun configs — deep-sweep profiles only (Fallen /
    //    Rivals). Cache-path games skip it: huge hot trees, and their guns
    //    resolve through the roots above. Zeroing these is true "gun has no
    //    kick" recoil.
    if (Game::DeepSweep(g) && Globals::dataModel.Addr && valid_ptr(Globals::dataModel.Addr)) {
        auto rs = RBX::RbxInstance(Globals::dataModel.Addr).FindChild("ReplicatedStorage");
        if (rs.Addr && valid_ptr(rs.Addr)) {
            sig += "rs" + std::to_string(rs.Addr) + ";";
            for (auto& c : rs.GetChildList()) {
                std::string n = lower_str(c.GetName());
                if (n.find("weapon") != std::string::npos || n.find("gun") != std::string::npos ||
                    n == "arsenal" || n.find("config") != std::string::npos ||
                    n.find("stats") != std::string::npos || n.find("balance") != std::string::npos) {
                    scan_root(c.Addr, visited, budget);
                    if (visited >= budget)
                        break;
                }
            }
        }
    }
    lastSig = sig;
}

void restore_all() {
    auto& t = tracked();
    for (auto& kv : t) {
        const Tracked& tr = kv.second;
        if (!tr.addr || !valid_ptr(tr.addr))
            continue;
        if (tr.isInt)
            memory->write<int32_t>(tr.addr + Offsets::Value::Value, tr.backupInt);
        else if (std::isfinite(tr.backupFloat))
            memory->write<float>(tr.addr + Offsets::Value::Value, tr.backupFloat);
    }
}

} // namespace

void Tick() {
    const bool wantSpread = variables::Combat::noSpread;
    const bool wantRecoil = variables::Combat::noRecoil;
    auto& t = tracked();
    const auto now = std::chrono::steady_clock::now();

    // Camera trim runs on its own fast path (every call, ~100Hz from Loop)
    // and ONLY when Camera Assist is on — default is pure gun-value mode.
    if (wantRecoil && variables::Combat::recoilCamAssist)
        tick_camera_trim();

    if (!wantSpread && !wantRecoil) {
        if (!t.empty()) {
            restore_all();
            t.clear();
        }
        lastSig.clear();
        return;
    }

    // Value zeroing throttled 30Hz — hammering gun values causes rubber-band / flags.
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count() < 33)
        return;
    lastTick = now;

    // Rescan when loadout changes or on profile interval (deep 3s —
    // Fallen/Rivals swap tools constantly; light 5s for hot trees).
    const int rescanSecs = Game::DeepSweep(active_game()) ? 3 : 5;
    std::string sig;
    {
        auto ch = Globals::localPlayer.GetModelRef();
        if (ch.Addr && valid_ptr(ch.Addr)) {
            for (auto& c : ch.GetChildList())
                if (c.GetClass() == "Tool") { sig += std::to_string(c.Addr) + ";"; break; }
        }
    }
    if (sig != lastSig ||
        std::chrono::duration_cast<std::chrono::seconds>(now - lastScan).count() >= rescanSecs) {
        lastScan = now;
        rescan_all();
    }

    for (auto& kv : t) {
        Tracked& tr = kv.second;
        if (!tr.addr || !valid_ptr(tr.addr))
            continue;
        bool active = tr.isSpread ? wantSpread : wantRecoil;
        if (!active) {
            if (tr.isInt)
                memory->write<int32_t>(tr.addr + Offsets::Value::Value, tr.backupInt);
            else if (std::isfinite(tr.backupFloat))
                memory->write<float>(tr.addr + Offsets::Value::Value, tr.backupFloat);
            continue;
        }
        if (tr.isInt) {
            int32_t cur = memory->read<int32_t>(tr.addr + Offsets::Value::Value);
            if (cur != 0)
                memory->write<int32_t>(tr.addr + Offsets::Value::Value, 0);
        } else {
            float cur = memory->read<float>(tr.addr + Offsets::Value::Value);
            if (std::isfinite(cur) && std::fabs(cur) > 0.0001f)
                memory->write<float>(tr.addr + Offsets::Value::Value, 0.0f);
        }
    }
}

void Loop()
{
    using namespace std::chrono_literals;
    lastScan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    lastTick = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    while (Globals::running)
    {
        Tick();
        // Fast race vs gun Lua: 1ms while holding fire with cam assist,
        // otherwise chill at 10ms so we sip CPU.
        bool fast = variables::Combat::noRecoil && variables::Combat::recoilCamAssist && firing();
        std::this_thread::sleep_for(fast ? 1ms : 10ms);
    }
    restore_all();
}

}
