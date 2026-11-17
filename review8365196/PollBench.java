import java.util.Arrays;

/**
 * JDK-8365196 review: micro benchmarks for the cost of branch-based loop safepoint polls.
 * Usage: java PollBench <benchmark|all> <warmup seconds> <measurement seconds> <iterations>
 * Prints, per benchmark, the median and min/max throughput (operations per microsecond) of the iterations.
 */
public final class PollBench {
    static final int N = 4096;
    static final int[] ints = new int[N];
    static final float[] fa = new float[N], fb = new float[N];
    static Node list;
    static final Box[] boxes = new Box[N];
    static volatile long sink;

    static final class Node { final int v; final Node next; Node(int v, Node next) { this.v = v; this.next = next; } }
    static final class Box { final int v; Box(int v) { this.v = v; } }

    static {
        for (int i = 0; i < N; i++) {
            ints[i] = i * 31;
            fa[i] = i;
            fb[i] = i * 0.5f;
            boxes[i] = new Box(i);
            list = new Node(i, list);
        }
    }

    // counted int loop: strip mined, one poll per outer iteration
    static long countedSum() {
        long s = 0;
        for (int r = 0; r < 64; r++) {
            for (int i = 0; i < ints.length; i++) s += ints[i] ^ r;
        }
        return s;
    }

    // long induction variable
    static long longLoop() {
        long s = 0;
        for (long i = 0; i < 262144L; i++) s += (i * 7) ^ (s >>> 3);
        return s;
    }

    // non-counted loop: a poll in every iteration
    static long listWalk() {
        long s = 0;
        for (int r = 0; r < 64; r++) {
            for (Node n = list; n != null; n = n.next) s += n.v;
        }
        return s;
    }

    // vectorized float loop
    static long saxpy() {
        for (int r = 0; r < 64; r++) {
            for (int i = 0; i < fa.length; i++) fa[i] = fa[i] * 0.999f + fb[i];
        }
        return (long) fa[17];
    }

    // field loads through an array of objects (null checks)
    static long fieldLoads() {
        long s = 0;
        for (int r = 0; r < 64; r++) {
            for (int i = 0; i < boxes.length; i++) s += boxes[i].v;
        }
        return s;
    }

    interface Benchmark { long run(); }

    static double measure(Benchmark b, long nanos) {
        long ops = 0;
        long start = System.nanoTime();
        long end = start + nanos;
        long now;
        do {
            sink += b.run();
            ops++;
            now = System.nanoTime();
        } while (now < end);
        return ops / ((now - start) / 1000.0);
    }

    public static void main(String[] args) {
        String which = args[0];
        long warm = Long.parseLong(args[1]) * 1_000_000_000L;
        long meas = Long.parseLong(args[2]) * 1_000_000_000L;
        int iters = Integer.parseInt(args[3]);
        String[] names = {"countedSum", "longLoop", "listWalk", "saxpy", "fieldLoads"};
        Benchmark[] benches = {PollBench::countedSum, PollBench::longLoop, PollBench::listWalk, PollBench::saxpy, PollBench::fieldLoads};
        for (int k = 0; k < names.length; k++) {
            if (!which.equals("all") && !which.equals(names[k])) continue;
            measure(benches[k], warm);
            double[] r = new double[iters];
            for (int i = 0; i < iters; i++) r[i] = measure(benches[k], meas);
            Arrays.sort(r);
            System.out.printf("BENCH %s median=%.4f min=%.4f max=%.4f ops/us%n", names[k], r[iters / 2], r[0], r[iters - 1]);
        }
    }
}
