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

#ifndef SHARE_OPTO_TRAPSCRUB_HPP
#define SHARE_OPTO_TRAPSCRUB_HPP

#include "opto/optoreg.hpp"
#include "utilities/growableArray.hpp"

class C2_MacroAssembler;
class Compile;
class MachNode;
class Node;

// On some systems, resuming from a hardware exception can reset processor state that the
// context of the exception left out, when a stale header lies where the system builds the
// context: on x86, under some Windows builds, the upper halves of the vector registers, the
// registers only AVX-512 adds and the opmask registers. C2 code then keeps that place clear
// wherever a trap site at which such state is live can be reached. A platform that needs this
// returns an implementation from Matcher::trap_scrub(); the others return null.
class TrapScrubPlatform {
 public:
  // Units of register state that a resumed exception can lose, at most 64: the unit that a value
  // of ideal register class ideal_reg in register reg occupies, or -1 if the value is never lost.
  // A MachTemp passes 0 for ideal_reg: its width is unknown.
  virtual int losable_unit(OptoReg::Name reg, uint ideal_reg) const = 0;
  // Whether n, besides calls and the method and exception entries, may write deep below the
  // stack pointer and then let the frame's code continue.
  virtual bool is_writer(const MachNode* n) const = 0;
  // Whether the scrub goes right before each trap site instead of after each writer.
  virtual bool at_traps() const = 0;
  // Whether calls are left without a scrub (testing the checks below).
  virtual bool omit_after_calls() const = 0;
  // Whether to check, right before each trap site, that the place is clear.
  virtual bool verify() const = 0;
  // Emit the scrub: no register used and condition codes kept, unless free_scratch, where the
  // registers a call does not preserve hold nothing (right after a call, or at a block that only
  // the call and its empty exception handlers reach).
  virtual void emit_scrub(C2_MacroAssembler* masm, bool free_scratch) const = 0;
  virtual void emit_check(C2_MacroAssembler* masm) const = 0;
};

// The pass that places MachTrapScrubNodes after register allocation (see TrapScrubPlatform).
class TrapScrubs : public AllStatic {
 public:
  // Inserts the scrubs into C's CFG. Returns whether a trap site needed protection; if not, nothing
  // was inserted, and the out of line stubs of the compilation need no scrub either. When
  // hot_traps is not null, appends the trap sites the scrubs protect to it, for verify().
  static bool insert(Compile* C, const TrapScrubPlatform* platform, GrowableArray<Node*>* hot_traps);
#ifdef ASSERT
  // A self-check of the insertion: asserts that, in the code as laid out after scheduling, no
  // writer reaches one of hot_traps without passing a scrub, and that no scrub sits in a block
  // that is never executed. It shares the pass's liveness, trap sites and writers, so it cannot
  // find a mistake in those.
  static void verify(Compile* C, const TrapScrubPlatform* platform, const GrowableArray<Node*>* hot_traps);
#endif
};

#endif // SHARE_OPTO_TRAPSCRUB_HPP
