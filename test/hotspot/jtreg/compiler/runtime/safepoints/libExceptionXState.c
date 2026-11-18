/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * JDK-8365196. Some Windows builds copy the context of an exception to the stack with a routine
 * that trusts the XSAVE header already at the destination. When the stale stack value there
 * passes for a compacted header (0x8000000000000000 names no component), the context leaves the
 * extended state out, and resuming from it resets the upper halves of the vector registers.
 *
 * probe() takes such an exception itself, independently of the JVM, and reports whether the
 * context lost the AVX component, the length L of its extended state, and whether clearing the
 * most significant byte of the stale XCOMP_BV at ((sp - L) & -64) + 15 avoids the loss.
 * fillBelowStack() leaves the header-like value in the 32 KiB below the stack pointer of its
 * Java caller.
 */

#include "jni.h"
#include <stdint.h>

#define FILL_QWORDS 4096 /* 32 KiB */

static volatile uint64_t sink;

#if defined(_WIN64) && defined(_M_AMD64)
#include <windows.h>
#include <malloc.h>

#define STALE_HEADER 0x8000000000000000ULL

/* The layout descriptor (CONTEXT_EX) that follows a CONTEXT with CONTEXT_XSTATE. */
typedef struct {
  LONG  Offset;
  DWORD Length;
} ContextChunk;

typedef struct {
  ContextChunk All;
  ContextChunk Legacy;
  ContextChunk XState;
} ContextLayout;

typedef struct {
  int       faulted;
  int       has_xstate;
  uintptr_t sp;
  uintptr_t area;
  DWORD     length;
  uint64_t  xcomp_bv;
} Probe;

static Probe last;

static __declspec(noinline) void fill_below_with(uint64_t value) {
  volatile uint64_t buf[FILL_QWORDS];
  int i;
  for (i = 0; i < FILL_QWORDS; i++) {
    buf[i] = value;
  }
  sink = buf[FILL_QWORDS - 1];
}

static void fill_below(void) {
  fill_below_with(STALE_HEADER);
}

static void clear_below(void) {
  fill_below_with(0);
}

static int record(EXCEPTION_POINTERS* ep) {
  CONTEXT* context = ep->ContextRecord;
  last.faulted = 1;
  last.sp = (uintptr_t)context->Rsp;
  last.has_xstate = (context->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE;
  if (last.has_xstate) {
    ContextLayout* layout = (ContextLayout*)(context + 1);
    uint8_t* area = (uint8_t*)layout + layout->XState.Offset;
    last.area = (uintptr_t)area;
    last.length = layout->XState.Length;
    last.xcomp_bv = *(uint64_t*)(area + 8);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

/*
 * Fills the stack below this frame, clears one byte at scrub (if not 0), and takes an access
 * violation from this frame, whose stack pointer is the same at every call from probe().
 */
static __declspec(noinline) void fault(volatile uint8_t* page, uintptr_t scrub) {
  fill_below();
  if (scrub != 0) {
    *(volatile uint8_t*)scrub = 0;
  }
  ZeroMemory(&last, sizeof(last));
  __try {
    sink = *page;
  } __except (record(GetExceptionInformation())) {
  }
}

/* Whether the last context lacks the AVX component of its layout; -1 if it cannot tell. */
static int lost_avx(void) {
  if (!last.faulted || !last.has_xstate) {
    return -1;
  }
  /* A compacted layout (XCOMP_BV bit 63) without the AVX component. */
  return (last.xcomp_bv >> 63) != 0 && (last.xcomp_bv & XSTATE_MASK_AVX) == 0 ? 1 : 0;
}

/* Calls fault() with its stack pointer moved down by 16 * k bytes, k = 0..3. */
static __declspec(noinline) void fault_at(volatile uint8_t* page, uintptr_t scrub, int k) {
  volatile uint8_t* pad = (volatile uint8_t*)_alloca(16 * (k + 1));
  pad[0] = 0;
  fault(page, scrub);
  sink = pad[0];
}

/* {status, L, scrubbed}: status 1 if the AVX state is lost, 0 if kept, -1 if unknown;
 * L the length of the extended state: the XSTATE part of the context is at (sp - L) & -64 for
 * the stack pointer sp of each trap, which takes all four values modulo 64 (the XState length
 * that CONTEXT_EX reports includes the alignment of the trap and is not L); 0 if no single L
 * fits; scrubbed 1 if clearing the stale header at that place keeps the AVX state for each
 * value of sp modulo 64, 0 if not, -1 if not tried. */
JNIEXPORT jlongArray JNICALL
Java_compiler_runtime_safepoints_ExceptionXState_probe0(JNIEnv* env, jclass cls) {
  jlong result[3] = { -1, 0, -1 };
  jlongArray array = (*env)->NewLongArray(env, 3);
  volatile uint8_t* page;
  int i, k;
  (void)cls;
  if (array == NULL) {
    return NULL;
  }
  if ((GetEnabledXStateFeatures() & XSTATE_MASK_AVX) != 0) {
    page = (volatile uint8_t*)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (page != NULL) {
      int lost = 0, kept = 0, layout = 1;
      long distance[4] = { 0, 0, 0, 0 };
      int sp_mod[4] = { 0, 0, 0, 0 };
      uintptr_t slot[4] = { 0, 0, 0, 0 };
      for (k = 0; k < 4; k++) {
        for (i = 0; i < 4; i++) {
          int r;
          fault_at(page, 0, k);
          r = lost_avx();
          if (r == 1) lost++;
          if (r == 0) kept++;
        }
        if (!last.has_xstate || last.area >= last.sp || (last.area & 63) != 0) {
          layout = 0;
        } else {
          distance[k] = (long)(last.sp - last.area);
          sp_mod[k] = (int)(last.sp & 63);
          slot[k] = last.area + 15;
        }
      }
      if (lost == 16) {
        result[0] = 1;
      } else if (kept == 16) {
        result[0] = 0;
      }
      if (layout) {
        long min_d = distance[0], max_d = distance[0], length;
        for (k = 1; k < 4; k++) {
          if (distance[k] < min_d) min_d = distance[k];
          if (distance[k] > max_d) max_d = distance[k];
        }
        for (length = min_d; length >= max_d - 63 && length > 0; length--) {
          int fits = 1;
          for (k = 0; k < 4; k++) {
            if (length + ((sp_mod[k] - length) & 63) != distance[k]) {
              fits = 0;
            }
          }
          if (fits) {
            result[1] = (jlong)length;
            break;
          }
        }
        if (result[0] == 1 && result[1] > 0) {
          int ok = 0;
          for (k = 0; k < 4; k++) {
            for (i = 0; i < 4; i++) {
              fault_at(page, slot[k], k);
              if (lost_avx() == 0) ok++;
            }
          }
          result[2] = ok == 16 ? 1 : 0;
        }
      }
      VirtualFree((LPVOID)(uintptr_t)page, 0, MEM_RELEASE);
      /* Do not leave the header-like value on this thread's stack. */
      clear_below();
    }
  }
  (*env)->SetLongArrayRegion(env, array, 0, 3, result);
  return array;
}

#else

JNIEXPORT jlongArray JNICALL
Java_compiler_runtime_safepoints_ExceptionXState_probe0(JNIEnv* env, jclass cls) {
  jlong result[3] = { -1, 0, -1 };
  jlongArray array = (*env)->NewLongArray(env, 3);
  (void)cls;
  if (array != NULL) {
    (*env)->SetLongArrayRegion(env, array, 0, 3, result);
  }
  return array;
}

#endif

/*
 * Writes 0x8000000000000000 into the 32 KiB below the stack pointer of the calling Java frame,
 * where the first exception that frame takes without the scrub would find a stale header.
 */
JNIEXPORT void JNICALL
Java_compiler_runtime_safepoints_ExceptionXState_fillBelowStack(JNIEnv* env, jclass cls) {
  volatile uint64_t buf[FILL_QWORDS];
  int i;
  (void)env;
  (void)cls;
  for (i = 0; i < FILL_QWORDS; i++) {
    buf[i] = 0x8000000000000000ULL;
  }
  sink = buf[FILL_QWORDS - 1];
}
