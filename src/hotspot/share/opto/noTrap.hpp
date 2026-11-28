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
 *
 */

#ifndef SHARE_OPTO_NOTRAP_HPP
#define SHARE_OPTO_NOTRAP_HPP

#include "memory/allStatic.hpp"
#include "opto/optoreg.hpp"
#include "utilities/globalDefinitions.hpp"

class C2_MacroAssembler;
class Compile;
class Label;
class Node;
class PhaseIterGVN;
class RegMask;

// AvoidLosableStateAtTraps. On some Windows kernels, resuming from a hardware exception resets
// the processor state that the exception's context left out: on x64 the upper halves of the
// vector registers, xmm16-31, the opmask registers and the APX registers; on AArch64 the SVE
// state beyond the NEON registers. The invariant is that compiled code takes no hardware exception
// that the VM resumes while such state is live:
//
// - C2 trap sites that resume in the same frame, loop polls and implicit null checks, get a
//   non-trapping form wherever such state is live, after register allocation, at the point the trap
//   would resume at (NoTrapSites): a loop poll, which resumes at the poll, becomes a branch to a
//   stub that saves every register where such state is live across it; an implicit null check,
//   which resumes at the null successor, becomes an explicit test of the value where such state is
//   live into that successor.
// - C2 code takes no implicit div0 (it tests the divisor) and no min_int / -1 trap.
// - C1 code and the interpreter hold no such state.
// - Java code always runs with the standard MXCSR, so no FP exception is resumed in it.
// - Unsafe access faults are still resumed past the access with an InternalError pending (see
//   SharedRuntime::handle_unsafe_access()): the writes of that window have unspecified values in
//   any case, and in a compilation with unsafe accesses, the only code such a fault is resumed in
//   (nmethod::has_unsafe_access()), nothing that steers an access is ever kept in such state while
//   the access can be reached: integer and pointer values are not spilled there, values are not
//   recomputed from it, no live range uses a losable general purpose register, and no mask or
//   vector that reaches a check is in a losable register across a raw access: each reader that
//   checks reads a copy that was kept on the stack, or is made in front of the access and leaves
//   an integer for its test (NoTrapSites::protect_checked_values()); for the masks, where they
//   cannot be told from other values, every raw access kills the mask registers instead
//   (NoTrapPlatform::raw_access_kill()).
//
// A platform that supports the mode returns an implementation from Matcher::no_trap().
class NoTrapPlatform {
 public:
  // Units of register state that a resumed exception can lose, at most 64: the unit that a value
  // of ideal register class ideal_reg in register reg occupies, or -1 if the value is never lost.
  // A MachTemp passes 0 for ideal_reg: its width is unknown.
  virtual int losable_unit(OptoReg::Name reg, uint ideal_reg) const = 0;
  // Units live everywhere in C whatever the allocation (an all-true predicate register), but for
  // the two exceptions below.
  virtual uint64_t always_live_units(const Compile* C) const = 0;
  // Whether those units are dead before n: n runs without reading them, and the code after it, if
  // n returns at all, sets them again before reading them (a call).
  virtual bool sets_always_live_units(const Node* n) const = 0;
  // Whether n, a node with code of its own, runs without reading those units.
  virtual bool ignores_always_live_units(const Node* n) const = 0;
  // Emits a test of the value in reg, a narrow oop when narrow, and a branch to is_null when it is
  // null. Its length does not depend on the label.
  virtual void emit_null_test(C2_MacroAssembler* masm, OptoReg::Name reg, bool narrow, Label& is_null) const = 0;
  // Whether that test kills the condition codes.
  virtual bool null_test_kills_flags() const = 0;
  // The registers an integer, pointer or narrow oop must not be spilled to in a compilation with
  // unsafe accesses, or null.
  virtual const RegMask* gpr_spill_exclusion() const = 0;
  // Whether an input with this register mask and ideal register class may be in a losable register
  // (a compilation with unsafe accesses recomputes no value from such an input).
  virtual bool is_losable_input(const RegMask& mask, uint ideal_reg) const = 0;
  // The registers no live range of C may use, or null: the losable general purpose registers in a
  // compilation with unsafe accesses.
  virtual const RegMask* allocation_exclusion(const Compile* C) const = 0;
  // The registers that a raw memory access of a compilation with unsafe accesses kills, or null:
  // those that can carry the result of a check (a mask) while losing more of it than the vector it
  // checks keeps, so that no such value is live across an access whose fault is resumed.
  virtual const RegMask* raw_access_kill() const = 0;
  // The units of raw_access_kill(), for the debug check that none is live across a raw access.
  virtual uint64_t raw_access_killed_units() const = 0;
  // Whether every value that can be in a register of raw_access_kill() has the ideal register
  // class Op_RegVectMask. The raw accesses that need the kill are then found by following the mask
  // values to their uses (NoTrapSites::protect_checked_values()); otherwise every raw access of
  // a compilation with unsafe accesses kills them.
  virtual bool raw_access_kill_follows_masks() const = 0;
  // Whether a compilation with unsafe accesses tests its masks as integers: the test of all or any
  // lanes of a mask reads the bits of the mask in a general purpose register, which no resumed
  // fault loses, instead of setting the condition codes from the mask (see
  // NoTrapSites::test_masks_as_integers()), and a check whose result is an integer is made right
  // behind the mask or vector it reads, in front of the raw accesses, where it can be (see
  // NoTrapSites::protect_checked_values()).
  virtual bool tests_masks_as_integers() const = 0;
  // Whether m, an input of the Binary binary of a vector compare, is matched as a part of the
  // compare even where other nodes read it, which then read a copy of their own in a register:
  // the vector of a constant, which the compare reads from the constant table instead of from a
  // register that a resumed fault can zero (the debug info of every trap of a method names such a
  // vector, where the method keeps it in a local).
  virtual bool matches_with_compare(const Node* binary, const Node* m) const = 0;
};

// The pass that gives C2 trap sites their non-trapping form after register allocation.
class NoTrapSites : public AllStatic {
 public:
  // Marks the loop polls across which losable state is live as branch polls and inserts an explicit
  // null check before each implicit null check access whose null successor losable state is live
  // into. Returns whether it changed anything;
  // such a compilation is not scheduled after register allocation, which could move the checks.
  // In a compilation with unsafe accesses, it first repeats the search of protect_checked_values()
  // on the allocated code, and if a mask or a vector that reaches a check is in a losable register
  // across a raw access, records C as failing with
  // C2Compiler::retry_kill_masks_at_every_raw_access() or C2Compiler::retry_avoid_vectors().
  static bool apply(Compile* C, const NoTrapPlatform* platform);

#ifndef PRODUCT
 private:
  // The trap sites of every compilation, and those that got their non-trapping form.
  static volatile int _null_checks;
  static volatile int _explicit_null_checks;
  static volatile int _loop_polls;
  static volatile int _branch_polls;

 public:
  static void print_statistics();
#endif

  // Whether the raw accesses of C that need the kill of NoTrapPlatform::raw_access_kill() are found
  // by following the mask values, instead of every raw access getting it in the matcher.
  static bool kill_follows_masks(const Compile* C, const NoTrapPlatform* platform);

  // Before matching, in a compilation with unsafe accesses: replaces every test of all or any lanes
  // of a mask (VectorTest, whose result is the condition codes) by the bits of the mask as a long
  // (VectorMaskToLong) and a compare of that long. The condition codes cannot be kept across the
  // instructions between a mask and its test, a long can: protect_checked_values() moves the
  // conversion to the long in front of the raw accesses that the scheduler put between the mask
  // and its test, so that the test reads a general purpose register across them and neither the
  // mask nor a vector it is made from is read after them for the test.
  static void test_masks_as_integers(Compile* C, PhaseIterGVN& igvn, const NoTrapPlatform* platform);

  // After scheduling and before register allocation, in a compilation with unsafe accesses: finds
  // the masks and vectors that can be in a losable register across a raw access and reach a check
  // afterwards, which is any reader that can let them decide something (a test, a conversion of a
  // mask to an integer), directly or through the masks and vectors computed from them, and gives
  // each such reader a copy that was kept on the stack, or makes the reader right behind the value
  // where its result is an integer (the bits of a mask for its test, see test_masks_as_integers()),
  // so that a general purpose register crosses the accesses. A value that reaches no check may be
  // lost at the access: its lanes are values of the unsafe access window, and a zero predicate
  // drops lanes of such values or stores nothing. A mask that cannot be given a copy keeps the kill of
  // the mask registers at the accesses it is live across; for a vector, C is recorded as failing
  // with C2Compiler::retry_avoid_vectors(). The allocator can still stretch a value across an
  // access (the inputs of a flags producer it clones), so apply() searches again.
  static void protect_checked_values(Compile* C, const NoTrapPlatform* platform);
};

#endif // SHARE_OPTO_NOTRAP_HPP
