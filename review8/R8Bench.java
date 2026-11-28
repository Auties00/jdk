package review8;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.concurrent.TimeUnit;
import org.openjdk.jmh.annotations.*;

// The cost of the MXCSR restore of AvoidLosableStateAtTraps: a JNI call, an FFM downcall (with
// and without the thread state transition) and an FFM upcall, each to or from a trivial function.
@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.NANOSECONDS)
@State(Scope.Thread)
public class R8Bench {
    static {
        System.loadLibrary("r8bench");
    }

    static native int jniNop(int x);

    static final Linker LINKER = Linker.nativeLinker();
    static final MethodHandle ABS = LINKER.downcallHandle(LINKER.defaultLookup().find("abs").orElseThrow(),
        FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));
    static final MethodHandle ABS_CRITICAL = LINKER.downcallHandle(LINKER.defaultLookup().find("abs").orElseThrow(),
        FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT), Linker.Option.critical(false));
    static final MethodHandle QSORT = LINKER.downcallHandle(LINKER.defaultLookup().find("qsort").orElseThrow(),
        FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));

    static int compare(MemorySegment x, MemorySegment y) {
        return Integer.compare(x.reinterpret(4).get(ValueLayout.JAVA_INT, 0), y.reinterpret(4).get(ValueLayout.JAVA_INT, 0));
    }

    int x = -5;
    MemorySegment pair;
    MemorySegment comparator;

    @Setup
    public void setup() throws Exception {
        pair = Arena.global().allocate(8, 4);
        comparator = LINKER.upcallStub(
            MethodHandles.lookup().findStatic(R8Bench.class, "compare",
                MethodType.methodType(int.class, MemorySegment.class, MemorySegment.class)),
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS), Arena.global());
    }

    @Benchmark
    public int jni() {
        return jniNop(x);
    }

    @Benchmark
    public int downcall() throws Throwable {
        return (int)ABS.invokeExact(x);
    }

    @Benchmark
    public int downcallCritical() throws Throwable {
        return (int)ABS_CRITICAL.invokeExact(x);
    }

    // qsort of two ints: one downcall and one upcall to the comparator.
    @Benchmark
    public int upcall() throws Throwable {
        pair.set(ValueLayout.JAVA_INT, 0, 2);
        pair.set(ValueLayout.JAVA_INT, 4, 1);
        QSORT.invokeExact(pair, 2L, 4L, comparator);
        return pair.get(ValueLayout.JAVA_INT, 0);
    }
}
