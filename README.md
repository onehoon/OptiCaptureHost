# OptiCaptureHost

OptiCaptureHost is a modified development fork built around a Windows Graphics Capture baseline for OptiScaler input. It removes fork-specific graphics controls and supplies color-derived inputs through the public FidelityFX DX12 API. Hook interception, selected-upscaler evaluation and frame generation still require the maintainer's runtime verification.

## Current scope

- `OptiCaptureLauncher.exe` lists visible windows, lets you select a target, choose native or scaled target resolution, start or stop capture, and configure host shortcuts.
- `OptiCaptureHost.exe` captures the selected window with Windows Graphics Capture, shares the image between D3D11 and D3D12, and displays it in a borderless overlay.
- Native mode leaves the target window size unchanged. Scaled target-resolution choices resize the target while capturing and restore its prior size when capture stops.
- The host generates color-derived motion vectors, a flat synthetic depth input and a reactive mask, then supplies them through the public FidelityFX DX12 API. OptiScaler owns the selected upscaler and all frame-generation, multiplier, VSync, frame-limit, latency, overlay and graphics settings.
- The launcher only reads OptiScaler's menu shortcut when relaying the menu key; it does not write or copy `OptiScaler.ini`.

## Limitations and status

- Use a windowed or borderless game. Exclusive fullscreen capture is not supported.
- Motion vectors are estimated from captured color frames and depth is flat synthetic depth; neither is game-provided data. Jitter is not synthesized.
- The host sends a native FidelityFX upscaler context and dispatch through the same D3D12 device, command list and queue used for presentation. Hook interception and successful OptiScaler evaluation still require the maintainer's runtime verification.
- Native frame-generation-only passthrough is not implemented. No OptiScaler, FidelityFX, XeSS, XeFG, XeLL, DLSS or other runtime DLLs are included in this repository or the development ZIP.

## Manual OptiScaler V10 setup and maintainer checks

Runtime verification is for the maintainer. Codex validation is limited to source review, compilation and CI/package checks; host, game, OptiScaler, smoke and GPU tests are not run by Codex.

Install the unmodified official OptiScaler V10 `dxgi.dll` beside `OptiCaptureHost.exe`, never in the target game's folder. Supply a compatible FidelityFX runtime next to the host: prefer a matching `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` pair, or use the supported monolithic `amd_fidelityfx_dx12.dll` fallback. Do not mix DLLs from different runtime packages. The host does not download, copy or configure these files.

Use OptiScaler's supplied INI, manual edits or its overlay. For the initial XeSS input check, start with a scaled target such as 67% input to native monitor output, and use:

```ini
[Upscalers]
Dx12Upscaler=xess

[Inputs]
EnableFfxInputs=true
UseFfxInputs=true
EnableHotSwapping=false

[FrameGen]
Enabled=false
FGInput=upscaler
FGOutput=xefg

[XeFG]
InterpolationCount=1
DynamicMFG=false
```

1. Without OptiScaler/FFX, check native and scaled capture, repeated start/stop, target resize and target close.
2. With FG disabled, verify OptiScaler's official log and overlay show FFX DX12 input, XeSS output, successful feature creation/evaluation and visibly upscaled output. Confirm the displayed image is not the raw neutral blit or an unhooked standalone AMD upscaler.
3. Enable FG in OptiScaler and restart the host when its swapchain settings require it. XeFG requires `libxess_fg.dll` and `libxell.dll`; XeSS requires its matching SDK. Confirm XeFG initializes and generated frames are presented at 2x, then test 3x and 4x where the SDK and GPU support them (`InterpolationCount=1`, `2` and `3`). Do not add unlocker support, force `IgnoreInitChecks`, or select unsupported backends/GPU modes.
4. FSR4 can be checked with `Dx12Upscaler=ffx` and the official supported provider/model configuration when compatible hardware and runtime are available. DLSS on compatible RTX hardware is an optional later check, not an Intel PoC requirement.
5. Record source capture FPS separately from displayed/generated FPS, official logs, visual behavior, HUD/ghosting, menu/focus toggles, alt-tab and restart behavior. Report FSR4 only when confirmed on a supported runtime.

Runtime validation remains pending maintainer testing. No automatic INI modification or launcher graphics settings are provided.

For OptiScaler setup, obtain the official project and follow its own installation instructions: [optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler). This host does not install, update, or configure OptiScaler. Set source-game frame limits in the game itself; OptiScaler limits the host process.

## Build

Requirements: Windows 10 version 1903 or newer, a Direct3D 12 capable GPU, Visual Studio 2022 with Desktop development with C++, and CMake 3.20 or newer.

```powershell
git clone https://github.com/onehoon/OptiCaptureHost.git
cd OptiCaptureHost
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executables are written to `build/Release`. GitHub Actions compiles both executables for pull requests targeting `feature/optiscaler-upstream` or `refactor/upstream-provider-cleanup`; PR builds do not upload artifacts. To package a development ZIP, use **Actions → build → Run workflow**. The manual run uploads one ZIP containing both executables, this README, the GPL license, the notice, and the retained FidelityFX license. GitHub Actions must be enabled for this fork under **Settings → Actions → General** for these workflows to run.

## License and provenance

OptiCaptureHost is distributed under [GNU GPL version 3](LICENSE). The original project copyright attribution is retained in source and executable metadata. This development fork is modified as of 2026-11-24. See [NOTICE.md](NOTICE.md) for provenance and third-party notices.
