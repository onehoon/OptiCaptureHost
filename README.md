# OptiCaptureHost

OptiCaptureHost is a modified development fork exploring a Windows Graphics Capture baseline for an eventual upstream OptiScaler input provider. PR1 keeps the capture and display host buildable while removing fork-specific graphics controls. It does not establish compatibility with official OptiScaler V10.

## Current scope

- `OptiCaptureLauncher.exe` lists visible windows, lets you select a target, choose native or scaled target resolution, start or stop capture, and configure host shortcuts.
- `OptiCaptureHost.exe` captures the selected window with Windows Graphics Capture, shares the image between D3D11 and D3D12, and displays it in a borderless overlay.
- Native mode leaves the target window size unchanged. Scaled target-resolution choices resize the target while capturing and restore its prior size when capture stops.
- The host retains its GPU optical-flow and FFX temporal scaffold, including estimated motion vectors and synthetic depth. Those inputs and their interoperability with OptiScaler remain experimental and are not validated by PR1.
- OptiScaler owns upscaling, frame generation, MFG, VSync, frame limits, latency and graphics settings. The launcher only reads OptiScaler's menu shortcut when relaying the menu key; it does not write or copy `OptiScaler.ini`.

## Limitations and status

- Use a windowed or borderless game. Exclusive fullscreen capture is not supported.
- Motion vectors are estimated from captured color frames and depth is synthetic; they are not game-provided data.
- Official OptiScaler V10 integration has not been verified. Native frame-generation-only passthrough is not implemented.
- A compatible FidelityFX runtime may be supplied manually beside the executables to exercise the retained temporal scaffold. No runtime DLLs are included in this repository or the development ZIP.
- Runtime smoke checks should cover capture with and without OptiScaler, target resize and close, and repeated start/stop. They require a Windows desktop and a suitable target application.

For OptiScaler setup, obtain the official project and follow its own installation instructions: [optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler). This host does not install, update, or configure OptiScaler. Set source-game frame limits in the game itself; OptiScaler limits the host process.

## Build

Requirements: Windows 10 version 1903 or newer, a Direct3D 12 capable GPU, Visual Studio 2022 with Desktop development with C++, and CMake 3.20 or newer.

```powershell
git clone https://github.com/onehoon/OptiCaptureHost.git
cd OptiCaptureHost
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executables are written to `build/Release`. The manually dispatched GitHub Actions workflow builds Windows x64 Release and uploads one ZIP containing both executables, this README, the GPL license, the notice, and the retained FidelityFX license.

## License and provenance

OptiCaptureHost is distributed under [GNU GPL version 3](LICENSE). The original project copyright attribution is retained in source and executable metadata. This development fork is modified as of 2026-11-24. See [NOTICE.md](NOTICE.md) for provenance and third-party notices.
