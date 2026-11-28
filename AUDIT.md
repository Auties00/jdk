# JDK-8365196: AvoidLosableStateAtTraps, the design and its audit

Branch `JDK-8365196-notrap` of Auties00/jdk, on mainline `2b376fb353c`. Every file below is under `src/hotspot/`
unless it says otherwise; line numbers are those of the tree the first audit read (`c6f997c2c66`) and are kept as
pointers, not as exact positions.

On some Windows kernels, resuming from a hardware exception resets the processor state that the exception's
context left out. That state is called losable here: on x64 the upper halves of ymm and zmm, xmm16-31, k1-k7 and
the APX registers; on Arm64 the SVE Z registers above their low 128 bits, P0-P15 and FFR.

The invariant of the mode (`-XX:+AvoidLosableStateAtTraps`, diagnostic, on by default on Windows): **compiled code
takes no hardware exception that the VM resumes while losable state is live.** There is no detection of the
kernel, no scrub and no fallback: the code is shaped so that the question does not arise.

- C2 loop polls and implicit null checks take a non-trapping form wherever losable state is live at the point the
  trap would resume at (sections 2 and 3). A trap that must throw names only what the handlers it can reach
  read, so a null check whose exception leaves the frame through a `finally` keeps its trap (section 3).
- C2 takes no implicit div0 and no min_int / -1 trap; C1 uses xmm16-31 only where no trap of its own resumes its
  frame (section 4); the interpreter holds no losable state.
- Java code always runs with the standard MXCSR, so no FP exception is resumed in it (section 6).
- One exception cannot be avoided: a page error at a raw (unsafe) memory access is resumed past the access with
  an InternalError pending. For it the mode guarantees that nothing that steers a later access was in losable
  state at the fault (section 5).
- What it costs, and what each rule was narrowed to, is in section 8; where each part is in the tree, and its
  tests, in section 9.

## 1. How the Windows filter resumes

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

## 2. Path by path

The question for each path: does HotSpot resume Java or compiled execution after a hardware exception (or after
restoring a saved context) while losable state can be live, and what does the mode do about it?

### C2 loop polls

- x64: faulting, `testl rax, [poll]` with `relocInfo::poll_type` (cpu/x86/x86.ad:17104-17119). Arm64: faulting,
  `ldrw zr, [poll]` (cpu/aarch64/aarch64.ad:15438-15450).
- The fault goes to `get_poll_stub`, which picks the wide-vector blob when the nmethod has wide vectors
  (runtime/sharedRuntime.cpp:667-700); the blob then saves registers that the broken context already zeroed.
- Losable state live: yes, routinely (vectorized loops, Vector API). JFR makes it frequent: it arms the local poll
  of every thread it samples (jfr/periodic/sampling/jfrThreadSampler.cpp:53, 370), and handshakes and safepoints
  arm it too.
- With the mode: a poll across which losable state is live is a branch to a stub (C2LoopPollStub), which enters a
  handler blob that saves every vector register and does not step over the poll
  (polling_page_branch_safepoint_handler). The branch form tests the polling word of the thread and loads no
  polling page address (section 3).

### C2 return polls

- Branch-based on both: `safepoint_poll(..., at_return = true)` to a `C2SafepointPollStub`
  (cpu/x86/x86.ad:1972-1983, cpu/aarch64/aarch64.ad:1910-1911). Loop polls fault and
  return polls branch.
- No hardware exception. Nothing to do.

### C2 implicit null checks

- Formed before register allocation by `PhaseCFG::implicit_null_check` (opto/lcm.cpp:116-400) and recorded in the
  implicit exception table from `MachNullCheck` (opto/output.cpp:1717-1720, 2077-2086). The continuation is
  `non_connector_successor(0)` of the null-check block (opto/output.cpp:2083), code in the same frame (an
  uncommon trap or an exception path) that spills or uses whatever the registers hold after the broken restore.
  ZGC and Shenandoah late-expanded barrier loads can be null-check accesses too (opto/output.cpp:2079-2082).
- Losable state live: yes.
- With the mode: an explicit test and branch to the same successor, inserted after register allocation in front
  of the access, wherever losable state is live into the null successor (section 3). Two facts:
  - x64: the test kills the flags. The accepted accesses are plain loads and stores plus any instruction with an
    embedded load except flag producers (opto/lcm.cpp:197-262), so a flag consumer with a memory operand (a cmov
    from memory) can be the access. Such candidates are refused while the mode is on (opto/lcm.cpp).
  - Arm64: `cbz`/`cbzw` needs no flags.

### Implicit div0 and min_int / -1

- C2 has no implicit div0 on any platform here: `zero_check_int` and `zero_check_long` build explicit tests
  (opto/parse2.cpp:3285, 3294, 3565, 3577; opto/graphKit.hpp:404-412), and only C1 records div0 sites
  (cpu/x86/c1_LIRAssembler_x86.cpp:2144, cpu/x86/c1_LIRGenerator_x86.cpp:495).
- min_int / -1: the C2 x64 `idiv` encodings check it explicitly (cpu/x86/x86.ad:4532-4611); Arm64 `sdiv` never
  traps.
- `Handle_IDiv_Exception` (os/windows/os_windows.cpp:2652-2687) is then reachable only from code that runs a raw
  `idiv` on min_int / -1, which neither C2, C1 nor the interpreter emits.
- Nothing to do.

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
- With the mode: x64 nothing to do. On Arm64 `reinitialize_ptrue()` runs after the bang.

### Uncommon traps and deoptimization

- Uncommon traps are calls; frames are deoptimized by patching return addresses. No hardware exception.
- `nmethod::make_deoptimized` turns call sites into UD instructions (code/nmethod.cpp:2154-2205); the UD is
  redirected to the deopt handler (os/windows/os_windows.cpp:3020-3037). At a call site no losable state is live:
  every XMM and k register is save-on-call for Java calls on Windows (cpu/x86/x86.ad:352-353, 797-815), and on
  Arm64 `p7` is rebuilt after calls and not needed by the deopt handler.
- Nothing to do.

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
- With the mode: C1's loop polls branch and its xmm16-31 are dead wherever a trap of its own resumes its frame
  (section 4).

### Interpreter

- Polls branch; implicit null checks in templates go to throw entries; no 256/512-bit, opmask or SVE code in the
  template interpreter. Nothing losable is live. Nothing to do.

### JVMCI

- Removed from mainline by 8382582 ("Remove the experimental JVMCI feature"); src/hotspot/share/jvmci does not
  exist. Not applicable.

### AOT code cache

- Stores C1 and C2 nmethods (tiers 1, 2, 4 and 5: code/aotCodeCache.cpp:1293-1330), validated against a list of VM
  settings (code/aotCodeCache.hpp, the `do_var`/`do_fun` list; scrub14 adds its entries at 545-550).
- The mode is recorded in that list, so code compiled without it (faulting polls and implicit null checks at hot
  sites) is rejected.

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
  code and the JVM cannot change it. Out of scope.

## 3. The C2 trap sites: which take the non-trapping form

The trap sites are chosen per site after register allocation (NoTrapSites::apply(), opto/noTrap.cpp). Each rule
is as narrow as what the resumed code can read, and the non-trapping forms carry no instruction they do not need.
For each, the argument that the invariant holds.

### An implicit null check counts what is live into its null successor

Before: an implicit null check became explicit when losable state was live across the access, was an input or TEMP
of the access, or was the access's own result. Now: when losable state is live into the null successor of its
block (opto/noTrap.cpp, NoTrapSites::apply()).

Argument. A fault at the access is not resumed at the access. `continuation_for_implicit_exception()` returns the
continuation of the nmethod's implicit exception table, which PhaseOutput::FillExceptionTables() sets to the label
of `non_connector_successor(0)` of the null check's block; the handler puts that pc in the context and returns
`EXCEPTION_CONTINUE_EXECUTION`. So after the fault the thread runs the null successor and whatever follows it, and
neither the rest of the access nor the not-null successor from that point. A register the context lost is observed
only if code reachable from the null successor reads it before writing it, which is the definition of live into
that block. The liveness is the same backward dataflow as before, over every block and every input of every node,
so it includes:

- the debug info of an uncommon trap on the null path: its values are inputs of the call (of the MachUncommonTrap
  node on AArch64), the fields of scalarized vector boxes included;
- an exception handler of the same frame, and any code after it, loops included: they are successors;
- Phi inputs, all of them (the dataflow does not tell the predecessors apart, which only adds).

The common case it frees: a null check whose exception leaves the method. Its null successor is an uncommon trap
that must throw, and GraphKit::add_safepoint_edges() prunes the locals and the stack of such a trap down to the
inputs of the bytecode in every frame without exception handlers (opto/graphKit.cpp, `can_prune_locals`): the trap
names no vector, so nothing losable is live into it, whatever is live across the access. In review7's
nullCheckVectorLive the four checks of the unrolled loop are implicit again, as without the mode (the round 11
build made 8 of that kernel's 14 null checks explicit, this one 2: those whose trap is a loop predicate's and
keeps the accumulator in its debug info).

What it leaves out is a value read only by the access itself or on the not-null path: the access's inputs and TEMPs
(the faulting instruction is abandoned; an access that is several instructions, a late barrier load, faults on its
first, the load), the access's result (not defined on the null path), and values computed ahead for the not-null
path, such as a broadcast or another vector that C2 hoisted out of a loop and that no bytecode state names.

Tests: TestAvoidLosableStateAtTraps, id explicit, checks both directions on x64 (`kernel`, whose handler reads the
accumulator, gets explicit checks; `invariantKernel`, with a hoisted broadcast live across the check and an uncommon
trap on the null path, keeps its implicit check; the round 11 build made that one explicit); the stress ids run
`invariantKernel` with a null that takes the trap; TestExceptionXStateLoss on a kernel that loses state has eight
accumulators live into the handler of its null check and fails if that check traps, and its `kernelHoisted` keeps
live into the handler a vector that no bytecode state names (a broadcast hoisted out of the loop, read again by the
iterations after the handler), which a rule that looked at the debug info alone would miss. The debug check of
NoTrapSites still asserts that a null check is explicit exactly where the rule says so.

### AArch64: p7 does not count where the null successor sets it again first

Before: with SVE, p7 counted as live everywhere in a compilation with vectors, so every implicit null check of
such a compilation became explicit. Now p7 does not count at a null check whose null successor, followed through
straight-line code only, reaches an uncommon trap or a Java call through nothing but moves of general purpose or
NEON values, integer and pointer constants, Phis, projections and jumps (NoTrapPlatform::sets_always_live_units(),
ignores_always_live_units(); opto/noTrap.cpp, always_live_units_dead_into()). Any other node, or a block with two
successors, keeps p7 live. Loop polls are unchanged: p7 counts at every one.

Argument. p7 matters only as the governing predicate of an SVE instruction. After the fault the code that runs is
the listed nodes and then the trap or the call:

- the moves and constants are `mov`, `ldr`, `str`, `fmov` and NEON moves (MachSpillCopyNode::implementation(),
  cpu/aarch64/aarch64.ad); moves of scalable vectors and of predicates are not in the list (the stack to stack copy
  of a predicate goes through p7);
- an uncommon trap is a `bl` to the uncommon trap blob, which saves the registers with unpredicated `str` of Z and
  P (MacroAssembler::push_CPU_state()) and does not return;
- a Java call is the branch; a compiled callee sets p7 in its own entry (MacroAssembler::verified_entry()), the
  interpreter and the resolution and adapter stubs govern nothing with a predicate, and when the call returns the
  caller sets p7 again (aarch64_enc_java_static_call, aarch64_enc_java_dynamic_call).

So nothing reads the lost p7. The Z registers above 128 bits and the other predicates are ordinary units of the
dataflow and still count when live into the null successor.

### The unsafe window rules apply in compilations with unsafe accesses only

Before: integer and pointer spills were kept out of xmm16-31, and no value was rematerialized from a losable input,
in every compilation. Now both apply only when `C->has_unsafe_access()` (opto/matcher.cpp,
init_first_stack_mask(); MachNode::rematerialize()), as the APX exclusion and the mask kill already did.

Argument. Both rules exist for one window: an unsafe access fault resumed past the access, which the handler does
only for an nmethod with `has_unsafe_access()` (os/windows/os_windows.cpp, EXCEPTION_IN_PAGE_ERROR), a flag copied
from the compilation and set while parsing, before matching. In a compilation without it no exception is resumed
while losable state is live: its only resumed traps are loop polls and null checks, and NoTrapSites decides those
from the final allocation, where a spilled integer in xmm16-31, or an input whose live range a rematerialization
stretched, is losable state like any other and gives the site its non-trapping form. Oops in such a spill at a
poll are found as they are without the mode (the handler blob saves every register and the oop map names it). The
debug checks moved with the rules: verify_placement() and the assert of split_Rematerialize() test compilations
with unsafe accesses.

### A branch poll does not load the polling page address

Before: the branch poll kept the load of the thread's polling page address, the input of the faulting form, as a
dead instruction. Now NoTrapSites removes that load when it is a load off the thread alone, in the block of the
poll, and the poll is its only use (remove_poll_address()). The branch poll is then a test of the polling word and
a branch, two instructions as the faulting poll was.

Argument. The branch form reads only `JavaThread::_polling_word`. The load has no other use (checked: one out,
which also rules out a null check on it and projections), no side effect and cannot fault (its base is the thread).
The poll takes the thread, the removed load's own input, in place of the load: BuildOopMaps computes the liveness
of the registers from the inputs of the nodes in the blocks, so an edge to a node that is in no block would mark a
register live with no definition that reaches it. The thread's register is read at the poll as it was read at the
load, a few nodes earlier in the same block, and the code of the branch form does not read the operand. A load
that the allocator split from the poll (a spill copy in between), a load in another block, or one with an index or
a second use, stays.

### The non-trapping forms take part in the Intel JCC erratum mitigation

The explicit null check and the branch poll are conditional branches that C2's mitigation did not know: on the
affected processors they could cross a 32 byte boundary, which C2 avoids for every other branch. They are now
tagged and padded like the others (cpu/x86/c2_intelJccErratum_x86.cpp). No effect on the invariant.

### A trap that must throw names what the handlers it can reach read

What the explicit null checks of the segment kernels were. Every Vector API access to a memory segment runs,
in `ScopedMemoryAccess`, `session.checkValidStateRaw()` on the scope of the segment, and the null check of
that receiver is an implicit one on the load of `MemorySessionImpl.owner`. Its null path is the uncommon trap
of a null check that must throw (GraphKit::builtin_throw()), like the null check of the segment itself in
`IntVector.fromMemorySegment`. For such a trap GraphKit::add_safepoint_edges() names only the inputs of the
bytecode, in every frame, but only while no frame has an exception handler ("as a quick and dirty
approximation, we look for any handlers at all"): from the first frame whose method has one, that frame and
all its callers are described in full. The check of the scope is in
`...ScopedInternal()`, a method with a `finally` (`Reference.reachabilityFence(session)`), called from one with
a `catch (ScopedAccessError)`. So its trap described every frame up to the kernel: the vector that a store
writes, the mask of the iteration, the accumulator of a reduction, as fields of scalarized boxes in ymm, zmm
or mask registers. Losable state was live into the null successor and the check was explicit, once per access
and iteration (PrintOptoAssembly of the review9 kernels under JMH: every explicit check of a hot block was
this one, at `...ScopedInternal @ bci:1`, reason `null_check`; the null check of the segment, in frames
without handlers, was implicit).

Now, with the mode, add_safepoint_edges() looks at the handlers (handlers_reached_by_throw(),
handler_always_throws(), opto/graphKit.cpp). For each frame of a trap that must throw, from the youngest, as
long as the throw is known to reach the frame:

- the handlers that cover the bytecode of the frame are found (the bytecode that throws, or the call that the
  throw comes out of), whatever class they catch;
- the frame keeps the locals that are live into one of those handlers (MethodLiveness at the handler) and
  nothing of its stack but, in the youngest frame, the inputs of the bytecode. Without a covering handler it
  keeps nothing, as a frame of a method without handlers did before;
- the throw is known to reach the caller if every covering handler always leaves the frame by an exception:
  its code runs in a straight line (no branch, switch, jsr, ret or return) to an `athrow`, and every handler
  that covers a bytecode on the way does the same. That is the handler javac makes for a `finally`, and a
  `catch` that throws another exception. Otherwise the callers are described in full, as before.

For the check of the scope: `...ScopedInternal()` keeps `session`, the one local its `finally` reads;
`loadFromMemorySegmentMasked()` keeps nothing (its handler reads the exception); the frames of the Vector API
and the kernel have no handler and keep nothing. The trap names no vector, nothing losable is live into the
null successor, and the check is implicit again, in a loop or in front of it, whatever the kernel keeps in
vector registers. The same holds for a null, range or class check inside a `try` with a `finally` or with
resources in the user's own method.

Argument. A trap that must throw re-executes its bytecode in the interpreter, and the bytecode throws: the
value that failed the check is among its inputs, which the trap keeps (null checks, range checks, division
by zero, class and array store checks, and the same when the trap is taken to post a JVMTI exception event).
An asynchronous exception delivered at that point (the ScopedAccessError of a concurrent close, a JVMTI
stop) is thrown at the same bytecode. The interpreter then looks for a handler of the frame that covers the
bytecode and catches the class of the exception, and unwinds the frame if there is none:

- a handler that is entered reads only locals that are live into it, and starts with a stack that holds
  the exception alone. The locals the frame keeps are the union over all the covering handlers, so it does not
  matter which of them catches, or what was thrown;
- a frame that is unwound is read by nothing;
- a handler that always leaves by an exception never returns to the caller and never falls out of its code:
  the only ways out of a straight line that ends in `athrow` are that `athrow` and an exception of a bytecode
  on the way; either is caught by a handler that covers that bytecode, of which the same was checked (a
  handler that covers its own code, as javac's `finally` handlers do with their first instruction, is entered
  again and leaves in no other way), or unwinds the frame. So the caller is reached, if at all, by the same
  throw at its call, and the argument repeats there. Where it cannot be shown, the callers keep everything.

What reads a frame's locals besides its own code is ruled out as before: a debugger or an agent that can
read locals or pop frames turns the pruning off altogether (`should_retain_local_variables()`), and
ciMethod::liveness_at_bci() then reports every local live.

For the invariant nothing changes: the rule for implicit null checks still counts what is live into the null
successor, and its debug check still asserts that a check is explicit exactly there. The trap simply names
less. A handler that goes on (the `catch (NullPointerException)` of TestExceptionXStateLoss and of
TestAvoidLosableStateAtTraps.kernel(), whose loops continue with their accumulators) keeps what it reads
live, in its frame and in every caller, and those checks stay explicit. The change is made with the mode
only: without it the traps are as upstream.

Tests: TestAvoidLosableStateAtTraps.finallyKernel() keeps a vector in a local across a null check inside a
`try` with a `finally`. The id `explicit` checks that the check is implicit with the mode (it was explicit:
the trap named the vector), and the other ids take the trap with a null and check that the `finally` read
the index it was given, the one local the trap keeps.

### The checks of a native memory segment leave the loop of a masked Vector API access

Before: in a loop that reads or writes a native memory segment through the Vector API with a mask
(`fromMemorySegment(..., m)`, `intoMemorySegment(..., m)`), every iteration ran, for each access, all the
checks of the segment: its null and class check (`ms.byteSize()`), and the null check of its scope, the owner
check and the state check. LibraryCallKit::inline_vector_mem_masked_operation() put a MemBarCPUOrder around
every masked access whose base can be null, which includes the base that is always null of a native segment.
The barrier defines the whole memory state, so every field load after it in the loop (the field that holds
the segment, `scope`, `owner`, `state`, `length`, `min`) depended on the memory of the previous iteration,
none was loop invariant, and loop predication could hoist none of their checks. The unmasked accesses
(inline_vector_mem_operation()) and the scalar ones (C2Access::needs_cpu_membar()) place the barrier only for
an access that may be to the heap or not, and their loops keep no such check: in compareAcrossLoad every
check of the three segments is a loop predicate in front of the loop.

Now the masked access follows the same rule: a barrier only when the base may be null or not. With a base
that is always null the loop has no barrier, the field loads are invariant, loop predication hoists the null
check and the class check of the segment, and the profiled loop predicates hoist the checks of the scope in
the path of the full iterations. This is not a rule of the mode, and it changes the code with the mode off
as well; it is here because the checks it removes from the loop were the trap sites the mode paid for.

Argument. The barrier exists to keep an access whose alias category is unknown ordered with the accesses to
the heap it may alias. An access whose base is the null constant has a raw address
(LibraryCallKit::make_unsafe_address() returns `CastX2P(offset)`), its alias category is the raw one
(TypeRawPtr::BOTTOM), and its load or store takes and defines the memory of that category alone: it is
ordered with every other raw access by its memory edges, and it cannot alias a field or an array element.
The load keeps its control input, the last check in front of it, and a load from raw memory never follows a
test to a dominating one (LoadNode::depends_only_on_test_impl()), so it cannot move above its bounds check:
when loop predication hoists the checks of the scope, the load stays in the loop under the bounds check of
its iteration. A session that is closed while the loop runs is handled as for the scalar and unmasked
accesses, which already hoist the state check: the handshake of the close deoptimizes a frame of an nmethod
with scoped accesses (`nmethod::has_scoped_access()`, set when a `@Scoped` method is inlined, whatever
barriers it has). The trap sites that are left go through NoTrapSites::apply() like any other.

Where the checks stay in the loop: a segment that may be on or off the heap keeps the barrier; and the
profiled predicates are gone from a method after one of them failed, which happens to the loop with a masked
tail and a store: tailMaskedCopy takes a `profile_predicate` trap at the entry of its loop in every run and
is compiled again without profiled predicates, so the checks under a branch stay in its loop
(`intoMemorySegment(..., m)` stores without a mask when the mask is all true, and a check of that store,
hoisted for every iteration, fails for the last one, whose mask is partial). Those loops are where the rule
above matters: their null checks of the scope are implicit.

Tests: compiler/vectorapi/TestMaskedNativeSegmentChecks, id `ir`, has no MemBarCPUOrder in a masked sum and a
masked copy over native segments (it fails on the build before); id `close` runs the compiled loop on a
segment whose arena is closed before the call, closed by another thread during the loop (30 times), confined
to another thread, and with a mask that reaches past the end.

## 4. C1

Before: with the mode, C1 allocated xmm0-xmm15 only, on every processor. On AVX-512 processors that costs a
C1-compiled method with more than sixteen floating point values live at once a spill for each further one
(measured below: 6% and 14% on a kernel with twenty live doubles, run with C1 only). Now, on a processor with
xmm16-xmm31 and in a VM that has C2's handler blob for branch polls, C1 allocates all of them, under two rules
(cpu/x86/c1_FrameMap_x86.hpp, FrameMap::uses_losable_xmm_regs()):

- its loop polls test the polling word and branch to a stub (LIR_Assembler::safepoint_poll(),
  C1SafepointPollStub with `at_return` false), whose handler is the blob of C2's branch polls: it saves every
  register and returns to the poll;
- an operation with debug info that is not a call or a poll, and that either has an exception handler of the
  frame, or deoptimizes on its exception, destroys xmm16-xmm31 for the allocator, as a call destroys every
  register (LinearScan::pd_add_temps() adds a fixed-interval use of each at the operation); when the locals must
  stay readable for a debugger (`should_retain_local_variables()`), every such operation does.

Elsewhere (no AVX-512, or no C2 in the VM) C1 is as before: xmm0-xmm15 when the mode is on, faulting polls.

Argument. C1 code takes three kinds of hardware exception that the VM resumes: the loop poll, the implicit null
check and the implicit division by zero (the last two record an entry of the implicit exception table at the
operation, with its debug info: every such operation has a CodeEmitInfo). The poll no longer traps. An implicit
exception continues at the operation's stub (ImplicitNullCheckStub, DivByZeroStub), which calls the runtime, and
from there the frame goes on in exactly one of these ways:

- in an exception handler of the frame: then the operation has that handler in its XHandlers (they cover the
  scopes inlined around it), so no value live across it, which includes every value live into the handler, is in
  xmm16-xmm31; the values the handler reads arrive from the locations they had at the operation;
- in the interpreter after a deoptimization that does not throw: only where the debug info says
  `deoptimize_on_exception()` (ImplicitNullCheckStub then calls the predicate-failed trap); such an operation
  destroys the registers too, so its debug info names none of them;
- nowhere: the exception is thrown out of the frame, which is unwound (Runtime1's unwind handler), or the frame is
  deoptimized with the exception pending (JVMTI exception events, a frame invalidated while in the runtime) and
  its interpreter frames find no handler, as the compiled code found none, and are unwound. Nothing reads the
  frame's registers; a debugger that could read its locals on the way is the retained-locals case above.

Values in xmm16-xmm31 across a branch poll are saved and restored by the blob, and named by its oop map for a
deoptimization at the poll, as for C2. Across a call no register is live. A value used by the trapping operation
for the last time may be in xmm16-xmm31: it is dead after it. C1 keeps only floats and doubles in these registers
(no integer, pointer or mask), so the window of an unsafe access fault holds data there, as it does for C2.

Tests: TestExceptionXStateLossC1 (C1 only, and tiered) keeps twenty-four accumulators live across the polls of a
loop without handlers and across a null check with a handler in the frame; on a kernel that loses state it fails
without the mode (C1 then keeps its faulting polls and xmm16-xmm31).

## 5. The unsafe access window

### What happens

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

Options that were considered:

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

Decided: unsafe access errors stay asynchronous (d), and the damage is confined by an argument that is checked
and enforced. After a page error at an unsafe access, Java program order says nothing after the access should
run, so every write in the window up to the throw already has unspecified values in stock HotSpot (the failed
load's register keeps a leftover value); the kernel bug must only change values in writes that should not exist.
That needs three properties of a compilation with unsafe accesses (`nmethod::has_unsafe_access()`, the only code
such a fault is resumed in):

- **(a)** no oop or derived pointer is in a losable register;
- **(b)** every address and every bounds check is in general purpose registers, or is re-derived from state that
  is not losable;
- **(c)** a mask or index that comes back zeroed only shrinks or redirects accesses within what was checked.

Four counterexamples were found, and two more of the same kind as the fourth. Each is closed by a rule that the
compiler enforces and a debug or product check verifies.

**Where the window exists.** `Compile::has_unsafe_access()` is set by the scalar Unsafe intrinsics alone
(opto/library_call.cpp), which includes every compilation that inlines a scalar `MemorySegment` or `ByteBuffer`
access. The Vector API's segment loads and stores (opto/vectorIntrinsics.cpp) do not set it: a page error at such
an access in a method without a scalar unsafe access is not resumed but reported as a crash, as upstream does.
The window matters where vector code and a scalar segment or Unsafe access share a compilation: a vector loop with
a scalar tail loop, a header read in front of a vector loop. Every rule of this section applies in those
compilations only; in any other, losable state is simply accounted for at the trap sites (section 3).

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

Closed: in a compilation with unsafe accesses, integer and pointer spills stay out of xmm16-31
(Matcher::init_first_stack_mask()), and MachNode::rematerialize() refuses a losable input.

### Further routes that were examined

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

### Counterexample 3: a Vector API index check whose result is in a mask register (fails (b), x64 AVX-512)

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

### Counterexample 4: the check of a masked access reads a vector the fault can zero (fails (c))

x64 with AVX2 and AVX-512; AArch64 SVE by the same reading. The slow path of every masked load and store tests its
mask against the lanes that are in bounds: `AbstractMask.checkIndexByLane()` computes
`badMask = iotaScaled.compare(GE, broadcast(indexLimit))` (`checkIndex0()`, AbstractMask.java:292-354), ands it
with the access mask and throws if any lane is left. `iotaScaled` comes from the species' iota vector, a loop
invariant that C2 loads once and keeps in a vector register across the loop. After a resumed fault at a raw access
of the loop, that register comes back with its lanes above 128 bits zeroed (all of them in xmm16-31), and a zero
lane compares as in bounds wherever `indexLimit > 0`. The check then passes for lanes that are out of bounds, and
the masked access that follows takes the access mask, which is intact wherever the loop computes it again after
the fault. The frame runs on until its next poll, which in a strip-mined loop is many iterations away. A program
whose mask has a lane beyond the array or segment, which must get an IndexOutOfBoundsException, instead reads or
writes past the end. Same preconditions as counterexample 3 (a wrong program, an I/O error on a mapped page, a
compilation with unsafe accesses), but the value that is lost is a vector operand of the check, not its mask
result, and the check compares that vector with nothing the access uses, so "the mask loses the same lanes as the
vector it checks" does not help.

What the compiled code looks like (x64, -XX:UseAVX=2, PrintOptoAssembly of the masked store of
TestCheckedVectorsAtRawAccesses.laneCheckKernel): the lane numbers are a memory operand of a multiply by a
broadcast of the lane size, `vpmulld ymm2, ymm7, [iota]`, and it is that broadcast, `replicateI ymm7, #4`, which
the loop keeps in a register; zeroed above bit 128 it gives the lanes 4 to 7 the scaled number 0. Three things
decide whether the hole is reached, and a test of it has to get all three right. The limit of the lanes must not
be a vector of the loop too (zeroed with the lane numbers, 0 >= 0 reports every lane and the check fails safe).
The mask must not have every lane set: the Vector API tests `allTrue` first and checks such a mask as a whole
range, in general purpose registers. And the mask must have a clear lane below bit 128: the `allTrue` test reads
an all-ones vector of the loop, and with only upper lanes clear a zeroed all-ones vector reports "all true".

### The same shape for the index vector of a gather or scatter

`checkIndex(vix, length)` and the gather or scatter both read `vix`. The argument of (c) for them ("a zeroed lane
becomes index 0, which a passed check implies is in range") assumes that both read one copy. The allocator can
give them two: a use is pointed at a register that still holds the value (postaloc.cpp `elide_copy()`), or one
reads a register and the other a stack copy made before the fault. Then the check can see zeroed upper lanes
(index 0, in bounds) while the access uses the intact ones.

### The same shape in a check that the program computes itself

A check that user code computes in vector registers on a copy of the indices with another lane layout, and keeps
as a vector across a raw access: the indices widened from int to long lanes, compared as longs, the result kept
as a long-lane vector mask in xmm0-15 (AVX2) or as a vector in z0-z31, and tested after the access. Its lanes 2
and 3 sit above bit 128 and are lost, while the int indices of those lanes sit below it and are kept, so a failed
check of those lanes passes. The same for a mask turned into a vector (a blend, `VectorMask.toVector()`) and
compared again.

### The rule that closes 3, 4 and both shapes: nothing losable reaches a check across a raw access

> In a compilation with unsafe accesses, no mask and no vector that is in a losable register across a raw access
> is read afterwards by a reader that checks, directly or through the masks and vectors computed from it.

The values the rule follows are the masks (ideal register class RegVectMask) and the vectors (VecA to VecZ). A
reader of such a value is one of three kinds (`checks_values()`, `always_checks()` and CheckFlow::step_back(),
opto/noTrap.cpp):

- **A reader that defines a mask or a vector passes the question on**: its inputs are read for a check exactly
  if its result is. Mask logic, compares (vector to mask), blends, casts and conversions, masked loads and
  gathers, arithmetic, the allocator's copies and merges, Phis. This is what follows a check back to its
  operands: the test of counterexample 4 reads a mask, the mask is the result of a compare, and the compare reads
  the lane numbers. VectorStoreMask always reads for a check: its vector holds the mask itself (the payload of
  a boxed mask).
- **A reader that checks**: everything else that defines no mask or vector. Flags producers (VectorTest, that is
  `anyTrue`/`allTrue`, on masks and, with AVX2, on vectors), conversions of a mask to an integer (`toLong`,
  `trueCount`, `firstTrue`, `lastTrue`), the arguments of calls, returns. On x64 `anyTrue` and `allTrue` are
  themselves conversions to an integer in these compilations (step 0 below): the reader that checks is then the
  instruction that moves the bits of the mask to a general purpose register, and the compare of that register
  reads no mask and no vector.
- **A reader that does not check**: the vector stores and scatters (they store what the vector holds or, with a
  zeroed mask, nothing), and the arithmetic reductions and lane extracts. The integer or floating point result of
  a reduction or an extract is a value of the window like the vector it is computed from, and it is tested as an
  integer wherever it steers a checked access. The debug info of a safepoint, call or uncommon trap is no reader
  that checks either (see below).

Why this keeps (b) and (c):

- Every reader that checks sees a value that was not in a losable register at the fault: it was on the stack, in a
  general purpose register, or is computed afterwards from values of which the same holds, because the question
  is followed backwards through the operations. So a check gives the result it gives in stock HotSpot, on the
  operands the program computed. That closes counterexample 3 (the mask result), counterexample 4 (the lane
  numbers), and the check a program computes itself, whatever the lane layout of its vectors.
- For a gather or scatter, the check reads the index vector intact, whatever copy the access reads. If the check
  passes, every index is in range and the array is not empty; the access then reads either the same indices or,
  in lanes the fault zeroed, index 0, which is in range.
- A mask or vector that reaches no check can hold anything after a fault (zeroed lanes, or a value computed from
  them, such as all lanes for the negation of a lost mask). It is only ever a predicate or data. The masked access
  a predicate governs is one whose mask the compilation never tests: the Vector API tests the mask of an access
  exactly when the access is not wholly in bounds (`checkMaskFromIndexSize`, in the slow path of each masked load
  and store, on the same mask value that the access of that path takes), so an access with an untested mask is
  the access of the fast path, whose lanes are all in bounds by the scalar test of its offset. Any mask is safe
  for it.
- What changes for values is that a predicate or a vector can differ from the program's where a reload would have
  restored it: lanes of a value of the window dropped, passed through or zero, or a store of the window that does
  not happen or stores unspecified lanes in bounds.

**Debug info is no check.** A mask or vector in the debug info of a safepoint, call or uncommon trap is read only
if the frame is deoptimized there. Deoptimization turns it into a heap object, which every later reader sees
alike, and whatever runs after a deoptimization checks its own accesses: the interpreter, C1 code and the Java
bodies of the Vector API (the fallbacks of `VectorSupport`) by the bounds checks of the language, and other
compiled code by the checks it carries for its own intrinsic accesses, on the objects it is given. No check of the
Vector API is split from its intrinsic access by a deoptimization with the result of the check as state other
than in a mask, and an intrinsic access is only reached from compiled code of the method that also contains its
check. Independently, the pending InternalError is thrown where the interpreter takes over (`uncommon_trap_inner`
is a JRT_ENTRY, whose return to Java installs the asynchronous exception, and the interpreter's deoptimization
entries throw a pending exception before the next bytecode), except where the thread has asynchronous exceptions
blocked (Thread.exit, the virtual thread transitions).

**What the rule leaves out, deliberately.** A reduction or a lane extract is not a reader that checks. A program
that builds a check out of one (the maximum of its indices by `reduceLanes(MAX)`, compared as an integer) and
relies on it across a raw access is not protected: the integer it compares is computed from the lanes the fault
left. No access of the Vector API or of the language depends on such a check (each carries its own, which the rule
protects, or tests the integer it is given), so what such a program can lose is its own exception, or the bound of
a raw Unsafe access it guards that way, which has no bounds check by definition and is outside (b). Counting
reductions as checks would put a store of every vector accumulator into the loop of every auto-vectorized sum over
a memory segment, the common case, for the sake of that one.

### How the rule is enforced

0. **Before matching, the tests of masks read integers** (`NoTrapSites::test_masks_as_integers()`, called from
   `Compile::Optimize()` once the compilation is known to have unsafe accesses; x64 only,
   `NoTrapPlatform::tests_masks_as_integers()`). A test of all or any lanes of a mask is a VectorTest, whose
   result is the condition codes: a `kortest` of the mask register or, where masks are vectors, a `ptest` against
   a vector of ones. The condition codes cannot be kept across the instructions that the scheduler puts between a
   mask and its test (an add with a memory operand sets them, and so does the explicit null test of section 3),
   so the instruction that reads the mask for the test was always the last one in front of the branch, behind
   every raw access of its block. Each VectorTest is replaced by `VectorMaskToLong(mask)` and a compare of that
   long: equal to the bits of a mask with every lane set for `allTrue`, not zero for `anyTrue` (a `kmov`, or a
   `movmsk` of the vector, and a `cmp` with an immediate). It is the conversion behind `VectorMask.toLong()`, with
   the same bits. Nothing about the rule changes: the conversion defines no mask and no vector, so it is a reader
   that checks, as the VectorTest was, and a mask or a vector that reaches it across a raw access is found and
   protected like any other. What changes is that its result can cross an access: it is a long in a general
   purpose register, and these compilations keep integers out of every losable register (counterexample 1, and
   r16-r31 with APX). A VectorTest stays as it is where its shape is not the one the Vector API makes (a second
   input that is not the mask or a vector of ones, a Bool that is neither the test nor its negation) or where the
   conversion is not supported for the mask (more than 64 lanes; a vector mask of two lanes or of more than 256
   bits): it is then a reader that checks whose value comes from the stack or is computed again, as before.

   A tested mask that is made by bitwise operations from other masks is converted through them where one of
   those masks is tested itself, or is the constant with every lane set: the bits of each mask are converted
   and the operation is made on the longs (`toLong(a & b)` is `toLong(a) & toLong(b)`, bit for bit, also where
   masks are vectors and the bit of a lane is its sign). The slow path of a masked access tests whether any lane
   of its mask is out of bounds, `m.and(outOfBounds).anyTrue()`, and `intoMemorySegment(..., m)` tests the same
   `m` with `allTrue()` on the fast path: with the operation on the longs, both tests read one conversion of
   `m`, which step 1 puts right behind `m`, and `m` has no other reader that checks. Without it, the `and` of
   the slow path was a reader of `m` that defines a mask, behind every raw access of the loop, and `m` was
   kept on the stack on every iteration for it.

   The same is done for the other reader of a mask that every loop has: the debug info. A mask that a Java
   local holds is named by every trap behind its definition, as the booleans of its box
   (`VectorStoreMask(mask)`, a reader that always checks: a mask in the debug info can be the result of a
   check that the interpreter tests after the deoptimization). Those conversions sit in the blocks of the
   traps, behind the raw accesses of the iteration, so the mask of every iteration was kept on the stack for
   them. Where a test reads the bits of the mask, the booleans are made from those bits instead
   (`VectorLongToMask`, a `kmov` back into a mask register in the block of the trap, or the bytes themselves
   where masks are vectors): the trap reads the general purpose register that the test reads, and the mask
   has no reader behind the accesses. The mask that the trap describes is the one the program computed,
   bit for bit.
1. **Before register allocation, on the scheduled code** (`NoTrapSites::protect_checked_values()`, called from
   `Compile::Code_Gen()` after global code motion). `CheckFlow` is a backward dataflow over the masks and vectors,
   "reaches a check"; it finds the values that can be in a losable register across a raw access (a vector whose
   register class includes one; a mask, where the mask registers are not killed at every raw access) and reach a
   check afterwards. `protect_values()` then gives each reader of such a value that checks, or whose result
   reaches a check, a value the fault cannot have zeroed, and leaves the value in its register for the other
   readers:
   - a constant mask (MaskAll of a constant, the all-true mask of the full iterations of a loop with a masked
     tail): a copy of its definition right in front of the reader. On x64 the all-true mask of up to 16 lanes
     is a kxnor (cpu/x86/x86.ad, mask_all_true_evexI_LE16), so the copy needs no general purpose register;
   - a mask or vector whose instruction can be run again (no TEMPs, kills, control or memory input; inputs that
     are constants, integer or pointer values, and other masks and vectors: the mask of a loop iteration, a
     compare of the lane numbers with a broadcast of the lanes left): a copy of that instruction right in front of
     the reader. The integer inputs are never in losable registers in these compilations, and the masks and
     vectors among the inputs are then read for the reader's check, so they are found and protected in turn. The
     paths that do not check pay nothing;
   - any other mask or vector: a copy bound to the stack, made right after the definition, and one reload of it
     right in front of each reader. Neither node carries the copy flag, so the allocator's copy removal, which
     follows copies back to a register that still holds the value, cannot point the reader at the value's
     register, or at an earlier reload, again.

   In front of these three forms, on x64, a reader that checks and whose result is an integer (the bits of a
   mask for its test, by step 0; `toLong`, `trueCount`, `firstTrue`, `lastTrue`) is not given a copy at all: the
   reader itself is moved up, right behind the definition of the value, in its block or from a block that the
   definition dominates (move_check_behind_value()). Nothing losable then crosses the accesses for that check:
   the check has read the value before any raw access can follow, and its result waits for the compare in a
   general purpose register. The move is made wherever the reader can run there: it has no control input and
   reads no memory, every other input is defined at that place (in a block that dominates it, or earlier in the
   block), and what it kills besides its own TEMPs, which move with it, is the condition codes, which must then
   not be live at that place (set before it and read behind it). Where the value could be computed again for
   the reader instead, the move is made unless the definition runs more than twice as often as the reader (a
   test in a slow path of a loop keeps its copy there); where the value would be kept on the stack, the move
   always costs less, one instruction where there would be a store.

   And in front of all of these, on x64, for a value that is a Phi: the raw loads of its block are moved to the
   end of the block that immediately dominates it, in front of its branch, if every raw access of the block is
   a load that can be made there and that block does not run more often (hoist_accesses_above_merge()). The
   mask of an iteration of a loop with a masked tail (`S.indexInRange(i, n)`) is such a Phi: every lane on one
   side of a branch on the lanes left, the lanes left on the other. A raw read of the same iteration that the
   scheduler put behind the merge is one the Phi is live across, although the read does not depend on the
   branch; in front of the branch, the Phi does not exist yet. The Phi then needs nothing: no store on every
   iteration for a check in a slow path, and no check made behind it. A load can be made there if the
   scheduler could have put it there itself: its control, its memory state and its other inputs are defined
   there, the nodes of the block that compute its address (which read no memory and have no control) move with
   it, and what they kill besides their TEMPs is the condition codes, which are then not live in front of the
   branch and of the compare that sets them for it. An access is moved once.

   Two choices between the three forms are made by cost, and neither adds a form:
   - a value that can be computed again is kept on the stack instead where that costs less by the frequencies
     of the blocks (is_cheaper_on_stack()): a store where it is defined and a load for each reader, against
     its instruction for each reader plus, for each mask or vector among its inputs, that input's instruction
     for each reader or, if the input cannot be computed again, a load for each reader and a store where the
     input is defined. The mask of a loop with a masked tail, read for a check in a slow path, is still computed
     again there. A compare of a vector that the loop has just loaded, tested on every iteration, was computed
     again from a copy of that vector, stored and loaded on every iteration; now the compare's own result is
     stored and loaded;
   - where the masks are followed, a vector that cannot be computed again and is read, after the access, by an
     instruction of its block that makes a mask from its inputs alone (the same compare, where the scheduler
     put it behind the access) is not copied for that reader: the instruction is moved up, right behind the
     definition of the vector and in front of the access, with the copies of constants an earlier round put in
     front of it (move_in_front_of_accesses()). The next round finds its result, the mask, live across the
     access and keeps that on the stack: a few bytes, where the vector took a slot of its own size, 64 bytes
     for a zmm register in a frame that is aligned to 16. Where the masks are vectors (AVX2), the instruction that
     is moved is one that makes a vector from its inputs alone; and a value that was moved this way lets its own
     readers be moved behind it in the next round. With the checks that read integers, the chain ends in a general
     purpose register and nothing is kept on the stack: the vector is loaded, the compare that makes the mask
     follows it, the conversion to the long follows the compare, and only then comes the raw access that the
     scheduler had put in between.

   Argument. The form on the stack is the general one, sound for every value. A check that is moved behind its
   value reads the value itself, with no raw access between the definition and the read, so it reads what the
   program computed; its instruction has no memory and no control input, so it computes at the new place what
   it computed at the old one, from the same inputs; the test that follows reads an integer, which is in no
   losable register across the accesses in between (the debug check of the placement of integers and step 2
   below are run on the code as it then is). A raw load that is moved in front of a merge is made in a block
   between the earliest one its inputs allow and the one the scheduler chose, which is a placement the
   scheduler was free to make: the load reads the same memory state, a load changes nothing that the nodes it
   now precedes read, and a page error at it is reported as before, by an error that is thrown at a later
   poll. Neither move is trusted for the rule: after each, the dataflow is run again on the code as it then
   is, and whatever a move left or put in a losable register across a raw access on its way to a check is
   found and given one of the three forms. The instruction that is moved
   has no memory or control input, no TEMP and no kill, so it computes the same mask wherever its inputs are
   defined, and they are (checked: each is defined in another block, which dominates this one, or earlier in
   this block, or is such a constant that moves with it); every reader of the mask follows the old place of
   the instruction. The dataflow is run again after every round, on the code as it then is, and once more on
   the allocated code (step 2), so a move or a choice that left a value in a losable register across a raw
   access on its way to a check would be found like any other. A mask that was moved is always kept on the
   stack: computed again for its readers it would read the vector behind the access once more.
   A reader that follows the definition in its block with no raw access in between reads the value itself. For a
   Phi the copy or reload goes to the end of the predecessor block. Nothing can follow the access of an implicit
   null check in its block, so in these compilations an access that defines a mask or a vector is no candidate
   for one (opto/lcm.cpp).
2. **After register allocation, on the final code** (`NoTrapSites::apply()`): the same dataflow over the
   registers and stack slots that hold masks and vectors, each register as two places, one for the values a fault
   can lose there (a vector of more than 128 bits in xmm0-15, anything in xmm16-31, a mask register) and one for
   the others. If a losable place that is live across a raw access reaches a check, the compilation is abandoned
   and made again: for a mask with `C2Compiler::retry_kill_masks_at_every_raw_access()`, the unconditional kill
   of the matcher (`Matcher::kill_masks_at_raw_accesses()`), whose debug check is that no killed register is live
   across a raw access; for a vector with `C2Compiler::retry_avoid_vectors()`, a compilation without
   auto-vectorization and without Vector API intrinsics (the Java code of the Vector API runs instead), which has
   no vector to lose. This is a product check, not an assert: the allocator may stretch a value across an access
   after step 1 (the inputs of a flags producer it clones), and step 2 is what makes the rule hold whatever it
   did. No compilation was retried in any test, bench or PrintOptoAssembly log of the CI runs.

Platforms: on x64 with AVX-512 VL, BW and DQ every mask is an opmask register value and both masks and vectors
are followed. On AVX-512 without them and on AArch64 SVE, masks cannot be told from other values or their readers
have not been examined (p7 governs most SVE instructions without being an input of their nodes), so every raw
access of such a compilation kills the mask registers, or xmm16-31 too (NoTrapPlatform::raw_access_kill()), and
the vectors are followed. On AVX2 there are no mask registers; masks are vectors and are followed as such. p7 is
never killed: losing it only stops the accesses it governs.

What a protected value costs: for a constant mask, one kxnor in front of each reader that checks it; for a value
that can be computed again, its instruction once more in front of each such reader; for any other mask or vector,
one store to the stack after its definition and one load in front of each reader that checks it,
which in Vector API code is on the slow paths (an access that is not wholly in bounds) and at explicit tests. On
x64 a test of the lanes of a mask (`allTrue`, `anyTrue`) and a conversion of a mask to an integer cost nothing
where the reader can be made behind the mask: the same instruction, earlier. What is left in that case is what
the mask itself is made from: a vector that the loop keeps in a register across its raw accesses and that the
compare reads is made again in front of the compare, once per iteration (the limit of a compare, broadcast
from an integer; the lane numbers, reloaded from the stack, where masks are vectors). Where that vector is a
broadcast of a constant and the compare makes a mask register (AVX-512), there is no such vector: in a
compilation with unsafe accesses the compare is matched with the constant, which it reads from the constant
table as a memory operand (evcmp_con and evcmp_conL in cpu/x86/x86.ad; `X86NoTrap::matches_with_compare()`
lets the matcher do so although the debug info of every trap of the method reads the same vector, as it does
for a scalar constant). A value that no check reads costs nothing, whatever raw accesses it is live across.

### What holds without a rule

- AArch64 passes (a)-(c): `UseFPUForSpilling` is off by default there and, if set, spills into the D registers,
  the NEON low bits that the context keeps. The rematerializable extracts and mask nodes are the NEON-only ones
  (`UseSVE == 0`, cpu/aarch64/aarch64_vector.ad:6838-6935); the SVE ones have TEMPs (6896-6912, 6969-6970), and
  lane extracts take an immediate (5987-6000).
- Gathers: the Vector API checks every lane of the index vector and gathers with that same vector against
  `ARRAY_BASE`, with nothing added after the check (src/jdk.incubator.vector/share/classes/jdk/incubator/vector/
  IntVector.java:3116-3125, VectorIntrinsics.java:74-87). A zeroed lane becomes index 0, which is in range because
  at least one lane passed `0 <= index < length`, so `length >= 1`; the check itself reads the indices intact (the
  rule above).
- Masked loads and stores check only the active lanes and pass the same mask on (AbstractMask.java:266-288,
  IntVector.java:3060-3073). Zero means inactive for k registers, AVX2 vector masks and SVE predicates (including
  `p7`), so a zeroed mask only drops lanes. An empty array or segment: no active lane can pass the check, so a zeroed
  mask or index there still touches nothing.
- Contiguous Vector API and FFM accesses check offsets in GPRs and address `base + offset` in GPRs; mask-driven
  control flow (`anyTrue`, `allTrue`) only picks between paths that each carry their own checks.
- Raw Unsafe addresses that user code builds from vector lanes have no bounds check by definition, and stock HotSpot
  already lets the window write through an address built from the failed load; they are outside (b).

## 6. MXCSR: no FP exception is resumed in compiled code

What happened without the mode: native code returns to Java with FP exceptions unmasked in MXCSR; the next C2 FP
instruction that raises one traps, and the handler resets MXCSR and resumes it with the broken context
(os_cpu/windows_x86/os_windows_x86.cpp:166-198). The call stub sets the standard MXCSR on entry from native code
(cpu/x86/stubGenerator_x86_64.cpp:259-265), but JNI returns and FFM downcalls did not, and the upcall stub loaded
the standard MXCSR only off Windows.

The invariant: compiled Java code never runs with a floating point exception unmasked, so no FP trap is taken and
resumed in it (os_cpu/windows_x86/os_windows_x86.cpp, handle_FLT_exception()). Only native code can change MXCSR,
so it is enough to establish the standard control bits at every point where control passes from native code to
Java code. Those points, and what each does with the mode on:

- **Return from a JNI call** (the native wrapper, cpu/x86/sharedRuntime_x86_64.cpp; the interpreter's native
  entry, cpu/x86/templateInterpreterGenerator_x86.cpp) and **from an FFM downcall with a transition**:
  MacroAssembler::restore_cpu_control_state_after_jni() loads the standard MXCSR, unconditionally.
- **Return from a critical FFM downcall**: the same load, in the stub (cpu/x86/downcallLinker_x86_64.cpp).
- **Entry from native code through the call stub** (JNI Call<Type>Method, thread start): the stub saves the
  caller's MXCSR and loads the standard one if the control bits differ (cpu/x86/stubGenerator_x86_64.cpp,
  unchanged by the mode).
- **Entry through an FFM upcall stub** (cpu/x86/upcallLinker_x86_64.cpp): stmxcsr to the frame, the standard
  MXCSR loaded if the control bits differ. On return the caller's value is loaded back **only if its control
  bits were not the standard ones** (this change).

Why the conditional restore on upcall return is correct. The Windows x64 convention makes the MXCSR control bits
(masks, rounding, DAZ, FTZ: 0xFFC0) preserved across calls and the status bits (0x3F) volatile, so the stub owes
its caller the control bits only. Between the stub's entry and its exit run Java code and UpcallLinker::on_entry()
and on_exit() (VM code compiled by MSVC, which follows the convention). Java code leaves the control bits
standard: the only code of its own that changes them is C2's rounding conversions
(cpu/x86/c2_MacroAssembler_x86.cpp) and the fmod stub (cpu/x86/stubGenerator_x86_64_fmod.cpp), which load the
standard value back before they end, and every return from native code inside the upcall is one of the points
above. So at the exit the control bits are the standard ones. If the saved value had standard control bits, the
register already holds what the caller is owed and the load would only rewrite the status bits, which the caller
may not rely on; if it had others, the stub loads the saved value as before. The test is on the saved value in
the frame, with the same mask as the entry (MacroAssembler::cmp32_mxcsr_std(): the status bits are ignored, also
with EnableX86ECoreOpts, whose standard value has them set). TestMXCSRRestore covers a caller with standard
control bits, one with exceptions unmasked, one with every exception masked and rounding toward zero, and both,
and runs once with EnableX86ECoreOpts.

Why the load after a downcall stays unconditional. The alternative, stmxcsr, compare, and ldmxcsr only if the
control bits changed, reads MXCSR on every call. On AMD (Zen 2 to Zen 5) stmxcsr is microcoded and takes 14 to 16
cycles, against 1 to 2 for ldmxcsr; on Intel it is the reverse (stmxcsr 1 cycle, ldmxcsr 3 to 5). Measured on
Zen 3, Zen 4 and Zen 5 the conditional form costs 3 to 10 ns per call where the unconditional load costs 0 to
0.3 ns (table below). No condition under which the load could be skipped without reading MXCSR is provable: the
convention obliges a native function to give the control bits back, but the functions this exists for are the
ones that do not (a library that unmasks exceptions and returns), and nothing tells them apart before the call.
Where the load sits in the stub (after the call, after vzeroupper, before ret) makes no difference.

Why the upcall entry keeps its stmxcsr. The stub must give its caller's control bits back, and the only way to
know them is to read them; a native caller may legitimately run with other bits (a rounding mode set around the
callback). This is what the stub does on every other platform already.

Windows/AArch64: the mode adds nothing to the JNI and FFM paths. HotSpot resumes no FP exception there
(handle_FLT_exception() is x64 only, os/windows/os_windows.cpp), so a trap enabled through FPCR by native code
ends the process as before instead of resuming with SVE state lost, and
MacroAssembler::restore_cpu_control_state_after_jni() touches FPCR only with RestoreMXCSROnJNICalls.

Measured (review8 kernels, ns/op, median of 9 forks per VM, all variants from one build on the same VM; "floor"
is the mode on with no MXCSR handling at all):

| CPU (Windows VM) | kernel | mode off | floor | load after call | store, compare, load if changed |
|---|---|---|---|---|---|
| EPYC 9V74, Zen 4 | downcall | 4.14 | 4.14 | 4.43 | 12.90 |
| EPYC 9V74, Zen 4 | critical downcall | 3.57 | 3.57 | 3.85 | 6.86 |
| EPYC 7763, Zen 3 | downcall | 5.06 | 5.07 | 5.39 | 15.04 |
| EPYC 7763, Zen 3 | critical downcall | 4.41 | 4.41 | 4.73 | 8.15 |
| EPYC 9V45, Zen 5 | downcall | 6.26 | 6.31 | 6.35 | 11.06 |
| EPYC 9V45, Zen 5 | critical downcall | 2.16 | 2.13 | 2.12 | 6.14 |

The JNI call is unchanged under every variant. The upcall kernel (qsort of two ints, 35 to 60 ns) varies by a
few ns between rounds on these VMs, which hides both the stmxcsr of the entry and the load the conditional
restore removes.

**Native code the VM itself calls on a Java thread (closed).** A JVMTI event callback posted from a runtime call
of compiled code (SampledObjectAlloc, VMObjectAlloc, the monitor and class events) returns to the VM through a C++
transition, not through a JNI wrapper, and the runtime call then returns to the compiled code: a callback that
unmasked FP exceptions left that code running with them unmasked. The transitions that bring a Java thread back
from such native code now load the standard MXCSR under the condition of the JNI return (RestoreMXCSROnJNICalls or
AvoidLosableStateAtTraps, x64 on every OS): ThreadStateTransition::restore_cpu_control_state_after_native()
(runtime/interfaceSupport.cpp, StubRoutines::x86::restore_mxcsr_after_native()), called by ~ThreadToNativeFromVM()
(which JvmtiJavaThreadEventTransition, the agent transitions of prims/jvmtiAgent.cpp, the library load of
prims/jvm.cpp and the class loader's library calls go through) and by ~JvmtiThreadEventTransition(). With these,
the list of points above is complete for user native code: it runs on a Java thread only under a JNI wrapper, a
downcall stub, or one of these transitions, and Java code is entered from it only through the call stub or an upcall stub.
Not covered, by choice: ZipLibrary's own transition (utilities/zipLibrary.cpp) calls the JDK's zip library only,
and JNI functions (ThreadInVMfromNative) are VM code called by native code, which returns to that native code.
The load is not on any JNI or FFM call path: the stubs are unchanged. TestMXCSRAfterJVMTICallback has an agent
whose SampledObjectAlloc callback, posted from the allocation slow path of a C2 method, returns with rounding
toward zero, and checks the sum that method computes next.

## 7. APX

- r30 and r31: on Windows C2 treated them as kept by C callees (cpu/x86/x86.ad:176-179). A resumed exception
  inside a leaf callee, such as an unsafe page error in an arraycopy stub, zeroes them under the caller. They are
  now save-on-call for C calls on Windows (cpu/x86/x86.ad reg_defs).
- r16-r31 in the unsafe window: see "Further routes" in section 5. Elsewhere they count as losable units at the
  trap sites.
- UseAPX is off by default.

## 8. Cost

### The integrated build

The three cost changes and the rule for masks and vectors together. JMH, three interleaved rounds of two forks on
each machine, the median of the rounds, as a percentage against the same build with
-XX:-AvoidLosableStateAtTraps. "before" is the branch at 2fc793f7a63 (before the cost changes), "now" the
integrated build. windows-2025 is the kernel that loses the state. Runs 46048738808
(https://github.com/Auties00/jdk/actions/runs/46048738808, the EPYC 9V74 column, measured before the values that
can be computed again were) and 46062686610 (https://github.com/Auties00/jdk/actions/runs/46062686610, the other
columns, the final code).

| kernel | EPYC 9V74 (AVX-512, 3 machines) before -> now | Xeon 8573C (AVX-512) | EPYC 9V45 (AVX-512, windows-2022) | EPYC 7763 (AVX2, 2 machines) |
|---|---|---|---|---|
| unsafeWithLiveMask | +98% -> +2% | +43% -> +5% | +74% -> +1% | +6% -> +6% |
| constantMaskPredicate | +62% -> +2% | +44% -> -1% | +93% -> +2% | 0% -> 0% |
| tailMaskedCopy | +17% -> +6% | +14% -> +2% | +22% -> +6% | +7% -> +8% |
| compareAcrossLoad | +4% -> +3% | -3% -> -4% | 0% -> +2% | +1% -> +1% |
| compareAcrossLoadMaskedStore | +11% -> +10% | +22% -> +7% | +13% -> +22% | +8% -> +5% |
| segmentMaskedSum | +7% -> +1% | +16% -> +11% | +1% -> -2% | +4% -> +9% |
| segmentCompareBlendStore | +7% -> +10% | +19% -> +5% | +8% -> -1% | +5% -> +7% |
| heapMaskedSum (control) | -2% -> +3% | 0% -> -2% | -4% -> -3% | -1% -> +1% |
| heapCompareBlendStore (control) | -1% -> +1% | -2% -> 0% | -12% -> -6% | 0% -> 0% |
| nullCheckVectorDeadOnNullPath | +2% -> -3% | 0% -> 0% | +2% -> +1% | +6% -> +3% |
| scalarFpPressure | -3% -> 0% | -7% -> -1% | -1% -> +2% | +1% -> 0% |
| JNI call | +1% -> +1% | -1% -> -1% | -4% -> 0% | 0% -> 0% |
| FFM downcall | +7% -> +7% | +3% -> +3% | -5% -> -4% | +6% -> +6% |
| FFM critical downcall | +8% -> +8% | +18% -> +18% | -2% -> -2% | +7% -> +7% |
| FFM upcall | +1% -> +1% | +5% -> +3% | +10% -> +10% | 0% -> 0% |

The gains of each change are in the integrated build: the mask kernels on AVX-512, the null check and spill
kernels, the upcall on the Xeon. The vectorPressure kernels of review10 are left out: they differ by 10% to 30%
between forks of one configuration, the unpatched base included, and the 9V45 column is one machine whose controls
move by as much as its kernels.

AVX2. With the first form of the rule for vectors (a copy on the stack for every protected value), tailMaskedCopy
was +16% and +14% on the two 7763 of run 46048738808 against +6% before: the reader that needs the mask of the
iteration really checks (the mask is live across the masked load of the segment and is tested in the slow path of
the store, and the lane numbers feed it), so the rule was not narrowed; the mask and the compare that makes it
are computed again in the slow path instead, and tailMaskedCopy is +8% (before +7%; the same build measures +2%
and +6% on AVX-512, where it was +14% to +22%). segmentMaskedSum stays at +9% on the 7763 against +4% before,
in both runs: its compilation has no unsafe access (no decision of the rule is logged for it), so the rule for
vectors does not touch it; the difference is where the branches of the loop fall (see "segmentMaskedSum on
AVX2" below).

On Linux (the same runs, mode forced, against the build before with the mode off): unsafeWithLiveMask +20% -> -4%,
constantMaskPredicate +35% -> -2%, tailMaskedCopy +14% -> +1% (two Xeon 6975P-C). No compilation of a bench kernel
was made again without vectors or with the kill at every raw access, and no value was left without a copy, on
Linux and on the four Windows machines (the decisions logs of run 46062686610).

Renaissance (run 46062686610; scrabble, future-genetic, mnemonics, fj-kmeans, dotty; three interleaved rounds of
12 repetitions, the second half of each, the median of the rounds, against the unpatched base on the same machine;
each cell is the mode off / the mode on):

| | Xeon 8573C | EPYC 7763 | EPYC 7763 | EPYC 9V45 (windows-2022) |
|---|---|---|---|---|
| dotty | 0% / -3% | +2% / +2% | +2% / +4% | +9% / -4% |
| fj-kmeans | 0% / +1% | -2% / -2% | -1% / 0% | +4% / +5% |
| future-genetic | 0% / 0% | 0% / +1% | +1% / 0% | +4% / +4% |
| mnemonics | 0% / -4% | +2% / +2% | +2% / -1% | -5% / -5% |
| scrabble | 0% / +2% | 0% / +4% | -1% / -1% | +2% / +7% |

The rounds of one configuration on one machine differ by up to 13% (dotty and scrabble on the 9V45), and the mode
off moves against the base by as much as the mode on does, so nothing here is outside the scatter. scrabble is the
one benchmark where the mode on is above the mode off on three of the four machines (by 2 to 5 points). mnemonics,
which was 7% to 9% slower than the branch before on two of three 9V74 in run 46048738808, is not slower than the
base on any machine of this run.

### The segment kernels: the checks of the segment, and what the mode adds to their loops

The kernels are those of review9: Vector API loops over native memory segments (segmentMaskedSum,
segmentCompareBlendStore, tailMaskedCopy, compareAcrossLoad, compareAcrossLoadMaskedStore,
constantMaskPredicate), a heap loop with a scalar Unsafe read (unsafeWithLiveMask) and two heap controls. All
but segmentMaskedSum and the controls are compilations with unsafe accesses (a scalar tail loop or a scalar
read), where the rule of section 5 applies as well.

What the mode added to their loops before, and why (PrintOptoAssembly under JMH, fastdebug builds of the branch
at e77590df934, with AVX2 and with AVX-512, on Linux and Windows):

- one explicit null check per access and iteration: the null check of the scope of the segment
  (`session.checkValidStateRaw()` in `ScopedMemoryAccess`), whose trap named every local of every frame because
  of the `finally` around it, and with them the accumulator, the vector to store and the mask of the iteration
  (section 3). It was the only explicit check of the hot blocks: 1 per iteration in segmentMaskedSum,
  segmentCompareBlendStore and tailMaskedCopy, 2 in compareAcrossLoadMaskedStore, none in compareAcrossLoad,
  whose checks were loop predicates already;
- in the compilations with unsafe accesses, the copies of section 5 for what
  `intoMemorySegment(..., m)` tests with `m.allTrue()` on every iteration, behind a raw load: the mask of the
  iteration in tailMaskedCopy (a mask register stored and loaded), and in compareAcrossLoadMaskedStore the
  vector that the mask is compared from (a zmm or ymm register stored and loaded, with a broadcast of the
  limit made again).

Four changes (sections 3 and 5):

1. the trap of a check that must throw names what the handlers it can reach read: the null check of the scope
   is implicit, in a loop or not;
2. a masked access to raw memory has no memory barrier, so the checks of a native segment leave the loop, as
   they did for the unmasked accesses. This changes the code without the mode as well: the loops lose four
   to ten loads and branches per iteration, and their main loops are unrolled;
3. a value that can be computed again is kept on the stack where that is cheaper;
4. a mask that is made from a vector behind a raw access is made in front of it, so that the mask, not the
   vector, is kept on the stack.

Null checks in the hot blocks of the loops, implicit + explicit, before -> now, and in brackets the same with
the mode off (the same counts with UseAVX=2 and 3, on Linux and on Windows): segmentMaskedSum 1+1 -> 0
(2 -> 0), segmentCompareBlendStore 3+1 -> 0 (4 -> 0), tailMaskedCopy 3+1 -> 3+0 (4 -> 3),
compareAcrossLoadMaskedStore 4+2 -> 0 (6 -> 0), compareAcrossLoad 0 -> 0. No null check of a loop is explicit
any more. What is left explicit is in front of the loops, once per call: the null checks that C2 folds into
the first load of a hoisted check, whose loop predicate trap names the initial value of an accumulator (2 in
segmentMaskedSum, 6 in compareAcrossLoad, as before).

JMH, three interleaved rounds of two forks per configuration on each machine; a cell is the median of the
forks of all the machines with that processor. "before" is the branch at e77590df934 (the bundles of run
46062686610), "now" this branch (the bundles of run 46095852250); each is compared with itself with
-XX:-AvoidLosableStateAtTraps (on Linux the mode is forced on and off). Bench run 46099646297
(https://github.com/Auties00/jdk/actions/runs/46099646297); the runs of the rounds before it, on other
machines (two Xeon 8573C, three EPYC 9V45, three Xeon 8370C, two more EPYC 7763), are 46088774917 (the memory
barrier alone) and 46095375455 (with the traps).

The cost of the mode: the mode on against the mode off of the same build, before -> now.

| kernel | EPYC 9V74, windows-2025 (2 machines) | Xeon 8573C, windows-2022 | EPYC 7763, windows-2025 (4 machines) | EPYC 9V45, Linux |
|---|---|---|---|---|
| segmentMaskedSum | +1% -> +0% | +10% -> -4% | +9% -> -0% | +2% -> -0% |
| segmentCompareBlendStore | +1% -> +3% | +3% -> +5% | +4% -> +4% | -0% -> +5% |
| tailMaskedCopy | +9% -> +1% | +10% -> +1% | +10% -> +10% | +6% -> +13% |
| compareAcrossLoadMaskedStore | +18% -> +45% | +7% -> +30% | +4% -> +8% | +12% -> +55% |
| compareAcrossLoad | +3% -> +2% | -2% -> -3% | +1% -> +1% | -0% -> +1% |
| constantMaskPredicate | +2% -> +2% | +1% -> +3% | +0% -> -0% | +2% -> +2% |
| unsafeWithLiveMask | +8% -> +8% | +7% -> +4% | +5% -> +6% | +1% -> +3% |
| heapMaskedSum (control) | -2% -> +2% | +1% -> -2% | +1% -> +1% | -8% -> -6% |
| heapCompareBlendStore (control) | -2% -> -1% | +7% -> -2% | -0% -> +0% | -2% -> +2% |

The times behind it: ns per call, the mode off / on, before -> now.

| kernel | EPYC 9V74, windows-2025 (2 machines) | Xeon 8573C, windows-2022 | EPYC 7763, windows-2025 (4 machines) | EPYC 9V45, Linux |
|---|---|---|---|---|
| segmentMaskedSum | 112 / 113 -> 35 / 35 | 127 / 140 -> 48 / 46 | 267 / 292 -> 84 / 84 | 71 / 72 -> 25 / 25 |
| segmentCompareBlendStore | 204 / 206 -> 54 / 55 | 219 / 225 -> 59 / 61 | 891 / 922 -> 536 / 555 | 128 / 128 -> 33 / 34 |
| tailMaskedCopy | 208 / 227 -> 145 / 146 | 239 / 262 -> 214 / 216 | 502 / 551 -> 420 / 461 | 134 / 142 -> 99 / 112 |
| compareAcrossLoadMaskedStore | 284 / 335 -> 63 / 91 | 314 / 337 -> 58 / 75 | 1095 / 1142 -> 561 / 605 | 190 / 212 -> 35 / 54 |
| compareAcrossLoad | 51 / 52 -> 51 / 52 | 47 / 46 -> 47 / 45 | 58 / 59 -> 58 / 59 | 28 / 28 -> 27 / 27 |
| constantMaskPredicate | 23 / 24 -> 23 / 24 | 19 / 19 -> 18 / 19 | 68 / 68 -> 68 / 68 | 12 / 13 -> 12 / 13 |
| unsafeWithLiveMask | 47 / 51 -> 46 / 50 | 56 / 60 -> 57 / 59 | 88 / 93 -> 87 / 93 | 30 / 31 -> 31 / 32 |
| heapMaskedSum (control) | 38 / 37 -> 37 / 38 | 51 / 52 -> 51 / 50 | 80 / 81 -> 80 / 81 | 25 / 23 -> 25 / 23 |
| heapCompareBlendStore (control) | 71 / 70 -> 71 / 70 | 66 / 71 -> 64 / 63 | 532 / 531 -> 530 / 531 | 33 / 32 -> 32 / 32 |

Against the unpatched base: the mode off / on, before -> now.

| kernel | EPYC 9V74, windows-2025 (2 machines) | Xeon 8573C, windows-2022 | EPYC 7763, windows-2025 (4 machines) |
|---|---|---|---|
| segmentMaskedSum | -0% / +1% -> -69% / -69% | +3% / +14% -> -61% / -63% | -0% / +9% -> -69% / -69% |
| segmentCompareBlendStore | +3% / +4% -> -73% / -72% | +2% / +5% -> -73% / -71% | +1% / +4% -> -40% / -37% |
| tailMaskedCopy | -1% / +7% -> -31% / -31% | -2% / +8% -> -12% / -11% | -1% / +9% -> -17% / -9% |
| compareAcrossLoadMaskedStore | +0% / +19% -> -78% / -68% | -1% / +6% -> -82% / -76% | -0% / +4% -> -49% / -45% |
| compareAcrossLoad | -1% / +2% -> +0% / +2% | -2% / -4% -> -2% / -6% | -0% / +1% -> -0% / +1% |
| constantMaskPredicate | -0% / +2% -> +0% / +2% | -2% / -1% -> -5% / -2% | -0% / -0% -> -0% / -0% |
| unsafeWithLiveMask | +2% / +10% -> -1% / +7% | -4% / +2% -> -3% / +1% | -0% / +5% -> -1% / +5% |
| heapMaskedSum (control) | +0% / -2% -> -2% / +0% | -3% / -2% -> -3% / -5% | +0% / +1% -> -0% / +1% |
| heapCompareBlendStore (control) | +3% / +1% -> +2% / +1% | +1% / +8% -> -2% / -4% | +0% / +0% -> -0% / +0% |

What the tables say:

- the checks of the segment were most of the time of the masked loops, with or without the mode: without the
  barrier the four masked kernels take 10% to 82% less time with the mode off, and 16% to 78% less with it on;
- segmentMaskedSum, the one kernel here without unsafe accesses, costs nothing with the mode (it was +1% to
  +10%): its loop is the same code with the mode on and off;
- compareAcrossLoad, constantMaskPredicate and the heap kernels are as before, within the scatter of the
  machines;
- in the three compilations with unsafe accesses and a masked store, what the mode adds is now the copies of
  section 5 alone (the hot loops differ from the mode off by these instructions, and here and there by a
  spill move of a general purpose register), and in loops that are up to five times shorter they weigh as
  much as before or more:
  - compareAcrossLoadMaskedStore, AVX-512: 17 to 28 ns per call where it was 22 to 51 ns, which is +30% to +55%
    of the shorter loop (it was +7% to +18% of the longer one); AVX2: 44 ns where it was 47 ns, +8% (+4%).
    Before the fourth change (round 2, bench run 46095375455), with the zmm register still stored and loaded,
    it was +81% on an EPYC 9V45 and +120% on two Xeon 8370C;
  - tailMaskedCopy, AVX-512: 1 to 13 ns per call where it was 8 to 23 ns, +1% to +13% (+6% to +10%); AVX2:
    41 ns where it was 49 ns, +10% (+10%). On the EPYC 7763 its forks fall on two levels with the mode on and
    off alike (6 of 24 forks near 320 to 340 ns, 18 near 420 to 460 ns): the mode is +6% on the first and
    +10% on the second;
  - segmentCompareBlendStore: 1 to 2 ns per call with AVX-512 and 19 ns with AVX2, +3% to +5% (+0% to +4%).

What is left, and what would remove it. With AVX-512 the loop of compareAcrossLoadMaskedStore has, per
iteration, a broadcast of the limit, a store of the mask to the stack and a load of it (it had a store and a
load of a zmm register in an unaligned slot and the broadcast); tailMaskedCopy has a store and a load of the
mask; segmentCompareBlendStore the broadcast. With AVX2, where a mask is a ymm vector, the stores and loads
are of 32 bytes, and an all-ones vector is made again for the test. All of it exists for one reader, the
`m.allTrue()` with which `intoMemorySegment(..., m)` picks between the store without a mask and the masked one. That test is not a
bounds check (both paths check their own bounds, the first in general purpose registers, the second with
`anyTrue()` on the lanes out of bounds in a slow path), and a wrong answer after a resumed fault would store
lanes in bounds that the mask excluded, which section 5 accepts for the stores of the window. But the rule
counts every flags producer as a reader that checks, so that a check a program computes itself is covered
whatever it is written with, and telling the two apart needs either trust in the shape of the Vector API (its
own bounds checks use `anyTrue()` only) or a look at what the branch leads to. That is a narrowing of the rule
of section 5, not of its enforcement, and it is not made here. Short of it, the test could be made in front of
the raw load and its result carried in a general purpose register (a `kmov` and a compare in place of the
store, the load and the `ktest`), which needs instructions that the matcher does not select today; or the raw
load that sits between the compare and the test could be scheduled first, which needs it not to be folded
into the add as a memory operand. unsafeWithLiveMask is as before (+3% to +8%): on the EPYC 7763 its +6% is
two spill moves of a general purpose register in the loop, which is otherwise the same instruction for
instruction with the mode on and off, and not a trap site.

### Renaissance scrabble

Round 17 had scrabble 2 to 5 points slower with the mode on than off on three of four machines, from three
runs of 12 repetitions per configuration. That is not a difference. Measured again with ten interleaved pairs
of runs per machine (30 repetitions each, the median of the last 15; the order within the pair alternates),
and six pairs with C2 alone (-XX:-TieredCompilation) and with C1 alone (-XX:TieredStopAtLevel=1), in the
bench runs of rounds 1, 2 and 4 of this branch (46088774917, 46095375455, 46099646297):

| processor (machines) | every tier | C2 alone | C1 alone | sd of the runs, mode off |
|---|---|---|---|---|
| EPYC 7763 (6) | -0.8% (-2.1% to +0.6%), 25 of 60 | -1.8% (-3.8% to +0.3%), 15 of 36 | -2.2% (-5.7% to +1.3%), 18 of 36 | 2.5% to 4.7% |
| EPYC 9V45 (3) | -0.2% (-1.1% to +0.8%), 15 of 30 | +0.0% (-1.6% to +1.7%), 7 of 18 | -0.2% (-5.8% to +5.7%), 7 of 18 | 1.8% to 2.7% |
| EPYC 9V74 (2) | -0.1% (-1.7% to +1.5%), 11 of 20 | +2.9% (+0.7% to +5.2%), 9 of 12 | -2.1% (-7.5% to +3.6%), 6 of 12 | 1.6% to 2.4% |
| Xeon 8370C (3) | -0.1% (-1.4% to +1.2%), 19 of 30 | -2.3% (-5.6% to +1.1%), 8 of 18 | +1.8% (-1.6% to +5.3%), 8 of 18 | 1.3% to 13.5% |
| Xeon 8573C (3) | +0.8% (-0.7% to +2.3%), 18 of 30 | +0.0% (-1.0% to +1.1%), 12 of 18 | +3.9% (-1.2% to +9.4%), 9 of 18 | 1.4% to 3.7% |
| all (17) | -0.2% (-0.9% to +0.4%), 88 of 170 | -0.7% (-1.7% to +0.4%), 51 of 102 | -0.1% (-2.1% to +1.9%), 48 of 102 | |

Each cell is the mode on against the mode off: the mean of the pairs, its 95% interval, and the number of
pairs in which the mode on was the slower one.

One run of a configuration differs from the next by 1% to 5% (the standard deviation of the ten runs with the
mode off on a machine), and now and then a whole run is 10% to 40% slower: 21 of 352 runs with the mode on and
22 of 352 with it off are more than 8% above the median of their configuration on their machine. Over all 170
pairs the mode on is 0.2% faster, between 0.9% faster and 0.4% slower, and the slower one in 88; one cell of
the fifteen by processor has an interval that leaves out zero (C2 alone on the EPYC 9V74, 12 pairs). The
fastdebug build counts no trap site of scrabble that the mode changes (0 of 823 implicit null checks explicit,
0 of 125 loop polls branch). So the mode costs scrabble nothing that these machines can show. With three runs
per configuration and this scatter, medians 2 to 5 points apart are what such a comparison gives every second
or third time; the mode on was above the mode off on three of the four machines of round 17 by chance.

### segmentMaskedSum on AVX2: +9% where it was +4%

On the two EPYC 7763 of round 17, segmentMaskedSum took 292 ns with the mode on at e77590df934 and 278 ns at
2fc793f7a63 (the base 265 ns, the mode off 267 ns), although the later build has one explicit null check per
iteration where the earlier one had two. The compiled code of the two builds was compared (fastdebug, the same
flags, the Intel JCC padding off as on AMD): the hot loop is the same, instruction for instruction and register
for register, except that the earlier build has the explicit null check of the segment in front of its klass
load (`test`, `jz`, 8 bytes) and the later one does not (the rule for null checks was narrowed in between);
the rule for masks and vectors does not touch the compilation, which has no unsafe access. So nothing was
added between the two builds: one check was removed, and every instruction behind it moved 8 bytes down. The
loop had nine conditional branches in about 220 bytes, and its time on Zen 3 follows where they fall. The three
of them again on four EPYC 7763 (bench run 46099646297; two rounds of two forks each; -XX:CodeEntryAlignment=64
and the loops aligned as the row says; the median and the range of the eight measurements, ns):

| loops aligned to | 2fc793f7a63, mode on | e77590df934, mode on | e77590df934, mode off |
|---|---|---|---|
| 16 bytes | 293 (286 to 295) | 274 (274 to 277) | 263 (262 to 276) |
| 32 bytes | 278 (278 to 282) | 275 (273 to 292) | 292 (291 to 297) |
| 64 bytes | 278 (277 to 283) | 292 (291 to 294) | 292 (291 to 294) |

Each build is on one of three levels (near 263, 276 and 292 ns) and changes level with the alignment alone; the
code without the mode goes from the lowest to the highest, 11%, when its loops are aligned to 32 bytes. With the
default alignment the earlier build was on the middle level, the later one on the highest and the mode off on
the lowest, which read as +4% and +9%. There was nothing to fix in the rules. The loop itself is gone with the
memory barrier (its checks are in front of it): the kernel takes 84 ns on the same processor, with the mode on
or off.

### MXCSR

Measured with the rules they belong to, in section 6.

### The trap site rules and C1

Runs on windows-2025 runners (kernel 26100.33828, which loses the state). Configurations: B the unpatched base, N
the notrap branch before the narrowed rules (round 11 bundles), C the branch with them, O the same with
-XX:-AvoidLosableStateAtTraps. JMH, average time, interleaved rounds, medians of the forks pooled by processor;
percentages are against B. Processors: AMD EPYC 7763 (AVX2), AMD EPYC 9V74 and 9V45 and Intel Xeon 8573C (AVX-512).

- Bench 1 (run 45913396018, six VMs, every review kernel, Renaissance, C1 only, UseFPUForSpilling, code size):
  C is the build of CI run 45905096158, before the C1 change.
- Bench 2 (run 45946446923, five VMs): the register pressure kernels with the unsafe read inside the compilation,
  the null check kernels again and with loops and entries aligned to 64 bytes.
- Bench 3 (run 45954224299, five VMs): C is the build of CI run 45948142657, with the C1 change: C1 only, review7
  and review10 with every tier, Renaissance.

#### Null checks

| kernel, processor | N | C | O |
| --- | --- | --- | --- |
| review7 nullCheckVectorLive, 7763 (bench 1, 6 medians) | +1.6% | -0.1% | -0.2% |
| the same, fork by fork (16 forks) | +1.6% | -0.1% | -0.1% |
| the same, bench 2 (6 medians) | +1.6% | +0.2% | +0.0% |
| nullCheckVectorLive, Xeon 8573C (bench 1) | +0.3% | +0.3% | -0.7% |
| the same, fork by fork | +2.3% | +0.9% | +0.1% |
| nullCheckVectorLive, 9V45 (bench 2, 3 medians) | +4.8% | +0.4% | +1.1% |
| nullCheckVectorLive, 9V74 (bench 1; 92 to 103 ns in every configuration) | -0.8% | +1.3% | +0.5% |
| review10 nullCheckVectorDeadOnNullPath, 7763 (bench 1) | +6.5% | +2.0% | +0.2% |
| the same, bench 2 | +5.5% | +1.3% | -0.1% |
| the same, loops aligned to 64 bytes | +3.0% | +1.0% | +0.1% |
| nullCheckVectorDeadOnNullPath, Xeon 8573C | +0.6% | -0.2% | -0.0% |
| nullCheckVectorDeadOnNullPath, 9V74 | -0.7% | -4.4% | -1.2% |

The explicit checks are gone from both kernels' loops (PrintOptoAssembly of C: nullCheckVectorLive has 2 explicit of
14 null checks, both outside the loop; N had 8; nullCheckVectorDeadOnNullPath has none of 12; N had 6). What is left
in nullCheckVectorDeadOnNullPath on the 7763, about 3 ns per call of 233, is in code that differs from O by the
form of one loop poll per call; it is there with 64 byte alignment too, and not on the other processors, nor in the
list walks, which take a branch poll on every iteration.

Sites that take the non-trapping form, counted by the fastdebug build of C (PrintOptoStatistics): -Xcomp -version,
0 of 201 to 203 implicit null checks and 0 of 31 loop polls; Renaissance scrabble, 0 of 816 to 1000 and 0 of 119 to 133;
fj-kmeans, 0 of 753 to 851 and 1 of 148 to 199; the stress test under -Xcomp on an AVX2 host, 0 of 10669 and 4
of 778.

#### The megamorphic kernel

Fork by fork, 16 forks per configuration and processor (ns): on the 9V74 every configuration, B included, falls on
the same three levels, about 2920, 3230 and 3400 to 3500; on the 7763 on 3650, 4030 and 4300; on the Xeon on 2110
and 2450. The medians move with the mix: bench 1 pooled N -4.7%, C -4.8%, O -4.9% on the 9V74, +2.0%, +3.0%, +2.2%
on the 7763; fork by fork N -0.1%, C -9.0%, O -4.3% on the 9V74 and N -9.2%, C +0.2%, O -6.0% on the Xeon. The
normal compilation of the kernel is the same code with the mode on and off; its OSR and JMH stub compilations
differ from run to run within one configuration. The +4.6% of the earlier run was that mix.

#### Loop polls, divisions, list walks

Bench 1, C against B: listWalkVectorLive -0.1% (7763), -0.1% (9V74), -0.1% (Xeon); uncountedListWalkVector +0.9%,
+0.5%, +0.1%; listWalkVector +/-0.4%; countedLoopVectorLive 0.0%, +0.1%, +0.7%; divisionVectorLive 0.0%, +0.2%,
-0.0%. O is within the same range.

#### Register pressure: rematerialization and spills

review10 vectorPressure (twenty-two live vectors, a call every few iterations, no unsafe access) and
vectorPressureUnsafeInside (the same with an unsafe read in the compilation). Both are multimodal from fork to fork
in every configuration (for example B on the 7763: 1855 to 2666 ns).

| kernel, processor | N | C | O |
| --- | --- | --- | --- |
| vectorPressure, 7763 (bench 2) | +25.5% | +3.0% | +1.4% |
| vectorPressure, 9V74 | -14.5% | +1.0% | +0.7% |
| vectorPressure, 9V45 | -29.6% | -0.8% | +1.0% |
| vectorPressure, Xeon 8573C (bench 1) | -30.0% | -2.4% | -2.3% |
| vectorPressureUnsafeInside, 7763 | +3.0% | +28.3% | -0.2% |
| vectorPressureUnsafeInside, 9V74 | -14.2% | -15.7% | +0.3% |
| vectorPressureUnsafeInside, 9V45 | -27.8% | -36.0% | -6.8% |
| scalarFpPressure, 9V74 / Xeon (bench 1) | -2.6% / -5.6% | -0.3% / +0.8% | -0.0% / +1.0% |
| scalarFpPressureUnsafeInside, 9V74 / 9V45 | -3.1% / -1.3% | -3.2% / -2.8% | +0.1% / +0.7% |

The rule that refuses rematerialization from a losable input changes what the allocator does under pressure, in
both directions: it made these kernels faster on AVX-512 and slower on the 7763. C applies it only in compilations
with unsafe accesses, so vectorPressure is as without the mode, and vectorPressureUnsafeInside behaves as N did.
gprPressureUnsafe with -XX:+UseFPUForSpilling: N +0.3%, -0.5%, +1.2%; C +0.5%, -0.3%, +1.1% (7763, 9V74, Xeon).

#### C1

C1 only (-XX:TieredStopAtLevel=1), twenty live doubles:

| kernel, processor | N | C before the C1 change (bench 1) | C (bench 3) | O |
| --- | --- | --- | --- | --- |
| scalarFpPressure, 9V74 | +6.1% | +6.3% | -0.1% | +0.1% |
| scalarFpPressure, Xeon 8573C | +15.5% | +16.2% | | +0.4% |
| scalarFpPressureUnsafeInside, 9V74 | +7.8% | | -1.2% | -0.3% |
| scalarFpPressureUnsafeInside, 9V45 | +12.3% | | -0.3% | +1.3% |
| scalarFpPressure, Xeon 8370C | +3.8% | | -3.2% | +0.3% |
| scalarFpPressure, 7763 (no xmm16-31) | -0.4% | -0.5% | | -0.3% |

The other C1 kernels of bench 3, C against B on the 9V74, the 9V45 and the Xeon 8370C: saxpy -4.8%, -12.9% and
-22.8%; fieldChain -2.0%, -5.8% and -9.2%; shortInner -0.2%, -3.6% and -40.8%; nullCheckScalar -0.3%, -7.9% and
-2.5%; listWalk -0.8%, -0.4% and -1.1%; hashMapGet -0.3%, -0.1% and -0.8%; megamorphic 0.0%, +2.0% and -0.2%;
countedSum -1.0%, +0.9% and +2.2% (N and O are within 4% of B on all of them): in C1 loops the branch poll is no
slower than the faulting one, and on the Intel processor it is faster.

#### Renaissance and code size

Renaissance (last five of ten repetitions, against O): bench 1, C: dotty +1.4%, +0.8%, +1.0%; fj-kmeans -0.4%,
+0.1%, -1.6%; future-genetic -1.5%, -0.1%, +0.1%; mnemonics +2.6%, +2.8%, -0.5%; scrabble +1.3%, +0.4%, +0.5%
(7763, 9V74, Xeon; four runs each, which scatter by 3% to 5%). Bench 3, C with the C1 change: dotty +0.8%, -2.6%,
-0.2%; fj-kmeans +0.5%, -0.5%, -0.7%; future-genetic +1.5%, 0.0%, -0.3%; mnemonics -2.0%, -5.1%, -0.7%; scrabble
-3.1%, -0.2%, +0.6% (9V45, 9V74, Xeon 8370C).

C2 code size of -Xcomp -XX:-TieredCompilation -version (bytes; B, N, C, O) on a 9V74: Serial 160344, 160344,
160424, 160344; G1 197816, 197896, 197816, 197896; Z 229784, 228728, 228544, 228728; Shenandoah 229768, 229880,
229880, 229880. On linux (mode off, notrap forced, cost-checks forced): Serial 145792, 145792, 145872; G1 178112,
178032, 178112; Z 205016, 205016, 205192; Shenandoah 207440 in all three.

What was looked at and found to be no cost of the shapes:

- The megamorphic kernel (review2, +4.6% on AVX-512): its compiled code has no vector, no losable state and no
  trap site that the mode changes, and the normal compilation is the same, instruction for instruction, with the
  mode on and off (PrintOptoAssembly of the round 11 fastdebug build). The kernel's time is multimodal from fork to
  fork in every configuration: its OSR and JMH stub compilations differ between two runs of one configuration as
  much as between configurations.
- UseFPUForSpilling is off by default on every EVEX processor (AMD from family 0x19 on, Intel with AVX2 or later),
  so the spill rule changes nothing by default.
- The explicit null check that remains is `test reg, reg` and `jz`, one fused instruction of 8 or 9 bytes, at the
  sites whose null path reads losable state. No form without it exists: the null path needs the registers the trap
  would lose. (The null path of a check that must throw reads less than its trap named in frames with handlers:
  see "A trap that must throw" in section 3 and the segment kernels below.)

### Masks at raw accesses

Before the rule of section 5, every raw access of a compilation with unsafe accesses killed the mask registers.

- **The +5-9% of the off-heap masked kernels of review9 is not the kill.** `segmentMaskedSum` and
  `segmentCompareBlendStore` are not unsafe compilations (see above). PrintOptoAssembly of their hot loops with the
  mode off and on (linux-x64 fastdebug, UseAVX=3 and UseAVX=2, run 45898446266) differs only in explicit null
  checks: two per iteration in `segmentMaskedSum` (the klass load of the segment, the load of
  `scope.owner`), four in `segmentCompareBlendStore`, one in `unsafeWithLiveMask` and `heapMaskedSum` (the
  range load of the heap array): the explicit null checks of section 3.
- **The kill is expensive because of where the allocator puts a killed mask.** A mask live range that crosses a
  kill cannot be colored. The kill is an interference, not register pressure (the mask registers count in
  neither pressure class), so PhaseChaitin::Split() has no place at which to split it: after the first failed
  round the live range is forced down everywhere (`_was_spilled1`), it lives in a stack slot for its whole
  extent, every use reloads it, and a Phi of it is a memory-to-memory copy. In `unsafeWithLiveMask` the live
  range is the all-true mask of the full iterations, which C2 computes once in front of the loop; the scalar read
  of the loop kills it on every iteration, although the loop's own masks are dead at the read.

Measured before the vectors were added to the rule (run 45961686077,
https://github.com/Auties00/jdk/actions/runs/45961686077; 30 jobs, all green):

Cost of the mode against the same kernels with the mode off, median of 3 rounds of 2 forks on the same machine,
before (the branch at 2fc793f7a63) -> after (the mask rule). Windows: O is the same build with the mode
off; Linux: F is the build before with the mode off.

| kernel | windows-2025 EPYC 9V74/9V45 (3 machines, median) | windows-2025 Xeon 8573C | Linux EPYC 9V74 | Linux Xeon 8573C |
|---|---|---|---|---|
| unsafeWithLiveMask | +84% -> +3% | +45% -> +1% | +93% -> +6% | +42% -> -1% |
| constantMaskPredicate | +63% -> 0% | +51% -> +2% | +62% -> 0% | +41% -> 0% |
| tailMaskedCopy | +20% -> +10% | +14% -> +17% | +16% -> +11% | +16% -> +20% |
| compareAcrossLoad | +4% -> +3% | -1% -> -1% | +1% -> +4% | -2% -> +1% |
| compareAcrossLoadMaskedStore | +15% -> +10% | +22% -> +21% | -4% -> -8% | +20% -> +20% |
| segmentMaskedSum | +6% -> +8% | +14% -> +15% | +6% -> +6% | +8% -> +12% |
| segmentCompareBlendStore | +9% -> +8% | +25% -> +20% | +6% -> +9% | +13% -> +14% |
| heapMaskedSum (control) | +2% -> 0% | -4% -> -5% | 0% -> -3% | -2% -> 0% |
| heapCompareBlendStore (control) | +1% -> +4% | +4% -> +11% | 0% -> +1% | 0% -> 0% |

unsafeWithLiveMask and constantMaskPredicate are the kernels the kill was measured on; their cost is gone. The
segment kernels of the Vector API never had a kill (their compilations are not marked as having unsafe accesses, see
below), and what they and tailMaskedCopy and compareAcrossLoadMaskedStore still pay is the explicit null checks of
the mode, 1 to 4 per iteration: the same +4% to +13% shows on AVX2 (EPYC 7763, 4 machines in run 45945893039 and one
in this run), where there are no mask registers and the mask rule changes no code. The heap controls move with their
forks (heapCompareBlendStore on the Xeon: 50.5, 52.1, 50.8 off; 56.4 +- 4.7, 52.8, 57.4 +- 6.5 after; 58.2 +- 5.2,
52.4, 52.8 before), and tailMaskedCopy and segmentCompareBlendStore have two speeds per fork in every build.

Renaissance (scrabble, future-genetic, mnemonics, fj-kmeans, dotty; 3 rounds of 12 repetitions, after against
before): -5.1% to +2.6% on the four AVX-512 machines, no benchmark slower on more than one of them. On the AVX2
machines scrabble is 0% to +6% slower after in the two rounds that run it second and equal in the round that runs it
first.

What a protected mask costs: for a constant mask, one kxnor in front of each reader that checks it; for any other
mask, one store to the stack after its definition and one load in front of each reader that checks it. A mask that
no check reads costs nothing, whatever raw accesses it is live across.

## 9. Where each part is, and its tests

- The flag: AvoidLosableStateAtTraps, diagnostic, on by default on Windows (share/runtime/globals.hpp).
- The pass: share/opto/noTrap.hpp, noTrap.cpp (NoTrapSites::apply(), run from PhaseOutput::Output() before sizing;
  a compilation it changes is not scheduled after register allocation). Platform sides: X86NoTrap
  (cpu/x86/x86.ad), AArch64NoTrap (cpu/aarch64/aarch64.ad).
- Explicit null checks: MachExplicitNullCheckNode (share/opto/machnode.hpp/.cpp); the MachNullCheck it replaces
  records no implicit exception (share/opto/output.cpp, fill_buffer() and FillExceptionTables()). x64 refuses
  flag-consuming candidates (share/opto/lcm.cpp).
- What a trap that must throw names in frames with exception handlers: GraphKit::add_safepoint_edges(),
  handlers_reached_by_throw() and handler_always_throws() (share/opto/graphKit.cpp), with the mode only.
- No memory barrier at a masked Vector API access to raw memory: LibraryCallKit::
  inline_vector_mem_masked_operation() (share/opto/vectorIntrinsics.cpp), whatever the mode.
- Branch loop polls: MachSafePointNode::is_branch_poll(), the safePoint instructs of both .ad files,
  C2LoopPollStub (share/opto/c2_CodeStubs.hpp, cpu/*/c2_CodeStubs_*.cpp), and the shared blob
  polling_page_branch_safepoint_handler (share/runtime/stubDeclarations.hpp, sharedRuntime.cpp), generated by
  generate_handler_blob() without the step over the poll and saving every vector register.
- The unsafe window: spill masks (share/opto/matcher.cpp, init_first_stack_mask()), rematerialization
  (MachNode::rematerialize(), has_losable_input()), APX exclusion (opto/chaitin.cpp), the masks and vectors that
  reach a check (NoTrapSites::protect_checked_values() from Compile::Code_Gen(), CheckFlow and protect_values()
  with is_cheaper_on_stack() and move_in_front_of_accesses()
  in opto/noTrap.cpp, the check on the allocated code in NoTrapSites::apply(), the retries in
  opto/c2compiler.cpp, the kill of the mask registers in Matcher::kill_masks_at_raw_accesses(), no implicit null
  check on an access that defines a mask or a vector in opto/lcm.cpp), comment at the handler
  (os/windows/os_windows.cpp, EXCEPTION_IN_PAGE_ERROR).
- AArch64: p7 always live with vectors, set after the stack bang (cpu/aarch64/macroAssembler_aarch64.cpp,
  verified_entry()); the SVE revision gate is skipped with the mode (os_cpu/windows_aarch64/vm_version_windows_aarch64.cpp).
- MXCSR: MacroAssembler::restore_cpu_control_state_after_jni() (JNI wrappers, the interpreter, FFM downcalls with
  a transition), the critical downcall (cpu/x86/downcallLinker_x86_64.cpp), the upcall stub
  (cpu/x86/upcallLinker_x86_64.cpp), ~ThreadToNativeFromVM and ~JvmtiThreadEventTransition
  (share/runtime/interfaceSupport.inline.hpp).
- r30/r31: cpu/x86/x86.ad reg_defs, save-on-call for C calls on Windows.
- C1: cpu/x86/c1_LinearScan_x86.hpp, c1_FrameMap_x86.hpp, c1_LIRAssembler_x86.cpp, c1_CodeStubs_x86.cpp. AOT code
  cache: share/code/aotCodeCache.hpp, x86 and AArch64 lists.
- Statistics of the trap sites: -XX:+PrintOptoStatistics (NoTrapSites::print_statistics()).
- Debug checks (NoTrapSites::apply() and verify_placement(), reg_split.cpp): every remaining implicit null check and
  faulting loop poll has no losable state live; no RegI/N/L/P value in a losable register (and in an unsafe
  compilation, none in r16-r31); no rematerialization from a losable input; no resumable raw access between a flags
  producer with a losable input and its use; where every raw access kills the mask registers, none is live
  across one. The check that no mask or vector that reaches a check is in a losable register across a raw access
  is in product builds too.

Tests (test/hotspot/jtreg):

- compiler/runtime/safepoints/TestExceptionXStateLoss (five collectors): vector accumulators live across trapping
  loop polls and a null check with a handler, with the stale header planted; `kernelHoisted` keeps a vector live
  into the handler that only the compiler knows. Fails on an affected kernel without the mode.
- compiler/runtime/safepoints/TestExceptionXStateLossC1: the same for C1 in xmm16-31, C1 only and tiered.
- compiler/runtime/safepoints/TestAvoidLosableStateAtTraps: the trap sites and their forms, with `invariantKernel`
  for the narrowed null check rule and `finallyKernel` for the trap that must throw in a frame with a `finally`.
- compiler/vectorapi/TestMaskedNativeSegmentChecks: no memory barrier at a masked access to a native segment
  (id `ir`), and the loop whose checks were hoisted when its segment is closed, before the call or by another
  thread during it, confined to another thread, or too short (id `close`).
- compiler/runtime/safepoints/TestMaskKillAtRawAccesses: masks live across raw accesses as predicates and as the
  results of checks; its `kills` id reads the decisions of protect_checked_values() from the log.
- compiler/runtime/safepoints/TestCheckedVectorsAtRawAccesses: the lane numbers of the check of a masked store
  (counterexample 4), the index vector of a gather, and a check kept in long-lane vectors; its `protects` id reads
  the decisions from the log, and its `fault` id takes a real page error at the raw access.
- runtime/jni/mxcsr/TestMXCSRRestore (JNI, FFM downcalls and upcalls) and TestMXCSRAfterJVMTICallback.

CI of the integrated branch: run 46062686610 (https://github.com/Auties00/jdk/actions/runs/46062686610), builds
of windows-x64, windows-aarch64 and linux-x64, release and fastdebug; on linux-x64 with the mode forced the
compiler tier1 groups, tier2, the vector tests, compiler/runtime/safepoints and jdk/foreign; on windows-2025 the
vector tests, jdk/incubator/vector, jdk/foreign, compiler/runtime/safepoints, runtime/jni/mxcsr, compiler/c1 and
the first compiler tier1 group; on windows-11-arm compiler/runtime/safepoints, runtime/jni/mxcsr and
compiler/vectorapi. All pass but compiler/vectorapi/TestMaskedNotAllOnes.java on windows-11-arm, which fails in
the matcher (`duplicating node that's already been matched`) with the mode off as well: it is JDK-8386697 of
mainline, open at the base of this branch.

CI of the changes to the null checks of the segment kernels and to the copies of section 5 (the last two
subsections of section 3, "Two choices" in section 5): run 46095852250
(https://github.com/Auties00/jdk/actions/runs/46095852250), 22 jobs, all pass: the builds of windows-x64 and
linux-x64, release and fastdebug; on linux-x64 with the mode forced the three compiler tier1 groups, tier2,
compiler/vectorapi and compiler/vectorization, compiler/runtime/safepoints and jdk/foreign; on windows-2025 the
same vector tests, jdk/incubator/vector, jdk/foreign, compiler/runtime/safepoints with runtime/jni/mxcsr and
compiler/c1, and the first compiler tier1 group; and the reproducers on windows-2025 (26100.33828, bug present,
an EPYC 7763 and an EPYC 9V45) and on windows-2022, which is not affected and where everything passes. On the
affected kernel TestExceptionXStateLoss passes with the mode (5 of 5 ids,
twice per machine) and fails with -XX:-AvoidLosableStateAtTraps and on the base; TestExceptionXStateLossC1
passes with the mode and fails without it on the AVX-512 machine; TestCheckedVectorsAtRawAccesses passes with
the mode (4 of 4 ids, twice per machine, `fault` with the page error) and its `fault` id fails with the mode
off, on cost-kmask and on the base. The same passed with the change to the memory barrier alone (run
46084046187) and with that and the change to the traps (run 46089976745). On windows-11-arm (run 46101545889,
https://github.com/Auties00/jdk/actions/runs/46101545889) compiler/runtime/safepoints with runtime/jni/mxcsr and
jdk/foreign pass, and compiler/vectorapi passes (114 tests, the new one included) but for
TestMaskedNotAllOnes.java, the failure of mainline that is there with the mode off as well (JDK-8386697).

The reproducers, in the same run and in run 46078200747 (https://github.com/Auties00/jdk/actions/runs/46078200747,
the final test), on windows-2025 (26100.33828, native probe: bug present; EPYC 9V74 and 9V45) and windows-2022
(not affected):

- TestExceptionXStateLoss passes with the mode (5 of 5 ids, twice per machine) and fails with
  -XX:-AvoidLosableStateAtTraps and on the unpatched base (a quarter to three quarters of the calls wrong);
  TestExceptionXStateLossC1 passes with the mode and fails without it on the AVX-512 machines.
- TestCheckedVectorsAtRawAccesses, id `fault`, takes a real page error at the raw access of each kernel, resumed
  in the compiled frame: a file mapped from a virtual disk that is then detached (it needs an administrator and
  runs only with -Dtest.xstate.vhd=true), with -XX:UseAVX=2 so that every machine keeps masks in vectors, and
  without on-stack replacement so that the kernels run compiled from their first iteration. On the affected
  kernel, on the build of cost-kmask (this branch without the rule for vectors), with the mode off and on the
  unpatched base, in 10 of 10 runs on two machines:
  - `laneCheckKernel` (counterexample 4): the masked store, whose mask has a lane outside the segment from the
    iteration of the page error on, passes its check in that iteration and in all 127 after it, and writes
    outside the segment;
  - `userCheckKernel` (a check kept in long-lane vectors across the read) lets the failed check pass.
  With the rule both stop at the iteration of the page error (4 of 4 runs), as every build does on windows-2022.
- `gatherKernel` cannot fail there: with one copy of the index vector, which is what this compilation has, a
  zeroed lane is index 0 and the gather stays in bounds; the case needs the allocator to give the check and the
  access different copies, which no source construction forces. Its guard is the id `protects`: on every kernel,
  the compilations of the three kernels have values that are live across the raw access and reach a check, each
  gets its copy, and nothing is compiled again; that id fails on the build without the rule.

## 10. History

The null checks of the segment kernels (the last two subsections of section 3, the two choices by cost in
section 5, the segment kernels, scrabble and segmentMaskedSum on AVX2 in section 8) were done on branch
JDK-8365196-cost-nullseg, on top of e77590df934.

The first audit found the design (explicit null test, explicit div0 test, branch loop poll, C1 off
xmm16-31) sound for loop polls, implicit null checks, div0, stack banging, deoptimization, handshakes, async
exceptions, GC barriers, stubs, C1, the interpreter, JFR and HotSpot's own C++ code, and three paths outside it:
unsafe raw accesses in C2 code (section 5), FP exceptions in compiled code (section 6) and APX r30/r31 across C
calls (section 7).

The scrub14 mechanism is gone from the branch: the Windows kernel classifier and its revision tables, the
stale-header scrub (TrapScrub, MachTrapScrubNode, the startup verification of L), the fallback cap on UseAVX and
MaxVectorSize, the Windows/AArch64 SVE gate, the flags ScrubExceptionXStateHeader, ExceptionXStateLength,
ForceExceptionXStateProtection and ExceptionXStateMitigation with the develop and verify flags, and their gtests and
jtreg tests. AvoidLosableStateAtTraps is the only mechanism; -XX:-AvoidLosableStateAtTraps gives compiled code with
no protection at all (the A/B of the repro). TestExceptionXStateScrub, which plants the stale header and keeps vector
state live across polls and a faulting null check, stays as TestExceptionXStateLoss, the regression test of the mode.
