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
 * @summary With the stale exception context header scrub forced on (any OS), the checks of
 *          VerifyExceptionXStateScrub before every protected C2 trap site find the place clear:
 *          no instruction that writes deep below the stack pointer was left without a scrub.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.gc.G1 & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native -XX:+UseG1GC -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps -XX:ExceptionXStateLength=1920
 *      -XX:+VerifyExceptionXStateScrub -XX:+StressGCM -XX:+StressLCM -XX:+SafepointALot
 *      -XX:GuaranteedSafepointInterval=1 -XX:CompileCommand=dontinline,*TestExceptionXStateScrubVerify::callee
 *      --add-modules jdk.incubator.vector compiler.runtime.safepoints.TestExceptionXStateScrubVerify
 */

/*
 * @test id=Serial
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.gc.Serial & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native -XX:+UseSerialGC -XX:+UseCountedLoopSafepoints -XX:LoopStripMiningIter=1000
 *      -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps -XX:ExceptionXStateLength=1920
 *      -XX:+VerifyExceptionXStateScrub -XX:+StressGCM -XX:+StressLCM -XX:+SafepointALot
 *      -XX:GuaranteedSafepointInterval=1 -XX:CompileCommand=dontinline,*TestExceptionXStateScrubVerify::callee
 *      --add-modules jdk.incubator.vector compiler.runtime.safepoints.TestExceptionXStateScrubVerify
 */

/*
 * @test id=Parallel
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.gc.Parallel & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native -XX:+UseParallelGC -XX:+UseCountedLoopSafepoints -XX:LoopStripMiningIter=1000
 *      -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps -XX:ExceptionXStateLength=1920
 *      -XX:+VerifyExceptionXStateScrub -XX:+StressGCM -XX:+StressLCM -XX:+SafepointALot
 *      -XX:GuaranteedSafepointInterval=1 -XX:CompileCommand=dontinline,*TestExceptionXStateScrubVerify::callee
 *      --add-modules jdk.incubator.vector compiler.runtime.safepoints.TestExceptionXStateScrubVerify
 */

/*
 * @test id=Z
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.gc.Z & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native -XX:+UseZGC -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps -XX:ExceptionXStateLength=1920
 *      -XX:+VerifyExceptionXStateScrub -XX:+StressGCM -XX:+StressLCM -XX:+SafepointALot
 *      -XX:GuaranteedSafepointInterval=1 -XX:CompileCommand=dontinline,*TestExceptionXStateScrubVerify::callee
 *      --add-modules jdk.incubator.vector compiler.runtime.safepoints.TestExceptionXStateScrubVerify
 */

/*
 * @test id=Shenandoah
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.gc.Shenandoah & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps -XX:ExceptionXStateLength=1920
 *      -XX:+VerifyExceptionXStateScrub -XX:+StressGCM -XX:+StressLCM -XX:+SafepointALot
 *      -XX:GuaranteedSafepointInterval=1 -XX:CompileCommand=dontinline,*TestExceptionXStateScrubVerify::callee
 *      --add-modules jdk.incubator.vector compiler.runtime.safepoints.TestExceptionXStateScrubVerify
 */

/*
 * @test id=at-traps
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps -XX:ExceptionXStateLength=1920
 *      -XX:+VerifyExceptionXStateScrub -XX:+ScrubExceptionXStateHeaderAtTraps -XX:+StressGCM -XX:+StressLCM
 *      -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 -XX:CompileCommand=dontinline,*TestExceptionXStateScrubVerify::callee
 *      --add-modules jdk.incubator.vector compiler.runtime.safepoints.TestExceptionXStateScrubVerify
 */

/*
 * @test id=connector
 * @bug 8365196
 * @summary The scrub after a call whose return block is empty (a call in an arm of an if or a
 *          switch, merging into a join) is placed where it runs, not in the empty connector block
 *          that the code layout skips: the check before the vector loop after the join stops the
 *          VM otherwise, deterministically and on any OS.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps -XX:ExceptionXStateLength=1920
 *      -XX:+VerifyExceptionXStateScrub -XX:-TieredCompilation -XX:+UseCountedLoopSafepoints
 *      -XX:LoopStripMiningIter=1000
 *      --add-modules jdk.incubator.vector compiler.runtime.safepoints.TestExceptionXStateScrubVerify connector
 */

/*
 * @test id=plant-G1
 * @bug 8365196
 * @summary With PlantExceptionXStateHeader, every compiled method's return and every slow path that
 *          calls out leaves a value that passes for a stale header: the checks before the protected
 *          trap sites still find the place clear, so no scrub after a writer is missing.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.gc.G1 & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native -XX:+UseG1GC -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps -XX:ExceptionXStateLength=1920
 *      -XX:+VerifyExceptionXStateScrub -XX:+PlantExceptionXStateHeader -XX:+StressGCM -XX:+StressLCM
 *      -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 -XX:CompileCommand=dontinline,*TestExceptionXStateScrubVerify::callee
 *      --add-modules jdk.incubator.vector compiler.runtime.safepoints.TestExceptionXStateScrubVerify
 */

/*
 * @test id=plant-Z
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.gc.Z & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native -XX:+UseZGC -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps -XX:ExceptionXStateLength=1920
 *      -XX:+VerifyExceptionXStateScrub -XX:+PlantExceptionXStateHeader -XX:+StressGCM -XX:+StressLCM
 *      -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 -XX:CompileCommand=dontinline,*TestExceptionXStateScrubVerify::callee
 *      --add-modules jdk.incubator.vector compiler.runtime.safepoints.TestExceptionXStateScrubVerify
 */

/*
 * @test id=plant-negative
 * @bug 8365196
 * @summary With PlantExceptionXStateHeader and without the scrub after calls, the check stops the VM
 *          at the first protected trap site after a call to a compiled method, with no native code
 *          involved.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native compiler.runtime.safepoints.TestExceptionXStateScrubVerify plant-negative
 */

/*
 * @test id=negative
 * @bug 8365196
 * @summary The check fails when the scrub after a call is missing: a native call leaves
 *          0x8000000000000000 where the header would be read, and the next protected trap site
 *          sees it, on any OS.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.debug & vm.compiler2.enabled & vm.flagless
 * @modules jdk.incubator.vector java.base/jdk.internal.misc
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native compiler.runtime.safepoints.TestExceptionXStateScrubVerify negative
 */

package compiler.runtime.safepoints;

import java.util.ArrayList;
import java.util.List;
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorSpecies;
import jdk.internal.misc.Unsafe;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestExceptionXStateScrubVerify {
    static final VectorSpecies<Double> S256 = DoubleVector.SPECIES_256;
    static final Unsafe U = Unsafe.getUnsafe();
    static final int ITERS = 20_000;

    static volatile boolean stop;
    static Object sink;
    static volatile double sinkD;

    static class Holder {
        int x = 1;
    }

    static double callee(double x) {
        return x + 1;
    }

    // Vector state live across loop polls, GC barriers and implicit null checks, after a native call.
    static long vectorKernel(double neg, Object[] refs, Holder[] holders) {
        ExceptionXState.fillBelowStack();
        DoubleVector v = DoubleVector.broadcast(S256, neg);
        DoubleVector a0 = DoubleVector.broadcast(S256, neg);
        DoubleVector a1 = DoubleVector.broadcast(S256, neg);
        DoubleVector a2 = DoubleVector.broadcast(S256, neg);
        DoubleVector a3 = DoubleVector.broadcast(S256, neg);
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
        }
        sink = count;
        LongVector r = a0.reinterpretAsLongs().and(a1.reinterpretAsLongs())
                         .and(a2.reinterpretAsLongs()).and(a3.reinterpretAsLongs());
        return r.lane(0) == Long.MIN_VALUE ? 0 : 1;
    }

    // A call in each arm of an if, or in one arm, or in each arm of a switch, then vector state live
    // across a loop with implicit null checks and a poll. The return block of each call is empty,
    // so it is a connector by the time scrubs are placed; the native calls leave
    // 0x8000000000000000 where the header would be read. The loop is written out in each kernel
    // so that the vectors are live in the kernel's own frame.
    static long joinBoth(double neg, boolean c, Holder[] hs) {
        if (c) {
            ExceptionXState.fillBelowStack();
        } else {
            ExceptionXState.fillBelowStack();
        }
        DoubleVector v = DoubleVector.broadcast(S256, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            try {
                count += hs[i & m].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        LongVector r = a0.reinterpretAsLongs().and(a1.reinterpretAsLongs())
                         .and(a2.reinterpretAsLongs()).and(a3.reinterpretAsLongs());
        return r.lane(0) == Long.MIN_VALUE ? 0 : 1;
    }

    static long joinOne(double neg, boolean c, Holder[] hs) {
        if (c) {
            ExceptionXState.fillBelowStack();
        }
        DoubleVector v = DoubleVector.broadcast(S256, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            try {
                count += hs[i & m].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        LongVector r = a0.reinterpretAsLongs().and(a1.reinterpretAsLongs())
                         .and(a2.reinterpretAsLongs()).and(a3.reinterpretAsLongs());
        return r.lane(0) == Long.MIN_VALUE ? 0 : 1;
    }

    static long joinSwitch(double neg, int k, Holder[] hs) {
        switch (k % 3) {
            case 0 -> ExceptionXState.fillBelowStack();
            case 1 -> ExceptionXState.fillBelowStack();
            default -> ExceptionXState.fillBelowStack();
        }
        DoubleVector v = DoubleVector.broadcast(S256, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            try {
                count += hs[i & m].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        LongVector r = a0.reinterpretAsLongs().and(a1.reinterpretAsLongs())
                         .and(a2.reinterpretAsLongs()).and(a3.reinterpretAsLongs());
        return r.lane(0) == Long.MIN_VALUE ? 0 : 1;
    }

    static void connector(long millis) {
        Holder[] hs = new Holder[256];
        for (int i = 0; i < hs.length; i++) {
            hs[i] = new Holder();
        }
        Holder[] withNull = hs.clone();
        withNull[7] = null;
        long end = System.nanoTime() + millis * 1_000_000L;
        long calls = 0;
        while (System.nanoTime() < end) {
            Holder[] h = (calls & 63) == 63 ? withNull : hs;
            sink = joinBoth(-0.0, (calls & 1) == 0, h);
            sink = joinOne(-0.0, (calls & 1) == 0, h);
            sink = joinSwitch(-0.0, (int) calls, h);
            calls++;
        }
        System.out.println("connector calls=" + calls);
    }

    // A call to a compiled method, then vector state live across a loop with implicit null checks
    // and a poll: with PlantExceptionXStateHeader, the callee's return leaves a header-like value
    // that only the scrub after the call clears.
    static long callThenLoop(double neg, Holder[] hs, double x) {
        sinkD = callee(x);
        DoubleVector v = DoubleVector.broadcast(S256, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            count += hs[i & m].x;
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        LongVector r = a0.reinterpretAsLongs().and(a1.reinterpretAsLongs())
                         .and(a2.reinterpretAsLongs()).and(a3.reinterpretAsLongs());
        return r.lane(0) == Long.MIN_VALUE ? 0 : 1;
    }

    // Vector state live across a loop with a call in it (the scrub after the call).
    static double vectorCallKernel(double neg, double[] a) {
        DoubleVector acc = DoubleVector.broadcast(S256, neg);
        for (int i = 0; i < a.length; i++) {
            acc = acc.add(callee(a[i]));
        }
        return acc.lane(0);
    }

    // Scalars in xmm16-xmm31 (AVX-512) live across a leaf call (from the review of the scheduling).
    static double scalarSinKernel(double x, int n) {
        double a0 = x + 0, a1 = x + 1, a2 = x + 2, a3 = x + 3, a4 = x + 4, a5 = x + 5, a6 = x + 6, a7 = x + 7;
        double a8 = x + 8, a9 = x + 9, a10 = x + 10, a11 = x + 11, a12 = x + 12, a13 = x + 13, a14 = x + 14;
        double a15 = x + 15, a16 = x + 16, a17 = x + 17, a18 = x + 18, a19 = x + 19, a20 = x + 20;
        double a21 = x + 21, a22 = x + 22, a23 = x + 23;
        for (int i = 0; i < n; i++) {
            a0 += Math.sin(a0);
            a1 += a0 * 0.5; a2 += a1 * 0.5; a3 += a2 * 0.5; a4 += a3 * 0.5; a5 += a4 * 0.5;
            a6 += a5 * 0.5; a7 += a6 * 0.5; a8 += a7 * 0.5; a9 += a8 * 0.5; a10 += a9 * 0.5;
            a11 += a10 * 0.5; a12 += a11 * 0.5; a13 += a12 * 0.5; a14 += a13 * 0.5; a15 += a14 * 0.5;
            a16 += a15 * 0.5; a17 += a16 * 0.5; a18 += a17 * 0.5; a19 += a18 * 0.5; a20 += a19 * 0.5;
            a21 += a20 * 0.5; a22 += a21 * 0.5; a23 += a22 * 0.5;
        }
        return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16
               + a17 + a18 + a19 + a20 + a21 + a22 + a23;
    }

    // The same with a raw memory access after the leaf call: a trap site in a compilation with
    // unsafe accesses.
    static double rawKernel(double x, long addr, int n) {
        double a0 = x + 0, a1 = x + 1, a2 = x + 2, a3 = x + 3, a4 = x + 4, a5 = x + 5, a6 = x + 6, a7 = x + 7;
        double a8 = x + 8, a9 = x + 9, a10 = x + 10, a11 = x + 11, a12 = x + 12, a13 = x + 13, a14 = x + 14;
        double a15 = x + 15, a16 = x + 16, a17 = x + 17, a18 = x + 18, a19 = x + 19, a20 = x + 20;
        double a21 = x + 21, a22 = x + 22, a23 = x + 23;
        for (int i = 0; i < n; i++) {
            a0 += Math.sin(a0);
            a0 += U.getLong(addr + ((i & 7) << 3));
            a1 += a0 * 0.5; a2 += a1 * 0.5; a3 += a2 * 0.5; a4 += a3 * 0.5; a5 += a4 * 0.5;
            a6 += a5 * 0.5; a7 += a6 * 0.5; a8 += a7 * 0.5; a9 += a8 * 0.5; a10 += a9 * 0.5;
            a11 += a10 * 0.5; a12 += a11 * 0.5; a13 += a12 * 0.5; a14 += a13 * 0.5; a15 += a14 * 0.5;
            a16 += a15 * 0.5; a17 += a16 * 0.5; a18 += a17 * 0.5; a19 += a18 * 0.5; a20 += a19 * 0.5;
            a21 += a20 * 0.5; a22 += a21 * 0.5; a23 += a22 * 0.5;
        }
        return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16
               + a17 + a18 + a19 + a20 + a21 + a22 + a23;
    }

    static void run(long millis, boolean vectorOnly) {
        Object[] refs = new Object[1 << 12];
        for (int i = 0; i < refs.length; i++) {
            refs[i] = new int[1 + (i & 7)];
        }
        Holder[] holders = new Holder[1 << 8];
        for (int i = 0; i < holders.length; i++) {
            holders[i] = new Holder();
        }
        Holder[] withNull = holders.clone();
        withNull[7] = null;
        double[] data = new double[256];
        long addr = U.allocateMemory(64);
        U.setMemory(addr, 64, (byte) 0);
        long end = System.nanoTime() + millis * 1_000_000L;
        long calls = 0;
        double r = 0;
        while (System.nanoTime() < end) {
            sink = vectorKernel(-0.0, refs, (calls & 63) == 63 ? withNull : holders);
            if (!vectorOnly) {
                sinkD = vectorCallKernel(-0.0, data);
                r += scalarSinKernel(r * 1e-300, 1000);
                r += rawKernel(r * 1e-300, addr, 1000);
                sink = joinBoth(-0.0, (calls & 1) == 0, holders);
                sink = joinSwitch(-0.0, (int) calls, holders);
                sink = callThenLoop(-0.0, holders, calls);
            }
            calls++;
        }
        sinkD = r;
        U.freeMemory(addr);
        System.out.println("calls=" + calls);
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("negative")) {
            negative();
            return;
        }
        if (args.length > 0 && args[0].equals("connector")) {
            connector(Long.getLong("millis", 8000));
            return;
        }
        if (args.length > 0 && args[0].equals("plant-negative")) {
            plantNegative();
            return;
        }
        if (args.length > 0 && args[0].equals("plant-child")) {
            Holder[] hs = new Holder[256];
            for (int i = 0; i < hs.length; i++) {
                hs[i] = new Holder();
            }
            long end = System.nanoTime() + Long.parseLong(args[1]) * 1_000_000L;
            double x = 0;
            while (System.nanoTime() < end) {
                sink = callThenLoop(-0.0, hs, x);
                x += 1;
            }
            return;
        }
        if (args.length > 0 && args[0].equals("child")) {
            run(Long.parseLong(args[1]), true);
            return;
        }
        Thread gc = new Thread(() -> {
            while (!stop) {
                for (int i = 0; i < 2000; i++) {
                    sink = new byte[256];
                }
                System.gc();
                try {
                    Thread.sleep(5);
                } catch (InterruptedException e) {
                    return;
                }
            }
        });
        gc.setDaemon(true);
        gc.start();
        run(Long.getLong("millis", 8000), false);
        stop = true;
    }

    static void plantNegative() throws Exception {
        List<String> opts = new ArrayList<>(List.of(
            "-XX:+UnlockDiagnosticVMOptions", "-XX:-AvoidLosableStateAtTraps", "-XX:ExceptionXStateLength=1920",
            "-XX:+VerifyExceptionXStateScrub", "-XX:+PlantExceptionXStateHeader", "-XX:+TrapScrubOmitAfterCalls",
            "-XX:-TieredCompilation", "-XX:+UseCountedLoopSafepoints", "-XX:LoopStripMiningIter=1000",
            "-XX:CompileCommand=dontinline,*TestExceptionXStateScrubVerify::callee",
            "-XX:-CreateCoredumpOnCrash", "--add-modules", "jdk.incubator.vector",
            "--add-exports", "java.base/jdk.internal.misc=ALL-UNNAMED",
            TestExceptionXStateScrubVerify.class.getName(), "plant-child", "20000"));
        OutputAnalyzer out = ProcessTools.executeTestJava(opts);
        out.shouldNotHaveExitValue(0);
        out.shouldContain("stale XSAVE header below a compiled trap site");
    }

    static void negative() throws Exception {
        List<String> opts = new ArrayList<>(List.of(
            "-Djava.library.path=" + System.getProperty("java.library.path"),
            "-XX:+UnlockDiagnosticVMOptions", "-XX:-AvoidLosableStateAtTraps", "-XX:ExceptionXStateLength=4096",
            "-XX:+VerifyExceptionXStateScrub", "-XX:+TrapScrubOmitAfterCalls",
            "-XX:+UseCountedLoopSafepoints", "-XX:LoopStripMiningIter=1000",
            "-XX:-CreateCoredumpOnCrash", "--add-modules", "jdk.incubator.vector",
            "--add-exports", "java.base/jdk.internal.misc=ALL-UNNAMED",
            TestExceptionXStateScrubVerify.class.getName(), "child", "20000"));
        OutputAnalyzer out = ProcessTools.executeTestJava(opts);
        out.shouldNotHaveExitValue(0);
        out.shouldContain("stale XSAVE header below a compiled trap site");
    }
}
