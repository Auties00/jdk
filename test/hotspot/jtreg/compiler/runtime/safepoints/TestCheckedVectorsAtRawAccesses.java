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
 * @summary Vectors that a bounds check reads after a raw memory access (the lane numbers of the
 *          check of a masked access, the index vector of a gather, a check that the program keeps
 *          in a vector, a check that the program writes with allTrue()) keep their values, and
 *          the checks throw where they must.
 * @requires (os.arch == "amd64" | os.arch == "x86_64" | os.arch == "aarch64") & vm.compiler2.enabled
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run main/othervm/timeout=300
 *      --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
 *      compiler.runtime.safepoints.TestCheckedVectorsAtRawAccesses
 */

/*
 * @test id=forced
 * @bug 8365196
 * @summary The same with AvoidLosableStateAtTraps forced on and the scheduling of nodes randomized.
 * @requires (os.arch == "amd64" | os.arch == "x86_64" | os.arch == "aarch64") & vm.compiler2.enabled
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run main/othervm/timeout=300 -XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps
 *      -XX:+StressGCM -XX:+StressLCM
 *      --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
 *      compiler.runtime.safepoints.TestCheckedVectorsAtRawAccesses
 */

/*
 * @test id=protects
 * @bug 8365196
 * @summary With the mode, each of the kernels has a mask or a vector that is live across a raw
 *          access and reaches a check, and the readers that check get a copy from the stack or
 *          are made in front of the access; the tests of all lanes read their masks as integers.
 * @requires (os.arch == "amd64" | os.arch == "x86_64") & vm.compiler2.enabled & vm.flagless
 * @requires vm.cpu.features ~= ".*avx2.*"
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @run driver compiler.runtime.safepoints.TestCheckedVectorsAtRawAccesses protects
 */

/*
 * @test id=fault
 * @bug 8365196
 * @summary The kernels with a page error at their raw access, which is resumed in the compiled
 *          frame: on a Windows build that loses vector state there, the checks after the access
 *          must still see the lanes they were computed with. The page error is that of a file
 *          mapped from a virtual disk that is detached, which needs an administrator, so the test
 *          only runs with -Dtest.xstate.vhd=true.
 * @requires os.family == "windows" & (os.arch == "amd64" | os.arch == "x86_64") & vm.compiler2.enabled
 * @requires vm.cpu.features ~= ".*avx2.*"
 * @modules jdk.incubator.vector
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @comment Without on-stack replacement, so that the kernels run compiled from their first
 *          iteration: an interpreted read throws at once.
 * @run main/othervm/native/timeout=600 -XX:UseAVX=2 -XX:-TieredCompilation -Xbatch -XX:-UseOnStackReplacement
 *      -XX:CompileCommand=dontinline,*TestCheckedVectorsAtRawAccesses::*Kernel
 *      -Xlog:os+cpu=info --add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED
 *      compiler.runtime.safepoints.TestCheckedVectorsAtRawAccesses fault
 */

package compiler.runtime.safepoints;

import java.io.File;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Arrays;
import jdk.incubator.vector.IntVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorMask;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import jtreg.SkippedException;

/*
 * Each kernel reads an int from a segment (a raw access of a compilation with unsafe accesses) and
 * then runs a bounds check that reads a vector computed before the read. The kernels with good
 * arguments are compared against what they must compute, and with bad arguments must throw. With
 * "fault", the read of one iteration takes a page error, after which an affected Windows build has
 * zeroed the upper halves of the vector registers: the arguments are bad from that iteration on
 * and only in the lanes of the upper halves, so a check that reads a register passes.
 */
public class TestCheckedVectorsAtRawAccesses {
    static final VectorSpecies<Integer> S = IntVector.SPECIES_256;
    static final VectorSpecies<Integer> S128 = IntVector.SPECIES_128;
    static final VectorSpecies<Long> L256 = LongVector.SPECIES_256;
    static final ValueLayout.OfInt INT = ValueLayout.JAVA_INT_UNALIGNED;
    static final ByteOrder ORDER = ByteOrder.nativeOrder();
    static final boolean FILL = Boolean.getBoolean("test.xstate.fill");

    static final int ITERS = 256;
    static final long LIMIT = 256;           // the bytes of the segment the masked store writes to
    static final long STORE_AT = LIMIT - 24; // lanes 0-5 are inside, lanes 6 and 7 are outside
    // Lane 0 is never set: a mask with every lane set is checked as a whole range, not by lane, and
    // the test for that case must fail in the lanes that a resumed exception keeps.
    static final int GUARD = 0x11111111;
    static final long GOOD_BITS = 0x3E;
    static final long BAD_BITS = 0xBE;       // and lane 7

    // The check of the masked store compares the lane numbers, a vector the loop keeps in a
    // register, with the lanes left before the limit. The store is never wholly in bounds, so
    // every iteration runs the check. The offset of the store, and so the limit of the lanes,
    // depends on the value read: were the limit a vector of the loop like the lane numbers, a
    // resumed fault would zero both, and zero is not below zero.
    static int laneCheckKernel(MemorySegment in, long[] pos, MemorySegment out, long[] at, long[] bits,
                               int[] done, int iters) {
        if (FILL) {
            ExceptionXState.fillBelowStack();
        }
        IntVector v = IntVector.broadcast(S, 0x5A5A5A5A);
        int sum = 0;
        for (int i = 0; i < iters; i++) {
            int x = in.get(INT, pos[i]);
            sum += x;
            VectorMask<Integer> m = VectorMask.fromLong(S, bits[i + (x & 1)]);
            v.intoMemorySegment(out, at[i + (x & 1)], ORDER, m);
            done[0] = i + 1; // how far the stores got
        }
        return sum;
    }

    // The check of the gather compares the index vector, which does not change in the loop, with
    // the length of the array of the iteration.
    static int gatherKernel(MemorySegment in, long[] pos, int[][] from, int[] sel, int[] ix, int iters) {
        if (FILL) {
            ExceptionXState.fillBelowStack();
        }
        IntVector acc = IntVector.zero(S);
        int sum = 0;
        for (int i = 0; i < iters; i++) {
            int x = in.get(INT, pos[i]);
            sum += x;
            acc = acc.add(IntVector.fromArray(S, from[sel[i] + (x & 2)], 0, ix, 0));
        }
        return sum + acc.reduceLanes(VectorOperators.ADD);
    }

    // A check that the program computes itself on a widened copy of its indices and keeps in
    // vectors of long lanes across the read: the address of the read depends on both masks, and
    // which of them is tested depends on the value read.
    static void userCheckKernel(MemorySegment in, long[] pos, int[] ix, long[] limits, byte[] flags, int iters) {
        if (FILL) {
            ExceptionXState.fillBelowStack();
        }
        for (int i = 0; i < iters; i++) {
            IntVector vix = IntVector.fromArray(S128, ix, 4 * i);
            LongVector wide = vix.convertShape(VectorOperators.I2L, L256, 0).reinterpretAsLongs();
            VectorMask<Long> bad0 = wide.compare(VectorOperators.GE, limits[0]);
            VectorMask<Long> bad1 = wide.compare(VectorOperators.GE, limits[1]);
            int x = in.get(INT, pos[i] + 4L * (bad0.trueCount() + bad1.trueCount()));
            VectorMask<Long> bad = (x & 1) == 0 ? bad0 : bad1;
            flags[i] = (byte) (bad.anyTrue() ? 1 : 0);
        }
    }

    // Checks that the program writes with allTrue(). The first reads, after the read of the
    // segment, the negation of a mask of the bad indices that is made before it (the address of the
    // read depends on the mask). The second compares the indices, a vector from before the read (the
    // address depends on a lane of it), with a limit that depends on the value read, so the compare
    // follows the read. A resumed fault that zeroed the upper lanes of the mask or of the indices
    // would let both pass: no lane of a zeroed mask is bad, and index 0 is below the limit.
    static void allTrueCheckKernel(MemorySegment in, long[] pos, int[] ix, int[] limits, byte[] flags, int iters) {
        if (FILL) {
            ExceptionXState.fillBelowStack();
        }
        for (int i = 0; i < iters; i++) {
            IntVector vix = IntVector.fromArray(S, ix, 8 * i);
            VectorMask<Integer> bad = vix.compare(VectorOperators.GE, limits[0]);
            int x = in.get(INT, pos[i] + 4L * (bad.trueCount() + (vix.lane(0) & 1)));
            boolean none = bad.not().allTrue();
            boolean below = vix.compare(VectorOperators.LT, limits[x & 1]).allTrue();
            flags[i] = (byte) ((none ? 0 : 1) | (below ? 0 : 2));
        }
    }

    static void check(boolean ok, String what) {
        if (!ok) {
            throw new RuntimeException(what);
        }
    }

    // What the kernels take: everything good up to the iteration badFrom, bad from there on, in the
    // lanes of the upper halves only.
    static final class Arguments {
        final long[] pos = new long[ITERS];
        final long[] bits = new long[ITERS + 1];
        final long[] at = new long[ITERS + 1];
        final int[][] from = new int[4][];
        final int[] sel = new int[ITERS];
        // Index 60 is in the arrays of 64 elements and not in those of 32.
        final int[] ix = { 3, 1, 4, 1, 5, 9, 2, 6 };
        final int[] badIx = { 3, 1, 4, 1, 5, 9, 2, 60 };
        final int[] userIx = new int[4 * ITERS];
        final long[] limits = { 1000, 1000 };
        final byte[] flags = new byte[ITERS];
        // The indices of allTrueCheckKernel: lane 0 is even, lane 7 is the bad one.
        final int[] allIx = new int[8 * ITERS];
        final int[] intLimits = { 1000, 1000 };
        final byte[] allFlags = new byte[ITERS];
        final int[] done = new int[1];
        final MemorySegment whole;
        final MemorySegment out;

        Arguments(Arena arena, int badFrom) {
            Arrays.fill(bits, GOOD_BITS);
            Arrays.fill(at, STORE_AT);
            for (int i = badFrom; i < bits.length; i++) {
                bits[i] = BAD_BITS;
            }
            for (int i = 0; i < from.length; i++) {
                from[i] = new int[(i & 1) == 0 ? 64 : 32];
                for (int j = 0; j < from[i].length; j++) {
                    from[i][j] = j * 7 + i;
                }
            }
            for (int i = 0; i < userIx.length; i++) {
                userIx[i] = i % 1000;
            }
            for (int i = badFrom; i < ITERS; i++) {
                userIx[4 * i + 3] = 5000;
            }
            for (int i = 0; i < ITERS; i++) {
                // With bad arguments, the arrays of 64 elements up to badFrom.
                sel[i] = badFrom > ITERS ? i & 1 : (i < badFrom ? 0 : 1);
            }
            Arrays.fill(flags, (byte) 2);
            for (int i = 0; i < allIx.length; i++) {
                allIx[i] = (i % 400) * 2;
            }
            for (int i = badFrom; i < ITERS; i++) {
                allIx[8 * i + 7] = 5000;
            }
            Arrays.fill(allFlags, (byte) 4);
            whole = arena.allocate(LIMIT + 64, 64);
            out = whole.asSlice(0, LIMIT);
            // Not zero: a lane that a resumed exception zeroed would store zero.
            for (long at = LIMIT; at < LIMIT + 64; at += 4) {
                whole.set(INT, at, GUARD);
            }
        }

        void checkGuard() {
            for (long at = LIMIT; at < LIMIT + 64; at += 4) {
                check(whole.get(INT, at) == GUARD, "the masked store wrote outside its segment, at " + at);
            }
        }
    }

    static void good(Arena arena, MemorySegment in, String only) {
        Arguments a = new Arguments(arena, ITERS + 1);
        if (only == null || only.equals("laneCheck")) {
            int sum = laneCheckKernel(in, a.pos, a.out, a.at, a.bits, a.done, ITERS);
            check(sum == 0, "laneCheckKernel: " + sum);
            for (long at = STORE_AT + 4; at < LIMIT; at += 4) {
                check(a.whole.get(INT, at) == 0x5A5A5A5A, "laneCheckKernel did not store at " + at);
            }
            a.checkGuard();
        }
        if (only == null || only.equals("gather")) {
            int want = 0;
            for (int i = 0; i < ITERS; i++) {
                for (int j = 0; j < a.ix.length; j++) {
                    want += a.from[i & 1][a.ix[j]];
                }
            }
            int sum = gatherKernel(in, a.pos, a.from, a.sel, a.ix, ITERS);
            check(sum == want, "gatherKernel: " + sum + ", expected " + want);
        }
        if (only == null || only.equals("userCheck")) {
            userCheckKernel(in, a.pos, a.userIx, a.limits, a.flags, ITERS);
            for (int i = 0; i < ITERS; i++) {
                check(a.flags[i] == 0, "userCheckKernel found a bad index at " + i);
            }
        }
        if (only == null || only.equals("allTrueCheck")) {
            allTrueCheckKernel(in, a.pos, a.allIx, a.intLimits, a.allFlags, ITERS);
            for (int i = 0; i < ITERS; i++) {
                check(a.allFlags[i] == 0, "allTrueCheckKernel: flag " + a.allFlags[i] + " at " + i);
            }
        }
    }

    static void bad(Arena arena, MemorySegment in) {
        final int badFrom = ITERS / 2;
        Arguments a = new Arguments(arena, badFrom);
        try {
            laneCheckKernel(in, a.pos, a.out, a.at, a.bits, a.done, ITERS);
            throw new RuntimeException("laneCheckKernel: no IndexOutOfBoundsException");
        } catch (IndexOutOfBoundsException expected) {
        }
        a.checkGuard();
        try {
            gatherKernel(in, a.pos, a.from, a.sel, a.badIx, ITERS);
            throw new RuntimeException("gatherKernel: no IndexOutOfBoundsException");
        } catch (IndexOutOfBoundsException expected) {
        }
        userCheckKernel(in, a.pos, a.userIx, a.limits, a.flags, ITERS);
        for (int i = 0; i < ITERS; i++) {
            check(a.flags[i] == (i < badFrom ? 0 : 1), "userCheckKernel: flag " + a.flags[i] + " at " + i);
        }
        allTrueCheckKernel(in, a.pos, a.allIx, a.intLimits, a.allFlags, ITERS);
        for (int i = 0; i < ITERS; i++) {
            check(a.allFlags[i] == (i < badFrom ? 0 : 3), "allTrueCheckKernel: flag " + a.allFlags[i] + " at " + i);
        }
    }

    static void run(int rounds, String only) {
        for (int round = 0; round < rounds; round++) {
            // An arena per round, for the segments of the arguments.
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment in = arena.allocate(4096, 64);
                good(arena, in, only);
                if (only == null && round % 32 == 31) {
                    bad(arena, in);
                }
            }
        }
    }

    static void protects() throws Exception {
        for (String kernel : new String[] { "laneCheck", "gather", "userCheck", "allTrueCheck" }) {
            OutputAnalyzer out = ProcessTools.executeTestJava(
                "-XX:+UnlockDiagnosticVMOptions", "-XX:+AvoidLosableStateAtTraps", "-XX:-TieredCompilation", "-Xbatch",
                "-XX:CompileCommand=compileonly,*TestCheckedVectorsAtRawAccesses::" + kernel + "Kernel",
                "-Xlog:jit+compilation=debug", "-Dtest.xstate.fill=false",
                "--add-modules", "jdk.incubator.vector", "--enable-native-access=ALL-UNNAMED",
                TestCheckedVectorsAtRawAccesses.class.getName(), "child", kernel);
            out.shouldHaveExitValue(0);
            out.shouldMatch("no-trap: [0-9]+ raw accesses of " + kernel + "Kernel kill the mask registers, "
                            + "[1-9][0-9]* values protected for their checks");
            out.shouldNotContain("retrying");
            out.shouldNotContain("no copy for");
            if (kernel.equals("allTrueCheck")) {
                // Both tests of all lanes read their masks in general purpose registers.
                out.shouldMatch("no-trap: 2 of 2 tests of masks of allTrueCheckKernel read the mask as an integer");
            }
        }
    }

    // The virtual disk and the file on it.

    static void diskpart(Path dir, String... lines) throws Exception {
        Path script = dir.resolve("diskpart.txt");
        Files.write(script, Arrays.asList(lines));
        ProcessBuilder pb = new ProcessBuilder("diskpart", "/s", script.toString());
        pb.redirectErrorStream(true);
        Process p = pb.start();
        String output = new String(p.getInputStream().readAllBytes());
        int code = p.waitFor();
        System.out.println(output);
        if (code != 0) {
            throw new SkippedException("diskpart failed with " + code + " (not an administrator?)");
        }
    }

    static char freeLetter() {
        for (char c = 'Q'; c <= 'Z'; c++) {
            if (!new File(c + ":\\").exists()) {
                return c;
            }
        }
        throw new SkippedException("no free drive letter");
    }

    static final long PAGE = 4096;
    static final long FILE_SIZE = 64 * PAGE * 16;

    static void fault() throws Exception {
        if (!Boolean.getBoolean("test.xstate.vhd")) {
            throw new SkippedException("needs -Dtest.xstate.vhd=true and an administrator: it creates a virtual disk");
        }
        if (!FILL) {
            // The kernels plant the stale header only with this property, which is read once.
            throw new SkippedException("needs -Dtest.xstate.fill=true");
        }
        System.out.println("Native probe: " + ExceptionXState.probe());
        Path dir = Files.createTempDirectory(Path.of("").toAbsolutePath(), "vhd").normalize();
        Path vhd = dir.resolve("xstate.vhd");
        char letter = freeLetter();
        String select = "select vdisk file=\"" + vhd + "\"";
        diskpart(dir, "create vdisk file=\"" + vhd + "\" maximum=64 type=fixed", select, "attach vdisk",
                 "create partition primary", "format fs=ntfs quick label=xstate", "assign letter=" + letter);
        Path file = Path.of(letter + ":\\data.bin");
        boolean attached = true;
        try (Arena arena = Arena.ofShared()) {
            Files.write(file, new byte[(int) FILE_SIZE]);
            // Detached and attached again, so that no page of the file is in memory.
            diskpart(dir, select, "detach vdisk");
            diskpart(dir, select, "attach vdisk");
            if (!Files.exists(file)) {
                throw new SkippedException("the volume did not come back as " + letter + ":");
            }
            MemorySegment mapped;
            try (FileChannel channel = FileChannel.open(file, StandardOpenOption.READ)) {
                mapped = channel.map(FileChannel.MapMode.READ_ONLY, 0, FILE_SIZE, arena);
            }
            MemorySegment in = arena.allocate(4096, 64);
            // Compiles the kernels, with the types of both segments.
            MemorySegment firstPage = mapped.asSlice(0, PAGE);
            for (int round = 0; round < 8000; round++) {
                try (Arena local = Arena.ofConfined()) {
                    good(local, in, null);
                    good(local, firstPage, null);
                    if (round % 32 == 31) {
                        // Both outcomes of the checks of allTrueCheckKernel, so that neither is
                        // compiled as an uncommon trap.
                        Arguments b = new Arguments(local, ITERS / 2);
                        allTrueCheckKernel(in, b.pos, b.allIx, b.intLimits, b.allFlags, ITERS);
                        allTrueCheckKernel(firstPage, b.pos, b.allIx, b.intLimits, b.allFlags, ITERS);
                    }
                }
            }
            diskpart(dir, select, "detach vdisk");
            attached = false;

            final int faultAt = ITERS / 2;
            int failures = 0;

            Arguments a = new Arguments(arena, faultAt);
            a.pos[faultAt] = 100 * PAGE;
            String outcome;
            try {
                laneCheckKernel(mapped, a.pos, a.out, a.at, a.bits, a.done, ITERS);
                outcome = "returned";
            } catch (IndexOutOfBoundsException | InternalError e) {
                outcome = e.getClass().getSimpleName();
            }
            boolean wrote = false;
            for (long at = LIMIT; at < LIMIT + 64; at += 4) {
                wrote |= a.whole.get(INT, at) != GUARD;
            }
            System.out.println("laneCheckKernel: " + outcome + ", wrote outside: " + wrote + ", stores done: " + a.done[0]
                               + " (page error in iteration " + faultAt + ")");

            // With the bad mask from the iteration of the page error on, no store of that iteration or
            // of a later one may pass its check.
            if (wrote || a.done[0] > faultAt || outcome.equals("returned")) {
                failures++;
            }

            a = new Arguments(arena, faultAt);
            a.pos[faultAt] = 200 * PAGE;
            try {
                gatherKernel(mapped, a.pos, a.from, a.sel, a.badIx, ITERS);
                outcome = "returned";
            } catch (IndexOutOfBoundsException | InternalError e) {
                outcome = e.getClass().getSimpleName();
            }
            // A lane of the index vector that comes back zeroed selects index 0, which is in range, so
            // a check that reads the register lets nothing out of bounds: this one only shows which
            // copy the check read (IndexOutOfBoundsException: the one from before the page error).
            System.out.println("gatherKernel: " + outcome);
            if (outcome.equals("returned")) {
                failures++;
            }

            a = new Arguments(arena, faultAt);
            a.pos[faultAt] = 300 * PAGE;
            try {
                userCheckKernel(mapped, a.pos, a.userIx, a.limits, a.flags, ITERS);
                outcome = "returned";
            } catch (InternalError e) {
                outcome = "InternalError";
            }
            System.out.println("userCheckKernel: " + outcome + ", flag of the faulting iteration: " + a.flags[faultAt]);
            if (a.flags[faultAt] == 0) {
                failures++;
            }

            a = new Arguments(arena, faultAt);
            a.pos[faultAt] = 350 * PAGE;
            try {
                allTrueCheckKernel(mapped, a.pos, a.allIx, a.intLimits, a.allFlags, ITERS);
                outcome = "returned";
            } catch (InternalError e) {
                outcome = "InternalError";
            }
            // Both checks of the faulting iteration must find the bad index of lane 7 (3), unless the
            // iteration did not get to store its flags (4).
            System.out.println("allTrueCheckKernel: " + outcome + ", flags of the faulting iteration: " + a.allFlags[faultAt]);
            if (a.allFlags[faultAt] != 3 && a.allFlags[faultAt] != 4) {
                failures++;
            }

            // Whether the pages really were unreadable.
            boolean error = false;
            try {
                mapped.get(INT, 400 * PAGE);
            } catch (InternalError e) {
                error = true;
            }
            System.out.println("page error on an untouched page: " + error + ", failures: " + failures);
            if (!error) {
                throw new SkippedException("reading the detached file took no page error");
            }
            if (failures != 0) {
                throw new RuntimeException(failures + " kernels let a check pass that a resumed page error must not change");
            }
        } finally {
            if (attached) {
                try {
                    diskpart(dir, select, "detach vdisk");
                } catch (Exception | Error ignored) {
                }
            }
            try {
                Files.deleteIfExists(vhd);
            } catch (Exception ignored) {
            }
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("protects")) {
            protects();
        } else if (args.length > 0 && args[0].equals("fault")) {
            fault();
        } else if (args.length > 1 && args[0].equals("child")) {
            run(12_000, args[1]);
        } else {
            run(Integer.getInteger("rounds", 2_000), null);
        }
    }
}
