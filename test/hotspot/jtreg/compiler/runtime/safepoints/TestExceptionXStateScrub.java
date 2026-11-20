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
 * @test id=G1
 * @bug 8365196
 * @summary Vector state live across trapping loop polls and implicit null checks survives when
 *          the stack below the compiled frame holds a value that passes for a stale XSAVE header.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler2.enabled & vm.gc.G1
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native/timeout=300 -XX:+UseG1GC
 *      -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      -XX:PerMethodTrapLimit=100000 -XX:PerBytecodeTrapLimit=100000
 *      -XX:PerMethodRecompilationCutoff=-1 -XX:PerBytecodeRecompilationCutoff=-1
 *      -Xlog:os+cpu=info --add-modules jdk.incubator.vector
 *      compiler.runtime.safepoints.TestExceptionXStateScrub
 */

/*
 * @test id=Serial
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler2.enabled & vm.gc.Serial
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native/timeout=300 -XX:+UseSerialGC -XX:+UseCountedLoopSafepoints -XX:LoopStripMiningIter=1000
 *      -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      -XX:PerMethodTrapLimit=100000 -XX:PerBytecodeTrapLimit=100000
 *      -XX:PerMethodRecompilationCutoff=-1 -XX:PerBytecodeRecompilationCutoff=-1
 *      -Xlog:os+cpu=info --add-modules jdk.incubator.vector
 *      compiler.runtime.safepoints.TestExceptionXStateScrub
 */

/*
 * @test id=Parallel
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler2.enabled & vm.gc.Parallel
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native/timeout=300 -XX:+UseParallelGC -XX:+UseCountedLoopSafepoints -XX:LoopStripMiningIter=1000
 *      -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      -XX:PerMethodTrapLimit=100000 -XX:PerBytecodeTrapLimit=100000
 *      -XX:PerMethodRecompilationCutoff=-1 -XX:PerBytecodeRecompilationCutoff=-1
 *      -Xlog:os+cpu=info --add-modules jdk.incubator.vector
 *      compiler.runtime.safepoints.TestExceptionXStateScrub
 */

/*
 * @test id=Z
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler2.enabled & vm.gc.Z
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native/timeout=300 -XX:+UseZGC
 *      -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      -XX:PerMethodTrapLimit=100000 -XX:PerBytecodeTrapLimit=100000
 *      -XX:PerMethodRecompilationCutoff=-1 -XX:PerBytecodeRecompilationCutoff=-1
 *      -Xlog:os+cpu=info --add-modules jdk.incubator.vector
 *      compiler.runtime.safepoints.TestExceptionXStateScrub
 */

/*
 * @test id=Shenandoah
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler2.enabled & vm.gc.Shenandoah
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native/timeout=300 -XX:+UseShenandoahGC
 *      -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      -XX:PerMethodTrapLimit=100000 -XX:PerBytecodeTrapLimit=100000
 *      -XX:PerMethodRecompilationCutoff=-1 -XX:PerBytecodeRecompilationCutoff=-1
 *      -Xlog:os+cpu=info --add-modules jdk.incubator.vector
 *      compiler.runtime.safepoints.TestExceptionXStateScrub
 */

package compiler.runtime.safepoints;

import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorSpecies;

/*
 * Accumulators of -0.0 stay -0.0 under -0.0 additions; an upper lane reset by an incomplete
 * exception context becomes +0.0 and stays, so the final AND of the lanes detects any loss.
 * Each kernel starts with a native call that writes 0x8000000000000000 below the compiled
 * frame's stack pointer, where an affected Windows build reads the XSAVE header of the next
 * exception's context: only the clearing after that call keeps the first trap of the loop, a
 * safepoint poll taken under SafepointALot, from losing the vector state. Serial and Parallel
 * do not poll in counted loops by default, so their ids turn the polls on.
 */
public class TestExceptionXStateScrub {
    static final int ITERS = 200_000;
    static final int NULL_PERIOD = 64;
    static final long SECONDS = Long.getLong("seconds", 10);

    static volatile boolean stop;
    static Object sink;

    static class Holder {
        int x = 1;
    }

    static final VectorSpecies<Double> S256 = DoubleVector.SPECIES_256;
    static final VectorSpecies<Double> S512 = DoubleVector.SPECIES_512;

    static long kernel256(double neg, Object[] refs, Holder[] holders) {
        ExceptionXState.fillBelowStack();
        DoubleVector v = DoubleVector.broadcast(S256, neg);
        DoubleVector a0 = DoubleVector.broadcast(S256, neg);
        DoubleVector a1 = DoubleVector.broadcast(S256, neg);
        DoubleVector a2 = DoubleVector.broadcast(S256, neg);
        DoubleVector a3 = DoubleVector.broadcast(S256, neg);
        DoubleVector a4 = DoubleVector.broadcast(S256, neg);
        DoubleVector a5 = DoubleVector.broadcast(S256, neg);
        DoubleVector a6 = DoubleVector.broadcast(S256, neg);
        DoubleVector a7 = DoubleVector.broadcast(S256, neg);
        int mask = refs.length - 1;
        int hmask = holders.length - 1;
        int count = 0;
        for (int i = 0; i < ITERS; i++) {
            if (refs[i & mask] != null) {
                count++;
            }
            try {
                count += holders[i & hmask].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v);
            a1 = a1.add(v);
            a2 = a2.add(v);
            a3 = a3.add(v);
            a4 = a4.add(v);
            a5 = a5.add(v);
            a6 = a6.add(v);
            a7 = a7.add(v);
        }
        sink = count;
        LongVector r = a0.reinterpretAsLongs()
                .and(a1.reinterpretAsLongs())
                .and(a2.reinterpretAsLongs())
                .and(a3.reinterpretAsLongs())
                .and(a4.reinterpretAsLongs())
                .and(a5.reinterpretAsLongs())
                .and(a6.reinterpretAsLongs())
                .and(a7.reinterpretAsLongs());
        long lost = 0;
        for (int l = 0; l < r.length(); l++) {
            if (r.lane(l) != Long.MIN_VALUE) {
                lost++;
            }
        }
        return lost;
    }

    static long kernel512(double neg, Object[] refs, Holder[] holders) {
        ExceptionXState.fillBelowStack();
        DoubleVector v = DoubleVector.broadcast(S512, neg);
        DoubleVector a0 = DoubleVector.broadcast(S512, neg);
        DoubleVector a1 = DoubleVector.broadcast(S512, neg);
        DoubleVector a2 = DoubleVector.broadcast(S512, neg);
        DoubleVector a3 = DoubleVector.broadcast(S512, neg);
        DoubleVector a4 = DoubleVector.broadcast(S512, neg);
        DoubleVector a5 = DoubleVector.broadcast(S512, neg);
        DoubleVector a6 = DoubleVector.broadcast(S512, neg);
        DoubleVector a7 = DoubleVector.broadcast(S512, neg);
        int mask = refs.length - 1;
        int hmask = holders.length - 1;
        int count = 0;
        for (int i = 0; i < ITERS; i++) {
            if (refs[i & mask] != null) {
                count++;
            }
            try {
                count += holders[i & hmask].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v);
            a1 = a1.add(v);
            a2 = a2.add(v);
            a3 = a3.add(v);
            a4 = a4.add(v);
            a5 = a5.add(v);
            a6 = a6.add(v);
            a7 = a7.add(v);
        }
        sink = count;
        LongVector r = a0.reinterpretAsLongs()
                .and(a1.reinterpretAsLongs())
                .and(a2.reinterpretAsLongs())
                .and(a3.reinterpretAsLongs())
                .and(a4.reinterpretAsLongs())
                .and(a5.reinterpretAsLongs())
                .and(a6.reinterpretAsLongs())
                .and(a7.reinterpretAsLongs());
        long lost = 0;
        for (int l = 0; l < r.length(); l++) {
            if (r.lane(l) != Long.MIN_VALUE) {
                lost++;
            }
        }
        return lost;
    }

    public static void main(String[] args) throws Exception {
        ExceptionXState.Probe probe = ExceptionXState.probe();
        System.out.println("Native probe: " + probe);

        Object[] refs = new Object[1 << 14];
        for (int i = 0; i < refs.length; i++) {
            refs[i] = new int[1 + (i & 7)];
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

        boolean has512 = DoubleVector.SPECIES_PREFERRED.vectorBitSize() >= 512;
        long end = System.nanoTime() + SECONDS * 1_000_000_000L;
        long calls = 0;
        long wrong = 0;
        while (System.nanoTime() < end) {
            Holder[] h = (calls % NULL_PERIOD == NULL_PERIOD - 1) ? withNull : holders;
            long lost = kernel256(-0.0, refs, h);
            if (has512) {
                lost += kernel512(-0.0, refs, h);
            }
            calls++;
            if (lost != 0 && wrong++ < 5) {
                System.out.println("call " + calls + ": " + lost + " vector lanes lost");
            }
        }
        stop = true;
        System.out.println("calls=" + calls + " wrong=" + wrong);
        if (wrong != 0) {
            throw new RuntimeException(wrong + " of " + calls + " calls lost vector state across a trap");
        }
    }
}
