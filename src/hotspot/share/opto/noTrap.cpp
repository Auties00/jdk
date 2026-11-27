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

// The index of the first node of block after its head and its Phis.
static uint first_after_phis(const Block* block) {
  uint i = 1;
  while (i < block->number_of_nodes() && block->get_node(i)->is_Phi()) {
    i++;
  }
  return i;
}

// The inputs of n whose reading of a mask is no check, as [start, end): none.
static void debug_inputs(const Node* n, uint& start, uint& end) {
  start = 0;
  end = 0;
}

// Which masks reach a check, as a backward dataflow over the places a mask can be in: before
// register allocation each mask value is its own place, afterwards the places are the registers
// and stack slots that hold mask values. A place reaches a check at a point of the code if on some
// path from there its mask is read by a node that checks_masks(), or by a node that defines a mask
// whose place reaches a check after it (the mask operations, the copies and merges of the
// allocator and, across a block boundary, the Phis). A raw access needs the kill if a place that a
// resumed fault can lose reaches a check right after the access. Before register allocation every
// mask value counts as such a place, except the copies that protect_masks() binds to the stack.
class MaskFlow : public StackObj {
 private:
  PhaseCFG* const _cfg;
  int* _place;                         // by node index: the place the node defines, or -1
  VectorSet _masks;                    // the mask values
  VectorSet _reaching;                 // the mask values that reach a check (see accesses_before_checks())
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
      uint debug_start = 0;
      uint debug_end = 0;
      if (!is_mask(n)) {
        debug_inputs(n, debug_start, debug_end);
      }
      // A mask that is a projection is computed from the inputs of the node it projects.
      const Node* reader = n->is_Proj() && n->in(0) != nullptr ? n->in(0) : n;
      for (uint k = 1; k < reader->req(); k++) {
        const Node* in = reader->in(k);
        if (in != nullptr && (k < debug_start || k >= debug_end) && is_mask(in) && _place[in->_idx] >= 0) {
          add(_live, _place[in->_idx]);
        }
      }
    }
  }

 public:
  bool is_mask(const Node* n) const {
    return _masks.test(n->_idx);
  }

  // After accesses_before_checks(): whether the mask value n is read for a check on some path.
  bool reaches_check(const Node* n) const {
    return _reaching.test(n->_idx);
  }

  // ra is null before register allocation, and stack_bound then holds the mask values that the
  // allocator can only put on the stack.
  MaskFlow(Compile* C, PhaseRegAlloc* ra, const NoTrapPlatform* platform, const VectorSet* stack_bound = nullptr)
      : _cfg(C->cfg()) {
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
          _place[n->_idx] = _losable.append(stack_bound == nullptr || !stack_bound->test(n->_idx));
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
        if (is_mask(n) && (_place[n->_idx] < 0 || test(_live, _place[n->_idx]))) {
          _reaching.set(n->_idx);
        }
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
      for (uint j = 1; j < first; j++) {
        const Node* phi = block->get_node(j);
        if (is_mask(phi) && (_place[phi->_idx] < 0 || test(_live, _place[phi->_idx]))) {
          _reaching.set(phi->_idx);
        }
      }
    }
  }
};

// Whether a raw access is among the nodes of block after index from and before index to.
static bool raw_access_between(const Block* block, uint from, uint to) {
  for (uint i = from + 1; i < to; i++) {
    const Node* n = block->get_node(i);
    if (n->is_Mach() && Matcher::is_raw_memory_access(n->as_Mach())) {
      return true;
    }
  }
  return false;
}

// Whether n is a mask with every lane set or clear made from a constant (an immediate, or a constant
// in a register), by an instruction without TEMPs or kills: a copy of it can be placed anywhere.
// Like other constants, the one made from an immediate has the root as its control input.
static bool is_constant_mask(const Node* n) {
  if (!n->is_Mach() || n->as_Mach()->ideal_Opcode() != Op_MaskAll || n->req() > 2 ||
      (n->in(0) != nullptr && !n->in(0)->is_Root()) ||
      (n->req() == 2 && (n->in(1) == nullptr || !n->in(1)->is_Con()))) {
    return false;
  }
  for (DUIterator_Fast kmax, k = n->fast_outs(kmax); k < kmax; k++) {
    if (n->fast_out(k)->is_Proj()) {
      return false;
    }
  }
  return true;
}

// The masks that the raw accesses found by MaskFlow would lose (masks, which are in mask registers
// across such an access and reach a check) usually have many readers that do not check, on the
// hot path, and a few that do, in the slow paths: the mask of a loop iteration is the predicate of
// its loads, stores and arithmetic, and the Vector API tests it only where an access is not wholly
// in bounds. The kill would put the whole live range on the stack (PhaseChaitin::Split() has no
// place at which to split it: the kill is an interference, not register pressure) and make every
// reader reload it. Instead, this leaves the mask in its register for the readers that do not
// check, where a zeroed mask is harmless, and gives each reader that checks a value that a fault
// cannot have zeroed:
// - for a constant mask (see is_constant_mask()), a copy of its definition right in front of the
//   reader;
// - for any other mask, a copy bound to the stack that is made right after the definition (keep),
//   and a reload of that copy right in front of the reader. Neither is marked as a copy, so that
//   the allocator's copy removal, which follows copies to a register that still holds the value,
//   cannot make a reader read the register of the mask, or an earlier reload, again.
// A reader that follows the definition in its block with no raw access in between reads the mask
// itself. The copy for a Phi goes to the end of the predecessor block. Returns the number of masks
// it protected; the others keep the kill.
static int protect_masks(Compile* C, PhaseCFG* cfg, const MaskFlow& flow, const GrowableArray<Node*>& masks,
                         VectorSet& stack_bound, const RegMask& stack) {
  const RegMask& anywhere = *C->matcher()->idealreg2spillmask[Op_RegVectMask];
  const RegMask& registers = *C->matcher()->idealreg2regmask[Op_RegVectMask];
  int count = 0;
  for (int i = 0; i < masks.length(); i++) {
    Node* mask = masks.at(i);
    Block* block = cfg->get_block_for_node(mask);
    if (block == nullptr) {
      continue;
    }
    // The last node of the definition, and where a copy of the mask can follow it.
    const uint def_at = mask->is_Phi() ? first_after_phis(block) - 1 : block->find_node(mask);
    uint keep_at = def_at + 1;
    while (keep_at < block->number_of_nodes() && block->get_node(keep_at)->is_Proj() &&
           block->get_node(keep_at)->in(0) == mask) {
      keep_at++;
    }
    const bool constant = is_constant_mask(mask);
    if (!constant && (keep_at >= block->number_of_nodes() || block->get_node(keep_at)->is_MachNullCheck())) {
      continue; // nothing can follow the access of an implicit null check
    }

    // The readers that check, each with the input that reads the mask.
    GrowableArray<Node*> users;
    for (DUIterator_Fast kmax, k = mask->fast_outs(kmax); k < kmax; k++) {
      users.append_if_missing(mask->fast_out(k));
    }
    GrowableArray<Node*> readers;
    GrowableArray<uint> inputs;
    for (int u = 0; u < users.length(); u++) {
      Node* user = users.at(u);
      Block* user_block = cfg->get_block_for_node(user);
      if (user_block == nullptr) {
        continue;
      }
      uint debug_start = 0;
      uint debug_end = 0;
      if (flow.is_mask(user)) {
        if (!flow.reaches_check(user)) {
          continue;
        }
      } else {
        if (!checks_masks(user)) {
          continue;
        }
        debug_inputs(user, debug_start, debug_end);
      }
      for (uint k = 1; k < user->req(); k++) {
        if (user->in(k) != mask || (k >= debug_start && k < debug_end)) {
          continue;
        }
        if (user->is_Phi()) {
          if (cfg->get_block_for_node(user_block->pred(k)) == block &&
              !raw_access_between(block, def_at, block->number_of_nodes())) {
            continue;
          }
        } else if (user_block == block) {
          const uint user_at = block->find_node(user);
          if (user_at > def_at && !raw_access_between(block, def_at, user_at)) {
            continue;
          }
        }
        readers.append(user);
        inputs.append(k);
      }
    }
    if (readers.is_empty()) {
      continue;
    }

    Node* keep = nullptr;
    if (!constant) {
      keep = new MachSpillCopyNode(MachSpillCopyNode::RegToMem, mask, anywhere, stack);
      keep->remove_flag(Node::Flag_is_Copy);
      block->insert_node(keep, keep_at);
      cfg->map_node_to_block(keep, block);
      stack_bound.set(keep->_idx);
    }
    for (int r = 0; r < readers.length(); r++) {
      Node* reader = readers.at(r);
      const uint k = inputs.at(r);
      if (r > 0 && readers.at(r - 1) == reader && !reader->is_Phi()) {
        reader->set_req(k, reader->in(inputs.at(r - 1))); // the copy made for its other input
        continue;
      }
      Node* copy;
      if (constant) {
        copy = mask->clone();
      } else {
        copy = new MachSpillCopyNode(MachSpillCopyNode::MemToReg, keep, stack, registers);
        copy->remove_flag(Node::Flag_is_Copy);
      }
      Block* reader_block = cfg->get_block_for_node(reader);
      if (reader->is_Phi()) {
        Block* pred = cfg->get_block_for_node(reader_block->pred(k));
        pred->add_inst(copy);
        cfg->map_node_to_block(copy, pred);
      } else {
        reader_block->insert_node(copy, reader_block->find_node(reader));
        cfg->map_node_to_block(copy, reader_block);
      }
      reader->set_req(k, copy);
    }
    if (mask->outcnt() == 0) {
      block->find_remove(mask);
      cfg->unmap_node_from_block(mask);
      mask->disconnect_inputs(C);
    }
    count++;
  }
  return count;
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
  // The stack slots a mask can be spilled to, for the copies of protect_masks().
  RegMask* stack = new (C->comp_arena()->Amalloc(sizeof(RegMask))) RegMask(C->comp_arena());
  stack->assignFrom(*C->matcher()->idealreg2spillmask[Op_RegVectMask]);
  stack->subtract(*C->matcher()->idealreg2regmask[Op_RegVectMask]);
  VectorSet stack_bound;
  GrowableArray<MachNode*> accesses;
  int protected_masks = 0;
  for (int round = 0; ; round++) {
    accesses.clear();
    GrowableArray<Node*> masks;
    MaskFlow flow(C, nullptr, platform, &stack_bound);
    flow.accesses_before_checks(accesses, &masks);
    // The copies of a protected mask are read right where they are made, and a mask computed from
    // them can itself be found, so a few rounds are enough; whatever is left keeps the kill.
    if (round == 4) {
      break;
    }
    const int protected_now = protect_masks(C, cfg, flow, masks, stack_bound, *stack);
    if (protected_now == 0) {
      break;
    }
    protected_masks += protected_now;
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
  log_debug(jit, compilation)("no-trap: %d raw accesses of %s kill the mask registers, %d masks protected for their checks",
                              accesses.length(), method_name(C), protected_masks);
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
  NOT_PRODUCT(int loop_polls = 0;)
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
