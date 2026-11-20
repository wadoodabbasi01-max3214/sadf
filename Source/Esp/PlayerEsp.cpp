#include "Pch.h"
#include "PlayerEsp.h"
#include "EspDraw.h"
#include "../SDK/RobRunReader.h"
#include "../LocalMenu.h"
#include "../Overlay/Overlay.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>

namespace {
struct SmoothedBounds {
    ImVec2 min{};
    ImVec2 max{};
    Vector3 worldPosition{};
    std::chrono::steady_clock::time_point updated{};
    std::uint64_t lastFrame = 0;
    bool inVehicle = false;
    bool initialized = false;
};

LocalMenu::Group GroupFor(const RobRunReader::EntityData& entity) {
    std::string role = entity.roleName;
    std::transform(role.begin(), role.end(), role.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (role.find("robber") != std::string::npos || role.find("criminal") != std::string::npos)
        return LocalMenu::Group::Robber;
    if (role.find("cop") != std::string::npos || role.find("police") != std::string::npos)
        return LocalMenu::Group::Cop;
    return LocalMenu::Group::Civilian;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), count, nullptr, nullptr);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

void SmoothBounds(SmoothedBounds& state, const ImVec2& targetMin, const ImVec2& targetMax,
    const Vector3& worldPosition, bool inVehicle, bool cameraMoving,
    std::uint64_t frame, std::chrono::steady_clock::time_point now) {
    const float worldDelta = state.initialized ? Distance(state.worldPosition, worldPosition) : 0.f;
    if (!state.initialized || state.inVehicle != inVehicle || cameraMoving || worldDelta > 0.035f || worldDelta > 2.0f) {
        state.min = targetMin;
        state.max = targetMax;
        state.worldPosition = worldPosition;
        state.updated = now;
        state.lastFrame = frame;
        state.inVehicle = inVehicle;
        state.initialized = true;
        return;
    }

    const bool stationary = worldDelta < 0.012f;
    const float timeConstant = stationary ? 0.075f : 0.035f;
    float dt = std::chrono::duration<float>(now - state.updated).count();
    dt = (std::clamp)(dt, 0.001f, 0.12f);
    const float alpha = 1.f - std::exp(-dt / timeConstant);
    const float delta = (std::max)({ std::fabs(targetMin.x - state.min.x), std::fabs(targetMin.y - state.min.y),
        std::fabs(targetMax.x - state.max.x), std::fabs(targetMax.y - state.max.y) });
    if (!stationary || delta >= 0.65f) {
        state.min.x += (targetMin.x - state.min.x) * alpha;
        state.min.y += (targetMin.y - state.min.y) * alpha;
        state.max.x += (targetMax.x - state.max.x) * alpha;
        state.max.y += (targetMax.y - state.max.y) * alpha;
    }
    state.worldPosition = worldPosition;
    state.updated = now;
    state.lastFrame = frame;
}

void DrawOutlinedText(ImDrawList* draw, const std::string& text, float centerX, float y, ImU32 color) {
    if (text.empty()) return;
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pos(centerX - size.x * 0.5f, y);
    const ImU32 shadow = IM_COL32(0, 0, 0, 230);
    draw->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), shadow, text.c_str());
    draw->AddText(pos, color, text.c_str());
}

void DrawPlayerLabels(ImDrawList* draw, const RobRunReader::EntityData& entity,
    LocalMenu::Group group, bool showRole, bool showName, bool showHeldItem, float distance,
    const ImVec2& min, const ImVec2& max) {
    const float centerX = (min.x + max.x) * 0.5f;
    const std::string name = showName ? ToUtf8(entity.nickname) : std::string{};
    float y = min.y - 4.f;
    if (showName && !name.empty()) {
        const float height = ImGui::GetFontSize();
        y -= height;
        DrawOutlinedText(draw, name, centerX, y, IM_COL32(248, 244, 250, 255));
        y -= 2.f;
    }
    if (showRole) {
        ImU32 roleColor = IM_COL32(90, 220, 125, 255);
        const char* roleText = "Civilian";
        if (group == LocalMenu::Group::Robber) {
            roleColor = IM_COL32(255, 82, 86, 255);
            roleText = "Robber";
        } else if (group == LocalMenu::Group::Cop) {
            roleColor = IM_COL32(90, 160, 255, 255);
            roleText = "Cop";
        }
        y -= ImGui::GetFontSize();
        DrawOutlinedText(draw, roleText, centerX, y, roleColor);
    }

    float bottomY = max.y + 3.f;
    if (showHeldItem && !entity.weaponName.empty()) {
        const std::string item = ToUtf8(entity.weaponName);
        if (!item.empty()) {
            DrawOutlinedText(draw, item, centerX, bottomY, IM_COL32(232, 225, 242, 255));
            bottomY += ImGui::GetFontSize() + 2.f;
        }
    }

    char distanceText[32]{};
    std::snprintf(distanceText, sizeof(distanceText), "%.0f m", distance);
    DrawOutlinedText(draw, distanceText, centerX, bottomY, IM_COL32(235, 232, 240, 255));
}

void DrawVehicleEsp(ImDrawList* draw, const RobRunReader::Snapshot& snapshot,
    const std::function<bool(const Vector3&, float&, float&)>& project) {
    const bool showBox = LocalMenu::CarBoxesEnabled();
    const bool showName = LocalMenu::CarNamesEnabled();
    const bool showDistance = LocalMenu::CarDistanceEnabled();
    if ((!showBox && !showName && !showDistance) || (!snapshot.vehicles && (!snapshot.world || !snapshot.world->valid)))
        return;

    constexpr float halfLength = 2.15f;
    constexpr float halfWidth = 0.95f;
    constexpr float boxHeight = 1.65f;
    const ImU32 color = IM_COL32(209, 102, 207, 245);
    std::int32_t localVehicleId = 0;
    for (const auto& player : snapshot.entities) {
        if (player.entityId == snapshot.localEntityId && player.inVehicle) {
            localVehicleId = player.vehicleId;
            break;
        }
    }
    const auto& vehicles = snapshot.vehicles ? *snapshot.vehicles : snapshot.world->entities;
    for (const auto& vehicle : vehicles) {
        if (vehicle.kind != RobRunWorld::Kind::Vehicle ||
            (localVehicleId > 0 && vehicle.id == localVehicleId) ||
            vehicle.position.IsZero()) continue;

        // Car transforms are rooted at the body base. Project all eight corners
        // so the renderer draws a true 3D prism instead of a flat 2D rectangle.
        std::array<Vector2, 8> screen{};
        bool valid[8]{};
        const float cosine = vehicle.hasYaw ? std::cos(vehicle.yaw) : 1.f;
        const float sine = vehicle.hasYaw ? std::sin(vehicle.yaw) : 0.f;
        float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        for (int i = 0; i < 8; ++i) {
            const float localX = (i & 1) ? halfLength : -halfLength;
            const float y = (i & 2) ? boxHeight : 0.f;
            const float localZ = (i & 4) ? halfWidth : -halfWidth;
            const float x = cosine * localX + sine * localZ;
            const float z = -sine * localX + cosine * localZ;
            const Vector3 corner(vehicle.position.x + x, vehicle.position.y + y,
                vehicle.position.z + z);
            float sx = 0.f, sy = 0.f;
            valid[i] = project(corner, sx, sy);
            if (valid[i]) {
                screen[i] = Vector2(sx, sy);
                minX = (std::min)(minX, sx); minY = (std::min)(minY, sy);
                maxX = (std::max)(maxX, sx); maxY = (std::max)(maxY, sy);
            }
        }
        int visibleCorners = 0;
        for (bool cornerValid : valid) if (cornerValid) ++visibleCorners;
        if (visibleCorners < 4 || !std::isfinite(minX) || !std::isfinite(maxX)) continue;

        if (showBox) {
            for (int i = 0; i < 8; ++i) {
                for (int axis : { 1, 2, 4 }) {
                    const int j = i ^ axis;
                    if (i < j && valid[i] && valid[j])
                        draw->AddLine(ImVec2(screen[i].x, screen[i].y),
                            ImVec2(screen[j].x, screen[j].y), color, 1.5f);
                }
            }
        }

        const float centerX = (minX + maxX) * 0.5f;
        const float distance = Distance(snapshot.localPosition, vehicle.position);
        if (showName) {
            const std::string label = vehicle.label.empty() ? "Car" : vehicle.label;
            DrawOutlinedText(draw, label, centerX, minY - ImGui::GetFontSize() - 3.f,
                IM_COL32(248, 244, 250, 255));
        }
        if (showDistance) {
            char text[32]{};
            std::snprintf(text, sizeof(text), "%.0f m", distance);
            DrawOutlinedText(draw, text, centerX, maxY + 3.f, IM_COL32(235, 232, 240, 255));
        }
    }
}
}

void PlayerEsp::Draw() {
    const auto snapshot = RobRunReader::GetSnapshot();
    if (!snapshot) return;
    Matrix4x4 vp{};
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const int width = static_cast<int>(display.x), height = static_cast<int>(display.y);
    if (width < 2 || height < 2 || !RobRunReader::ResolveViewProjection(vp, width, height)) return;
    const auto project = [&vp, width, height](const Vector3& p, float& x, float& y) {
        Vector2 out{};
        if (!RobRunReader::ProjectWorld(vp, p, width, height, out)) return false;
        x = out.x; y = out.y; return true;
    };

    static Matrix4x4 previousViewProjection{};
    static bool havePreviousViewProjection = false;
    float cameraMatrixDelta = 0.f;
    if (havePreviousViewProjection) {
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                cameraMatrixDelta = (std::max)(cameraMatrixDelta,
                    std::fabs(vp.matrix[row][col] - previousViewProjection.matrix[row][col]));
    }
    const bool cameraMoving = havePreviousViewProjection && cameraMatrixDelta > 0.001f;
    previousViewProjection = vp;
    havePreviousViewProjection = true;

    static std::unordered_map<std::int32_t, SmoothedBounds> boundsCache;
    static std::uint64_t frame = 0;
    ++frame;
    const auto now = std::chrono::steady_clock::now();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const auto& entity : snapshot->entities) {
        if (entity.isDead || entity.entityId == snapshot->localEntityId || entity.position.IsZero()) continue;
        const LocalMenu::Group group = GroupFor(entity);
        const bool drawBox = LocalMenu::BoxesEnabled(group);
        const bool drawName = LocalMenu::NamesEnabled(group);
        const bool drawRole = LocalMenu::RolesEnabled(group);
        const bool drawHeldItem = LocalMenu::HeldItemEnabled(group);
        const bool drawSkeleton = LocalMenu::SkeletonsEnabled(group);
        if (!drawBox && !drawName && !drawRole && !drawHeldItem && !drawSkeleton) continue;

        float left = FLT_MAX, top = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;
        int projected = 0;
        // Use every cached bone point for bounds; visibility flags can flicker
        // with animator culling and should not make the rectangle jump.
        for (const auto& bone : entity.bones) {
            for (const Vector3* p : { &bone.a, &bone.b }) {
                float x = 0.f, y = 0.f;
                if (project(*p, x, y)) {
                    left = (std::min)(left, x); top = (std::min)(top, y);
                    right = (std::max)(right, x); bottom = (std::max)(bottom, y);
                    ++projected;
                }
            }
        }

        ImVec2 mn{}, mx{}, fallbackMin{}, fallbackMax{};
        const Vector3 boundsAnchor = entity.position;
        const Vector3 fallbackCenter(entity.position.x, entity.position.y + 0.95f, entity.position.z);
        const bool fallbackValid = Esp::ProjectWorldAABB(project, fallbackCenter,
            0.35f, 0.95f, fallbackMin, fallbackMax, 6.f);
        const bool bonesValid = projected >= 2 && right - left >= 4.f && bottom - top >= 6.f;
        bool valid = bonesValid || fallbackValid;
        if (!valid) continue;

        if (!bonesValid) {
            mn = fallbackMin;
            mx = fallbackMax;
        } else {
            mn = ImVec2(left, top);
            mx = ImVec2(right, bottom);
            if (fallbackValid) {
                const float boneHeight = bottom - top;
                const float expectedHeight = fallbackMax.y - fallbackMin.y;
                const float expectedWidth = fallbackMax.x - fallbackMin.x;
                const float boneWidth = right - left;
                // Partial skeleton samples (often just head and neck) are not
                // valid box bounds. Fall back to the projected body volume.
                const float minimumHeightRatio = entity.inVehicle ? 0.32f : 0.55f;
                const float minimumWidthRatio = entity.inVehicle ? 0.20f : 0.28f;
                if (boneHeight < expectedHeight * minimumHeightRatio || boneWidth < expectedWidth * minimumWidthRatio) {
                    mn = fallbackMin;
                    mx = fallbackMax;
                }
            } else if (bottom - top < 20.f) {
                // With no usable body fallback, suppress a tiny head/neck box.
                continue;
            }
        }
        if (!valid || mx.x < 0.f || mx.y < 0.f || mn.x > display.x || mn.y > display.y) continue;

        auto& smoothed = boundsCache[entity.entityId];
        SmoothBounds(smoothed, mn, mx, boundsAnchor, entity.inVehicle, cameraMoving, frame, now);
        mn = smoothed.min;
        mx = smoothed.max;
        if (drawSkeleton) {
            const ImU32 skeletonColor = IM_COL32(225, 205, 245, 235);
            for (const auto& bone : entity.bones) {
                const float worldDx = bone.b.x - bone.a.x;
                const float worldDy = bone.b.y - bone.a.y;
                const float worldDz = bone.b.z - bone.a.z;
                if (worldDx * worldDx + worldDy * worldDy + worldDz * worldDz > 1.21f) continue;
                // End the visible rig at the neck; the head-top/head chain is
                // the source of the stray line that used to extend upward.
                if (entity.bonePoints.hasNeck) {
                    const float neckLimit = entity.bonePoints.neck.y + 0.025f;
                    if (bone.a.y > neckLimit || bone.b.y > neckLimit) continue;
                }
                float ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
                if (!project(bone.a, ax, ay) || !project(bone.b, bx, by)) continue;
                const float dx = bx - ax, dy = by - ay;
                // Ignore corrupt/stale links that would draw long lines across the screen.
                if (dx * dx + dy * dy > display.x * display.x + display.y * display.y) continue;
                dl->AddLine(ImVec2(ax, ay), ImVec2(bx, by), skeletonColor, 1.35f);
            }
        }
        if (drawBox) Esp::DrawCornerBox(dl, mn, mx, IM_COL32(209, 102, 207, 255), 1.5f, 25.f, true);
        const float distance = Distance(snapshot->localPosition, entity.position);
        DrawPlayerLabels(dl, entity, group, drawRole, drawName, drawHeldItem, distance, mn, mx);
    }

    for (auto it = boundsCache.begin(); it != boundsCache.end();) {
        if (frame - it->second.lastFrame > 300) it = boundsCache.erase(it);
        else ++it;
    }
    DrawVehicleEsp(dl, *snapshot, project);
}
