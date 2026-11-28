#include "jni.h"

JNIEXPORT jint JNICALL Java_review8_R8Bench_jniNop(JNIEnv* env, jclass cls, jint x) {
  return x + 1;
}
