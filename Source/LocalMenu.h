#pragma once
#include <string>
#include <cstdint>

namespace LocalMenu {
enum class Group : int { Civilian = 0, Robber = 1, Cop = 2 };
struct AimSettings {
    bool enabled = false;
    bool showFov = true;
    bool showDeadzoneFov = false;
    bool dynamicFov = false;
    bool adsSmoothnessEnabled = false;
    bool makcuConnected = false;
    int aimKey = 0x05; // XBUTTON1 / Mouse 4
    std::uint32_t boneMask = 0x0Fu; // Head, neck, chest, pelvis
    float smoothness = 6.f;
    float fov = 20.f;
    float deadzone = 0.f;
    float adsFov = 10.f;
    float adsSmoothness = 6.f;
    float inputGain = 1.f;
};
bool Start();
void Stop();
bool BoxesEnabled(Group group);
bool NamesEnabled(Group group);
bool RolesEnabled(Group group);
bool HeldItemEnabled(Group group);
bool SkeletonsEnabled(Group group);
bool CarBoxesEnabled();
bool CarNamesEnabled();
bool CarDistanceEnabled();
void SetFeature(Group group, const std::string& feature, bool enabled);
bool TransparentBackground();
bool BlackBackground();
bool ConsumeExitRequest();
AimSettings GetAimSettings();
bool SetAimSetting(const std::string& field, const std::string& value);
void SetAimStatus(const std::string& status);
std::string GetAimStatus();
std::string Url();
}
