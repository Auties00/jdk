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
 * @test id=default
 * @bug 8365196
 * @summary Vector values stay correct across implicit null checks, caught null pointer
 *          exceptions and loop polls taken under constant safepoints and handshakes, with the
 *          platform's default for AvoidLosableStateAtTraps (on Windows, the mode).
 * @requires (os.arch == "amd64" | os.arch == "x86_64" | os.arch == "aarch64") & vm.compiler2.enabled
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run main/othervm/timeout=300 -XX:+UseCountedLoopSafepoints -XX:LoopStripMiningIter=1000
 *      -XX:CompileCommand=exclude,*TestAvoidLosableStateAtTraps::reference*
 *      --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
 *      compiler.runtime.safepoints.TestAvoidLosableStateAtTraps
 */

/*
 * @test id=forced
 * @bug 8365196
 * @summary The same with AvoidLosableStateAtTraps forced on, every safepoint stressed and the
 *          scheduling of nodes randomized, so that the debug checks of the mode (no trap site
 *          left trapping with losable state live, no integer or pointer in a losable register, no
 *          rematerialization from one) run on every compilation.
 * @requires (os.arch == "amd64" | os.arch == "x86_64" | os.arch == "aarch64") & vm.compiler2.enabled & vm.debug
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run main/othervm/timeout=300 -XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps
 *      -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 -XX:+StressGCM -XX:+StressLCM
 *      -XX:+UseCountedLoopSafepoints -XX:LoopStripMiningIter=1000
 *      -XX:CompileCommand=exclude,*TestAvoidLosableStateAtTraps::reference*
 *      --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
 *      compiler.runtime.safepoints.TestAvoidLosableStateAtTraps
 */

/*
 * @test id=fpu-spilling
 * @bug 8365196
 * @summary The same with UseFPUForSpilling, whose spills of integers and pointers the mode keeps
 *          out of xmm16-31 in compilations with unsafe accesses, and counts as losable state at
 *          the trap sites of the others.
 * @requires (os.arch == "amd64" | os.arch == "x86_64" | os.arch == "aarch64") & vm.compiler2.enabled & vm.debug
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run main/othervm/timeout=300 -XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps
 *      -XX:+UseFPUForSpilling -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1 -XX:+StressLCM
 *      -XX:+UseCountedLoopSafepoints -XX:LoopStripMiningIter=1000
 *      -XX:CompileCommand=exclude,*TestAvoidLosableStateAtTraps::reference*
 *      --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
 *      compiler.runtime.safepoints.TestAvoidLosableStateAtTraps
 */

/*
 * @test id=off
 * @bug 8365196
 * @summary The same without the mode (a sanity run of the workload; on a Windows kernel that
 *          loses AVX state it can fail).
 * @requires (os.arch == "amd64" | os.arch == "x86_64" | os.arch == "aarch64") & vm.compiler2.enabled
 * @requires os.family != "windows"
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run main/othervm/timeout=300 -XX:+UnlockDiagnosticVMOptions -XX:-AvoidLosableStateAtTraps
 *      -XX:+UseCountedLoopSafepoints -XX:LoopStripMiningIter=1000
 *      -XX:CompileCommand=exclude,*TestAvoidLosableStateAtTraps::reference*
 *      --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
 *      compiler.runtime.safepoints.TestAvoidLosableStateAtTraps
 */

/*
 * @test id=explicit
 * @bug 8365196
 * @summary With the mode, C2 replaces the implicit null checks whose null path vector state is
 *          live into by explicit ones, keeps those whose null path reads none of it, and without
 *          the mode replaces none.
 * @requires (os.arch == "amd64" | os.arch == "x86_64") & vm.compiler2.enabled & vm.debug & vm.flagless
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run driver compiler.runtime.safepoints.TestAvoidLosableStateAtTraps explicit
 */

package compiler.runtime.safepoints;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestAvoidLosableStateAtTraps {
    static final VectorSpecies<Long> L = LongVector.SPECIES_PREFERRED;
    static final VectorSpecies<Double> D = DoubleVector.SPECIES_PREFERRED;
    static final int N = 1 << 12;
    static final long K = 0x9E3779B97F4A7C15L;

    static final class Box {
        long v;
        Box(long v) { this.v = v; }
    }

    static final long[] data = new long[N];
    static final double[] doubles = new double[N];
    static final Box[] boxes = new Box[64];
    static volatile boolean stop;

    static {
        for (int i = 0; i < N; i++) {
            data[i] = i * 0x5DEECE66DL + 11;
            doubles[i] = i * 0.25 - 100.0;
        }
        for (int i = 0; i < boxes.length; i++) {
            boxes[i] = new Box(i * 3 + 1);
        }
    }

    // A vector accumulator and a vector constant are live across the implicit null check of
    // box.v, its caught exception and the loop poll.
    static long kernel(long[] a, Box[] b) {
        LongVector acc = LongVector.zero(L);
        LongVector k = LongVector.broadcast(L, K);
        long nulls = 0;
        for (int i = 0; i + L.length() <= a.length; i += L.length()) {
            Box box = b[(i / L.length()) & (b.length - 1)];
            long x;
            try {
                x = box.v;
            } catch (NullPointerException e) {
                nulls++;
                x = 7;
            }
            acc = acc.add(LongVector.fromArray(L, a, i).mul(k).add(x));
        }
        return acc.reduceLanes(VectorOperators.ADD) * 31 + nulls;
    }

    static long referenceKernel(long[] a, Box[] b) {
        int len = L.length();
        long[] acc = new long[len];
        long nulls = 0;
        for (int i = 0; i + len <= a.length; i += len) {
            Box box = b[(i / len) & (b.length - 1)];
            long x;
            if (box == null) {
                nulls++;
                x = 7;
            } else {
                x = box.v;
            }
            for (int j = 0; j < len; j++) {
                acc[j] += a[i + j] * K + x;
            }
        }
        long sum = 0;
        for (long v : acc) {
            sum += v;
        }
        return sum * 31 + nulls;
    }

    // A vector that C2 computes ahead of the loop is live across the implicit null check of box.v,
    // but not into the path its trap resumes at: the uncommon trap there names what the bytecode
    // holds, the scalar k. The check can stay implicit.
    static long invariantKernel(long[] a, Box[] b, long k) {
        long sum = 0;
        for (int i = 0; i + L.length() <= a.length; i += L.length()) {
            long x = b[(i / L.length()) & (b.length - 1)].v;
            sum += LongVector.fromArray(L, a, i).add(LongVector.broadcast(L, k)).reduceLanes(VectorOperators.ADD) + x;
        }
        return sum;
    }

    static long referenceInvariantKernel(long[] a, Box[] b, long k) {
        int len = L.length();
        long sum = 0;
        for (int i = 0; i + len <= a.length; i += len) {
            long x = b[(i / len) & (b.length - 1)].v;
            for (int j = 0; j < len; j++) {
                sum += a[i + j] + k;
            }
            sum += x;
        }
        return sum;
    }

    // Doubles in vectors live across a null check whose exception leaves the method. Every value
    // is a multiple of 1/8 well below 2^53, so the sums are exact in any order of reduction.
    static double doubleKernel(double[] a, Box box) {
        DoubleVector acc = DoubleVector.zero(D);
        DoubleVector scale = DoubleVector.broadcast(D, 1.5);
        for (int i = 0; i + D.length() <= a.length; i += D.length()) {
            acc = acc.add(DoubleVector.fromArray(D, a, i).mul(scale)).add((double)(box.v & 3));
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    static double referenceDoubleKernel(double[] a, Box box) {
        int len = D.length();
        double[] acc = new double[len];
        for (int i = 0; i + len <= a.length; i += len) {
            for (int j = 0; j < len; j++) {
                acc[j] = acc[j] + a[i + j] * 1.5 + (double)(box.v & 3);
            }
        }
        double sum = 0;
        for (double v : acc) {
            sum += v;
        }
        return sum;
    }

    // A compilation with unsafe accesses (the segment accessors) that keeps vector values live.
    static long segmentKernel(MemorySegment s, long[] a) {
        long sum = 0;
        LongVector acc = LongVector.zero(L);
        for (int i = 0; i + L.length() <= a.length; i += L.length()) {
            long w = s.getAtIndex(ValueLayout.JAVA_LONG, i);
            acc = acc.add(LongVector.fromArray(L, a, i).add(w));
            sum += w ^ (w >>> 7);
        }
        return acc.reduceLanes(VectorOperators.ADD) + sum;
    }

    static long referenceSegmentKernel(MemorySegment s, long[] a) {
        long sum = 0;
        long vectorSum = 0;
        int len = L.length();
        for (int i = 0; i + len <= a.length; i += len) {
            long w = s.getAtIndex(ValueLayout.JAVA_LONG, i);
            for (int j = 0; j < len; j++) {
                vectorSum += a[i + j] + w;
            }
            sum += w ^ (w >>> 7);
        }
        return vectorSum + sum;
    }

    static void check(boolean ok, String what, Object got, Object want) {
        if (!ok) {
            throw new RuntimeException(what + ": got " + got + ", expected " + want);
        }
    }

    static void run(long millis) {
        Thread disturber = new Thread(() -> {
            while (!stop) {
                Thread.getAllStackTraces(); // a handshake with every thread, through its polls
                System.gc();
            }
        });
        disturber.setDaemon(true);
        disturber.start();

        try (Arena arena = Arena.ofConfined()) {
            MemorySegment segment = arena.allocate(8L * N, 64);
            for (int i = 0; i < N; i++) {
                segment.setAtIndex(ValueLayout.JAVA_LONG, i, i * 977L - 5);
            }
            final long wantSegment = referenceSegmentKernel(segment, data);
            final long wantKernel = referenceKernel(data, boxes);
            final double wantDouble = referenceDoubleKernel(doubles, boxes[1]);
            Box[] withNull = boxes.clone();
            withNull[5] = null;
            final long wantNull = referenceKernel(data, withNull);
            final long wantInvariant = referenceInvariantKernel(data, boxes, K);

            long end = System.currentTimeMillis() + millis;
            for (long iteration = 0; System.currentTimeMillis() < end; iteration++) {
                long got = kernel(data, boxes);
                check(got == wantKernel, "kernel", got, wantKernel);
                double d = doubleKernel(doubles, boxes[1]);
                check(Double.compare(d, wantDouble) == 0, "doubleKernel", d, wantDouble);
                long s = segmentKernel(segment, data);
                check(s == wantSegment, "segmentKernel", s, wantSegment);
                long v = invariantKernel(data, boxes, K);
                check(v == wantInvariant, "invariantKernel", v, wantInvariant);
                if (iteration % 1024 == 1023) {
                    // A rare null: the compiled null check takes its exception and the method goes on.
                    long n = kernel(data, withNull);
                    check(n == wantNull, "kernel with a null", n, wantNull);
                    try {
                        doubleKernel(doubles, null);
                        throw new RuntimeException("no NullPointerException");
                    } catch (NullPointerException expected) {
                    }
                    try {
                        invariantKernel(data, withNull, K);
                        throw new RuntimeException("no NullPointerException");
                    } catch (NullPointerException expected) {
                    }
                }
            }
        } finally {
            stop = true;
        }
    }

    static OutputAnalyzer child(String mode, String method) throws Exception {
        OutputAnalyzer output = ProcessTools.executeTestJava(
            "-XX:+UnlockDiagnosticVMOptions", mode, "-XX:-TieredCompilation", "-Xbatch",
            "-XX:CompileCommand=compileonly,*TestAvoidLosableStateAtTraps::" + method,
            "-XX:CompileCommand=print,*TestAvoidLosableStateAtTraps::" + method,
            "--add-modules", "jdk.incubator.vector", "--enable-native-access=ALL-UNNAMED",
            TestAvoidLosableStateAtTraps.class.getName(), "child", method);
        output.shouldHaveExitValue(0);
        return output;
    }

    static void explicit() throws Exception {
        final String on = "-XX:+AvoidLosableStateAtTraps";
        final String off = "-XX:-AvoidLosableStateAtTraps";
        final boolean wide = L.vectorBitSize() > 128;

        // The accumulator has an upper half that the handler of the caught exception reads.
        OutputAnalyzer kernelOn = child(on, "kernel");
        if (wide) {
            kernelOn.shouldContain("explicit null check");
        }
        child(off, "kernel").shouldNotContain("explicit null check");

        // The broadcast has an upper half live across the check, which its null path does not read.
        OutputAnalyzer invariantOn = child(on, "invariantKernel");
        invariantOn.shouldContain("NullCheck");
        invariantOn.shouldNotContain("explicit null check");
        child(off, "invariantKernel").shouldNotContain("explicit null check");
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("explicit")) {
            explicit();
        } else if (args.length > 1 && args[0].equals("child") && args[1].equals("kernel")) {
            long want = referenceKernel(data, boxes);
            for (int i = 0; i < 20_000; i++) {
                long got = kernel(data, boxes);
                check(got == want, "kernel", got, want);
            }
        } else if (args.length > 1 && args[0].equals("child")) {
            long want = referenceInvariantKernel(data, boxes, K);
            for (int i = 0; i < 20_000; i++) {
                long got = invariantKernel(data, boxes, K);
                check(got == want, "invariantKernel", got, want);
            }
        } else {
            run(Long.getLong("millis", 20_000));
        }
    }
}
