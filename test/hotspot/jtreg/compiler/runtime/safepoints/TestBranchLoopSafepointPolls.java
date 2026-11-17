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
 * @test id=c2-branch
 * @bug 8365196
 * @summary Values kept in registers across loop safepoint polls survive frequent safepoints,
 *          with branch-based and trap-based loop polls.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler2.enabled
 * @run main/othervm -Xbatch -XX:-TieredCompilation -XX:+UnlockDiagnosticVMOptions
 *      -XX:+UseBranchLoopSafepointPolls -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      compiler.runtime.safepoints.TestBranchLoopSafepointPolls
 */

/*
 * @test id=c2-branch-stress
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler2.enabled
 * @key randomness
 * @run main/othervm -Xbatch -XX:-TieredCompilation -XX:+UnlockDiagnosticVMOptions
 *      -XX:+UseBranchLoopSafepointPolls -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      -XX:+StressGCM -XX:+StressLCM -XX:+UseFPUForSpilling
 *      compiler.runtime.safepoints.TestBranchLoopSafepointPolls
 */

/*
 * @test id=c2-trap
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler2.enabled
 * @run main/othervm -Xbatch -XX:-TieredCompilation -XX:+UnlockDiagnosticVMOptions
 *      -XX:-UseBranchLoopSafepointPolls -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      compiler.runtime.safepoints.TestBranchLoopSafepointPolls
 */

/*
 * @test id=c1-branch
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler1.enabled
 * @run main/othervm -Xbatch -XX:TieredStopAtLevel=1 -XX:+UnlockDiagnosticVMOptions
 *      -XX:+UseBranchLoopSafepointPolls -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 *      compiler.runtime.safepoints.TestBranchLoopSafepointPolls
 */

package compiler.runtime.safepoints;

/**
 * Runs loops that keep many float, double and reference values in registers (vectorized float
 * loops, 24 double accumulators, objects read after the loop) while safepoints are requested every
 * millisecond, and checks every result against the same computation run before compilation.
 */
public class TestBranchLoopSafepointPolls {
    static final int N = 1024;
    static final int REPEAT = 3000;

    static final class Box {
        final double v;
        Box(double v) { this.v = v; }
    }

    // SuperWord loop with a loop invariant kept in a vector register across the outer loop's poll.
    static void scaleAdd(float[] a, float[] b, float k, int reps) {
        for (int r = 0; r < reps; r++) {
            for (int i = 0; i < a.length; i++) {
                a[i] = a[i] * 0.5f + b[i] * k;
            }
        }
    }

    // 24 double accumulators and two references live across the loop's poll.
    static double accumulate(double[] x, Box p, Box q) {
        double s0 = 0, s1 = 1, s2 = 2, s3 = 3, s4 = 4, s5 = 5, s6 = 6, s7 = 7;
        double s8 = 8, s9 = 9, s10 = 10, s11 = 11, s12 = 12, s13 = 13, s14 = 14, s15 = 15;
        double s16 = 16, s17 = 17, s18 = 18, s19 = 19, s20 = 20, s21 = 21, s22 = 22, s23 = 23;
        for (long i = 0; i < x.length * 64L; i++) {
            double v = x[(int) (i & (x.length - 1))];
            s0 += v * 1.01; s1 += v * 1.02; s2 += v * 1.03; s3 += v * 1.04;
            s4 += v * 1.05; s5 += v * 1.06; s6 += v * 1.07; s7 += v * 1.08;
            s8 += v * 1.09; s9 += v * 1.10; s10 += v * 1.11; s11 += v * 1.12;
            s12 += v * 1.13; s13 += v * 1.14; s14 += v * 1.15; s15 += v * 1.16;
            s16 += v * 1.17; s17 += v * 1.18; s18 += v * 1.19; s19 += v * 1.20;
            s20 += v * 1.21; s21 += v * 1.22; s22 += v * 1.23; s23 += v * 1.24;
        }
        return p.v * (s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8 + s9 + s10 + s11)
             + q.v * (s12 + s13 + s14 + s15 + s16 + s17 + s18 + s19 + s20 + s21 + s22 + s23);
    }

    public static void main(String[] args) {
        float[] b = new float[N];
        double[] x = new double[N];
        for (int i = 0; i < N; i++) {
            b[i] = i * 0.25f;
            x[i] = (i % 7) * 0.125;
        }
        Box p = new Box(1.5);
        Box q = new Box(-0.75);

        // Reference results, computed while the methods are still interpreted.
        float[] expectedA = new float[N];
        scaleAdd(expectedA, b, 3.0f, 2);
        double expectedSum = accumulate(x, p, q);

        int errors = 0;
        for (int r = 0; r < REPEAT; r++) {
            float[] a = new float[N];
            scaleAdd(a, b, 3.0f, 2);
            for (int i = 0; i < N; i++) {
                if (Float.floatToRawIntBits(a[i]) != Float.floatToRawIntBits(expectedA[i])) {
                    if (errors++ < 10) {
                        System.err.println("scaleAdd: a[" + i + "] = " + a[i] + ", expected " + expectedA[i]);
                    }
                    break;
                }
            }
            double sum = accumulate(x, p, q);
            if (Double.doubleToRawLongBits(sum) != Double.doubleToRawLongBits(expectedSum)) {
                if (errors++ < 10) {
                    System.err.println("accumulate: " + sum + ", expected " + expectedSum);
                }
            }
        }
        if (errors > 0) {
            throw new RuntimeException(errors + " wrong results");
        }
    }
}
