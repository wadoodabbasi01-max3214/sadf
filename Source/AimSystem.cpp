#include "Misc/Pch/Pch.h"
#include "AimSystem.h"
#include "LocalMenu.h"
#include "MakcuApi.h"
#include "SDK/RobRunReader.h"
#include "ImGUI/imgui.h"
#include <atomic>
#include <cmath>
#include <thread>

namespace {
std::atomic<bool> g_running{ false };
std::atomic<bool> g_ads{ false };
std::atomic<int> g_viewWidth{ 0 };
std::atomic<int> g_viewHeight{ 0 };
std::thread g_worker;

constexpr float kPi = 3.14159265358979323846f;

float VerticalFov(const RobRunReader::Snapshot& snapshot) {
	const float fov = snapshot.camera.verticalFov;
	return std::isfinite(fov) && fov > 1.f && fov < 179.f ? fov : 60.f;
}

float AngularRadius(float pixels, float height, float cameraFov) {
	const float tanHalf = std::tan(cameraFov * kPi / 360.f);
	return std::atan((pixels * 2.f / height) * tanHalf) * 180.f / kPi;
}

float FovRadiusPixels(float degrees, float height, float cameraFov) {
	const float denominator = std::tan(cameraFov * kPi / 360.f);
	if (height <= 0.f || denominator <= 0.f) return 0.f;
	const float radians = (std::clamp)(degrees, 0.f, 80.f) * kPi / 180.f;
	return std::tan(radians) / denominator * height * 0.5f;
}

struct AimPoint { const Vector3* position; };

bool ChooseTarget(const RobRunReader::Snapshot& snapshot, const Matrix4x4& vp,
	int width, int height, std::uint32_t mask, float fovDegrees,
	Vector2& screenError, float& angularError) {
	const Vector2 center(width * 0.5f, height * 0.5f);
	const float cameraFov = VerticalFov(snapshot);
	float bestAngle = fovDegrees;
	bool found = false;
	for (const auto& entity : snapshot.entities) {
		if (entity.isDead || entity.entityId == snapshot.localEntityId || !entity.bonePoints.valid) continue;
		const auto& p = entity.bonePoints;
		const AimPoint points[] = {
			{ p.hasHead ? &p.head : nullptr },
			{ p.hasNeck ? &p.neck : nullptr },
			{ p.hasChest ? &p.chest : nullptr },
			{ p.hasPelvis ? &p.pelvis : nullptr }
		};
		for (unsigned i = 0; i < 4; ++i) {
			if ((mask & (1u << i)) == 0 || !points[i].position) continue;
			Vector2 projected{};
			if (!RobRunReader::ProjectWorld(vp, *points[i].position, width, height, projected)) continue;
			const Vector2 error = projected - center;
			const float angle = AngularRadius(std::sqrt(error.x * error.x + error.y * error.y),
				static_cast<float>(height), cameraFov);
			if (!std::isfinite(angle) || angle > bestAngle) continue;
			bestAngle = angle;
			screenError = error;
			angularError = angle;
			found = true;
		}
	}
	return found;
}

void Worker() {
	MakcuApi makcu;
	std::string connectedPort;
	ULONGLONG nextConnect = 0;
	ULONGLONG nextButtonPoll = 0;
	bool wasRequested = true;
	unsigned buttonReadFailures = 0;
	float remainderX = 0.f, remainderY = 0.f;
	while (g_running.load()) {
		const auto settings = LocalMenu::GetAimSettings();
		const ULONGLONG now = GetTickCount64();
		if (!settings.makcuConnected) {
			if (wasRequested || makcu.IsConnected()) {
				if (makcu.IsConnected()) makcu.Disconnect();
				connectedPort.clear();
				g_ads.store(false);
				LocalMenu::SetAimStatus("Makcu disconnected");
			}
			wasRequested = false;
			Sleep(25);
			continue;
		}
		wasRequested = true;
		if (!makcu.IsConnected()) {
			g_ads.store(false);
			if (now >= nextConnect) {
				LocalMenu::SetAimStatus("Searching for Makcu...");
				if (makcu.ConnectAuto(connectedPort)) {
					LocalMenu::SetAimStatus("Makcu connected · " + connectedPort);
					nextButtonPoll = 0;
				} else {
					LocalMenu::SetAimStatus("Makcu not found · searching again");
					nextConnect = GetTickCount64() + 2000;
				}
			}
			if (!makcu.IsConnected()) { Sleep(50); continue; }
		}

		bool aimHeld = false;
		bool ads = g_ads.load();
		bool inputOk = true;
		if (now >= nextButtonPoll) {
			const int aimMouseButton = settings.aimKey == 2 ? 2 : settings.aimKey == 5 ? 4 : settings.aimKey == 6 ? 5 : 0;
			bool aimMouseState = false;
			bool aimMouseRead = false;
			if (settings.enabled && aimMouseButton != 0) {
				inputOk = makcu.ButtonDown(aimMouseButton, aimMouseState);
				aimMouseRead = inputOk;
				aimHeld = aimMouseState;
			} else if (settings.enabled) {
				aimHeld = (GetAsyncKeyState(settings.aimKey) & 0x8000) != 0;
			}
			if (inputOk && (settings.dynamicFov || settings.adsSmoothnessEnabled)) {
				if (aimMouseButton == 2 && aimMouseRead) ads = aimMouseState;
				else inputOk = makcu.ButtonDown(2, ads);
			} else if (!settings.dynamicFov && !settings.adsSmoothnessEnabled) {
				ads = false;
			}
			if (inputOk) {
				const bool recovered = buttonReadFailures != 0;
				buttonReadFailures = 0;
				const bool previousAds = g_ads.exchange(ads);
				if (recovered || previousAds != ads)
					LocalMenu::SetAimStatus("Makcu connected · " + connectedPort + (ads ? " · ADS" : ""));
			} else {
				++buttonReadFailures;
				LocalMenu::SetAimStatus("Makcu button read retry " + std::to_string(buttonReadFailures) + "/3");
				if (buttonReadFailures >= 3) {
					makcu.Disconnect();
					connectedPort.clear();
					g_ads.store(false);
					LocalMenu::SetAimStatus("Makcu button read failed · reconnecting");
					nextConnect = now + 500;
					buttonReadFailures = 0;
				}
			}
			nextButtonPoll = now + 5;
		}

		if (inputOk && settings.enabled && aimHeld) {
			const int width = g_viewWidth.load();
			const int height = g_viewHeight.load();
			auto snapshot = RobRunReader::GetSnapshot();
			if (width > 1 && height > 1 && snapshot && snapshot->stampMs != 0 &&
				GetTickCount64() - snapshot->stampMs < 300) {
				Matrix4x4 vp{};
				if (RobRunReader::ResolveViewProjection(vp, width, height)) {
					const bool ads = g_ads.load();
					const float fov = settings.dynamicFov && ads ? settings.adsFov : settings.fov;
					Vector2 error{};
					float angle = 0.f;
					if (ChooseTarget(*snapshot, vp, width, height, settings.boneMask, fov, error, angle) &&
						angle > settings.deadzone) {
						const float smoothness = settings.adsSmoothnessEnabled && ads ? settings.adsSmoothness : settings.smoothness;
						const float divisor = (std::max)(1.f, smoothness);
						float moveX = error.x / divisor * settings.inputGain + remainderX;
						float moveY = error.y / divisor * settings.inputGain + remainderY;
						const int dx = (std::clamp)(static_cast<int>(std::lround(moveX)), -160, 160);
						const int dy = (std::clamp)(static_cast<int>(std::lround(moveY)), -160, 160);
						remainderX = moveX - dx;
						remainderY = moveY - dy;
						if ((dx != 0 || dy != 0) && !makcu.Move(dx, dy)) {
							makcu.Disconnect();
						LocalMenu::SetAimStatus("Makcu movement failed · reconnecting");
						nextConnect = now + 500;
						g_ads.store(false);
							remainderX = remainderY = 0.f;
						}
					} else {
						remainderX = remainderY = 0.f;
					}
				}
			}
		} else {
			remainderX = remainderY = 0.f;
		}
		Sleep(8);
	}
	makcu.Disconnect();
	g_ads.store(false);
	LocalMenu::SetAimStatus("Aim input stopped");
}
}

bool AimSystem::Start() {
	if (g_running.exchange(true)) return true;
	g_worker = std::thread(Worker);
	return true;
}

void AimSystem::Stop() {
	if (!g_running.exchange(false)) return;
	if (g_worker.joinable()) g_worker.join();
}

bool AimSystem::AdsActive() { return g_ads.load(); }

void AimSystem::DrawOverlay() {
	const auto settings = LocalMenu::GetAimSettings();
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const int width = static_cast<int>(display.x), height = static_cast<int>(display.y);
	g_viewWidth.store(width);
	g_viewHeight.store(height);
        if (width < 2 || height < 2) return;
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        draw->AddText(ImVec2(12.f, 12.f), IM_COL32(0, 0, 0, 220), "RiggaRun Loaded");
        draw->AddText(ImVec2(11.f, 11.f), IM_COL32(235, 220, 242, 240), "RiggaRun Loaded");
        if (!settings.showFov && !settings.showDeadzoneFov) return;
	const auto snapshot = RobRunReader::GetSnapshot();
	const float cameraFov = snapshot ? VerticalFov(*snapshot) : 60.f;
	const float activeFov = settings.dynamicFov && g_ads.load() ? settings.adsFov : settings.fov;
	const ImVec2 center(display.x * 0.5f, display.y * 0.5f);
	if (settings.showFov) {
		const float radius = FovRadiusPixels(activeFov, display.y, cameraFov);
		if (radius > 1.f) draw->AddCircle(center, radius, IM_COL32(204, 110, 224, 190), 96, 1.25f);
	}
	if (settings.showDeadzoneFov && settings.deadzone > 0.f) {
		const float radius = FovRadiusPixels(settings.deadzone, display.y, cameraFov);
		if (radius > 1.f) draw->AddCircle(center, radius, IM_COL32(255, 177, 104, 210), 64, 1.2f);
	}
}
