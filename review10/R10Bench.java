package review10;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.concurrent.TimeUnit;
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;
import org.openjdk.jmh.annotations.*;

// Kernels for the costs of AvoidLosableStateAtTraps that the earlier reviews did not measure: an implicit null
// check with vector state live across it but not into its null path; values the allocator may rematerialize from
// vector or floating point registers (refused in compilations with unsafe accesses); register pressure on the
// floating point registers (C1 is kept to xmm0-15: run the scalar kernels with -XX:TieredStopAtLevel=1) and on
// the general purpose registers (their spills are kept out of xmm16-31: run with -XX:+UseFPUForSpilling).
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@State(Scope.Thread)
public class R10Bench {
    static final VectorSpecies<Double> S = DoubleVector.SPECIES_PREFERRED;
    static final VectorSpecies<Long> L = LongVector.SPECIES_PREFERRED;

    static final class Box {
        long v;
        Box(long v) { this.v = v; }
    }

    double[] a = new double[1024];
    long[] l = new long[1024];
    Box[] boxes = new Box[128];
    MemorySegment seg = MemorySegment.ofArray(new long[1024]);

    @Setup
    public void setup() {
        for (int i = 0; i < a.length; i++) {
            a[i] = i * 0.25 + 1;
            l[i] = i * 7L + 3;
            seg.setAtIndex(ValueLayout.JAVA_LONG, i, i * 5L + 1);
        }
        for (int i = 0; i < boxes.length; i++) {
            boxes[i] = new Box(i + 1);
        }
    }

    @CompilerControl(CompilerControl.Mode.DONT_INLINE)
    static double opaque(double x) { return x + 1; }

    // A broadcast that C2 computes ahead of the loop is live across the implicit null check of each box, but the
    // null path (an uncommon trap) reads only scalars.
    @Benchmark
    public long nullCheckVectorDeadOnNullPath() {
        long sum = 0;
        long k = l[5];
        Box[] bs = boxes;
        for (int i = 0, j = 0; i < L.loopBound(l.length); i += L.length(), j++) {
            long x = bs[j & (bs.length - 1)].v;
            sum += LongVector.fromArray(L, l, i).add(LongVector.broadcast(L, k)).reduceLanes(VectorOperators.ADD) + x;
        }
        return sum;
    }

    // Twenty live doubles and one-input operations on them (negations, conversions) around a call, which leaves no
    // floating point register: the allocator spills or recomputes. Heap only.
    @Benchmark
    public double scalarFpPressure() {
        return fpPressure(a, 0);
    }

    // The same in a compilation with an unsafe access, where values are not recomputed from xmm16-31.
    @Benchmark
    public double scalarFpPressureUnsafe() {
        return fpPressure(a, seg.getAtIndex(ValueLayout.JAVA_LONG, 3));
    }

    private static double fpPressure(double[] a, long bias) {
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0, s8 = 0, s9 = 0;
        double t0 = 1, t1 = 2, t2 = 3, t3 = 4, t4 = 5, t5 = 6, t6 = 7, t7 = 8, t8 = 9, t9 = 10;
        long bits = bias;
        for (int i = 0; i < a.length; i += 8) {
            double x = a[i];
            float f = (float) x;
            double n = -x;
            int r = (int) x;
            bits += Double.doubleToRawLongBits(x) & 0xff;
            s0 += x * t0; s1 += n * t1; s2 += f * t2; s3 += r * t3; s4 += x + t4;
            s5 += n + t5; s6 += f + t6; s7 += r + t7; s8 += x - t8; s9 += n - t9;
            if ((i & 63) == 0) {
                t0 = opaque(t0);
            }
            t1 += s0 * 1e-9; t2 += n * 1e-9; t3 += f * 1e-9; t4 += r * 1e-9; t5 += x * 1e-9;
        }
        return s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8 + s9 + t0 + t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 + t9 + bits;
    }

    // Twenty live vectors and one-input vector operations (negation, absolute value) with a call per stripe.
    @Benchmark
    public double vectorPressure() {
        return vecPressure(a, 0);
    }

    // The same in a compilation with an unsafe access.
    @Benchmark
    public double vectorPressureUnsafe() {
        return vecPressure(a, seg.getAtIndex(ValueLayout.JAVA_LONG, 3));
    }

    private static double vecPressure(double[] a, long bias) {
        DoubleVector s0 = DoubleVector.zero(S), s1 = s0, s2 = s0, s3 = s0, s4 = s0, s5 = s0, s6 = s0, s7 = s0, s8 = s0, s9 = s0;
        DoubleVector t0 = DoubleVector.broadcast(S, 1), t1 = t0, t2 = t0, t3 = t0, t4 = t0, t5 = t0, t6 = t0, t7 = t0, t8 = t0, t9 = t0;
        double c = bias;
        for (int i = 0; i < S.loopBound(a.length); i += S.length()) {
            DoubleVector x = DoubleVector.fromArray(S, a, i);
            DoubleVector n = x.neg();
            DoubleVector b = x.abs();
            s0 = s0.add(x.mul(t0)); s1 = s1.add(n.mul(t1)); s2 = s2.add(b.mul(t2)); s3 = s3.add(x.mul(t3)); s4 = s4.add(n.mul(t4));
            s5 = s5.add(b.mul(t5)); s6 = s6.add(x.mul(t6)); s7 = s7.add(n.mul(t7)); s8 = s8.add(b.mul(t8)); s9 = s9.add(x.mul(t9));
            if ((i & 63) == 0) {
                c = opaque(c);
            }
            t0 = t0.add(n); t1 = t1.add(b); t2 = t2.add(x); t3 = t3.add(n); t4 = t4.add(b);
            t5 = t5.add(x); t6 = t6.add(n); t7 = t7.add(b); t8 = t8.add(x); t9 = t9.add(n);
        }
        return s0.add(s1).add(s2).add(s3).add(s4).add(s5).add(s6).add(s7).add(s8).add(s9)
                .add(t0).add(t1).add(t2).add(t3).add(t4).add(t5).add(t6).add(t7).add(t8).add(t9)
                .reduceLanes(VectorOperators.ADD) + c;
    }

    // Twenty-four live longs (more than the general purpose registers) and eight doubles around unsafe reads: with
    // UseFPUForSpilling the longs spill to the floating point registers.
    @Benchmark
    public long gprPressureUnsafe() {
        MemorySegment s = seg;
        long a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6, a6 = 7, a7 = 8, a8 = 9, a9 = 10, a10 = 11, a11 = 12;
        long b0 = 1, b1 = 2, b2 = 3, b3 = 4, b4 = 5, b5 = 6, b6 = 7, b7 = 8, b8 = 9, b9 = 10, b10 = 11, b11 = 12;
        double d0 = 0, d1 = 0, d2 = 0, d3 = 0, d4 = 0, d5 = 0, d6 = 0, d7 = 0;
        for (int i = 0; i < 1024; i += 4) {
            long x = s.getAtIndex(ValueLayout.JAVA_LONG, i);
            long y = s.getAtIndex(ValueLayout.JAVA_LONG, i + 1);
            a0 += x ^ b0; a1 += y ^ b1; a2 += x ^ b2; a3 += y ^ b3; a4 += x ^ b4; a5 += y ^ b5;
            a6 += x ^ b6; a7 += y ^ b7; a8 += x ^ b8; a9 += y ^ b9; a10 += x ^ b10; a11 += y ^ b11;
            b0 += a1; b1 += a2; b2 += a3; b3 += a4; b4 += a5; b5 += a6;
            b6 += a7; b7 += a8; b8 += a9; b9 += a10; b10 += a11; b11 += a0;
            d0 += x; d1 += y; d2 += d0; d3 += d1; d4 += d2; d5 += d3; d6 += d4; d7 += d5;
        }
        return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11
             + b0 + b1 + b2 + b3 + b4 + b5 + b6 + b7 + b8 + b9 + b10 + b11
             + (long) (d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7);
    }
}
