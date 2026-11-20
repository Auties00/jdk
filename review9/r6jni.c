// Review-only (review6): JNI natives that fill 32 KiB of their own frame (below the calling
// compiled frame's stack pointer) with 0x8000000000000000, optionally throwing afterwards.
#include <jni.h>
#ifdef _WIN32
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
static NOINLINE void fill(void) {
  volatile long long buf[4096];
  for (int i = 0; i < 4096; i++) buf[i] = (long long)0x8000000000000000ULL;
}
JNIEXPORT void JNICALL Java_R6_fillA(JNIEnv* e, jclass c) { fill(); }
JNIEXPORT void JNICALL Java_R6_fillB(JNIEnv* e, jclass c) { fill(); }
JNIEXPORT void JNICALL Java_R6_fillThrow(JNIEnv* e, jclass c, jboolean t) {
  fill();
  if (t) {
    jclass ex = (*e)->FindClass(e, "java/lang/IllegalStateException");
    fill();
    (*e)->ThrowNew(e, ex, "r6");
    fill();
  }
}
