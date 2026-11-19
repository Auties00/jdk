package review3;

import java.util.concurrent.TimeUnit;
import jdk.incubator.vector.*;
import org.openjdk.jmh.annotations.*;

// JDK-8365196 review 3: worst cases for the scrub prototype. Each benchmark is a compilation the
// criterion selects (vector state live at a poll or an implicit null check) that also returns
// from a call, or is entered, as often as possible, so that the scrub runs once per few ns.
@State(Scope.Thread)
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@Warmup(iterations = 4, time = 1)
@Measurement(iterations = 6, time = 1)
@Fork(value = 3, jvmArgsAppend = {"--add-modules", "jdk.incubator.vector"})
public class R3Bench {
    static final VectorSpecies<Double> S = DoubleVector.SPECIES_PREFERRED;

    interface Op { double apply(double x); }
    static final class Inc implements Op { public double apply(double x) { return x + 1; } }
    static final class Dbl implements Op { public double apply(double x) { return x * 2; } }
    static final class Neg implements Op { public double apply(double x) { return -x; } }
    static final class Half implements Op { public double apply(double x) { return x * 0.5; } }

    double[] a = new double[1024];
    double[] b = new double[1024];
    Op[] ops = { new Inc(), new Dbl(), new Neg(), new Half() };

    @Setup
    public void setup() {
        for (int i = 0; i < a.length; i++) { a[i] = i * 0.25; b[i] = 1.0 / (i + 1); }
    }

    @CompilerControl(CompilerControl.Mode.DONT_INLINE)
    static double tiny(double x) { return x + 1; }

    // Vector accumulator live around the loop, one static call per vector step: one scrub per call.
    @Benchmark
    public double vectorLoopStaticCall() {
        DoubleVector acc = DoubleVector.zero(S);
        double s = 0;
        for (int i = 0; i < S.loopBound(a.length); i += S.length()) {
            acc = acc.add(DoubleVector.fromArray(S, a, i));
            s = tiny(s);
        }
        return acc.reduceLanes(VectorOperators.ADD) + s;
    }

    // The same with a megamorphic interface call (itable stub) per step.
    @Benchmark
    public double vectorLoopMegamorphicCall() {
        DoubleVector acc = DoubleVector.zero(S);
        double s = 0;
        Op[] o = ops;
        for (int i = 0, k = 0; i < S.loopBound(a.length); i += S.length(), k++) {
            acc = acc.add(DoubleVector.fromArray(S, a, i));
            s = o[k & 3].apply(s);
        }
        return acc.reduceLanes(VectorOperators.ADD) + s;
    }

    // A small selected method (a vector live across the implicit null check of the second
    // array) entered once per element group: one scrub at every entry.
    @CompilerControl(CompilerControl.Mode.DONT_INLINE)
    static double dot(double[] x, double[] y, int i) {
        DoubleVector vx = DoubleVector.fromArray(S, x, i);
        DoubleVector vy = DoubleVector.fromArray(S, y, i);
        return vx.mul(vy).reduceLanes(VectorOperators.ADD);
    }

    @Benchmark
    public double smallSelectedMethod() {
        double s = 0;
        for (int i = 0; i < S.loopBound(a.length); i += S.length()) {
            s += dot(a, b, i);
        }
        return s;
    }

    // Poll-dense vector loop with no call: the scrub runs only at entry (control).
    @Benchmark
    public double vectorLoopNoCall() {
        DoubleVector acc = DoubleVector.zero(S);
        for (int r = 0; r < 4; r++) {
            for (int i = 0; i < S.loopBound(a.length); i += S.length()) {
                acc = acc.add(DoubleVector.fromArray(S, a, i).mul(DoubleVector.fromArray(S, b, i)));
            }
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }
}
