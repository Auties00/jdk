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
#define ALL(p) K(p##_O); K(p##_K); K(p##_KN); K(p##_KL); K(p##_M); K(p##_G); K(p##_X); K(p##_E); K(p##_F); K(p##_MD);
ALL(masked) ALL(blend)

#define NV 10
static const char* names[NV] = {"O", "K", "KN", "KL", "M", "G", "X", "E", "F", "MD"};
static kernel masked[NV] = {masked_O, masked_K, masked_KN, masked_KL, masked_M, masked_G, masked_X, masked_E, masked_F, masked_MD};
static kernel blend[NV] = {blend_O, blend_K, blend_KN, blend_KL, blend_M, blend_G, blend_X, blend_E, blend_F, blend_MD};

#define ROUNDS 41
#define CALLS 200000

static int cmp_double(const void* a, const void* b) {
  double x = *(const double*)a, y = *(const double*)b;
  return x < y ? -1 : x > y;
}

static void run(const char* what, kernel* ks, int* src, int* src2, int* dst, int n) {
  static double t[NV][ROUNDS];
  int ref[1024];
  for (int v = 0; v < NV; v++) {
    memset(dst, 0, 4 * n);
    puts(names[v]);
    ks[v](src, src2, dst);
    if (v == 0) memcpy(ref, dst, 4 * n);
    else if (memcmp(ref, dst, 4 * n) != 0) printf("%s %s: RESULT DIFFERS\n", what, names[v]);
    for (int c = 0; c < CALLS; c++) ks[v](src, src2, dst);
  }
  for (int r = 0; r < ROUNDS; r++) {
    for (int j = 0; j < NV; j++) {
      int v = (r & 1) ? NV - 1 - j : j;
      double a = now_ns();
      for (int c = 0; c < CALLS; c++) ks[v](src, src2, dst);
      t[v][r] = (now_ns() - a) / CALLS;
    }
  }
  double med[NV], min[NV];
  for (int v = 0; v < NV; v++) {
    qsort(t[v], ROUNDS, sizeof(double), cmp_double);
    med[v] = t[v][ROUNDS / 2];
    min[v] = t[v][0];
  }
  for (int v = 0; v < NV; v++) {
    printf("%-7s %-3s median %7.2f ns  min %7.2f ns  vs O %+6.1f%%  vs K %+6.1f%%  vs M %+6.1f%%\n", what, names[v],
           med[v], min[v], (med[v] / med[0] - 1) * 100, (med[v] / med[1] - 1) * 100, (med[v] / med[4] - 1) * 100);
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
    run("masked", masked, src, src2, dst, n);
    run("blend", blend, src, src2, dst, n);
  }
  return 0;
}
