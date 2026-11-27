package review9;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteOrder;
import java.util.concurrent.TimeUnit;
import jdk.incubator.vector.IntVector;
import jdk.incubator.vector.VectorMask;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;
import org.openjdk.jmh.annotations.*;

// More kernels for the kill of k1-k7 at raw accesses: Vector API loops over off-heap segments in methods that
// also read a segment with a scalar accessor, which is what makes a compilation one "with unsafe accesses"
// (the Vector API's own segment accesses do not), so that its raw accesses kill the mask registers.
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@State(Scope.Thread)
public class R10Bench {
    static final VectorSpecies<Integer> S = IntVector.SPECIES_PREFERRED;
    static final ByteOrder LE = ByteOrder.LITTLE_ENDIAN;
    static final ValueLayout.OfInt INT = ValueLayout.JAVA_INT;

    final int n = 1003;
    Arena arena;
    MemorySegment src, src2, dst;

    @Setup
    public void setup() {
        arena = Arena.ofConfined();
        src = arena.allocate(4L * n, 64);
        src2 = arena.allocate(4L * n, 64);
        dst = arena.allocate(4L * n, 64);
        for (int i = 0; i < n; i++) {
            src.setAtIndex(INT, i, (i * 37) % 200);
            src2.setAtIndex(INT, i, (i * 53) % 200);
        }
    }

    @TearDown
    public void tearDown() {
        arena.close();
    }

    // The loop with a masked tail: one mask per iteration governs a load, an add and a store, and the
    // Vector API checks it in the slow path of each access. A scalar read makes the compilation unsafe.
    @Benchmark
    public int tailMaskedCopy() {
        int bias = src2.getAtIndex(INT, 0);
        for (int i = 0; i < n; i += S.length()) {
            VectorMask<Integer> m = S.indexInRange(i, n);
            IntVector v = IntVector.fromMemorySegment(S, src, 4L * i, LE, m);
            v.add(bias, m).intoMemorySegment(dst, 4L * i, LE, m);
        }
        return bias;
    }

    // Full vectors and a scalar tail: a compare mask is live across the second load and then only blends.
    @Benchmark
    public void compareAcrossLoad() {
        int ub = S.loopBound(n);
        IntVector limit = IntVector.broadcast(S, 100);
        int i = 0;
        for (; i < ub; i += S.length()) {
            IntVector v = IntVector.fromMemorySegment(S, src, 4L * i, LE);
            VectorMask<Integer> m = v.compare(VectorOperators.LT, limit);
            IntVector w = IntVector.fromMemorySegment(S, src2, 4L * i, LE);
            w.blend(v, m).intoMemorySegment(dst, 4L * i, LE);
        }
        for (; i < n; i++) {
            int v = src.getAtIndex(INT, i);
            dst.setAtIndex(INT, i, v < 100 ? v : src2.getAtIndex(INT, i));
        }
    }

    // The same with a masked store: the mask is checked in the slow path of the store.
    @Benchmark
    public void compareAcrossLoadMaskedStore() {
        int ub = S.loopBound(n);
        IntVector limit = IntVector.broadcast(S, 100);
        int i = 0;
        for (; i < ub; i += S.length()) {
            IntVector v = IntVector.fromMemorySegment(S, src, 4L * i, LE);
            VectorMask<Integer> m = v.compare(VectorOperators.LT, limit);
            IntVector w = IntVector.fromMemorySegment(S, src2, 4L * i, LE);
            w.add(v).intoMemorySegment(dst, 4L * i, LE, m);
        }
        for (; i < n; i++) {
            int v = src.getAtIndex(INT, i);
            if (v < 100) {
                dst.setAtIndex(INT, i, v + src2.getAtIndex(INT, i));
            }
        }
    }

    // A constant mask kept across a loop of raw loads and used as a predicate only.
    @Benchmark
    public int constantMaskPredicate() {
        VectorMask<Integer> odd = VectorMask.fromLong(S, 0xAAAAAAAAAAAAAAAAL);
        IntVector acc = IntVector.zero(S);
        int ub = S.loopBound(n);
        int s = src2.getAtIndex(INT, 1);
        for (int i = 0; i < ub; i += S.length()) {
            acc = acc.add(IntVector.fromMemorySegment(S, src, 4L * i, LE), odd);
        }
        return s + acc.reduceLanes(VectorOperators.ADD);
    }
}
