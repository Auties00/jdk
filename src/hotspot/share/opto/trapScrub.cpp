/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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
#include "opto/cfgnode.hpp"
#include "opto/compile.hpp"
#include "opto/machnode.hpp"
#include "opto/opcodes.hpp"
#include "opto/regalloc.hpp"
#include "opto/trapScrub.hpp"
#include "opto/type.hpp"

// Keeps the place where a resumed exception would find a stale header clear wherever a trap site
// at which a value holding losable state is live (a hot trap) can be reached.
//
// After register allocation only physical registers matter, so liveness is a backward dataflow
// over the platform's losable register units (at most 64, one word per block). At a trap site the
// node's inputs (its TEMPs included) and the node itself count as live, since the fault can happen
// in the middle of its code.
//
// The place can only become dirty at a writer (is_writer()), so a MachTrapScrubNode goes right
// after each writer from which a hot trap is reachable without passing another writer: a second
// backward dataflow. A loop without calls then has no scrub inside it, whatever traps it contains.
// With TrapScrubPlatform::at_traps() (or where a writer ends its block) the scrub goes right
// before each hot trap instead; with TrapScrubPlatform::verify(), a check goes there as well.
//
// The pass runs after PhaseCFG::remove_empty_blocks(): an empty block is then a connector, which
// is never executed (branches and fall-throughs go to its non_connector()). Placement and
// verification follow non_connector() successors, and no scrub goes into a connector.

// Whether n can fault and resume in this frame: a loop safepoint poll, the memory access of an
// implicit null check, or, in a compilation with unsafe accesses, an access to raw or unknown
// memory (a fault there resumes at the next instruction, see JavaThread::doing_unsafe_access()).
static bool may_be_raw_memory(const TypePtr* adr_type) {
  return adr_type == nullptr || adr_type->base() == Type::RawPtr || adr_type->base() == Type::AnyPtr;
}

static bool is_resumable_trap_site(const Node* n, const VectorSet& null_check_accesses, bool raw_accesses_trap) {
  if (null_check_accesses.test(n->_idx)) {
    return true;
  }
  if (!n->is_Mach()) {
    return false;
  }
  if (n->is_MachSafePoint()) {
    return !n->is_MachCall();
  }
  if (raw_accesses_trap) {
    const MachNode* mach = n->as_Mach();
    if (mach->memory_operand() == nullptr) {
      return false;
    }
    return may_be_raw_memory(mach->in_adr_type()) || may_be_raw_memory(mach->out_adr_type());
  }
  return false;
}

// Whether n can leave arbitrary data deep below the stack pointer of this frame while the frame's
// code continues after it: calls (the callee's frames), the method entry (the frames of the
// caller and of whatever ran before on this thread), exception entry (the unwinding runtime),
// and the instructions the platform names. Everything else C2 emits writes only into its own
// frame, above the stack pointer, or a few words below it; out of line slow paths that call out
// clear the stale header themselves before they return.
static bool is_writer(const TrapScrubPlatform* platform, const Node* n) {
  if (!n->is_Mach()) {
    return false;
  }
  const MachNode* mach = n->as_Mach();
  return mach->is_MachCall() || mach->is_MachProlog() || mach->ideal_Opcode() == Op_CreateEx ||
         platform->is_writer(mach);
}

#ifdef ASSERT
static bool is_scrub(const Node* n) {
  return n->is_Mach() && n->as_Mach()->is_trap_scrub() && !((const MachTrapScrubNode*)n)->is_check();
}
#endif

// The unit of losable state the value defined by n occupies, or -1.
static int unit_of(const TrapScrubPlatform* platform, PhaseRegAlloc* ra, const Node* n) {
  if (n == nullptr || n->_idx >= ra->node_regs_max_index()) {
    return -1; // created after register allocation without a register (prolog, epilog)
  }
  OptoReg::Name reg = ra->get_reg_first(n);
  if (!OptoReg::is_reg(reg)) {
    return -1;
  }
  int unit = platform->losable_unit(reg, n->is_MachTemp() ? 0 : n->ideal_reg());
  assert(unit < 64, "at most 64 units");
  return unit;
}

static uint64_t bit(int unit) {
  return unit < 0 ? 0 : (uint64_t)1 << unit;
}

// Whether, from the end of block, a hot trap is reachable without passing a writer.
static bool need_out(const Block* block, const bool* need_in) {
  for (uint s = 0; s < block->_num_succs; s++) {
    if (need_in[block->_succs[s]->non_connector()->_pre_order]) {
      return true;
    }
  }
  return false;
}

// Whether the only way into target leads from block, through connectors only.
static bool only_predecessor_is(PhaseCFG* cfg, Block* target, const Block* block) {
  Block* b = target;
  while (b->num_preds() == 2) {
    Block* pred = cfg->get_block_for_node(b->pred(1));
    if (pred == block) {
      return true;
    }
    if (!pred->is_connector()) {
      return false;
    }
    b = pred;
  }
  return false;
}

void TrapScrubs::insert(Compile* C, const TrapScrubPlatform* platform, GrowableArray<Node*>* hot_traps_out) {
  PhaseCFG* cfg = C->cfg();
  PhaseRegAlloc* ra = C->regalloc();
  ResourceMark rm;

  uint block_count = 0;
  VectorSet null_check_accesses;
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    block_count = MAX2(block_count, block->_pre_order + 1);
    for (uint j = 0; j < block->number_of_nodes(); j++) {
      Node* n = block->get_node(j);
      if (n->is_MachNullCheck()) {
        null_check_accesses.set(n->in(1)->_idx);
      }
    }
  }
  const bool raw_accesses_trap = C->has_unsafe_access();

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

  // The hot traps.
  GrowableArray<Node*> hot_traps;
  VectorSet hot;
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    uint64_t live = live_out(block);
    for (int j = (int)block->number_of_nodes() - 1; j >= 0; j--) {
      Node* n = block->get_node(j);
      if (is_resumable_trap_site(n, null_check_accesses, raw_accesses_trap)) {
        uint64_t at_trap = live | bit(unit_of(platform, ra, n));
        for (uint k = 0; k < n->req(); k++) {
          at_trap |= bit(unit_of(platform, ra, n->in(k)));
        }
        if (at_trap != 0) {
          hot.set(n->_idx);
          hot_traps.append(n);
        }
      }
      live = step_back(live, n);
    }
  }
  if (hot_traps.is_empty()) {
    return;
  }

  // need_in[b]: a hot trap is reachable from the start of b without passing a writer.
  bool* need_in = NEW_RESOURCE_ARRAY(bool, block_count);
  for (uint i = 0; i < block_count; i++) {
    need_in[i] = false;
  }
  // The writers after which a scrub goes.
  GrowableArray<Node*> after;
  bool at_traps = platform->at_traps();
  if (!at_traps) {
    for (bool changed = true; changed; ) {
      changed = false;
      for (int i = (int)cfg->number_of_blocks() - 1; i >= 0; i--) {
        Block* block = cfg->get_block(i);
        bool need = need_out(block, need_in);
        for (int j = (int)block->number_of_nodes() - 1; j >= 0; j--) {
          Node* n = block->get_node(j);
          if (is_writer(platform, n)) {
            need = false;
          }
          if (hot.test(n->_idx)) {
            need = true;
          }
        }
        if (need != need_in[block->_pre_order]) {
          need_in[block->_pre_order] = need;
          changed = true;
        }
      }
    }
    for (uint i = 0; i < cfg->number_of_blocks() && !at_traps; i++) {
      Block* block = cfg->get_block(i);
      bool need = need_out(block, need_in);
      for (int j = (int)block->number_of_nodes() - 1; j >= 0; j--) {
        Node* n = block->get_node(j);
        if (is_writer(platform, n)) {
          if (need && !(platform->omit_after_calls() && n->is_MachCall())) {
            if ((uint)j + 1 >= block->number_of_nodes()) {
              // No place after it in its block: scrub at the traps instead.
              at_traps = true;
              break;
            }
            after.append(n);
          }
          need = false;
        }
        if (hot.test(n->_idx)) {
          need = true;
        }
      }
    }
  }
  if (at_traps) {
    after.clear();
  }
  const bool verify = platform->verify();

  // A call's Catch can have several successors.
  uint scrubs = (uint)(2 * after.length() + 2 * hot_traps.length());
  if (C->check_node_count(scrubs, "out of nodes inserting trap scrubs")) {
    return;
  }
  if (C->unique() + scrubs >= ra->node_regs_max_index()) {
    // Not permanent: a later compilation of the method can succeed.
    C->record_failure("out of register allocation slots inserting trap scrubs");
    return;
  }

  // A call whose block ends in a Catch must be followed only by its projections and the Catch
  // (the exception table is built from the call's return): the scrub then goes at the start of
  // each block the Catch leads to that needs it, other than the exception handlers (they start
  // with CreateEx, itself a writer). An empty successor is a connector by now, so the scrub goes
  // into the block it leads to. That block can be a join reached by other paths, which then run
  // a scrub they do not need (harmless); it uses the registers the call kills only when the call
  // is the only way into it.
  VectorSet scrubbed_at_start;
  for (int i = 0; i < after.length(); i++) {
    Node* writer = after.at(i);
    Block* block = cfg->get_block_for_node(writer);
    uint k = block->find_node(writer) + 1;
    // Keep the writer's data projections next to it (they emit no code), but stay before a
    // projection that ends the block.
    while (k + 1 < block->number_of_nodes() && block->get_node(k)->is_MachProj() &&
           block->get_node(k)->in(0) == writer && block->get_node(k)->is_block_proj() == nullptr) {
      k++;
    }
    if (block->get_node(k)->is_Catch()) {
      for (uint s = 0; s < block->_num_succs; s++) {
        Block* target = block->_succs[s]->non_connector();
        if (!need_in[target->_pre_order] || scrubbed_at_start.test(target->_pre_order)) {
          continue;
        }
        uint start = 1;
        while (start < target->number_of_nodes() && target->get_node(start)->is_Phi()) {
          start++;
        }
        if (start < target->number_of_nodes() && target->get_node(start)->is_Mach() &&
            target->get_node(start)->as_Mach()->ideal_Opcode() == Op_CreateEx) {
          continue;
        }
        scrubbed_at_start.set(target->_pre_order);
        bool after_call = writer->is_MachCall() && only_predecessor_is(cfg, target, block);
        MachTrapScrubNode* scrub = new MachTrapScrubNode(false /* check */, after_call);
        target->insert_node(scrub, start);
        cfg->map_node_to_block(scrub, target);
      }
      continue;
    }
    MachTrapScrubNode* scrub = new MachTrapScrubNode(false /* check */, writer->is_MachCall());
    block->insert_node(scrub, k);
    cfg->map_node_to_block(scrub, block);
  }
  for (int i = 0; i < hot_traps.length(); i++) {
    Node* trap = hot_traps.at(i);
    Block* block = cfg->get_block_for_node(trap);
    if (at_traps) {
      MachTrapScrubNode* scrub = new MachTrapScrubNode();
      block->insert_node(scrub, block->find_node(trap));
      cfg->map_node_to_block(scrub, block);
    }
    if (verify) {
      MachTrapScrubNode* check = new MachTrapScrubNode(true /* check */);
      block->insert_node(check, block->find_node(trap));
      cfg->map_node_to_block(check, block);
    }
  }
  if (hot_traps_out != nullptr) {
    hot_traps_out->appendAll(&hot_traps);
  }
}

#ifdef ASSERT
void TrapScrubs::verify(Compile* C, const TrapScrubPlatform* platform, const GrowableArray<Node*>* hot_traps) {
  if (platform->omit_after_calls()) {
    return;
  }
  PhaseCFG* cfg = C->cfg();
  ResourceMark rm;
  VectorSet hot;
  for (int i = 0; i < hot_traps->length(); i++) {
    hot.set(hot_traps->at(i)->_idx);
  }
  uint block_count = 0;
  for (uint i = 0; i < cfg->number_of_blocks(); i++) {
    Block* block = cfg->get_block(i);
    block_count = MAX2(block_count, block->_pre_order + 1);
    if (block->is_connector()) {
      for (uint j = 0; j < block->number_of_nodes(); j++) {
        assert(!is_scrub(block->get_node(j)), "a trap scrub in a block that is never executed");
      }
    }
  }
  bool* need_in = NEW_RESOURCE_ARRAY(bool, block_count);
  for (uint i = 0; i < block_count; i++) {
    need_in[i] = false;
  }
  for (bool changed = true; changed; ) {
    changed = false;
    for (int i = (int)cfg->number_of_blocks() - 1; i >= 0; i--) {
      Block* block = cfg->get_block(i);
      if (block->is_connector()) {
        continue;
      }
      bool need = need_out(block, need_in);
      for (int j = (int)block->number_of_nodes() - 1; j >= 0; j--) {
        Node* n = block->get_node(j);
        if (is_scrub(n)) {
          need = false;
        } else if (is_writer(platform, n)) {
          if (need) {
            n->dump();
          }
          assert(!need, "a hot trap can be reached from this writer without passing a trap scrub");
          need = false;
        }
        if (hot.test(n->_idx)) {
          need = true;
        }
      }
      if (need != need_in[block->_pre_order]) {
        need_in[block->_pre_order] = need;
        changed = true;
      }
    }
  }
}
#endif
