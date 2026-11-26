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

#include "ci/ciMethod.hpp"
#include "ci/ciSymbol.hpp"
#include "libadt/vectset.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "opto/block.hpp"
#include "opto/c2compiler.hpp"
#include "opto/compile.hpp"
#include "opto/machnode.hpp"
#include "opto/matcher.hpp"
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

// The name of the method C compiles, for logging, in the resource area.
static const char* method_name(const Compile* C) {
  return C->method() != nullptr ? C->method()->name()->as_utf8() : "a stub";
}

// Whether n defines a mask: a value of the registers that carry the result of a vector check.
static bool is_mask_value(const Node* n) {
  return !n->is_MachTemp() && value_ideal_reg(n) == Op_RegVectMask;
}

static bool is_vector_ideal_reg(uint ideal_reg) {
  return ideal_reg == Op_VecA || ideal_reg == Op_VecS || ideal_reg == Op_VecD ||
         ideal_reg == Op_VecX || ideal_reg == Op_VecY || ideal_reg == Op_VecZ;
}

// Whether n, which defines no mask, can let a mask it reads decide something: every reader except
// those that only apply the mask as a predicate, which are the instructions that compute a vector
// and the masked vector stores. With a mask that a resumed fault zeroed, those compute a value with
// lanes dropped, which is a value of the unsafe access window, or store nothing. VectorStoreMask
// also computes a vector, but one that holds the mask itself (the payload of a boxed mask), so it
// counts as a reader that decides.
static bool checks_masks(const Node* n) {
  if (!n->is_Mach() || n->is_MachReturn()) {
    return true; // calls and safepoints with their debug info, returns
  }
  const int opcode = n->as_Mach()->ideal_Opcode();
  if (is_vector_ideal_reg(value_ideal_reg(n))) {
    return opcode == Op_VectorStoreMask;
  }
  return opcode != Op_StoreVectorMasked && opcode != Op_StoreVectorScatterMasked;
}

// Which masks reach a check, as a backward dataflow over the places a mask can be in: before
// register allocation each mask value is its own place, afterwards the places are the registers
// and stack slots that hold mask values. A place reaches a check at a point of the code if on some
// path from there its mask is read by a node that checks_masks(), or by a node that defines a mask
// whose place reaches a check after it (the mask operations, the copies and merges of the
// allocator and, across a block boundary, the Phis). A raw access needs the kill if a place that a
// resumed fault can lose reaches a check right after the access.
class MaskFlow : public StackObj {
 private:
  PhaseCFG* const _cfg;
  int* _place;                         // by node index: the place the node defines, or -1
  VectorSet _masks;                    // the mask values
  GrowableArray<OptoReg::Name> _regs;  // by place, after register allocation
  GrowableArray<bool> _losable;        // by place: whether a resumed fault can lose its content
  GrowableArray<Node*> _values;        // by place, before register allocation
  uint _words;                         // the words of a set of places
  uint64_t* _top;                      // by block: the places that reach a check after the Phis of the block
  uint64_t* _live;
  uint64_t* _scratch;

  static bool test(const uint64_t* set, int place) {
    return (set[place >> 6] & ((uint64_t)1 << (place & 63))) != 0;
  }

  static void add(uint64_t* set, int place) {
    set[place >> 6] |= (uint64_t)1 << (place & 63);
  }

  static void remove(uint64_t* set, int place) {
    set[place >> 6] &= ~((uint64_t)1 << (place & 63));
  }

  uint64_t* top(const Block* block) const {
    return _top + (size_t)block->_pre_order * _words;
  }

  bool is_mask(const Node* n) const {
    return _masks.test(n->_idx);
  }

  // The index of the first node of block after its head and its Phis.
  static uint first_after_phis(const Block* block) {
    uint i = 1;
    while (i < block->number_of_nodes() && block->get_node(i)->is_Phi()) {
      i++;
    }
    return i;
  }

  // Sets _live to the places that reach a check at the end of block: those at the top of its
  // successors, where the place of a Phi stands for the place of its input from block.
  void at_end(const Block* block) {
    for (uint w = 0; w < _words; w++) {
      _live[w] = 0;
    }
    for (uint s = 0; s < block->_num_succs; s++) {
      const Block* succ = block->_succs[s];
      const uint64_t* succ_top = top(succ);
      for (uint w = 0; w < _words; w++) {
        _scratch[w] = succ_top[w];
      }
      const uint phis_end = first_after_phis(succ);
      for (uint i = 1; i < phis_end; i++) {
        const int place = _place[succ->get_node(i)->_idx];
        if (place >= 0) {
          remove(_scratch, place);
        }
      }
      for (uint i = 1; i < phis_end; i++) {
        const Node* phi = succ->get_node(i);
        const int place = _place[phi->_idx];
        if (!is_mask(phi) || (place >= 0 && !test(succ_top, place))) {
          continue;
        }
        for (uint j = 1; j < succ->num_preds(); j++) {
          const Node* in = phi->in(j);
          if (in != nullptr && _cfg->get_block_for_node(succ->pred(j)) == block &&
              is_mask(in) && _place[in->_idx] >= 0) {
            add(_scratch, _place[in->_idx]);
          }
        }
      }
      for (uint w = 0; w < _words; w++) {
        _live[w] |= _scratch[w];
      }
    }
  }

  // Steps _live from after n to before n.
  void step_back(const Node* n) {
    const int place = _place[n->_idx];
    bool reads_for_a_check;
    if (is_mask(n)) {
      // A mask without a place is not followed: its inputs count as checked.
      reads_for_a_check = place < 0 || test(_live, place);
    } else {
      reads_for_a_check = checks_masks(n);
    }
    if (place >= 0) {
      remove(_live, place);
    }
    if (reads_for_a_check) {
      for (uint k = 1; k < n->req(); k++) {
        const Node* in = n->in(k);
        if (in != nullptr && is_mask(in) && _place[in->_idx] >= 0) {
          add(_live, _place[in->_idx]);
        }
      }
    }
  }

 public:
  // ra is null before register allocation.
  MaskFlow(Compile* C, PhaseRegAlloc* ra, const NoTrapPlatform* platform) : _cfg(C->cfg()) {
    const uint unique = C->unique();
    _place = NEW_RESOURCE_ARRAY(int, unique);
    for (uint i = 0; i < unique; i++) {
      _place[i] = -1;
    }
    uint block_count = 0;
    for (uint i = 0; i < _cfg->number_of_blocks(); i++) {
      const Block* block = _cfg->get_block(i);
      block_count = MAX2(block_count, block->_pre_order + 1);
      for (uint j = 1; j < block->number_of_nodes(); j++) {
        const Node* n = block->get_node(j);
        if (!is_mask_value(n)) {
          continue;
        }
        _masks.set(n->_idx);
        if (ra == nullptr) {
          _place[n->_idx] = _losable.append(true);
          _values.append(const_cast<Node*>(n));
          continue;
        }
        const OptoReg::Name reg = n->_idx < ra->node_regs_max_index() ? ra->get_reg_first(n) : OptoReg::Bad;
        if (!OptoReg::is_valid(reg)) {
          continue;
        }
        int place = _regs.find(reg);
        if (place < 0) {
          place = _regs.append(reg);
          _losable.append(OptoReg::is_reg(reg) && platform->losable_unit(reg, Op_RegVectMask) >= 0);
        }
        _place[n->_idx] = place;
      }
    }
    if (ra != nullptr && _regs.length() > 0) {
      // Every other definition into one of those registers or stack slots ends what it held.
      for (uint i = 0; i < _cfg->number_of_blocks(); i++) {
        const Block* block = _cfg->get_block(i);
        for (uint j = 1; j < block->number_of_nodes(); j++) {
          const Node* n = block->get_node(j);
          if (!is_mask(n) && n->_idx < ra->node_regs_max_index() && OptoReg::is_valid(ra->get_reg_first(n))) {
            _place[n->_idx] = _regs.find(ra->get_reg_first(n));
          }
        }
      }
    }
    _words = ((uint)_losable.length() + 63) >> 6;
    if (_words == 0) {
      _top = nullptr;
      _live = nullptr;
      _scratch = nullptr;
      return;
    }
    const size_t top_words = (size_t)block_count * _words;
    _top = NEW_RESOURCE_ARRAY(uint64_t, top_words);
    for (size_t i = 0; i < top_words; i++) {
      _top[i] = 0;
    }
    _live = NEW_RESOURCE_ARRAY(uint64_t, _words);
    _scratch = NEW_RESOURCE_ARRAY(uint64_t, _words);
    for (bool changed = true; changed; ) {
      changed = false;
      for (int i = (int)_cfg->number_of_blocks() - 1; i >= 0; i--) {
        const Block* block = _cfg->get_block(i);
        at_end(block);
        const uint first = first_after_phis(block);
        for (uint j = block->number_of_nodes(); j > first; j--) {
          step_back(block->get_node(j - 1));
        }
        uint64_t* block_top = top(block);
        for (uint w = 0; w < _words; w++) {
          if (block_top[w] != _live[w]) {
            block_top[w] = _live[w];
            changed = true;
          }
        }
      }
    }
  }

  // Appends the raw accesses right after which a place that a resumed fault can lose reaches a
  // check. The mask that an access defines itself is defined after the point of the fault. Before
  // register allocation, also appends the mask values that are such places to masks, if given.
  void accesses_before_checks(GrowableArray<MachNode*>& accesses, GrowableArray<Node*>* masks = nullptr) {
    if (_words == 0) {
      return;
    }
    for (uint i = 0; i < _cfg->number_of_blocks(); i++) {
      const Block* block = _cfg->get_block(i);
      at_end(block);
      const uint first = first_after_phis(block);
      for (uint j = block->number_of_nodes(); j > first; j--) {
        Node* n = block->get_node(j - 1);
        if (n->is_Mach() && Matcher::is_raw_memory_access(n->as_Mach())) {
          bool lost = false;
          for (int place = 0; place < _losable.length(); place++) {
            if (_losable.at(place) && place != _place[n->_idx] && test(_live, place)) {
              lost = true;
              if (masks != nullptr && place < _values.length()) {
                masks->append_if_missing(_values.at(place));
              }
            }
          }
          if (lost) {
            accesses.append(n->as_Mach());
          }
        }
        step_back(n);
      }
    }
  }
};

// A mask with every lane set or clear, made from a constant, is the same wherever it is computed,
// and is usually computed once in front of a loop and kept in a mask register across it: the
// full-vector mask of a loop whose last iteration takes a partial one. When it reaches a check
// (through the Phi that merges it with the partial mask, which the Vector API tests in its slow
// path), every raw access of the loop needs the kill and the mask lives on the stack. For those of
// masks that are such constants, this gives each use its own copy of the definition instead,
// right before the use or at the end of the predecessor block of a Phi, so that the mask is not
// live across a raw access. Returns the number of definitions it replaced.
static int sink_constant_masks(Compile* C, PhaseCFG* cfg, const GrowableArray<Node*>& masks) {
  GrowableArray<Node*> defs;
  for (int i = 0; i < masks.length(); i++) {
    Node* n = masks.at(i);
    if (!n->is_Mach() || n->as_Mach()->ideal_Opcode() != Op_MaskAll ||
        n->req() != 2 || n->in(0) != nullptr || n->in(1) == nullptr || !n->in(1)->is_Con()) {
      continue;
    }
    // Without TEMPs and projections (kills), the copies need nothing but their place.
    bool plain = true;
    for (DUIterator_Fast kmax, k = n->fast_outs(kmax); k < kmax && plain; k++) {
      plain = !n->fast_out(k)->is_Proj();
    }
    if (plain) {
      defs.append(n);
    }
  }
  for (int i = 0; i < defs.length(); i++) {
    Node* def = defs.at(i);
    GrowableArray<Node*> uses;
    for (DUIterator_Fast kmax, k = def->fast_outs(kmax); k < kmax; k++) {
      uses.append_if_missing(def->fast_out(k));
    }
    for (int u = 0; u < uses.length(); u++) {
      Node* use = uses.at(u);
      Block* use_block = cfg->get_block_for_node(use);
      if (use_block == nullptr) {
        continue;
      }
      if (use->is_Phi()) {
        for (uint k = 1; k < use->req(); k++) {
          if (use->in(k) == def) {
            Block* pred = cfg->get_block_for_node(use_block->pred(k));
            Node* copy = def->clone();
            pred->add_inst(copy);
            cfg->map_node_to_block(copy, pred);
            use->set_req(k, copy);
          }
        }
      } else {
        Node* copy = def->clone();
        use_block->insert_node(copy, use_block->find_node(use));
        cfg->map_node_to_block(copy, use_block);
        for (uint k = 1; k < use->req(); k++) {
          if (use->in(k) == def) {
            use->set_req(k, copy);
          }
        }
      }
    }
    if (def->outcnt() == 0) {
      cfg->get_block_for_node(def)->find_remove(def);
      cfg->unmap_node_from_block(def);
      def->disconnect_inputs(C);
    }
  }
  return defs.length();
}

bool NoTrapSites::kill_follows_masks(const Compile* C, const NoTrapPlatform* platform) {
  return C->has_unsafe_access() && platform->raw_access_kill() != nullptr &&
         platform->raw_access_kill_follows_masks() && !C->kill_masks_at_every_raw_access();
}

void NoTrapSites::kill_masks_where_checked(Compile* C, const NoTrapPlatform* platform) {
  if (!kill_follows_masks(C, platform)) {
    return;
  }
  ResourceMark rm;
  PhaseCFG* cfg = C->cfg();
  GrowableArray<MachNode*> accesses;
  int sunk = 0;
  for (int round = 0; ; round++) {
    accesses.clear();
    GrowableArray<Node*> masks;
    MaskFlow flow(C, nullptr, platform);
    flow.accesses_before_checks(accesses, &masks);
    // A copy is not live across a raw access, so the second round should sink nothing.
    if (round == 3) {
      break;
    }
    const int sunk_now = sink_constant_masks(C, cfg, masks);
    if (sunk_now == 0) {
      break;
    }
    sunk += sunk_now;
  }
  const RegMask* kill = platform->raw_access_kill();
  for (int i = 0; i < accesses.length(); i++) {
    MachNode* access = accesses.at(i);
    MachProjNode* proj = new MachProjNode(access, 20000, RegMask::EMPTY, MachProjNode::fat_proj);
    proj->_rout.or_with(*kill);
    proj->_rout.subtract(access->out_RegMask()); // the result of the access is defined after the fault point
    if (!proj->_rout.is_empty()) {
      Block* block = cfg->get_block_for_node(access);
      block->insert_node(proj, block->find_node(access) + 1);
      cfg->map_node_to_block(proj, block);
    }
  }
  log_debug(jit, compilation)("no-trap: %d raw accesses of %s kill the mask registers, %d constant masks sunk to their uses",
                              accesses.length(), method_name(C), sunk);
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

  // A mask that reaches a check must not be in a mask register across a raw access (see
  // kill_masks_where_checked(), which gave such accesses the kill before the allocator moved or
  // copied anything).
  if (kill_follows_masks(C, platform)) {
    GrowableArray<MachNode*> lossy_accesses;
    MaskFlow flow(C, ra, platform);
    flow.accesses_before_checks(lossy_accesses);
    if (!lossy_accesses.is_empty()) {
      log_debug(jit, compilation)("no-trap: a mask that reaches a check is in a mask register across a raw access "
                                  "of %s, retrying with the kill at every raw access", method_name(C));
      C->record_failure(C2Compiler::retry_kill_masks_at_every_raw_access());
      return false;
    }
  }

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
  // Where every raw access kills them, no killed register is live across one.
  DEBUG_ONLY(const uint64_t raw_killed = C->has_unsafe_access() && !kill_follows_masks(C, platform) ?
                                         platform->raw_access_killed_units() : 0;)
  GrowableArray<Node*> polls;
  GrowableArray<Node*> accesses;
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    uint64_t live = live_out(block);
    for (int j = (int)block->number_of_nodes() - 1; j >= 0; j--) {
      Node* n = block->get_node(j);
#ifdef ASSERT
      // No register that could carry the result of a check is live across a raw access (see
      // Matcher::kill_masks_at_raw_accesses()).
      if (raw_killed != 0 && n->is_Mach() && Matcher::is_raw_memory_access(n->as_Mach())) {
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
