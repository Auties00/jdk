import java.util.Arrays;

/**
 * Implicit null check leftover probe for JDK-8365196. A C2-compiled method keeps 28 doubles live
 * (some in xmm16-31 with UseAVX=3) across an implicit null check, inside a loop that also runs a
 * SuperWord loop (zmm in use). The null check traps once per compilation; C2 continues to an
 * uncommon trap and deoptimizes from the registers. The result is compared with the same code run
 * by the interpreter (reference() is excluded from compilation).
 */
public final class NpeLeftover {
    static final class H {
        final double v;
        H(double v) { this.v = v; }
    }

    static final float VALUE = 15.0f;

    static double kernel(H[] hs, float[] a, double x) {
        double s00 = x * 1.0 + 0.0;
        double s01 = x * 1.03125 + 0.5;
        double s02 = x * 1.0625 + 1.0;
        double s03 = x * 1.09375 + 1.5;
        double s04 = x * 1.125 + 2.0;
        double s05 = x * 1.15625 + 2.5;
        double s06 = x * 1.1875 + 3.0;
        double s07 = x * 1.21875 + 3.5;
        double s08 = x * 1.25 + 4.0;
        double s09 = x * 1.28125 + 4.5;
        double s10 = x * 1.3125 + 5.0;
        double s11 = x * 1.34375 + 5.5;
        double s12 = x * 1.375 + 6.0;
        double s13 = x * 1.40625 + 6.5;
        double s14 = x * 1.4375 + 7.0;
        double s15 = x * 1.46875 + 7.5;
        double s16 = x * 1.5 + 8.0;
        double s17 = x * 1.53125 + 8.5;
        double s18 = x * 1.5625 + 9.0;
        double s19 = x * 1.59375 + 9.5;
        double s20 = x * 1.625 + 10.0;
        double s21 = x * 1.65625 + 10.5;
        double s22 = x * 1.6875 + 11.0;
        double s23 = x * 1.71875 + 11.5;
        double s24 = x * 1.75 + 12.0;
        double s25 = x * 1.78125 + 12.5;
        double s26 = x * 1.8125 + 13.0;
        double s27 = x * 1.84375 + 13.5;
        for (int i = 0; i < hs.length; i++) {
            for (int j = 0; j < a.length; j++) {
                a[j] += VALUE;
            }
            double h;
            try {
                h = hs[i].v;
            } catch (NullPointerException e) {
                h = -3.25;
            }
            s00 = s00 * 0.999 + h * 1.0;
            s01 = s01 * 0.998 + h * 1.01;
            s02 = s02 * 0.997 + h * 1.02;
            s03 = s03 * 0.996 + h * 1.03;
            s04 = s04 * 0.995 + h * 1.04;
            s05 = s05 * 0.994 + h * 1.05;
            s06 = s06 * 0.993 + h * 1.06;
            s07 = s07 * 0.992 + h * 1.07;
            s08 = s08 * 0.991 + h * 1.08;
            s09 = s09 * 0.99 + h * 1.09;
            s10 = s10 * 0.989 + h * 1.1;
            s11 = s11 * 0.988 + h * 1.11;
            s12 = s12 * 0.987 + h * 1.12;
            s13 = s13 * 0.986 + h * 1.13;
            s14 = s14 * 0.985 + h * 1.1400000000000001;
            s15 = s15 * 0.984 + h * 1.15;
            s16 = s16 * 0.983 + h * 1.16;
            s17 = s17 * 0.982 + h * 1.17;
            s18 = s18 * 0.981 + h * 1.18;
            s19 = s19 * 0.98 + h * 1.19;
            s20 = s20 * 0.979 + h * 1.2;
            s21 = s21 * 0.978 + h * 1.21;
            s22 = s22 * 0.977 + h * 1.22;
            s23 = s23 * 0.976 + h * 1.23;
            s24 = s24 * 0.975 + h * 1.24;
            s25 = s25 * 0.974 + h * 1.25;
            s26 = s26 * 0.973 + h * 1.26;
            s27 = s27 * 0.972 + h * 1.27;
        }
        return s00 * 1.0 + s01 * 2.0 + s02 * 3.0 + s03 * 4.0 + s04 * 5.0 + s05 * 6.0 + s06 * 7.0 + s07 * 8.0 + s08 * 9.0 + s09 * 10.0 + s10 * 11.0 + s11 * 12.0 + s12 * 13.0 + s13 * 14.0 + s14 * 15.0 + s15 * 16.0 + s16 * 17.0 + s17 * 18.0 + s18 * 19.0 + s19 * 20.0 + s20 * 21.0 + s21 * 22.0 + s22 * 23.0 + s23 * 24.0 + s24 * 25.0 + s25 * 26.0 + s26 * 27.0 + s27 * 28.0 + a[0] + a[a.length - 1];
    }

    static double reference(H[] hs, float[] a, double x) {
        double s00 = x * 1.0 + 0.0;
        double s01 = x * 1.03125 + 0.5;
        double s02 = x * 1.0625 + 1.0;
        double s03 = x * 1.09375 + 1.5;
        double s04 = x * 1.125 + 2.0;
        double s05 = x * 1.15625 + 2.5;
        double s06 = x * 1.1875 + 3.0;
        double s07 = x * 1.21875 + 3.5;
        double s08 = x * 1.25 + 4.0;
        double s09 = x * 1.28125 + 4.5;
        double s10 = x * 1.3125 + 5.0;
        double s11 = x * 1.34375 + 5.5;
        double s12 = x * 1.375 + 6.0;
        double s13 = x * 1.40625 + 6.5;
        double s14 = x * 1.4375 + 7.0;
        double s15 = x * 1.46875 + 7.5;
        double s16 = x * 1.5 + 8.0;
        double s17 = x * 1.53125 + 8.5;
        double s18 = x * 1.5625 + 9.0;
        double s19 = x * 1.59375 + 9.5;
        double s20 = x * 1.625 + 10.0;
        double s21 = x * 1.65625 + 10.5;
        double s22 = x * 1.6875 + 11.0;
        double s23 = x * 1.71875 + 11.5;
        double s24 = x * 1.75 + 12.0;
        double s25 = x * 1.78125 + 12.5;
        double s26 = x * 1.8125 + 13.0;
        double s27 = x * 1.84375 + 13.5;
        for (int i = 0; i < hs.length; i++) {
            for (int j = 0; j < a.length; j++) {
                a[j] += VALUE;
            }
            double h;
            try {
                h = hs[i].v;
            } catch (NullPointerException e) {
                h = -3.25;
            }
            s00 = s00 * 0.999 + h * 1.0;
            s01 = s01 * 0.998 + h * 1.01;
            s02 = s02 * 0.997 + h * 1.02;
            s03 = s03 * 0.996 + h * 1.03;
            s04 = s04 * 0.995 + h * 1.04;
            s05 = s05 * 0.994 + h * 1.05;
            s06 = s06 * 0.993 + h * 1.06;
            s07 = s07 * 0.992 + h * 1.07;
            s08 = s08 * 0.991 + h * 1.08;
            s09 = s09 * 0.99 + h * 1.09;
            s10 = s10 * 0.989 + h * 1.1;
            s11 = s11 * 0.988 + h * 1.11;
            s12 = s12 * 0.987 + h * 1.12;
            s13 = s13 * 0.986 + h * 1.13;
            s14 = s14 * 0.985 + h * 1.1400000000000001;
            s15 = s15 * 0.984 + h * 1.15;
            s16 = s16 * 0.983 + h * 1.16;
            s17 = s17 * 0.982 + h * 1.17;
            s18 = s18 * 0.981 + h * 1.18;
            s19 = s19 * 0.98 + h * 1.19;
            s20 = s20 * 0.979 + h * 1.2;
            s21 = s21 * 0.978 + h * 1.21;
            s22 = s22 * 0.977 + h * 1.22;
            s23 = s23 * 0.976 + h * 1.23;
            s24 = s24 * 0.975 + h * 1.24;
            s25 = s25 * 0.974 + h * 1.25;
            s26 = s26 * 0.973 + h * 1.26;
            s27 = s27 * 0.972 + h * 1.27;
        }
        return s00 * 1.0 + s01 * 2.0 + s02 * 3.0 + s03 * 4.0 + s04 * 5.0 + s05 * 6.0 + s06 * 7.0 + s07 * 8.0 + s08 * 9.0 + s09 * 10.0 + s10 * 11.0 + s11 * 12.0 + s12 * 13.0 + s13 * 14.0 + s14 * 15.0 + s15 * 16.0 + s16 * 17.0 + s17 * 18.0 + s18 * 19.0 + s19 * 20.0 + s20 * 21.0 + s21 * 22.0 + s22 * 23.0 + s23 * 24.0 + s24 * 25.0 + s25 * 26.0 + s26 * 27.0 + s27 * 28.0 + a[0] + a[a.length - 1];
    }

    public static void main(String[] args) {
        long seconds = Long.parseLong(args[0]);
        H[] good = new H[64];
        for (int i = 0; i < good.length; i++) good[i] = new H(i * 0.25);
        H[] bad = good.clone();
        bad[40] = null;
        float[] base = new float[97];
        for (int i = 0; i < base.length; i++) base[i] = i;
        long end = System.nanoTime() + seconds * 1_000_000_000L;
        long cycles = 0, mismatches = 0, goodMismatches = 0;
        double sink = 0;
        while (System.nanoTime() < end) {
            for (int k = 0; k < 2000; k++) {
                sink += kernel(good, base.clone(), k);
            }
            double gr = kernel(good, base.clone(), 3.0);
            double ge = reference(good, base.clone(), 3.0);
            if (Double.doubleToRawLongBits(gr) != Double.doubleToRawLongBits(ge)) goodMismatches++;
            double r = kernel(bad, base.clone(), 1.0);
            double e = reference(bad, base.clone(), 1.0);
            cycles++;
            if (Double.doubleToRawLongBits(r) != Double.doubleToRawLongBits(e)) {
                mismatches++;
                if (mismatches <= 5) System.out.println("MISMATCH cycle=" + cycles + " got=" + r + " expected=" + e);
            }
        }
        System.out.println("RESULT cycles=" + cycles + " npe_mismatches=" + mismatches + " good_mismatches=" + goodMismatches + " sink=" + (sink != 0));
        System.exit(mismatches + goodMismatches > 0 ? 3 : 0);
    }
}
