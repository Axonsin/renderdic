/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
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

#include <winsock2.h>
#include <shellapi.h>
#include "core/core.h"
#include "core/settings.h"
#include "hooks/hooks.h"
#include "os/os_specific.h"
#include "strings/string_utils.h"

#include <string>

typedef int(WSAAPI *PFN_WSASTARTUP)(__in WORD wVersionRequested, __out LPWSADATA lpWSAData);
typedef int(WSAAPI *PFN_WSACLEANUP)();

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_A)(LPCSTR lpApplicationName, LPSTR lpCommandLine,
                                           LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                           LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                           BOOL bInheritHandles, DWORD dwCreationFlags,
                                           LPVOID lpEnvironment, LPCSTR lpCurrentDirectory,
                                           LPSTARTUPINFOA lpStartupInfo,
                                           LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_W)(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                           LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                           LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                           BOOL bInheritHandles, DWORD dwCreationFlags,
                                           LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
                                           LPSTARTUPINFOW lpStartupInfo,
                                           LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_AS_USER_A)(
    HANDLE hToken, LPCSTR lpApplicationName, LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_AS_USER_W)(
    HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_WITH_LOGON_W)(LPCWSTR lpUsername, LPCWSTR lpDomain,
                                                      LPCWSTR lpPassword, DWORD dwLogonFlags,
                                                      LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                                      DWORD dwCreationFlags, LPVOID lpEnvironment,
                                                      LPCWSTR lpCurrentDirectory,
                                                      LPSTARTUPINFOW lpStartupInfo,
                                                      LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL(WINAPI *PFN_SHELLEXECUTE_EXW)(SHELLEXECUTEINFOW *pExecInfo);

RDOC_CONFIG(bool, Driver_NTEEarlyChildCapture, false,
            "Launch the NTE game child suspended from its launcher for ordinary capture, and "
            "hook the native DXGI parent factory retained by HTGame.exe. Only applies to the "
            "observed HTGame.exe process chain.");

class SysHook : LibraryHook
{
public:
  SysHook()
  {
    // we start with a refcount of 1 because we initialise WSA ourselves for our own sockets.
    m_WSARefCount = 1;
  }

  void RegisterHooks()
  {
    RDCLOG("Registering Win32 system hooks");

    // register libraries that we care about. We don't need a callback when they are loaded
    LibraryHooks::RegisterLibraryHook("kernel32.dll", NULL);
    LibraryHooks::RegisterLibraryHook("advapi32.dll", NULL);
    LibraryHooks::RegisterLibraryHook("api-ms-win-core-processthreads-l1-1-0.dll", NULL);
    LibraryHooks::RegisterLibraryHook("api-ms-win-core-processthreads-l1-1-1.dll", NULL);
    LibraryHooks::RegisterLibraryHook("api-ms-win-core-processthreads-l1-1-2.dll", NULL);
    LibraryHooks::RegisterLibraryHook("ws2_32.dll", NULL);

    // we want to hook CreateProcess purely so that we can recursively insert our hooks (if we so
    // wish)
    CreateProcessA.Register("kernel32.dll", "CreateProcessA", CreateProcessA_hook);
    CreateProcessW.Register("kernel32.dll", "CreateProcessW", CreateProcessW_hook);

    CreateProcessAsUserA.Register("advapi32.dll", "CreateProcessAsUserA", CreateProcessAsUserA_hook);
    CreateProcessAsUserW.Register("advapi32.dll", "CreateProcessAsUserW", CreateProcessAsUserW_hook);

    CreateProcessWithLogonW.Register("advapi32.dll", "CreateProcessWithLogonW",
                                     CreateProcessWithLogonW_hook);

    LibraryHooks::RegisterLibraryHook("shell32.dll", NULL);

    // launchers commonly spawn a requireAdministrator child through ShellExecuteEx instead of
    // CreateProcess, which would bypass every hook above and lose the child process
    ShellExecuteExW.Register("shell32.dll", "ShellExecuteExW", ShellExecuteExW_hook);

    // handle API set exports if they exist. These don't really exist so we don't have to worry
    // about double hooking, and also they call into the 'real' implementation in kernelbase.dll
    API110CreateProcessA.Register("api-ms-win-core-processthreads-l1-1-0.dll", "CreateProcessA",
                                  API110CreateProcessA_hook);
    API110CreateProcessW.Register("api-ms-win-core-processthreads-l1-1-0.dll", "CreateProcessW",
                                  API110CreateProcessW_hook);
    API110CreateProcessAsUserW.Register("api-ms-win-core-processthreads-l1-1-0.dll",
                                        "CreateProcessAsUserW", API110CreateProcessAsUserW_hook);

    API111CreateProcessA.Register("api-ms-win-core-processthreads-l1-1-1.dll", "CreateProcessA",
                                  API111CreateProcessA_hook);
    API111CreateProcessW.Register("api-ms-win-core-processthreads-l1-1-1.dll", "CreateProcessW",
                                  API111CreateProcessW_hook);
    API111CreateProcessAsUserW.Register("api-ms-win-core-processthreads-l1-1-0.dll",
                                        "CreateProcessAsUserW", API111CreateProcessAsUserW_hook);

    API112CreateProcessA.Register("api-ms-win-core-processthreads-l1-1-2.dll", "CreateProcessA",
                                  API112CreateProcessA_hook);
    API112CreateProcessW.Register("api-ms-win-core-processthreads-l1-1-2.dll", "CreateProcessW",
                                  API112CreateProcessW_hook);
    API112CreateProcessAsUserW.Register("api-ms-win-core-processthreads-l1-1-0.dll",
                                        "CreateProcessAsUserW", API112CreateProcessAsUserW_hook);

    WSAStartup.Register("ws2_32.dll", "WSAStartup", WSAStartup_hook);
    WSACleanup.Register("ws2_32.dll", "WSACleanup", WSACleanup_hook);

    m_RecurseSlot = Threading::AllocateTLSSlot();
    Threading::SetTLSValue(m_RecurseSlot, NULL);
  }

private:
  static SysHook syshooks;

  int m_WSARefCount;
  uint64_t m_RecurseSlot = 0;

  bool CheckRecurse()
  {
    if(Threading::GetTLSValue(m_RecurseSlot) == NULL)
    {
      Threading::SetTLSValue(m_RecurseSlot, (void *)1);
      return false;
    }

    return true;
  }
  void EndRecurse() { Threading::SetTLSValue(m_RecurseSlot, NULL); }
  HookedFunction<PFN_CREATE_PROCESS_A> CreateProcessA;
  HookedFunction<PFN_CREATE_PROCESS_W> CreateProcessW;

  HookedFunction<PFN_CREATE_PROCESS_A> API110CreateProcessA;
  HookedFunction<PFN_CREATE_PROCESS_W> API110CreateProcessW;
  HookedFunction<PFN_CREATE_PROCESS_A> API111CreateProcessA;
  HookedFunction<PFN_CREATE_PROCESS_W> API111CreateProcessW;
  HookedFunction<PFN_CREATE_PROCESS_A> API112CreateProcessA;
  HookedFunction<PFN_CREATE_PROCESS_W> API112CreateProcessW;

  HookedFunction<PFN_CREATE_PROCESS_AS_USER_A> CreateProcessAsUserA;
  HookedFunction<PFN_CREATE_PROCESS_AS_USER_W> CreateProcessAsUserW;

  HookedFunction<PFN_CREATE_PROCESS_AS_USER_W> API110CreateProcessAsUserW;
  HookedFunction<PFN_CREATE_PROCESS_AS_USER_W> API111CreateProcessAsUserW;
  HookedFunction<PFN_CREATE_PROCESS_AS_USER_W> API112CreateProcessAsUserW;

  HookedFunction<PFN_CREATE_PROCESS_WITH_LOGON_W> CreateProcessWithLogonW;

  HookedFunction<PFN_SHELLEXECUTE_EXW> ShellExecuteExW;

  HookedFunction<PFN_WSASTARTUP> WSAStartup;
  HookedFunction<PFN_WSACLEANUP> WSACleanup;

  static int WSAAPI WSAStartup_hook(WORD wVersionRequested, LPWSADATA lpWSAData)
  {
    int ret = syshooks.WSAStartup()(wVersionRequested, lpWSAData);

    // only increment the refcount if the function succeeded
    if(ret == 0)
      syshooks.m_WSARefCount++;

    return ret;
  }

  static int WSAAPI WSACleanup_hook()
  {
    // don't let the application murder our sockets with a mismatched WSACleanup() call
    if(syshooks.m_WSARefCount == 1)
    {
      RDCLOG("WSACleanup called with (to the application) no WSAStartup! Ignoring.");
      SetLastError(WSANOTINITIALISED);
      return SOCKET_ERROR;
    }

    // decrement refcount and call the real thing
    syshooks.m_WSARefCount--;
    return syshooks.WSACleanup()();
  }

  static BOOL WINAPI
  Hooked_CreateProcess(const char *entryPoint,
                       std::function<BOOL(DWORD dwCreationFlags, LPVOID pEnvironment,
                                          LPPROCESS_INFORMATION lpProcessInformation)>
                           realFunc,
                       DWORD dwCreationFlags, bool inject, LPVOID pEnvironment,
                       LPPROCESS_INFORMATION lpProcessInformation)
  {
    bool recursive = syshooks.CheckRecurse();

    if(recursive)
      return realFunc(dwCreationFlags, pEnvironment, lpProcessInformation);

    PROCESS_INFORMATION dummy;
    RDCEraseEl(dummy);

    // not sure if this is valid, but I need the PID so I'll fill in my own struct to ensure that.
    if(lpProcessInformation == NULL)
    {
      lpProcessInformation = &dummy;
    }
    else
    {
      *lpProcessInformation = dummy;
    }

    bool resume = (dwCreationFlags & CREATE_SUSPENDED) == 0;
    dwCreationFlags |= CREATE_SUSPENDED;

    rdcstr envA;
    std::wstring envW;
    void *env = pEnvironment;
    const bool unicode_env = (dwCreationFlags & CREATE_UNICODE_ENVIRONMENT) != 0;

// give ourselves access to the ANSI version if we want it
#undef GetEnvironmentStrings

    static_assert(std::is_same<decltype(GetEnvironmentStrings()), char *>::value,
                  "GetEnvironmentStrings macro is messing up");

    // if we have no existing environment, take it from the current env strings that will be used
    // implicitly so we can patch it
    if(!env)
      env = unicode_env ? (void *)GetEnvironmentStringsW() : (void *)GetEnvironmentStrings();

    // patch the environment string to remove vulkan layer variable
    if(unicode_env)
    {
      const wchar_t *cur = (const wchar_t *)env;

      // loop over every A=B\0 string
      while(*cur)
      {
        // if it is NOT the vulkan env var, append it to our block
        if(wcsncmp(cur, CONCAT(L, RENDERDOC_VULKAN_LAYER_VAR), sizeof(RENDERDOC_VULKAN_LAYER_VAR) - 1))
        {
          envW += cur;
          envW.push_back(L'\0');
        }

        cur += wcslen(cur) + 1;
      }

      // append the extra \0 to terminate the block
      envW.push_back(L'\0');

      // use the patched block
      env = (void *)envW.data();
    }
    else
    {
      const char *cur = (const char *)env;

      // loop over every A=B\0 string
      while(*cur)
      {
        // if it is NOT the vulkan env var, append it to our block
        if(strncmp(cur, RENDERDOC_VULKAN_LAYER_VAR, sizeof(RENDERDOC_VULKAN_LAYER_VAR) - 1))
        {
          envA += cur;
          envA.push_back('\0');
        }

        cur += strlen(cur) + 1;
      }

      // append the extra \0 to terminate the block
      envA.push_back('\0');

      // use the patched block
      env = (void *)envA.data();
    }

    RDCDEBUG("Calling real %s", entryPoint);
    BOOL ret = realFunc(dwCreationFlags, env, lpProcessInformation);
    RDCDEBUG("Called real %s", entryPoint);

    if(ret && inject)
    {
      RDCDEBUG("Intercepting %s", entryPoint);

      // inherit logfile and capture options. The child was created suspended above and hasn't
      // run, so thread hijack injection is allowed for it - but only when the application
      // itself didn't ask for a suspended child, since the hijack path lets it run during
      // injection and would otherwise break the CREATE_SUSPENDED contract.
      rdcpair<RDResult, uint32_t> res = Process::InjectIntoProcess(
          lpProcessInformation->dwProcessId, {}, RenderDoc::Inst().GetCaptureFileTemplate(),
          RenderDoc::Inst().GetCaptureOptions(), false, resume);

      if(res.first == ResultCode::Succeeded)
        RenderDoc::Inst().AddChildProcess((uint32_t)lpProcessInformation->dwProcessId, res.second);
    }

    if(resume)
    {
      ResumeThread(lpProcessInformation->hThread);
    }

    // ensure we clean up after ourselves
    if(dummy.dwProcessId != 0)
    {
      CloseHandle(dummy.hProcess);
      CloseHandle(dummy.hThread);
    }

    syshooks.EndRecurse();

    return ret;
  }

  static bool ShouldInject(LPCWSTR lpApplicationName, LPCWSTR lpCommandLine)
  {
    if(!RenderDoc::Inst().GetCaptureOptions().hookIntoChildren)
      return false;

    if(Driver_NTEEarlyChildCapture())
    {
      wchar_t currentExe[MAX_PATH] = {};
      DWORD currentLen = GetModuleFileNameW(NULL, currentExe, MAX_PATH);
      if(currentLen > 0 && currentLen < MAX_PATH)
      {
        const wchar_t *currentName = wcsrchr(currentExe, L'\\');
        currentName = currentName ? currentName + 1 : currentExe;

        // The observed capture chain is NTEGame.exe -> HTGame.exe. The launcher also starts
        // NTEBrowser.exe, while the game starts service helpers; none render the game frame.
        if(_wcsicmp(currentName, L"HTGame.exe") == 0)
          return false;

        if(_wcsicmp(currentName, L"NTEGame.exe") == 0)
        {
          if(!lpApplicationName)
            return false;

          const wchar_t *childName = wcsrchr(lpApplicationName, L'\\');
          childName = childName ? childName + 1 : lpApplicationName;
          return _wcsicmp(childName, L"HTGame.exe") == 0;
        }
      }
    }

    bool inject = true;

    // sanity check to make sure we're not going to go into an infinity loop injecting into
    // ourselves.
    if(lpApplicationName)
    {
      rdcstr app = strlower(StringFormat::Wide2UTF8(lpApplicationName));

      if(app.contains("renderdiccmd.exe") || app.contains("qrenderdic.exe"))
      {
        inject = false;
      }
    }
    if(lpCommandLine)
    {
      rdcstr cmd = strlower(StringFormat::Wide2UTF8(lpCommandLine));

      if(cmd.contains("renderdiccmd.exe") || cmd.contains("qrenderdic.exe"))
      {
        inject = false;
      }
    }

    return inject;
  }

  static bool ShouldInject(LPCSTR lpApplicationName, LPCSTR lpCommandLine)
  {
    if(!RenderDoc::Inst().GetCaptureOptions().hookIntoChildren)
      return false;

    return ShouldInject(lpApplicationName ? StringFormat::UTF82Wide(lpApplicationName).c_str() : NULL,
                        lpCommandLine ? StringFormat::UTF82Wide(lpCommandLine).c_str() : NULL);
  }

  static BOOL WINAPI CreateProcessA_hook(
      __in_opt LPCSTR lpApplicationName, __inout_opt LPSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCSTR lpCurrentDirectory,
      __in LPSTARTUPINFOA lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessA()(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                           lpThreadAttributes, bInheritHandles, flags, env,
                                           lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI CreateProcessW_hook(__in_opt LPCWSTR lpApplicationName,
                                         __inout_opt LPWSTR lpCommandLine,
                                         __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                         __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                         __in BOOL bInheritHandles, __in DWORD dwCreationFlags,
                                         __in_opt LPVOID lpEnvironment,
                                         __in_opt LPCWSTR lpCurrentDirectory,
                                         __in LPSTARTUPINFOW lpStartupInfo,
                                         __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessW()(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                           lpThreadAttributes, bInheritHandles, flags, env,
                                           lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API110CreateProcessA_hook(
      __in_opt LPCSTR lpApplicationName, __inout_opt LPSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCSTR lpCurrentDirectory,
      __in LPSTARTUPINFOA lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API110CreateProcessA()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API110CreateProcessW_hook(
      __in_opt LPCWSTR lpApplicationName, __inout_opt LPWSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCWSTR lpCurrentDirectory,
      __in LPSTARTUPINFOW lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API110CreateProcessW()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API111CreateProcessA_hook(
      __in_opt LPCSTR lpApplicationName, __inout_opt LPSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCSTR lpCurrentDirectory,
      __in LPSTARTUPINFOA lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API111CreateProcessA()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API111CreateProcessW_hook(
      __in_opt LPCWSTR lpApplicationName, __inout_opt LPWSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCWSTR lpCurrentDirectory,
      __in LPSTARTUPINFOW lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API111CreateProcessW()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API112CreateProcessA_hook(
      __in_opt LPCSTR lpApplicationName, __inout_opt LPSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCSTR lpCurrentDirectory,
      __in LPSTARTUPINFOA lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API112CreateProcessA()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API112CreateProcessW_hook(
      __in_opt LPCWSTR lpApplicationName, __inout_opt LPWSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCWSTR lpCurrentDirectory,
      __in LPSTARTUPINFOW lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API112CreateProcessW()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI CreateProcessAsUserA_hook(
      HANDLE hToken, LPCSTR lpApplicationName, LPSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory,
      LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessAsUserA()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI CreateProcessAsUserW_hook(
      HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
      LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessAsUserW()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI CreateProcessWithLogonW_hook(LPCWSTR lpUsername, LPCWSTR lpDomain,
                                                  LPCWSTR lpPassword, DWORD dwLogonFlags,
                                                  LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                                  DWORD dwCreationFlags, LPVOID lpEnvironment,
                                                  LPCWSTR lpCurrentDirectory,
                                                  LPSTARTUPINFOW lpStartupInfo,
                                                  LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessWithLogonW()(lpUsername, lpDomain, lpPassword, dwLogonFlags,
                                                    lpApplicationName, lpCommandLine, flags, env,
                                                    lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI ShellExecuteExW_hook(SHELLEXECUTEINFOW *pExecInfo)
  {
    if(syshooks.CheckRecurse())
      return syshooks.ShellExecuteExW()(pExecInfo);

    // ShellExecuteEx has no way to create the child suspended, so unlike the CreateProcess hooks
    // above we can only inject after the original call returns and the child is already starting
    // to run. Only propagate into executables - ShellExecuteEx is also how launchers open URLs
    // and documents, and hooking the default browser was never the intent.
    bool isExe = false;
    if(pExecInfo->lpFile)
    {
      rdcstr file = strlower(StringFormat::Wide2UTF8(pExecInfo->lpFile));
      isExe = file.size() >= 4 && file.substr(file.size() - 4) == ".exe";
    }

    bool inject = isExe && ShouldInject(pExecInfo->lpFile, pExecInfo->lpParameters);

    DWORD oldMask = 0;
    if(inject)
    {
      // ask for a process handle so we can identify the child, undone again below
      oldMask = pExecInfo->fMask;
      pExecInfo->fMask |= SEE_MASK_NOCLOSEPROCESS;
    }

    // The NTE launcher uses ShellExecuteExW(open) for HTGame.exe. ShellExecuteExW returns after
    // the child has started, which can leave an already-created native DXGI factory outside our
    // wrappers. For this exact, opt-in case CreateProcessW is equivalent to opening the exe, and
    // lets us keep its main thread suspended until ordinary user-mode injection is complete.
    if(inject && Driver_NTEEarlyChildCapture() && pExecInfo->lpFile &&
       (!pExecInfo->lpVerb || _wcsicmp(pExecInfo->lpVerb, L"open") == 0) &&
       (oldMask & ~SEE_MASK_NOCLOSEPROCESS) == 0)
    {
      const wchar_t *filename = wcsrchr(pExecInfo->lpFile, L'\\');
      filename = filename ? filename + 1 : pExecInfo->lpFile;

      if(_wcsicmp(filename, L"HTGame.exe") == 0)
      {
        std::wstring commandLine = L"\"";
        commandLine += pExecInfo->lpFile;
        commandLine += L"\"";
        if(pExecInfo->lpParameters && *pExecInfo->lpParameters)
        {
          commandLine += L" ";
          commandLine += pExecInfo->lpParameters;
        }

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = (WORD)pExecInfo->nShow;
        PROCESS_INFORMATION child = {};

        if(::CreateProcessW(pExecInfo->lpFile, &commandLine[0], NULL, NULL, FALSE,
                            CREATE_SUSPENDED, NULL, pExecInfo->lpDirectory, &startup, &child))
        {
          RDCLOG("NTE early capture: created HTGame.exe suspended as process %lu", child.dwProcessId);

          rdcpair<RDResult, uint32_t> res = Process::InjectIntoProcess(
              child.dwProcessId, {}, RenderDoc::Inst().GetCaptureFileTemplate(),
              RenderDoc::Inst().GetCaptureOptions(), false, true);

          if(res.first == ResultCode::Succeeded)
            RenderDoc::Inst().AddChildProcess(child.dwProcessId, res.second);
          else
            RDCWARN("NTE early capture: injection failed for process %lu: %s", child.dwProcessId,
                    res.first.message.c_str());

          ResumeThread(child.hThread);
          CloseHandle(child.hThread);

          pExecInfo->hInstApp = (HINSTANCE)33;
          if(oldMask & SEE_MASK_NOCLOSEPROCESS)
            pExecInfo->hProcess = child.hProcess;
          else
          {
            CloseHandle(child.hProcess);
            pExecInfo->hProcess = NULL;
          }

          syshooks.EndRecurse();
          return TRUE;
        }

        RDCWARN("NTE early capture: CreateProcessW failed (%lu), using ShellExecuteExW",
                GetLastError());
      }
    }

    RDCDEBUG("Calling real ShellExecuteExW");
    BOOL ret = syshooks.ShellExecuteExW()(pExecInfo);
    RDCDEBUG("Called real ShellExecuteExW");

    if(ret && inject && pExecInfo->hProcess)
    {
      RDCLOG("Intercepting ShellExecuteExW child %ls", pExecInfo->lpFile);

      uint32_t pid = GetProcessId(pExecInfo->hProcess);

      rdcpair<RDResult, uint32_t> res =
          Process::InjectIntoProcess(pid, {}, RenderDoc::Inst().GetCaptureFileTemplate(),
                                     RenderDoc::Inst().GetCaptureOptions(), false, false);

      if(res.first == ResultCode::Succeeded)
        RenderDoc::Inst().AddChildProcess(pid, res.second);
    }

    if(inject && !(oldMask & SEE_MASK_NOCLOSEPROCESS))
    {
      // the caller didn't ask to own a process handle, so close it and don't hand one out
      CloseHandle(pExecInfo->hProcess);
      pExecInfo->hProcess = NULL;
    }

    syshooks.EndRecurse();
    return ret;
  }

  static BOOL WINAPI API110CreateProcessAsUserW_hook(
      HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
      LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API110CreateProcessAsUserW()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API111CreateProcessAsUserW_hook(
      HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
      LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API111CreateProcessAsUserW()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API112CreateProcessAsUserW_hook(
      HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
      LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API112CreateProcessAsUserW()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }
};

SysHook SysHook::syshooks;
