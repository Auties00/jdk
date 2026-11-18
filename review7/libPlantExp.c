#include "jni.h"
#include <stdint.h>
static volatile uint64_t sink;
JNIEXPORT void JNICALL Java_PlantExp_fillBelowStack(JNIEnv* env, jclass cls) {
  volatile uint64_t buf[4096];
  int i;
  (void)env; (void)cls;
  for (i = 0; i < 4096; i++) buf[i] = 0x8000000000000000ULL;
  sink = buf[4095];
}
