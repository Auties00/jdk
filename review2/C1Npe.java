/**
 * JDK-8365196 review 2: C1 implicit exceptions. 24 double accumulators are live across an implicit null check
 * (hs[i].v) and an implicit divide check (1000 / ds[i]) that are caught in the same method. C1 continues to
 * its handler with the registers its throw stub saved after the trap, so a short exception context loses the
 * accumulators that live in xmm16-31. The results of the compiled kernel are compared with the same code run
 * by the interpreter (reference() is excluded from compilation).
 * Usage: java -XX:CompileCommand=exclude,C1Npe::reference C1Npe <seconds>
 */
public final class C1Npe {
    static final class H {
        final double v;
        H(double v) { this.v = v; }
    }

    static double kernel(H[] hs, int[] ds, double x) {
        double s0 = x, s1 = x + 1, s2 = x + 2, s3 = x + 3, s4 = x + 4, s5 = x + 5, s6 = x + 6, s7 = x + 7;
        double s8 = x + 8, s9 = x + 9, s10 = x + 10, s11 = x + 11, s12 = x + 12, s13 = x + 13, s14 = x + 14, s15 = x + 15;
        double s16 = x + 16, s17 = x + 17, s18 = x + 18, s19 = x + 19, s20 = x + 20, s21 = x + 21, s22 = x + 22, s23 = x + 23;
        for (int i = 0; i < hs.length; i++) {
            double h;
            try {
                h = hs[i].v;
            } catch (NullPointerException e) {
                h = -0.5;
            }
            int q;
            try {
                q = 1000 / ds[i];
            } catch (ArithmeticException e) {
                q = 7;
            }
            h += q * 0.001;
            s0 = s0 * 0.999 + h * 1.01; s1 = s1 * 0.998 + h * 1.02; s2 = s2 * 0.997 + h * 1.03;
            s3 = s3 * 0.996 + h * 1.04; s4 = s4 * 0.995 + h * 1.05; s5 = s5 * 0.994 + h * 1.06;
            s6 = s6 * 0.993 + h * 1.07; s7 = s7 * 0.992 + h * 1.08; s8 = s8 * 0.991 + h * 1.09;
            s9 = s9 * 0.990 + h * 1.10; s10 = s10 * 0.989 + h * 1.11; s11 = s11 * 0.988 + h * 1.12;
            s12 = s12 * 0.987 + h * 1.13; s13 = s13 * 0.986 + h * 1.14; s14 = s14 * 0.985 + h * 1.15;
            s15 = s15 * 0.984 + h * 1.16; s16 = s16 * 0.983 + h * 1.17; s17 = s17 * 0.982 + h * 1.18;
            s18 = s18 * 0.981 + h * 1.19; s19 = s19 * 0.980 + h * 1.20; s20 = s20 * 0.979 + h * 1.21;
            s21 = s21 * 0.978 + h * 1.22; s22 = s22 * 0.977 + h * 1.23; s23 = s23 * 0.976 + h * 1.24;
        }
        return s0 + 2 * s1 + 3 * s2 + 5 * s3 + 7 * s4 + 11 * s5 + 13 * s6 + 17 * s7 + 19 * s8 + 23 * s9 + 29 * s10
             + 31 * s11 + 37 * s12 + 41 * s13 + 43 * s14 + 47 * s15 + 53 * s16 + 59 * s17 + 61 * s18 + 67 * s19
             + 71 * s20 + 73 * s21 + 79 * s22 + 83 * s23;
    }

    // Same code, run by the interpreter.
    static double reference(H[] hs, int[] ds, double x) {
        double s0 = x, s1 = x + 1, s2 = x + 2, s3 = x + 3, s4 = x + 4, s5 = x + 5, s6 = x + 6, s7 = x + 7;
        double s8 = x + 8, s9 = x + 9, s10 = x + 10, s11 = x + 11, s12 = x + 12, s13 = x + 13, s14 = x + 14, s15 = x + 15;
        double s16 = x + 16, s17 = x + 17, s18 = x + 18, s19 = x + 19, s20 = x + 20, s21 = x + 21, s22 = x + 22, s23 = x + 23;
        for (int i = 0; i < hs.length; i++) {
            double h;
            try {
                h = hs[i].v;
            } catch (NullPointerException e) {
                h = -0.5;
            }
            int q;
            try {
                q = 1000 / ds[i];
            } catch (ArithmeticException e) {
                q = 7;
            }
            h += q * 0.001;
            s0 = s0 * 0.999 + h * 1.01; s1 = s1 * 0.998 + h * 1.02; s2 = s2 * 0.997 + h * 1.03;
            s3 = s3 * 0.996 + h * 1.04; s4 = s4 * 0.995 + h * 1.05; s5 = s5 * 0.994 + h * 1.06;
            s6 = s6 * 0.993 + h * 1.07; s7 = s7 * 0.992 + h * 1.08; s8 = s8 * 0.991 + h * 1.09;
            s9 = s9 * 0.990 + h * 1.10; s10 = s10 * 0.989 + h * 1.11; s11 = s11 * 0.988 + h * 1.12;
            s12 = s12 * 0.987 + h * 1.13; s13 = s13 * 0.986 + h * 1.14; s14 = s14 * 0.985 + h * 1.15;
            s15 = s15 * 0.984 + h * 1.16; s16 = s16 * 0.983 + h * 1.17; s17 = s17 * 0.982 + h * 1.18;
            s18 = s18 * 0.981 + h * 1.19; s19 = s19 * 0.980 + h * 1.20; s20 = s20 * 0.979 + h * 1.21;
            s21 = s21 * 0.978 + h * 1.22; s22 = s22 * 0.977 + h * 1.23; s23 = s23 * 0.976 + h * 1.24;
        }
        return s0 + 2 * s1 + 3 * s2 + 5 * s3 + 7 * s4 + 11 * s5 + 13 * s6 + 17 * s7 + 19 * s8 + 23 * s9 + 29 * s10
             + 31 * s11 + 37 * s12 + 41 * s13 + 43 * s14 + 47 * s15 + 53 * s16 + 59 * s17 + 61 * s18 + 67 * s19
             + 71 * s20 + 73 * s21 + 79 * s22 + 83 * s23;
    }

    public static void main(String[] args) {
        long seconds = Long.parseLong(args[0]);
        int n = 512;
        H[] hs = new H[n];
        int[] ds = new int[n];
        for (int i = 0; i < n; i++) {
            hs[i] = (i % 61 == 7) ? null : new H(i * 0.0625);
            ds[i] = (i % 67 == 11) ? 0 : 1 + (i % 13);
        }
        double[] expected = new double[8];
        for (int k = 0; k < 8; k++) {
            expected[k] = reference(hs, ds, k * 0.25);
        }
        long end = System.nanoTime() + seconds * 1_000_000_000L;
        long calls = 0, bad = 0;
        while (System.nanoTime() < end) {
            for (int k = 0; k < 8; k++) {
                double r = kernel(hs, ds, k * 0.25);
                calls++;
                if (Double.doubleToRawLongBits(r) != Double.doubleToRawLongBits(expected[k])) {
                    if (bad++ < 5) {
                        System.out.println("MISMATCH call=" + calls + " k=" + k + " got=" + r + " expected=" + expected[k]);
                    }
                }
            }
        }
        System.out.println("C1Npe calls=" + calls + " traps~=" + (calls * 17) + " mismatches=" + bad);
        if (bad > 0) {
            System.exit(3);
        }
    }
}
