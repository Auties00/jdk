/*
 * xstate_block_probe: tries to make Windows build an exception CONTEXT without the AVX-512
 * XSTATE components a thread is using (the defect behind JDK-8365196), with no JVM involved.
 *
 * Each iteration of a worker thread:
 *   1. PRE puts the vector state in a given shape: low (every vector and opmask register zero, so
 *      XINUSE is about 0x3), vzu (patterns, then vzeroupper: zmm16-31 and k1-k7 stay in use),
 *      high (patterns);
 *   2. BLOCK optionally gives up the CPU: none, yield (SwitchToThread), sleep (Sleep(1)), wait (waits
 *      on an auto-reset event another thread sets every millisecond, as a JVM thread blocks at a
 *      safepoint);
 *   3. loads distinct patterns into zmm0-31 and k1-k7 and calls a thunk `test ecx, [rbx]; ret` whose
 *      load hits a PAGE_NOACCESS page; a handler (veh: vectored handler; seh: language handler of a
 *      RtlAddFunctionTable entry, the way HotSpot handles traps in its code heap) records
 *      ContextFlags and GetXStateFeaturesMask and skips the 2-byte instruction;
 *   4. stores every register and compares with the patterns.
 * Optionally (env PROBE_DUMMY=n) an extra trap with no live state is taken before step 2.
 *
 * Usage: xstate_block_probe <veh|seh> <low|vzu|high> <none|yield|sleep|wait> <seconds> <threads> [jvmthreads]
 *   threads 0: the probe runs on the calling thread (useful from the probe_entry DLL export).
 *   jvmthreads: workers are created like HotSpot Java threads (_beginthreadex with CREATE_SUSPENDED |
 *   STACK_SIZE_PARAM_IS_A_RESERVATION and a 1 MiB stack, then ResumeThread).
 * Output: trap count, histogram of the context XSTATE masks, corrupted registers per XSAVE component.
 * Exit code: 0 clean, 3 corruption or a context without Hi16_ZMM, 2 no AVX-512.
 * Build: clang -O2 -o xstate_block_probe.exe xstate_block_probe.c
 */
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cpuid.h>

static unsigned char *trap_page, *trap_thunk;
static int use_seh, pre_mode, block_mode;
static volatile LONG64 traps, mask_hist[256], bad_comp[5], bad_iters, ctx_no_hi16, ctx_no_xstate;
static volatile LONG stop;
static HANDLE vm_event;

enum { PRE_LOW, PRE_VZU, PRE_HIGH };
enum { B_NONE, B_YIELD, B_SLEEP, B_WAIT };

static void record(PCONTEXT ctx) {
    InterlockedIncrement64(&traps);
    if ((ctx->ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE) { InterlockedIncrement64(&ctx_no_xstate); return; }
    DWORD64 m = 0;
    if (!GetXStateFeaturesMask(ctx, &m)) { InterlockedIncrement64(&ctx_no_xstate); return; }
    InterlockedIncrement64(&mask_hist[m & 0xff]);
    if (!(m & (1ull << XSTATE_AVX512_ZMM))) InterlockedIncrement64(&ctx_no_hi16);
}

static LONG CALLBACK on_veh(PEXCEPTION_POINTERS info) {
    PEXCEPTION_RECORD r = info->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && (unsigned char *) r->ExceptionInformation[1] == trap_page) {
        record(info->ContextRecord);
        info->ContextRecord->Rip += 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static EXCEPTION_DISPOSITION on_seh(PEXCEPTION_RECORD r, ULONG64 f, PCONTEXT ctx, PDISPATCHER_CONTEXT dc) {
    (void) f; (void) dc;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && (unsigned char *) r->ExceptionInformation[1] == trap_page) {
        record(ctx);
        ctx->Rip += 2;
        return ExceptionContinueExecution;
    }
    return ExceptionContinueSearch;
}

static void install_function_table(unsigned char *base) {
    unsigned char *stub = base + 16;
    stub[0] = 0x48; stub[1] = 0xB8;
    uint64_t target = (uint64_t) (uintptr_t) on_seh;
    memcpy(stub + 2, &target, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    unsigned char *unwind = base + 32;
    unwind[0] = 1 | (UNW_FLAG_EHANDLER << 3); unwind[1] = 0; unwind[2] = 0; unwind[3] = 0;
    uint32_t handler_rva = 16;
    memcpy(unwind + 4, &handler_rva, 4);
    static RUNTIME_FUNCTION rf;
    rf.BeginAddress = 0; rf.EndAddress = 3; rf.UnwindData = 32;
    if (!RtlAddFunctionTable(&rf, 1, (DWORD64) (uintptr_t) base)) { fprintf(stderr, "RtlAddFunctionTable failed\n"); exit(1); }
}

__attribute__((noinline, target("avx512f,avx512bw")))
static void set_low(void) {
    __asm__ volatile("vzeroall\n\t"
        "vpxord %%zmm16, %%zmm16, %%zmm16\n\t"
        "vpxord %%zmm17, %%zmm17, %%zmm17\n\t"
        "vpxord %%zmm18, %%zmm18, %%zmm18\n\t"
        "vpxord %%zmm19, %%zmm19, %%zmm19\n\t"
        "vpxord %%zmm20, %%zmm20, %%zmm20\n\t"
        "vpxord %%zmm21, %%zmm21, %%zmm21\n\t"
        "vpxord %%zmm22, %%zmm22, %%zmm22\n\t"
        "vpxord %%zmm23, %%zmm23, %%zmm23\n\t"
        "vpxord %%zmm24, %%zmm24, %%zmm24\n\t"
        "vpxord %%zmm25, %%zmm25, %%zmm25\n\t"
        "vpxord %%zmm26, %%zmm26, %%zmm26\n\t"
        "vpxord %%zmm27, %%zmm27, %%zmm27\n\t"
        "vpxord %%zmm28, %%zmm28, %%zmm28\n\t"
        "vpxord %%zmm29, %%zmm29, %%zmm29\n\t"
        "vpxord %%zmm30, %%zmm30, %%zmm30\n\t"
        "vpxord %%zmm31, %%zmm31, %%zmm31\n\t"
        "kxorw %%k1, %%k1, %%k1\n\t"
        "kxorw %%k2, %%k2, %%k2\n\t"
        "kxorw %%k3, %%k3, %%k3\n\t"
        "kxorw %%k4, %%k4, %%k4\n\t"
        "kxorw %%k5, %%k5, %%k5\n\t"
        "kxorw %%k6, %%k6, %%k6\n\t"
        "kxorw %%k7, %%k7, %%k7\n\t"
        ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23", "xmm24", "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31", "k1", "k2", "k3", "k4", "k5", "k6", "k7");
}

__attribute__((noinline, target("avx512f,avx512bw")))
static void set_patterns(const uint64_t *in, const uint16_t *kin, int vzu) {
    register const uint16_t *kin_r __asm__("r8") = kin;
    __asm__ volatile(
        "vmovdqu64 0(%%rax), %%zmm0\n\t"
        "vmovdqu64 64(%%rax), %%zmm1\n\t"
        "vmovdqu64 128(%%rax), %%zmm2\n\t"
        "vmovdqu64 192(%%rax), %%zmm3\n\t"
        "vmovdqu64 256(%%rax), %%zmm4\n\t"
        "vmovdqu64 320(%%rax), %%zmm5\n\t"
        "vmovdqu64 384(%%rax), %%zmm6\n\t"
        "vmovdqu64 448(%%rax), %%zmm7\n\t"
        "vmovdqu64 512(%%rax), %%zmm8\n\t"
        "vmovdqu64 576(%%rax), %%zmm9\n\t"
        "vmovdqu64 640(%%rax), %%zmm10\n\t"
        "vmovdqu64 704(%%rax), %%zmm11\n\t"
        "vmovdqu64 768(%%rax), %%zmm12\n\t"
        "vmovdqu64 832(%%rax), %%zmm13\n\t"
        "vmovdqu64 896(%%rax), %%zmm14\n\t"
        "vmovdqu64 960(%%rax), %%zmm15\n\t"
        "vmovdqu64 1024(%%rax), %%zmm16\n\t"
        "vmovdqu64 1088(%%rax), %%zmm17\n\t"
        "vmovdqu64 1152(%%rax), %%zmm18\n\t"
        "vmovdqu64 1216(%%rax), %%zmm19\n\t"
        "vmovdqu64 1280(%%rax), %%zmm20\n\t"
        "vmovdqu64 1344(%%rax), %%zmm21\n\t"
        "vmovdqu64 1408(%%rax), %%zmm22\n\t"
        "vmovdqu64 1472(%%rax), %%zmm23\n\t"
        "vmovdqu64 1536(%%rax), %%zmm24\n\t"
        "vmovdqu64 1600(%%rax), %%zmm25\n\t"
        "vmovdqu64 1664(%%rax), %%zmm26\n\t"
        "vmovdqu64 1728(%%rax), %%zmm27\n\t"
        "vmovdqu64 1792(%%rax), %%zmm28\n\t"
        "vmovdqu64 1856(%%rax), %%zmm29\n\t"
        "vmovdqu64 1920(%%rax), %%zmm30\n\t"
        "vmovdqu64 1984(%%rax), %%zmm31\n\t"
        "kmovw 2(%%r8), %%k1\n\t"
        "kmovw 4(%%r8), %%k2\n\t"
        "kmovw 6(%%r8), %%k3\n\t"
        "kmovw 8(%%r8), %%k4\n\t"
        "kmovw 10(%%r8), %%k5\n\t"
        "kmovw 12(%%r8), %%k6\n\t"
        "kmovw 14(%%r8), %%k7\n\t"
        "test %%ecx, %%ecx\n\t"
        "jz 1f\n\t"
        "vzeroupper\n\t"
        "1:\n\t"
        : : "a"(in), "r"(kin_r), "c"(vzu) : "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23", "xmm24", "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31", "k1", "k2", "k3", "k4", "k5", "k6", "k7");
}

__attribute__((noinline, target("avx512f,avx512bw")))
static void dummy_trap(void) {
    register unsigned char *thunk __asm__("r10") = trap_thunk;
    __asm__ volatile("sub $40, %%rsp\n\t" "call *%%r10\n\t" "add $40, %%rsp\n\t"
                     : : "b"(trap_page), "c"(0), "r"(thunk) : "memory", "cc", "rax", "rdx", "r8", "r9", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23", "xmm24", "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31", "k1", "k2", "k3", "k4", "k5", "k6", "k7");
}

/* Loads the patterns, traps through the thunk and stores every register. */
__attribute__((noinline, target("avx512f,avx512bw")))
static void round_trip(const uint64_t *in, const uint16_t *kin, uint64_t *out, uint16_t *kout) {
    register const uint16_t *kin_r __asm__("r8") = kin;
    register uint16_t *kout_r __asm__("r9") = kout;
    register unsigned char *thunk __asm__("r10") = trap_thunk;
    __asm__ volatile(
        "vmovdqu64 0(%%rax), %%zmm0\n\t"
        "vmovdqu64 64(%%rax), %%zmm1\n\t"
        "vmovdqu64 128(%%rax), %%zmm2\n\t"
        "vmovdqu64 192(%%rax), %%zmm3\n\t"
        "vmovdqu64 256(%%rax), %%zmm4\n\t"
        "vmovdqu64 320(%%rax), %%zmm5\n\t"
        "vmovdqu64 384(%%rax), %%zmm6\n\t"
        "vmovdqu64 448(%%rax), %%zmm7\n\t"
        "vmovdqu64 512(%%rax), %%zmm8\n\t"
        "vmovdqu64 576(%%rax), %%zmm9\n\t"
        "vmovdqu64 640(%%rax), %%zmm10\n\t"
        "vmovdqu64 704(%%rax), %%zmm11\n\t"
        "vmovdqu64 768(%%rax), %%zmm12\n\t"
        "vmovdqu64 832(%%rax), %%zmm13\n\t"
        "vmovdqu64 896(%%rax), %%zmm14\n\t"
        "vmovdqu64 960(%%rax), %%zmm15\n\t"
        "vmovdqu64 1024(%%rax), %%zmm16\n\t"
        "vmovdqu64 1088(%%rax), %%zmm17\n\t"
        "vmovdqu64 1152(%%rax), %%zmm18\n\t"
        "vmovdqu64 1216(%%rax), %%zmm19\n\t"
        "vmovdqu64 1280(%%rax), %%zmm20\n\t"
        "vmovdqu64 1344(%%rax), %%zmm21\n\t"
        "vmovdqu64 1408(%%rax), %%zmm22\n\t"
        "vmovdqu64 1472(%%rax), %%zmm23\n\t"
        "vmovdqu64 1536(%%rax), %%zmm24\n\t"
        "vmovdqu64 1600(%%rax), %%zmm25\n\t"
        "vmovdqu64 1664(%%rax), %%zmm26\n\t"
        "vmovdqu64 1728(%%rax), %%zmm27\n\t"
        "vmovdqu64 1792(%%rax), %%zmm28\n\t"
        "vmovdqu64 1856(%%rax), %%zmm29\n\t"
        "vmovdqu64 1920(%%rax), %%zmm30\n\t"
        "vmovdqu64 1984(%%rax), %%zmm31\n\t"
        "kmovw 2(%%r8), %%k1\n\t"
        "kmovw 4(%%r8), %%k2\n\t"
        "kmovw 6(%%r8), %%k3\n\t"
        "kmovw 8(%%r8), %%k4\n\t"
        "kmovw 10(%%r8), %%k5\n\t"
        "kmovw 12(%%r8), %%k6\n\t"
        "kmovw 14(%%r8), %%k7\n\t"
        "sub $40, %%rsp\n\t"
        "call *%%r10\n\t"
        "add $40, %%rsp\n\t"
        "vmovdqu64 %%zmm0, 0(%%rdx)\n\t"
        "vmovdqu64 %%zmm1, 64(%%rdx)\n\t"
        "vmovdqu64 %%zmm2, 128(%%rdx)\n\t"
        "vmovdqu64 %%zmm3, 192(%%rdx)\n\t"
        "vmovdqu64 %%zmm4, 256(%%rdx)\n\t"
        "vmovdqu64 %%zmm5, 320(%%rdx)\n\t"
        "vmovdqu64 %%zmm6, 384(%%rdx)\n\t"
        "vmovdqu64 %%zmm7, 448(%%rdx)\n\t"
        "vmovdqu64 %%zmm8, 512(%%rdx)\n\t"
        "vmovdqu64 %%zmm9, 576(%%rdx)\n\t"
        "vmovdqu64 %%zmm10, 640(%%rdx)\n\t"
        "vmovdqu64 %%zmm11, 704(%%rdx)\n\t"
        "vmovdqu64 %%zmm12, 768(%%rdx)\n\t"
        "vmovdqu64 %%zmm13, 832(%%rdx)\n\t"
        "vmovdqu64 %%zmm14, 896(%%rdx)\n\t"
        "vmovdqu64 %%zmm15, 960(%%rdx)\n\t"
        "vmovdqu64 %%zmm16, 1024(%%rdx)\n\t"
        "vmovdqu64 %%zmm17, 1088(%%rdx)\n\t"
        "vmovdqu64 %%zmm18, 1152(%%rdx)\n\t"
        "vmovdqu64 %%zmm19, 1216(%%rdx)\n\t"
        "vmovdqu64 %%zmm20, 1280(%%rdx)\n\t"
        "vmovdqu64 %%zmm21, 1344(%%rdx)\n\t"
        "vmovdqu64 %%zmm22, 1408(%%rdx)\n\t"
        "vmovdqu64 %%zmm23, 1472(%%rdx)\n\t"
        "vmovdqu64 %%zmm24, 1536(%%rdx)\n\t"
        "vmovdqu64 %%zmm25, 1600(%%rdx)\n\t"
        "vmovdqu64 %%zmm26, 1664(%%rdx)\n\t"
        "vmovdqu64 %%zmm27, 1728(%%rdx)\n\t"
        "vmovdqu64 %%zmm28, 1792(%%rdx)\n\t"
        "vmovdqu64 %%zmm29, 1856(%%rdx)\n\t"
        "vmovdqu64 %%zmm30, 1920(%%rdx)\n\t"
        "vmovdqu64 %%zmm31, 1984(%%rdx)\n\t"
        "kmovw %%k1, 2(%%r9)\n\t"
        "kmovw %%k2, 4(%%r9)\n\t"
        "kmovw %%k3, 6(%%r9)\n\t"
        "kmovw %%k4, 8(%%r9)\n\t"
        "kmovw %%k5, 10(%%r9)\n\t"
        "kmovw %%k6, 12(%%r9)\n\t"
        "kmovw %%k7, 14(%%r9)\n\t"
        : : "a"(in), "d"(out), "b"(trap_page), "c"(0), "r"(kin_r), "r"(kout_r), "r"(thunk)
        : "memory", "cc", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21", "xmm22", "xmm23", "xmm24", "xmm25", "xmm26", "xmm27", "xmm28", "xmm29", "xmm30", "xmm31", "k1", "k2", "k3", "k4", "k5", "k6", "k7");
}

static unsigned __stdcall vm_main(void *arg) {
    (void) arg;
    while (!stop) { SetEvent(vm_event); Sleep(1); }
    SetEvent(vm_event);
    return 0;
}

static long long seconds;
static int dummy_traps;

static unsigned __stdcall worker(void *arg) {
    uint64_t seed = 0x9E3779B97F4A7C15ull * ((uintptr_t) arg + 1);
    uint64_t in[32 * 8], out[32 * 8];
    uint16_t kin[8], kout[8];
    ULONGLONG end = GetTickCount64() + (ULONGLONG) seconds * 1000;
    while (GetTickCount64() < end) {
        for (int i = 0; i < 32 * 8; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; in[i] = seed | 1; }
        for (int k = 0; k < 8; k++) kin[k] = (uint16_t) ((seed >> (k * 7)) | 0x0101);
        memset(out, 0xEE, sizeof out); memset(kout, 0xEE, sizeof kout);
        if (pre_mode == PRE_LOW) set_low();
        else set_patterns(in, kin, pre_mode == PRE_VZU);
        for (int d = 0; d < dummy_traps; d++) dummy_trap();
        switch (block_mode) {
            case B_YIELD: SwitchToThread(); break;
            case B_SLEEP: Sleep(1); break;
            case B_WAIT: WaitForSingleObject(vm_event, 50); break;
            default: break;
        }
        round_trip(in, kin, out, kout);
        int bad = 0;
        for (int r = 0; r < 32; r++) for (int q = 0; q < 8; q++) if (out[r * 8 + q] != in[r * 8 + q]) {
            int c = r >= 16 ? 3 : q < 2 ? 0 : q < 4 ? 1 : 2;
            InterlockedIncrement64(&bad_comp[c]); bad = 1;
        }
        for (int k = 1; k < 8; k++) if (kout[k] != kin[k]) { InterlockedIncrement64(&bad_comp[4]); bad = 1; }
        if (bad) InterlockedIncrement64(&bad_iters);
    }
    return 0;
}

static int avx512_usable(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27))) return 0;
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 0xE6) != 0xE6) return 0;
    __cpuid_count(7, 0, a, b, c, d);
    return (b & (1u << 16)) && (b & (1u << 30));
}

static int run_probe(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <veh|seh> <low|vzu|high> <none|yield|sleep|wait> <seconds> <threads> [jvmthreads]\n", argv[0]);
        return 1;
    }
    if (!avx512_usable()) { printf("RESULT no AVX-512\n"); return 2; }
    use_seh = !strcmp(argv[1], "seh");
    pre_mode = !strcmp(argv[2], "low") ? PRE_LOW : !strcmp(argv[2], "vzu") ? PRE_VZU : PRE_HIGH;
    block_mode = !strcmp(argv[3], "yield") ? B_YIELD : !strcmp(argv[3], "sleep") ? B_SLEEP
               : !strcmp(argv[3], "wait") ? B_WAIT : B_NONE;
    seconds = atoll(argv[4]);
    int threads = atoi(argv[5]);
    int jvmthreads = argc > 6 && !strcmp(argv[6], "jvmthreads");
    dummy_traps = getenv("PROBE_DUMMY") ? atoi(getenv("PROBE_DUMMY")) : 0;
    trap_page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(trap_page, 4096, PAGE_NOACCESS, &old);
    trap_thunk = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    trap_thunk[0] = 0x85; trap_thunk[1] = 0x0B; trap_thunk[2] = 0xC3;
    if (use_seh) install_function_table(trap_thunk); else AddVectoredExceptionHandler(1, on_veh);
    vm_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    HANDLE vm = (HANDLE) _beginthreadex(NULL, 0, vm_main, NULL, 0, NULL);
    HANDLE hs[64];
    if (threads > 64) threads = 64;
    if (threads == 0) worker(NULL); /* on the calling thread */
    for (int t = 0; t < threads; t++) {
        if (jvmthreads) {
            hs[t] = (HANDLE) _beginthreadex(NULL, 1 << 20, worker, (void *) (uintptr_t) t,
                                            CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
            ResumeThread(hs[t]);
        } else {
            hs[t] = (HANDLE) _beginthreadex(NULL, 0, worker, (void *) (uintptr_t) t, 0, NULL);
        }
    }
    if (threads > 0) WaitForMultipleObjects(threads, hs, TRUE, INFINITE);
    stop = 1;
    WaitForSingleObject(vm, INFINITE);
    printf("RESULT handler=%s pre=%s block=%s threads=%d%s dummy=%d traps=%lld ctx_no_xstate=%lld ctx_no_hi16=%lld bad_iterations=%lld\n",
           argv[1], argv[2], argv[3], threads, jvmthreads ? " jvmthreads" : "", dummy_traps, traps, ctx_no_xstate,
           ctx_no_hi16, bad_iters);
    printf("  masks:");
    for (int m = 0; m < 256; m++) if (mask_hist[m]) printf(" 0x%x=%lld", m, mask_hist[m]);
    printf("\n  corrupted: sse=%lld avx=%lld zmm_hi256=%lld hi16_zmm=%lld opmask=%lld\n",
           bad_comp[0], bad_comp[1], bad_comp[2], bad_comp[3], bad_comp[4]);
    fflush(stdout);
    return (bad_iters || ctx_no_hi16) ? 3 : 0;
}

/* Entry point when loaded as a DLL (for example into a JVM through FFM): the arguments of main in one
 * space-separated string, the first one being the handler. */
__declspec(dllexport) int probe_entry(const char *args) {
    static char buf[512];
    char *argv[16];
    int argc = 0;
    strncpy(buf, args, sizeof buf - 1);
    argv[argc++] = "probe";
    for (char *t = strtok(buf, " "); t != NULL && argc < 15; t = strtok(NULL, " ")) argv[argc++] = t;
    argv[argc] = NULL;
    return run_probe(argc, argv);
}

int main(int argc, char **argv) {
    return run_probe(argc, argv);
}
