# NTE D3D11 capture handoff (2026-09-24)

## Goal and current result

The target is one replayable D3D11 `.rdc` from a playable `HTGame.exe` frame,
launched by clicking **Start Game** in the NTE launcher. This has **not** been
achieved. The only saved `.rdc` is a black transition frame (frame 662): it
reopens as D3D11, but contains only `DrawIndexed(6)` and Present.

The branch is based on RenderDoc v1.46. The opt-in **Settings > Advanced > NTE
launcher/game compatibility** mode is off by default. It uses the normal
user-mode capture library, with the launcher and RenderDic run as administrator
because the game launch requires elevation. Kernel capture and global hook are
off. No game files were changed.

## What was verified

- Normal administrator launch reaches a playable street scene. The real game
  process is `Client/WindowsNoEditor/HT/Binaries/win64/HTGame.exe`, launched from
  `NTELauncher/NTEGame.exe`; its command line contains `-d3d11`.
- With the compatibility mode and **Capture Child Processes** on, the launcher
  creates `HTGame.exe` suspended, injects `renderdic.dll`, then resumes it. The
  game log shows a wrapped D3D11 device and a registered swapchain. The overlay
  displays `Capturing D3D11`, but the game later goes black and Live Capture
  reports `D3D11 (Not Presenting)`.
- Disabling DLSS and using TAA still produced black; the user's DLSS setting was
  restored to ON/Quality. Temporarily clearing `Driver.ExtraAPILayerModules`
  (normally `sl.interposer.dll`) also stayed black; the setting was restored.
  A temporary NVAPI passthrough test also stayed black; its code was reverted.
- Two controlled source experiments did reach the playable street scene:
  skipping injection into the game child, and leaving the library injected but
  passing native D3D11 device creation through without RenderDic wrapping. Both
  diagnostic code changes were reverted. This narrows the issue to interactions
  of the D3D11 device/swapchain wrappers with game startup. No explicit
  anti-cheat rejection was observed; its role is unproven.
- A CDB snapshot found an RHI thread waiting inside
  `sl_interposer!slIsFeatureSupported`, but this does not establish the cause.
- The restored capture-enabled source built successfully with `renderdoc.sln`,
  `Development|x64`, and passed `git diff --check` on this machine.

## Changes on this branch

- Advanced setting and default-off opt-in UI (`Driver.NTEEarlyChildCapture`).
- A glslang initialization lock change to avoid an observed launcher crash
  involving the already-loaded `MSVCP140.dll`.
- Scoped `NTEGame.exe` -> `HTGame.exe` early child injection through suspended
  `CreateProcessW`, excluding unrelated launcher/game helper processes.
- A per-instance native DXGI parent factory hook so its
  `CreateSwapChainForHwnd` receives the real D3D11 device and returns a wrapped
  swapchain. Without this, the game crashed at that native factory call.

## Reproduce and continue

1. Build `renderdoc.sln` in Visual Studio/MSBuild with `Development|x64`. The
   resulting UI executable is `x64/Development/qrenderdic.exe`.
2. Run RenderDic as administrator. Keep kernel injection and global hook off.
   In **Settings > Advanced**, turn on **NTE launcher/game compatibility**;
   restart the launcher capture after changing it.
3. In **Launch Application**, select the installed `NTELauncher/NTEGame.exe`,
   use that executable's directory as the working directory, and enable
   **Capture Child Processes**. Click **Start Game** in the launcher and select
   the `HTGame.exe` Live Capture target. Check its API and presentation status.
4. Diagnose the first divergence after D3D11 device creation/swapchain wrapping.
   Compare D3D11/DXGI wrapper behavior with the native passthrough experiment,
   and inspect game/RenderDic logs and a passive hang stack. Change one scoped
   behavior at a time, rebuild, and retry the whole launch path.
5. Acceptance requires entering a playable scene, capturing a nonblack `.rdc`,
   reopening it in RenderDic, and confirming D3D11 plus actual scene draw
   events. A launcher frame or black transition frame does not count.

Local diagnostic files remain under `build/captures/diagnostics/` on the original
machine and are Git-ignored; they are **not** transferred with this commit. Key
names are `nte-final-capture-attempt.log`,
`nte-no-child-injection-control.log`,
`nte-injected-native-d3d11-control.log`, `nte-hung-stack.log`,
`nte-development-x64-build.log`, and `nte-black-frame662-d3d11.rdc`.
