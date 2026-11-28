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
 * @test id=ir
 * @bug 8365196
 * @summary A masked Vector API access to a native memory segment is an access to raw memory: it
 *          has no memory barrier, so the loads of the segment's fields do not depend on the memory
 *          state of the loop and their checks can leave it.
 * @requires vm.compiler2.enabled
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run driver compiler.vectorapi.TestMaskedNativeSegmentChecks ir
 */

/*
 * @test id=close
 * @bug 8365196
 * @summary A loop of masked accesses whose checks of the segment were hoisted stops with an
 *          IllegalStateException when another thread closes the arena, and keeps checking its
 *          bounds and the thread that owns a confined arena.
 * @requires vm.compiler2.enabled
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run main/othervm/timeout=300 --add-modules jdk.incubator.vector -XX:-TieredCompilation
 *      compiler.vectorapi.TestMaskedNativeSegmentChecks close
 */

package compiler.vectorapi;

import compiler.lib.ir_framework.Check;
import compiler.lib.ir_framework.CompilePhase;
import compiler.lib.ir_framework.IR;
import compiler.lib.ir_framework.IRNode;
import compiler.lib.ir_framework.Test;
import compiler.lib.ir_framework.TestFramework;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicReference;
import jdk.incubator.vector.IntVector;
import jdk.incubator.vector.VectorMask;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;

public class TestMaskedNativeSegmentChecks {
    static final VectorSpecies<Integer> S = IntVector.SPECIES_PREFERRED;
    static final ByteOrder LE = ByteOrder.LITTLE_ENDIAN;
    // not a multiple of any vector length, so the last iteration has a partial mask
    static final int N = 1003;

    static MemorySegment src = Arena.global().allocate(4L * N, 64);
    static MemorySegment dst = Arena.global().allocate(4L * N, 64);
    static final int WANT;

    static {
        int sum = 0;
        for (int i = 0; i < N; i++) {
            int v = (i * 37) % 200;
            src.setAtIndex(ValueLayout.JAVA_INT, i, v);
            sum += v;
        }
        WANT = sum;
    }

    static int maskedSum(MemorySegment ms, int n) {
        IntVector acc = IntVector.zero(S);
        for (int i = 0; i < n; i += S.length()) {
            VectorMask<Integer> m = S.indexInRange(i, n);
            acc = acc.add(IntVector.fromMemorySegment(S, ms, 4L * i, LE, m), m);
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    @Test
    @IR(failOn = {"MemBarCPUOrder"}, phase = CompilePhase.PRINT_IDEAL,
        applyIfCPUFeatureOr = {"avx2", "true", "sve", "true"})
    @IR(counts = {IRNode.LOAD_VECTOR_MASKED, ">0"},
        applyIfCPUFeatureOr = {"avx2", "true", "sve", "true"})
    static int irMaskedSum() {
        return maskedSum(src, N);
    }

    @Check(test = "irMaskedSum")
    static void checkMaskedSum(int sum) {
        if (sum != WANT) {
            throw new RuntimeException("masked sum " + sum + ", expected " + WANT);
        }
    }

    @Test
    @IR(failOn = {"MemBarCPUOrder"}, phase = CompilePhase.PRINT_IDEAL,
        applyIfCPUFeatureOr = {"avx2", "true", "sve", "true"})
    @IR(counts = {IRNode.LOAD_VECTOR_MASKED, ">0", IRNode.STORE_VECTOR_MASKED, ">0"},
        applyIfCPUFeatureOr = {"avx2", "true", "sve", "true"})
    static void irMaskedCopy() {
        for (int i = 0; i < N; i += S.length()) {
            VectorMask<Integer> m = S.indexInRange(i, N);
            IntVector.fromMemorySegment(S, src, 4L * i, LE, m).add(1, m).intoMemorySegment(dst, 4L * i, LE, m);
        }
    }

    @Check(test = "irMaskedCopy")
    static void checkMaskedCopy() {
        for (int i = 0; i < N; i++) {
            int got = dst.getAtIndex(ValueLayout.JAVA_INT, i);
            int want = (i * 37) % 200 + 1;
            if (got != want) {
                throw new RuntimeException("masked copy at " + i + ": " + got + ", expected " + want);
            }
        }
    }

    static void expect(Class<? extends RuntimeException> type, Runnable action, String what) {
        try {
            action.run();
        } catch (RuntimeException e) {
            if (type.isInstance(e)) {
                return;
            }
            throw new RuntimeException(what + ": " + e, e);
        }
        throw new RuntimeException(what + ": no " + type.getSimpleName());
    }

    static void close() throws Exception {
        for (int i = 0; i < 20_000; i++) {
            if (maskedSum(src, N) != WANT) {
                throw new RuntimeException("masked sum");
            }
        }
        // A mask with a lane past the end of the segment, in the compiled loop.
        expect(IndexOutOfBoundsException.class, () -> maskedSum(src, N + 1), "past the end");
        // A segment whose arena was closed before the call.
        Arena closed = Arena.ofConfined();
        MemorySegment stale = closed.allocate(4L * N, 64);
        closed.close();
        expect(IllegalStateException.class, () -> maskedSum(stale, N), "closed arena");
        // A segment confined to another thread.
        try (Arena confined = Arena.ofConfined()) {
            MemorySegment owned = confined.allocate(4L * N, 64);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread other = new Thread(() -> {
                try {
                    expect(WrongThreadException.class, () -> maskedSum(owned, N), "confined to another thread");
                } catch (Throwable t) {
                    failure.set(t);
                }
            });
            other.start();
            other.join();
            if (failure.get() != null) {
                throw new RuntimeException(failure.get());
            }
        }
        // An arena that another thread closes while the loop runs: the loop must not go on with
        // the checks it hoisted.
        for (int round = 0; round < 30; round++) {
            Arena shared = Arena.ofShared();
            MemorySegment ms = shared.allocate(4L * N, 64);
            AtomicReference<Throwable> ended = new AtomicReference<>();
            Thread reader = new Thread(() -> {
                try {
                    long sum = 0;
                    while (true) {
                        sum += maskedSum(ms, N);
                    }
                } catch (Throwable t) {
                    ended.set(t);
                }
            });
            reader.start();
            Thread.sleep(50);
            shared.close();
            reader.join();
            if (!(ended.get() instanceof IllegalStateException)) {
                throw new RuntimeException("round " + round + ": the reader ended with " + ended.get(), ended.get());
            }
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("close")) {
            close();
        } else {
            TestFramework.runWithFlags("--add-modules=jdk.incubator.vector");
        }
    }
}
