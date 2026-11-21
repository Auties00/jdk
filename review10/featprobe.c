// Review-only (scrub10). Reads, on a Windows host, what static detection of the Server 2025 exception
// context bug (JDK-8365196) would use:
//   - the OS version (RtlGetVersion) and UBR (registry);
//   - the state of feature 58474193 (0x037C3AD1,
//     Feature_Servicing_ExceptionDispatchUninitializedUserXStateHeaderFix) through ntdll's undocumented
//     RtlQueryFeatureConfiguration, for the boot and the runtime configuration;
//   - XCR0, the CPUID 0xD component layout, and the length L of the XSTATE chunk the kernel would
//     place at (rsp - L) & -64, computed as the compacted size of the XSAVE header plus the enabled user
//     components (the legacy area excluded);
//   - the documented context sizes (GetEnabledXStateFeatures, InitializeContext2).
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef LONG (NTAPI* RtlGetVersion_t)(PRTL_OSVERSIONINFOW);

typedef struct {
  ULONG FeatureId;
  ULONG Flags; // Priority:4, EnabledState:2, IsWexpConfiguration:1, HasSubscriptions:1, Variant:6, VariantPayloadKind:2
  ULONG VariantPayload;
} FEATURE_CONFIGURATION;

typedef LONG (NTAPI* RtlQueryFeatureConfiguration_t)(ULONG, ULONG, ULONGLONG*, FEATURE_CONFIGURATION*);
typedef DWORD64 (WINAPI* GetEnabledXStateFeatures_t)(void);
typedef BOOL (WINAPI* InitializeContext2_t)(PVOID, DWORD, PCONTEXT*, PDWORD, ULONG64);

int main(void) {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  RTL_OSVERSIONINFOW v = { sizeof(v) };
  ((RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion"))(&v);
  DWORD ubr = 0, sz = sizeof(ubr);
  RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
               NULL, &ubr, &sz);
  wchar_t inst[64] = L"?";
  sz = sizeof(inst);
  RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"InstallationType",
               RRF_RT_REG_SZ, NULL, inst, &sz);
  printf("os %lu.%lu.%lu.%lu %ls\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, ubr, inst);

  RtlQueryFeatureConfiguration_t q =
      (RtlQueryFeatureConfiguration_t)GetProcAddress(ntdll, "RtlQueryFeatureConfiguration");
  if (q == NULL) {
    printf("feature api=absent\n");
  } else {
    for (ULONG type = 0; type <= 1; type++) {
      FEATURE_CONFIGURATION c;
      memset(&c, 0, sizeof(c));
      ULONGLONG stamp = 0;
      LONG st = q(58474193, type, &stamp, &c);
      printf("feature type=%lu status=0x%08lx id=%lu flags=0x%08lx priority=%lu state=%lu wexp=%lu variant=%lu payload=0x%lx stamp=%llu\n",
             type, (unsigned long)st, c.FeatureId, c.Flags, c.Flags & 15, (c.Flags >> 4) & 3,
             (c.Flags >> 6) & 1, (c.Flags >> 8) & 63, c.VariantPayload, stamp);
    }
  }

  int r[4];
  uint64_t xcr0 = _xgetbv(0);
  uint64_t xinuse = 0;
  __cpuidex(r, 0xD, 1);
  int xgetbv1 = (r[0] >> 2) & 1;
  if (xgetbv1) xinuse = _xgetbv(1);
  printf("xcr0=0x%llx xinuse=0x%llx\n", (unsigned long long)xcr0, (unsigned long long)xinuse);
  __cpuidex(r, 0xD, 0);
  printf("cpuid 0xd.0 ebx(xcr0 standard size)=%d ecx(max)=%d\n", r[1], r[2]);
  __cpuidex(r, 0xD, 1);
  printf("cpuid 0xd.1 eax=0x%x ebx(compacted size, xcr0|xss)=%d\n", r[0], r[1]);
  // The compacted layout of the user components in XCR0, after the legacy area (512) and the header (64).
  int offset = 512 + 64;
  for (int i = 2; i < 63; i++) {
    if (!((xcr0 >> i) & 1)) continue;
    __cpuidex(r, 0xD, i);
    int size = r[0], std_offset = r[1], align64 = (r[2] >> 1) & 1;
    if (align64) offset = (offset + 63) & ~63;
    printf("component %d size=%d std_offset=%d align64=%d compacted_offset=%d\n", i, size, std_offset, align64, offset);
    offset += size;
  }
  printf("L_formula=%d (compacted size %d minus the legacy area)\n", offset - 512, offset);

  GetEnabledXStateFeatures_t gf = (GetEnabledXStateFeatures_t)GetProcAddress(k32, "GetEnabledXStateFeatures");
  if (gf != NULL) printf("GetEnabledXStateFeatures=0x%llx\n", (unsigned long long)gf());
  InitializeContext2_t ic2 = (InitializeContext2_t)GetProcAddress(k32, "InitializeContext2");
  DWORD len = 0;
  PCONTEXT ctx = NULL;
  if (ic2 != NULL) {
    ic2(NULL, CONTEXT_ALL | CONTEXT_XSTATE, NULL, &len, xcr0);
    void* buf = malloc(len);
    if (ic2(buf, CONTEXT_ALL | CONTEXT_XSTATE, &ctx, &len, xcr0)) {
      DWORD* ex = (DWORD*)(ctx + 1); // CONTEXT_EX: All, Legacy, XState as (LONG Offset, DWORD Length)
      printf("InitializeContext2 length=%lu xstate_chunk_length=%lu\n", len, ex[5]);
    } else {
      printf("InitializeContext2 failed %lu\n", GetLastError());
    }
  }
  return 0;
}
