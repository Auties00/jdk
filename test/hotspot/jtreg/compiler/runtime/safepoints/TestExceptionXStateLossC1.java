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
 */

/*
 * @test id=C1
 * @bug 8365196
 * @summary Doubles that C1 code keeps in xmm16-xmm31 survive its loop polls, and the doubles
 *          that are live into the handler of a null check survive its trap, when the stack below
 *          the compiled frame holds a value that passes for a stale XSAVE header.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler1.enabled
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native/timeout=300 -XX:TieredStopAtLevel=1
 *      -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      -Xlog:os+cpu=info
 *      compiler.runtime.safepoints.TestExceptionXStateLossC1
 */

/*
 * @test id=tiered
 * @bug 8365196
 * @summary The same with every tier, C1 code with profiling included.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler1.enabled
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native/timeout=300
 *      -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      -Xlog:os+cpu=info
 *      compiler.runtime.safepoints.TestExceptionXStateLossC1
 */

package compiler.runtime.safepoints;

/*
 * Twenty-four accumulators, more than xmm0-xmm15 hold, each count the iterations of a loop that
 * polls on every iteration (C1 loops do). An accumulator in xmm16-xmm31 that an incomplete
 * exception context resets comes out short. Each kernel starts with a native call that writes
 * 0x8000000000000000 below the compiled frame's stack pointer, where an affected Windows build
 * reads the XSAVE header of the next exception's context. The first kernel has no exception
 * handler; the second catches the NullPointerException of a field load inside its loop, so its
 * accumulators are live into the handler of that implicit null check.
 */
public class TestExceptionXStateLossC1 {
    static final int ITERS = 100_000;
    static final int NULL_PERIOD = 64;
    static final long SECONDS = Long.getLong("seconds", 10);

    static volatile boolean stop;
    static Object sink;

    static class Holder {
        int x = 1;
    }

    static long kernelPolls(double one, int[] steps) {
        ExceptionXState.fillBelowStack();
        double a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, b7 = 0;
        double c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0, c6 = 0, c7 = 0;
        int mask = steps.length - 1;
        int count = 0;
        for (int i = 0; i < ITERS; i++) {
            count += steps[i & mask];
            a0 += one; a1 += one; a2 += one; a3 += one; a4 += one; a5 += one; a6 += one; a7 += one;
            b0 += one; b1 += one; b2 += one; b3 += one; b4 += one; b5 += one; b6 += one; b7 += one;
            c0 += one; c1 += one; c2 += one; c3 += one; c4 += one; c5 += one; c6 += one; c7 += one;
        }
        sink = count;
        return lost(a0, a1, a2, a3, a4, a5, a6, a7) + lost(b0, b1, b2, b3, b4, b5, b6, b7)
             + lost(c0, c1, c2, c3, c4, c5, c6, c7);
    }

    static long kernelHandler(double one, Holder[] holders) {
        ExceptionXState.fillBelowStack();
        double a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, b7 = 0;
        double c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0, c6 = 0, c7 = 0;
        int mask = holders.length - 1;
        int count = 0;
        for (int i = 0; i < ITERS; i++) {
            try {
                count += holders[i & mask].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 += one; a1 += one; a2 += one; a3 += one; a4 += one; a5 += one; a6 += one; a7 += one;
            b0 += one; b1 += one; b2 += one; b3 += one; b4 += one; b5 += one; b6 += one; b7 += one;
            c0 += one; c1 += one; c2 += one; c3 += one; c4 += one; c5 += one; c6 += one; c7 += one;
        }
        sink = count;
        return lost(a0, a1, a2, a3, a4, a5, a6, a7) + lost(b0, b1, b2, b3, b4, b5, b6, b7)
             + lost(c0, c1, c2, c3, c4, c5, c6, c7);
    }

    static long lost(double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7) {
        long lost = 0;
        if (d0 != ITERS) lost++;
        if (d1 != ITERS) lost++;
        if (d2 != ITERS) lost++;
        if (d3 != ITERS) lost++;
        if (d4 != ITERS) lost++;
        if (d5 != ITERS) lost++;
        if (d6 != ITERS) lost++;
        if (d7 != ITERS) lost++;
        return lost;
    }

    public static void main(String[] args) throws Exception {
        ExceptionXState.Probe probe = ExceptionXState.probe();
        System.out.println("Native probe: " + probe);

        int[] steps = new int[1 << 10];
        for (int i = 0; i < steps.length; i++) {
            steps[i] = i & 3;
        }
        Holder[] holders = new Holder[1 << 10];
        for (int i = 0; i < holders.length; i++) {
            holders[i] = new Holder();
        }
        Holder[] withNull = holders.clone();
        withNull[withNull.length / 2] = null;

        Thread gc = new Thread(() -> {
            while (!stop) {
                for (int i = 0; i < 2000; i++) {
                    sink = new byte[256];
                }
                System.gc();
                try {
                    Thread.sleep(2);
                } catch (InterruptedException e) {
                    return;
                }
            }
        });
        gc.setDaemon(true);
        gc.start();

        long end = System.nanoTime() + SECONDS * 1_000_000_000L;
        long calls = 0;
        long wrong = 0;
        long handler = 0;
        while (System.nanoTime() < end) {
            Holder[] h = (calls % NULL_PERIOD == NULL_PERIOD - 1) ? withNull : holders;
            long lost = kernelPolls(1.0, steps);
            long handlerLost = kernelHandler(1.0, h);
            handler += handlerLost;
            lost += handlerLost;
            calls++;
            if (lost != 0 && wrong++ < 5) {
                System.out.println("call " + calls + ": " + lost + " accumulators lost");
            }
        }
        stop = true;
        System.out.println("calls=" + calls + " wrong=" + wrong + " handler=" + handler);
        if (wrong != 0) {
            throw new RuntimeException(wrong + " of " + calls + " calls lost floating point state across a trap");
        }
    }
}
