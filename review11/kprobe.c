// Review-only (scrub11, review7 R1). Prints every source of the Windows version a process can read, and
// every container (server silo) signal, so that the JVM's decision can be built on the sources that
// reflect the running kernel, not the files of a container image.
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef LONG (NTAPI* RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
typedef VOID (NTAPI* RtlGetNtVersionNumbers_t)(DWORD*, DWORD*, DWORD*);
typedef LONG (NTAPI* NtQuerySystemInformationEx_t)(ULONG, PVOID, ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI* NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);

typedef struct {
  USHORT LayerNumber;
  USHORT LayerCount;
  ULONG OsMajorVersion;
  ULONG OsMinorVersion;
  ULONG NtBuildNumber;
  ULONG NtBuildQfe;
  UCHAR LayerName[128];
  UCHAR NtBuildBranch[128];
  UCHAR NtBuildLab[128];
  UCHAR NtBuildLabEx[128];
  UCHAR NtBuildStamp[26];
  UCHAR NtBuildArch[16];
  ULONG Flags;
} BuildVersionInfo;

typedef struct {
  DWORD SiloId;
  DWORD SiloParentId;
  DWORD NumberOfProcesses;
  BOOLEAN IsInServerSilo;
  BYTE Reserved[3];
} SiloBasic;

typedef struct {
  DWORD ServiceSessionId;
  DWORD State;
  DWORD ExitStatus;
  BOOLEAN IsDownlevelContainer;
  PVOID ApiSetSchema;
  PVOID HostApiSetSchema;
} ServerSiloBasic;

static void file_version(const char* name) {
  char path[MAX_PATH];
  UINT n = GetSystemDirectoryA(path, MAX_PATH - 40);
  if (n == 0) { printf("file %s: no system dir\n", name); return; }
  strcat(path, "\\");
  strcat(path, name);
  DWORD h, sz = GetFileVersionInfoSizeA(path, &h);
  if (sz == 0) { printf("file %s: no version (err %lu)\n", name, GetLastError()); return; }
  void* buf = malloc(sz);
  VS_FIXEDFILEINFO* fi; UINT fl;
  if (GetFileVersionInfoA(path, 0, sz, buf) && VerQueryValueA(buf, "\\", (void**)&fi, &fl)) {
    printf("file %s: %u.%u.%u.%u\n", name, HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
           HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
  } else {
    printf("file %s: unreadable (err %lu)\n", name, GetLastError());
  }
  free(buf);
}

static void reg_str(const char* key, const char* value) {
  char buf[512]; DWORD sz = sizeof(buf);
  LSTATUS st = RegGetValueA(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, NULL, buf, &sz);
  printf("reg %s\\%s: %s\n", key, value, st == ERROR_SUCCESS ? buf : "(absent)");
}

static void reg_dword(const char* key, const char* value) {
  DWORD v = 0, sz = sizeof(v);
  LSTATUS st = RegGetValueA(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD, NULL, &v, &sz);
  if (st == ERROR_SUCCESS) printf("reg %s\\%s: %lu\n", key, value, v); else printf("reg %s\\%s: (absent %ld)\n", key, value, st);
}

static LARGE_INTEGER freq;
static double us_since(LARGE_INTEGER s) {
  LARGE_INTEGER e; QueryPerformanceCounter(&e);
  return (double)(e.QuadPart - s.QuadPart) * 1e6 / (double)freq.QuadPart;
}

int main(void) {
  QueryPerformanceFrequency(&freq);
  HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  LARGE_INTEGER t;

  RTL_OSVERSIONINFOW v = { sizeof(v) };
  QueryPerformanceCounter(&t);
  ((RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion"))(&v);
  printf("RtlGetVersion: %lu.%lu.%lu (%.1f us)\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, us_since(t));

  DWORD ma = 0, mi = 0, bu = 0;
  RtlGetNtVersionNumbers_t gn = (RtlGetNtVersionNumbers_t)GetProcAddress(ntdll, "RtlGetNtVersionNumbers");
  if (gn) { gn(&ma, &mi, &bu); printf("RtlGetNtVersionNumbers: %lu.%lu.%lu (raw build 0x%lx)\n", ma, mi, bu & 0xffff, bu); }

  volatile BYTE* kusd = (volatile BYTE*)0x7FFE0000;
  printf("KUSER_SHARED_DATA: NtBuildNumber=0x%lx NtProductType=%lu NtMajor=%lu NtMinor=%lu\n",
         *(volatile ULONG*)(kusd + 0x260), *(volatile ULONG*)(kusd + 0x264), *(volatile ULONG*)(kusd + 0x26C),
         *(volatile ULONG*)(kusd + 0x270));

  PPEB peb = NtCurrentTeb()->ProcessEnvironmentBlock;
  BYTE* pb = (BYTE*)peb;
  printf("PEB: OSMajor=%lu OSMinor=%lu OSBuild=%u OSCSD=0x%x OSPlatformId=%lu\n", *(ULONG*)(pb + 0x118),
         *(ULONG*)(pb + 0x11C), *(USHORT*)(pb + 0x120), *(USHORT*)(pb + 0x122), *(ULONG*)(pb + 0x124));

  NtQuerySystemInformationEx_t qex = (NtQuerySystemInformationEx_t)GetProcAddress(ntdll, "NtQuerySystemInformationEx");
  for (ULONG layer = 0; qex && layer < 8; layer++) {
    BuildVersionInfo bi; memset(&bi, 0, sizeof(bi));
    ULONG ret = 0;
    QueryPerformanceCounter(&t);
    LONG st = qex(222, &layer, sizeof(layer), &bi, sizeof(bi), &ret);
    double el = us_since(t);
    printf("SystemBuildVersionInformation layer %lu: status=0x%08lx ret=%lu (%.1f us)", layer, (unsigned long)st, ret, el);
    if (st >= 0) {
      printf(" n=%u count=%u %lu.%lu.%lu.%lu name=%.32s branch=%.64s lab=%.64s labex=%.64s flags=0x%lx",
             bi.LayerNumber, bi.LayerCount, bi.OsMajorVersion, bi.OsMinorVersion, bi.NtBuildNumber, bi.NtBuildQfe,
             bi.LayerName, bi.NtBuildBranch, bi.NtBuildLab, bi.NtBuildLabEx, bi.Flags);
    }
    printf("\n");
    if (st < 0 || layer + 1 >= bi.LayerCount) break;
  }

  // NtQuerySystemInformation(SystemModuleInformation): the loaded kernel image's path.
  NtQuerySystemInformation_t q = (NtQuerySystemInformation_t)GetProcAddress(ntdll, "NtQuerySystemInformation");
  {
    ULONG need = 0;
    static BYTE mods[1 << 20];
    LONG st = q(11, mods, sizeof(mods), &need);
    if (st >= 0) {
      // RTL_PROCESS_MODULES: ULONG count; then RTL_PROCESS_MODULE_INFORMATION (FullPathName at +0x28 of each, 256 bytes; entry 0x128)
      ULONG n = *(ULONG*)mods;
      printf("SystemModuleInformation: %lu modules, first=%s\n", n, (char*)(mods + 8 + 0x28));
    } else {
      printf("SystemModuleInformation: status=0x%08lx\n", (unsigned long)st);
    }
  }

  BOOL injob = FALSE;
  IsProcessInJob(GetCurrentProcess(), NULL, &injob);
  printf("IsProcessInJob=%d\n", injob);
  for (int cls = 36; cls <= 37; cls++) {
    BYTE buf[256]; memset(buf, 0, sizeof(buf));
    DWORD ret = 0;
    QueryPerformanceCounter(&t);
    BOOL ok = QueryInformationJobObject(NULL, (JOBOBJECTINFOCLASS)cls, buf, cls == 36 ? sizeof(SiloBasic) : sizeof(ServerSiloBasic), &ret);
    double el = us_since(t);
    DWORD err = ok ? 0 : GetLastError();
    if (cls == 36) {
      SiloBasic* s = (SiloBasic*)buf;
      printf("JobObjectSiloBasicInformation: ok=%d err=%lu ret=%lu SiloId=%lu Parent=%lu Procs=%lu IsInServerSilo=%d (%.1f us)\n",
             ok, err, ret, s->SiloId, s->SiloParentId, s->NumberOfProcesses, s->IsInServerSilo, el);
    } else {
      ServerSiloBasic* s = (ServerSiloBasic*)buf;
      printf("JobObjectServerSiloBasicInformation: ok=%d err=%lu ret=%lu ServiceSession=%lu State=%lu IsDownlevel=%d (%.1f us)\n",
             ok, err, ret, s->ServiceSessionId, s->State, s->IsDownlevelContainer, el);
    }
  }

  {
    HKEY k;
    LSTATUS st = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending", 0, KEY_READ, &k);
    printf("CBS RebootPending: %s (%ld)\n", st == ERROR_SUCCESS ? "present" : "absent", st);
    if (st == ERROR_SUCCESS) RegCloseKey(k);
    st = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired", 0, KEY_READ, &k);
    printf("WU RebootRequired: %s (%ld)\n", st == ERROR_SUCCESS ? "present" : "absent", st);
    if (st == ERROR_SUCCESS) RegCloseKey(k);
  }
  {
    HANDLE j = CreateJobObjectA(NULL, NULL);
    BOOL as = AssignProcessToJobObject(j, GetCurrentProcess());
    SiloBasic sb; memset(&sb, 0, sizeof(sb)); DWORD ret = 0;
    BOOL ok = QueryInformationJobObject(NULL, (JOBOBJECTINFOCLASS)36, &sb, sizeof(sb), &ret);
    printf("nested job: assign=%d; JobObjectSiloBasicInformation ok=%d err=%lu SiloId=%lu IsInServerSilo=%d\n",
           as, ok, ok ? 0 : GetLastError(), sb.SiloId, sb.IsInServerSilo);
  }
  {
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH - 20);
    strcat(path, "\\CExecSvc.exe");
    QueryPerformanceCounter(&t);
    DWORD attr = GetFileAttributesA(path);
    printf("file CExecSvc.exe: %s (%.1f us)\n", attr == INVALID_FILE_ATTRIBUTES ? "absent" : "present", us_since(t));
  }
  QueryPerformanceCounter(&t);
  SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
  if (scm) {
    SC_HANDLE s = OpenServiceA(scm, "cexecsvc", SERVICE_QUERY_STATUS);
    printf("service cexecsvc: %s (err %lu)\n", s ? "present" : "absent", s ? 0 : GetLastError());
    if (s) CloseServiceHandle(s);
    CloseServiceHandle(scm);
  } else {
    printf("service manager: unavailable (err %lu)\n", GetLastError());
  }
  reg_dword("SYSTEM\\CurrentControlSet\\Control", "ContainerType");
  reg_str("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild");
  reg_dword("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "UBR");
  reg_str("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "BuildLabEx");
  reg_str("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "EditionID");
  reg_str("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "InstallationType");
  QueryPerformanceCounter(&t);
  file_version("ntoskrnl.exe");
  printf("ntoskrnl read took %.1f us\n", us_since(t));
  file_version("kernel32.dll");
  file_version("ntdll.dll");
  return 0;
}
