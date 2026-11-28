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
#include "opto/addnode.hpp"
#include "opto/block.hpp"
#include "opto/c2compiler.hpp"
#include "opto/cfgnode.hpp"
#include "opto/compile.hpp"
#include "opto/connode.hpp"
#include "opto/machnode.hpp"
#include "opto/matcher.hpp"
#include "opto/mulnode.hpp"
#include "opto/noTrap.hpp"
#include "opto/opcodes.hpp"
#include "opto/phaseX.hpp"
#include "opto/regalloc.hpp"
#include "opto/subnode.hpp"
#include "opto/type.hpp"
#include "opto/vectornode.hpp"
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

// Whether n defines a vector.
static bool is_vector_value(const Node* n) {
  return !n->is_MachTemp() && is_vector_ideal_reg(value_ideal_reg(n));
}

// Whether n, which defines neither a mask nor a vector, can let a mask or vector it reads decide
// something. Every such reader does (a flags producer, a conversion of a mask to an integer, the
// arguments of calls, returns), except those that turn the lanes into a value of the unsafe access
// window: the vector stores, which store what the vector holds or, with a mask that a resumed fault
// zeroed, nothing; and the reductions and lane extracts, whose integer or floating point result is
// such a value like the vector it is computed from, and is tested as an integer wherever it steers
// an access.
static bool checks_values(const Node* n) {
  if (!n->is_Mach() || n->is_MachReturn()) {
    return true; // the arguments of calls, returns (for the debug info see CheckFlow::step_back())
  }
  switch (n->as_Mach()->ideal_Opcode()) {
    case Op_StoreVector:
    case Op_StoreVectorMasked:
    case Op_StoreVectorScatter:
    case Op_StoreVectorScatterMasked:
    case Op_AddReductionVI:
    case Op_AddReductionVL:
    case Op_AddReductionVHF:
    case Op_AddReductionVF:
    case Op_AddReductionVD:
    case Op_MulReductionVI:
    case Op_MulReductionVL:
    case Op_MulReductionVHF:
    case Op_MulReductionVF:
    case Op_MulReductionVD:
    case Op_AndReductionV:
    case Op_OrReductionV:
    case Op_XorReductionV:
    case Op_MinReductionV:
    case Op_MaxReductionV:
    case Op_UMinReductionV:
    case Op_UMaxReductionV:
    case Op_ExtractB:
    case Op_ExtractUB:
    case Op_ExtractC:
    case Op_ExtractS:
    case Op_ExtractI:
    case Op_ExtractL:
    case Op_ExtractF:
    case Op_ExtractD:
      return false;
    default:
      return true;
  }
}

// Whether n, which defines a vector, reads its inputs for a check whatever reads the vector: the
// vector of VectorStoreMask holds the mask itself (the payload of a boxed mask).
static bool always_checks(const Node* n) {
  return n->is_Mach() && n->as_Mach()->ideal_Opcode() == Op_VectorStoreMask;
}

// The index of the first node of block after its head and its Phis.
static uint first_after_phis(const Block* block) {
  uint i = 1;
  while (i < block->number_of_nodes() && block->get_node(i)->is_Phi()) {
    i++;
  }
  return i;
}

// The inputs of n that are its debug info, as [start, end), or an empty range.
static void debug_inputs(const Node* n, uint& start, uint& end) {
  start = 0;
  end = 0;
  if (n->is_MachSafePoint() && n->jvms() != nullptr) {
    start = n->jvms()->debug_start();
    end = n->jvms()->debug_end();
  }
}

// Which masks and vectors reach a check, as a backward dataflow over the places such a value can be
// in: before register allocation each value is its own place, afterwards each register or stack
// slot that holds such values is two places, one for the values a resumed fault can lose there and
// one for the others (a vector of 128 bits in xmm0-15 is kept, a wider one is not), and a definition
// into the register ends both. A place reaches a check at a point of the code if on some path from
// there its value is read by a node that checks_values(), or by a node that defines a mask or a
// vector whose place reaches a check after it (the mask and vector operations, the copies and
// merges of the allocator and, across a block boundary, the Phis). A value that a resumed fault can
// lose must not be in such a place across a raw access. Before register allocation the losable
// places are the vectors that can be given a losable register and, where the raw accesses do not
// all kill the mask registers, the masks; but not the copies that protect_values() binds to the
// stack.
class CheckFlow : public StackObj {
 private:
  PhaseCFG* const _cfg;
  const bool _paired;                  // after register allocation: places 2k and 2k + 1 are one register
  int* _place;                         // by node index: the place the node defines, or -1
  VectorSet _masks;                    // the mask and vector values
  VectorSet _reaching;                 // the values that reach a check (see accesses_before_checks())
  GrowableArray<OptoReg::Name> _regs;  // by pair of places, after register allocation
  GrowableArray<bool> _losable;        // by place: whether a resumed fault can lose its content
  GrowableArray<bool> _vector;         // by place: whether it holds vectors
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

  // Removes place and the other place of its register.
  void end(uint64_t* set, int place) const {
    remove(set, place);
    if (_paired) {
      remove(set, place ^ 1);
    }
  }

  // Whether n defines place, or the other place of its register.
  bool defines(const Node* n, int place) const {
    const int own = _place[n->_idx];
    return own >= 0 && (own == place || (_paired && (own ^ 1) == place));
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
          end(_scratch, place);
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
      reads_for_a_check = place < 0 || test(_live, place) || always_checks(n);
    } else {
      // A projection that defines no mask or vector (a kill, another result) reads nothing: the node
      // it projects is asked itself.
      reads_for_a_check = !n->is_Proj() && checks_values(n);
    }
    if (place >= 0) {
      end(_live, place);
    }
    if (reads_for_a_check) {
      // The debug info of a safepoint or call is no check. A mask it describes is read only if the
      // frame is deoptimized there, which makes it a heap object that every later reader sees
      // alike, and what runs after a deoptimization checks its own accesses: the interpreter, C1
      // code and the Java code of the Vector API by the bounds checks of the language, and other
      // compiled code by the checks it carries for its intrinsic accesses, on the objects it is
      // given. (The pending InternalError is also thrown where the interpreter takes over.)
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

  // After accesses_before_checks(): whether the value n is read for a check on some path.
  bool reaches_check(const Node* n) const {
    return _reaching.test(n->_idx) || always_checks(n);
  }

  // ra is null before register allocation, and stack_bound then holds the values that the allocator
  // can only put on the stack. masks_losable tells whether a mask register can hold a mask across a
  // raw access at all (see NoTrapSites::kill_follows_masks()).
  CheckFlow(Compile* C, PhaseRegAlloc* ra, const NoTrapPlatform* platform, bool masks_losable,
            const VectorSet* stack_bound = nullptr)
      : _cfg(C->cfg()), _paired(ra != nullptr) {
    const uint unique = C->unique();
    _place = NEW_RESOURCE_ARRAY(int, unique);
    for (uint i = 0; i < unique; i++) {
      _place[i] = -1;
    }
    uint block_count = 0;
    bool any_losable = false;
    for (uint i = 0; i < _cfg->number_of_blocks(); i++) {
      const Block* block = _cfg->get_block(i);
      block_count = MAX2(block_count, block->_pre_order + 1);
      for (uint j = 1; j < block->number_of_nodes(); j++) {
        const Node* n = block->get_node(j);
        const bool mask = is_mask_value(n);
        if (!mask && !is_vector_value(n)) {
          continue;
        }
        _masks.set(n->_idx);
        if (ra == nullptr) {
          const bool bound = stack_bound != nullptr && stack_bound->test(n->_idx);
          const bool losable = !bound && (mask ? masks_losable
                                               : platform->is_losable_input(n->out_RegMask(), value_ideal_reg(n)));
          any_losable |= losable;
          _place[n->_idx] = _losable.append(losable);
          _vector.append(!mask);
          _values.append(const_cast<Node*>(n));
          continue;
        }
        const OptoReg::Name reg = n->_idx < ra->node_regs_max_index() ? ra->get_reg_first(n) : OptoReg::Bad;
        if (!OptoReg::is_valid(reg)) {
          continue;
        }
        int pair = _regs.find(reg);
        if (pair < 0) {
          pair = _regs.append(reg);
          _losable.append(false);
          _losable.append(true);
          _vector.append(!mask);
          _vector.append(!mask);
        }
        const bool losable = OptoReg::is_reg(reg) && (!mask || masks_losable) &&
                             platform->losable_unit(reg, value_ideal_reg(n)) >= 0;
        any_losable |= losable;
        _place[n->_idx] = 2 * pair + (losable ? 1 : 0);
      }
    }
    if (ra != nullptr && _regs.length() > 0) {
      // Every other definition into one of those registers or stack slots ends what it held.
      for (uint i = 0; i < _cfg->number_of_blocks(); i++) {
        const Block* block = _cfg->get_block(i);
        for (uint j = 1; j < block->number_of_nodes(); j++) {
          const Node* n = block->get_node(j);
          if (!is_mask(n) && n->_idx < ra->node_regs_max_index() && OptoReg::is_valid(ra->get_reg_first(n))) {
            const int pair = _regs.find(ra->get_reg_first(n));
            _place[n->_idx] = pair < 0 ? -1 : 2 * pair;
          }
        }
      }
    }
    // Without a place that a fault can lose there is nothing to find.
    _words = any_losable ? ((uint)_losable.length() + 63) >> 6 : 0;
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
  // check. The value that an access defines itself is defined after the point of the fault. Before
  // register allocation, also appends the values that are such places to masks, if given. Returns
  // whether a vector is among the values in those places.
  bool accesses_before_checks(GrowableArray<MachNode*>& accesses, GrowableArray<Node*>* masks = nullptr) {
    bool vector_lost = false;
    if (_words == 0) {
      return vector_lost;
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
            if (_losable.at(place) && !defines(n, place) && test(_live, place)) {
              lost = true;
              vector_lost |= _vector.at(place);
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
    return vector_lost;
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

static bool is_general_purpose_ideal_reg(uint ideal_reg) {
  return ideal_reg == Op_RegI || ideal_reg == Op_RegN || ideal_reg == Op_RegL || ideal_reg == Op_RegP;
}

// Whether the mask or vector n can be computed again anywhere its definition dominates, by a copy
// of its instruction: the instruction has no TEMPs, kills or control, reads no memory, and its
// inputs are constants, integer or pointer values (which a compilation with unsafe accesses keeps
// out of losable registers) and other masks and vectors. A copy in front of a reader computes the
// value the definition computed, from inputs that a fault cannot have changed: the masks and
// vectors among them are then read for the check of that reader, so they are found and protected
// in turn.
static bool is_recomputable(const Node* n) {
  if (!n->is_Mach() || n->is_MachSpillCopy() || n->is_MachTemp() || n->is_MachCall() ||
      (n->in(0) != nullptr && !n->in(0)->is_Root())) {
    return false;
  }
  for (uint k = 1; k < n->req(); k++) {
    const Node* in = n->in(k);
    if (in == nullptr || in->is_MachTemp() || in->bottom_type() == Type::MEMORY) {
      return false;
    }
    if (!in->is_Con() && !in->is_MachConstantBase() && !is_mask_value(in) && !is_vector_value(in) &&
        !is_general_purpose_ideal_reg(value_ideal_reg(in))) {
      return false;
    }
  }
  for (DUIterator_Fast kmax, k = n->fast_outs(kmax); k < kmax; k++) {
    if (n->fast_out(k)->is_Proj()) {
      return false;
    }
  }
  return true;
}

// Whether keeping value, which is_recomputable(), on the stack costs less than computing it again
// for the readers that check it, by the frequencies of the blocks. On the stack it costs a store
// where it is defined and a load for each reader. Computed again it costs its instruction for each
// reader, and for each mask or vector among its inputs what that input costs in turn: its
// instruction for each reader if it can be computed again, else a load for each reader and a store
// where the input is defined. The mask of a loop with a masked tail, which is read for a check in a
// slow path and made from the lane numbers, is computed again; a compare of a vector that the loop
// has just loaded, which is tested on every iteration, is kept, instead of the vector.
static bool is_cheaper_on_stack(PhaseCFG* cfg, const Node* value, const Block* block,
                                const GrowableArray<Node*>& readers, const GrowableArray<uint>& inputs) {
  double reads = 0;
  for (int r = 0; r < readers.length(); r++) {
    const Node* reader = readers.at(r);
    const Block* reader_block = cfg->get_block_for_node(reader);
    if (reader->is_Phi()) {
      reader_block = cfg->get_block_for_node(reader_block->pred(inputs.at(r)));
    }
    reads += reader_block->_freq;
  }
  const double on_stack = block->_freq + reads;
  double again = reads;
  for (uint k = 1; k < value->req(); k++) {
    const Node* in = value->in(k);
    if (!is_mask_value(in) && !is_vector_value(in)) {
      continue;
    }
    again += reads;
    if (!(is_mask_value(in) && is_constant_mask(in)) && !is_recomputable(in)) {
      const Block* in_block = cfg->get_block_for_node(in);
      if (in_block == nullptr) {
        return false;
      }
      again += in_block->_freq;
    }
  }
  return on_stack < again;
}

// Moves reader, a mask that is made from its inputs alone (see is_recomputable()) and that follows a
// raw access of block, up to position at, in front of the access, if its inputs are defined there;
// those that are defined later in the block and can be made anywhere (the copies of constants that
// an earlier round put in front of the reader) move with it. The reader then reads its inputs
// before the access, and what crosses the access for the check is its result, a mask, which is
// kept on the stack in a few bytes where a vector among the inputs would take a slot of its own
// size (64 bytes for a zmm register, which no frame aligns), or which crosses no access at all
// once the check that reads it is moved behind it (see move_check_behind_value()). Where the masks
// are not followed (masks_followed is false: they are vectors, or their registers are killed at
// every raw access), the reader is a vector instead. Returns whether it moved the reader.
static bool move_in_front_of_accesses(PhaseCFG* cfg, Block* block, Node* reader, uint at, VectorSet& moved,
                                      bool masks_followed) {
  if (!(masks_followed ? is_mask_value(reader) : is_vector_value(reader)) || !is_recomputable(reader) ||
      moved.test(reader->_idx)) {
    return false;
  }
  GrowableArray<Node*> late;
  for (uint k = 1; k < reader->req(); k++) {
    Node* in = reader->in(k);
    const Block* in_block = cfg->get_block_for_node(in);
    if (in_block == nullptr) {
      return false;
    }
    if (in_block != block || block->find_node(in) < at) {
      continue;
    }
    if (!is_recomputable(in) && !(is_mask_value(in) && is_constant_mask(in))) {
      return false;
    }
    for (uint j = 1; j < in->req(); j++) {
      const Block* from = cfg->get_block_for_node(in->in(j));
      if (from == nullptr || (from == block && block->find_node(in->in(j)) >= at)) {
        return false;
      }
    }
    late.append_if_missing(in);
  }
  uint to = at;
  for (int i = 0; i < late.length(); i++) {
    block->find_remove(late.at(i));
    block->insert_node(late.at(i), to++);
  }
  block->find_remove(reader);
  block->insert_node(reader, to);
  moved.set(reader->_idx);
  return true;
}

// Whether the condition codes are live in front of position at of block: set before it and read at
// or after it.
static bool flags_live_at(PhaseCFG* cfg, const Block* block, uint at) {
  for (uint i = 1; i < block->number_of_nodes(); i++) {
    const Node* n = block->get_node(i);
    if (i < at) {
      if (value_ideal_reg(n) != Op_RegFlags) {
        continue;
      }
      for (DUIterator_Fast kmax, k = n->fast_outs(kmax); k < kmax; k++) {
        const Node* use = n->fast_out(k);
        if (cfg->get_block_for_node(use) != block || block->find_node(use) >= at) {
          return true;
        }
      }
    } else {
      for (uint k = 1; k < n->req(); k++) {
        const Node* in = n->in(k);
        if (in != nullptr && in->is_Mach() && value_ideal_reg(in) == Op_RegFlags &&
            cfg->get_block_for_node(in) != block) {
          return true;
        }
      }
    }
  }
  return false;
}

// Moves reader, which reads the mask or vector value for a check and whose result is an integer (the
// bits of a mask as a long, the number or the position of its set lanes), to position at of block,
// right behind the definition of value, if it can run there: it has no control and reads no memory,
// its other inputs are defined there, and what it kills besides its TEMPs is the condition codes,
// which are then not live there. The TEMPs and the kill move with it. The check then reads value
// before any raw access can follow, and what crosses the accesses up to the test of the integer is
// a general purpose register, which a compilation with unsafe accesses keeps out of the losable
// ones (NoTrapPlatform::allocation_exclusion(), gpr_spill_exclusion()). The instruction computes
// the same integer at either place, since value is the same at both. Returns whether it moved the
// reader.
static bool move_check_behind_value(PhaseCFG* cfg, Block* block, uint at, Node* reader, const Node* value) {
  if (!reader->is_Mach() || reader->is_MachSpillCopy() || reader->is_MachTemp() || reader->is_MachReturn() ||
      reader->is_MachBranch() || reader->is_MachNullCheck() || reader->is_MachConstant() ||
      (reader->in(0) != nullptr && !reader->in(0)->is_Root()) ||
      !is_general_purpose_ideal_reg(value_ideal_reg(reader))) {
    return false;
  }
  Block* reader_block = cfg->get_block_for_node(reader);
  if (reader_block == nullptr || (reader_block != block && !block->dominates(reader_block))) {
    return false;
  }
  GrowableArray<Node*> temps;
  for (uint k = 1; k < reader->req(); k++) {
    Node* in = reader->in(k);
    if (in == nullptr || in->bottom_type() == Type::MEMORY) {
      return false;
    }
    Block* in_block = cfg->get_block_for_node(in);
    if (in_block == nullptr) {
      return false;
    }
    if (in->is_MachTemp()) {
      if (in->outcnt() != 1) {
        return false;
      }
      if (in_block != block || block->find_node(in) >= at) {
        temps.append_if_missing(in);
      }
      continue;
    }
    if (in == value) {
      continue;
    }
    if (in_block == block ? block->find_node(in) >= at : !in_block->dominates(block)) {
      return false;
    }
  }
  GrowableArray<Node*> kills;
  for (DUIterator_Fast kmax, k = reader->fast_outs(kmax); k < kmax; k++) {
    Node* out = reader->fast_out(k);
    if (!out->is_Proj()) {
      continue;
    }
    if (!out->is_MachProj() || out->as_MachProj()->_ideal_reg != Op_RegFlags || out->outcnt() != 0 ||
        cfg->get_block_for_node(out) != reader_block) {
      return false;
    }
    kills.append(out);
  }
  if (!kills.is_empty() && flags_live_at(cfg, block, at)) {
    return false;
  }
  uint to = at;
  for (int i = 0; i < temps.length(); i++) {
    Node* temp = temps.at(i);
    cfg->get_block_for_node(temp)->find_remove(temp);
    block->insert_node(temp, to++);
    cfg->map_node_to_block(temp, block);
  }
  reader_block->find_remove(reader);
  block->insert_node(reader, to++);
  cfg->map_node_to_block(reader, block);
  for (int i = 0; i < kills.length(); i++) {
    Node* kill = kills.at(i);
    reader_block->find_remove(kill);
    block->insert_node(kill, to++);
    cfg->map_node_to_block(kill, block);
  }
  log_debug(jit, compilation)("no-trap: %s %u (%s), a check of %s %u, is made behind it", NodeClassNames[reader->Opcode()],
                              reader->_idx, NodeClassNames[reader->as_Mach()->ideal_Opcode()],
                              NodeClassNames[value->Opcode()], value->_idx);
  return true;
}

// Collects n, a node of block, and the nodes of block it reads, inputs first, if all of them can be
// made in front of position at of block to, which dominates block: n itself (depth 0) is a raw load,
// whose control, memory and other inputs are all defined there, so that to is a block the scheduler
// could have chosen for it; a node it reads is one without control or memory, whose inputs are
// defined there in turn. No node may define more than its value, its TEMPs and the condition codes
// (kills_flags tells whether one sets them).
static bool collect_hoisted(PhaseCFG* cfg, const Block* block, Block* to, uint at, Node* n, int depth,
                            GrowableArray<Node*>& nodes, bool& kills_flags) {
  if (nodes.contains(n)) {
    return true;
  }
  if (depth > 4 || !n->is_Mach() || n->is_MachSpillCopy() || n->is_MachReturn() || n->is_MachBranch() ||
      n->is_MachNullCheck() || n->bottom_type() == Type::MEMORY) {
    return false;
  }
  if (n->in(0) != nullptr && !n->in(0)->is_Root()) {
    Block* control = depth == 0 ? cfg->get_block_for_node(n->in(0)) : nullptr;
    if (control == nullptr || control == block || !control->dominates(to)) {
      return false;
    }
  }
  for (uint k = 1; k < n->len(); k++) { // the precedence edges too
    Node* in = n->in(k);
    if (in == nullptr) {
      continue;
    }
    if (depth > 0 && in->bottom_type() == Type::MEMORY) {
      return false;
    }
    Block* in_block = cfg->get_block_for_node(in);
    if (in_block == nullptr) {
      return false;
    }
    if (in_block == block) {
      if (in->is_MachTemp() ? in->outcnt() != 1
                            : !collect_hoisted(cfg, block, to, at, in, depth + 1, nodes, kills_flags)) {
        return false;
      }
      if (in->is_MachTemp()) {
        nodes.append_if_missing(in);
      }
    } else if (in_block == to ? to->find_node(in) >= at : !in_block->dominates(to)) {
      return false;
    }
  }
  for (DUIterator_Fast kmax, k = n->fast_outs(kmax); k < kmax; k++) {
    const Node* out = n->fast_out(k);
    if (out->is_MachNullCheck()) {
      return false;
    }
    if (!out->is_Proj()) {
      continue;
    }
    if (!out->is_MachProj() || out->as_MachProj()->_ideal_reg != Op_RegFlags || out->outcnt() != 0 ||
        cfg->get_block_for_node(out) != block) {
      return false;
    }
    kills_flags = true;
  }
  nodes.append(n);
  return true;
}

// Moves the raw accesses of block, which starts with the Phi of a mask or vector that reaches a
// check across them, to the end of the block that immediately dominates it, in front of its branch,
// if every one of them is a load that can be made there and that block does not run more often.
// The mask of a loop iteration that is all lanes or the lanes left (S.indexInRange()) is such a Phi,
// behind a branch on the lanes left, and a raw read of the same iteration that the scheduler put
// behind the Phi is one the Phi is live across, although the read does not depend on the branch:
// in front of the branch, nothing of the Phi is live across it, where the Phi would be kept on the
// stack with a store on every iteration. The scheduler could have put each load there itself: its
// control, its memory state and its other inputs are defined there (the nodes of block that compute
// its address move with it), and a load does not change what the nodes it now precedes read. The
// search for the values that reach a check is made again on the code as it then is. Returns whether
// it moved the accesses.
static bool hoist_accesses_above_merge(PhaseCFG* cfg, Block* block, VectorSet& moved) {
  Block* to = block->_idom;
  if (to == nullptr || to == block || to->_freq > block->_freq * 1.01 || to->number_of_nodes() < 2) {
    return false;
  }
  // In front of the branch of the block and of what sets the condition codes for it.
  uint at = to->end_idx();
  const Node* end = to->get_node(at);
  if (!end->is_MachIf() && !end->is_MachGoto()) {
    return false;
  }
  for (uint k = 1; k < end->req(); k++) {
    const Node* in = end->in(k);
    if (in != nullptr && value_ideal_reg(in) == Op_RegFlags && cfg->get_block_for_node(in) == to) {
      at = MIN2(at, to->find_node(in));
    }
  }
  GrowableArray<Node*> nodes;
  bool kills_flags = false;
  bool any = false;
  for (uint i = first_after_phis(block); i < block->number_of_nodes(); i++) {
    Node* n = block->get_node(i);
    if (n->is_Mach() && Matcher::is_raw_memory_access(n->as_Mach())) {
      // An access is moved once: one that was moved here stays, and the Phi is kept on the stack.
      if (moved.test(n->_idx) || !collect_hoisted(cfg, block, to, at, n, 0, nodes, kills_flags)) {
        return false;
      }
      any = true;
    }
  }
  if (!any || (kills_flags && flags_live_at(cfg, to, at))) {
    return false;
  }
  for (int i = 0; i < nodes.length(); i++) {
    Node* n = nodes.at(i);
    moved.set(n->_idx);
    block->find_remove(n);
    to->insert_node(n, at++);
    cfg->map_node_to_block(n, to);
    for (DUIterator_Fast kmax, k = n->fast_outs(kmax); k < kmax; k++) {
      Node* out = n->fast_out(k);
      if (out->is_Proj()) {
        block->find_remove(out);
        to->insert_node(out, at++);
        cfg->map_node_to_block(out, to);
      }
    }
  }
  log_debug(jit, compilation)("no-trap: the raw accesses of B%d are made in B%d, in front of a merge of masks or vectors",
                              block->_pre_order, to->_pre_order);
  return true;
}

// Logs why value, which reaches a check across a raw access, gets no copy.
static void log_unprotected(const Node* value, const char* reason) {
  log_debug(jit, compilation)("no-trap: no copy for %s %u (%s): %s", NodeClassNames[value->Opcode()], value->_idx,
                              value->is_Mach() ? NodeClassNames[value->as_Mach()->ideal_Opcode()] : "-", reason);
}

// The masks and vectors that the raw accesses found by CheckFlow would lose (values, which can be in
// losable registers across such an access and reach a check) usually have many readers that do not
// check, on the hot path, and a few that do, in the slow paths: the mask of a loop iteration is the
// predicate of its loads, stores and arithmetic, and the Vector API tests it only where an access is
// not wholly in bounds, against a vector of lane numbers that the loop keeps in a register. A kill
// at the access would put the whole live range on the stack (PhaseChaitin::Split() has no place at
// which to split it: the kill is an interference, not register pressure) and make every reader
// reload it. Instead, this leaves the value in its register for the readers that do not check,
// where zeroed lanes are harmless, and gives each reader that checks a value that a fault cannot
// have zeroed:
// - for a constant mask (see is_constant_mask()) and for a value that can be computed again from
//   values that are not losable (see is_recomputable(): the mask of a loop iteration, a compare of
//   the lane numbers with a broadcast of the lanes left), a copy of its definition right in front
//   of the reader, which costs the paths that do not check nothing, unless the stack is cheaper
//   (see is_cheaper_on_stack());
// - for any other mask, a copy bound to the stack that is made right after the definition (keep),
//   and a reload of that copy right in front of the reader. Neither is marked as a copy, so that
//   the allocator's copy removal, which follows copies to a register that still holds the value,
//   cannot make a reader read the register of the mask, or an earlier reload, again.
// A reader that follows the definition in its block with no raw access in between reads the mask
// itself; and where the masks are followed (masks_followed: no raw access kills the mask registers),
// a reader of a vector that makes a mask from it is moved there (see move_in_front_of_accesses()),
// which the moved set remembers. Where the tests of masks read integers (checks_move, see
// NoTrapPlatform::tests_masks_as_integers()), a check whose result is an integer is moved there as
// well (see move_check_behind_value()), and so is a reader that makes a vector, where the masks are
// vectors. The copy for a Phi goes to the end of the predecessor block.
// Returns the number of values it protected. stacks holds, by ideal register class, the stack slots
// a value of the class can be spilled to, made on demand.
static int protect_values(Compile* C, PhaseCFG* cfg, const CheckFlow& flow, const GrowableArray<Node*>& masks,
                          VectorSet& stack_bound, VectorSet& moved, bool masks_followed, bool checks_move,
                          RegMask** stacks) {
  int count = 0;
  for (int i = 0; i < masks.length(); i++) {
    Node* mask = masks.at(i);
    Block* block = cfg->get_block_for_node(mask);
    if (block == nullptr) {
      log_unprotected(mask, "in no block");
      continue;
    }
    const uint ideal_reg = value_ideal_reg(mask);
    assert(ideal_reg > 0 && ideal_reg < (uint)_last_machine_leaf, "a mask or a vector");
    const RegMask& anywhere = *C->matcher()->idealreg2spillmask[ideal_reg];
    const RegMask& registers = *C->matcher()->idealreg2regmask[ideal_reg];
    if (stacks[ideal_reg] == nullptr) {
      stacks[ideal_reg] = new (C->comp_arena()->Amalloc(sizeof(RegMask))) RegMask(C->comp_arena());
      stacks[ideal_reg]->assignFrom(anywhere);
      stacks[ideal_reg]->subtract(registers);
    }
    const RegMask& stack = *stacks[ideal_reg];
    // The last node of the definition, and where a copy of the mask can follow it.
    const uint def_at = mask->is_Phi() ? first_after_phis(block) - 1 : block->find_node(mask);
    uint keep_at = def_at + 1;
    while (keep_at < block->number_of_nodes() && block->get_node(keep_at)->is_Proj() &&
           block->get_node(keep_at)->in(0) == mask) {
      keep_at++;
    }
    const bool can_keep = keep_at < block->number_of_nodes() && !block->get_node(keep_at)->is_MachNullCheck();
    const bool constant_mask = is_mask_value(mask) && is_constant_mask(mask);
    bool constant = constant_mask || is_recomputable(mask);
    if (!constant && !can_keep) {
      log_unprotected(mask, "nothing can follow it in its block"); // the access of an implicit null check
      continue;
    }
    // A Phi: the raw accesses of its block are made in front of the merge, where they can be, so
    // that the Phi is not live across them and needs no copy, which would cost a store, or a check
    // that is made behind it, wherever the block runs. What the Phi is then still live across is
    // found in the next round.
    if (checks_move && mask->is_Phi() && hoist_accesses_above_merge(cfg, block, moved)) {
      count++;
      continue;
    }

    // The readers that check, each with the input that reads the mask.
    GrowableArray<Node*> users;
    for (DUIterator_Fast kmax, k = mask->fast_outs(kmax); k < kmax; k++) {
      users.append_if_missing(mask->fast_out(k));
    }
    GrowableArray<Node*> readers;
    GrowableArray<uint> inputs;
    bool moved_readers = false;
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
        if (!checks_values(user)) {
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
          // A vector that cannot be computed again, or that was itself made here, and is read,
          // after the access, for a mask (for a vector, where the masks are not followed): the
          // reader is made in front of the access instead, and its result is found in the next
          // round.
          if ((masks_followed || checks_move) && user_at > def_at && (!constant || moved.test(mask->_idx)) &&
              can_keep && is_vector_value(mask) &&
              move_in_front_of_accesses(cfg, block, user, keep_at, moved, masks_followed)) {
            moved_readers = true;
            break;
          }
        }
        // A check whose result is an integer is made right behind the value, in front of the
        // accesses: always where the value would be kept on the stack, which costs a store there,
        // and else unless that place runs much more often than the check.
        if (checks_move && !user->is_Phi() && !flow.is_mask(user) && can_keep &&
            (!constant || moved.test(mask->_idx) || user_block == block || block->_freq <= 2 * user_block->_freq) &&
            move_check_behind_value(cfg, block, keep_at, user, mask)) {
          moved_readers = true;
          break;
        }
        readers.append(user);
        inputs.append(k);
      }
    }
    if (readers.is_empty()) {
      if (moved_readers) {
        count++;
      } else {
        log_unprotected(mask, "no reader that checks after a raw access");
      }
      continue;
    }

    // A mask that was moved in front of the accesses is kept on the stack: computed again for its
    // readers, it would read its inputs after the accesses again.
    if (constant && !constant_mask && can_keep &&
        (moved.test(mask->_idx) || is_cheaper_on_stack(cfg, mask, block, readers, inputs))) {
      constant = false;
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

// Whether n is a vector with every bit of every lane set, made from a constant.
static bool is_all_ones_constant(const Node* n) {
  if (n->Opcode() != Op_Replicate || n->in(1) == nullptr || !n->in(1)->is_Con()) {
    return false;
  }
  const Type* t = n->in(1)->bottom_type();
  switch (t->base()) {
    case Type::Int:       return t->as_int()->get_con() == -1;
    case Type::Long:      return t->as_long()->get_con() == -1;
    case Type::FloatCon:  return jint_cast(t->getf()) == -1;
    case Type::DoubleCon: return jlong_cast(t->getd()) == -1;
    default:              return false;
  }
}

// Whether n is a mask with every lane set, made from a constant.
static bool is_all_true_constant(const Node* n) {
  return is_all_ones_constant(n) || VectorNode::is_all_ones_vector(const_cast<Node*>(n));
}

// The long operation on the bits of two masks that gives the bits of the mask that n, a bitwise
// operation on masks (or on vectors: the bit of a lane is its sign), makes of them; or 0.
static int bits_opcode(const Node* n) {
  if (n->req() != 3) {
    return 0; // an operation under a mask of its own
  }
  switch (n->Opcode()) {
    case Op_AndVMask: case Op_AndV: return Op_AndL;
    case Op_OrVMask:  case Op_OrV:  return Op_OrL;
    case Op_XorVMask: case Op_XorV: return Op_XorL;
    default:                        return 0;
  }
}

// Whether mask is a merge of masks at a place that is not the head of a loop.
static bool is_merge_of_masks(const Node* mask) {
  return mask->is_Phi() && mask->in(0) != nullptr && mask->in(0)->is_Region() && !mask->in(0)->is_Loop();
}

// Whether the bits of mask are better read from the masks it is made of, by bitwise operations or
// by a merge: one of them is tested itself (tested, which holds the masks without their casts), so
// that its bits are in a general purpose register in any case, or is the mask with every lane set,
// whose bits are a constant.
static bool reads_bits_of_inputs(const Node* mask, const VectorSet& tested, int depth) {
  if ((bits_opcode(mask) == 0 && !is_merge_of_masks(mask)) || depth > 3) {
    return false;
  }
  // Masks of one shape: the bits of each are then those of its lanes.
  const TypeVect* vt = mask->bottom_type()->isa_vect();
  for (uint k = 1; k < mask->req(); k++) {
    const TypeVect* in_vt = mask->in(k) == nullptr ? nullptr : mask->in(k)->bottom_type()->isa_vect();
    if (vt == nullptr || in_vt == nullptr || in_vt->element_basic_type() != vt->element_basic_type() ||
        in_vt->length() != vt->length() || in_vt->is_pvectmask() != vt->is_pvectmask()) {
      return false;
    }
  }
  for (uint k = 1; k < mask->req(); k++) {
    const Node* in = mask->in(k)->uncast();
    if (tested.test(in->_idx) || is_all_true_constant(in) || reads_bits_of_inputs(in, tested, depth + 1)) {
      return true;
    }
  }
  return false;
}

// The bits of mask as a long: of a mask that is made by a bitwise operation from masks of which
// one is tested itself, the same operation on their bits, and of a merge of such masks, the merge
// of their bits, so that the check of each mask reads that mask alone and can be made right behind
// it; of the mask with every lane set, the constant every; of any other mask, its conversion (of its
// lanes as bytes first, if as_bytes). A cast of a mask is the mask: the conversion reads the mask
// itself, so that every test of one mask reads one conversion.
static Node* mask_bits(PhaseIterGVN& igvn, Node* mask, const VectorSet& tested, int depth, bool as_bytes,
                       BasicType bt, uint lanes, jlong every) {
  mask = mask->uncast();
  if (is_all_true_constant(mask)) {
    return igvn.longcon(every);
  }
  if (reads_bits_of_inputs(mask, tested, depth) && is_merge_of_masks(mask)) {
    PhiNode* merge = new PhiNode(mask->in(0), TypeLong::LONG);
    for (uint k = 1; k < mask->req(); k++) {
      merge->init_req(k, mask_bits(igvn, mask->in(k), tested, depth + 1, as_bytes, bt, lanes, every));
    }
    return igvn.transform(merge);
  }
  if (reads_bits_of_inputs(mask, tested, depth)) {
    Node* a = mask_bits(igvn, mask->in(1), tested, depth + 1, as_bytes, bt, lanes, every);
    Node* b = mask_bits(igvn, mask->in(2), tested, depth + 1, as_bytes, bt, lanes, every);
    switch (bits_opcode(mask)) {
      case Op_AndL: return igvn.transform(new AndLNode(a, b));
      case Op_OrL:  return igvn.transform(new OrLNode(a, b));
      default:      return igvn.transform(new XorLNode(a, b));
    }
  }
  Node* bits_of = mask;
  if (as_bytes) {
    bits_of = igvn.transform(VectorStoreMaskNode::make(igvn, mask, bt, lanes));
  }
  return igvn.transform(new VectorMaskToLongNode(bits_of, TypeLong::LONG));
}

// Whether the bits of masks of the type vt can be read as a long, and whether they are read from
// the lanes as bytes (as_bytes) or from the mask itself.
static bool converts_to_bits(const TypeVect* vt, bool& as_bytes) {
  const uint lanes = vt->length();
  const BasicType bt = vt->element_basic_type();
  as_bytes = !Matcher::mask_op_prefers_predicate(Op_VectorMaskToLong, vt);
  return lanes <= 64 && Matcher::match_rule_supported_vector(Op_VectorMaskToLong, lanes, bt) &&
         (!as_bytes || (Matcher::match_rule_supported_vector(Op_VectorStoreMask, lanes, bt) &&
                        vt->length_in_bytes() <= 32));
}

// The bits of a mask of lanes lanes with every lane set.
static jlong bits_of_every_lane(uint lanes) {
  return lanes == 64 ? (jlong)-1 : (((jlong)1 << lanes) - 1);
}

// Whether test, a test of all lanes of a mask (all) or of any, can read the bits of the mask as a
// long: it has the shape the Vector API gives it, and the conversion is supported. Its readers are
// then appended to bools.
static bool reads_bits(VectorTestNode* test, bool& all, GrowableArray<BoolNode*>& bools) {
  Node* mask = test->in(1);
  const TypeVect* vt = mask->bottom_type()->isa_vect();
  all = test->get_predicate() == BoolTest::overflow;
  if (vt == nullptr || (!all && test->get_predicate() != BoolTest::ne)) {
    return false;
  }
  // The test of all lanes of a mask that is a vector reads the lanes through its second input,
  // which the Vector API sets to a vector of ones; any other test reads its first input alone.
  if (Matcher::vectortest_needs_second_argument(all, vt->is_pvectmask()) ? !is_all_ones_constant(test->in(2))
                                                                        : test->in(2) != mask) {
    return false;
  }
  bool as_bytes;
  if (!converts_to_bits(vt, as_bytes)) {
    return false;
  }
  // The readers of the test: the test holds if the Bool is the one the Vector API makes for it,
  // and does not hold if it is the opposite one.
  const BoolTest::mask holds = Matcher::vectortest_mask(all, vt->is_pvectmask(), (int)vt->length());
  for (DUIterator_Fast kmax, k = test->fast_outs(kmax); k < kmax; k++) {
    Node* out = test->fast_out(k);
    if (!out->is_Bool() || (out->as_Bool()->_test._test != holds &&
                            out->as_Bool()->_test._test != BoolTest::negate_mask(holds))) {
      return false;
    }
    bools.append(out->as_Bool());
  }
  return !bools.is_empty();
}

void NoTrapSites::test_masks_as_integers(Compile* C, PhaseIterGVN& igvn, const NoTrapPlatform* platform) {
  if (!C->has_unsafe_access() || !platform->tests_masks_as_integers()) {
    return;
  }
  ResourceMark rm;
  Unique_Node_List<Node> useful;
  C->identify_useful_nodes(useful);
  GrowableArray<VectorTestNode*> tests;
  VectorSet tested; // the masks, without their casts, whose own test reads their bits
  int all_tests = 0;
  for (uint i = 0; i < useful.size(); i++) {
    if (useful.at(i)->Opcode() == Op_VectorTest) {
      VectorTestNode* test = static_cast<VectorTestNode*>(useful.at(i));
      bool all;
      GrowableArray<BoolNode*> bools;
      all_tests++;
      if (reads_bits(test, all, bools)) {
        tests.append(test);
        tested.set(test->in(1)->uncast()->_idx);
      }
    }
  }
  if (tests.is_empty()) {
    return;
  }
  for (int i = 0; i < tests.length(); i++) {
    VectorTestNode* test = tests.at(i);
    bool all;
    GrowableArray<BoolNode*> bools;
    if (!reads_bits(test, all, bools)) {
      continue; // not reached: the test is as it was
    }
    Node* mask = test->in(1);
    const TypeVect* vt = mask->bottom_type()->as_vect();
    bool as_bytes;
    converts_to_bits(vt, as_bytes);
    // Every lane set: the bits are those of a mask with every lane set. Any lane set: they are not 0.
    const jlong every = bits_of_every_lane(vt->length());
    const BoolTest::mask holds = Matcher::vectortest_mask(all, vt->is_pvectmask(), (int)vt->length());
    Node* bits = mask_bits(igvn, mask, tested, 0, as_bytes, vt->element_basic_type(), vt->length(), every);
    Node* cmp = igvn.transform(new CmpLNode(bits, igvn.longcon(all ? every : 0)));
    for (int b = 0; b < bools.length(); b++) {
      BoolNode* bol = bools.at(b);
      const bool positive = bol->_test._test == holds;
      Node* as_integer = igvn.transform(new BoolNode(cmp, positive == all ? BoolTest::eq : BoolTest::ne));
      igvn.replace_node(bol, as_integer);
    }
  }
  // The lanes of a mask as booleans (VectorStoreMask), for the debug info of a trap or for a box:
  // a reader of the mask that always checks, behind whatever raw accesses lie between the mask and
  // the trap, and the mask of a loop iteration is named by every trap of the iteration. Where a
  // test reads the bits of that mask, or of the masks it is merged or made from, the booleans are
  // made from the bits, which are in a general purpose register, and the mask has no reader in
  // front of the trap.
  Unique_Node_List<Node> after;
  C->identify_useful_nodes(after);
  int payloads = 0;
  for (uint i = 0; i < after.size(); i++) {
    Node* booleans = after.at(i);
    if (booleans->Opcode() != Op_VectorStoreMask || booleans->in(1) == nullptr) {
      continue;
    }
    Node* mask = booleans->in(1)->uncast();
    const TypeVect* vt = mask->bottom_type()->isa_vect();
    bool as_bytes;
    if (vt == nullptr || !converts_to_bits(vt, as_bytes) ||
        !Matcher::match_rule_supported_vector(Op_VectorLongToMask, vt->length(), vt->element_basic_type()) ||
        (!tested.test(mask->_idx) && !reads_bits_of_inputs(mask, tested, 0))) {
      continue;
    }
    // Every reader of the booleans but the conversions to the bits, which read these booleans
    // where masks are vectors.
    GrowableArray<Node*> readers;
    for (DUIterator_Fast kmax, k = booleans->fast_outs(kmax); k < kmax; k++) {
      if (booleans->fast_out(k)->Opcode() != Op_VectorMaskToLong) {
        readers.append_if_missing(booleans->fast_out(k));
      }
    }
    if (readers.is_empty()) {
      continue;
    }
    Node* bits = mask_bits(igvn, mask, tested, 0, as_bytes, vt->element_basic_type(), vt->length(),
                           bits_of_every_lane(vt->length()));
    // Made where the cast of the mask is, the trap it is made for: with nothing to hold them there,
    // the conversions for all the traps would be one, in a block that runs for all of them.
    Node* control = nullptr;
    for (Node* cast = booleans->in(1); cast->is_ConstraintCast() && control == nullptr; cast = cast->in(1)) {
      control = cast->in(0);
    }
    if (vt->is_pvectmask()) {
      // The mask again, from its bits, for the conversion to booleans.
      Node* again = new VectorLongToMaskNode(bits, vt);
      again->init_req(0, control);
      igvn.replace_input_of(booleans, 1, igvn.transform(again));
    } else {
      // The booleans themselves.
      Node* again = new VectorLongToMaskNode(bits, booleans->bottom_type()->as_vect());
      again->init_req(0, control);
      again = igvn.transform(again);
      for (int r = 0; r < readers.length(); r++) {
        Node* reader = readers.at(r);
        for (uint k = 0; k < reader->req(); k++) {
          if (reader->in(k) == booleans) {
            igvn.replace_input_of(reader, k, again);
          }
        }
      }
    }
    payloads++;
  }
  log_debug(jit, compilation)("no-trap: %d of %d tests of masks of %s read the mask as an integer, %d masks are made "
                              "again from their bits", tests.length(), all_tests, method_name(C), payloads);
  igvn.optimize();
}

bool NoTrapSites::kill_follows_masks(const Compile* C, const NoTrapPlatform* platform) {
  return C->has_unsafe_access() && platform->raw_access_kill() != nullptr &&
         platform->raw_access_kill_follows_masks() && !C->kill_masks_at_every_raw_access();
}

void NoTrapSites::protect_checked_values(Compile* C, const NoTrapPlatform* platform) {
  if (!C->has_unsafe_access()) {
    return;
  }
  ResourceMark rm;
  PhaseCFG* cfg = C->cfg();
  const bool masks_losable = kill_follows_masks(C, platform);
  RegMask* stacks[_last_machine_leaf];
  for (uint i = 0; i < (uint)_last_machine_leaf; i++) {
    stacks[i] = nullptr;
  }
  VectorSet stack_bound;
  VectorSet moved;
  GrowableArray<MachNode*> accesses;
  bool vector_lost = false;
  int protected_values = 0;
  for (int round = 0; ; round++) {
    accesses.clear();
    GrowableArray<Node*> values;
    CheckFlow flow(C, nullptr, platform, masks_losable, &stack_bound);
    vector_lost = flow.accesses_before_checks(accesses, &values);
    // The copies of a protected value are read right where they are made, and a value computed
    // from them can itself be found, so a few rounds are enough; a chain of readers that are made
    // in front of the accesses takes one round for each.
    if (round == 16) {
      break;
    }
    const int protected_now = protect_values(C, cfg, flow, values, stack_bound, moved, masks_losable,
                                             platform->tests_masks_as_integers(), stacks);
    if (protected_now == 0) {
      break;
    }
    protected_values += protected_now;
  }
  if (vector_lost) {
    // A vector that could not be given a copy (nothing can follow its definition in its block).
    // There is no kill for vectors: the compilation is made again without them.
    log_debug(jit, compilation)("no-trap: a vector that reaches a check cannot be kept on the stack across a raw "
                                "access of %s", method_name(C));
    if (C->avoid_vectors()) {
      C->record_method_not_compilable("a vector that reaches a check is live across a raw access");
    } else {
      C->record_failure(C2Compiler::retry_avoid_vectors());
    }
    return;
  }
  // Whatever mask is left keeps the kill at the accesses it is live across.
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
  log_debug(jit, compilation)("no-trap: %d raw accesses of %s kill the mask registers, %d values protected for their checks",
                              accesses.length(), method_name(C), protected_values);
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

  // A mask or a vector that reaches a check must not be in a losable register across a raw access
  // (see protect_checked_values(), which gave the readers that check their own copies before the
  // allocator moved or copied anything). Where the allocator undid that for a mask, every raw access
  // kills the mask registers in the next attempt; for a vector there is no such kill, and the next
  // attempt has no vectors.
  if (C->has_unsafe_access()) {
    GrowableArray<MachNode*> lossy_accesses;
    CheckFlow flow(C, ra, platform, kill_follows_masks(C, platform));
    const bool vector_lost = flow.accesses_before_checks(lossy_accesses);
    if (!lossy_accesses.is_empty()) {
      log_debug(jit, compilation)("no-trap: a %s that reaches a check is in a losable register across a raw access "
                                  "of %s, retrying %s", vector_lost ? "vector" : "mask", method_name(C),
                                  vector_lost ? "without vectors" : "with the kill at every raw access");
      if (!vector_lost) {
        C->record_failure(C2Compiler::retry_kill_masks_at_every_raw_access());
      } else if (!C->avoid_vectors()) {
        C->record_failure(C2Compiler::retry_avoid_vectors());
      } else {
        C->record_method_not_compilable("a vector that reaches a check is in a losable register across a raw access");
      }
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
