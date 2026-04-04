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

#include "opto/intrinsicnode.hpp"
#include "unittest.hpp"

TEST_VM(opto, carryless_multiply) {
  // Identity: clmul(x, 0) == 0
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(0xCAFEBABELL, 0LL, 64), 0LL);
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(0LL, 0xCAFEBABELL, 64), 0LL);

  // Identity: clmul(x, 1) == x
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(0xCAFEBABELL, 1LL, 64), (jlong)0xCAFEBABELL);
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(1LL, 0xCAFEBABELL, 64), (jlong)0xCAFEBABELL);

  // clmul(x, 2) == x << 1 (shift by one)
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(0x12345678LL, 2LL, 64), 0x2468ACF0LL);

  // Commutativity: clmul(a, b) == clmul(b, a)
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(0xABCDLL, 0x1234LL, 64),
            CarrylessMultiplyNode::carryless_multiply(0x1234LL, 0xABCDLL, 64));

  // Prefix XOR: clmul(x, -1) computes cumulative XOR
  // For 0b1010 (10), prefix XOR should be 0b1100...0001 pattern
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(0x5LL, -1LL, 64), 0x3LL);

  // 32-bit variants
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(0xCAFEBABELL, 0LL, 32), 0LL);
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(0xCAFEBABELL, 1LL, 32), (jlong)(jint)0xCAFEBABELL);

  // Known value: clmul(0xFF, 0xFF) in 32-bit
  // 0xFF * 0xFF in GF(2) = 0x5555 (alternating bits pattern)
  ASSERT_EQ(CarrylessMultiplyNode::carryless_multiply(0xFFLL, 0xFFLL, 32), 0x5555LL);
}
