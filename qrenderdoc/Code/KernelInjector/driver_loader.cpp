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

#include "driver_loader.h"
#include "driver_resources.h"
#include "trace_cleanup.h"
#include "kernel_mem.h"
#include "nt_internals.h"

#if defined(Q_OS_WIN)

#include <windows.h>
#include <winternl.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDebug>

#include <cstdlib>
#include <ctime>
#include <memory>

namespace KernelInjector
{
namespace
{
// Registry path fragments for the kernel service.
const wchar_t kServicesPath[] = L"SYSTEM\\CurrentControlSet\\Services\\";
const wchar_t kRegistryPrefix[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";
const wchar_t kDosDevicePrefix[] = L"\\??\\";

const int kSeLoadDriverPrivilege = 10;
const int kSeDebugPrivilege = 20;
const int kServiceKernelDriver = 1;

const wchar_t *DevicePathFor(BackendId backend)
{
  switch(backend)
  {
    case BackendId::Portwell: return PortwellProtocol::kDevicePath;
    case BackendId::Tbt: return TbtProtocol::kDevicePath;
  }
  return nullptr;
}

const unsigned char *DriverBytesFor(BackendId backend, size_t *size)
{
  switch(backend)
  {
    case BackendId::Portwell:
      *size = portwell_driver_bytes_size;
      return portwell_driver_bytes;
    case BackendId::Tbt:
      *size = tbt_driver_bytes_size;
      return tbt_driver_bytes;
  }
  return nullptr;
}

// The PiDDB lookup matches on both name and PE timestamp, so the cleanup
// needs the embedded driver's TimeDateStamp.
}    // namespace

extern "C" NTSTATUS NTAPI RtlAdjustPrivilege(ULONG Privilege, BOOLEAN Enable, BOOLEAN CurrentThread,
                                             PBOOLEAN WasEnabled);
extern "C" NTSTATUS NTAPI NtLoadDriver(PUNICODE_STRING DriverServiceName);
extern "C" NTSTATUS NTAPI NtUnloadDriver(PUNICODE_STRING DriverServiceName);

uint32_t DriverLoader::DriverTimeDateStamp(BackendId backend)
{
  size_t size = 0;
  const unsigned char *bytes = nullptr;
  switch(backend)
  {
    case BackendId::Portwell:
      size = portwell_driver_bytes_size;
      bytes = portwell_driver_bytes;
      break;
    case BackendId::Tbt:
      size = tbt_driver_bytes_size;
      bytes = tbt_driver_bytes;
      break;
  }

  if(bytes == nullptr || size < sizeof(IMAGE_DOS_HEADER))
    return 0;

  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)bytes;
  if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > size)
    return 0;

  const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(bytes + dos->e_lfanew);
  if(nt->Signature != IMAGE_NT_SIGNATURE)
    return 0;

  return nt->FileHeader.TimeDateStamp;
}

QString DriverLoader::RandomServiceName()
{
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const int len = 10 + (rand() % 20);

  QString name;
  name.reserve(len);
  for(int i = 0; i < len; i++)
    name.append(QLatin1Char(alphabet[rand() % (sizeof(alphabet) - 1)]));
  return name;
}

bool DriverLoader::EnablePrivilege(int privilege)
{
  BOOLEAN wasEnabled = FALSE;
  return NT_SUCCESS(RtlAdjustPrivilege((ULONG)privilege, TRUE, FALSE, &wasEnabled));
}

bool DriverLoader::WriteTempFile(const QString &path, const void *bytes, size_t size)
{
  QFile file(path);
  if(!file.open(QIODevice::WriteOnly))
    return false;
  return file.write((const char *)bytes, (qint64)size) == (qint64)size;
}

bool DriverLoader::CreateServiceRegistryKey(const QString &serviceName, const QString &imagePath)
{
  HKEY key = nullptr;
  LONG status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, (kServicesPath + serviceName.toStdWString()).c_str(),
                                0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
  if(status != ERROR_SUCCESS)
    return false;

  bool ok = true;

  // QDir paths use '/' internally, which QFile accepts but the kernel object manager does not
  // accept in an NT path. Always write a fully native path to the service ImagePath value.
  const QString nativeImagePath = QDir::toNativeSeparators(QDir::cleanPath(imagePath));
  std::wstring imageValue = kDosDevicePrefix + nativeImagePath.toStdWString();
  status = RegSetValueExW(key, L"ImagePath", 0, REG_EXPAND_SZ, (const BYTE *)imageValue.c_str(),
                          (DWORD)((imageValue.size() + 1) * sizeof(wchar_t)));
  ok = ok && status == ERROR_SUCCESS;

  DWORD type = kServiceKernelDriver;
  status = RegSetValueExW(key, L"Type", 0, REG_DWORD, (const BYTE *)&type, sizeof(type));
  ok = ok && status == ERROR_SUCCESS;

  RegCloseKey(key);
  return ok;
}

bool DriverLoader::DeleteServiceRegistryKey(const QString &serviceName)
{
  LONG status = RegDeleteKeyW(HKEY_LOCAL_MACHINE, (kServicesPath + serviceName.toStdWString()).c_str());
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool DriverLoader::NtLoadDriverByName(const QString &serviceName)
{
  std::wstring regPath = kRegistryPrefix + serviceName.toStdWString();
  UNICODE_STRING us;
  us.Length = (USHORT)(regPath.size() * sizeof(wchar_t));
  us.MaximumLength = (USHORT)((regPath.size() + 1) * sizeof(wchar_t));
  us.Buffer = (PWSTR)regPath.data();
  return NT_SUCCESS(NtLoadDriver(&us));
}

bool DriverLoader::NtUnloadDriverByName(const QString &serviceName)
{
  std::wstring regPath = kRegistryPrefix + serviceName.toStdWString();
  UNICODE_STRING us;
  us.Length = (USHORT)(regPath.size() * sizeof(wchar_t));
  us.MaximumLength = (USHORT)((regPath.size() + 1) * sizeof(wchar_t));
  us.Buffer = (PWSTR)regPath.data();
  return NT_SUCCESS(NtUnloadDriver(&us));
}

void *DriverLoader::OpenDevice(const wchar_t *devicePath)
{
  HANDLE h = CreateFileW(devicePath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  return (h != INVALID_HANDLE_VALUE) ? h : nullptr;
}

bool DriverLoader::Load(BackendId backend, LoadedDriver *out, QString *errorDetail)
{
  if(out == nullptr || errorDetail == nullptr)
    return false;

  *errorDetail = QString();
  srand((unsigned)time(nullptr) ^ GetCurrentThreadId());

  const wchar_t *devicePath = DevicePathFor(backend);
  if(devicePath == nullptr)
  {
    *errorDetail = QStringLiteral("Unknown backend");
    return false;
  }

  // Bail out if the driver is already loaded (by a previous session).
  {
    void *existing = OpenDevice(devicePath);
    if(existing != nullptr)
    {
      CloseHandle((HANDLE)existing);
      *errorDetail = QStringLiteral("The kernel driver device is already in use. "
                                    "A previous session may have left it loaded - restart the machine.");
      return false;
    }
  }

  LoadedDriver driver;
  driver.serviceName = RandomServiceName();

  driver.tempPath = QDir::toNativeSeparators(
      QDir::cleanPath(QDir(QDir::tempPath()).filePath(driver.serviceName)));

  size_t driverSize = 0;
  const unsigned char *driverBytes = DriverBytesFor(backend, &driverSize);

  QFile::remove(driver.tempPath);
  if(!WriteTempFile(driver.tempPath, driverBytes, driverSize))
  {
    *errorDetail = QStringLiteral("Failed to write driver file to %1").arg(driver.tempPath);
    return false;
  }

  const QFileInfo writtenDriver(driver.tempPath);
  if(!writtenDriver.exists() || writtenDriver.size() != (qint64)driverSize)
  {
    *errorDetail = QStringLiteral("The temporary driver image is missing or incomplete: %1")
                       .arg(driver.tempPath);
    QFile::remove(driver.tempPath);
    return false;
  }

  if(!EnablePrivilege(kSeDebugPrivilege))
    qWarning() << "KernelInjector: failed to enable SeDebugPrivilege";

  if(!EnablePrivilege(kSeLoadDriverPrivilege))
  {
    *errorDetail = QStringLiteral("Failed to acquire SeLoadDriverPrivilege. "
                                  "Make sure you are running as administrator.");
    QFile::remove(driver.tempPath);
    return false;
  }

  if(!CreateServiceRegistryKey(driver.serviceName, driver.tempPath))
  {
    *errorDetail = QStringLiteral("Failed to create the service registry key");
    QFile::remove(driver.tempPath);
    return false;
  }

  NTSTATUS loadStatus = STATUS_UNSUCCESSFUL;
  {
    std::wstring regPath = kRegistryPrefix + driver.serviceName.toStdWString();
    UNICODE_STRING us;
    us.Length = (USHORT)(regPath.size() * sizeof(wchar_t));
    us.MaximumLength = (USHORT)((regPath.size() + 1) * sizeof(wchar_t));
    us.Buffer = (PWSTR)regPath.data();
    loadStatus = NtLoadDriver(&us);
  }

  if(!NT_SUCCESS(loadStatus))
  {
    if(loadStatus == (NTSTATUS)0xC0000603L)
    {
      *errorDetail = QStringLiteral(
          "The vulnerable driver list blocked the driver load. Disable "
          "VulnerableDriverBlocklistEnable under HKLM\\SYSTEM\\CurrentControlSet\\Control\\CI\\Config "
          "or use a driver that is not blocked.");
    }
    else if(loadStatus == (NTSTATUS)0xC0000022L || loadStatus == (NTSTATUS)0xC000000AL)
    {
      *errorDetail = QStringLiteral("Access denied or insufficient resources (0x%1) - "
                                    "an anti-cheat or antivirus may be blocking the load.")
                         .arg((quint32)loadStatus, 8, 16, QLatin1Char('0'));
    }
    else if(loadStatus == (NTSTATUS)0xC000003AL)
    {
      *errorDetail = QStringLiteral("The temporary driver image path was not found: %1")
                         .arg(driver.tempPath);
    }
    else
    {
      *errorDetail = QStringLiteral("NtLoadDriver failed with status 0x%1")
                         .arg((quint32)loadStatus, 8, 16, QLatin1Char('0'));
    }

    DeleteServiceRegistryKey(driver.serviceName);
    QFile::remove(driver.tempPath);
    return false;
  }

  driver.device = OpenDevice(devicePath);
  if(driver.device == nullptr)
  {
    *errorDetail = QStringLiteral("Failed to open the driver device %1").arg(QString::fromWCharArray(devicePath));
    NtUnloadDriverByName(driver.serviceName);
    DeleteServiceRegistryKey(driver.serviceName);
    QFile::remove(driver.tempPath);
    return false;
  }

  // Sanity check the R/W primitive before touching any kernel structure.
  {
    std::unique_ptr<KernelMem> probe;
    if(backend == BackendId::Portwell)
      probe.reset(new PortwellBackend(driver.device));
    else
      probe.reset(new TbtBackend(driver.device));

    if(!probe->SelfTest())
    {
      *errorDetail = QStringLiteral("The driver backend self-test failed - the R/W contract does not "
                                    "match this driver version");
      Unload(driver);
      return false;
    }
  }

  *out = driver;
  return true;
}

void DriverLoader::Unload(const LoadedDriver &driver)
{
  if(driver.device != nullptr)
  {
    CloseHandle((HANDLE)driver.device);
  }

  NtUnloadDriverByName(driver.serviceName);
  DeleteServiceRegistryKey(driver.serviceName);

  // Overwrite the temp file with random data before deleting it so the driver
  // bytes cannot be recovered from disk (kdmapper's Unload behaviour).
  QFile file(driver.tempPath);
  if(file.open(QIODevice::WriteOnly))
  {
    const qint64 overwriteLen = 64 * 1024 + (rand() % (2 * 1024 * 1024));
    QByteArray junk((int)overwriteLen, 0);
    for(qint64 i = 0; i < overwriteLen; i++)
      junk[(int)i] = (char)(rand() & 0xFF);
    file.write(junk);
    file.close();
  }

  QFile::remove(driver.tempPath);
}
}    // namespace KernelInjector

#endif    // Q_OS_WIN
