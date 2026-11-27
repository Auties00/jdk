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
 * @summary Vector masks that are live across raw memory accesses, as predicates and as the results
 *          of checks, keep their values, with the platform's default for AvoidLosableStateAtTraps.
 * @requires (os.arch == "amd64" | os.arch == "x86_64" | os.arch == "aarch64") & vm.compiler2.enabled
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run main/othervm/timeout=300
 *      -XX:CompileCommand=exclude,*TestMaskKillAtRawAccesses::reference*
 *      --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
 *      compiler.runtime.safepoints.TestMaskKillAtRawAccesses
 */

/*
 * @test id=forced
 * @bug 8365196
 * @summary The same with AvoidLosableStateAtTraps forced on and the scheduling of nodes randomized,
 *          so that the search for the raw accesses that must kill the mask registers, and its
 *          repetition on the allocated code, see many orders of the masks and the accesses.
 * @requires (os.arch == "amd64" | os.arch == "x86_64" | os.arch == "aarch64") & vm.compiler2.enabled
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run main/othervm/timeout=300 -XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps
 *      -XX:+StressGCM -XX:+StressLCM
 *      -XX:CompileCommand=exclude,*TestMaskKillAtRawAccesses::reference*
 *      --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
 *      compiler.runtime.safepoints.TestMaskKillAtRawAccesses
 */

/*
 * @test id=kills
 * @bug 8365196
 * @summary With the mode, on AVX-512 with VL, BW and DQ, a mask that is live across a raw access
 *          and reaches a check afterwards is protected (the check reads a copy that was kept on the
 *          stack), and no raw access kills the mask registers; where the masks live across a raw
 *          access are only predicates of vector instructions, nothing is done.
 * @requires (os.arch == "amd64" | os.arch == "x86_64") & vm.compiler2.enabled & vm.flagless
 * @requires vm.cpu.features ~= ".*avx512vl.*" & vm.cpu.features ~= ".*avx512bw.*" & vm.cpu.features ~= ".*avx512dq.*"
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run driver compiler.runtime.safepoints.TestMaskKillAtRawAccesses kills
 */

package compiler.runtime.safepoints;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteOrder;
import jdk.incubator.vector.IntVector;
import jdk.incubator.vector.VectorMask;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestMaskKillAtRawAccesses {
    static final VectorSpecies<Integer> S = IntVector.SPECIES_PREFERRED;
    static final ValueLayout.OfInt INT = ValueLayout.JAVA_INT;
    static final ByteOrder ORDER = ByteOrder.nativeOrder();
    static final int N = 1003; // no multiple of a vector length, so the masked loops have a partial tail

    static final int[] values = new int[N];
    static final int[] indices = new int[N];
    static final int[] target = new int[N];
    static final int[] referenceTarget = new int[N];

    static {
        for (int i = 0; i < N; i++) {
            values[i] = (i * 37) % 200;
            indices[i] = (i * 613) % N;
        }
    }

    // A compare mask that is only the predicate of an add, live across a raw read. No check reads a
    // mask in this method.
    static int predicateKernel(int[] a, MemorySegment s) {
        IntVector acc = IntVector.zero(S);
        IntVector limit = IntVector.broadcast(S, 100);
        int sum = 0;
        for (int i = 0; i < S.loopBound(a.length); i += S.length()) {
            IntVector v = IntVector.fromArray(S, a, i);
            VectorMask<Integer> lt = v.compare(VectorOperators.LT, limit);
            sum += s.getAtIndex(INT, i);
            acc = acc.add(v, lt);
        }
        return sum + acc.reduceLanes(VectorOperators.ADD);
    }

    static int referencePredicateKernel(int[] a, MemorySegment s) {
        int sum = 0;
        int bound = S.loopBound(a.length);
        for (int i = 0; i < bound; i++) {
            if (a[i] < 100) {
                sum += a[i];
            }
            if (i % S.length() == 0) {
                sum += s.getAtIndex(INT, i);
            }
        }
        return sum;
    }

    // The result of an index check, live across a raw read whose address depends on it and tested
    // after the read.
    static int checkKernel(int[] ix, int length, MemorySegment s) {
        int sum = 0;
        for (int i = 0; i < S.loopBound(ix.length); i += S.length()) {
            IntVector vix = IntVector.fromArray(S, ix, i);
            VectorMask<Integer> bad = vix.compare(VectorOperators.LT, 0).or(vix.compare(VectorOperators.GE, length));
            sum += s.getAtIndex(INT, bad.trueCount());
            if (bad.anyTrue()) {
                sum += 1_000_000;
            }
        }
        return sum;
    }

    static int referenceCheckKernel(int[] ix, int length, MemorySegment s) {
        int sum = 0;
        int bound = S.loopBound(ix.length);
        for (int i = 0; i < bound; i += S.length()) {
            int bad = 0;
            for (int j = 0; j < S.length(); j++) {
                if (ix[i + j] < 0 || ix[i + j] >= length) {
                    bad++;
                }
            }
            sum += s.getAtIndex(INT, bad);
            if (bad > 0) {
                sum += 1_000_000;
            }
        }
        return sum;
    }

    // A scatter, whose index check the Vector API computes in mask registers, next to raw reads.
    static void scatterKernel(int[] a, int[] ix, int[] to, MemorySegment s) {
        for (int i = 0; i < S.loopBound(a.length); i += S.length()) {
            IntVector v = IntVector.fromArray(S, a, i).add(s.getAtIndex(INT, i));
            v.intoArray(to, 0, ix, i);
            to[0] += s.getAtIndex(INT, i + 1);
        }
    }

    static void referenceScatterKernel(int[] a, int[] ix, int[] to, MemorySegment s) {
        int bound = S.loopBound(a.length);
        for (int i = 0; i < bound; i += S.length()) {
            int w = s.getAtIndex(INT, i);
            for (int j = 0; j < S.length(); j++) {
                to[ix[i + j]] = a[i + j] + w;
            }
            to[0] += s.getAtIndex(INT, i + 1);
        }
    }

    // Masked segment accesses with a partial tail, whose masks the Vector API checks, around a raw
    // read.
    static int tailKernel(MemorySegment from, MemorySegment to, int n) {
        int sum = 0;
        for (int i = 0; i < n; i += S.length()) {
            VectorMask<Integer> m = S.indexInRange(i, n);
            IntVector v = IntVector.fromMemorySegment(S, from, 4L * i, ORDER, m);
            sum += from.getAtIndex(INT, i);
            v.add(1, m).intoMemorySegment(to, 4L * i, ORDER, m);
        }
        return sum;
    }

    static int referenceTailKernel(MemorySegment from, MemorySegment to, int n) {
        int sum = 0;
        for (int i = 0; i < n; i++) {
            if (i % S.length() == 0) {
                sum += from.getAtIndex(INT, i);
            }
            to.setAtIndex(INT, i, from.getAtIndex(INT, i) + 1);
        }
        return sum;
    }

    static void check(boolean ok, String what, Object got, Object want) {
        if (!ok) {
            throw new RuntimeException(what + ": got " + got + ", expected " + want);
        }
    }

    static void run(int rounds, String only) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment from = arena.allocate(4L * N, 64);
            MemorySegment to = arena.allocate(4L * N, 64);
            MemorySegment referenceTo = arena.allocate(4L * N, 64);
            for (int i = 0; i < N; i++) {
                from.setAtIndex(INT, i, values[i] * 3 - 7);
            }
            int[] badIndices = indices.clone();
            badIndices[N / 2] = N;
            badIndices[7] = -1;

            final int wantPredicate = referencePredicateKernel(values, from);
            final int wantCheck = referenceCheckKernel(indices, N, from);
            final int wantBadCheck = referenceCheckKernel(badIndices, N, from);
            referenceScatterKernel(values, indices, referenceTarget, from);
            final int wantTail = referenceTailKernel(from, referenceTo, N);

            for (int round = 0; round < rounds; round++) {
                if (only == null || only.equals("predicate")) {
                    int got = predicateKernel(values, from);
                    check(got == wantPredicate, "predicateKernel", got, wantPredicate);
                }
                if (only == null || only.equals("check")) {
                    int got = checkKernel(indices, N, from);
                    check(got == wantCheck, "checkKernel", got, wantCheck);
                    if (round % 16 == 15) {
                        got = checkKernel(badIndices, N, from);
                        check(got == wantBadCheck, "checkKernel with bad indices", got, wantBadCheck);
                    }
                }
                if (only == null) {
                    java.util.Arrays.fill(target, 0);
                    scatterKernel(values, indices, target, from);
                    check(java.util.Arrays.equals(target, referenceTarget), "scatterKernel", "a difference", "none");
                    if (round % 64 == 63) {
                        try {
                            scatterKernel(values, badIndices, target, from);
                            throw new RuntimeException("no IndexOutOfBoundsException");
                        } catch (IndexOutOfBoundsException expected) {
                        }
                    }
                    int got = tailKernel(from, to, N);
                    check(got == wantTail, "tailKernel", got, wantTail);
                    check(to.mismatch(referenceTo) == -1, "tailKernel", "a difference", "none");
                }
            }
        }
    }

    static OutputAnalyzer child(String kernel) throws Exception {
        OutputAnalyzer out = ProcessTools.executeTestJava(
            "-XX:+UnlockDiagnosticVMOptions", "-XX:+AvoidLosableStateAtTraps", "-XX:-TieredCompilation", "-Xbatch",
            "-XX:CompileCommand=compileonly,*TestMaskKillAtRawAccesses::" + kernel + "Kernel",
            "-Xlog:jit+compilation=debug",
            "--add-modules", "jdk.incubator.vector", "--enable-native-access=ALL-UNNAMED",
            TestMaskKillAtRawAccesses.class.getName(), "child", kernel);
        out.shouldHaveExitValue(0);
        return out;
    }

    static void kills() throws Exception {
        child("predicate").shouldContain(
            "no-trap: 0 raw accesses of predicateKernel kill the mask registers, 0 masks protected for their checks");
        child("check").shouldMatch(
            "no-trap: 0 raw accesses of checkKernel kill the mask registers, [1-9][0-9]* masks protected for their checks");
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("kills")) {
            kills();
        } else if (args.length > 1 && args[0].equals("child")) {
            run(20_000, args[1]);
        } else {
            run(Integer.getInteger("rounds", 4_000), null);
        }
    }
}
