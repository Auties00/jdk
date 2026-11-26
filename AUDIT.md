# JDK-8365196 notrap: audit of resumed hardware exceptions on Windows

Tree: `D:/jdk-notrap`, branch `JDK-8365196-notrap`, at `c6f997c2c66` (scrub14 on mainline `2b376fb353c`).
Every file:line below is in this tree, under `src/hotspot/` unless it says otherwise.

The question for each path: does HotSpot resume Java or compiled execution after a hardware exception (or after
restoring a saved context) while losable state can be live, and if so, does the notrap design (explicit null test,
explicit div0 test, branch-based loop poll, C1 off xmm16-31) remove it?

Losable state, as agreed: on x64 the ymm/zmm upper halves, xmm16-31, k1-k7 and APX state; on Arm64 SVE Z above the
low 128 bits, P0-P15 and FFR.

## Result

The design covers loop polls, implicit null checks, div0, stack banging, deopt, handshakes, async exceptions,
GC barriers, stubs, C1, the interpreter, JFR and HotSpot's own C++ code. One path it cannot cover cleanly, and
two it does not name:

1. **Blocker: unsafe raw accesses in C2 code, on x64 and Arm64.** A page error at a raw memory access in a C2
   nmethod that contains unsafe accesses is resumed at the next instruction, with an `InternalError` pending, in
   the same frame and with every register live (os/windows/os_windows.cpp:2985-3000,
   runtime/sharedRuntime.cpp:1856-1869). No non-trapping form exists: the fault is an I/O error on a mapped page
   (a truncated or unreachable mapped file), not something a test can predict. scrub14 counted these accesses as
   trap sites (opto/trapScrub.cpp:61-83); the notrap design has no counterpart.
2. **Not in the design: FP exceptions in compiled code (x64).** The handler resets MXCSR and resumes the faulting
   instruction (os_cpu/windows_x86/os_windows_x86.cpp:166-198, called at os/windows/os_windows.cpp:3013-3016). It
   happens in Java code after native code returns with FP exceptions unmasked, because by default neither JNI
   returns (cpu/x86/macroAssembler_x86.cpp:5573-5583, `RestoreMXCSROnJNICalls` false at
   runtime/globals.hpp:783) nor FFM downcalls (no MXCSR handling in cpu/x86/downcallLinker_x86_64.cpp) restore
   it. This one can be covered cleanly, but by a different mechanism from the design's (see Options).
3. **Not in the design: APX r30 and r31 across C calls (x64).** On Windows C2 treats them as kept by C callees
   (cpu/x86/x86.ad:176-179). A resumed exception inside a leaf callee, such as an unsafe page error in an arraycopy
   stub, zeroes them under the caller, and no explicit form in the caller helps. Coverable by policy (see Options).

As instructed, I stopped after step 1. Nothing is prototyped; the branch and worktree exist with this file only.

## How the Windows filter resumes

Every hardware exception on a Java thread reaches `topLevelExceptionFilter` (os/windows/os_windows.cpp:2854),
through the code-cache function table on x64 (os_cpu/windows_x86/os_windows_x86.cpp:82-136) and the vectored
handler where `USE_VECTORED_EXCEPTION_HANDLING` is defined, which includes Arm64 (os/windows/os_windows.cpp:4980-4982).
`Handle_Exception` only rewrites the PC of the dispatched context and returns `EXCEPTION_CONTINUE_EXECUTION`
(os/windows/os_windows.cpp:2559-2581), so every redirect below restores the dispatched context, the broken one on
an affected kernel. The sites that return `EXCEPTION_CONTINUE_EXECUTION`:

- the startup probes of `VM_Version` (cpuinfo, the scrub's XSTATE probes, APX): 2862-2887;
- the assert-poison page (debug builds, VM code): 2893-2897;
- stack overflow (yellow zone): Java code goes to the SOE continuation, other code is resumed in place: 2908-2932;
- access violation in Java code: stack overflow continuation 2938-2945, poll stub 2948-2954, stack growth resumed in
  place 2956-2961, implicit null continuation 2962-2966; in native code, the JNI fast field accessors' slow case
  2972-2977;
- page error with unsafe access: resumed at the next instruction 2985-3000;
- integer divide by zero: implicit div0 continuation 3003-3006; integer overflow: `Handle_IDiv_Exception` resumes
  after the `idiv` 3008-3009 (2652-2687);
- FP exceptions, Java or native: resumed in place after MXCSR reset 3012-3017;
- the UD at a deoptimized call site: deopt handler 3020-3037.

Nothing else in HotSpot restores a context. There is no `SetThreadContext`, `NtContinue` or `RtlRestoreContext`
anywhere in the tree (searched all of src/hotspot). The only suspension is `SuspendedThreadTask`
(os/windows/os_windows.cpp:6376-6397): `SuspendThread`, `GetThreadContext`, `ResumeThread`. The suspended thread's
state is saved by the kernel in its own save area, not through user-mode exception dispatch, and no context is
written back, so nothing is lost there.

## Path by path

### C2 loop polls

- x64: faulting, `testl rax, [poll]` with `relocInfo::poll_type` (cpu/x86/x86.ad:17104-17119). Arm64: faulting,
  `ldrw zr, [poll]` (cpu/aarch64/aarch64.ad:15438-15450).
- The fault goes to `get_poll_stub`, which picks the wide-vector blob when the nmethod has wide vectors
  (runtime/sharedRuntime.cpp:667-700); the blob then saves registers that the broken context already zeroed.
- Losable state live: yes, routinely (vectorized loops, Vector API). JFR makes it frequent: it arms the local poll
  of every thread it samples (jfr/periodic/sampling/jfrThreadSampler.cpp:53, 370), and handshakes and safepoints
  arm it too.
- Design: covered by a branch poll to a stub, at sites where losable state is live. One constraint for the
  prototype: the non-return handler blob unconditionally steps over the poll instruction at the saved PC
  (cpu/x86/sharedRuntime_x86_64.cpp:3158-3240, it adds 2 or 3 bytes), so a branch poll cannot reuse it as is. It
  needs a blob variant that does not skip (or a skip conditional on the poll encoding), with the stub storing the
  oopmap PC in `saved_exception_pc` as `C2SafepointPollStub` does for return polls.

### C2 return polls

- Branch-based on both: `safepoint_poll(..., at_return = true)` to a `C2SafepointPollStub`
  (cpu/x86/x86.ad:1972-1983, cpu/aarch64/aarch64.ad:1910-1911). Your belief is right: loop polls fault and
  return polls branch.
- No hardware exception. Design: nothing to do.

### C2 implicit null checks

- Formed before register allocation by `PhaseCFG::implicit_null_check` (opto/lcm.cpp:116-400) and recorded in the
  implicit exception table from `MachNullCheck` (opto/output.cpp:1717-1720, 2077-2086). The continuation is
  `non_connector_successor(0)` of the null-check block (opto/output.cpp:2083), code in the same frame (an
  uncommon trap or an exception path) that spills or uses whatever the registers hold after the broken restore.
  ZGC and Shenandoah late-expanded barrier loads can be null-check accesses too (opto/output.cpp:2079-2082).
- Losable state live: yes.
- Design: covered by an explicit test and branch to the same successor. Since the decision needs register
  allocation and the check is formed before it, the explicit form has to be emitted after allocation, in front of
  the access. Two facts for the prototype:
  - x64: the test kills the flags. The accepted accesses are plain loads and stores plus any instruction with an
    embedded load except flag producers (opto/lcm.cpp:197-262), so a flag consumer with a memory operand (a cmov
    from memory) can be the access. Such candidates need to be refused while the mode is on, or the test must
    preserve the flags.
  - Arm64: `cbz`/`cbzw` needs no flags.

### Implicit div0 and min_int / -1

- C2 has no implicit div0 on any platform here: `zero_check_int` and `zero_check_long` build explicit tests
  (opto/parse2.cpp:3285, 3294, 3565, 3577; opto/graphKit.hpp:404-412), and only C1 records div0 sites
  (cpu/x86/c1_LIRAssembler_x86.cpp:2144, cpu/x86/c1_LIRGenerator_x86.cpp:495).
- min_int / -1: the C2 x64 `idiv` encodings check it explicitly (cpu/x86/x86.ad:4532-4611); Arm64 `sdiv` never
  traps.
- `Handle_IDiv_Exception` (os/windows/os_windows.cpp:2652-2687) is then reachable only from code that runs a raw
  `idiv` on min_int / -1, which neither C2, C1 nor the interpreter emits.
- Design: the explicit div0 item is already true. Nothing to do.

### Stack banging

- C2 bangs in the verified entry (cpu/x86/c2_MacroAssembler_x86.cpp:91; Arm64
  cpu/aarch64/macroAssembler_aarch64.cpp:7088). A bang into the yellow zone goes to the SOE continuation, which
  unwinds the frame (runtime/sharedRuntime.cpp:919 and the compiled-code case after it). A bang into a reserved but
  uncommitted stack page is resumed in place after the stack is grown (os/windows/os_windows.cpp:2956-2961,
  2689-2708).
- Live at an x64 bang: the arguments only, in GPRs and the low halves of xmm0-7. Not losable.
- Live at an Arm64 bang: in any compilation with vectors, `p7` is set all-true before the bang
  (cpu/aarch64/macroAssembler_aarch64.cpp:7085-7086). That is losable P state.
- The deopt and uncommon-trap blobs bang with only the result registers live.
- Design: x64 nothing to do. Arm64 needs `reinitialize_ptrue()` moved after the bang, a one-line reorder.

### Uncommon traps and deoptimization

- Uncommon traps are calls; frames are deoptimized by patching return addresses. No hardware exception.
- `nmethod::make_deoptimized` turns call sites into UD instructions (code/nmethod.cpp:2154-2205); the UD is
  redirected to the deopt handler (os/windows/os_windows.cpp:3020-3037). At a call site no losable state is live:
  every XMM and k register is save-on-call for Java calls on Windows (cpu/x86/x86.ad:352-353, 797-815), and on
  Arm64 `p7` is rebuilt after calls and not needed by the deopt handler.
- Design: nothing to do.

### Handshakes and async exceptions

- Both are delivered at polls and thread-state transitions (runtime/javaThread.cpp:774, 1879;
  runtime/handshake.cpp:758-770). The only hardware exception involved is the faulting loop poll, covered above.

### Stubs with UnsafeMemoryAccess

- A page error in an arraycopy or setMemory stub resumes at the stub's error exit
  (os/windows/os_windows.cpp:2992-2998; marks at cpu/x86/stubGenerator_x86_64_arraycopy.cpp:658, 827, 1045). The
  stub's own vector temporaries are abandoned. The C2 caller has no XMM or k value live across the call (all
  save-on-call), so nothing it uses is lost, except APX r30/r31 (finding 3).

### SafeFetch

- x64: SEH `__try`/`__except(EXCEPTION_EXECUTE_HANDLER)` in C++ (os/windows/safefetch_windows.hpp:33-43): an unwind
  into the handler block, not a resume. Arm64: an assembly stub whose fault PC is redirected to its continuation
  (os/windows/safefetch_static_windows.cpp:45-60, os_cpu/windows_aarch64/safefetch_windows_aarch64.S). Neither
  runs with losable state, and their callers are C++ (see below).

### GC barriers (G1, Shenandoah, ZGC)

- All branch to out-of-line slow paths; none relies on a fault. `SaveLiveRegisters` saves the live wide state
  around the runtime call (cpu/x86/gc/shared/barrierSetAssembler_x86.cpp, destructor at 642-673). The
  only exception is a barrier load used as an implicit null check, covered above.

### Vector API and intrinsic stubs

- Called as leaf calls (SVML through `CallLeafVector`, the crypto, CRC, base64 and copy stubs); none takes a
  handled exception except the UnsafeMemoryAccess stubs above. AVX-512 masked loads suppress faults on masked-off
  lanes; a fault on an active lane of a raw access is finding 1.

### C1

- Loop polls fault (cpu/x86/c1_LIRAssembler_x86.cpp:558-564; Arm64 cpu/aarch64/c1_LIRAssembler_aarch64.cpp:563-568),
  return polls branch (549 and 555), implicit null and div0 checks go to C1 stubs.
- C1 emits no ymm, zmm or opmask code (no 256/512-bit or `KRegister` use in cpu/x86/c1_*.cpp) and no SVE. It holds
  only scalar float and double, which are legacy SSE state on xmm0-15. Without the limit, C1 allocates xmm16-31
  when `UseAVX > 2` (cpu/x86/c1_LinearScan_x86.hpp:63-68).
- Design: covered by keeping C1 off xmm16-31, unconditionally on Windows instead of behind
  `exception_resume_loses_xstate()`.

### Interpreter

- Polls branch; implicit null checks in templates go to throw entries; no 256/512-bit, opmask or SVE code in the
  template interpreter. Nothing losable is live. Nothing to do.

### JVMCI

- Removed from mainline by 8382582 ("Remove the experimental JVMCI feature"); src/hotspot/share/jvmci does not
  exist. Not applicable.

### AOT code cache

- Stores C1 and C2 nmethods (tiers 1, 2, 4 and 5: code/aotCodeCache.cpp:1293-1330), validated against a list of VM
  settings (code/aotCodeCache.hpp, the `do_var`/`do_fun` list; scrub14 adds its entries at 545-550).
- Design: covered by recording the notrap mode in that list, so code compiled without it (faulting polls and
  implicit null checks at hot sites) is rejected.

### JFR sampling and thread suspension

- Suspend, GetThreadContext, Resume only (os/windows/os_windows.cpp:6376-6397); no context is written back. JFR
  arms polls (above), which the branch poll covers. Nothing else.

### HotSpot's own C++ code (MSVC)

- On x64 the JVM is compiled with no `/arch` option (only IA32 sets one, make/autoconf/flags-cflags.m4:790), so the
  baseline is SSE2, and there are no AVX intrinsics in src/hotspot. MSVC code therefore holds no losable state
  across any of its handled exceptions (SafeFetch unwinds, the assert-poison resume, `ThreadCrashProtection`).
- One exception: an unsafe copy in VM code (`doing_unsafe_access`, os/windows/os_windows.cpp:2993) can fault inside
  the CRT's `memcpy`, which uses AVX on capable CPUs, and be resumed there. That corrupts bytes of a copy that is
  reported as failed with `InternalError`, so nothing correct is lost.

### Native code the JVM resumes

- The yellow zone outside Java code (os/windows/os_windows.cpp:2925-2926), FP exceptions in native code (3013), and
  unsafe accesses in native code resume JNI or FFM code that may be AVX- or SVE-compiled. This is not compiled Java
  code and the JVM cannot change it; scrub14 did not cover it either. Out of scope.

## The paths not covered

### 1. Unsafe raw accesses in C2 code (blocker)

What happens: `nm->has_unsafe_access()` is set for every C2 compilation that inlines an Unsafe access, copy,
setMemory or atomic (opto/library_call.cpp:2458, 2731, 3001, 6104, 6161), which includes every compilation that
inlines a `MemorySegment` or `ByteBuffer` access. A page error at any raw access in it resumes at
`Assembler::locate_next_instruction(pc)` (os/windows/os_windows.cpp:2995). The frame then keeps running until the
pending `InternalError` is thrown at the next poll or transition, which in a strip-mined loop can be a thousand
iterations later. HotSpot already accepts that the faulting load returns garbage, but on an affected kernel every
other live losable value is zeroed as well: a vector constant, an unrelated accumulator, or `p7` on Arm64 (after
which every SVE instruction governed by it does nothing). Stores of those values before the throw are visible to a
caller that catches the error.

Which code it hits: auto-vectorized and Vector API loops over native or mapped memory segments, exactly the code
whose 256/512-bit or SVE state the design wants to keep. The fault needs an unreadable mapped page (a file truncated
underneath, a network share that goes away), so it is rare, but it is a hardware exception resumed in compiled code
with losable state live, which is what the invariant forbids.

Why the design cannot cover it: the access cannot be made non-trapping, because whether a mapped page can be read is
only known by reading it.

### 2. FP exceptions in compiled code (x64)

What happens: native code returns to Java with FP exceptions unmasked in MXCSR; the next C2 FP instruction that
raises one traps, and the handler resets MXCSR and resumes it with the broken context. The call stub sets the
standard MXCSR on entry from native code (cpu/x86/stubGenerator_x86_64.cpp:259-265), but JNI returns and FFM
downcalls do not.

### 3. APX r30/r31 across C calls (x64)

What happens: C2 keeps values in r30/r31 across C calls on Windows (cpu/x86/x86.ad:176-179). A resumed exception in
the callee (the UnsafeMemoryAccess stubs above) loses them under the caller. UseAPX is off by default and scrub14
turned APX off wherever state could be lost.

## Options

For 1 (pick one; each is a design change, so I did not start any):

- **a. Make losable state dead at those accesses.** While the mode is on, each C2 raw memory access in a compilation
  with unsafe accesses kills the losable registers (the ymm/zmm upper slots, xmm16-31, k1-k7; P and the Z upper
  slots on Arm64), so the allocator spills live vectors around it, keeping the access's own result. This keeps the
  invariant with no detection, but it costs a spill and reload of every live vector per access in exactly the FFM
  vector loops, likely tens of percent there, and needs a careful allocator change (the kill must not cover the
  access's own definition).
- **b. Make unsafe errors synchronous.** HotSpot's own comment asks for this (runtime/sharedRuntime.cpp:1857-1862):
  give raw unsafe accesses an exception edge and debug info so that a page error deoptimizes and throws at the
  access instead of resuming. It removes the resume entirely, but it is a large C2 change (pinned accesses, worse
  vectorization) and an upstream-sized project.
- **c. Keep the scrub for these sites only.** Retains L, its startup traps and stack writes for the unsafe case,
  which is what the design set out to remove.
- **d. Accept and document it.** The resume already produces a garbage load and a pending asynchronous error; the
  extra damage is confined to frames that hit an I/O error on mapped memory. This weakens the invariant to
  "except unsafe page errors", and I would not call it a clean cover.

For 2: while the mode is on, restore the standard MXCSR on every return from native code into Java: the existing
`RestoreMXCSROnJNICalls` path (cpu/x86/macroAssembler_x86.cpp:5576-5578) made unconditional on Windows, and the same
`ldmxcsr` after the call in the FFM downcall stub. Java code then never runs with FP exceptions unmasked, so no FP
trap is resumed in compiled code. Cost: one `ldmxcsr` per native call. Clean, but not in the approved design.

For 3: on Windows, make r30/r31 save-on-call for C calls while the mode is on, or keep APX off on Windows as
scrub14 did. Either is a one-place change.

## Notes for the prototype, if it goes ahead

- The explicit null form must be emitted after register allocation, in front of the access, with the flags caveat
  above on x64.
- The branch loop poll needs a handler blob that does not step over the poll (cpu/x86/sharedRuntime_x86_64.cpp:3158),
  and should always use the wide-vector blob where one exists, since it is chosen only where losable state is live.
- On Arm64 `p7` is not an allocated value, so the liveness analysis must treat it as live everywhere in a
  compilation with `max_vector_size() > 0` under `UseSVE > 0`; on SVE every trap site in such a compilation then
  takes the explicit form, besides the bang reorder.
- The scrub's liveness (opto/trapScrub.cpp:228-320) and `losable_unit` (cpu/x86/x86.ad:2662-2680) are reusable
  as they are for choosing the sites.

## Verification of the unsafe-window argument (decision 1)

The argument to verify: after a page error at an unsafe access, Java program order says nothing after the access
should run, so every write in the window up to the throw at the next poll already has unspecified values in stock
HotSpot (the failed load's register keeps a leftover value), and the kernel bug only changes values in writes that
should not exist. It needs (a) no oop or derived pointer in a losable register, (b) every address and bounds check
in GPRs or re-derived from non-losable state, and (c) a zeroed mask or index only shrinking or redirecting accesses
within what was checked.

**Result: (a) and (b) fail on x64 with `UseFPUForSpilling` on an EVEX CPU, and (c) fails on x64 through
rematerialization. AArch64 passes all three.** Per the instruction, I stopped here; decisions 2-4 are not started.
Both failures have small fixes, proposed at the end of this section.

### Counterexample 1: GPR values spilled into xmm16-31 (fails (a) and (b), x64)

- With `UseFPUForSpilling`, C2 adds the float and double register masks to the spill masks of `RegI`, `RegN`,
  `RegL` and `RegP` (opto/matcher.cpp:650-662). On an EVEX CPU those masks are `float_reg_evex` and
  `double_reg_evex`, which include xmm16-31 (cpu/x86/x86.ad:1126, 1148-1181). So an oop, a narrow oop, a derived
  pointer or a raw native address can be spilled into xmm16-31, which a resumed exception restores as zero.
- When it is on: by default on AMD family 0x17-0x18 and Hygon (cpu/x86/vm_version_x86.cpp:1769-1775, no
  AVX-512, so xmm0-15 only and safe), on Intel server parts only with `UseAVX <= 1` (2031-2038, no EVEX, safe), and
  on Zhaoxin family 6/7 with SSE4.2 (2042-2048: unsafe if such a part reports AVX-512). On any EVEX CPU it is on
  when set on the command line.
- The failure: in a C2 loop over a native segment, the segment's base address (a `RegL`) or a heap array (a `RegP`
  or `RegN`) is spilled to, say, xmm20 under register pressure. A page error at an unsafe access is resumed; the
  reload gives 0. The next access goes to `0 + offset`, or for a narrow oop to `heap_base + offset`. With a large
  offset (a big segment, a high array index) that is a write into mapped memory the program never addresses, for
  example the start of the Java heap. Stock HotSpot never writes there, because the base it reloads is intact.
- Not affected: oops never sit in XMM at a safepoint (the debug masks drop save-on-call registers,
  opto/matcher.cpp:664-675), and vectors never hold oops (opto/superword.cpp:162, 196, 715-743;
  opto/vectorIntrinsics.cpp:302-374). The only route is the spill.

### Counterexample 2: rematerialization from a losable register (fails (c), x64)

- ADLC marks every one-input, one-output instruction rematerializable unless it is a load, a copy or expensive
  (adlc/formssel.cpp:550-561), and `MachNode::rematerialize()` adds no check on where the input lives
  (opto/machnode.cpp:581-630). `MoveF2I` and `MoveD2L` from a register are `DEF dst, USE src`
  (cpu/x86/x86.ad:14829-14841), and their source can be in xmm16-31 on EVEX. When the allocator splits the result's
  live range, it may recompute it at the use and stretches the input's live range to get there
  (opto/reg_split.cpp:321-330).
- The failure: `int i = Float.floatToRawIntBits(f); a[i + 1] = x;` with `f` in xmm20. The range check is on `i + 1`;
  the store folds the `+ 1` into the displacement and indexes with `i` (`[a + i*4 + 20]`). If `i` is
  rematerialized at the store, after the check, and the window has zeroed xmm20, the store writes `a[1]`. The check
  passed with `i = -1`, so `a.length` may be 1, and `a[1]` is past the end of the array: heap corruption, which
  stock HotSpot would not do (it reads `f` intact). Index 0 is in range wherever a check on the index itself
  passed, but not where an offset is applied after the check.
- Not rematerializable, so not affected: the mask-to-int nodes (`VectorMaskFirstTrue`, `LastTrue`, `TrueCount`,
  `ToLong`) all have TEMP or KILL effects (cpu/x86/x86.ad:23815-23935), and lane extracts take an immediate (three
  components, cpu/x86/x86.ad:22497-22545). A rematerialized `firstTrue` of a zeroed mask would have returned the lane
  count, which is why this matters.

### What holds

- AArch64 passes (a)-(c): `UseFPUForSpilling` is off by default there and, if set, spills into the D registers,
  the NEON low bits that the context keeps. The rematerializable extracts and mask nodes are the NEON-only ones
  (`UseSVE == 0`, cpu/aarch64/aarch64_vector.ad:6838-6935); the SVE ones have TEMPs (6896-6912, 6969-6970), and
  lane extracts take an immediate (5987-6000).
- Gathers: the Vector API checks every lane of the index vector and gathers with that same vector against
  `ARRAY_BASE`, with nothing added after the check (src/jdk.incubator.vector/share/classes/jdk/incubator/vector/
  IntVector.java:3116-3125, VectorIntrinsics.java:74-87). A zeroed lane becomes index 0, which is in range because
  at least one lane passed `0 <= index < length`, so `length >= 1`.
- Masked loads and stores check only the active lanes and pass the same mask on (AbstractMask.java:266-288,
  IntVector.java:3060-3073). Zero means inactive for k registers, AVX2 vector masks and SVE predicates (including
  `p7`), so a zeroed mask only drops lanes. An empty array or segment: no active lane can pass the check, so a zeroed
  mask or index there still touches nothing.
- Contiguous Vector API and FFM accesses check offsets in GPRs and address `base + offset` in GPRs; mask-driven
  control flow (`anyTrue`, `allTrue`) only picks between paths that each carry their own checks.
- Raw Unsafe addresses that user code builds from vector lanes have no bounds check by definition, and stock HotSpot
  already lets the window write through an address built from the failed load; they are outside (b).

### Proposed fixes (both apply only while the mode is on, so other platforms do not change)

- For 1: x64 keeps GPR spills out of xmm16-31: the masks added at opto/matcher.cpp:650-662 become the legacy ones
  (xmm0-15, whose low 64 bits are in every context). Equivalent and simpler: turn `UseFPUForSpilling` off on EVEX
  CPUs, which costs nothing by default.
- For 2: `MachNode::rematerialize()` returns false when an input can be in a losable register (on x64, an input
  whose mask includes xmm16-31, a vector width above 128 bits or a k register). It touches only `MoveF2I`/`MoveD2L`
  from xmm16-31, so the cost should be nil.

With both, (a)-(c) hold on x64 as they do on AArch64, and the argument can be written into the unsafe handler's
comment.

## Decisions and the further search (after the verification)

Decided: unsafe access errors stay asynchronous; counterexample 1 is closed by keeping integer and pointer spills
out of xmm16-31, counterexample 2 by refusing rematerialization from a losable input; MXCSR is restored for JNI and
FFM; r30/r31 are save-on-call for C calls on Windows.

The further search for other ways C2 can derive an address, index or oop from a losable register found one more,
and three that do not apply:

- **Found: APX registers in the unsafe window (x64).** With UseAPX, C2 allocates oops, pointers and indices to
  r16-r31 (cpu/x86/x86.ad, reg_mask_init()), which are losable APX state. Fixed at no cost by default (UseAPX is
  off): in a compilation with unsafe accesses, while the mode is on, no live range may use r16-r31
  (NoTrapPlatform::allocation_exclusion(), applied to every live range in PhaseChaitin::gather_lrg_masks(),
  opto/chaitin.cpp). Elsewhere r16-r31 count as losable units, so trap sites with them live take the non-trapping
  forms.
- **Not a route: flags producers.** They are rematerialized whatever their inputs (opto/machnode.cpp, "Defining
  flags - can't spill these"), and a vector test (kortest, vptest) reads a losable register. But a flags producer
  used by a branch in its block is scheduled right before the branch (PhaseCFG::select(), opto/lcm.cpp:631-657),
  so no access falls between the test and its use. A debug check in NoTrapSites asserts it.
- **Not a route: ideal-graph clones.** Loop sinking clones a pure node per use (opto/loopopts.cpp:1943-1985), but
  IGVN hash-conses identical unpinned clones again (opto/phaseX.cpp:1954-1966), and the pinned clones are casts and
  loads, whose inputs are general purpose registers.
- **Not a route: C1 and the interpreter.** Neither uses r16-r31 (no EGPR use in cpu/x86/c1_*).
- **Found while implementing decision 2: FFM upcalls on Windows.** The upcall stub loads the standard MXCSR only
  off Windows (cpu/x86/upcallLinker_x86_64.cpp, `#ifndef _WIN64`), so Java code in an upcall ran with the native
  caller's MXCSR. With the mode on, Windows now does what Linux does: saves the caller's MXCSR, loads the standard
  one if it differs, and restores the caller's on return. Same cost class as decision 2; measured in review8.

## Where each part is (branch JDK-8365196-notrap)

- The flag: AvoidLosableStateAtTraps, diagnostic, on by default on Windows (share/runtime/globals.hpp).
- The pass: share/opto/noTrap.hpp, noTrap.cpp (NoTrapSites::apply(), run from PhaseOutput::Output() before sizing;
  a compilation it changes is not scheduled after register allocation). Platform sides: X86NoTrap
  (cpu/x86/x86.ad), AArch64NoTrap (cpu/aarch64/aarch64.ad).
- Explicit null checks: MachExplicitNullCheckNode (share/opto/machnode.hpp/.cpp); the MachNullCheck it replaces
  records no implicit exception (share/opto/output.cpp, fill_buffer() and FillExceptionTables()). x64 refuses
  flag-consuming candidates (share/opto/lcm.cpp).
- Branch loop polls: MachSafePointNode::is_branch_poll(), the safePoint instructs of both .ad files,
  C2LoopPollStub (share/opto/c2_CodeStubs.hpp, cpu/*/c2_CodeStubs_*.cpp), and the shared blob
  polling_page_branch_safepoint_handler (share/runtime/stubDeclarations.hpp, sharedRuntime.cpp), generated by
  generate_handler_blob() without the step over the poll and saving every vector register.
- The unsafe window: spill masks (share/opto/matcher.cpp, init_first_stack_mask()), rematerialization
  (MachNode::rematerialize(), has_losable_input()), APX exclusion (opto/chaitin.cpp), comment at the handler
  (os/windows/os_windows.cpp, EXCEPTION_IN_PAGE_ERROR).
- AArch64: p7 always live with vectors, set after the stack bang (cpu/aarch64/macroAssembler_aarch64.cpp,
  verified_entry()); the SVE revision gate is skipped with the mode (os_cpu/windows_aarch64/vm_version_windows_aarch64.cpp).
- MXCSR: MacroAssembler::restore_cpu_control_state_after_jni() (JNI wrappers, the interpreter, FFM downcalls with
  a transition), the critical downcall (cpu/x86/downcallLinker_x86_64.cpp), the upcall stub on Windows.
- r30/r31: cpu/x86/x86.ad reg_defs, save-on-call for C calls on Windows.
- C1: cpu/x86/c1_LinearScan_x86.hpp. AOT code cache: share/code/aotCodeCache.hpp, x86 and AArch64 lists.
- The old machinery is off with the mode (VM_Version::set_exception_xstate_mitigation() returns first); it stays
  available with -XX:-AvoidLosableStateAtTraps.
- Debug checks (NoTrapSites::apply() and verify_placement(), reg_split.cpp): every remaining implicit null check and
  faulting loop poll has no losable state live; no RegI/N/L/P value in a losable register (and in an unsafe
  compilation, none in r16-r31); no rematerialization from a losable input; no resumable raw access between a flags
  producer with a losable input and its use.

## Counterexample 3: a Vector API index check whose result is in a mask register (fails (b), x64 AVX-512)

Found in the further search; per the rule I stopped here, because the airtight fix costs something.

- The Vector API checks the index vector of a gather or scatter in vector code:
  `vix.compare(LT, 0).or(vix.compare(GE, length)).anyTrue()` (VectorIntrinsics.java:74-87), then gathers or
  scatters with `vix` (IntVector.java:3116-3125, 3385-3400). On AVX-512 the compares and the `or` produce opmask
  registers and `anyTrue` is a `VectorTest` (kortest) that sets the flags for the branch to the throw. The scatter
  itself writes every lane under an all-ones temporary mask (cpu/x86/x86.ad:18615-18630).
- `VectorTest` is a must-clone flags producer (adlc/archDesc.cpp:1296), so local scheduling places it last in its
  block, right before its branch (opto/lcm.cpp:631-657), while the mask compares that feed it can be scheduled
  earlier. A raw memory access of the same block that is independent of them (an FFM segment read elsewhere in the
  loop body, earlier in program order) can then sit between the compares and the test: the natural order is
  compares, raw access, test.
- The failure: the compares run, find out-of-range lanes and set bits in k1; the raw access faults and is resumed; the
  context comes back with k1 zeroed; the test sees no bad lane, the branch does not throw, and the scatter writes with
  `vix`, whose low 128 bits (lanes 0-3) are in the context and intact. Those lanes are out of range: a write outside
  the array. Stock HotSpot keeps k1 and throws IndexOutOfBoundsException. So the bug moves a write of the window,
  which (b) forbids.
- It needs a program whose indices are out of range (a correct program's mask is zero anyway), an I/O error on a
  mapped page, and that schedule. The same holds for a mask in xmm16-31 with indices in xmm0-15 (a 128-bit species
  on AVX-512).
- Not affected: AVX2 (the mask and the indices lose the same upper lanes, which become index 0); AArch64 SVE in
  practice (the scatter is governed by p7, which the same loss zeroes, so it stores nothing; only a call between the
  test and the scatter, which re-sets p7, would expose it, and checkIndex has none); masked accesses (the access mask
  is lost with the check's, so the access shrinks).

Fixes considered:
- **Kill the opmask registers at raw accesses in compilations with unsafe accesses, while the mode is on** (a
  MachProj that kills k1-k7 on each such access, so the allocator spills live masks around it). Airtight on x64; the
  cost is a kmov to the stack and back per live mask per raw access, in FFM code that keeps Vector API masks live
  (masked tails, gathers): small, but not nothing at default settings.
- **Keep the producers of a `VectorTest`'s mask after the raw accesses of their block** (a local scheduling
  constraint): no instruction cost, but it cannot always hold (a raw access that depends on the mask chain, or a mask
  computed in a dominating block), so it would need the kill as a fallback.
- **Accept it**, as only out-of-range programs are affected, and document it; this weakens the argument.

## Counterexample 3: the fix, and check-carrying values in general (decided)

Decided: the airtight option. While the mode is on, every access to raw or unknown memory in a compilation with
unsafe accesses carries a fat projection that kills the mask registers (Matcher::kill_masks_at_raw_accesses(),
opto/matcher.cpp, a walk over the matched graph at the end of Matcher::match(); the registers come from
NoTrapPlatform::raw_access_kill(); the raw accesses are Matcher::is_raw_memory_access(), which the debug check
uses too; the kill is a projection, so the allocator's copies of an access keep it). The allocator then spills any such mask that is live across the access and
reloads it afterwards from memory, which the fault does not touch, so the check reads the mask computed before the
fault. The access's own result is left out of the kill. No scheduling rule was added. Debug check (NoTrapSites):
in such a compilation no killed register is live across an access with a raw memory operand.

Which registers can carry a check's result across a raw access, and what is killed:
- **x64, opmask registers k1-k7: killed.** With AVX-512 the Vector API's masks are opmask registers
  (Matcher::has_predicated_vectors() is supports_evex(), cpu/x86/matcher_x86.hpp:138-140). A resumed exception
  loses an opmask register entirely, while the vector it checks keeps its low 128 bits.
- **x64, xmm16-31 holding a mask: killed only on AVX-512 without VL, BW and DQ.** There, masks of some shapes stay
  vectors (TypeVect::makemask() needs the masked VectorLoadMask), and such a vector can be in xmm16-31, which loses
  it entirely. With VL, BW and DQ every Vector API mask is an opmask register.
- **x64, a mask in xmm0-15 (AVX2, and narrow shapes without VL): not killed.** A compare produces lanes of the width
  of the vectors it compares, so a mask computed from the index vector of a gather or scatter (VectorIntrinsics
  checkIndex) or from an access mask (AbstractMask.checkIndexByLane) has the lane layout of the vector or mask it
  guards. A resumed exception loses the same bits, above 128, of both: the lost lanes of the indices read as 0,
  which a passed check implies is in range, and their lost mask lanes read as "not out of range". Lanes below 128 are
  kept in both. So a mask in these registers can only fail to report lanes whose indices are now 0.
- **x64, a check reduced to a general purpose register (vpmovmskb, kmov to a GPR, a reduction): safe**, GPRs are kept.
- **AArch64 SVE, predicates p0-p6 and p8-p15: killed.** The check's result is a predicate, lost entirely, while the
  vector it checks keeps its low 128 bits. The access is governed by p7, lost too, so the loss alone makes it do
  nothing; but a call between the check and the access sets p7 again, and a predicate kept across a raw access
  would then let a failed check pass. p7 is not killed: losing it only stops the accesses it governs.
- **AArch64 NEON: nothing to kill**, every NEON register is in the context.

**Residual, not closed:** a check that user code computes itself, in vector registers, on a copy of the indices with
another lane layout, and keeps as a vector across a raw access: for example the indices widened from int to long
lanes, compared as longs, the result kept as a long-lane vector mask in xmm0-15 (AVX2) or as a vector in z0-z31,
and tested after the access. Its lanes 2 and 3 sit above bit 128 and are lost, while the int indices of those lanes
sit below it and are kept, so a failed check of those lanes could pass. The Vector API's own checks never do this
(their masks are computed from the vector they guard, and on AVX-512 and SVE they are mask registers, which are now
killed). Closing it would mean killing the upper halves of every vector register at raw accesses, which spills
every wide vector live across one: the cost that ruled out spilling around unsafe accesses.

## The older machinery removed (decided)

The scrub14 mechanism is gone from the branch: the Windows kernel classifier and its revision tables, the
stale-header scrub (TrapScrub, MachTrapScrubNode, the startup verification of L), the fallback cap on UseAVX and
MaxVectorSize, the Windows/AArch64 SVE gate, the flags ScrubExceptionXStateHeader, ExceptionXStateLength,
ForceExceptionXStateProtection and ExceptionXStateMitigation with the develop and verify flags, and their gtests and
jtreg tests. AvoidLosableStateAtTraps is the only mechanism; -XX:-AvoidLosableStateAtTraps gives compiled code with
no protection at all (the A/B of the repro). TestExceptionXStateScrub, which plants the stale header and keeps vector
state live across polls and a faulting null check, stays as TestExceptionXStateLoss, the regression test of the mode.

## The cost of the mask kill, and which raw accesses need it (branch JDK-8365196-cost-kmask)

### What the kill costs, and where

- **Only compilations that inline a scalar unsafe access have it.** `Compile::has_unsafe_access()` is set by the
  scalar Unsafe intrinsics alone (opto/library_call.cpp); the Vector API's segment loads and stores
  (opto/vectorIntrinsics.cpp) do not set it. A method whose only raw accesses are Vector API segment accesses is
  therefore not "a compilation with unsafe accesses": none of its accesses kills anything, and a page error in it
  is not resumed at all (the nmethod has no `has_unsafe_access`, so the handler does not take the unsafe path and
  the VM reports the fault, as upstream does). The kill matters where vector code and a scalar segment or Unsafe
  access share a compilation: a vector loop with a scalar tail loop, a header read in front of a vector loop.
- **The +5-9% of the off-heap masked kernels of review9 is not the kill.** `segmentMaskedSum` and
  `segmentCompareBlendStore` are not unsafe compilations (see above). PrintOptoAssembly of their hot loops with the
  mode off and on (linux-x64 fastdebug, UseAVX=3 and UseAVX=2, run 45898446266) differs only in explicit null
  checks: two per iteration in `segmentMaskedSum` (the klass load of the segment, the load of
  `scope.owner`), four in `segmentCompareBlendStore`, one in `unsafeWithLiveMask` and `heapMaskedSum` (the
  range load of the heap array). That is the cost-checks area.
- **The kill is expensive because of where the allocator puts a killed mask.** A mask live range that crosses a
  kill cannot be colored. The kill is an interference, not register pressure (the mask registers count in
  neither pressure class), so PhaseChaitin::Split() has no place at which to split it: after the first failed
  round the live range is forced down everywhere (`_was_spilled1`), it lives in a stack slot for its whole
  extent, every use reloads it, and a Phi of it is a memory-to-memory copy. In `unsafeWithLiveMask` the live
  range is the all-true mask of the full iterations, which C2 computes once in front of the loop; the scalar read
  of the loop kills it on every iteration, although the loop's own masks are dead at the read.

### The rule: a mask that a fault can zero must not reach a check

Counterexample 3 needs a mask that was computed before the fault and is *tested* after it. The unconditional kill
protected every mask, including the ones that are only predicates. The rule that the branch implements, on x64
with AVX-512 VL, BW and DQ (where every mask is an opmask register value; elsewhere, AVX-512 without them and
AArch64 SVE, every raw access still kills, as before):

> In a compilation with unsafe accesses, no mask that is in a mask register across a raw access is read
> afterwards by a reader that checks, directly or through the masks computed from it.

A reader *checks* unless it only applies the mask as a predicate (`checks_masks()`, opto/noTrap.cpp):

- **Predicates (not checks):** an instruction that computes a vector (a masked load, gather, arithmetic or
  compare with a vector result, a blend, a compress or expand), and the masked vector stores and scatters. With a
  zeroed mask they compute a vector with lanes dropped or passed through, or store nothing. The vector is a value
  of the unsafe access window, like the upper halves the fault zeroes in any case.
- **Checks:** everything else that reads a mask. Flags producers (VectorTest, that is `anyTrue`/`allTrue`),
  conversions to an integer (`toLong`, `trueCount`, `firstTrue`, `lastTrue`), VectorStoreMask (it computes a
  vector, but one that holds the mask itself: the payload of a boxed mask), the arguments of calls, returns.
- **Mask to mask:** mask logic, masked compares with a mask result, casts, the allocator's copies and merges, Phis.
  They pass the question on: their input is read for a check exactly if their result is.
- **Debug info (not a check).** A mask in the debug info of a safepoint, call or uncommon trap is read only if the
  frame is deoptimized there. Deoptimization turns it into a heap object, which every later reader sees alike,
  and whatever runs after a deoptimization checks its own accesses: the interpreter, C1 code and the Java bodies
  of the Vector API (the fallbacks of `VectorSupport`) by the bounds checks of the language, and other compiled
  code by the checks it carries for its own intrinsic accesses, on the objects it is given. No check of the
  Vector API is split from its intrinsic access by a deoptimization with the *result* of the check as state
  other than in a mask, and an intrinsic access is only reached from compiled code of the method that also
  contains its check. Independently, the pending InternalError is thrown where the interpreter takes over
  (`uncommon_trap_inner` is a JRT_ENTRY, whose return to Java installs the asynchronous exception, and the
  interpreter's deoptimization entries throw a pending exception before the next bytecode), except where the
  thread has asynchronous exceptions blocked (Thread.exit, the virtual thread transitions).

Why this keeps (c):

- Every reader that checks sees a mask that was not in a mask register at the fault: it was on the stack, in a
  general purpose register, or is computed afterwards from masks of which the same holds, because the question is
  followed backwards through the mask operations (an operation whose result reaches a check makes its inputs
  reach it). That is exactly what the unconditional kill gave these readers.
- So a mask that reaches a check holds, at every reader, either the value the program computed or, in its
  register after a fault, zero. Never more lanes.
- A mask that reaches no check can hold anything after a fault (zero, or a value computed from zeroed masks, such
  as all lanes for the negation of a lost mask). It is only ever a predicate, and the masked access it governs is
  one whose mask the compilation never tests: the Vector API tests the mask of an access exactly when the access
  is not wholly in bounds (`checkMaskFromIndexSize`, in the slow path of each masked load and store, on the same
  mask value that the access of that path takes), so an access with an untested mask is the access of the fast
  path, whose lanes are all in bounds by the scalar test of its offset. Any mask is safe for it. Gathers and
  scatters test their index vector, not their mask.
- What changes for values is that a predicate can differ from the program's mask where the kill would have
  reloaded it: lanes of a value of the window dropped or passed through, or a store of the window that does not
  happen or stores unspecified lanes in bounds.

The residual of the earlier section is unchanged and the same kind: a check that the program carries through
vector registers (here also: a mask turned into a vector by a blend, `VectorMask.toVector()`, and compared
again) is not protected, with the kill or without.

### How the rule is enforced

1. **Before register allocation, on the scheduled code** (`NoTrapSites::kill_masks_where_checked()`, called from
   `Compile::Code_Gen()` after global code motion): `MaskFlow` is a backward dataflow over the mask values,
   "reaches a check"; it finds the masks that are live across a raw access and reach a check afterwards.
   `protect_masks()` then gives each reader of such a mask that checks a value the fault cannot have zeroed,
   and leaves the mask in its register for the other readers:
   - a constant mask (MaskAll of a constant, the all-true mask of the full iterations of a loop with a masked
     tail): a copy of its definition right in front of the reader;
   - any other mask: a copy bound to the stack, made right after the definition, and a reload of it right in
     front of the reader. Neither node carries the copy flag, so the allocator's copy removal, which follows
     copies back to a register that still holds the value, cannot point the reader at the mask's register again.
   A reader that follows the definition in its block with no raw access in between reads the mask itself. For a
   Phi the copy or reload goes to the end of the predecessor block. A mask it cannot protect (the result of the
   access of an implicit null check) leaves the kill on the accesses it is live across, as a projection added
   after scheduling.
2. **After register allocation, on the final code** (`NoTrapSites::apply()`): the same dataflow over the
   registers and stack slots that hold masks. If a mask register that is live across a raw access reaches a
   check, the compilation is abandoned with `C2Compiler::retry_kill_masks_at_every_raw_access()` and compiled
   again with the unconditional kill of the matcher (`Matcher::kill_masks_at_raw_accesses()`), whose debug
   check is the earlier one (no killed register live across a raw access). This is a product check, not an
   assert: the allocator may stretch a mask across an access after step 1 (the masks of a flags producer it
   clones), and step 2 is what makes the rule hold whatever it did.

The cost that remains at a protected mask is one store of the mask per execution of its definition, and a reload
only on the paths that check, which in Vector API code are the slow paths (an access that is not wholly in
bounds). The unconditional kill cost a store and a reload at every use, and a memory-to-memory copy at every
Phi, because the allocator can only spill such a live range everywhere.

### Found on the way, outside this branch

These are in the design the branch starts from, and the branch does not change them. I stopped at the description.

1. **Counterexample 4 to (c): the check of a masked access reads a vector the fault can zero (x64, AVX2 and
   AVX-512; AArch64 SVE by the same reading).** The slow path of every masked load and store tests its mask
   against the lanes that are in bounds: `AbstractMask.checkIndexByLane()` computes
   `badMask = iotaScaled.compare(GE, broadcast(indexLimit))` (`checkIndex0()`, AbstractMask.java:292-354),
   ands it with the access mask and throws if any lane is left. `iotaScaled` comes from the species' iota
   vector, a loop invariant that C2 loads once and keeps in a vector register across the loop. After a resumed
   fault at a raw access of the loop, that register comes back with its lanes above 128 bits zeroed (all of them
   in xmm16-31), and a zero lane compares as in bounds wherever `indexLimit > 0`. The check then passes for
   lanes that are out of bounds, and the masked access that follows takes the access mask, which is intact: the
   loop computes it again on every iteration (the iota is not computed again), and for the iteration of the fault
   the mask kill reloads it on AVX-512, and on AVX2 it is intact whenever it sits in a stack slot. The frame runs
   on until its next poll, which in a strip-mined loop is many iterations away. A program whose mask has a lane
   beyond the array or segment, which must get an
   IndexOutOfBoundsException, instead reads or writes past the end. Same kind and same preconditions as
   counterexample 3 (a wrong program, an I/O error on a mapped page, a compilation with unsafe accesses), but the
   value that is lost is a vector operand of the check, not its mask result, so the mask kill does not cover it.
   The argument "a mask in xmm0-15 loses the same lanes as the vector it checks" does not help: here the check
   compares a vector with nothing the access uses.
   Possible fixes, cheapest first: compute the bounds mask of `checkIndex0()` from the general purpose
   registers where the platform can (the VectorMaskGen intrinsic behind `indexPartiallyInUpperRange`), so that
   the check has no vector operand; or extend the search of this branch from masks to the vectors that reach a
   check (a compare whose mask reaches one) and keep those on the stack for the compare. The second is general
   but puts a reload in front of every such compare whose operand is live across a raw access, including the
   needle of a search loop (`v.compare(EQ, needle).anyTrue()`).
2. **The same shape for the index vector of a gather or scatter, if the allocator gives the check and the access
   different copies.** `checkIndex(vix, length)` and the scatter both read `vix`. If one reads a register
   copy and the other a stack copy made before the fault (post-allocation copy removal points a use at a
   register that still holds the value, postaloc.cpp `elide_copy()`), the check can see zeroed upper lanes
   (index 0, in bounds) while the scatter uses the intact ones. I did not construct a case; the argument of (c)
   for gathers ("a zeroed lane becomes index 0, which a passed check implies is in range") assumes one copy.
3. **Vector API segment accesses do not make a compilation "unsafe"** (see the top of this section), so a page
   error at such an access in a method without a scalar unsafe access is not resumed but reported as a crash.
   That is upstream behaviour and outside the invariant, but it bounds where the unsafe window exists.
