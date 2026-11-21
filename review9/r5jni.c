// Review-only (review5): JNI natives whose own 32 KiB frame is filled with 0x8000000000000000,
// i.e. the stack below the calling compiled frame.
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
JNIEXPORT void JNICALL Java_R5Conn_fillA(JNIEnv* e, jclass c) { fill(); }
JNIEXPORT void JNICALL Java_R5Conn_fillB(JNIEnv* e, jclass c) { fill(); }
JNIEXPORT void JNICALL Java_R5Conn_fillC(JNIEnv* e, jclass c) { fill(); }
