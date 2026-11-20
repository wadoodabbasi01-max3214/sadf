#include "Misc/Pch/Pch.h"
#include "Memory/Memory.h"
#include "SDK/CameraFinder.h"
#include "SDK/RobRunReader.h"
#include "Overlay/Overlay.h"
#include "Esp/PlayerEsp.h"
#include "AimSystem.h"
#include "LocalMenu.h"
#include "ImGUI/imgui.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (!TargetProcess.Init("ROB RUN.exe", false, false)) {
        MessageBoxW(nullptr, L"Could not initialize the DMA reader or find ROB RUN.exe.", L"RiggaRun", MB_OK | MB_ICONERROR);
        return 1;
    }
    const auto gameAssembly = TargetProcess.GetBaseAddress("GameAssembly.dll");
    const auto unityPlayer = TargetProcess.GetBaseAddress("UnityPlayer.dll");
    if (!gameAssembly || !unityPlayer) {
        MessageBoxW(nullptr, L"The game modules were not found. Start the single-player match, then launch RiggaRun.", L"RiggaRun", MB_OK | MB_ICONERROR);
        return 1;
    }
    CameraFinder::Init(unityPlayer);
    RobRunReader::Init(gameAssembly, unityPlayer);
    if (!LocalMenu::Start()) {
        RobRunReader::Shutdown();
        MessageBoxW(nullptr, L"Could not start the local menu. Check that a private Ethernet or Wi-Fi IPv4 address is available and port 17845 is free.", L"RiggaRun", MB_OK | MB_ICONERROR);
        return 1;
    }
    const std::string menuUrl = LocalMenu::Url();
    const std::string message = "On this PC: http://127.0.0.1:17845/\nOn other PCs on your network: " + menuUrl +
        "\n\nIf another PC cannot connect, allow RiggaRun through Windows Firewall on Private networks.";
    MessageBoxA(nullptr, message.c_str(), "RiggaRun Network Menu", MB_OK | MB_ICONINFORMATION);
    Overlay::Callbacks callbacks;
    callbacks.init = [] { AimSystem::Start(); };
    callbacks.tick = [] {
        static bool applied = false;
        static bool lastTransparent = false;
        static bool lastBlack = false;
        const bool transparent = LocalMenu::TransparentBackground();
        const bool black = LocalMenu::BlackBackground();
        if (!applied || transparent != lastTransparent) Overlay::SetTransparent(transparent);
        if (!applied || black != lastBlack) Overlay::SetBlackBackground(black);
        lastTransparent = transparent;
        lastBlack = black;
        applied = true;
        if (LocalMenu::ConsumeExitRequest()) Overlay::RequestExit();
        RobRunReader::RefreshCamera();
    };
    callbacks.draw = [] { PlayerEsp::Draw(); AimSystem::DrawOverlay(); };
    callbacks.shutdown = [] { AimSystem::Stop(); RobRunReader::Shutdown(); LocalMenu::Stop(); };
    callbacks.status = [] { return std::string("Read-only DMA | ESP + Makcu input"); };
    return Overlay::Run(instance, L"RiggaRun", L"RiggaRunOverlayWindow", callbacks);
}
