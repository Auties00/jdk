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

#include "libadt/vectset.hpp"
#include "memory/resourceArea.hpp"
#include "opto/block.hpp"
#include "opto/compile.hpp"
#include "opto/machnode.hpp"
#include "opto/matcher.hpp"
#include "opto/noTrap.hpp"
#include "opto/opcodes.hpp"
#include "opto/regalloc.hpp"
#include "opto/type.hpp"
#include "runtime/atomicAccess.hpp"
#include "utilities/ostream.hpp"

// After register allocation only physical registers matter, so the liveness of the losable state
// is a backward dataflow over the platform's units (at most 64, one word per block).
//
// A loop poll that traps is resumed at the poll, so what counts at it is everything live across
// it, its inputs (its TEMPs included) and the node itself, since the fault can happen in the middle
// of its code.
//
// An implicit null check that traps is not resumed at the access: the handler continues at the null
// successor of the block (the continuation of the implicit exception table, see
// PhaseOutput::FillExceptionTables()), and neither the access nor the not-null successor runs. So
// what counts at it is what is live into the null successor: a value that only the access or the
// not-null path reads is never read after the trap. That includes the values the debug info of an
// uncommon trap names, which are inputs of its call, and the values an exception handler of the
// same frame reads.
//
// The units the platform keeps live everywhere count at both, with one exception: they do not count
// at an implicit null check whose null successor sets them again before anything can read them,
// which is the case of a null successor that only moves general purpose values and calls (an
// uncommon trap, which never returns, or a call after which compiled code sets them again).

// Whether n is a machine node without operands whose ideal_reg() would read the operand it does not
// have: the MachIdealNodes other than those that define their own (spill copies, merges, the
// constant base, null checks).
static bool defines_no_operand_value(const Node* n) {
  return n->is_Mach() && n->as_Mach()->_opnds == nullptr && !n->is_MachSpillCopy() && !n->is_MachMerge() &&
         !n->is_MachConstantBase() && !n->is_MachNullCheck() && !n->is_MachTemp();
}

// The ideal register class of the value n defines, or 0 when it defines none.
static uint value_ideal_reg(const Node* n) {
  return defines_no_operand_value(n) ? 0 : n->ideal_reg();
}

// The unit of losable state the value defined by n occupies, or -1.
static int unit_of(const NoTrapPlatform* platform, PhaseRegAlloc* ra, const Node* n) {
  if (n == nullptr || n->_idx >= ra->node_regs_max_index()) {
    return -1; // created after register allocation without a register (prolog, epilog)
  }
  OptoReg::Name reg = ra->get_reg_first(n);
  if (!OptoReg::is_reg(reg)) {
    return -1;
  }
  if (defines_no_operand_value(n)) {
    return -1;
  }
  int unit = platform->losable_unit(reg, n->is_MachTemp() ? 0 : value_ideal_reg(n));
  assert(unit < 64, "at most 64 units");
  return unit;
}

static uint64_t bit(int unit) {
  return unit < 0 ? 0 : (uint64_t)1 << unit;
}

static bool is_loop_poll(const Node* n) {
  return n->is_MachSafePoint() && !n->is_MachCall();
}

#ifdef ASSERT
static bool reads_flags(const Node* n) {
  for (uint k = 1; k < n->req(); k++) {
    if (n->in(k) != nullptr && n->in(k)->ideal_reg() == Op_RegFlags) {
      return true;
    }
  }
  return false;
}

static bool is_gpr_ideal_reg(uint ideal_reg) {
  return ideal_reg == Op_RegI || ideal_reg == Op_RegN || ideal_reg == Op_RegL || ideal_reg == Op_RegP;
}

// Checks what the unsafe access window relies on (see NoTrapPlatform): in a compilation with
// unsafe accesses, no integer, pointer or narrow oop is in a losable register, be it a floating
// point or vector register (a spill) or a general purpose one. A compilation without unsafe
// accesses has no such window: there these values are losable state like any other, which the trap
// sites they are live at account for.
static void verify_placement(Compile* C, const NoTrapPlatform* platform) {
  if (!C->has_unsafe_access()) {
    return;
  }
  PhaseCFG* cfg = C->cfg();
  PhaseRegAlloc* ra = C->regalloc();
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    for (uint j = 0; j < block->number_of_nodes(); j++) {
      Node* n = block->get_node(j);
      if (n->_idx < ra->node_regs_max_index() && is_gpr_ideal_reg(value_ideal_reg(n))) {
        OptoReg::Name reg = ra->get_reg_first(n);
        if (OptoReg::is_reg(reg)) {
          int unit = platform->losable_unit(reg, value_ideal_reg(n));
          if (unit >= 0) {
            n->dump();
          }
          assert(unit < 0, "an integer or pointer value in a register that a resumed exception can lose");
        }
      }
    }
  }
}
#endif // ASSERT

// Whether the units the platform keeps live everywhere are dead at the start of block: the code
// from there sets them again before any of it can read them. Only straight-line code is followed,
// and any node that is not known to leave them unread ends the search.
static bool always_live_units_dead_into(const NoTrapPlatform* platform, const Block* block) {
  for (int blocks = 0; blocks < 4; blocks++) {
    for (uint j = 0; j < block->number_of_nodes(); j++) {
      const Node* n = block->get_node(j);
      if (n->is_Region() || n->is_Phi() || n->is_MachProj() || n->is_MachTemp() || n->is_MachGoto()) {
        continue; // no code, or a jump
      }
      if (platform->sets_always_live_units(n)) {
        return true;
      }
      if (!platform->ignores_always_live_units(n)) {
        return false;
      }
    }
    if (block->_num_succs != 1) {
      return false;
    }
    block = block->_succs[0];
  }
  return false;
}

// A branch poll tests the polling word of the thread and does not read the address of the polling
// page, which the poll took as an input for its faulting form. When that address is a load off the
// thread in the block of the poll, with no other use, the load is removed, so that the branch poll
// costs no more instructions than the faulting one. The poll takes the thread, the address of the
// removed load, in its place: an input whose register holds its value at the poll as it did at the
// load (the oop maps are built from the inputs of every node, BuildOopMaps), and which the code of
// the branch form does not read.
static void remove_poll_address(Compile* C, MachSafePointNode* poll) {
  PhaseCFG* cfg = C->cfg();
  const uint address_index = poll->oper_input_base();
  if (poll->num_opnds() < 2 || poll->req() <= address_index) {
    return; // no polling page operand
  }
  Node* address = poll->in(address_index);
  // A single use rules out projections and an implicit null check on the load.
  if (address == nullptr || !address->is_Mach() || address->outcnt() != 1 ||
      address->as_Mach()->ideal_Opcode() != Op_LoadP) {
    return;
  }
  Block* block = cfg->get_block_for_node(poll);
  if (block == nullptr || cfg->get_block_for_node(address) != block) {
    return;
  }
  Node* thread = nullptr;
  for (uint k = address->as_Mach()->oper_input_base(); k < address->req(); k++) {
    Node* in = address->in(k);
    if (in == nullptr) {
      continue;
    }
    if (thread != nullptr || !in->is_Mach() || in->as_Mach()->ideal_Opcode() != Op_ThreadLocal) {
      return; // not a load off the thread alone
    }
    thread = in;
  }
  if (thread == nullptr) {
    return;
  }
  poll->set_req(address_index, thread);
  block->find_remove(address);
  cfg->unmap_node_from_block(address);
  address->disconnect_inputs(C);
}

bool NoTrapSites::apply(Compile* C, const NoTrapPlatform* platform) {
  PhaseCFG* cfg = C->cfg();
  PhaseRegAlloc* ra = C->regalloc();
  ResourceMark rm;

  DEBUG_ONLY(verify_placement(C, platform);)

  // The implicit null checks (Block::end() is not reliable for every block here, so every node is
  // looked at).
  uint block_count = 0;
  VectorSet null_check_accesses;
  GrowableArray<MachNullCheckNode*> null_checks;
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    block_count = MAX2(block_count, block->_pre_order + 1);
    for (uint j = 0; j < block->number_of_nodes(); j++) {
      Node* n = block->get_node(j);
      if (n->is_MachNullCheck()) {
        null_check_accesses.set(n->in(1)->_idx);
        null_checks.append(n->as_MachNullCheck());
      }
    }
  }

  // Liveness of the losable units, backwards to a fixed point.
  uint64_t* live_in = NEW_RESOURCE_ARRAY(uint64_t, block_count);
  for (uint i = 0; i < block_count; i++) {
    live_in[i] = 0;
  }
  auto live_out = [&](const Block* block) {
    uint64_t live = 0;
    for (uint s = 0; s < block->_num_succs; s++) {
      live |= live_in[block->_succs[s]->_pre_order];
    }
    return live;
  };
  auto step_back = [&](uint64_t live, const Node* n) {
    live &= ~bit(unit_of(platform, ra, n));
    for (uint k = 1; k < n->req(); k++) {
      live |= bit(unit_of(platform, ra, n->in(k)));
    }
    return live;
  };
  for (bool changed = true; changed; ) {
    changed = false;
    for (int i = (int)cfg->number_of_blocks() - 1; i >= 0; i--) {
      Block* block = cfg->get_block(i);
      uint64_t live = live_out(block);
      for (int j = (int)block->number_of_nodes() - 1; j >= 0; j--) {
        live = step_back(live, block->get_node(j));
      }
      if (live != live_in[block->_pre_order]) {
        live_in[block->_pre_order] = live;
        changed = true;
      }
    }
  }

  // The trap sites at which losable state is live.
  const uint64_t always_live = platform->always_live_units(C);
  DEBUG_ONLY(const uint64_t raw_killed = C->has_unsafe_access() ? platform->raw_access_killed_units() : 0;)
  GrowableArray<Node*> polls;
  GrowableArray<Node*> accesses;
  NOT_PRODUCT(int loop_polls = 0;)
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    uint64_t live = live_out(block);
    for (int j = (int)block->number_of_nodes() - 1; j >= 0; j--) {
      Node* n = block->get_node(j);
#ifdef ASSERT
      // No mask that could carry a check's result is live across a raw access (see
      // Matcher::kill_masks_at_raw_accesses()).
      if (raw_killed != 0 && n->is_Mach() && Matcher::is_raw_memory_access(n->as_Mach())) {
        uint64_t across = live & raw_killed & ~bit(unit_of(platform, ra, n));
        if (across != 0) {
          n->dump();
        }
        assert(across == 0, "a mask register is live across a raw access of a compilation with unsafe accesses");
      }
#endif
      if (is_loop_poll(n)) {
        NOT_PRODUCT(loop_polls++;)
        uint64_t at_trap = always_live | live | bit(unit_of(platform, ra, n));
        for (uint k = 0; k < n->req(); k++) {
          at_trap |= bit(unit_of(platform, ra, n->in(k)));
        }
        if (at_trap != 0) {
          polls.append(n);
        }
      } else if (null_check_accesses.test(n->_idx)) {
        // The null successor is succs[0] of the block of the null check, which is the block of
        // its access (checked below).
        const Block* null_successor = block->_succs[0];
        uint64_t at_trap = live_in[null_successor->_pre_order];
        if (always_live != 0 && !always_live_units_dead_into(platform, null_successor)) {
          at_trap |= always_live;
        }
        if (at_trap != 0) {
          accesses.append(n);
        }
      }
      live = step_back(live, n);
    }
  }
#ifndef PRODUCT
  AtomicAccess::add(&_null_checks, null_checks.length());
  AtomicAccess::add(&_explicit_null_checks, accesses.length());
  AtomicAccess::add(&_loop_polls, loop_polls);
  AtomicAccess::add(&_branch_polls, polls.length());
#endif
  if (polls.is_empty() && accesses.is_empty()) {
    return false;
  }

  for (int i = 0; i < polls.length(); i++) {
    MachSafePointNode* poll = polls.at(i)->as_MachSafePoint();
    poll->set_branch_poll();
    remove_poll_address(C, poll);
  }

  // Emission reads the register of every node from the table: the checks have none.
  ra->grow_node_regs(C->unique() + accesses.length());
  for (int i = 0; i < accesses.length(); i++) {
    Node* access = accesses.at(i);
    Block* block = cfg->get_block_for_node(access);
    MachNullCheckNode* null_check = nullptr;
    for (int k = 0; k < null_checks.length() && null_check == nullptr; k++) {
      if (null_checks.at(k)->in(1) == access) {
        null_check = null_checks.at(k);
      }
    }
    guarantee(null_check != nullptr && cfg->get_block_for_node(null_check) == block, "the null check of this access");
    Node* value = access->in(null_check->_vidx);
    assert(!platform->null_test_kills_flags() || !reads_flags(access),
           "the explicit test would kill the flags the access reads (see PhaseCFG::implicit_null_check())");
    MachExplicitNullCheckNode* check = new MachExplicitNullCheckNode(value, value->ideal_reg() == Op_RegN);
    block->insert_node(check, block->find_node(access));
    cfg->map_node_to_block(check, block);
    null_check->set_explicit();
  }
  guarantee(C->unique() <= ra->node_regs_max_index(), "register allocation table too small for the null checks");

#ifdef ASSERT
  // Every trap site that still traps has no losable state live.
  VectorSet marked;
  for (int i = 0; i < polls.length(); i++) {
    marked.set(polls.at(i)->_idx);
  }
  for (int i = 0; i < accesses.length(); i++) {
    marked.set(accesses.at(i)->_idx);
  }
  for (int k = 0; k < null_checks.length(); k++) {
    assert(null_checks.at(k)->is_explicit() == marked.test(null_checks.at(k)->in(1)->_idx),
           "an implicit null check is explicit exactly where losable state is live");
  }
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    for (uint j = 0; j < block->number_of_nodes(); j++) {
      Node* n = block->get_node(j);
      if (is_loop_poll(n)) {
        assert(n->as_MachSafePoint()->is_branch_poll() == marked.test(n->_idx),
               "a loop poll branches exactly where losable state is live");
      }
    }
  }
#endif
  return true;
}

#ifndef PRODUCT
volatile int NoTrapSites::_null_checks = 0;
volatile int NoTrapSites::_explicit_null_checks = 0;
volatile int NoTrapSites::_loop_polls = 0;
volatile int NoTrapSites::_branch_polls = 0;

void NoTrapSites::print_statistics() {
  if (Matcher::no_trap() != nullptr) {
    tty->print_cr("AvoidLosableStateAtTraps: %d of %d implicit null checks explicit, %d of %d loop polls branch",
                  AtomicAccess::load(&_explicit_null_checks), AtomicAccess::load(&_null_checks),
                  AtomicAccess::load(&_branch_polls), AtomicAccess::load(&_loop_polls));
  }
}
#endif
