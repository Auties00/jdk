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

/*
 * @test
 * @summary Test carryless multiply methods
 * @key randomness
 * @run testng/othervm -XX:+UnlockDiagnosticVMOptions -XX:DisableIntrinsic=_carrylessMultiply_i,_carrylessMultiply_l CarrylessMultiplyTest
 * @run testng CarrylessMultiplyTest
 */

import org.testng.annotations.DataProvider;
import org.testng.annotations.Test;

import java.util.Random;

import static org.testng.Assert.assertEquals;

public class CarrylessMultiplyTest {

    // Reference implementation for int
    static int refCarrylessMultiply(int a, int b) {
        int result = 0;
        for (int bit = 0; bit < 32; bit++) {
            if (((b >>> bit) & 1) != 0) {
                result ^= (a << bit);
            }
        }
        return result;
    }

    // Reference implementation for long
    static long refCarrylessMultiply(long a, long b) {
        long result = 0;
        for (int bit = 0; bit < 64; bit++) {
            if (((b >>> bit) & 1) != 0) {
                result ^= (a << bit);
            }
        }
        return result;
    }

    @Test
    public void testIntIdentities() {
        // clmul(x, 0) == 0
        assertEquals(Integer.carrylessMultiply(0xCAFEBABE, 0), 0);
        assertEquals(Integer.carrylessMultiply(0, 0xCAFEBABE), 0);

        // clmul(x, 1) == x
        assertEquals(Integer.carrylessMultiply(0xCAFEBABE, 1), 0xCAFEBABE);
        assertEquals(Integer.carrylessMultiply(1, 0xCAFEBABE), 0xCAFEBABE);

        // clmul(x, 2) == x << 1
        assertEquals(Integer.carrylessMultiply(0x12345678, 2), 0x2468ACF0);
    }

    @Test
    public void testLongIdentities() {
        // clmul(x, 0) == 0
        assertEquals(Long.carrylessMultiply(0xCAFEBABEDEADBEEFL, 0L), 0L);
        assertEquals(Long.carrylessMultiply(0L, 0xCAFEBABEDEADBEEFL), 0L);

        // clmul(x, 1) == x
        assertEquals(Long.carrylessMultiply(0xCAFEBABEDEADBEEFL, 1L), 0xCAFEBABEDEADBEEFL);
        assertEquals(Long.carrylessMultiply(1L, 0xCAFEBABEDEADBEEFL), 0xCAFEBABEDEADBEEFL);

        // clmul(x, 2) == x << 1
        assertEquals(Long.carrylessMultiply(0x123456789ABCDEF0L, 2L), 0x2468ACF13579BDE0L);
    }

    @Test
    public void testCommutativity() {
        assertEquals(Integer.carrylessMultiply(0xABCD, 0x1234),
                     Integer.carrylessMultiply(0x1234, 0xABCD));
        assertEquals(Long.carrylessMultiply(0xABCDL, 0x1234L),
                     Long.carrylessMultiply(0x1234L, 0xABCDL));
    }

    @Test
    public void testPrefixXor() {
        // clmul(x, -1) computes prefix XOR - the key simdjson use case
        // For 0b101 = 5: prefix XOR = 0b011 = 3
        assertEquals(Long.carrylessMultiply(5L, -1L), 3L);

        // For 0b1 = 1: prefix XOR = -1 (all ones)
        assertEquals(Long.carrylessMultiply(1L, -1L), -1L);
    }

    @Test
    public void testKnownValues() {
        // 0xFF * 0xFF in GF(2) = 0x5555
        assertEquals(Integer.carrylessMultiply(0xFF, 0xFF), 0x5555);
        assertEquals(Long.carrylessMultiply(0xFFL, 0xFFL), 0x5555L);
    }

    @Test
    public void testRandomInt() {
        Random rng = new Random(42);
        for (int i = 0; i < 100_000; i++) {
            int a = rng.nextInt();
            int b = rng.nextInt();
            assertEquals(Integer.carrylessMultiply(a, b), refCarrylessMultiply(a, b),
                    String.format("clmul(%08X, %08X)", a, b));
        }
    }

    @Test
    public void testRandomLong() {
        Random rng = new Random(42);
        for (int i = 0; i < 100_000; i++) {
            long a = rng.nextLong();
            long b = rng.nextLong();
            assertEquals(Long.carrylessMultiply(a, b), refCarrylessMultiply(a, b),
                    String.format("clmul(%016X, %016X)", a, b));
        }
    }

    @Test
    public void testEdgeCases() {
        // All ones
        int intResult = Integer.carrylessMultiply(-1, -1);
        assertEquals(intResult, refCarrylessMultiply(-1, -1));

        long longResult = Long.carrylessMultiply(-1L, -1L);
        assertEquals(longResult, refCarrylessMultiply(-1L, -1L));

        // Min values
        assertEquals(Integer.carrylessMultiply(Integer.MIN_VALUE, Integer.MIN_VALUE), 0);
        assertEquals(Long.carrylessMultiply(Long.MIN_VALUE, Long.MIN_VALUE), 0L);

        // Max values
        assertEquals(Integer.carrylessMultiply(Integer.MAX_VALUE, 1), Integer.MAX_VALUE);
        assertEquals(Long.carrylessMultiply(Long.MAX_VALUE, 1L), Long.MAX_VALUE);
    }
}
