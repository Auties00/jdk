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
#include "opto/noTrap.hpp"
#include "opto/opcodes.hpp"
#include "opto/regalloc.hpp"
#include "opto/type.hpp"

// After register allocation only physical registers matter, so the liveness of the losable state
// is a backward dataflow over the platform's units (at most 64, one word per block). At a trap
// site the node's inputs (its TEMPs included) and the node itself count as live, since the fault
// can happen in the middle of its code, and so do the units the platform keeps live everywhere.

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

// Whether n may access raw or unknown memory, which in a compilation with unsafe accesses can
// fault and be resumed at the next instruction.
static bool may_access_raw_memory(const Node* n) {
  if (!n->is_Mach() || n->as_Mach()->memory_operand() == nullptr) {
    return false;
  }
  const TypePtr* types[] = { n->as_Mach()->in_adr_type(), n->as_Mach()->out_adr_type() };
  for (const TypePtr* t : types) {
    if (t == nullptr || t->base() == Type::RawPtr || t->base() == Type::AnyPtr) {
      return true;
    }
  }
  return false;
}

// Checks what the unsafe access window relies on (see NoTrapPlatform): no integer, pointer or
// narrow oop is in a losable floating point or vector register, and none is in a losable general
// purpose register in a compilation with unsafe accesses.
static void verify_placement(Compile* C, const NoTrapPlatform* platform) {
  PhaseCFG* cfg = C->cfg();
  PhaseRegAlloc* ra = C->regalloc();
  const bool unsafe = C->has_unsafe_access();
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    for (uint j = 0; j < block->number_of_nodes(); j++) {
      Node* n = block->get_node(j);
      if (n->_idx < ra->node_regs_max_index() && is_gpr_ideal_reg(value_ideal_reg(n))) {
        OptoReg::Name reg = ra->get_reg_first(n);
        if (OptoReg::is_reg(reg)) {
          int unit = platform->losable_unit(reg, value_ideal_reg(n));
          if (unit >= 0 && (unsafe || !platform->is_general_purpose_unit(unit))) {
            n->dump();
          }
          assert(unit < 0 || (!unsafe && platform->is_general_purpose_unit(unit)),
                 "an integer or pointer value in a register that a resumed exception can lose");
        }
      }
    }
  }
}
#endif // ASSERT

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
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    uint64_t live = live_out(block);
    for (int j = (int)block->number_of_nodes() - 1; j >= 0; j--) {
      Node* n = block->get_node(j);
#ifdef ASSERT
      // No mask that could carry a check's result is live across a raw access (see
      // Matcher::kill_masks_at_raw_access()).
      if (raw_killed != 0 && may_access_raw_memory(n)) {
        uint64_t across = live & raw_killed & ~bit(unit_of(platform, ra, n));
        if (across != 0) {
          n->dump();
        }
        assert(across == 0, "a mask register is live across a raw access of a compilation with unsafe accesses");
      }
#endif
      bool poll = is_loop_poll(n);
      bool access = null_check_accesses.test(n->_idx);
      if (poll || access) {
        uint64_t at_trap = always_live | live | bit(unit_of(platform, ra, n));
        for (uint k = 0; k < n->req(); k++) {
          at_trap |= bit(unit_of(platform, ra, n->in(k)));
        }
        if (at_trap != 0) {
          (poll ? polls : accesses).append(n);
        }
      }
      live = step_back(live, n);
    }
  }
  if (polls.is_empty() && accesses.is_empty()) {
    return false;
  }

  for (int i = 0; i < polls.length(); i++) {
    polls.at(i)->as_MachSafePoint()->set_branch_poll();
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
