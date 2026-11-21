// Review-only probe for JDK-8365196 (review5).
//
// Questions it answers, on a Windows host, without a JVM:
//   A. Where the kernel places the XSTATE chunk of an exception context, for every rsp mod 64
//      that is a multiple of 8, and whether the formula (rsp - L) & -64 holds for all of them.
//   B. Whether the one-store scrub (XCOMP_BV := 0) at the measured L keeps the state, and a
//      wrong L does not (sanity).
//   C. Whether clearing XCOMP_BV alone is enough whatever else the stale header holds
//      (XSTATE_BV, the reserved quadwords), i.e. whether the kernel reads anything else.
//   D. Which components a stale compacted header with a partial mask loses (the rule the
//      VerifyExceptionXStateScrub check assumes: a compacted header that names every losable
//      component keeps them).
//   E. Random stale stack contents, with and without the scrub.
//   F. A second fault on a kernel-written header.
//   G. Whether L is the same on every thread, also after a thread used AMX.
//   H. (child process) whether an exception survives when rsp - L lies below the committed
//      stack (the guard page): explains STATUS_STACK_OVERFLOW without hs_err.
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

typedef struct {
  int64_t fill_qwords;
  int64_t sp_adjust;
  uint64_t* fill_ptr;
  int64_t scrub_len;
  int64_t flags;
  uint64_t trap_rsp;
  uint64_t base_rsp;
  uint64_t pad;
  uint32_t zmm0[16];
  uint32_t zmm16[16];
  uint64_t k1;
} Probe;

extern void xprobe_run(Probe* p);
extern void xprobe_fault(void);
extern void xprobe_cont(void);
extern void amx_ldtilecfg(const void* cfg);
extern void amx_tilezero0(void);
extern void amx_tilerelease(void);

#define FILLQ 4096 /* 32 KiB */

static volatile uint64_t g_trap_rsp, g_chunk;
static volatile uint32_t g_chunk_len;
static volatile int g_hits;
static int g_avx512, g_amx;
static uint64_t g_xcr0;

typedef struct { LONG Offset; DWORD Length; } CtxChunk;
typedef struct { CtxChunk All, Legacy, XState; } CtxEx;

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      ep->ExceptionRecord->ExceptionAddress == (PVOID)xprobe_fault) {
    PCONTEXT c = ep->ContextRecord;
    g_trap_rsp = c->Rsp;
    g_chunk = 0; g_chunk_len = 0;
    if ((c->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE) {
      CtxEx* ex = (CtxEx*)(c + 1);
      g_chunk = (uint64_t)((char*)ex + ex->XState.Offset);
      g_chunk_len = ex->XState.Length;
    }
    g_hits++;
    c->Rip = (DWORD64)xprobe_cont;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static uint64_t* g_fill;

// bits: 1 ymm0 upper (AVX), 2 zmm0 hi256, 4 zmm16, 8 k1, 16 xmm0 low (should never)
static int lost_bits(const Probe* p) {
  int b = 0;
  if (g_avx512) {
    for (int i = 0; i < 4; i++) if (p->zmm0[i] != 0x22222222u) b |= 16;
    for (int i = 4; i < 8; i++) if (p->zmm0[i] != 0x22222222u) b |= 1;
    for (int i = 8; i < 16; i++) if (p->zmm0[i] != 0x22222222u) b |= 2;
    for (int i = 0; i < 16; i++) if (p->zmm16[i] != 0x33333333u) b |= 4;
    if ((p->k1 & 0xffff) != 0xA5A5) b |= 8;
  } else {
    for (int i = 0; i < 4; i++) if (p->zmm0[i] != 0x11111111u) b |= 16;
    for (int i = 4; i < 8; i++) if (p->zmm0[i] != 0x11111111u) b |= 1;
  }
  return b;
}

static int run1(int64_t adjust, int64_t scrub, int64_t fillq, uint64_t base_rsp) {
  Probe p;
  memset(&p, 0, sizeof p);
  p.fill_qwords = fillq;
  p.sp_adjust = adjust;
  p.fill_ptr = g_fill;
  p.scrub_len = scrub;
  p.flags = g_avx512 ? 1 : 0;
  p.base_rsp = base_rsp;
  g_trap_rsp = 0; g_chunk = 0; g_chunk_len = 0;
  int before = g_hits;
  xprobe_run(&p);
  if (g_hits != before + 1) return -1;
  if (g_trap_rsp != p.trap_rsp) return -2;
  return lost_bits(&p);
}

static void fill_all(uint64_t v) { for (int i = 0; i < FILLQ; i++) g_fill[i] = v; }
// index in g_fill of the qword at address a below rsp
static int idx_of(uint64_t rsp, uint64_t a) { return (int)((rsp - a) / 8) - 1; }

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t xr(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static int g_L = 0;
static uint64_t g_rsp_at[8];

static void expA(void) {
  printf("== A: chunk position per rsp mod 64 (fill 0x8000000000000000 everywhere)\n");
  int dist[8], lostc = 0;
  for (int k = 0; k < 8; k++) {
    fill_all(0x8000000000000000ull);
    int r = run1(k * 8, 0, FILLQ, 0);
    g_rsp_at[k] = g_trap_rsp;
    dist[k] = g_chunk ? (int)(g_trap_rsp - g_chunk) : -1;
    if (r > 0) lostc++;
    printf("A sp%%64=%2d lost=0x%x dist=%d len=%u chunk%%64=%d\n", (int)(g_trap_rsp & 63), r, dist[k],
           (unsigned)g_chunk_len, (int)(g_chunk & 63));
  }
  // single L for the four 16-byte alignments, as the JVM derives it
  int mn = 1 << 30, mx = 0;
  for (int k = 0; k < 8; k += 2) { if (dist[k] < mn) mn = dist[k]; if (dist[k] > mx) mx = dist[k]; }
  for (int L = mn; L >= mx - 63 && L > 0; L--) {
    int ok = 1;
    for (int k = 0; k < 8; k += 2) if (L + (((int)(g_rsp_at[k] & 63) - L) & 63) != dist[k]) ok = 0;
    if (ok) { g_L = L; break; }
  }
  int ok8 = 1;
  for (int k = 0; k < 8; k++) if (g_L && g_L + (((int)(g_rsp_at[k] & 63) - g_L) & 63) != dist[k]) ok8 = 0;
  printf("A L=%d (from 16-aligned) formula holds for all 8-aligned=%d affected=%d\n", g_L, ok8, lostc > 0);
}

static void expB(void) {
  printf("== B: one-store scrub at L, and at wrong lengths\n");
  int64_t lens[] = { g_L, g_L - 64, g_L + 64, g_L - 8, g_L + 8 };
  for (int j = 0; j < 5; j++) {
    int lost = 0, err = 0;
    for (int k = 0; k < 8; k++) {
      fill_all(0x8000000000000000ull);
      int r = run1(k * 8, lens[j], FILLQ, 0);
      if (r < 0) err++; else if (r) lost++;
    }
    printf("B scrub_len=%lld lost_in=%d/8 err=%d\n", (long long)lens[j], lost, err);
  }
}

// Builds a fill with the header at the given rsp holding h[0..7], everything else 'rest'.
static void fill_header(uint64_t rsp, const uint64_t h[8], uint64_t rest, int random_rest) {
  for (int i = 0; i < FILLQ; i++) g_fill[i] = random_rest ? xr() : rest;
  uint64_t H = (rsp - g_L) & ~63ull;
  for (int q = 0; q < 8; q++) {
    int i = idx_of(rsp, H + 8 * q);
    if (i >= 0 && i < FILLQ) g_fill[i] = h[q];
  }
}

static void expC(void) {
  printf("== C: XCOMP_BV = 0 with other stale header contents\n");
  uint64_t xs[] = { 0, 1, 2, 3, 4, 7, 0xE0, 0xE7, 0x600E7, g_xcr0, g_xcr0 | 0x8000000000000000ull,
                    0x8000000000000000ull, ~0ull, 0x7FFFFFFFFFFFFFFFull, 0x80000000000000E7ull };
  int total = 0, lost = 0, err = 0;
  for (int a = 0; a < (int)(sizeof xs / sizeof xs[0]); a++) {
    for (int rv = 0; rv < 3; rv++) {
      for (int k = 0; k < 8; k++) {
        uint64_t h[8];
        h[0] = xs[a]; h[1] = 0;
        for (int q = 2; q < 8; q++) h[q] = rv == 0 ? 0 : rv == 1 ? ~0ull : xr();
        fill_header(g_rsp_at[k], h, 0x8000000000000000ull, rv == 2);
        int r = run1(k * 8, 0, FILLQ, 0);
        total++;
        if (r < 0) err++;
        else if (r) { lost++; printf("C LOST xstate_bv=0x%llx reserved=%d sp%%64=%d bits=0x%x\n",
                                     (unsigned long long)xs[a], rv, (int)(g_rsp_at[k] & 63), r); }
      }
    }
  }
  // random XSTATE_BV too
  for (int n = 0; n < 400; n++) {
    int k = n & 7;
    uint64_t h[8];
    for (int q = 0; q < 8; q++) h[q] = xr();
    h[1] = 0;
    fill_header(g_rsp_at[k], h, 0, 1);
    int r = run1(k * 8, 0, FILLQ, 0);
    total++;
    if (r < 0) err++; else if (r) { lost++; printf("C LOST random xstate_bv=0x%llx bits=0x%x\n", (unsigned long long)h[0], r); }
  }
  printf("C total=%d lost=%d err=%d\n", total, lost, err);
}

static void expD(void) {
  printf("== D: stale compacted header with partial masks (XCOMP_BV = bit63 | m)\n");
  uint64_t ms[] = { 0, 1, 2, 3, 4, 7, 0x20, 0x27, 0x40, 0x47, 0x80, 0x87, 0x67, 0xA7, 0xC7, 0xE3, 0xE4,
                    0xE5, 0xE6, 0xE7, 0x2E7, 0x600E7, g_xcr0, g_xcr0 | (1ull << 40), g_xcr0 | (1ull << 9),
                    g_xcr0 | (1ull << 11), g_xcr0 | (1ull << 62), 0x7FFFFFFFFFFFFFFFull };
  for (int a = 0; a < (int)(sizeof ms / sizeof ms[0]); a++) {
    for (int xsv = 0; xsv < 2; xsv++) {
      int bits_or = 0, bits_and = 0xff, err = 0;
      for (int k = 0; k < 8; k += 2) {
        uint64_t h[8] = { xsv ? ms[a] : 0, 0x8000000000000000ull | ms[a], 0, 0, 0, 0, 0, 0 };
        fill_header(g_rsp_at[k], h, 0, 0);
        int r = run1(k * 8, 0, FILLQ, 0);
        if (r < 0) { err++; continue; }
        bits_or |= r; bits_and &= r;
      }
      printf("D m=0x%llx xstate_bv=%s lost_or=0x%x lost_and=0x%x err=%d\n", (unsigned long long)ms[a],
             xsv ? "m" : "0", bits_or, bits_and, err);
    }
  }
}

static void expE(void) {
  printf("== E: random stale stack\n");
  int lost_s = 0, lost_n = 0, err = 0;
  for (int n = 0; n < 2000; n++) {
    int k = n & 7;
    for (int i = 0; i < FILLQ; i++) g_fill[i] = xr();
    uint64_t H = (g_rsp_at[k] - g_L) & ~63ull;
    int i1 = idx_of(g_rsp_at[k], H + 8);
    uint64_t keep = g_fill[i1];
    g_fill[i1] = 0;
    int r = run1(k * 8, 0, FILLQ, 0);
    if (r < 0) err++; else if (r) lost_s++;
    g_fill[i1] = keep;
    r = run1(k * 8, 0, FILLQ, 0);
    if (r < 0) err++; else if (r) lost_n++;
  }
  // random qwords with bit 63 set and a mask that is a subset of XCR0 at the header
  int lost_v = 0;
  for (int n = 0; n < 400; n++) {
    int k = n & 7;
    for (int i = 0; i < FILLQ; i++) g_fill[i] = xr();
    uint64_t H = (g_rsp_at[k] - g_L) & ~63ull;
    g_fill[idx_of(g_rsp_at[k], H + 8)] = 0x8000000000000000ull | (xr() & g_xcr0);
    int r = run1(k * 8, 0, FILLQ, 0);
    if (r < 0) err++; else if (r) lost_v++;
  }
  printf("E random: scrubbed lost=%d/2000 unscrubbed lost=%d/2000 header-like(bit63|subset) lost=%d/400 err=%d\n",
         lost_s, lost_n, lost_v, err);
}

static void expF(void) {
  printf("== F: fault twice on the kernel-written header\n");
  int lost = 0, err = 0;
  for (int k = 0; k < 8; k++) {
    fill_all(0);
    int r1 = run1(k * 8, 0, FILLQ, 0);
    int r2 = run1(k * 8, 0, 0, 0);   // no fill: the header the first fault wrote is still there
    if (r1 < 0 || r2 < 0) err++; else if (r1 || r2) lost++;
  }
  printf("F lost=%d/8 err=%d\n", lost, err);
}

// ---- G: per thread
static unsigned char tilecfg[64];
static int measure_L_here(void) {
  // L from the four 16-aligned positions, as the JVM does
  int dist[4]; uint64_t rsp[4];
  for (int k = 0; k < 4; k++) {
    fill_all(0x8000000000000000ull);
    if (run1(k * 16, 0, FILLQ, 0) < 0) return -1;
    rsp[k] = g_trap_rsp; dist[k] = g_chunk ? (int)(g_trap_rsp - g_chunk) : -1;
  }
  int mn = 1 << 30, mx = 0;
  for (int k = 0; k < 4; k++) { if (dist[k] < mn) mn = dist[k]; if (dist[k] > mx) mx = dist[k]; }
  for (int L = mn; L >= mx - 63 && L > 0; L--) {
    int ok = 1;
    for (int k = 0; k < 4; k++) if (L + (((int)(rsp[k] & 63) - L) & 63) != dist[k]) ok = 0;
    if (ok) return L;
  }
  return 0;
}
static DWORD WINAPI thr_plain(LPVOID arg) { *(int*)arg = measure_L_here(); return 0; }
static DWORD WINAPI thr_amx(LPVOID arg) {
  int* out = (int*)arg;
  out[0] = measure_L_here();
  __try {
    amx_ldtilecfg(tilecfg);
    amx_tilezero0();
    out[3] = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[3] = -(int)(GetExceptionCode() & 0xffff);
  }
  out[1] = measure_L_here();
  if (out[3] == 1) {
    // with the scrub at the L measured before AMX use
    int lost = 0;
    for (int k = 0; k < 8; k++) {
      fill_all(0x8000000000000000ull);
      int r = run1(k * 8, out[0], FILLQ, 0);
      if (r) lost++;
    }
    out[4] = lost;
    __try { amx_tilerelease(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
  out[2] = measure_L_here();
  return 0;
}
static void expG(void) {
  printf("== G: L per thread\n");
  int l1 = 0;
  HANDLE t = CreateThread(NULL, 1 << 20, thr_plain, &l1, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  WaitForSingleObject(t, INFINITE);
  printf("G fresh thread L=%d (main worker L=%d)\n", l1, g_L);
  if (g_amx) {
    memset(tilecfg, 0, sizeof tilecfg);
    tilecfg[0] = 1;                       // palette 1
    *(uint16_t*)&tilecfg[16] = 64;        // tile 0: 64 bytes per row
    tilecfg[48] = 16;                     // 16 rows
    int o[5] = { 0, 0, 0, 0, -1 };
    t = CreateThread(NULL, 1 << 20, thr_amx, o, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    WaitForSingleObject(t, INFINITE);
    printf("G amx thread: L before=%d after tile use=%d after release=%d amx_ok=%d lost_with_scrub_at_before_L=%d/8\n",
           o[0], o[1], o[2], o[3], o[4]);
    int l2 = 0;
    t = CreateThread(NULL, 1 << 20, thr_plain, &l2, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    WaitForSingleObject(t, INFINITE);
    printf("G fresh thread after amx thread L=%d\n", l2);
  } else {
    printf("G no AMX\n");
  }
}

// ---- H: child process: fault with rsp just above the committed stack limit
static __declspec(noinline) void commit_stack(void) {
  volatile char* p = (volatile char*)_alloca(48 * 1024);  // __chkstk touches the pages in order
  p[0] = 1;
}
static DWORD WINAPI thr_guard(LPVOID arg) {
  int d = *(int*)arg;
  commit_stack();
  NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
  uint64_t limit = (uint64_t)tib->StackLimit;
  printf("H d=%d StackLimit=0x%llx base=0x%llx\n", d, (unsigned long long)limit,
         (unsigned long long)tib->StackBase);
  fflush(stdout);
  // place rsp d bytes above the lowest committed address; nothing below is touched before the fault
  uint64_t base = (limit + d + 64) & ~63ull;
  uint64_t cur = (uint64_t)_AddressOfReturnAddress();
  if (base + 4096 >= cur) {
    printf("H d=%d skipped: committed region too small (rsp 0x%llx)\n", d, (unsigned long long)cur);
    return 0;
  }
  int r = run1(0, 0, 0, base);
  NT_TIB* tib2 = (NT_TIB*)NtCurrentTeb();
  printf("H d=%d survived lost=0x%x newlimit=0x%llx\n", d, r, (unsigned long long)tib2->StackLimit);
  fflush(stdout);
  return 0;
}

static void cpu_info(void) {
  int r[4];
  char brand[49] = { 0 };
  __cpuid(r, 0x80000000);
  if ((unsigned)r[0] >= 0x80000004) {
    __cpuid((int*)brand, 0x80000002); __cpuid((int*)(brand + 16), 0x80000003); __cpuid((int*)(brand + 32), 0x80000004);
  }
  __cpuidex(r, 7, 0);
  int avx512f = (r[1] >> 16) & 1, amx_tile = (r[3] >> 24) & 1;
  g_xcr0 = _xgetbv(0);
  g_avx512 = avx512f && ((g_xcr0 & 0xE6) == 0xE6);
  g_amx = amx_tile && ((g_xcr0 & 0x60000) == 0x60000);
  int d0[4], d1[4];
  __cpuidex(d0, 0xD, 0); __cpuidex(d1, 0xD, 1);
  DWORD64 enabled = GetEnabledXStateFeatures();
  OSVERSIONINFOEXW v; memset(&v, 0, sizeof v); v.dwOSVersionInfoSize = sizeof v;
  typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOEXW*);
  RtlGetVersion_t rgv = (RtlGetVersion_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
  if (rgv) rgv(&v);
  printf("cpu: %s\nos: %lu.%lu.%lu\nxcr0: 0x%llx enabled_xstate: 0x%llx avx512=%d amx=%d xfd=%d\n", brand,
         v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, (unsigned long long)g_xcr0,
         (unsigned long long)enabled, g_avx512, g_amx, (d1[0] >> 4) & 1);
  printf("cpuid 0xD: xcr0 size %d, max %d, compacted xcr0|xss %d\n", d0[1], d0[2], d1[1]);
}

static DWORD WINAPI worker(LPVOID arg) {
  expA();
  if (g_L <= 0) { printf("no L, stopping\n"); return 0; }
  expB();
  expC();
  expD();
  expE();
  expF();
  expG();
  return 0;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  g_fill = (uint64_t*)malloc(FILLQ * 8);
  cpu_info();
  AddVectoredExceptionHandler(1, veh);
  if (argc > 2 && strcmp(argv[1], "guard") == 0) {
    int d = atoi(argv[2]);
    HANDLE t = CreateThread(NULL, 1 << 20, thr_guard, &d, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    WaitForSingleObject(t, INFINITE);
    return 0;
  }
  HANDLE t = CreateThread(NULL, 4 << 20, worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  WaitForSingleObject(t, INFINITE);
  return 0;
}
