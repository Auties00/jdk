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

/*
 * A JVMTI agent whose SampledObjectAlloc callback returns to the VM with another rounding mode
 * (TestMXCSRAfterJVMTICallback).
 */

#include <string.h>

#include "jni.h"
#include "jvmti.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#else
/* Not x64: the test does not run; these keep the library buildable. */
static unsigned int _mm_getcsr(void) { return 0; }
static void _mm_setcsr(unsigned int value) { (void)value; }
#endif

/* The rounding control bits, both set: rounding toward zero. */
#define MXCSR_ROUND_TO_ZERO 0x6000u

static jvmtiEnv* jvmti;
static volatile jint callbacks;

static void JNICALL sampled_object_alloc(jvmtiEnv* env, JNIEnv* jni, jthread thread, jobject object,
                                         jclass object_klass, jlong size) {
  callbacks++;
  _mm_setcsr(_mm_getcsr() | MXCSR_ROUND_TO_ZERO);
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* reserved) {
  jvmtiCapabilities capabilities;
  jvmtiEventCallbacks event_callbacks;

  if ((*vm)->GetEnv(vm, (void**)&jvmti, JVMTI_VERSION_11) != JNI_OK) {
    return JNI_ERR;
  }
  memset(&capabilities, 0, sizeof(capabilities));
  capabilities.can_generate_sampled_object_alloc_events = 1;
  if ((*jvmti)->AddCapabilities(jvmti, &capabilities) != JVMTI_ERROR_NONE) {
    return JNI_ERR;
  }
  memset(&event_callbacks, 0, sizeof(event_callbacks));
  event_callbacks.SampledObjectAlloc = &sampled_object_alloc;
  if ((*jvmti)->SetEventCallbacks(jvmti, &event_callbacks, sizeof(event_callbacks)) != JVMTI_ERROR_NONE) {
    return JNI_ERR;
  }
  return JNI_OK;
}

/* Samples every allocation of the given thread, or stops; returns whether JVMTI accepted it. */
JNIEXPORT jboolean JNICALL
Java_runtime_jni_mxcsr_TestMXCSRAfterJVMTICallback_sample(JNIEnv* env, jclass cls, jthread thread, jboolean on) {
  if (on && (*jvmti)->SetHeapSamplingInterval(jvmti, 0) != JVMTI_ERROR_NONE) {
    return JNI_FALSE;
  }
  return (*jvmti)->SetEventNotificationMode(jvmti, on ? JVMTI_ENABLE : JVMTI_DISABLE,
                                            JVMTI_EVENT_SAMPLED_OBJECT_ALLOC, thread) == JVMTI_ERROR_NONE
      ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_runtime_jni_mxcsr_TestMXCSRAfterJVMTICallback_callbacks(JNIEnv* env, jclass cls) {
  return callbacks;
}
