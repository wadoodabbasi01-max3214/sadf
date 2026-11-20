# RiggaRun

A standalone Windows x64 player overlay with per-role box, skeleton, name, role, and held-item toggles, plus range labels. Vehicle occupants keep player-sized bounds. The Aimbot tab selects cached player bones and exposes FOV, key, smoothness, deadzone, dynamic ADS FOV, and separate ADS smoothness controls. Input is sent through the MAKCU serial API. Screen-space easing yields immediately to camera motion so labels do not trail. The control panel also has a blank World tab and Settings for background transparency, background color, and stopping RiggaRun. It includes no hand chams or other chams controls.

The control page is served on the active private Ethernet/Wi-Fi IPv4 address at port **17845**, so other devices on your local network can open it. RiggaRun displays the exact URL at startup. `127.0.0.1` works only on the host PC. If you want the host's LAN address to stay fixed, create a DHCP reservation for that PC in your router; if Windows Firewall prompts, allow RiggaRun on **Private networks** only. The service does not bind to public IPv4 addresses.

## Build

Open `RiggaRun.sln` in Visual Studio and build `Debug|x64` or `Release|x64`. The DMA runtime DLLs are copied beside the executable after build. Start the single-player match before launching the program.

## Memory scope

The included game-data path reads target-process data through the DMA reader. The aim worker reads the same immutable player snapshots and sends relative mouse input only through the MAKCU API; it does not write to the target process or inject code. The DMA initialization retains its FPGA configuration command; that configures the DMA device and is separate from writing game-process memory. The vendored SDK header still contains unused write-function declarations.


## One-click Visual Studio build

On a Windows PC with Visual Studio 2022 and the **Desktop development with C++** workload installed, run `Build-Release-x64.bat` from File Explorer or a Developer Command Prompt. It builds `Release|x64`. The executable is written to `bin\\Release\\RiggaRun.exe`; the project post-build step copies the three required runtime DLLs beside it. This script does not install drivers or configure the DMA/MAKCU hardware.

## Build online for free with GitHub Actions

1. Create a **private** repository on GitHub.
2. Upload the *contents* of this `RiggaRun` folder to the repository root (so `RiggaRun.sln` is at the top level). Include the `.github` folder; it contains the Windows build workflow.
3. Open the repository's **Actions** tab.
4. Select **Build RiggaRun (Windows x64)**, click **Run workflow**, and wait for the job to finish.
5. Open the successful workflow run and download the `RiggaRun-Release-x64` artifact. It contains the executable and the DLLs copied by the project build.

Keep the repository private if you do not want the source code to be publicly visible. GitHub Actions usage for private repositories is subject to the included usage allowance and billing limits for your plan.
