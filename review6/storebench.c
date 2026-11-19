// JDK-8365196 scrub6: cost of the scrub stores inside a pointer-chasing loop with a live vector
// accumulator (the shape of R5Bench.listWalkVector), by store form and by the depth L.
//
//   none   no scrub
//   byte4  4 x mov byte [rsp + d], 0   (the scrub5 form: scratch-free, flags kept)
//   qword4 4 x mov qword [rsp + d], 0  (imm32 sign-extended; scratch-free, flags kept)
//   dword4 4 x mov dword [rsp + d], 0  (the high half of XCOMP_BV)
//   reg1   lea r11, [rsp - L]; and r11, -64; mov byte [r11 + 15], 0  (one store, needs r11, kills flags)
//
// Build: clang -O2 -mavx2 storebench.c -o storebench
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>
#ifdef _WIN32
#include <windows.h>
static double now_ns(void) {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}
#endif

typedef struct Node { struct Node* next; double v; } Node;

#define STEPS 1024
#define OFF(L, s) (-((L) + ((((s) - (L)) % 64 + 64) % 64)) + 15)

#define BYTE4(L) __asm__ volatile("movb $0, %c0(%%rsp)\n\tmovb $0, %c1(%%rsp)\n\tmovb $0, %c2(%%rsp)\n\tmovb $0, %c3(%%rsp)" \
  :: "i"(OFF(L,0)), "i"(OFF(L,16)), "i"(OFF(L,32)), "i"(OFF(L,48)) : "memory")
#define QWORD4(L) __asm__ volatile("movq $0, %c0(%%rsp)\n\tmovq $0, %c1(%%rsp)\n\tmovq $0, %c2(%%rsp)\n\tmovq $0, %c3(%%rsp)" \
  :: "i"(OFF(L,0)-7), "i"(OFF(L,16)-7), "i"(OFF(L,32)-7), "i"(OFF(L,48)-7) : "memory")
#define DWORD4(L) __asm__ volatile("movl $0, %c0(%%rsp)\n\tmovl $0, %c1(%%rsp)\n\tmovl $0, %c2(%%rsp)\n\tmovl $0, %c3(%%rsp)" \
  :: "i"(OFF(L,0)-3), "i"(OFF(L,16)-3), "i"(OFF(L,32)-3), "i"(OFF(L,48)-3) : "memory")
#define REG1(L) __asm__ volatile("lea %c0(%%rsp), %%r11\n\tand $-64, %%r11\n\tmovb $0, 15(%%r11)" \
  :: "i"(-(L)) : "r11", "cc", "memory")
#define NONE(L) do {} while (0)

#define KERNEL(NAME, FORM, L) \
  __attribute__((noinline)) static double NAME(Node* head, int reps) { \
    __m256d acc = _mm256_setzero_pd(); \
    for (int r = 0; r < reps; r++) { \
      Node* p = head; \
      for (int i = 0; i < STEPS; i++) { \
        FORM(L); \
        acc = _mm256_add_pd(acc, _mm256_set1_pd(p->v)); \
        p = p->next; \
      } \
    } \
    double out[4]; _mm256_storeu_pd(out, acc); return out[0] + out[1] + out[2] + out[3]; \
  }

KERNEL(k_none, NONE, 320)
KERNEL(k_byte4_320, BYTE4, 320)
KERNEL(k_qword4_320, QWORD4, 320)
KERNEL(k_dword4_320, DWORD4, 320)
KERNEL(k_reg1_320, REG1, 320)
KERNEL(k_byte4_1920, BYTE4, 1920)
KERNEL(k_qword4_1920, QWORD4, 1920)
KERNEL(k_reg1_1920, REG1, 1920)
KERNEL(k_byte4_10176, BYTE4, 10176)
KERNEL(k_qword4_10176, QWORD4, 10176)

// The same with one 8-byte load from the frame per step, as a spill reload would do.
#define KERNEL_SPILL(NAME, FORM, L) \
  __attribute__((noinline)) static double NAME(Node* head, int reps) { \
    __m256d acc = _mm256_setzero_pd(); \
    volatile double slot = 1.0; \
    for (int r = 0; r < reps; r++) { \
      Node* p = head; \
      for (int i = 0; i < STEPS; i++) { \
        FORM(L); \
        acc = _mm256_add_pd(acc, _mm256_set1_pd(p->v + slot)); \
        p = p->next; \
      } \
    } \
    double out[4]; _mm256_storeu_pd(out, acc); return out[0] + out[1] + out[2] + out[3]; \
  }
KERNEL_SPILL(s_none, NONE, 320)
KERNEL_SPILL(s_byte4_320, BYTE4, 320)
KERNEL_SPILL(s_qword4_320, QWORD4, 320)
KERNEL_SPILL(s_reg1_320, REG1, 320)

typedef double (*kernel_t)(Node*, int);
static struct { const char* name; kernel_t k; } kernels[] = {
  {"none", k_none}, {"byte4 L=320", k_byte4_320}, {"qword4 L=320", k_qword4_320}, {"dword4 L=320", k_dword4_320},
  {"reg1 L=320", k_reg1_320}, {"byte4 L=1920", k_byte4_1920}, {"qword4 L=1920", k_qword4_1920},
  {"reg1 L=1920", k_reg1_1920}, {"byte4 L=10176", k_byte4_10176}, {"qword4 L=10176", k_qword4_10176},
  {"spill none", s_none}, {"spill byte4 L=320", s_byte4_320}, {"spill qword4 L=320", s_qword4_320},
  {"spill reg1 L=320", s_reg1_320},
};

static volatile double sink;

__attribute__((noinline)) static void touch_stack(void) {
  volatile char buf[32768];
  for (int i = 0; i < (int)sizeof(buf); i += 64) buf[i] = 0;
  sink = buf[100];
}

int main(void) {
  touch_stack();
  int n = STEPS;
  Node* nodes = (Node*)calloc(n, sizeof(Node));
  int* perm = (int*)malloc(n * sizeof(int));
  for (int i = 0; i < n; i++) perm[i] = i;
  srand(42);
  for (int i = n - 1; i > 0; i--) { int j = rand() % (i + 1); int t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
  for (int i = 0; i < n; i++) { nodes[perm[i]].next = &nodes[perm[(i + 1) % n]]; nodes[perm[i]].v = i; }
  int reps = 2000;
  int nk = (int)(sizeof(kernels) / sizeof(kernels[0]));
  double best[64];
  for (int k = 0; k < nk; k++) best[k] = 1e30;
  for (int round = 0; round < 7; round++) {
    for (int k = 0; k < nk; k++) {
      double t0 = now_ns();
      sink = kernels[k].k(&nodes[perm[0]], reps);
      double t = (now_ns() - t0) / ((double)reps * STEPS);
      if (t < best[k]) best[k] = t;
    }
  }
  for (int k = 0; k < nk; k++) {
    printf("%-20s %7.3f ns/step  %+6.1f%%\n", kernels[k].name, best[k],
           100.0 * (best[k] / best[k < 10 ? 0 : 10] - 1));
  }
  return 0;
}
