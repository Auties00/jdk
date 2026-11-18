import jdk.incubator.vector.*;

// JDK-8365196 review 3: does a C2 GC barrier stub that calls the runtime leave a header-like
// value where Windows reads the XSAVE header of the next trap's context, after the scrub at
// method entry? Mode y: 13 x 256-bit accumulators (AVX2 hosts, L = 320); mode z: 30 x 512-bit
// accumulators (AVX-512 hosts, L = 1920). Prints the number of wrong kernel results.
public class BarrierStubScrub {
    static volatile boolean stop;
    static Object sink;

    // 13 accumulators of DoubleVector.SPECIES_256 lanes, all -0.0, live across the loop: the barrier stub's
    // SaveLiveRegisters spills them below rsp, so every spilled quadword is 0x8000000000000000.
    static long kernelY(double[] init, double neg, Object[] a, int iters) {
        DoubleVector v = DoubleVector.broadcast(DoubleVector.SPECIES_256, neg);
        DoubleVector a0 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[0]);
        DoubleVector a1 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[1]);
        DoubleVector a2 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[2]);
        DoubleVector a3 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[3]);
        DoubleVector a4 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[4]);
        DoubleVector a5 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[5]);
        DoubleVector a6 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[6]);
        DoubleVector a7 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[7]);
        DoubleVector a8 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[8]);
        DoubleVector a9 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[9]);
        DoubleVector a10 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[10]);
        DoubleVector a11 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[11]);
        DoubleVector a12 = DoubleVector.broadcast(DoubleVector.SPECIES_256, init[12]);
        int mask = a.length - 1;
        int nonNull = 0;
        for (int i = 0; i < iters; i++) {
            if (a[i & mask] != null) nonNull++;   // GC load barrier; slow path after each phase change
            a0 = a0.add(v);
            a1 = a1.add(v);
            a2 = a2.add(v);
            a3 = a3.add(v);
            a4 = a4.add(v);
            a5 = a5.add(v);
            a6 = a6.add(v);
            a7 = a7.add(v);
            a8 = a8.add(v);
            a9 = a9.add(v);
            a10 = a10.add(v);
            a11 = a11.add(v);
            a12 = a12.add(v);
        }
        LongVector r = a0.reinterpretAsLongs();
        r = r.and(a1.reinterpretAsLongs());
        r = r.and(a2.reinterpretAsLongs());
        r = r.and(a3.reinterpretAsLongs());
        r = r.and(a4.reinterpretAsLongs());
        r = r.and(a5.reinterpretAsLongs());
        r = r.and(a6.reinterpretAsLongs());
        r = r.and(a7.reinterpretAsLongs());
        r = r.and(a8.reinterpretAsLongs());
        r = r.and(a9.reinterpretAsLongs());
        r = r.and(a10.reinterpretAsLongs());
        r = r.and(a11.reinterpretAsLongs());
        r = r.and(a12.reinterpretAsLongs());
        // -0.0 + -0.0 = -0.0; an upper lane zeroed by an incomplete exception context becomes +0.0 and stays.
        long bad = 0;
        for (int l = 0; l < DoubleVector.SPECIES_256.length(); l++) if (r.lane(l) != Long.MIN_VALUE) bad++;
        return (bad << 32) | (nonNull & 0xffffffffL);
    }

    // 30 accumulators of DoubleVector.SPECIES_512 lanes, all -0.0, live across the loop: the barrier stub's
    // SaveLiveRegisters spills them below rsp, so every spilled quadword is 0x8000000000000000.
    static long kernelZ(double[] init, double neg, Object[] a, int iters) {
        DoubleVector v = DoubleVector.broadcast(DoubleVector.SPECIES_512, neg);
        DoubleVector a0 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[0]);
        DoubleVector a1 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[1]);
        DoubleVector a2 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[2]);
        DoubleVector a3 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[3]);
        DoubleVector a4 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[4]);
        DoubleVector a5 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[5]);
        DoubleVector a6 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[6]);
        DoubleVector a7 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[7]);
        DoubleVector a8 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[8]);
        DoubleVector a9 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[9]);
        DoubleVector a10 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[10]);
        DoubleVector a11 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[11]);
        DoubleVector a12 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[12]);
        DoubleVector a13 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[13]);
        DoubleVector a14 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[14]);
        DoubleVector a15 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[15]);
        DoubleVector a16 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[16]);
        DoubleVector a17 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[17]);
        DoubleVector a18 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[18]);
        DoubleVector a19 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[19]);
        DoubleVector a20 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[20]);
        DoubleVector a21 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[21]);
        DoubleVector a22 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[22]);
        DoubleVector a23 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[23]);
        DoubleVector a24 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[24]);
        DoubleVector a25 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[25]);
        DoubleVector a26 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[26]);
        DoubleVector a27 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[27]);
        DoubleVector a28 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[28]);
        DoubleVector a29 = DoubleVector.broadcast(DoubleVector.SPECIES_512, init[29]);
        int mask = a.length - 1;
        int nonNull = 0;
        for (int i = 0; i < iters; i++) {
            if (a[i & mask] != null) nonNull++;   // GC load barrier; slow path after each phase change
            a0 = a0.add(v);
            a1 = a1.add(v);
            a2 = a2.add(v);
            a3 = a3.add(v);
            a4 = a4.add(v);
            a5 = a5.add(v);
            a6 = a6.add(v);
            a7 = a7.add(v);
            a8 = a8.add(v);
            a9 = a9.add(v);
            a10 = a10.add(v);
            a11 = a11.add(v);
            a12 = a12.add(v);
            a13 = a13.add(v);
            a14 = a14.add(v);
            a15 = a15.add(v);
            a16 = a16.add(v);
            a17 = a17.add(v);
            a18 = a18.add(v);
            a19 = a19.add(v);
            a20 = a20.add(v);
            a21 = a21.add(v);
            a22 = a22.add(v);
            a23 = a23.add(v);
            a24 = a24.add(v);
            a25 = a25.add(v);
            a26 = a26.add(v);
            a27 = a27.add(v);
            a28 = a28.add(v);
            a29 = a29.add(v);
        }
        LongVector r = a0.reinterpretAsLongs();
        r = r.and(a1.reinterpretAsLongs());
        r = r.and(a2.reinterpretAsLongs());
        r = r.and(a3.reinterpretAsLongs());
        r = r.and(a4.reinterpretAsLongs());
        r = r.and(a5.reinterpretAsLongs());
        r = r.and(a6.reinterpretAsLongs());
        r = r.and(a7.reinterpretAsLongs());
        r = r.and(a8.reinterpretAsLongs());
        r = r.and(a9.reinterpretAsLongs());
        r = r.and(a10.reinterpretAsLongs());
        r = r.and(a11.reinterpretAsLongs());
        r = r.and(a12.reinterpretAsLongs());
        r = r.and(a13.reinterpretAsLongs());
        r = r.and(a14.reinterpretAsLongs());
        r = r.and(a15.reinterpretAsLongs());
        r = r.and(a16.reinterpretAsLongs());
        r = r.and(a17.reinterpretAsLongs());
        r = r.and(a18.reinterpretAsLongs());
        r = r.and(a19.reinterpretAsLongs());
        r = r.and(a20.reinterpretAsLongs());
        r = r.and(a21.reinterpretAsLongs());
        r = r.and(a22.reinterpretAsLongs());
        r = r.and(a23.reinterpretAsLongs());
        r = r.and(a24.reinterpretAsLongs());
        r = r.and(a25.reinterpretAsLongs());
        r = r.and(a26.reinterpretAsLongs());
        r = r.and(a27.reinterpretAsLongs());
        r = r.and(a28.reinterpretAsLongs());
        r = r.and(a29.reinterpretAsLongs());
        // -0.0 + -0.0 = -0.0; an upper lane zeroed by an incomplete exception context becomes +0.0 and stays.
        long bad = 0;
        for (int l = 0; l < DoubleVector.SPECIES_512.length(); l++) if (r.lane(l) != Long.MIN_VALUE) bad++;
        return (bad << 32) | (nonNull & 0xffffffffL);
    }

    public static void main(String[] args) throws Exception {
        boolean z = args[0].equals("z");
        long seconds = Long.parseLong(args[1]);
        Object[] refs = new Object[1 << 16];
        for (int i = 0; i < refs.length; i++) refs[i] = new int[1 + (i & 7)];
        double[] init = new double[32];
        java.util.Arrays.fill(init, -0.0);
        Thread gc = new Thread(() -> {
            while (!stop) {
                for (int i = 0; i < 2000; i++) sink = new byte[256];
                System.gc();
                try { Thread.sleep(2); } catch (InterruptedException e) { return; }
            }
        });
        gc.setDaemon(true);
        gc.start();
        long end = System.nanoTime() + seconds * 1_000_000_000L;
        long calls = 0, wrong = 0;
        while (System.nanoTime() < end) {
            long r = z ? kernelZ(init, -0.0, refs, 200_000) : kernelY(init, -0.0, refs, 200_000);
            calls++;
            if ((r >>> 32) != 0) {
                if (wrong++ < 5) System.out.println("wrong result: " + (r >>> 32) + " lanes lost");
            }
        }
        stop = true;
        System.out.println("RESULT mode=" + args[0] + " calls=" + calls + " wrong=" + wrong);
        System.exit(wrong == 0 ? 0 : 1);
    }
}
