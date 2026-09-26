#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "mics.h"
#include "../../keys/keys.h"
#include "../../../render/menu/library.h"
#include "../world/world.h"
#include "../aim/magic.h"
#include "../aim/pf_silent.h"
#include "../aim/viewport_silent.h"
#include <thread>
#include <chrono>

namespace Mics {
void Tick() {
    auto character = Globals::localPlayer.GetModelRef();
    auto humanoid = character.Addr ? character.FindChildByClass("Humanoid") : RBX::RbxInstance{};
    if (!humanoid.Addr)
        return;
    static bool jumpWas = false;
    static float jumpBackup = 50.0f;
    static std::uintptr_t jumpHum = 0;
    static bool jumpTog = false;
    static bool jumpKeyWas = false;
    if (variables::Local::jumpEnabled && Keys::Gate(variables::Local::jumpKey, variables::Local::jumpKeyMode, jumpTog, jumpKeyWas)) {
        if (!jumpWas || jumpHum != humanoid.Addr) {
            jumpBackup = memory->read<float>(humanoid.Addr + Offsets::Humanoid::JumpPower);
            if (!std::isfinite(jumpBackup) || jumpBackup <= 0.0f)
                jumpBackup = 50.0f;
            jumpHum = humanoid.Addr;
        }
        RBX::ModifyJumpPower(humanoid, variables::Local::jumpPower);
        jumpWas = true;
    } else if (jumpWas) {
        if (jumpHum)
            RBX::ModifyJumpPower(RBX::RbxInstance{jumpHum}, jumpBackup);
        jumpWas = false;
    }
}

void TickFov() {
    static bool wasOn = false;
    static bool tog = false;
    static bool was = false;
    const bool on = variables::Movement::fov && Keys::Gate(variables::Movement::fovKey, variables::Movement::fovKeyMode, tog, was);
    if (on && Globals::camera.Addr) {
        wasOn = true;
        memory->write<float>(Globals::camera.Addr + Offsets::Camera::FieldOfView, variables::Movement::fovValue * (3.14159265f / 180.0f));
    } else if (wasOn) {
        wasOn = false;
        if (Globals::camera.Addr)
            memory->write<float>(Globals::camera.Addr + Offsets::Camera::FieldOfView, 70.0f * (3.14159265f / 180.0f));
    }
}

void TickLighting() {
    WorldVisuals::TickLighting();
}

void TickPanic() {
    // One-shot kill switch: press END (or your bind) and every module goes
    // dark + restores, menu closes, overlay hides from capture. Edge-triggered.
    static bool was = false;
    int key = variables::Misc::panicKey;
    if (key <= 0 || !variables::Misc::panicBound) {
        was = false;
        return;
    }
    bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (down && !was) {
        variables::Aimbot::enabled = false;
        variables::ESP::enabled = false;
        variables::World::enabled = false;
        variables::World::clockTimeEnabled = false;
        variables::World::brightnessEnabled = false;
        variables::World::ambientEnabled = false;
        variables::World::fogEnabled = false;
        variables::World::exposureEnabled = false;
        variables::World::skyboxEnabled = false;
        variables::Combat::noSpread = false;
        variables::Combat::noRecoil = false;
        variables::Movement::fly = false;
        variables::Movement::noclip = false;
        variables::Freecam::enabled = false;
        variables::Misc::watermark = false;
        variables::Misc::keybinds = false;
        variables::menuOpen = false;
        variables::Misc::streamProof = true;
        MagicBullet::SetActive(false, {});
        MagicBullet::Ensure(false);
        PfSilent::SetActive(false, {});
        ViewportSilent::Clear();
    }
    was = down;
}

void Loop() {
    using namespace std::chrono_literals;
    while (Globals::running) {
        Tick();
        TickFov();
        TickLighting();
        TickPanic();
        std::this_thread::sleep_for(5ms);
    }
}

void RenderLocalMenu() {
    ImVec2 mBase = ImGui::GetWindowPos();
    ImVec2 mLMin = mBase + ImVec2(6.0f, 40.0f);
    ImVec2 mLMax = mBase + ImVec2(6.0f + 290.0f, 40.0f + 340.0f);
    ImVec2 mMp = ImGui::GetIO().MousePos;
    static float localScroll = 0.f;
    if (mMp.x >= mLMin.x && mMp.x <= mLMax.x && mMp.y >= mLMin.y && mMp.y <= mLMax.y &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        float wh = ImGui::GetIO().MouseWheel;
        if (wh != 0.f && !imGuiCustom::PopupBlocking()) localScroll -= wh * 22.0f;
    }
    if (localScroll < 0.f) localScroll = 0.f;
    ImDrawList* mFg = ImGui::GetWindowDrawList();
    mFg->PushClipRect(mLMin, mLMax, true);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
    float ly = 46.0f - localScroll;
    float tJump = imGuiCustom::TreeToggle("JumpPower", &variables::Local::jumpEnabled, ImVec2(12.0f, ly));
    imGuiCustom::Keybind("jump_key", &variables::Local::jumpKey, ImVec2(222.0f, ly - 1.0f), ImVec2(68.0f, 13.0f), &variables::Local::jumpKeyMode);
    ly += imGuiCustom::CheckStep();
    if (tJump > 0.02f) {
        imGuiCustom::TreeScope _ts(14.0f, ly, &ly, tJump);
        imGuiCustom::SliderFloat("jumppower", &variables::Local::jumpPower, 50.0f, 200.0f, ImVec2(12.0f, ly + imGuiCustom::SliderTop()), 272.0f, "JumpPower", "%.0f");
        ly += imGuiCustom::SliderStep();
    }
    float tBhop = imGuiCustom::TreeToggle("BunnyHop", &variables::Movement::bunnyHop, ImVec2(12.0f, ly));
    imGuiCustom::Keybind("bhop_key", &variables::Movement::bunnyHopKey, ImVec2(222.0f, ly - 1.0f), ImVec2(68.0f, 13.0f), &variables::Movement::bunnyHopKeyMode);
    ly += imGuiCustom::CheckStep();
    if (tBhop > 0.02f) {
        imGuiCustom::TreeScope _ts(14.0f, ly, &ly, tBhop);
        imGuiCustom::SliderFloat("bhop_speed", &variables::Movement::bunnyHopSpeed, 16.0f, 250.0f, ImVec2(12.0f, ly + imGuiCustom::SliderTop()), 272.0f, "BunnyHop Speed", "%.0f");
        ly += imGuiCustom::SliderStep();
    }
    float tHip = imGuiCustom::TreeToggle("HipHeight", &variables::Movement::hipHeight, ImVec2(12.0f, ly));
    ly += imGuiCustom::CheckStep();
    if (tHip > 0.02f) {
        imGuiCustom::TreeScope _ts(14.0f, ly, &ly, tHip);
        imGuiCustom::SliderFloat("hip_height", &variables::Movement::hipHeightValue, 0.0f, 10.0f, ImVec2(12.0f, ly + imGuiCustom::SliderTop()), 272.0f, "HipHeight", "%.1f");
        ly += imGuiCustom::SliderStep();
    }
    float tWalk = imGuiCustom::TreeToggle("WalkSpeed", &variables::Misc::walkSpeed, ImVec2(12.0f, ly));
    ly += imGuiCustom::CheckStep();
    if (tWalk > 0.02f) {
        imGuiCustom::TreeScope _ts(14.0f, ly, &ly, tWalk);
        imGuiCustom::SliderFloat("walkspeed_value", &variables::Misc::walkSpeedValue, 16.0f, 250.0f, ImVec2(12.0f, ly + imGuiCustom::SliderTop()), 272.0f, "WalkSpeed", "%.0f");
        ly += imGuiCustom::SliderStep();
    }
    float tGrav = imGuiCustom::TreeToggle("Gravity", &variables::Misc::gravity, ImVec2(12.0f, ly));
    ly += imGuiCustom::CheckStep();
    if (tGrav > 0.02f) {
        imGuiCustom::TreeScope _ts(14.0f, ly, &ly, tGrav);
        imGuiCustom::SliderFloat("gravity_value", &variables::Misc::gravityValue, 0.0f, 500.0f, ImVec2(12.0f, ly + imGuiCustom::SliderTop()), 272.0f, "Gravity", "%.0f");
        ly += imGuiCustom::SliderStep();
    }
    float localContent = ly + localScroll - 46.0f;
    float localMaxScroll = localContent - 340.0f + 6.0f; if (localMaxScroll < 0.f) localMaxScroll = 0.f;
    if (localScroll > localMaxScroll) localScroll = localMaxScroll;
    mFg->PopClipRect();
    ImGui::PopStyleVar();
}

void RenderMiscMenu() {
    ImVec2 mBase = ImGui::GetWindowPos();
    ImVec2 mRMin = mBase + ImVec2(305.0f, 40.0f);
    ImVec2 mRMax = mBase + ImVec2(305.0f + 290.0f, 40.0f + 340.0f);
    ImVec2 mMp = ImGui::GetIO().MousePos;
    static float miscScroll = 0.f;
    if (mMp.x >= mRMin.x && mMp.x <= mRMax.x && mMp.y >= mRMin.y && mMp.y <= mRMax.y &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        float wh = ImGui::GetIO().MouseWheel;
        if (wh != 0.f && !imGuiCustom::PopupBlocking()) miscScroll -= wh * 22.0f;
    }
    if (miscScroll < 0.f) miscScroll = 0.f;
    ImDrawList* mFgR = ImGui::GetWindowDrawList();
    mFgR->PushClipRect(mRMin, mRMax, true);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
    float ry = 46.0f - miscScroll;
    imGuiCustom::Checkbox("Team Check", &variables::teamCheck, ImVec2(311.0f, ry));
    imGuiCustom::Keybind("team_key", &variables::teamCheckKey, ImVec2(505.0f, ry - 1.0f), ImVec2(68.0f, 13.0f), &variables::teamCheckKeyMode);
    ry += imGuiCustom::CheckStep();
    imGuiCustom::Checkbox("Stream Proof", &variables::Misc::streamProof, ImVec2(311.0f, ry));
    imGuiCustom::Keybind("stream_key", &variables::Misc::streamKey, ImVec2(505.0f, ry - 1.0f), ImVec2(68.0f, 13.0f), &variables::Misc::streamKeyMode);
    ry += imGuiCustom::CheckStep();
    imGuiCustom::Checkbox("Watermark", &variables::Misc::watermark, ImVec2(311.0f, ry));
    imGuiCustom::Keybind("watermark_key", &variables::Misc::watermarkKey, ImVec2(505.0f, ry - 1.0f), ImVec2(68.0f, 13.0f), &variables::Misc::watermarkKeyMode);
    ry += imGuiCustom::CheckStep();
    imGuiCustom::Checkbox("Keybinds", &variables::Misc::keybinds, ImVec2(311.0f, ry));
    imGuiCustom::Keybind("keybinds_key", &variables::Misc::keybindsKey, ImVec2(505.0f, ry - 1.0f), ImVec2(68.0f, 13.0f), &variables::Misc::keybindsKeyMode);
    ry += imGuiCustom::CheckStep();
    imGuiCustom::Checkbox("Panic", &variables::Misc::panicBound, ImVec2(311.0f, ry));
    imGuiCustom::Keybind("panic_key", &variables::Misc::panicKey, ImVec2(505.0f, ry - 1.0f), ImVec2(68.0f, 13.0f));
    ry += imGuiCustom::CheckStep();
    imGuiCustom::Checkbox("Music Player", &variables::Music::enabled, ImVec2(311.0f, ry));
    ry += imGuiCustom::CheckStep();
    float tFov = imGuiCustom::TreeToggle("FOV", &variables::Movement::fov, ImVec2(311.0f, ry));
    imGuiCustom::Keybind("fov_key", &variables::Movement::fovKey, ImVec2(505.0f, ry - 1.0f), ImVec2(68.0f, 13.0f), &variables::Movement::fovKeyMode);
    ry += imGuiCustom::CheckStep();
    if (tFov > 0.02f) {
        imGuiCustom::TreeScope _ts(313.0f, ry, &ry, tFov);
        imGuiCustom::SliderFloat("fov_value", &variables::Movement::fovValue, 30.0f, 150.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "FOV Value", "%.0f");
        ry += imGuiCustom::SliderStep();
    }
    float tFly = imGuiCustom::TreeToggle("Fly", &variables::Movement::fly, ImVec2(311.0f, ry));
    imGuiCustom::Keybind("fly_key", &variables::Movement::flyKey, ImVec2(505.0f, ry - 1.0f), ImVec2(68.0f, 13.0f), &variables::Movement::flyKeyMode);
    ry += imGuiCustom::CheckStep();
    if (tFly > 0.02f) {
        imGuiCustom::TreeScope _ts(313.0f, ry, &ry, tFly);
        imGuiCustom::SliderFloat("fly_speed", &variables::Movement::flySpeed, 5.0f, 1000.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Fly Speed", "%.0f");
        ry += imGuiCustom::SliderStep();
        imGuiCustom::SliderFloat("fly_vert", &variables::Movement::flyVerticalBoost, 0.1f, 3.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Vertical Boost", "%.2f");
        ry += imGuiCustom::SliderStep();
        imGuiCustom::SliderFloat("fly_damping", &variables::Movement::flyDamping, 0.0f, 50.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Damping", "%.1f");
        ry += imGuiCustom::SliderStep();
        ry += imGuiCustom::ComboTop();
        const char* flyModes[] = {"Velocity", "CFrame", "CFrame Lock", "Hover"};
        imGuiCustom::Combo("fly_mode", &variables::Movement::flyMethod, flyModes, 4, ImVec2(311.0f, ry), 158.0f, "Fly Mode:");
        ry += imGuiCustom::ComboStep();
    }
    imGuiCustom::Checkbox("Spiderman", &variables::Movement::spiderman, ImVec2(311.0f, ry));
    ry += imGuiCustom::CheckStep();
    imGuiCustom::Checkbox("No Fall Damage", &variables::Movement::noFallDamage, ImVec2(311.0f, ry));
    ry += imGuiCustom::CheckStep();
    float tHitbox = imGuiCustom::TreeToggle("Hitbox Expander", &variables::Movement::hitboxExpander, ImVec2(311.0f, ry));
    ry += imGuiCustom::CheckStep();
    if (tHitbox > 0.02f) {
        imGuiCustom::TreeScope _ts(313.0f, ry, &ry, tHitbox);
        imGuiCustom::SliderFloat("hitbox_x", &variables::Movement::hitboxX, 1.0f, 50.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Size X", "%.1f");
        ry += imGuiCustom::SliderStep();
        imGuiCustom::SliderFloat("hitbox_y", &variables::Movement::hitboxY, 1.0f, 50.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Size Y", "%.1f");
        ry += imGuiCustom::SliderStep();
        imGuiCustom::SliderFloat("hitbox_z", &variables::Movement::hitboxZ, 1.0f, 50.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Size Z", "%.1f");
        ry += imGuiCustom::SliderStep();
        imGuiCustom::Checkbox("HB Team Check", &variables::Movement::hitboxTeamCheck, ImVec2(311.0f, ry));
        ry += imGuiCustom::CheckStep();
        imGuiCustom::Checkbox("HB Knock Check", &variables::Movement::hitboxKnockCheck, ImVec2(311.0f, ry));
        ry += imGuiCustom::CheckStep();
    }
    float tTick = imGuiCustom::TreeToggle("Tickrate", &variables::Movement::tickrate, ImVec2(311.0f, ry));
    ry += imGuiCustom::CheckStep();
    if (tTick > 0.02f) {
        imGuiCustom::TreeScope _ts(313.0f, ry, &ry, tTick);
        imGuiCustom::SliderFloat("tickrate_val", &variables::Movement::tickrateValue, 60.0f, 500.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Rate", "%.0f");
        ry += imGuiCustom::SliderStep();
    }

    float tNoclip = imGuiCustom::TreeToggle("Noclip", &variables::Movement::noclip, ImVec2(311.0f, ry));
    imGuiCustom::Keybind("noclip_key", &variables::Movement::noclipKey, ImVec2(505.0f, ry - 1.0f), ImVec2(68.0f, 13.0f), &variables::Movement::noclipKeyMode);
    ry += imGuiCustom::CheckStep();
    if (tNoclip > 0.02f) {
        imGuiCustom::TreeScope _ts(313.0f, ry, &ry, tNoclip);
        ry += imGuiCustom::ComboTop();
        const char* noclipModes[] = {"All Parts", "Root Only"};
        imGuiCustom::Combo("noclip_mode", &variables::Movement::noclipMode, noclipModes, 2, ImVec2(311.0f, ry), 158.0f, "Noclip Mode:");
        ry += imGuiCustom::ComboStep();
    }
    float tFcam = imGuiCustom::TreeToggle("Freecam", &variables::Freecam::enabled, ImVec2(311.0f, ry));
    imGuiCustom::Keybind("freecam_key", &variables::Freecam::key, ImVec2(505.0f, ry - 1.0f), ImVec2(68.0f, 13.0f), &variables::Freecam::keyMode);
    ry += imGuiCustom::CheckStep();
    if (tFcam > 0.02f) {
        imGuiCustom::TreeScope _ts(313.0f, ry, &ry, tFcam);
        imGuiCustom::SliderFloat("fcam_speed", &variables::Freecam::speed, 5.0f, 300.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Speed", "%.0f");
        ry += imGuiCustom::SliderTop() + 15.0f;
        imGuiCustom::SliderFloat("fcam_sens", &variables::Freecam::sensitivity, 0.001f, 0.02f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Sensitivity", "%.4f");
        ry += imGuiCustom::SliderTop() + 15.0f;
        imGuiCustom::SliderFloat("fcam_smooth", &variables::Freecam::smoothing, 0.0f, 0.95f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Smoothing", "%.2f");
        ry += imGuiCustom::SliderTop() + 15.0f;
        imGuiCustom::SliderFloat("fcam_curve", &variables::Freecam::speedCurve, 0.5f, 3.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "Shift Curve", "%.2f");
        ry += imGuiCustom::SliderTop() + 15.0f;
        imGuiCustom::Checkbox("FOV Override##fcam_fov", &variables::Freecam::fovOverride, ImVec2(311.0f, ry));
        ry += imGuiCustom::CheckStep();
        if (variables::Freecam::fovOverride) {
            imGuiCustom::SliderFloat("fcam_fovval", &variables::Freecam::fovValue, 30.0f, 120.0f, ImVec2(311.0f, ry + imGuiCustom::SliderTop()), 272.0f, "FOV", "%.0f");
            ry += imGuiCustom::SliderTop() + 15.0f;
        }
    }
    float miscContent = ry + miscScroll - 46.0f;
    float miscMaxScroll = miscContent - 340.0f + 6.0f; if (miscMaxScroll < 0.f) miscMaxScroll = 0.f;
    if (miscScroll > miscMaxScroll) miscScroll = miscMaxScroll;
    mFgR->PopClipRect();
    ImGui::PopStyleVar();
}
}
