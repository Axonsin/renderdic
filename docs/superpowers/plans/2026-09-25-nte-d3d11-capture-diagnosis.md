# NTE D3D11 Capture Diagnosis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reproduce and localise the first RenderDic/D3D11/DXGI divergence that turns the playable NTE scene black, without modifying the NTE installation, and produce enough evidence for a separate minimal-fix plan.

**Architecture:** Keep `F:\Program Files\Neverness To Everness` read-only and run the normal user-mode capture path through `NTEGame.exe -> HTGame.exe`. First establish a post-v1.46 baseline, then add bounded diagnostic logging around device creation, the native parent-factory hook, swapchain construction, and presentation. If logs do not expose the stalled boundary, take a passive native thread-stack snapshot while leaving the game process alive.

**Tech Stack:** C++17, MSVC/Visual Studio 2026, Qt, Win32 process injection, D3D11, DXGI, RenderDic diagnostic logging, PowerShell.

---

## Scope and safety constraints

- Treat `F:\Program Files\Neverness To Everness` as read-only. Do not copy RenderDic DLLs into it, rename game DLLs, patch binaries, alter ACLs, or delete launcher update data.
- Run RenderDic and the launcher capture as administrator because the launcher elevates the game. Keep kernel injection and global hook disabled.
- Enable only `Capture Child Processes` and `Settings > Advanced > NTE launcher/game compatibility` for the baseline.
- Restart the entire launcher capture after every setting or code change. A surviving launcher process invalidates the next run.
- Do not infer success from the overlay alone. Acceptance requires a playable scene, a `Presenting` D3D11 live target, a nonblack `.rdc`, and visible scene draw events after reopening it.
- The current game log and user configuration under `C:\Users\Danny\AppData\Local\HT\Saved` are encoded. Preserve them, but use RenderDic's Diagnostic Log and native thread stacks as primary evidence.
- If login, CAPTCHA, account confirmation, or a game update prompt appears, stop automation and ask the user to complete that interaction.

## File map

- `NTE_CAPTURE_HANDOFF.md`: existing experiment history, current black-frame result, and final acceptance criteria.
- `renderdoc/os/win32/sys_win32_hooks.cpp`: scoped `NTEGame.exe -> HTGame.exe` suspended launch and user-mode injection.
- `renderdoc/driver/d3d11/d3d11_hooks.cpp`: D3D11 device wrapping and handoff to the NTE parent-factory hook.
- `renderdoc/driver/dxgi/dxgi_wrapped.cpp`: NTE native factory hook, wrapped swapchain lifecycle, buffer wrapping, and `Present`/`Present1`.
- `renderdoc/driver/dxgi/dxgi_wrapped.h`: declaration for `HookNTEFactoryFromAdapter` and DXGI wrapper types.
- `qrenderdoc/Windows/Dialogs/SettingsDialog.cpp`: load and save the opt-in NTE compatibility setting.
- `qrenderdoc/Windows/Dialogs/SettingsDialog.h`: settings dialog slots.
- `qrenderdoc/Windows/Dialogs/SettingsDialog.ui`: Advanced settings UI.
- `build/captures/diagnostics/`: ignored evidence directory for per-run RenderDic logs, stack captures, screenshots, manifests, and `.rdc` files.
- `docs/superpowers/plans/2026-09-25-nte-d3d11-capture-fix.md`: separate plan to create only after Task 6 identifies one concrete failure boundary.

### Task 1: Establish the post-pull build baseline

**Files:**
- Build: `renderdoc.sln`
- Output: `x64/Development/qrenderdic.exe`
- Output: `x64/Development/renderdic.dll`
- Log: `build_post_pull_development_x64.log`

- [x] **Step 1: Rebuild the current branch from scratch**

Run:

```powershell
$msbuild = 'D:\Program Files\Microsoft VisualStudio\VisualStudio 2026\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild 'D:\Projects\renderdoc\renderdoc.sln' '/t:Rebuild' '/m' '/p:Configuration=Development' '/p:Platform=x64' '/verbosity:minimal' '/nologo' '/fl' '/flp:logfile=D:\Projects\renderdoc\build_post_pull_development_x64.log;verbosity=normal'
```

Expected: exit code `0`, `qrenderdoc_local.vcxproj -> D:\Projects\renderdoc\x64\Development\qrenderdic.exe`.

- [x] **Step 2: Verify the executable was built from the pulled commit**

Run:

```powershell
& 'D:\Projects\renderdoc\x64\Development\renderdiccmd.exe' --version
```

Expected:

```text
renderdiccmd x64 v1.46 built from 78591f3f24df28e3b2e60c63742f82cc3626c916
```

- [x] **Step 3: Check the build log and fingerprint the outputs**

Run:

```powershell
$log = 'D:\Projects\renderdoc\build_post_pull_development_x64.log'
@(Select-String -LiteralPath $log -Pattern ': error ' -SimpleMatch).Count
@(Select-String -LiteralPath $log -Pattern ': warning ' -SimpleMatch).Count
Get-FileHash -Algorithm SHA256 -LiteralPath @(
  'D:\Projects\renderdoc\x64\Development\qrenderdic.exe',
  'D:\Projects\renderdoc\x64\Development\renderdic.dll'
)
```

Expected: `0` errors, `0` warnings. Current hashes are:

```text
qrenderdic.exe  22F335812EE15FEA0F22A9E2805C69FEB5325A99F40D463FAE8194B45641D26F
renderdic.dll   A4253CDB3500D178FBC78F47986EEB6CF25044ADEFA3F862036C5BA8A45379F9
```

- [x] **Step 4: Run the internal unit tests and record the existing environment failure**

Run:

```powershell
& 'D:\Projects\renderdoc\x64\Development\renderdiccmd.exe' test unit
```

Observed: `72/73` test cases passed and `21,763,139/21,763,141` assertions passed. The only failure is `Test OS-specific functions / Environment Variables` because the test overwrites its result with `Process::GetEnvVariable("HOME")`, while this Windows process has no `HOME` variable. Do not change or fabricate `HOME` merely to make this assertion pass.

### Task 2: Freeze a read-only NTE installation baseline

**Files:**
- Read: `F:\Program Files\Neverness To Everness\NTELauncher\NTEGame.exe`
- Read: `F:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\win64\HTGame.exe`
- Read: `F:\Program Files\Neverness To Everness\Client\WindowsNoEditor\Engine\Plugins\Runtime\Nvidia\StreamlineCore\Binaries\ThirdParty\Win64\sl.interposer.dll`
- Create: `build/captures/diagnostics/run-00-baseline/manifest.txt`

- [x] **Step 1: Create the evidence directory outside the game installation**

Run:

```powershell
$evidenceRoot = 'D:\Projects\renderdoc\build\captures\diagnostics\run-00-baseline'
New-Item -ItemType Directory -Path $evidenceRoot -Force
```

Expected: the directory exists under the repository; no file under `F:\Program Files\Neverness To Everness` changes.

- [x] **Step 2: Record executable identity and current hashes**

Run:

```powershell
$targets = @(
  'F:\Program Files\Neverness To Everness\NTELauncher\NTEGame.exe',
  'F:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\win64\HTGame.exe'
)
$targets | ForEach-Object {
  $item = Get-Item -LiteralPath $_
  $hash = Get-FileHash -LiteralPath $_ -Algorithm SHA256
  '{0}`t{1}`t{2:o}`t{3}' -f $item.FullName,$item.Length,$item.LastWriteTime,$hash.Hash
} | Set-Content -LiteralPath 'D:\Projects\renderdoc\build\captures\diagnostics\run-00-baseline\manifest.txt'
```

Expected current hashes:

```text
NTEGame.exe  3CF58ECBE4CD33A83E741C12437CA9BB67678D166548C5972579C1266F250D9F
HTGame.exe   0C43F0FE1E02802C99EEBC8DFE974A1FC4599FD1C1605EB50BC6BE37EB165DC3
```

If either hash differs, record the new value and restart from Task 3; do not replace or restore the game binary.

- [x] **Step 3: Confirm that the installation contains no persistent RenderDic payload**

Run:

```powershell
Get-ChildItem -LiteralPath 'F:\Program Files\Neverness To Everness' -Recurse -File |
  Where-Object { $_.Name -match 'renderdi(c|oc)|rdoc' } |
  Select-Object FullName,Length,LastWriteTime
```

Expected: no result. NVIDIA DLSS/Streamline libraries are expected and must remain untouched.

### Task 3: Reproduce the v1.46 baseline through the launcher

**Files:**
- Execute: `x64/Development/qrenderdic.exe`
- Execute through RenderDic: `F:\Program Files\Neverness To Everness\NTELauncher\NTEGame.exe`
- Save: `build/captures/diagnostics/run-00-baseline/renderdic.log`
- Save if produced: `build/captures/diagnostics/run-00-baseline/*.rdc`

- [ ] **Step 1: Start the freshly built UI elevated**

Run:

```powershell
Start-Process -FilePath 'D:\Projects\renderdoc\x64\Development\qrenderdic.exe' -Verb RunAs
```

Expected: the About dialog reports RenderDic `v1.46` and commit `78591f3f2`.

- [ ] **Step 2: Apply the exact capture settings**

In RenderDic:

```text
Kernel injection: Off
Global hook: Off
Settings > Advanced > NTE launcher/game compatibility: On
Launch Application > Executable:
  F:\Program Files\Neverness To Everness\NTELauncher\NTEGame.exe
Launch Application > Working Directory:
  F:\Program Files\Neverness To Everness\NTELauncher
Capture Child Processes: On
```

Close any old launcher/game processes before pressing Launch. After changing the NTE setting, always start a new launcher capture.

- [ ] **Step 3: Start the game and classify the visible outcome**

Use **Start Game** in the launcher, select the `HTGame.exe` Live Capture target, and wait until either the street scene is playable or the black-screen state is stable.

Record exactly one outcome:

```text
A. D3D11 (Presenting), playable scene
B. D3D11 (Not Presenting), black window
C. HTGame.exe absent from Live Capture
D. Process crash or launcher error
```

If login or account confirmation is needed, ask the user to complete it before continuing.

- [ ] **Step 4: Export the RenderDic Diagnostic Log**

Open the **Diagnostic Log** window, press **Save**, and write:

```text
D:\Projects\renderdoc\build\captures\diagnostics\run-00-baseline\renderdic.log
```

The log must contain, or explicitly lack, these messages:

```text
NTE early capture: created HTGame.exe suspended
NTE DXGI factory hook installed
NTE DXGI factory hook: CreateSwapChainForHwnd
```

- [ ] **Step 5: Attempt one capture only if the target is presenting**

Capture a playable frame, save it under `run-00-baseline`, reopen it in the same build, and verify:

```text
API: D3D11
Thumbnail/backbuffer: nonblack scene
Event Browser: more than transition DrawIndexed(6) + Present
Replay: succeeds without fatal error
```

If all four checks pass, skip Tasks 4-6 and proceed to Task 7.

### Task 4: Add bounded D3D11/DXGI boundary logging

Run this task only when Task 3 produces outcome B, C, or D.

**Files:**
- Modify: `renderdoc/driver/d3d11/d3d11_hooks.cpp:175`
- Modify: `renderdoc/driver/dxgi/dxgi_wrapped.cpp:69`
- Modify: `renderdoc/driver/dxgi/dxgi_wrapped.cpp:357`
- Modify: `renderdoc/driver/dxgi/dxgi_wrapped.cpp:691`
- Test: `build_post_pull_development_x64.log`

- [ ] **Step 1: Add a shared NTE-mode check and bounded Present counter**

Insert beside the existing NTE factory-hook state in `dxgi_wrapped.cpp`:

```cpp
static bool NTECaptureEnabled()
{
  const SDObject *setting = RenderDoc::Inst().GetConfigSetting("Driver.NTEEarlyChildCapture");
  return setting && setting->AsBool();
}

static volatile LONG ntePresentCalls = 0;
```

Replace the duplicate setting check in `HookNTEFactoryFromAdapter` with `NTECaptureEnabled()`.

- [ ] **Step 2: Log the native factory call and returned swapchain**

Immediately before and after the `original(...)` call in `NTECreateSwapChainForHwndHook`, add:

```cpp
if(desc)
  RDCLOG("NTE CreateSwapChainForHwnd enter: factory=%p wrappedDevice=%p realDevice=%p "
         "wnd=%p size=%ux%u format=%s buffers=%u usage=0x%x flags=0x%x effect=%u",
         factory, device, wrappedDevice->GetRealIUnknown(), wnd, desc->Width, desc->Height,
         ToStr(desc->Format).c_str(), desc->BufferCount, desc->BufferUsage, desc->Flags,
         (uint32_t)desc->SwapEffect);
else
  RDCLOG("NTE CreateSwapChainForHwnd enter: factory=%p wrappedDevice=%p realDevice=%p "
         "wnd=%p desc=NULL",
         factory, device, wrappedDevice->GetRealIUnknown(), wnd);

HRESULT result = original(factory, wrappedDevice->GetRealIUnknown(), wnd, desc, fullscreenDesc,
                          restrictToOutput, swapchain);

RDCLOG("NTE CreateSwapChainForHwnd exit: result=%s realSwapchain=%p",
       ToStr(result).c_str(), swapchain ? *swapchain : NULL);
```

Keep the existing wrapper construction after the exit log.

- [ ] **Step 3: Log completion of swapchain construction**

At the end of `WrappedIDXGISwapChain4::WrappedIDXGISwapChain4`, after `FirstFrame(this)`, add:

```cpp
if(NTECaptureEnabled())
  RDCLOG("NTE wrapped swapchain ready: this=%p real=%p wnd=%p real1=%p real2=%p real3=%p "
         "real4=%p",
         this, m_pReal, m_Wnd, m_pReal1, m_pReal2, m_pReal3, m_pReal4);
```

- [ ] **Step 4: Bound logs around capture bookkeeping and the real Present call**

Restructure `Present` so its final portion is:

```cpp
LONG call = NTECaptureEnabled() ? InterlockedIncrement(&ntePresentCalls) : 0;
if(call > 0 && call <= 16)
  RDCLOG("NTE Present enter: call=%ld swapchain=%p sync=%u flags=0x%x", call, this,
         SyncInterval, Flags);

if((Flags & DXGI_PRESENT_TEST) == 0)
{
  TickLastPresentedBuffer();
  m_pDevice->Present(this, SyncInterval, Flags);
}

if(call > 0 && call <= 16)
  RDCLOG("NTE Present after capture bookkeeping: call=%ld", call);

HRESULT ret = m_pReal->Present(SyncInterval, Flags);

if((call > 0 && call <= 16) || FAILED(ret))
  RDCLOG("NTE Present exit: call=%ld result=%s", call, ToStr(ret).c_str());

return ret;
```

Apply the same shape to `Present1`, calling `m_pReal1->Present1` and including the same `call` value. Sixteen calls are enough to establish forward progress without producing an unbounded per-frame log.

- [ ] **Step 5: Add one D3D11 device-wrap completion message**

After `HookNTEFactoryFromAdapter(pAdapter)` in `d3d11_hooks.cpp`, add:

```cpp
if(const SDObject *setting = RenderDoc::Inst().GetConfigSetting("Driver.NTEEarlyChildCapture"))
{
  if(setting->AsBool())
    RDCLOG("NTE D3D11 device wrapped: adapter=%p realDevice=%p wrappedDevice=%p "
           "immediateContext=%p initialSwapchain=%p",
           pAdapter, wrap->GetRealIUnknown(), wrap, ppImmediateContext ? *ppImmediateContext : NULL,
           ppSwapChain ? *ppSwapChain : NULL);
}
```

- [ ] **Step 6: Rebuild and check the patch**

Run:

```powershell
git diff --check
$msbuild = 'D:\Program Files\Microsoft VisualStudio\VisualStudio 2026\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild 'D:\Projects\renderdoc\renderdoc.sln' '/t:Build' '/m' '/p:Configuration=Development' '/p:Platform=x64' '/verbosity:minimal' '/nologo'
```

Expected: `git diff --check` prints nothing and MSBuild exits `0`.

### Task 5: Reproduce once with instrumentation and capture a passive stack

**Files:**
- Create: `build/captures/diagnostics/run-01-trace/renderdic.log`
- Create: `build/captures/diagnostics/run-01-trace/thread-stacks.txt`
- Save if produced: `build/captures/diagnostics/run-01-trace/*.rdc`

- [ ] **Step 1: Repeat the exact Task 3 launch path**

Create `run-01-trace`, restart RenderDic and the launcher, and reproduce the stable result once. Do not change DLSS, Streamline, game configuration, kernel injection, or global hook during this run.

- [ ] **Step 2: Save the bounded trace log**

Export the Diagnostic Log as `run-01-trace\renderdic.log`. Classify the last completed boundary:

```text
1. No "NTE early capture" message: child interception/injection boundary
2. Injection logged, no factory-hook install: D3D11 device creation/wrap boundary
3. Factory installed, no CreateSwapChainForHwnd entry: retained-factory call path changed
4. CreateSwapChainForHwnd entry without exit: native DXGI/Streamline call boundary
5. Swapchain exit without "wrapped swapchain ready": wrapper construction/backbuffer boundary
6. Wrapped swapchain ready without Present entry: game/RHI startup boundary after wrapping
7. Present entry without "after capture bookkeeping": RenderDic capture bookkeeping boundary
8. Capture bookkeeping completes without Present exit: native Present boundary
9. Present exits repeatedly while UI says Not Presenting: live-target registration/status boundary
```

- [ ] **Step 3: Capture native thread stacks only for a stable black/hung state**

Open Visual Studio 2026 as administrator, select **Debug > Attach to Process**, attach the Native debugger to `HTGame.exe`, choose **Break All**, and copy the call stacks for:

```text
Game/main thread
RHI thread
Render thread
Any thread inside sl.interposer.dll, dxgi.dll, d3d11.dll, renderdic.dll, or nvapi64.dll
```

Save the text to `run-01-trace\thread-stacks.txt`, then **Detach All** so Visual Studio does not terminate the game. Do not use a dump command that suspends or kills the process after collection.

### Task 6: Close the diagnosis phase at an evidence gate

**Files:**
- Modify: `NTE_CAPTURE_HANDOFF.md`
- Create: `docs/superpowers/plans/2026-09-25-nte-d3d11-capture-fix.md`

- [ ] **Step 1: Correlate trace order with the passive stack**

Write a short table in `NTE_CAPTURE_HANDOFF.md` with one row per run:

```markdown
| Run | Game hashes | Last completed boundary | Live state | Top RHI frame | Capture result |
| --- | --- | --- | --- | --- | --- |
```

Add one `run-00-baseline` row and one `run-01-trace` row using exact values from the evidence
files. If a field was not observed, write `not observed`; do not infer it.

- [ ] **Step 2: Select exactly one follow-up subsystem**

Use this routing table:

```text
Boundaries 1-2 -> sys_win32_hooks.cpp and d3d11_hooks.cpp injection/device plan
Boundaries 3-5 -> dxgi_wrapped.cpp parent-factory/swapchain construction plan
Boundary 6    -> D3D11 wrapper vs Streamline/NVAPI startup plan
Boundary 7    -> WrappedID3D11Device::Present capture bookkeeping plan
Boundary 8    -> DXGI Present descriptor/state plan
Boundary 9    -> frame-capturer registration and Live Capture reporting plan
```

Create the separate fix plan named above with only the selected subsystem in scope. Do not mix unrelated launcher, kernel, DLSS, and swapchain changes into one experiment.

- [ ] **Step 3: Review checkpoint with the user**

Present the baseline outcome, final trace boundary, stack evidence, and the proposed minimal subsystem before changing behavior. Ask for user input only when the next test requires login, a game setting change, or installation of an additional debugger.

### Task 7: Validate the eventual fix against the real acceptance criteria

**Files:**
- Update: `NTE_CAPTURE_HANDOFF.md`
- Save: `build/captures/diagnostics/accepted/*.rdc`
- Save: `build/captures/diagnostics/accepted/renderdic.log`

- [ ] **Step 1: Run a clean full rebuild after the selected fix**

Run the Task 1 rebuild command and require exit code `0`.

- [ ] **Step 2: Run the launcher path with production settings**

Remove temporary probe modes and high-volume diagnostic logging. Keep only the default-off NTE compatibility option, normal user-mode capture, child capture enabled, kernel injection off, and global hook off.

- [ ] **Step 3: Capture and reopen a playable frame**

Acceptance requires all of:

```text
HTGame.exe reaches the playable street scene
Live Capture reports D3D11 (Presenting)
The saved .rdc thumbnail/backbuffer is nonblack
Reopening succeeds in the newly built qrenderdic.exe
Event Browser contains real scene draws/resources, not only DrawIndexed(6) + Present
The game installation hashes are unchanged during the experiment
```

- [ ] **Step 4: Run final verification and update the handoff**

Run:

```powershell
git diff --check
& 'D:\Projects\renderdoc\x64\Development\renderdiccmd.exe' --version
& 'D:\Projects\renderdoc\x64\Development\renderdiccmd.exe' test unit
```

Report the known Windows `HOME` assertion separately if it remains the only unit-test failure. Record the accepted `.rdc`, RenderDic log, executable hashes, game hashes, and exact reproduction steps in `NTE_CAPTURE_HANDOFF.md`.
