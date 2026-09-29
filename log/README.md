# log/

Build and test logs kept with the fork for reference. Nothing here is read by the build.

| File | What it is |
| --- | --- |
| `build_clean3.log`, `build_clean4.log` | Windows MSVC clean builds of the fork, 2026-09-22 |
| `build_clean_release.log`, `build_clean_release2.log` | same, Release configuration |
| `build_fix2.log`, `build_fix3.log` | incremental builds while fixing the Breakpad/renderdocui targets |
| `build_post_pull_development_x64.log` | MSBuild output for the x64 Development rebuild right after syncing upstream v1.46 |
| `unittest.log`, `uitest.log`, `uitest2.log` | local test runs (`All tests passed`, 73 and 5 test cases) |
| `cap_out.txt` | a `capture_nte.py` run against the Android target **before** the layer fix: target control connects and the trigger goes out, but no capture arrives — the state that led to the probe-instance / stale-handle diagnosis |

The Android-side logs (RenderDoc internal log per run, device logcat) are pulled on demand into
`%TEMP%\rdoc0\` and are not committed; the runbook in the rdc_parasite repo describes how to
collect them. See that repo's `docs/RUNBOOK.md` for the capture recipe these logs came from.
