package review9;

import java.util.concurrent.TimeUnit;
import jdk.incubator.vector.*;
import org.openjdk.jmh.annotations.*;

// JDK-8365196 scrub8: the cost of the scrub after a call whose return block is empty (a call in
// an arm of an if or a switch, merging into a join), which scrub8 places at the join with four
// stores (the join has other predecessors) and scrub7 placed in a block that never ran. After the
// join, a vector accumulator is live across a short loop with a poll and a null check.
@State(Scope.Thread)
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@Warmup(iterations = 4, time = 1)
@Measurement(iterations = 6, time = 1)
@Fork(value = 3, jvmArgsAppend = {"--add-modules", "jdk.incubator.vector"})
public class R9Join {
    static final VectorSpecies<Double> S = DoubleVector.SPECIES_PREFERRED;
    static int counter;

    double[] a = new double[256];
    int k;

    @CompilerControl(CompilerControl.Mode.DONT_INLINE)
    static void tinyA() { counter++; }

    @CompilerControl(CompilerControl.Mode.DONT_INLINE)
    static void tinyB() { counter += 2; }

    @CompilerControl(CompilerControl.Mode.DONT_INLINE)
    static void tinyC() { counter += 3; }

    @Setup
    public void setup() {
        for (int i = 0; i < a.length; i++) { a[i] = i * 0.25; }
    }

    private double sum() {
        DoubleVector acc = DoubleVector.zero(S);
        double[] x = a;
        for (int i = 0; i < S.loopBound(x.length); i += S.length()) {
            acc = acc.add(DoubleVector.fromArray(S, x, i));
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    @Benchmark
    public double joinBoth() {
        if ((++k & 1) == 0) {
            tinyA();
        } else {
            tinyB();
        }
        return sum();
    }

    @Benchmark
    public double joinOne() {
        if ((++k & 1) == 0) {
            tinyA();
        }
        return sum();
    }

    @Benchmark
    public double joinSwitch() {
        switch (++k % 3) {
            case 0 -> tinyA();
            case 1 -> tinyB();
            default -> tinyC();
        }
        return sum();
    }

    // No call: the control.
    @Benchmark
    public double noCall() {
        ++k;
        return sum();
    }
}
