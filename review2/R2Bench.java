package review2;

import java.util.HashMap;
import java.util.Random;
import java.util.concurrent.TimeUnit;

import org.openjdk.jmh.annotations.*;

/**
 * JDK-8365196 review 2: JMH micro benchmarks for branch loop polls and explicit null checks.
 * Run with the flags under test passed through -jvmArgsAppend.
 */
@State(Scope.Thread)
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@Warmup(iterations = 5, time = 1)
@Measurement(iterations = 8, time = 1)
@Fork(3)
public class R2Bench {
    static final int N = 1024;

    static final class Node {
        int v;
        Node next;
        Node(int v) { this.v = v; }
    }

    static abstract class Shape { abstract int area(); }
    static final class Sq extends Shape { final int a; Sq(int a) { this.a = a; } int area() { return a * a; } }
    static final class Re extends Shape { final int a, b; Re(int a, int b) { this.a = a; this.b = b; } int area() { return a * b; } }
    static final class Tr extends Shape { final int a, b; Tr(int a, int b) { this.a = a; this.b = b; } int area() { return (a * b) >> 1; } }
    static final class Ci extends Shape { final int r; Ci(int r) { this.r = r; } int area() { return 3 * r * r; } }

    Node[] nodes;
    Node head;
    Shape[] shapes;
    int[] ints;
    int[] lens;
    float[] fa, fb;
    HashMap<Integer, Integer> map;
    Integer[] keys;

    @Setup
    public void setup() {
        Random r = new Random(42);
        nodes = new Node[N];
        for (int i = 0; i < N; i++) nodes[i] = new Node(i);
        for (int i = 0; i < N; i++) nodes[i].next = nodes[r.nextInt(N)];
        head = null;
        for (int i = 0; i < N; i++) { Node n = new Node(i); n.next = head; head = n; }
        shapes = new Shape[N];
        for (int i = 0; i < N; i++) {
            switch (i & 3) {
                case 0 -> shapes[i] = new Sq(i);
                case 1 -> shapes[i] = new Re(i, i + 1);
                case 2 -> shapes[i] = new Tr(i, i + 2);
                default -> shapes[i] = new Ci(i);
            }
        }
        ints = new int[N];
        lens = new int[N];
        for (int i = 0; i < N; i++) { ints[i] = r.nextInt(); lens[i] = 1 + r.nextInt(7); }
        fa = new float[N];
        fb = new float[N];
        for (int i = 0; i < N; i++) { fa[i] = i; fb[i] = i * 0.5f; }
        map = new HashMap<>();
        keys = new Integer[N];
        for (int i = 0; i < N; i++) { keys[i] = r.nextInt(4 * N); map.put(keys[i], i); }
    }

    // three dependent null-checked loads per element, L1/L2 resident
    @Benchmark
    public int fieldChain() {
        int s = 0;
        Node[] a = nodes;
        for (int i = 0; i < a.length; i++) s += a[i].next.next.v + a[i].v;
        return s;
    }

    // one null-checked load per element
    @Benchmark
    public int fieldLoads() {
        int s = 0;
        Node[] a = nodes;
        for (int i = 0; i < a.length; i++) s += a[i].v;
        return s;
    }

    // megamorphic virtual call: explicit receiver check without ImplicitNullChecks
    @Benchmark
    public int megamorphic() {
        int s = 0;
        Shape[] a = shapes;
        for (int i = 0; i < a.length; i++) s += a[i].area();
        return s;
    }

    // HashMap lookups: null checks, virtual equals/hashCode
    @Benchmark
    public int hashMapGet() {
        int s = 0;
        Integer[] k = keys;
        for (int i = 0; i < k.length; i++) s += map.get(k[i]);
        return s;
    }

    // short inner loops: one outer-strip poll per inner loop entry
    @Benchmark
    public long shortInner() {
        long s = 0;
        int[] a = ints;
        int[] l = lens;
        for (int i = 0; i < N; i++) {
            int m = l[i];
            for (int j = 0; j < m; j++) s += a[(i + j) & (N - 1)];
        }
        return s;
    }

    // non-counted loop: a poll per iteration
    @Benchmark
    public int listWalk() {
        int s = 0;
        for (Node n = head; n != null; n = n.next) s += n.v;
        return s;
    }

    // strip-mined counted loop
    @Benchmark
    public long countedSum() {
        long s = 0;
        int[] a = ints;
        for (int r = 0; r < 16; r++) {
            for (int i = 0; i < a.length; i++) s += a[i] ^ r;
        }
        return s;
    }

    // vectorized float loop
    @Benchmark
    public float saxpy() {
        float[] a = fa, b = fb;
        for (int r = 0; r < 16; r++) {
            for (int i = 0; i < a.length; i++) a[i] = a[i] * 0.999f + b[i];
        }
        return a[17];
    }
}
