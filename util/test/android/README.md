# Android loader thunk integration test

`short_thunk.cpp` exercises forward/backward branches, exact branch-range boundaries,
unsupported prologues without mutation, retained BTI/PAC instructions, calls to the
original implementation, and real Vulkan entry pointers cached before hook installation.
It creates and destroys a Vulkan instance through the cached pointer.

Build with a recent Android NDK ARM64 C++ compiler (API 23 or newer), `-std=c++11
-O2 -static-libstdc++ -ldl`, then push and execute through adb. No root or game data
is needed. The real-loader cases require BTI+B exports with the supported PAC/frame
prologues, as on the Android 15 validation device. An unsupported loader fails the
assertion rather than silently skipping the integration cases.

Also build and run with `-mbranch-protection=standard -Wl,-z,force-bti` to exercise a
BTI-protected test executable. Do not compile with `NDEBUG`: assertions perform the
operations under test. Successful hooks intentionally remain until process exit.
