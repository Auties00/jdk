// Review-only (JDK-8365196, scrub14). Native Windows/AArch64: does resuming from an exception keep
// the SVE state (predicate registers, FFR, Z registers) when stale stack memory below the stack
// pointer looks like the header of an extended-state area? Prints the XState configuration of the
// system, the layout of an exception context, and the result of faults taken with several header
// patterns planted below sp at several stack alignments.
// The SVE code is a machine-code blob (no assembler needed); its encodings are listed in mkblob().
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#ifndef PF_ARM_SVE_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SVE_INSTRUCTIONS_AVAILABLE 46
#endif
#ifndef PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE 47
#endif

typedef struct { LONG Offset; DWORD Length; } probe_chunk;
typedef struct { probe_chunk All, Legacy, XState; } probe_context_ex;

typedef void (*blob_fn)(void* pred_in, void* pred_out, void* fault, void* z_in, void* z_out,
                        uint64_t plant_lo, uint64_t plant_hi, uint64_t plant_len);
typedef uint64_t (*rdvl_fn)(void);

static uint32_t* code;
static uint32_t* trap_pc;
static volatile int traps;
static volatile DWORD seen_flags;
static volatile uintptr_t seen_ctx, seen_sp;
static volatile int dump_next;

static int zregs[] = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
#define NZ (sizeof(zregs) / sizeof(zregs[0]))

static uint32_t* mkblob(uint32_t* p) {
  int i;
  for (i = 0; i < 16; i++) *p++ = 0x85800000u | ((uint32_t)(i >> 3) << 16) | ((uint32_t)(i & 7) << 10) | (0 << 5) | i; // ldr p<i>, [x0, #i, mul vl]
  *p++ = 0x25289000u | (15 << 5);                                                                       // wrffr p15
  for (i = 0; i < (int)NZ; i++) *p++ = 0x85804000u | ((uint32_t)(i >> 3) << 16) | ((uint32_t)(i & 7) << 10) | (3 << 5) | zregs[i]; // ldr z<n>, [x3, #i, mul vl]
  *p++ = 0x910003EAu;                                    // mov x10, sp
  *p++ = 0xCB07014Bu;                                    // sub x11, x10, x7
  *p++ = 0xA8800000u | (2 << 15) | (6 << 10) | (11 << 5) | 5; // stp x5, x6, [x11], #16
  *p++ = 0xEB0A017Fu;                                    // cmp x11, x10
  *p++ = 0x54FFFFC3u;                                    // b.lo -2
  trap_pc = p;
  *p++ = 0xF9400049u;                                    // ldr x9, [x2]   (faults)
  for (i = 0; i < 16; i++) *p++ = 0xE5800000u | ((uint32_t)(i >> 3) << 16) | ((uint32_t)(i & 7) << 10) | (1 << 5) | i; // str p<i>, [x1, #i, mul vl]
  *p++ = 0x2519F000u | 0;                                // rdffr p0
  *p++ = 0xE5800000u | (2 << 16) | (0 << 10) | (1 << 5) | 0; // str p0, [x1, #16, mul vl]
  for (i = 0; i < (int)NZ; i++) *p++ = 0xE5804000u | ((uint32_t)(i >> 3) << 16) | ((uint32_t)(i & 7) << 10) | (4 << 5) | zregs[i]; // str z<n>, [x4, #i, mul vl]
  *p++ = 0xD65F03C0u;                                    // ret
  return p;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      ep->ExceptionRecord->ExceptionAddress == (void*)trap_pc) {
    CONTEXT* c = ep->ContextRecord;
    traps++;
    seen_flags = c->ContextFlags;
    seen_ctx = (uintptr_t)c;
    seen_sp = (uintptr_t)c->Sp;
    if (dump_next) {
      probe_context_ex* ex = (probe_context_ex*)(c + 1);
      int f;
      dump_next = 0;
      printf("  context at sp-0x%llx, ContextFlags 0x%lx, sizeof(CONTEXT) 0x%x\n", (unsigned long long)(seen_sp - seen_ctx),
             (unsigned long)c->ContextFlags, (unsigned)sizeof(CONTEXT));
      printf("  CONTEXT_EX: All %ld/%lu Legacy %ld/%lu XState %ld/%lu\n", ex->All.Offset, ex->All.Length, ex->Legacy.Offset,
             ex->Legacy.Length, ex->XState.Offset, ex->XState.Length);
      if (ex->XState.Length >= 64 && ex->XState.Length < 0x10000) {
        unsigned char* h = (unsigned char*)ex + ex->XState.Offset;
        int k;
        printf("  XState area at sp-0x%llx, first 64 bytes:", (unsigned long long)(seen_sp - (uintptr_t)h));
        for (k = 0; k < 64; k++) printf("%s%02x", (k % 8) ? "" : " ", h[k]);
        printf("\n");
      }
      for (f = 0; f < 64; f++) {
        DWORD len = 0;
        void* q = LocateXStateFeature(c, f, &len);
        if (q != NULL) printf("  LocateXStateFeature(%d): sp-0x%llx length %lu\n", f, (unsigned long long)(seen_sp - (uintptr_t)q), (unsigned long)len);
      }
    }
    c->Pc += 4;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static unsigned char pin[64 * 17], pout[64 * 17], zin[256 * 32], zout[256 * 32];

// Runs the blob with the stack pointer lowered by pad bytes (a multiple of 16).
static __declspec(noinline) int run(int pad, uint64_t lo, uint64_t hi, uint64_t plant_len, int vl, int dump) {
  volatile char* a = (volatile char*)_alloca(pad + 16);
  int bad = 0, i, pl = vl / 8;
  a[0] = 0;
  memset(pout, 0xEE, sizeof(pout));
  memset(zout, 0xEE, sizeof(zout));
  dump_next = dump;
  ((blob_fn)code)(pin, pout, NULL, zin, zout, lo, hi, plant_len);
  for (i = 0; i < 16; i++) if (memcmp(pin + i * pl, pout + i * pl, pl) != 0) bad |= 1;
  if (memcmp(pin + 15 * pl, pout + 16 * pl, pl) != 0) bad |= 2;
  for (i = 0; i < (int)NZ; i++) if (memcmp(zin + i * vl, zout + i * vl, vl) != 0) bad |= (memcmp(zin + i * vl, zout + i * vl, 16) != 0) ? 8 : 4;
  return bad;
}

static __declspec(noinline) void commit_stack(void) {
  volatile char big[0x20000];
  int i;
  for (i = 0; i < (int)sizeof(big); i += 0x800) big[i] = 1;
}

int main(void) {
  OSVERSIONINFOEXW v;
  typedef LONG (WINAPI* rtlgv)(OSVERSIONINFOEXW*);
  rtlgv f = (rtlgv)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
  const unsigned char* kusd = (const unsigned char*)0x7FFE0000;
  DWORD64 enabled = GetEnabledXStateFeatures();
  int sve = IsProcessorFeaturePresent(PF_ARM_SVE_INSTRUCTIONS_AVAILABLE);
  int sve2 = IsProcessorFeaturePresent(PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE);
  uint32_t* end;
  int vl, i, pad, pat, total = 0, lost = 0;
  struct { const char* name; uint64_t lo, hi; uint64_t len; } pats[] = {
    {"nothing planted", 0, 0, 16},
    {"zeros", 0, 0, 0x3000},
    {"{0, bit 63}", 0, 0x8000000000000000ull, 0x3000},
    {"{bit 63, 0}", 0x8000000000000000ull, 0, 0x3000},
    {"{0, bit 63 | 3}", 0, 0x8000000000000003ull, 0x3000},
    {"{3, bit 63 | 3}", 3, 0x8000000000000003ull, 0x3000},
    {"{1, bit 63 | 1}", 1, 0x8000000000000001ull, 0x3000},
    {"{bit 63, bit 63}", 0x8000000000000000ull, 0x8000000000000000ull, 0x3000},
    {"{0, bit 63 | all}", 0, 0xFFFFFFFFFFFFFFFFull, 0x3000},
    {"ones", 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0x3000},
  };
  memset(&v, 0, sizeof(v));
  v.dwOSVersionInfoSize = sizeof(v);
  if (f != NULL) f(&v);
  printf("Windows %lu.%lu.%lu, NtBuildNumber %u\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
         (unsigned)(*(const uint32_t*)(kusd + 0x260) & 0xffff));
  printf("PF_ARM_SVE %d, PF_ARM_SVE2 %d, GetEnabledXStateFeatures 0x%llx\n", sve, sve2, (unsigned long long)enabled);
  printf("KUSER_SHARED_DATA.XState: EnabledFeatures 0x%llx, EnabledVolatileFeatures 0x%llx, Size 0x%x, ControlFlags 0x%x "
         "(bit 0 OptimizedSave, bit 1 CompactionEnabled), EnabledSupervisorFeatures 0x%llx, AlignedFeatures 0x%llx\n",
         (unsigned long long)*(const uint64_t*)(kusd + 0x3D8), (unsigned long long)*(const uint64_t*)(kusd + 0x3E0),
         (unsigned)*(const uint32_t*)(kusd + 0x3E8), (unsigned)*(const uint32_t*)(kusd + 0x3EC),
         (unsigned long long)*(const uint64_t*)(kusd + 0x5F0), (unsigned long long)*(const uint64_t*)(kusd + 0x5F8));
  if (!sve) {
    printf("RESULT no-sve: Windows does not report SVE on this processor; nothing to lose\n");
    return 0;
  }
  code = (uint32_t*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  code[1000] = 0x04BF5020u; // rdvl x0, #1
  code[1001] = 0xD65F03C0u; // ret
  end = mkblob(code);
  {
    DWORD old;
    VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
  }
  vl = (int)((rdvl_fn)(code + 1000))();
  printf("SVE vector length %d bytes, blob %d instructions\n", vl, (int)(end - code));
  if (vl < 16 || vl > 256) {
    printf("RESULT error: unexpected vector length\n");
    return 2;
  }
  for (i = 0; i < (int)sizeof(pin); i++) pin[i] = (unsigned char)(0x5A ^ (i * 7 + 1));
  for (i = 0; i < (int)sizeof(zin); i++) zin[i] = (unsigned char)(0xA5 ^ (i * 13 + 3));
  AddVectoredExceptionHandler(1, veh);
  commit_stack();
  for (pat = 0; pat < (int)(sizeof(pats) / sizeof(pats[0])); pat++) {
    int bad_pat = 0, n = 0, kinds = 0;
    for (pad = 0; pad < 0x400; pad += 16) {
      int before = traps;
      int bad = run(pad, pats[pat].lo, pats[pat].hi, pats[pat].len, vl, pat <= 2 && pad == 0);
      if (traps != before + 1) {
        printf("RESULT error: the trap was not taken\n");
        return 2;
      }
      n++;
      total++;
      if (bad) { bad_pat++; lost++; kinds |= bad; }
    }
    printf("pattern %-18s: %d faults, state changed in %d (kinds 0x%x: 1 predicates, 2 FFR, 4 Z upper bits, 8 Z low 128 bits)\n",
           pats[pat].name, n, bad_pat, kinds);
  }
  printf("RESULT %s: %d faults, SVE state changed in %d\n", lost ? "LOSES" : "keeps", total, lost);
  return 0;
}
