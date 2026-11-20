package review5;

import java.util.concurrent.TimeUnit;
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;
import org.openjdk.jmh.annotations.*;

// JDK-8365196 scrub5: the cost of the scrub before implicit null checks and loop polls where
// vector state is live. listWalk*: one implicit null check per step (p = p.next); the vector
// variants keep a 256-bit accumulator live across it, the scalar one keeps nothing losable.
@State(Scope.Thread)
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@Warmup(iterations = 4, time = 1)
@Measurement(iterations = 6, time = 1)
@Fork(value = 3, jvmArgsAppend = {"--add-modules", "jdk.incubator.vector"})
public class R5Bench {
    static final VectorSpecies<Double> S = DoubleVector.SPECIES_256;

    static final class Node {
        Node next;
        double v;
    }

    Node head;
    double[] data;

    @Setup
    public void setup() {
        Node[] nodes = new Node[1024];
        for (int i = 0; i < nodes.length; i++) {
            nodes[i] = new Node();
            nodes[i].v = i;
        }
        for (int i = 0; i < nodes.length; i++) {
            nodes[i].next = nodes[(i * 7 + 1) & (nodes.length - 1)];
        }
        head = nodes[0];
        data = new double[1 << 16];
        for (int i = 0; i < data.length; i++) {
            data[i] = i & 15;
        }
    }

    @Benchmark
    public double listWalkVector() {
        DoubleVector acc = DoubleVector.zero(S);
        Node p = head;
        for (int i = 0; i < 1024; i++) {
            acc = acc.add(p.v);
            p = p.next;
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    @Benchmark
    public double listWalkVector4() {
        DoubleVector a0 = DoubleVector.zero(S), a1 = DoubleVector.zero(S);
        DoubleVector a2 = DoubleVector.zero(S), a3 = DoubleVector.zero(S);
        Node p = head;
        for (int i = 0; i < 1024; i++) {
            double v = p.v;
            a0 = a0.add(v);
            a1 = a1.add(v * 2);
            a2 = a2.add(v * 3);
            a3 = a3.add(v * 4);
            p = p.next;
        }
        return a0.add(a1).add(a2).add(a3).reduceLanes(VectorOperators.ADD);
    }

    @Benchmark
    public double listWalkScalar() {
        double acc = 0;
        Node p = head;
        for (int i = 0; i < 1024; i++) {
            acc += p.v;
            p = p.next;
        }
        return acc;
    }

    @Benchmark
    public double vectorArrayLoop() {
        DoubleVector acc = DoubleVector.zero(S);
        double[] d = data;
        for (int i = 0; i < d.length; i += S.length()) {
            acc = acc.add(DoubleVector.fromArray(S, d, i));
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }
}
