// Review-only probe: which sources reveal the RUNNING kernel's revision or image identity,
// natively and inside a process-isolated container (server silo).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <wmistr.h>
#include <evntrace.h>
#include <intrin.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef LONG (NTAPI* NtQSIEx_t)(ULONG, PVOID, ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
static HMODULE ntdll;
static NtQSI_t q;
static NtQSIEx_t qex;

typedef struct {
  USHORT LayerNumber, LayerCount;
  ULONG OsMajorVersion, OsMinorVersion, NtBuildNumber, NtBuildQfe;
  UCHAR LayerName[128], NtBuildBranch[128], NtBuildLab[128], NtBuildLabEx[128], NtBuildStamp[26], NtBuildArch[16];
  ULONG Flags;
} BuildVersionInfo;

typedef struct {
  HANDLE Section; PVOID MappedBase; PVOID ImageBase; ULONG ImageSize; ULONG Flags;
  USHORT LoadOrderIndex, InitOrderIndex, LoadCount, OffsetToFileName; UCHAR FullPathName[256];
} ModInfo;
typedef struct {
  USHORT NextOffset; ModInfo Base; ULONG ImageChecksum; ULONG TimeDateStamp; PVOID DefaultBase;
} ModInfoEx;

static void pe_from_headers(const BYTE* b, const char* what) {
  const IMAGE_DOS_HEADER* d = (const IMAGE_DOS_HEADER*)b;
  if (d->e_magic != IMAGE_DOS_SIGNATURE) { printf("  %s: not PE\n", what); return; }
  const IMAGE_NT_HEADERS64* n = (const IMAGE_NT_HEADERS64*)(b + d->e_lfanew);
  printf("  %s: TimeDateStamp=0x%08lx CheckSum=0x%08lx SizeOfImage=0x%lx\n", what, n->FileHeader.TimeDateStamp,
         n->OptionalHeader.CheckSum, n->OptionalHeader.SizeOfImage);
}

static void file_info(const wchar_t* path) {
  DWORD h, sz = GetFileVersionInfoSizeW(path, &h);
  printf("file %ls:", path);
  if (sz == 0) { printf(" no version (err %lu)\n", GetLastError()); }
  else {
    void* buf = malloc(sz); VS_FIXEDFILEINFO* fi; UINT fl;
    if (GetFileVersionInfoW(path, 0, sz, buf) && VerQueryValueW(buf, L"\\", (void**)&fi, &fl))
      printf(" %u.%u.%u.%u\n", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    else printf(" unreadable (err %lu)\n", GetLastError());
    free(buf);
  }
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) { printf("  open failed (err %lu)\n", GetLastError()); return; }
  BYTE hdr[4096]; DWORD rd = 0;
  if (ReadFile(f, hdr, sizeof(hdr), &rd, NULL) && rd >= 1024) pe_from_headers(hdr, "on-disk PE");
  wchar_t fin[1024];
  if (GetFinalPathNameByHandleW(f, fin, 1024, VOLUME_NAME_NT)) printf("  final NT path: %ls\n", fin);
  CloseHandle(f);
}

static void mem_module(const char* name) {
  HMODULE m = GetModuleHandleA(name);
  if (!m) { printf("loaded %s: not loaded\n", name); return; }
  wchar_t p[1024] = L"?", mp[1024] = L"?";
  GetModuleFileNameW(m, p, 1024);
  GetMappedFileNameW(GetCurrentProcess(), m, mp, 1024);
  printf("loaded %s: path=%ls mapped=%ls\n", name, p, mp);
  pe_from_headers((const BYTE*)m, "in-memory PE");
  HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), (LPCWSTR)RT_VERSION);
  if (r) {
    BYTE* d = (BYTE*)LoadResource(m, r); DWORD sz = SizeofResource(m, r);
    for (DWORD i = 0; d && i + sizeof(VS_FIXEDFILEINFO) <= sz; i += 4) {
      VS_FIXEDFILEINFO* fi = (VS_FIXEDFILEINFO*)(d + i);
      if (fi->dwSignature == 0xFEEF04BD) {
        printf("  in-memory version resource: %u.%u.%u.%u\n", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        break;
      }
    }
  }
}

typedef struct { UNICODE_STRING Name; UNICODE_STRING TypeName; } ObjDirInfo;
typedef LONG (NTAPI* NtOpenDirectoryObject_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
typedef LONG (NTAPI* NtQueryDirectoryObject_t)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);
typedef LONG (NTAPI* NtOpenSymbolicLinkObject_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
typedef LONG (NTAPI* NtQuerySymbolicLinkObject_t)(HANDLE, PUNICODE_STRING, PULONG);
typedef VOID (NTAPI* RtlInitUnicodeString_t)(PUNICODE_STRING, PCWSTR);
static RtlInitUnicodeString_t rius;

static void list_dir(const wchar_t* dir, int max) {
  NtOpenDirectoryObject_t od = (NtOpenDirectoryObject_t)GetProcAddress(ntdll, "NtOpenDirectoryObject");
  NtQueryDirectoryObject_t qd = (NtQueryDirectoryObject_t)GetProcAddress(ntdll, "NtQueryDirectoryObject");
  NtOpenSymbolicLinkObject_t os = (NtOpenSymbolicLinkObject_t)GetProcAddress(ntdll, "NtOpenSymbolicLinkObject");
  NtQuerySymbolicLinkObject_t qs = (NtQuerySymbolicLinkObject_t)GetProcAddress(ntdll, "NtQuerySymbolicLinkObject");
  UNICODE_STRING u; rius(&u, dir);
  OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE, NULL, NULL);
  HANDLE h; LONG st = od(&h, 0x0001 | 0x0002, &oa);
  printf("objdir %ls: status=0x%08lx\n", dir, (unsigned long)st);
  if (st < 0) return;
  static BYTE buf[1 << 16]; ULONG ctx = 0, ret; int n = 0;
  while (qd(h, buf, sizeof(buf), TRUE, FALSE, &ctx, &ret) >= 0 && n < max) {
    ObjDirInfo* e = (ObjDirInfo*)buf;
    printf("  %.*ls [%.*ls]", e->Name.Length / 2, e->Name.Buffer, e->TypeName.Length / 2, e->TypeName.Buffer);
    if (e->TypeName.Length == 24 && !memcmp(e->TypeName.Buffer, L"SymbolicLink", 24)) {
      OBJECT_ATTRIBUTES oa2; InitializeObjectAttributes(&oa2, &e->Name, OBJ_CASE_INSENSITIVE, h, NULL);
      HANDLE l; if (os(&l, 0x0001, &oa2) >= 0) {
        wchar_t t[512]; UNICODE_STRING tu = { 0, sizeof(t), t }; ULONG rl;
        if (qs(l, &tu, &rl) >= 0) printf(" -> %.*ls", tu.Length / 2, tu.Buffer);
        CloseHandle(l);
      }
    }
    printf("\n"); n++;
  }
  CloseHandle(h);
}

typedef LONG (NTAPI* NtOpenKey_t)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
static void nt_reg(const wchar_t* key, const wchar_t* dwvalue, const wchar_t* szvalue) {
  NtOpenKey_t ok = (NtOpenKey_t)GetProcAddress(ntdll, "NtOpenKey");
  UNICODE_STRING u; rius(&u, key);
  OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE, NULL, NULL);
  HANDLE h; LONG st = ok(&h, KEY_READ, &oa);
  printf("ntreg %ls: status=0x%08lx", key, (unsigned long)st);
  if (st >= 0) {
    DWORD v = 0, sz = sizeof(v);
    if (dwvalue && RegQueryValueExW((HKEY)h, dwvalue, NULL, NULL, (BYTE*)&v, &sz) == 0) printf(" %ls=%lu", dwvalue, v);
    wchar_t s[512]; sz = sizeof(s);
    if (szvalue && RegQueryValueExW((HKEY)h, szvalue, NULL, NULL, (BYTE*)s, &sz) == 0) printf(" %ls=%ls", szvalue, s);
    wchar_t name[256]; DWORD i = 0, nl = 256; int printed = 0;
    if (!dwvalue && !szvalue) while (RegEnumKeyExW((HKEY)h, i++, name, &nl, NULL, NULL, NULL, NULL) == 0 && printed++ < 40) { printf(" [%ls]", name); nl = 256; }
    CloseHandle(h);
  }
  printf("\n");
}

typedef struct { EVENT_TRACE_PROPERTIES p; wchar_t name[128]; wchar_t file[MAX_PATH]; } TraceProps;

static void etw(void) {
  TraceProps s;
  memset(&s, 0, sizeof(s));
  wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
  wchar_t file[MAX_PATH]; swprintf(file, MAX_PATH, L"%lskprobe2-%lu.etl", tmp, GetCurrentProcessId());
  s.p.Wnode.BufferSize = sizeof(s); s.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID; s.p.Wnode.ClientContext = 1;
  s.p.LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL; s.p.LoggerNameOffset = offsetof(TraceProps, name);
  s.p.LogFileNameOffset = offsetof(TraceProps, file); wcscpy(s.file, file);
  TRACEHANDLE th = 0;
  ULONG e = StartTraceW(&th, L"kprobe2-session", &s.p);
  printf("ETW StartTrace: err=%lu\n", e);
  if (e != 0) return;
  ControlTraceW(th, NULL, &s.p, EVENT_TRACE_CONTROL_STOP);
  EVENT_TRACE_LOGFILEW lf; memset(&lf, 0, sizeof(lf)); lf.LogFileName = file;
  TRACEHANDLE oh = OpenTraceW(&lf);
  if (oh == (TRACEHANDLE)INVALID_HANDLE_VALUE) { printf("ETW OpenTrace failed err=%lu\n", GetLastError()); }
  else {
    TRACE_LOGFILE_HEADER* hd = &lf.LogfileHeader;
    printf("ETW logfile header: Version=%u.%u.%u.%u ProviderVersion=%lu NumberOfProcessors=%lu\n", hd->VersionDetail.MajorVersion,
           hd->VersionDetail.MinorVersion, hd->VersionDetail.SubVersion, hd->VersionDetail.SubMinorVersion, hd->ProviderVersion, hd->NumberOfProcessors);
    CloseTrace(oh);
  }
  HANDLE f = CreateFileW(file, GENERIC_READ, 7, NULL, OPEN_EXISTING, 0, NULL);
  if (f != INVALID_HANDLE_VALUE) {
    static BYTE b[1 << 16]; DWORD rd = 0; ReadFile(f, b, sizeof(b), &rd, NULL); CloseHandle(f);
    printf("ETW first buffer strings:");
    for (DWORD i = 0; i + 16 < rd; i += 2) {
      DWORD j = i; while (j + 1 < rd && b[j] >= 0x20 && b[j] < 0x7f && b[j + 1] == 0) j += 2;
      if (j - i >= 16) { printf(" \"%.*ls\"", (int)((j - i) / 2), (wchar_t*)(b + i)); i = j; }
    }
    printf("\n");
  }
  DeleteFileW(file);
}

static void scan_buf(const char* what, int cls, const BYTE* b, size_t n, ULONG needle) {
  char a[16]; wchar_t w[16]; int al = sprintf(a, "%lu", needle); for (int i = 0; i <= al; i++) w[i] = a[i];
  int hits = 0;
  for (size_t i = 0; i + 4 <= n && hits < 6; i++) {
    int kind = 0;
    if ((i & 3) == 0 && *(const ULONG*)(b + i) == needle) kind = 1;
    else if (i + al <= n && !memcmp(b + i, a, al)) kind = 2;
    else if (i + 2 * al <= n && !memcmp(b + i, w, 2 * al)) kind = 3;
    if (kind) { printf("NEEDLE %s %d: offset 0x%zx of %zu as %s\n", what, cls, i, n, kind == 1 ? "dword" : kind == 2 ? "ascii" : "utf16"); hits++; }
  }
}

static void scan_classes(ULONG needle) {
  static BYTE buf[1 << 20];
  static const ULONG sizes[] = { 4, 8, 12, 16, 20, 24, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 160, 192, 256, 320, 384, 512, 580, 1024, 4096 };
  int ok = 0;
  for (int cls = 0; cls <= 300; cls++) {
    if (cls == 77 || cls == 11) continue;
    memset(buf, 0, 8192); ULONG ret = 0;
    LONG st = q(cls, buf, sizeof(buf), &ret);
    if (st < 0 && ret > 0 && ret <= sizeof(buf)) { memset(buf, 0, 8192); st = q(cls, buf, ret, &ret); }
    for (int i = 0; st < 0 && i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) { memset(buf, 0, 8192); ret = 0; st = q(cls, buf, sizes[i], &ret); if (st >= 0 && ret == 0) ret = sizes[i]; }
    if (st >= 0) { ok++; if (ret > sizeof(buf) || ret == 0) ret = 8192; scan_buf("NtQuerySystemInformation class", cls, buf, ret, needle); }
    // the Ex form with a zero ULONG and a zero USHORT/HANDLE input
    for (int in = 0; in < 2 && qex; in++) {
      ULONGLONG input = 0; memset(buf, 0, 8192); ret = 0;
      LONG st2 = qex(cls, &input, in ? 8 : 4, buf, sizeof(buf), &ret);
      if (st2 < 0 && ret > 0 && ret <= sizeof(buf)) { memset(buf, 0, 8192); st2 = qex(cls, &input, in ? 8 : 4, buf, ret, &ret); }
      if (st2 >= 0) { if (ret > sizeof(buf) || ret == 0) ret = 8192; scan_buf(in ? "NtQuerySystemInformationEx(8) class" : "NtQuerySystemInformationEx(4) class", cls, buf, ret, needle); }
    }
  }
  printf("scan: %d classes answered the plain query; needle=%lu\n", ok, needle);
  scan_buf("KUSER_SHARED_DATA", 0, (const BYTE*)0x7FFE0000, 0x1000, needle);
  scan_buf("PEB", 0, (const BYTE*)NtCurrentTeb()->ProcessEnvironmentBlock, 0x7d0, needle);
}

// Re-runs this program without administrator rights: the LUA form of the token, medium integrity.
static int run_restricted(const char* needle) {
  HANDLE tok, rt;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &tok)) { printf("restricted: OpenProcessToken err=%lu\n", GetLastError()); return 1; }
  if (!CreateRestrictedToken(tok, 0x4 /* LUA_TOKEN */ | DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 0, NULL, &rt)) { printf("restricted: CreateRestrictedToken err=%lu\n", GetLastError()); return 1; }
  SID_IDENTIFIER_AUTHORITY mla = SECURITY_MANDATORY_LABEL_AUTHORITY; PSID medium;
  AllocateAndInitializeSid(&mla, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0, 0, 0, 0, 0, &medium);
  TOKEN_MANDATORY_LABEL tml; tml.Label.Sid = medium; tml.Label.Attributes = SE_GROUP_INTEGRITY;
  if (!SetTokenInformation(rt, TokenIntegrityLevel, &tml, sizeof(tml) + GetLengthSid(medium))) printf("restricted: integrity err=%lu\n", GetLastError());
  char exe[MAX_PATH], cmd[MAX_PATH + 64]; GetModuleFileNameA(NULL, exe, MAX_PATH);
  sprintf(cmd, "\"%s\" %s", exe, needle);
  STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi;
  fflush(stdout);
  if (!CreateProcessAsUserA(rt, NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { printf("restricted: CreateProcessAsUser err=%lu\n", GetLastError()); return 1; }
  WaitForSingleObject(pi.hProcess, INFINITE);
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 2 && !strcmp(argv[1], "--restricted")) return run_restricted(argv[2]);
  ntdll = GetModuleHandleA("ntdll.dll");
  q = (NtQSI_t)GetProcAddress(ntdll, "NtQuerySystemInformation");
  qex = (NtQSIEx_t)GetProcAddress(ntdll, "NtQuerySystemInformationEx");
  rius = (RtlInitUnicodeString_t)GetProcAddress(ntdll, "RtlInitUnicodeString");

  { char user[256]; DWORD ul = 256; GetUserNameA(user, &ul);
    HANDLE tok; TOKEN_ELEVATION te = {0}; DWORD rl; DWORD il = 0;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok); GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &rl);
    BYTE ib[256]; if (GetTokenInformation(tok, TokenIntegrityLevel, ib, sizeof(ib), &rl)) { PSID sid = ((TOKEN_MANDATORY_LABEL*)ib)->Label.Sid; il = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1); }
    LUID dbg; PRIVILEGE_SET ps; BOOL has = FALSE; LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &dbg);
    ps.PrivilegeCount = 1; ps.Control = PRIVILEGE_SET_ALL_NECESSARY; ps.Privilege[0].Luid = dbg; ps.Privilege[0].Attributes = 0; PrivilegeCheck(tok, &ps, &has);
    static BYTE pb[4096]; int held = 0; if (GetTokenInformation(tok, TokenPrivileges, pb, sizeof(pb), &rl)) { TOKEN_PRIVILEGES* tp = (TOKEN_PRIVILEGES*)pb; for (DWORD i = 0; i < tp->PrivilegeCount; i++) if (tp->Privileges[i].Luid.LowPart == dbg.LowPart) held = 1; }
    printf("token: elevated=%lu integrity=0x%lx SeDebugPrivilege held=%d enabled=%d\n", te.TokenIsElevated, il, held, has); }

  RTL_OSVERSIONINFOW v = { sizeof(v) };
  ((LONG (NTAPI*)(PRTL_OSVERSIONINFOW))GetProcAddress(ntdll, "RtlGetVersion"))(&v);
  printf("RtlGetVersion: %lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
  volatile BYTE* kusd = (volatile BYTE*)0x7FFE0000;
  printf("KUSER_SHARED_DATA: NtBuildNumber=%lu NtMajor=%lu NtMinor=%lu NtSystemRoot=%ls\n", *(volatile ULONG*)(kusd + 0x260),
         *(volatile ULONG*)(kusd + 0x26C), *(volatile ULONG*)(kusd + 0x270), (wchar_t*)(kusd + 0x30));

  for (ULONG layer = 0; qex && layer < 8; layer++) {
    BuildVersionInfo bi; memset(&bi, 0, sizeof(bi)); ULONG ret = 0;
    LONG st = qex(222, &layer, sizeof(layer), &bi, sizeof(bi), &ret);
    printf("class222 layer %lu: status=0x%08lx", layer, (unsigned long)st);
    if (st >= 0) printf(" count=%u %lu.%lu.%lu.%lu name=%.32s branch=%.48s labex=%.64s stamp=%.26s arch=%.16s flags=0x%lx", bi.LayerCount, bi.OsMajorVersion,
                        bi.OsMinorVersion, bi.NtBuildNumber, bi.NtBuildQfe, bi.LayerName, bi.NtBuildBranch, bi.NtBuildLabEx, bi.NtBuildStamp, bi.NtBuildArch, bi.Flags);
    printf("\n");
    if (st < 0 || layer + 1 >= bi.LayerCount) break;
  }

  static BYTE mods[4 << 20]; ULONG need = 0;
  LONG st = q(11, mods, sizeof(mods), &need);
  printf("class11 SystemModuleInformation: status=0x%08lx", (unsigned long)st);
  if (st >= 0) { ModInfo* m = (ModInfo*)(mods + 8); printf(" n=%lu first=%s base=%p size=0x%lx", *(ULONG*)mods, m->FullPathName, m->ImageBase, m->ImageSize); }
  printf("\n");
  memset(mods, 0, sizeof(mods));
  st = q(77, mods, sizeof(mods), &need);
  printf("class77 SystemModuleInformationEx: status=0x%08lx need=%lu\n", (unsigned long)st, need);
  if (st >= 0) {
    ModInfoEx* e = (ModInfoEx*)mods; int i = 0;
    for (;;) {
      const char* nm = (const char*)e->Base.FullPathName + e->Base.OffsetToFileName;
      if (i < 4 || !_stricmp(nm, "ntoskrnl.exe") || !_stricmp(nm, "hal.dll") || !_stricmp(nm, "win32k.sys") || !_stricmp(nm, "ci.dll") || strstr(nm, "hotpatch") || strstr(nm, "_hp"))
        printf("  [%d] %s base=%p size=0x%lx checksum=0x%08lx timestamp=0x%08lx defbase=%p\n", i, e->Base.FullPathName, e->Base.ImageBase,
               e->Base.ImageSize, e->ImageChecksum, e->TimeDateStamp, e->DefaultBase);
      i++;
      if (e->NextOffset == 0) break;
      e = (ModInfoEx*)((BYTE*)e + e->NextOffset);
    }
    printf("  total %d modules\n", i);
  }

  { LPVOID drv[8]; DWORD cb;
    if (EnumDeviceDrivers(drv, sizeof(drv), &cb)) { wchar_t n[512] = L"?"; GetDeviceDriverFileNameW(drv[0], n, 512); printf("EnumDeviceDrivers: first=%p %ls\n", drv[0], n); }
    else printf("EnumDeviceDrivers: err=%lu\n", GetLastError()); }

  { typedef LONG (NTAPI* RQFC)(ULONG, ULONG, PULONGLONG, PVOID);
    RQFC f = (RQFC)GetProcAddress(ntdll, "RtlQueryFeatureConfiguration");
    if (f) { ULONGLONG cs = 0; ULONG cfg[4] = {0}; LONG s = f(58474193, 1, &cs, cfg); printf("RtlQueryFeatureConfiguration(58474193): status=0x%08lx cfg=%08lx %08lx %08lx\n", (unsigned long)s, cfg[0], cfg[1], cfg[2]); } }

  { int r[4]; __cpuid(r, 1); printf("CPUID.1 ECX.hypervisor=%d\n", (r[2] >> 31) & 1);
    if ((r[2] >> 31) & 1) for (int l = 0x40000000; l <= 0x40000006; l++) { __cpuid(r, l); printf("CPUID.%08x: eax=%08x ebx=%08x ecx=%08x edx=%08x\n", l, r[0], r[1], r[2], r[3]);
      if (l == 0x40000002) printf("  hypervisor version: build=%u major=%u minor=%u sp=%u svcbranch=%u svcnumber=%u\n", r[0], (unsigned)r[1] >> 16, r[1] & 0xffff, r[2], (unsigned)r[3] >> 24, r[3] & 0xffffff); } }

  mem_module("ntdll.dll"); mem_module("kernel32.dll"); mem_module("kernelbase.dll");
  wchar_t sys[MAX_PATH], p[1024]; GetSystemDirectoryW(sys, MAX_PATH);
  static const wchar_t* names[] = { L"ntoskrnl.exe", L"ntdll.dll", L"kernel32.dll", L"hal.dll", L"win32k.sys", L"ci.dll" };
  for (int i = 0; i < 6; i++) { swprintf(p, 1024, L"%ls\\%ls", sys, names[i]); file_info(p); }
  for (int n = 1; n <= 12; n++) {
    swprintf(p, 1024, L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume%d\\Windows\\System32\\ntoskrnl.exe", n);
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES) { DWORD e = GetLastError(); if (e != 2 && e != 3) printf("volume %d: err=%lu\n", n, e); }
    else file_info(p);
  }
  file_info(L"\\\\?\\GLOBALROOT\\Device\\BootDevice\\Windows\\System32\\ntoskrnl.exe");
  file_info(L"\\\\?\\GLOBALROOT\\SystemRoot\\System32\\ntoskrnl.exe");
  { wchar_t vol[MAX_PATH]; HANDLE fv = FindFirstVolumeW(vol, MAX_PATH);
    if (fv != INVALID_HANDLE_VALUE) { do { printf("volume %ls\n", vol); swprintf(p, 1024, L"%lsWindows\\System32\\ntoskrnl.exe", vol);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) file_info(p); } while (FindNextVolumeW(fv, vol, MAX_PATH)); FindVolumeClose(fv); } }

  list_dir(L"\\", 80);
  list_dir(L"\\Device", 300);
  list_dir(L"\\Silos", 20);
  nt_reg(L"\\Registry", NULL, NULL);
  nt_reg(L"\\Registry\\Machine", NULL, NULL);
  nt_reg(L"\\Registry\\WC", NULL, NULL);
  nt_reg(L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", L"CurrentBuild");
  nt_reg(L"\\Registry\\Machine\\SYSTEM\\Software\\Microsoft\\BuildLayers", NULL, NULL);
  nt_reg(L"\\Registry\\Machine\\SYSTEM\\Software\\Microsoft\\BuildLayers\\DesktopEditions", L"BuildQfe", L"BuildLabEx");
  nt_reg(L"\\Registry\\Machine\\SYSTEM\\Software\\Microsoft\\BuildLayers\\OnecoreUAP", L"BuildQfe", L"BuildLabEx");
  nt_reg(L"\\Registry\\Machine\\SYSTEM\\Software\\Microsoft\\BuildLayers\\ShellCommon", L"BuildQfe", L"BuildLabEx");
  nt_reg(L"\\Registry\\Machine\\SYSTEM\\Software\\Microsoft\\BuildLayers\\OSClient", L"BuildQfe", L"BuildLabEx");
  etw();
  fflush(stdout);
  if (argc > 1) scan_classes(strtoul(argv[1], NULL, 10));
  fflush(stdout);
  return 0;
}
