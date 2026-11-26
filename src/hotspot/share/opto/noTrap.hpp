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
class RegMask;

// AvoidLosableStateAtTraps. On some Windows kernels, resuming from a hardware exception resets
// the processor state that the exception's context left out: on x64 the upper halves of the
// vector registers, xmm16-31, the opmask registers and the APX registers; on AArch64 the SVE
// state beyond the NEON registers. The invariant is that compiled code takes no hardware exception
// that the VM resumes while such state is live:
//
// - C2 trap sites that resume in the same frame, loop polls and implicit null checks, get a
//   non-trapping form wherever such state is live after register allocation (NoTrapSites): a
//   branch to a stub that saves every register, and an explicit test of the value.
// - C2 code takes no implicit div0 (it tests the divisor) and no min_int / -1 trap.
// - C1 code and the interpreter hold no such state.
// - Java code always runs with the standard MXCSR, so no FP exception is resumed in it.
// - Unsafe access faults are still resumed past the access with an InternalError pending (see
//   SharedRuntime::handle_unsafe_access()): the writes of that window have unspecified values in
//   any case, and nothing that steers an access is ever kept in such state while the access can be
//   reached: integer and pointer values are not spilled there, values are not recomputed from it,
//   and in compilations with unsafe accesses no live range uses a losable general purpose register,
//   and no mask that reaches a check is in a mask register across a raw access: the access kills
//   the mask registers (NoTrapPlatform::raw_access_kill()) wherever such a mask is live across it
//   (NoTrapSites::kill_masks_where_checked()), or every raw access does where masks cannot be told
//   from other values.
//
// A platform that supports the mode returns an implementation from Matcher::no_trap().
class NoTrapPlatform {
 public:
  // Units of register state that a resumed exception can lose, at most 64: the unit that a value
  // of ideal register class ideal_reg in register reg occupies, or -1 if the value is never lost.
  // A MachTemp passes 0 for ideal_reg: its width is unknown.
  virtual int losable_unit(OptoReg::Name reg, uint ideal_reg) const = 0;
  // Units live everywhere in C whatever the allocation (an all-true predicate register).
  virtual uint64_t always_live_units(const Compile* C) const = 0;
  // Whether unit is a general purpose register.
  virtual bool is_general_purpose_unit(int unit) const = 0;
  // Emits a test of the value in reg, a narrow oop when narrow, and a branch to is_null when it is
  // null. Its length does not depend on the label.
  virtual void emit_null_test(C2_MacroAssembler* masm, OptoReg::Name reg, bool narrow, Label& is_null) const = 0;
  // Whether that test kills the condition codes.
  virtual bool null_test_kills_flags() const = 0;
  // The registers an integer, pointer or narrow oop must not be spilled to, or null.
  virtual const RegMask* gpr_spill_exclusion() const = 0;
  // Whether an input with this register mask and ideal register class may be in a losable register.
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
  // values to their uses (NoTrapSites::kill_masks_where_checked()); otherwise every raw access of
  // a compilation with unsafe accesses kills them.
  virtual bool raw_access_kill_follows_masks() const = 0;
};

// The pass that gives C2 trap sites their non-trapping form after register allocation.
class NoTrapSites : public AllStatic {
 public:
  // Marks the loop polls at which losable state is live as branch polls and inserts an explicit
  // null check before each such implicit null check access. Returns whether it changed anything;
  // such a compilation is not scheduled after register allocation, which could move the checks.
  // Where kill_follows_masks(), it first repeats the search of kill_masks_where_checked() on the
  // allocated code, and if a mask that reaches a check is in a mask register across a raw access,
  // records C as failing with C2Compiler::retry_kill_masks_at_every_raw_access().
  static bool apply(Compile* C, const NoTrapPlatform* platform);

  // Whether the raw accesses of C that need the kill of NoTrapPlatform::raw_access_kill() are found
  // by following the mask values, instead of every raw access getting it in the matcher.
  static bool kill_follows_masks(const Compile* C, const NoTrapPlatform* platform);

  // After scheduling and before register allocation, where kill_follows_masks(): gives the kill to
  // each raw access across which a mask is live that reaches a check, which is any use other than
  // as the predicate of an instruction that computes a vector or stores one. A mask that reaches
  // only such uses may be lost at the access: a zero predicate drops lanes of a value of the
  // unsafe access window, or stores nothing. The allocator can still stretch a mask across an
  // access without the kill (the masks of a flags producer it clones), so apply() searches again.
  static void kill_masks_where_checked(Compile* C, const NoTrapPlatform* platform);
};

#endif // SHARE_OPTO_NOTRAP_HPP
