// Review-only kernels for JDK-8365196 (review6). Each kernel keeps four 256-bit vectors of -0.0
// live across hot trap sites (implicit null checks, loop polls) after a writer that leaves
// 0x8000000000000000 below the compiled frame's stack pointer (a JNI native filling its frame).
// check() returns 1 when an upper lane was lost.
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorSpecies;

public class R6 {
    static native void fillA();
    static native void fillB();
    static native void fillThrow(boolean t);

    static final VectorSpecies<Double> S = DoubleVector.SPECIES_256;
    static volatile Object sink;
    static volatile boolean stop;

    static class Holder { int x = 1; }

    static long check(DoubleVector a0, DoubleVector a1, DoubleVector a2, DoubleVector a3) {
        LongVector r = a0.reinterpretAsLongs().and(a1.reinterpretAsLongs()).and(a2.reinterpretAsLongs())
                         .and(a3.reinterpretAsLongs());
        for (int i = 0; i < 4; i++) if (r.lane(i) != Long.MIN_VALUE) return 1;
        return 0;
    }

    // A call whose exception handler is empty: the handler block may hold only CreateEx.
    static long ecatch(double neg, boolean t, Holder[] hs) {
        try {
            fillThrow(t);
        } catch (IllegalStateException e) {
        }
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            try { count += hs[i & m].x; } catch (NullPointerException e) { count--; }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        return check(a0, a1, a2, a3);
    }

    // The same with work in the handler (control).
    static int hc;
    static long hcatch(double neg, boolean t, Holder[] hs) {
        try {
            fillThrow(t);
        } catch (IllegalStateException e) {
            hc++;
        }
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            try { count += hs[i & m].x; } catch (NullPointerException e) { count--; }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        return check(a0, a1, a2, a3);
    }

    // Reference stores in the loop: GC barrier slow paths (SaveLiveRegisters spills the live
    // -0.0 vectors below rsp, calls the runtime, and must clear the header before returning).
    static long barrier(double neg, Holder[] hs, Object[] dst) {
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            try { count += hs[i & m].x; } catch (NullPointerException e) { count--; }
            dst[i & m] = hs[(i * 7) & m];
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        return check(a0, a1, a2, a3);
    }

    // A long-counted loop with a call every 1024 iterations, entered by OSR on its first runs.
    static long osr(double neg, Holder[] hs, long n) {
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (long i = 0; i < n; i++) {
            if ((i & 1023) == 0) fillA();
            try { count += hs[(int) i & m].x; } catch (NullPointerException e) { count--; }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        return check(a0, a1, a2, a3);
    }

    // Performance shape: a vector live across an implicit null check in each iteration, and a
    // rare call at the bottom of the loop with nothing live across it, so the call's return
    // block is empty and its successor is the loop's back branch or head.
    static long rcl(double[] a, double[] b, Holder[] hs, int n) {
        long count = 0;
        int m = hs.length - 1;
        for (int i = 0; i + 4 <= n; i += 4) {
            DoubleVector x = DoubleVector.fromArray(S, a, i);
            int h = hs[i & m].x;
            x.add((double) h).intoArray(b, i);
            count += h;
            if ((i & 0xfffff) == 0x7fffc) fillB();
        }
        return count;
    }

    // The same without the call (reference for the cost of rcl's scrub).
    static long rcl0(double[] a, double[] b, Holder[] hs, int n) {
        long count = 0;
        int m = hs.length - 1;
        for (int i = 0; i + 4 <= n; i += 4) {
            DoubleVector x = DoubleVector.fromArray(S, a, i);
            int h = hs[i & m].x;
            x.add((double) h).intoArray(b, i);
            count += h;
        }
        return count;
    }

    public static void main(String[] args) throws Exception {
        System.load(System.getProperty("r6.jni"));
        String which = args[0];
        long millis = Long.parseLong(args[1]);
        Holder[] hs = new Holder[256];
        for (int i = 0; i < hs.length; i++) hs[i] = new Holder();
        Holder[] withNull = hs.clone();
        withNull[7] = null;
        if (which.startsWith("rcl")) {
            perf(which, millis, hs);
            return;
        }
        Thread gc = new Thread(() -> {
            while (!stop) {
                for (int i = 0; i < 2000; i++) sink = new byte[256];
                System.gc();
                try { Thread.sleep(2); } catch (InterruptedException e) { return; }
            }
        });
        gc.setDaemon(true);
        gc.start();
        Object[] dst = new Object[256];
        long wrong = 0, calls = 0;
        long end = System.nanoTime() + millis * 1_000_000L;
        while (System.nanoTime() < end) {
            Holder[] h = (calls & 63) == 63 ? withNull : hs;
            long r = switch (which) {
                case "ecatch" -> ecatch(-0.0, (calls & 3) == 0, h);
                case "hcatch" -> hcatch(-0.0, (calls & 3) == 0, h);
                case "barrier" -> barrier(-0.0, h, dst);
                case "osr" -> osr(-0.0, h, calls < 20 ? 3_000_000L : 4000L);
                default -> throw new IllegalArgumentException(which);
            };
            wrong += r;
            calls++;
        }
        stop = true;
        System.out.println("R6 " + which + " wrong=" + wrong + " calls=" + calls);
    }

    static void perf(String which, long millis, Holder[] hs) {
        int n = 4096;
        double[] a = new double[n], b = new double[n];
        long s = 0;
        boolean withCall = which.equals("rcl");
        for (int round = 0; round < 3; round++) {
            long calls = 0, t0 = System.nanoTime(), end = t0 + millis * 1_000_000L / 3;
            while (System.nanoTime() < end) {
                for (int k = 0; k < 100; k++) {
                    s += withCall ? rcl(a, b, hs, n) : rcl0(a, b, hs, n);
                }
                calls += 100;
            }
            long t1 = System.nanoTime();
            if (round > 0) System.out.printf("R6 %s round=%d ns/call=%.1f%n", which, round, (t1 - t0) / (double) calls);
        }
        sink = s;
    }
}
