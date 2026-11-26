/*
 * Copyright (c) 2026, Alessandro Autiero. All rights reserved.
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

/* Native code that returns to Java with floating point exceptions unmasked (TestMXCSRRestore). */

#include "jni.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#else
/* Not x64: the test does not run; these keep the library buildable. */
static unsigned int _mm_getcsr(void) { return 0; }
static void _mm_setcsr(unsigned int value) { (void)value; }
#endif

/* Every exception mask bit (invalid, denormal, divide by zero, overflow, underflow, precision). */
#define MXCSR_MASKS 0x1F80u

JNIEXPORT unsigned int mxcsr_current(void) {
  return _mm_getcsr();
}

/* Unmasks the divide-by-zero and invalid exceptions and leaves them so. */
JNIEXPORT void mxcsr_unmask(void) {
  _mm_setcsr(_mm_getcsr() & ~0x0280u);
}

/*
 * Runs callback with the given control bits cleared and set, then returns the MXCSR it finds after
 * the callback returns.
 */
JNIEXPORT unsigned int mxcsr_call_with(void (*callback)(void), unsigned int clear, unsigned int set) {
  unsigned int saved = _mm_getcsr();
  _mm_setcsr((saved & ~clear) | set);
  callback();
  unsigned int after = _mm_getcsr();
  _mm_setcsr(saved);
  return after;
}

JNIEXPORT jint JNICALL Java_runtime_jni_mxcsr_TestMXCSRRestore_jniCurrent(JNIEnv* env, jclass cls) {
  return (jint)mxcsr_current();
}

JNIEXPORT void JNICALL Java_runtime_jni_mxcsr_TestMXCSRRestore_jniUnmask(JNIEnv* env, jclass cls) {
  mxcsr_unmask();
}
