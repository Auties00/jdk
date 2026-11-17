import jdk.incubator.vector.FloatVector;
import jdk.incubator.vector.VectorSpecies;

/**
 * JDK-8365196 review 2: steers the Server 2025 stale-XSAVE-header defect (section 11) into a chosen trap.
 * fill() runs interpreted and leaves Long.MIN_VALUE (0x8000000000000000) in its frames below the caller's
 * stack pointer; the kernel then takes such a quadword pair for the XSAVE header of the next exception's
 * context. The kernels call fill() and then trap with live vector state:
 *   c2poll: C2 loop poll with four 256-bit accumulators (needs trapping polls: -UseBranchLoopSafepointPolls)
 *   npe:    implicit null check caught in the same method with 24 double accumulators (C1 with
 *           TieredStopAtLevel=1, or C2 with +ImplicitNullChecks)
 * The first call runs in the interpreter and gives the expected result.
 * Usage: java --add-modules jdk.incubator.vector -XX:CompileCommand=exclude,StaleProbe::fill -XX:CompileCommand=exclude,StaleProbe::sink48 StaleProbe <mode> <seconds>
 */
public final class StaleProbe {
    static final VectorSpecies<Float> S = FloatVector.SPECIES_256;
    static final long M = Long.MIN_VALUE;
    static long sink;

    static final class H {
        final double v;
        H(double v) { this.v = v; }
    }

    // Interpreted (excluded from compilation). The interpreter pushes each long argument as the pair
    // (value, 0) on the caller's expression stack, and the arguments become the callee's locals, so the frames
    // below the caller's rsp hold runs of 0x8000000000000000 next to 0 in both phases.
    static long fill(int d) {
        return d > 0 ? sink48(d - 1, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M) : 0;
    }

    static long sink48(int d, long p0, long p1, long p2, long p3, long p4, long p5, long p6, long p7, long p8, long p9, long p10, long p11, long p12, long p13, long p14, long p15, long p16, long p17, long p18, long p19, long p20, long p21, long p22, long p23, long p24, long p25, long p26, long p27, long p28, long p29, long p30, long p31, long p32, long p33, long p34, long p35, long p36, long p37, long p38, long p39, long p40, long p41, long p42, long p43, long p44, long p45, long p46, long p47) {
        long r = fill(d);
        return r ^ p0 ^ p1 ^ p2 ^ p3 ^ p4 ^ p5 ^ p6 ^ p7 ^ p8 ^ p9 ^ p10 ^ p11 ^ p12 ^ p13 ^ p14 ^ p15 ^ p16 ^ p17 ^ p18 ^ p19 ^ p20 ^ p21 ^ p22 ^ p23 ^ p24 ^ p25 ^ p26 ^ p27 ^ p28 ^ p29 ^ p30 ^ p31 ^ p32 ^ p33 ^ p34 ^ p35 ^ p36 ^ p37 ^ p38 ^ p39 ^ p40 ^ p41 ^ p42 ^ p43 ^ p44 ^ p45 ^ p46 ^ p47;
    }

    static float c2poll(float[] a, int n) {
        FloatVector v0 = FloatVector.broadcast(S, 1.0f);
        FloatVector v1 = FloatVector.broadcast(S, 2.0f);
        FloatVector v2 = FloatVector.broadcast(S, 3.0f);
        FloatVector v3 = FloatVector.broadcast(S, 4.0f);
        for (int i = 0; i < n; i++) {
            if ((i & 7) == 0) {
                sink += fill(12);
            }
            FloatVector m = FloatVector.fromArray(S, a, (i & 3) * S.length());
            v0 = v0.mul(0.5f).add(m);
            v1 = v1.mul(0.25f).add(m);
            v2 = v2.mul(0.75f).add(m);
            v3 = v3.mul(0.125f).add(m);
        }
        float[] o = new float[4 * S.length()];
        v0.intoArray(o, 0);
        v1.intoArray(o, S.length());
        v2.intoArray(o, 2 * S.length());
        v3.intoArray(o, 3 * S.length());
        float r = 0;
        for (int k = 0; k < o.length; k++) {
            r = r * 1.0009765625f + o[k];
        }
        return r;
    }

    static double npe(H[] hs) {
        double s0 = 0, s1 = 1, s2 = 2, s3 = 3, s4 = 4, s5 = 5, s6 = 6, s7 = 7, s8 = 8, s9 = 9, s10 = 10, s11 = 11;
        double s12 = 12, s13 = 13, s14 = 14, s15 = 15, s16 = 16, s17 = 17, s18 = 18, s19 = 19, s20 = 20, s21 = 21;
        double s22 = 22, s23 = 23;
        for (int i = 0; i < hs.length; i++) {
            if ((i & 7) == 7) {
                sink += fill(12);
            }
            double h;
            try {
                h = hs[i].v;
            } catch (NullPointerException e) {
                h = -0.5;
            }
            s0 = s0 * 0.999 + h; s1 = s1 * 0.998 + h; s2 = s2 * 0.997 + h; s3 = s3 * 0.996 + h;
            s4 = s4 * 0.995 + h; s5 = s5 * 0.994 + h; s6 = s6 * 0.993 + h; s7 = s7 * 0.992 + h;
            s8 = s8 * 0.991 + h; s9 = s9 * 0.990 + h; s10 = s10 * 0.989 + h; s11 = s11 * 0.988 + h;
            s12 = s12 * 0.987 + h; s13 = s13 * 0.986 + h; s14 = s14 * 0.985 + h; s15 = s15 * 0.984 + h;
            s16 = s16 * 0.983 + h; s17 = s17 * 0.982 + h; s18 = s18 * 0.981 + h; s19 = s19 * 0.980 + h;
            s20 = s20 * 0.979 + h; s21 = s21 * 0.978 + h; s22 = s22 * 0.977 + h; s23 = s23 * 0.976 + h;
        }
        return s0 + 2 * s1 + 3 * s2 + 5 * s3 + 7 * s4 + 11 * s5 + 13 * s6 + 17 * s7 + 19 * s8 + 23 * s9
             + 29 * s10 + 31 * s11 + 37 * s12 + 41 * s13 + 43 * s14 + 47 * s15 + 53 * s16 + 59 * s17
             + 61 * s18 + 67 * s19 + 71 * s20 + 73 * s21 + 79 * s22 + 83 * s23;
    }

    public static void main(String[] args) {
        String mode = args[0];
        long end = System.nanoTime() + Long.parseLong(args[1]) * 1_000_000_000L;
        long calls = 0, bad = 0;
        if (mode.equals("c2poll")) {
            float[] a = new float[4 * S.length()];
            for (int i = 0; i < a.length; i++) {
                a[i] = i * 0.125f;
            }
            float expected = c2poll(a, 512);
            while (System.nanoTime() < end) {
                float r = c2poll(a, 512);
                calls++;
                if (Float.floatToRawIntBits(r) != Float.floatToRawIntBits(expected) && bad++ < 5) {
                    System.out.println("MISMATCH call=" + calls + " got=" + r + " expected=" + expected);
                }
            }
        } else {
            H[] hs = new H[256];
            for (int i = 0; i < hs.length; i++) {
                hs[i] = (i % 16 == 5) ? null : new H(i * 0.0625);
            }
            double expected = npe(hs);
            while (System.nanoTime() < end) {
                double r = npe(hs);
                calls++;
                if (Double.doubleToRawLongBits(r) != Double.doubleToRawLongBits(expected) && bad++ < 5) {
                    System.out.println("MISMATCH call=" + calls + " got=" + r + " expected=" + expected);
                }
            }
        }
        System.out.println("StaleProbe " + mode + " calls=" + calls + " mismatches=" + bad);
        System.exit(bad > 0 ? 3 : 0);
    }
}
