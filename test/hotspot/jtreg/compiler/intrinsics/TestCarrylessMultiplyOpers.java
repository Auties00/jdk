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
 */

/**
 * @test
 * @summary To test intrinsic for CARRYLESS_MULTIPLY operations
 * @requires vm.compiler2.enabled
 * @requires (((os.arch=="amd64" | os.arch=="x86_64") &
 *            vm.cpu.features ~= ".*clmul.*") |
 *            (os.arch=="aarch64" & vm.cpu.features ~= ".*pmull.*"))
 * @library /test/lib /
 * @run driver compiler.intrinsics.TestCarrylessMultiplyOpers
 */
package compiler.intrinsics;

import compiler.lib.ir_framework.*;
import jdk.test.lib.Utils;
import java.util.Random;

public class TestCarrylessMultiplyOpers {
    int [] ri;
    int [] ai;
    int [] bi;

    long [] rl;
    long [] al;
    long [] bl;

    // ================ CarrylessMultiply Int ================= //

    @Test
    @IR(counts = {IRNode.CARRYLESS_MULTIPLY, " > 0 "})
    public void testClmulInt(int[] ri, int[] ai, int[] bi) {
        for (int i = 0; i < ri.length; i++) {
           ri[i] = Integer.carrylessMultiply(ai[i], bi[i]);
        }
    }

    @Run(test = {"testClmulInt"}, mode = RunMode.STANDALONE)
    public void kernel_testClmulInt() {
        for (int i = 0; i < 5000; i++) {
            testClmulInt(ri, ai, bi);
        }
        verifyClmulInts(ri, ai, bi);
    }

    // ================ CarrylessMultiply Long ================= //

    @Test
    @IR(counts = {IRNode.CARRYLESS_MULTIPLY, " > 0 "})
    public void testClmulLong(long[] rl, long[] al, long[] bl) {
        for (int i = 0; i < rl.length; i++) {
           rl[i] = Long.carrylessMultiply(al[i], bl[i]);
        }
    }

    @Run(test = {"testClmulLong"}, mode = RunMode.STANDALONE)
    public void kernel_testClmulLong() {
        for (int i = 0; i < 5000; i++) {
            testClmulLong(rl, al, bl);
        }
        verifyClmulLongs(rl, al, bl);
    }

    // ================ Prefix XOR (common use case) ================= //

    @Test
    public void testPrefixXor() {
        long resL;
        int resI;
        for (int i = 0; i < 5000; i++) {
           // Prefix XOR: clmul(x, -1)
           resL = Long.carrylessMultiply(al[i & (SIZE - 1)], -1L);
           verifyClmulLong(resL, al[i & (SIZE - 1)], -1L);

           resI = Integer.carrylessMultiply(ai[i & (SIZE - 1)], -1);
           verifyClmulInt(resI, ai[i & (SIZE - 1)], -1);

           // Identity: clmul(x, 1) == x
           resL = Long.carrylessMultiply(al[i & (SIZE - 1)], 1L);
           verifyClmulLong(resL, al[i & (SIZE - 1)], 1L);

           resI = Integer.carrylessMultiply(ai[i & (SIZE - 1)], 1);
           verifyClmulInt(resI, ai[i & (SIZE - 1)], 1);

           // Zero: clmul(x, 0) == 0
           resL = Long.carrylessMultiply(al[i & (SIZE - 1)], 0L);
           verifyClmulLong(resL, al[i & (SIZE - 1)], 0L);

           resI = Integer.carrylessMultiply(ai[i & (SIZE - 1)], 0);
           verifyClmulInt(resI, ai[i & (SIZE - 1)], 0);

           // Commutativity
           resL = Long.carrylessMultiply(al[i & (SIZE - 1)], bl[i & (SIZE - 1)]);
           long resL2 = Long.carrylessMultiply(bl[i & (SIZE - 1)], al[i & (SIZE - 1)]);
           if (resL != resL2) {
               throw new Error("commutativity failed for long: " +
                   al[i & (SIZE - 1)] + ", " + bl[i & (SIZE - 1)]);
           }

           // Known value
           resI = Integer.carrylessMultiply(0xFF, 0xFF);
           if (resI != 0x5555) {
               throw new Error("clmul(0xFF, 0xFF) expected 0x5555, got " + Integer.toHexString(resI));
           }
        }
    }

    // ===================================================== //

    private static final Random R = Utils.getRandomInstance();

    static int refClmulInt(int a, int b) {
        int result = 0;
        for (int bit = 0; bit < 32; bit++) {
            if (((b >>> bit) & 1) != 0) {
                result ^= (a << bit);
            }
        }
        return result;
    }

    static long refClmulLong(long a, long b) {
        long result = 0;
        for (int bit = 0; bit < 64; bit++) {
            if (((b >>> bit) & 1) != 0) {
                result ^= (a << bit);
            }
        }
        return result;
    }

    static void verifyClmulInt(int actual, int a, int b) {
        int exp = refClmulInt(a, b);
        if (actual != exp) {
            throw new Error("clmul_int: a = " + a + " b = " + b +
                            " actual = " + actual + " expected = " + exp);
        }
    }

    static void verifyClmulInts(int[] actual_res, int[] a_arr, int[] b_arr) {
        for (int i = 0; i < actual_res.length; i++) {
            verifyClmulInt(actual_res[i], a_arr[i], b_arr[i]);
        }
    }

    static void verifyClmulLong(long actual, long a, long b) {
        long exp = refClmulLong(a, b);
        if (actual != exp) {
            throw new Error("clmul_long: a = " + a + " b = " + b +
                            " actual = " + actual + " expected = " + exp);
        }
    }

    static void verifyClmulLongs(long[] actual_res, long[] a_arr, long[] b_arr) {
        for (int i = 0; i < actual_res.length; i++) {
            verifyClmulLong(actual_res[i], a_arr[i], b_arr[i]);
        }
    }

    static final int SIZE = 512;

    public TestCarrylessMultiplyOpers() {
        ri = new int[SIZE];
        ai = new int[SIZE];
        bi = new int[SIZE];
        for (int i = 0; i < SIZE; i++) {
            ai[i] = R.nextInt();
            bi[i] = R.nextInt();
        }

        rl = new long[SIZE];
        al = new long[SIZE];
        bl = new long[SIZE];
        for (int i = 0; i < SIZE; i++) {
            al[i] = R.nextLong();
            bl[i] = R.nextLong();
        }
    }

    public static void main(String[] args) {
        TestFramework.runWithFlags("-XX:-TieredCompilation",
                                   "-XX:CompileThresholdScaling=0.3");
    }
}
