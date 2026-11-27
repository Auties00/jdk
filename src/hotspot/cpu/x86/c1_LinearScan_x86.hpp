/*
 * Copyright (c) 2005, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef CPU_X86_C1_LINEARSCAN_X86_HPP
#define CPU_X86_C1_LINEARSCAN_X86_HPP

inline bool LinearScan::is_processed_reg_num(int reg_num) {
  // rsp and rbp, r10, r15 (numbers [12,15]) are ignored
  // r12 (number 11) is conditional on compressed oops.
  assert(FrameMap::r12_opr->cpu_regnr() == 11, "wrong assumption below");
  assert(FrameMap::r10_opr->cpu_regnr() == 12, "wrong assumption below");
  assert(FrameMap::r15_opr->cpu_regnr() == 13, "wrong assumption below");
  assert(FrameMap::rsp_opr->cpu_regnrLo() == 14, "wrong assumption below");
  assert(FrameMap::rbp_opr->cpu_regnrLo() == 15, "wrong assumption below");
  assert(reg_num >= 0, "invalid reg_num");
  return reg_num <= FrameMap::last_cpu_reg() || reg_num >= pd_nof_cpu_regs_frame_map;
}

inline int LinearScan::num_physical_regs(BasicType type) {
  return 1;
}


inline bool LinearScan::requires_adjacent_regs(BasicType type) {
  return false;
}

inline bool LinearScan::is_caller_save(int assigned_reg) {
  assert(assigned_reg >= 0 && assigned_reg < nof_regs, "should call this only for registers");
  return true; // no callee-saved registers on Intel

}


inline void LinearScan::pd_add_temps(LIR_Op* op) {
  // AvoidLosableStateAtTraps: C1 code takes its implicit exceptions by a trap, which on some
  // Windows kernels loses xmm16-xmm31. After such a trap the frame goes on in one of two ways only:
  // in an exception handler of its own, or, where the exception deoptimizes instead of being
  // thrown (ImplicitNullCheckStub), in the interpreter with the locals of its debug info. In any
  // other case the frame is unwound, or deoptimized into frames that rethrow and are unwound, and
  // nothing reads its registers. So an operation that can throw into a handler of the frame, or
  // that deoptimizes on its exception, destroys xmm16-xmm31, as a call destroys every register: no
  // value is kept there across it. The same holds for every operation that can throw when the
  // locals must stay readable for a debugger. A loop poll is a branch here and loses nothing
  // (LIR_Assembler::safepoint_poll()).
  if (!FrameMap::uses_losable_xmm_regs() || !has_fpu_registers() || op->code() == lir_safepoint) {
    return;
  }
  LIR_OpVisitState visitor;
  visitor.visit(op);
  if (visitor.info_count() == 0 || visitor.has_call()) {
    return; // cannot throw, or destroys every register already
  }
  bool resumes = compilation()->env()->should_retain_local_variables();
  for (int i = 0; i < visitor.info_count() && !resumes; i++) {
    CodeEmitInfo* info = visitor.info_at(i);
    XHandlers* handlers = info->exception_handlers();
    resumes = info->deoptimize_on_exception() || (handlers != nullptr && handlers->length() > 0);
  }
  if (!resumes) {
    return;
  }
  for (int reg = pd_first_xmm_reg + 16; reg < pd_first_xmm_reg + XMMRegister::available_xmm_registers(); reg++) {
    add_temp(reg, op->id(), noUse, T_ILLEGAL);
  }
}


// Implementation of LinearScanWalker

inline bool LinearScanWalker::pd_init_regs_for_alloc(Interval* cur) {
  int last_xmm_reg = pd_first_xmm_reg + XMMRegister::available_xmm_registers() - 1;
  if (AvoidLosableStateAtTraps && !FrameMap::uses_losable_xmm_regs()) {
    // The low 128 bits of xmm0-xmm15 are legacy SSE state, which every exception context has:
    // C1 code that keeps its faulting polls holds nothing else.
    last_xmm_reg = MIN2(last_xmm_reg, pd_first_xmm_reg + 15);
  }
  if (allocator()->gen()->is_vreg_flag_set(cur->reg_num(), LIRGenerator::byte_reg)) {
    assert(cur->type() != T_FLOAT && cur->type() != T_DOUBLE, "cpu regs only");
    _first_reg = pd_first_byte_reg;
    _last_reg = FrameMap::last_byte_reg();
    return true;
  } else if (cur->type() == T_FLOAT || cur->type() == T_DOUBLE) {
    _first_reg = pd_first_xmm_reg;
    _last_reg = last_xmm_reg;
    return true;
  }

  return false;
}

#endif // CPU_X86_C1_LINEARSCAN_X86_HPP
