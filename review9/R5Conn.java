// Review-only kernels for JDK-8365196 (review5).
//
// Hypothesis: when the return successor of a call (the fall-through CatchProj block) is empty,
// PhaseCFG::remove_empty_blocks() makes it a connector block. Branches and the fall-through go
// to its non-connector successor, so nothing in the connector block is ever executed. If
// insert_trap_scrubs() puts the scrub that follows the call at the start of that block, the
// scrub is dead code and the hot trap after the merge is reached without it. The calls here are
// JNI natives that fill 32 KiB of their own frame with 0x8000000000000000, which is below the
// compiled frame's stack pointer, where the stale header is read.
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorSpecies;

public class R5Conn {
    static native void fillA();
    static native void fillB();
    static native void fillC();

    static final VectorSpecies<Double> S = DoubleVector.SPECIES_256;
    static volatile Object sink;
    static volatile boolean stop;

    static class Holder {
        int x = 1;
    }

    static long check(DoubleVector a0, DoubleVector a1, DoubleVector a2, DoubleVector a3) {
        LongVector r = a0.reinterpretAsLongs().and(a1.reinterpretAsLongs()).and(a2.reinterpretAsLongs())
                         .and(a3.reinterpretAsLongs());
        for (int i = 0; i < 4; i++) {
            if (r.lane(i) != Long.MIN_VALUE) {
                return 1;
            }
        }
        return 0;
    }

    // Both arms call, so nothing is in a register at the merge and both return blocks are empty.
    static long both(double neg, boolean c, Holder[] hs) {
        if (c) {
            fillA();
        } else {
            fillB();
        }
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            try {
                count += hs[i & m].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        return check(a0, a1, a2, a3);
    }

    // One arm calls.
    static long one(double neg, boolean c, Holder[] hs) {
        if (c) {
            fillA();
        }
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            try {
                count += hs[i & m].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        return check(a0, a1, a2, a3);
    }

    // A three-way switch of calls.
    static long sw(double neg, int k, Holder[] hs) {
        switch (k % 3) {
            case 0 -> fillA();
            case 1 -> fillB();
            default -> fillC();
        }
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            try {
                count += hs[i & m].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        return check(a0, a1, a2, a3);
    }

    // Calls inside the loop, in two arms, with the vectors live across them.
    static long inLoop(double neg, Holder[] hs) {
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int count = 0, m = hs.length - 1;
        for (int i = 0; i < 4000; i++) {
            if ((i & 255) == 0) {
                if ((i & 256) == 0) {
                    fillA();
                } else {
                    fillB();
                }
            }
            try {
                count += hs[i & m].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        return check(a0, a1, a2, a3);
    }

    public static void main(String[] args) throws Exception {
        System.load(System.getProperty("r5.jni"));
        String which = args[0];
        long millis = Long.parseLong(args[1]);
        Holder[] hs = new Holder[256];
        for (int i = 0; i < hs.length; i++) hs[i] = new Holder();
        Holder[] withNull = hs.clone();
        withNull[7] = null;
        Thread gc = new Thread(() -> {
            while (!stop) {
                for (int i = 0; i < 2000; i++) sink = new byte[256];
                System.gc();
                try { Thread.sleep(2); } catch (InterruptedException e) { return; }
            }
        });
        gc.setDaemon(true);
        gc.start();
        long wrong = 0, calls = 0;
        long end = System.nanoTime() + millis * 1_000_000L;
        while (System.nanoTime() < end) {
            Holder[] h = (calls & 63) == 63 ? withNull : hs;
            long r = switch (which) {
                case "both" -> both(-0.0, (calls & 1) == 0, h);
                case "one" -> one(-0.0, (calls & 1) == 0, h);
                case "sw" -> sw(-0.0, (int) calls, h);
                case "inloop" -> inLoop(-0.0, h);
                default -> throw new IllegalArgumentException(which);
            };
            wrong += r;
            calls++;
        }
        stop = true;
        System.out.println("R5CONN " + which + " wrong=" + wrong + " calls=" + calls);
    }
}
