// Phase 3 (JDK-8365196). Queries, and optionally sets, a Windows feature configuration through ntdll:
//   setfeat query <id>            prints the boot and runtime configuration of the feature
//   setfeat set <id> <1|2>        1 = disabled, 2 = enabled, in the boot and the runtime store (reboot for kernel features)
//   setfeat reset <id>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  ULONG FeatureId;
  ULONG Priority;       // 8 = user
  ULONG EnabledState;   // 0 default, 1 disabled, 2 enabled
  ULONG EnabledStateOptions;
  UCHAR Variant;
  UCHAR Reserved[3];
  ULONG VariantPayloadKind;
  ULONG VariantPayload;
  ULONG Operation;      // 1 feature state, 2 variant state, 4 reset state
} Update;

typedef struct {
  ULONG FeatureId;
  ULONG Bits;           // Priority:4 EnabledState:2 IsWexpConfiguration:1 HasSubscriptions:1 Variant:6 VariantPayloadKind:2
  ULONG VariantPayload;
} Config;

typedef LONG (NTAPI* SetFn)(ULONGLONG*, ULONG, Update*, SIZE_T);
typedef LONG (NTAPI* QueryFn)(ULONG, ULONG, ULONGLONG*, Config*);

int main(int argc, char** argv) {
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  SetFn set = (SetFn)GetProcAddress(nt, "RtlSetFeatureConfigurations");
  QueryFn query = (QueryFn)GetProcAddress(nt, "RtlQueryFeatureConfiguration");
  ULONG id, type;
  if (argc < 3 || set == NULL || query == NULL) {
    printf("SETFEAT unavailable (set %p query %p)\n", (void*)set, (void*)query);
    return 2;
  }
  id = strtoul(argv[2], NULL, 10);
  if (strcmp(argv[1], "query") != 0) {
    for (type = 0; type < 2; type++) {
      Update u;
      LONG st;
      memset(&u, 0, sizeof(u));
      u.FeatureId = id;
      u.Priority = 8;
      if (strcmp(argv[1], "set") == 0 && argc > 3) {
        u.EnabledState = strtoul(argv[3], NULL, 10);
        u.Operation = 1;
      } else {
        u.Operation = 4;
      }
      st = set(NULL, type, &u, 1);
      printf("SETFEAT %s feature %lu store %lu: status 0x%08lx\n", argv[1], id, type, (ULONG)st);
    }
  }
  for (type = 0; type < 2; type++) {
    Config c;
    ULONGLONG stamp = 0;
    LONG st;
    memset(&c, 0, sizeof(c));
    st = query(id, type, &stamp, &c);
    printf("SETFEAT query feature %lu store %lu: status 0x%08lx priority %lu state %lu variant %lu stamp %llu\n", id, type,
           (ULONG)st, c.Bits & 15, (c.Bits >> 4) & 3, (c.Bits >> 8) & 63, stamp);
  }
  return 0;
}
