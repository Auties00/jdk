package review9;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.reflect.Field;
import java.nio.ByteOrder;
import java.util.concurrent.TimeUnit;
import jdk.incubator.vector.IntVector;
import jdk.incubator.vector.VectorMask;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;
import org.openjdk.jmh.annotations.*;
import sun.misc.Unsafe;

// The cost of killing k1-k7 at raw memory accesses (AvoidLosableStateAtTraps on AVX-512): kernels that keep an
// opmask live across off-heap MemorySegment or Unsafe accesses, where the mode makes the register allocator
// rebuild or spill the mask, and heap-only controls that have no raw access.
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@State(Scope.Thread)
public class R9Bench {
    static final VectorSpecies<Integer> S = IntVector.SPECIES_PREFERRED;
    static final ByteOrder LE = ByteOrder.LITTLE_ENDIAN;
    static final Unsafe U;
    static {
        try {
            Field f = Unsafe.class.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            U = (Unsafe) f.get(null);
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    // not a multiple of any vector length, so every masked loop has a partial tail
    final int n = 1003;
    Arena arena;
    MemorySegment src, dst;
    long raw;
    int[] heap = new int[n], heapDst = new int[n];

    @Setup
    public void setup() {
        arena = Arena.ofConfined();
        src = arena.allocate(4L * n, 64);
        dst = arena.allocate(4L * n, 64);
        raw = arena.allocate(4L * n, 64).address();
        for (int i = 0; i < n; i++) {
            int v = (i * 37) % 200;
            src.setAtIndex(java.lang.foreign.ValueLayout.JAVA_INT, i, v);
            U.putInt(raw + 4L * i, v);
            heap[i] = v;
        }
    }

    @TearDown
    public void tearDown() {
        arena.close();
    }

    // indexInRange mask feeding a masked off-heap load each iteration
    @Benchmark
    public int segmentMaskedSum() {
        IntVector acc = IntVector.zero(S);
        for (int i = 0; i < n; i += S.length()) {
            VectorMask<Integer> m = S.indexInRange(i, n);
            acc = acc.add(IntVector.fromMemorySegment(S, src, 4L * i, LE, m), m);
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    // a compare mask live between an off-heap load and an off-heap store
    @Benchmark
    public void segmentCompareBlendStore() {
        int ub = S.loopBound(n);
        IntVector limit = IntVector.broadcast(S, 100);
        int i = 0;
        for (; i < ub; i += S.length()) {
            IntVector v = IntVector.fromMemorySegment(S, src, 4L * i, LE);
            VectorMask<Integer> m = v.compare(VectorOperators.LT, limit);
            v.blend(0, m).add(1, m).intoMemorySegment(dst, 4L * i, LE, m);
        }
        for (; i < n; i++) {
            int v = src.getAtIndex(java.lang.foreign.ValueLayout.JAVA_INT, i);
            if (v < 100) dst.setAtIndex(java.lang.foreign.ValueLayout.JAVA_INT, i, 1);
        }
    }

    // a mask kept across a run of scalar Unsafe reads in the same loop
    @Benchmark
    public int unsafeWithLiveMask() {
        IntVector acc = IntVector.zero(S);
        IntVector limit = IntVector.broadcast(S, 100);
        int s = 0;
        for (int i = 0; i < n; i += S.length()) {
            VectorMask<Integer> m = S.indexInRange(i, n);
            IntVector v = IntVector.fromArray(S, heap, i, m);
            VectorMask<Integer> lt = v.compare(VectorOperators.LT, limit, m);
            s += U.getInt(raw + 4L * i);
            acc = acc.add(v, lt);
        }
        return s + acc.reduceLanes(VectorOperators.ADD);
    }

    // heap-only controls: the same loops without a raw access, so the mode kills nothing
    @Benchmark
    public int heapMaskedSum() {
        IntVector acc = IntVector.zero(S);
        for (int i = 0; i < n; i += S.length()) {
            VectorMask<Integer> m = S.indexInRange(i, n);
            acc = acc.add(IntVector.fromArray(S, heap, i, m), m);
        }
        return acc.reduceLanes(VectorOperators.ADD);
    }

    @Benchmark
    public void heapCompareBlendStore() {
        int ub = S.loopBound(n);
        IntVector limit = IntVector.broadcast(S, 100);
        int i = 0;
        for (; i < ub; i += S.length()) {
            IntVector v = IntVector.fromArray(S, heap, i);
            VectorMask<Integer> m = v.compare(VectorOperators.LT, limit);
            v.blend(0, m).add(1, m).intoArray(heapDst, i, m);
        }
        for (; i < n; i++) {
            if (heap[i] < 100) heapDst[i] = 1;
        }
    }
}
