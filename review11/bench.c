// JDK-8365196, review 11: times the variants of kern.S, interleaved. Prints the median and the
// minimum of the rounds, in ns per call, and the difference of each variant to K and to O.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cpuid.h>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
static double now_ns(void) {
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}
static void* alloc64(size_t n) { return _aligned_malloc(n, 64); }
#else
#include <time.h>
static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}
static void* alloc64(size_t n) { return aligned_alloc(64, (n + 63) & ~(size_t)63); }
#endif

typedef void (__attribute__((sysv_abi)) *kernel)(int* src, int* src2, int* dst);
#define K(n) void __attribute__((sysv_abi)) n(int*, int*, int*)
#define ALL(p) K(p##_O); K(p##_K); K(p##_KN); K(p##_KL); K(p##_M); K(p##_G); K(p##_X); K(p##_E); K(p##_F); K(p##_MD); K(p##_K2); K(p##_K3); K(p##_K4); K(p##_K5); K(p##_KF); K(p##_MK3); K(p##_XK3);
ALL(masked) ALL(blend)
#define FOUR(p) K(p##_O); K(p##_K3); K(p##_MK3); K(p##_XK3);
FOUR(l2a3) FOUR(l1a2) FOUR(l2a2)
static const char* names4[4] = {"O", "K3", "MK3", "XK3"};
static kernel l2a3[4] = {l2a3_O, l2a3_K3, l2a3_MK3, l2a3_XK3};
static kernel l1a2[4] = {l1a2_O, l1a2_K3, l1a2_MK3, l1a2_XK3};
static kernel l2a2[4] = {l2a2_O, l2a2_K3, l2a2_MK3, l2a2_XK3};

#define NV 17
static const char* names[NV] = {"O", "K", "KN", "KL", "M", "G", "X", "E", "F", "MD", "K2", "K3", "K4", "K5", "KF", "MK3", "XK3"};
static kernel masked[NV] = {masked_O, masked_K, masked_KN, masked_KL, masked_M, masked_G, masked_X, masked_E, masked_F, masked_MD, masked_K2, masked_K3, masked_K4, masked_K5, masked_KF, masked_MK3, masked_XK3};
static kernel blend[NV] = {blend_O, blend_K, blend_KN, blend_KL, blend_M, blend_G, blend_X, blend_E, blend_F, blend_MD, blend_K2, blend_K3, blend_K4, blend_K5, blend_KF, blend_MK3, blend_XK3};

#define ROUNDS 41
#define CALLS 200000

static int cmp_double(const void* a, const void* b) {
  double x = *(const double*)a, y = *(const double*)b;
  return x < y ? -1 : x > y;
}

static void run(const char* what, kernel* ks, const char** names, int nv, int* src, int* src2, int* dst, int n) {
  const int mi = nv > 4 ? 4 : 2;
  static double t[NV][ROUNDS];
  int ref[1024];
  for (int v = 0; v < nv; v++) {
    memset(dst, 0, 4 * n);
    ks[v](src, src2, dst);
    if (v == 0) memcpy(ref, dst, 4 * n);
    else if (memcmp(ref, dst, 4 * n) != 0) printf("%s %s: RESULT DIFFERS\n", what, names[v]);
    for (int c = 0; c < CALLS; c++) ks[v](src, src2, dst);
  }
  for (int r = 0; r < ROUNDS; r++) {
    for (int j = 0; j < nv; j++) {
      int v = (r & 1) ? nv - 1 - j : j;
      double a = now_ns();
      for (int c = 0; c < CALLS; c++) ks[v](src, src2, dst);
      t[v][r] = (now_ns() - a) / CALLS;
    }
  }
  double med[NV], min[NV];
  for (int v = 0; v < nv; v++) {
    qsort(t[v], ROUNDS, sizeof(double), cmp_double);
    med[v] = t[v][ROUNDS / 2];
    min[v] = t[v][0];
  }
  for (int v = 0; v < nv; v++) {
    printf("%-7s %-3s median %7.2f ns  min %7.2f ns  vs O %+6.1f%%  vs K %+6.1f%%  vs M %+6.1f%%\n", what, names[v],
           med[v], min[v], (med[v] / med[0] - 1) * 100, (med[v] / med[1] - 1) * 100, (med[v] / med[mi] - 1) * 100);
  }
}

int main(void) {
  unsigned a, b, c, d;
  char brand[49] = {0};
  for (unsigned i = 0; i < 3; i++) {
    __cpuid(0x80000002 + i, a, b, c, d);
    memcpy(brand + 16 * i, &a, 4); memcpy(brand + 16 * i + 4, &b, 4);
    memcpy(brand + 16 * i + 8, &c, 4); memcpy(brand + 16 * i + 12, &d, 4);
  }
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("cpu: %s\n", brand);
  __cpuid_count(7, 0, a, b, c, d);
  printf("cpuid 7.0: ebx %08x ecx %08x edx %08x\n", b, c, d);
  const unsigned need = (1u << 16) | (1u << 17) | (1u << 30) | (1u << 31); // F, DQ, BW, VL
  if ((b & need) != need) {
    printf("no AVX-512 F, DQ, BW and VL: nothing measured\n");
    return 0;
  }
  const int n = 1003;
  int* src = alloc64(4 * n);
  int* src2 = alloc64(4 * n);
  int* dst = alloc64(4 * n);
  for (int i = 0; i < n; i++) {
    src[i] = (i * 37) % 200;
    src2[i] = (i * 53) % 200;
  }
  printf("src %p src2 %p dst %p\n", (void*)src, (void*)src2, (void*)dst);
  for (int rep = 0; rep < 2; rep++) {
    run("masked", masked, names, NV, src, src2, dst, n);
    run("blend", blend, names, NV, src, src2, dst, n);
    run("l2a3", l2a3, names4, 4, src, src2, dst, n);
    run("l1a2", l1a2, names4, 4, src, src2, dst, n);
    run("l2a2", l2a2, names4, 4, src, src2, dst, n);
  }
  return 0;
}
