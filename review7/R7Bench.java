package review7;

import java.util.concurrent.TimeUnit;
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;
import org.openjdk.jmh.annotations.*;

// Kernels that keep AVX-512 (or AVX2) state live at the trap sites AvoidLosableStateAtTraps rewrites:
// implicit null checks (now an explicit test and branch), divisions (C2 tests the divisor in either
// mode, a control), and loop polls (now a test of the polling word and a branch).
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@State(Scope.Thread)
public class R7Bench {
    static final VectorSpecies<Double> S = DoubleVector.SPECIES_PREFERRED;
    static final VectorSpecies<Long> L = LongVector.SPECIES_PREFERRED;

    static final class Node {
        final long value;
        Node next;
        Node(long value) { this.value = value; }
    }

    static final class Box {
        double v;
        Box(double v) { this.v = v; }
    }

    double[] a = new double[1024];
    long[] l = new long[1024];
    Box[] boxes = new Box[1024 / 8];
    int[] divisors = new int[1024];
    Node list;

    @Setup
    public void setup() {
        for (int i = 0; i < a.length; i++) {
            a[i] = i * 0.25;
            l[i] = i * 7L + 3;
            divisors[i] = (i % 13) + 1;
        }
        for (int i = 0; i < boxes.length; i++) {
            boxes[i] = new Box(i + 0.5);
        }
        Node head = null;
        for (int i = 0; i < 1024; i++) {
            Node n = new Node(i * 3L);
            n.next = head;
            head = n;
        }
        list = head;
    }

    // A vector accumulator live across the implicit null check of each box field load.
    @Benchmark
    public double nullCheckVectorLive() {
        DoubleVector acc = DoubleVector.zero(S);
        Box[] bs = boxes;
        for (int i = 0, k = 0; i < S.loopBound(a.length); i += S.length(), k++) {
            double x = bs[k & (bs.length - 1)].v;
            acc = acc.add(DoubleVector.fromArray(S, a, i).mul(x));
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    // The same with no vector live at the null check (control).
    @Benchmark
    public double nullCheckScalar() {
        double s = 0;
        Box[] bs = boxes;
        for (int k = 0; k < bs.length; k++) {
            s += bs[k].v * a[k];
        }
        return s;
    }

    // Integer divisions with a vector accumulator live.
    @Benchmark
    public long divisionVectorLive() {
        LongVector acc = LongVector.zero(L);
        long q = 0;
        for (int i = 0; i < L.loopBound(l.length); i += L.length()) {
            acc = acc.add(LongVector.fromArray(L, l, i));
            q += (i + 1000) / divisors[i];
        }
        return acc.reduceLanes(VectorOperators.ADD) + q;
    }

    // A loop that is not counted (a list walk), so it polls on every iteration, with a vector live.
    @Benchmark
    public double listWalkVectorLive() {
        DoubleVector acc = DoubleVector.zero(S);
        DoubleVector one = DoubleVector.broadcast(S, 1.0);
        for (Node n = list; n != null; n = n.next) {
            acc = acc.add(one.mul((double)n.value));
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    // The same walk without vectors (control).
    @Benchmark
    public long listWalkScalar() {
        long s = 0;
        for (Node n = list; n != null; n = n.next) {
            s += n.value;
        }
        return s;
    }

    // A long counted vector loop: the poll of the strip-mined outer loop with the accumulators live.
    @Benchmark
    public double countedLoopVectorLive() {
        DoubleVector acc0 = DoubleVector.zero(S);
        DoubleVector acc1 = DoubleVector.zero(S);
        for (int r = 0; r < 64; r++) {
            for (int i = 0; i < S.loopBound(a.length); i += S.length()) {
                DoubleVector v = DoubleVector.fromArray(S, a, i);
                acc0 = acc0.add(v);
                acc1 = acc1.add(v.mul(v));
            }
        }
        return acc0.add(acc1).reduceLanes(VectorOperators.ADD);
    }
}
