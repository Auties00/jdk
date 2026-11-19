package review8;

import review3.R3Bench;

// Runs R3Bench.vectorLoopMegamorphicCall and vectorLoopStaticCall outside JMH, so that their C2
// code can be printed with -XX:+PrintOptoAssembly (debug build) and compared across modes.
public class MegaDriver {
    static volatile double sink;

    public static void main(String[] args) {
        R3Bench b = new R3Bench();
        b.setup();
        long end = System.nanoTime() + Long.getLong("ms", 4000) * 1_000_000L;
        double s = 0;
        while (System.nanoTime() < end) {
            s += b.vectorLoopMegamorphicCall();
            s += b.vectorLoopStaticCall();
        }
        sink = s;
        System.out.println("done");
    }
}
