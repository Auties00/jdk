package review6;

import java.util.concurrent.TimeUnit;
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;
import org.openjdk.jmh.annotations.*;

// JDK-8365196 scrub6: a non-counted loop (its safepoint poll runs every iteration) with an
// implicit null check per step and a live 256-bit accumulator, and its scalar control.
@State(Scope.Thread)
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@Warmup(iterations = 4, time = 1)
@Measurement(iterations = 6, time = 1)
@Fork(value = 2, jvmArgsAppend = {"--add-modules", "jdk.incubator.vector"})
public class R6Bench {
    static final VectorSpecies<Double> S = DoubleVector.SPECIES_256;

    static final class Node {
        Node next;
        double v;
    }

    Node head;

    @Setup
    public void setup() {
        java.util.List<Node> nodes = new java.util.ArrayList<>();
        for (int i = 0; i < 1024; i++) {
            Node n = new Node();
            n.v = i;
            nodes.add(n);
        }
        java.util.Collections.shuffle(nodes, new java.util.Random(42));
        for (int i = 0; i < nodes.size() - 1; i++) {
            nodes.get(i).next = nodes.get(i + 1);
        }
        head = nodes.get(0);
    }

    @Benchmark
    public double uncountedListWalkVector() {
        DoubleVector acc = DoubleVector.zero(S);
        for (Node p = head; p != null; p = p.next) {
            acc = acc.add(p.v);
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    @Benchmark
    public double uncountedListWalkScalar() {
        double acc = 0;
        for (Node p = head; p != null; p = p.next) {
            acc += p.v;
        }
        return acc;
    }
}
