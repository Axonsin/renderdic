/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "kernel_injector.h"
#include "kernel_mem.h"
#include "driver_loader.h"
#include "driver_resources.h"
#include "trace_cleanup.h"
#include "manual_map.h"

#include "../../../renderdocshim/renderdocshim.h"

#if defined(Q_OS_WIN)

#include <windows.h>
#include <sddl.h>
#include <QDebug>
#include <QThread>
#include <QProcess>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>

#include <random>
#include <memory>
#include <atomic>

namespace KernelInjector
{
namespace
{
// Matches RDI_IOCTL_INJECT in kernel_driver/driver.h.
constexpr uint32_t kInjectIoctl = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED,
                                           FILE_ANY_ACCESS);

// Serialises Capture/Shutdown/IsActive: the driver chain is session state and
// must not be torn down while a capture is in flight (closeEvent can run on
// the UI thread while a capture worker is still busy). Capture holds the lock
// for its whole duration, which blocks Shutdown until the pipeline finishes.
QMutex g_chainMutex;

// Session state: the vulnerable driver and the mapped injection driver stay
// loaded so repeated captures are fast. Only touched under g_chainMutex.
LoadedDriver g_loadedDriver;
std::unique_ptr<KernelMem> g_backend;
void *g_injectorDevice = nullptr;
BackendId g_activeBackend = BackendId::Portwell;
bool g_chainUp = false;

// One-time secret for the injection device's caller authentication (see
// kernel_driver/driver.h). Generated when the chain comes up; every inject
// request carries it.
uint64_t g_deviceSecret = 0;

// Set by Shutdown() before it blocks on g_chainMutex so an in-flight Capture
// can abandon its status poll instead of making closeEvent stall on the UI
// thread for the whole timeout. Only the polling phase is interruptible - a
// capture that is still bringing up the chain (driver load + physical scan)
// has to finish that first.
std::atomic<bool> g_cancelRequested{false};

const int kStatusTimeoutMs = 30000;
const int kStatusPollMs = 100;

// The shim reports completion by writing status + the target control ident
// back into the shared mapping. Status 2/3 are intermediate failure reports
// that other processes (e.g. under the global hook) can also write, so the
// coordinator keeps polling through them and only acts at timeout.
const uint32_t kShimStatusDone = 1;
const uint32_t kShimStatusMismatch = 2;
const uint32_t kShimStatusSetupFailed = 3;

bool IsHVCIEnabled()
{
  // Hypervisor-Enforced Code Integrity (memory integrity). Manual mapping of
  // executable kernel pages BSODs under HVCI, so the feature must refuse.
  // Enabled: 0 = off, 1 = on (UEFI locked), 2 = on (not locked) - any non-zero
  // value means the feature is active.
  HKEY key = nullptr;
  LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                              L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\"
                              L"HypervisorEnforcedCodeIntegrity",
                              0, KEY_READ, &key);
  if(status != ERROR_SUCCESS)
    return false;

  DWORD enabled = 0;
  DWORD size = sizeof(enabled);
  status = RegQueryValueExW(key, L"Enabled", nullptr, nullptr, (LPBYTE)&enabled, &size);
  RegCloseKey(key);

  return status == ERROR_SUCCESS && enabled != 0;
}

int QueryVbsStatus()
{
  // Virtualization-Based Security state from Win32_DeviceGuard: 0 = off,
  // 1 = configured but not running, 2 = running. This is the authoritative
  // source; the DeviceGuard registry values stay empty when VBS is on purely by
  // OS default, and securekernel.exe (the VTL1 kernel) is hidden from
  // SystemModuleInformation, so neither of those can detect it. Returns -1 if
  // the query itself failed.
  QProcess powershell;
  powershell.start(
      QStringLiteral("powershell"),
      QStringList()
          << QStringLiteral("-NoProfile") << QStringLiteral("-NonInteractive")
          << QStringLiteral("-Command")
          << QStringLiteral("(Get-CimInstance -Namespace root/Microsoft/Windows/DeviceGuard "
                            "-ClassName Win32_DeviceGuard).VirtualizationBasedSecurityStatus"));
  if(!powershell.waitForStarted(5000) || !powershell.waitForFinished(15000))
  {
    powershell.kill();
    return -1;
  }

  if(powershell.exitStatus() != QProcess::NormalExit || powershell.exitCode() != 0)
    return -1;

  bool ok = false;
  int status = QString::fromUtf8(powershell.readAllStandardOutput()).trimmed().toInt(&ok);
  return ok ? status : -1;
}

bool CreateShimDataMapping(const KernelInjectorCore::CaptureRequest &req, HANDLE *outMapping,
                           ShimData **outView)
{
  // The target may run under a different (possibly lower-privileged) account
  // than this elevated coordinator, and the default DACL of a section created
  // with an admin token would deny it FILE_MAP_WRITE - the shim's OpenFile-
  // Mapping would fail and the capture would time out. Grant admins, SYSTEM
  // and Everyone map read/write explicitly. This does allow any local process
  // to spoof the status fields, but the handshake is coordinator-owned and
  // bogus values only lead to the timeout/diagnosed-failure paths. (The
  // global hook flow in renderdoccmd creates the same named mapping with
  // default security; that side is intentionally unchanged.)
  PSECURITY_DESCRIPTOR sd = NULL;
  ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GWGR;;;BA)(A;;GWGR;;;SY)(A;;GWGR;;;WD)", SDDL_REVISION_1, &sd, NULL);

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = sd;

  // The 32-bit shim opens a differently-named mapping (the WIN32 / #else
  // branch of renderdocshim.h picks RenderDicGlobalHookData32 for
  // renderdicshim32.dll), so the coordinator must create the name that
  // matches the shim bitness it injects. The 64-bit name is taken from the
  // shared header so at least the default case cannot drift.
  const char *mappingName = req.targetIsWow64 ? "RenderDicGlobalHookData32" : GLOBAL_HOOK_DATA_NAME;

  HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, sd != NULL ? &sa : nullptr,
                                      PAGE_READWRITE, 0, sizeof(ShimData), mappingName);

  if(sd != NULL)
    LocalFree(sd);

  if(mapping == nullptr)
    return false;

  // A mapping with this name already exists: another kernel capture (or the
  // global hook flow) is active. Refuse rather than clobbering it.
  if(GetLastError() == ERROR_ALREADY_EXISTS)
  {
    CloseHandle(mapping);
    return false;
  }

  ShimData *view = (ShimData *)MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                             sizeof(ShimData));
  if(view == nullptr)
  {
    CloseHandle(mapping);
    return false;
  }

  memset(view, 0, sizeof(ShimData));

  wcsncpy_s(view->pathmatchstring, req.exeName.toStdWString().c_str(), _TRUNCATE);
  wcsncpy_s(view->rdocpath, req.renderdocPath.toStdWString().c_str(), _TRUNCATE);
  strncpy_s(view->capfile, req.captureFile.toUtf8().constData(), _TRUNCATE);

  if(sizeof(CaptureOptions) <= sizeof(view->opts))
    memcpy(view->opts, &req.opts, sizeof(CaptureOptions));

  view->status = 0;
  view->ident = 0;

  *outMapping = mapping;
  *outView = view;
  return true;
}

bool SendInjectRequest(uint32_t pid, const QString &shimPath, uint32_t *outMethod,
                       uint32_t *outHijackMs)
{
  if(g_injectorDevice == nullptr)
    return false;

  // Must match RDI_INJECT_REQUEST / RDI_INJECT_RESULT in kernel_driver/driver.h.
  // METHOD_BUFFERED: the small result struct is copied back over the request.
  struct InjectRequest
  {
    uint32_t processId;
    wchar_t dllPath[1024];
    uint64_t secret;
  } req = {};

  struct InjectResult
  {
    uint32_t method;    // 1 = ZwCreateThreadEx, 2 = context hijack
    uint32_t hijackMs;
  } result = {};

  req.processId = pid;
  req.secret = g_deviceSecret;

  std::wstring path = shimPath.toStdWString();
  if(path.size() >= 1024)
    return false;
  wcscpy_s(req.dllPath, path.c_str());

  DWORD returned = 0;
  if(DeviceIoControl((HANDLE)g_injectorDevice, kInjectIoctl, &req, sizeof(req), &result,
                     sizeof(result), &returned, nullptr) == FALSE)
    return false;

  if(outMethod != nullptr)
    *outMethod = result.method;
  if(outHijackMs != nullptr)
    *outHijackMs = result.hijackMs;
  return true;
}

bool EnsureChainUp(BackendId backend, QString *error)
{
  if(g_chainUp)
  {
    if(backend != g_activeBackend)
    {
      qWarning() << "KernelInjector: driver chain already up with backend"
                 << (int)g_activeBackend << "- request for backend" << (int)backend
                 << "is ignored (restart to switch)";
    }
    return true;
  }

  if(IsHVCIEnabled())
  {
    *error = QStringLiteral(
        "Windows Memory Integrity (HVCI) is enabled. Manual mapping of the injection driver "
        "requires it to be off - disable it under Windows Security -> Device Security and "
        "reboot before using kernel capture.");
    return false;
  }

  // VBS running is fatal for these backends in a nastier way than HVCI: the
  // kernel refuses MmMapIoSpace of protected RAM pages per page and the
  // vulnerable drivers never check for NULL, so the first protected page the
  // PML4 scan touches bugchecks the machine (observed on Win11 with VBS on and
  // all optional services off). A backend whose reads do not go through
  // MmMapIoSpace (e.g. an MmCopyMemory-based one) would not need this gate.
  {
    const int vbs = QueryVbsStatus();
    if(vbs < 0)
    {
      *error = QStringLiteral(
          "Could not determine the Virtualization-Based Security state of this machine "
          "(the Win32_DeviceGuard query failed). Kernel capture refuses to run when it "
          "cannot prove VBS is off, because a VBS-protected RAM page would crash the "
          "system instead of failing the read.");
      return false;
    }
    if(vbs == 2)
    {
      *error = QStringLiteral(
          "Virtualization-Based Security (VBS) is running on this machine. Under VBS "
          "Windows refuses to map some RAM pages for drivers, and the vulnerable driver "
          "backends crash the whole system when that happens mid-read. Disable VBS (set "
          "HKLM\\SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\EnableVirtualizationBasedSecurity "
          "to 0, also check Windows Security -> Device Security -> Core isolation) and "
          "reboot, or use kernel capture on a machine or VM without VBS.");
      return false;
    }
  }

  // 0. The mapped injection driver can't be unmapped, so its device object
  // survives a previous session until the next reboot. Check the \DosDevices
  // symlink instead of opening the device: opening would dispatch IRPs into
  // a driver that may be stale (or dangling after a failed map), while the
  // symlink lookup only touches the object manager namespace.
  {
    wchar_t dosTarget[512];
    if(QueryDosDeviceW(L"RenderDicInj", dosTarget, (DWORD)ARRAYSIZE(dosTarget)) != 0)
    {
      *error = QStringLiteral(
          "The kernel injection device \\\\.\\RenderDicInj already exists - a previous "
          "session left the injection driver mapped in memory. Restart the machine and try again.");
      return false;
    }
  }

  // 1. Load the vulnerable driver.
  LoadedDriver loaded;
  QString loadError;
  if(!DriverLoader::Load(backend, &loaded, &loadError))
  {
    *error = loadError;
    return false;
  }

  // 2. Backend over the driver device.
  std::unique_ptr<KernelMem> mem;
  if(backend == BackendId::Portwell)
    mem.reset(new PortwellBackend(loaded.device));
  else
    mem.reset(new TbtBackend(loaded.device));

  if(!mem->FindNtoskrnlBase())
  {
    *error = QStringLiteral("Failed to resolve ntoskrnl base");
    DriverLoader::Unload(loaded);
    return false;
  }

  // 3. Trace cleanup for the vulnerable driver itself.
  const uint32_t stamp = DriverLoader::DriverTimeDateStamp(backend);
  if(!TraceCleanup::CleanupAll(mem.get(), loaded.device, loaded.serviceName, loaded.tempPath, stamp))
  {
    qWarning() << "KernelInjector: trace cleanup reported failures (continuing anyway)";
  }

  // 4. Manual map the injection driver.
  void *injectorDevice = nullptr;
  QString mapError;
  if(!ManualMapper::MapDriver(mem.get(), injector_driver_bytes, injector_driver_bytes_size,
                              &injectorDevice, &mapError))
  {
    *error = mapError;
    DriverLoader::Unload(loaded);
    return false;
  }

  g_loadedDriver = loaded;
  g_backend = std::move(mem);
  g_injectorDevice = injectorDevice;
  g_activeBackend = backend;
  // 64-bit session secret for the injection device (std rather than
  // QRandomGenerator: the bundled Qt predates it).
  std::mt19937_64 rng(std::random_device{}());
  g_deviceSecret = rng();
  g_chainUp = true;
  return true;
}
}    // namespace

bool KernelInjectorCore::Capture(const CaptureRequest &req, uint32_t *outIdent, QString *error)
{
  if(outIdent == nullptr || error == nullptr)
    return false;

  *error = QString();

  // Held for the whole pipeline: protects the chain from concurrent Shutdown
  // and from a second concurrent Capture.
  QMutexLocker lock(&g_chainMutex);

  // Fresh capture: clear any cancel flag a previous Shutdown left behind.
  g_cancelRequested = false;

  if(!EnsureChainUp(req.backend, error))
    return false;

  if(req.exeName.isEmpty() || req.shimPath.isEmpty() || req.renderdocPath.isEmpty())
  {
    *error = QStringLiteral("Invalid capture request (missing exe/shim/renderdoc paths)");
    return false;
  }

  // 5. ShimData mapping for the shim.
  HANDLE mapping = nullptr;
  ShimData *view = nullptr;
  if(!CreateShimDataMapping(req, &mapping, &view))
  {
    *error = QStringLiteral(
        "Could not create the shim data mapping - another kernel capture or the global hook "
        "may be active");
    return false;
  }

  // 6. Inject the shim into the target. The method the driver used is
  // logged right away - it is the key diagnostic when a target misbehaves
  // after injection (1 = dedicated thread, 2 = context hijack fallback).
  {
    uint32_t method = 0, hijackMs = 0;
    if(!SendInjectRequest(req.pid, req.shimPath, &method, &hijackMs))
    {
      UnmapViewOfFile(view);
      CloseHandle(mapping);
      *error = QStringLiteral("The injection driver rejected the inject request");
      return false;
    }

    if(method == 2)
      qInfo() << "KernelInjector: injected via context hijack, parked after" << hijackMs << "ms";
    else
      qInfo() << "KernelInjector: injected via dedicated thread";
  }

  // 7. Wait for the shim to load renderdoc.dll and complete the setup. Status
  // 2/3 are also written by unrelated processes when the global hook is
  // active, so only status 1 terminates the wait; intermediate codes are
  // remembered for the timeout message.
  QElapsedTimer timer;
  timer.start();

  uint32_t shimStatus = 0;
  uint32_t lastStatus = 0;
  bool cancelled = false;
  while(timer.elapsed() < kStatusTimeoutMs)
  {
    if(g_cancelRequested.load())
    {
      cancelled = true;
      break;
    }

    // The shim writes through the same mapping.
    shimStatus = view->status;
    if(shimStatus == kShimStatusDone)
      break;
    if(shimStatus != 0)
      lastStatus = shimStatus;
    QThread::msleep(kStatusPollMs);
  }

  if(shimStatus != kShimStatusDone)
  {
    UnmapViewOfFile(view);
    CloseHandle(mapping);

    if(cancelled)
      *error = QStringLiteral("Kernel capture cancelled - the application is shutting down");
    else if(lastStatus == kShimStatusMismatch)
      *error = QStringLiteral("The shim loaded but did not match the target executable - "
                              "check that the process still runs under the expected name");
    else if(lastStatus == kShimStatusSetupFailed)
      *error = QStringLiteral(
          "The shim loaded but the capture setup failed (renderdic.dll could not be loaded or "
          "its INTERNAL_* exports are missing)");
    else
      *error = QStringLiteral("No response from %1 - the injection may have "
                              "been blocked or the target exited early")
                   .arg(req.targetIsWow64 ? lit("renderdicshim32.dll") : lit("renderdicshim64.dll"));
    return false;
  }

  if(view->ident == 0)
  {
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    *error = QStringLiteral("The shim reported success but no target control ident - the "
                            "renderdic.dll in the target may be incompatible");
    return false;
  }

  *outIdent = view->ident;

  UnmapViewOfFile(view);
  CloseHandle(mapping);

  qInfo() << "KernelInjector: capture setup complete in target, ident" << *outIdent;
  return true;
}

bool KernelInjectorCore::IsActive()
{
  QMutexLocker lock(&g_chainMutex);
  return g_chainUp;
}

void KernelInjectorCore::Shutdown()
{
  // Ask an in-flight Capture to abandon its status poll before blocking on
  // the mutex it holds (see g_cancelRequested above).
  g_cancelRequested = true;

  QMutexLocker lock(&g_chainMutex);

  if(g_injectorDevice != nullptr)
  {
    CloseHandle((HANDLE)g_injectorDevice);
    g_injectorDevice = nullptr;
  }

  g_backend.reset();

  if(g_loadedDriver.device != nullptr)
  {
    DriverLoader::Unload(g_loadedDriver);
    g_loadedDriver = {};
  }

  g_deviceSecret = 0;
  g_chainUp = false;
}
}    // namespace KernelInjector

#endif    // Q_OS_WIN
