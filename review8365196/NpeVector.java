import jdk.incubator.vector.FloatVector;
import jdk.incubator.vector.VectorSpecies;
import java.util.Arrays;

/**
 * JDK-8365196 review probe: 10 FloatVector and 12 double accumulators live across an implicit
 * null check (and across the loop's safepoint poll). An implicit null check trap per cycle deoptimizes
 * the C2 frame from the registers; the result is compared with the interpreter (reference() excluded
 * from compilation). With -XX:+SafepointALot the same comparison on non-null input checks the poll.
 */
public final class NpeVector {
    static final VectorSpecies<Float> S = FloatVector.SPECIES_PREFERRED;

    static final class H {
        final float v;
        H(float v) { this.v = v; }
    }

    static void kernel(H[] hs, float[] a, float x, float[] out) {
        FloatVector v0 = FloatVector.broadcast(S, x + 0.5f);
        FloatVector v1 = FloatVector.broadcast(S, x + 1.5f);
        FloatVector v2 = FloatVector.broadcast(S, x + 2.5f);
        FloatVector v3 = FloatVector.broadcast(S, x + 3.5f);
        FloatVector v4 = FloatVector.broadcast(S, x + 4.5f);
        FloatVector v5 = FloatVector.broadcast(S, x + 5.5f);
        FloatVector v6 = FloatVector.broadcast(S, x + 6.5f);
        FloatVector v7 = FloatVector.broadcast(S, x + 7.5f);
        FloatVector v8 = FloatVector.broadcast(S, x + 8.5f);
        FloatVector v9 = FloatVector.broadcast(S, x + 9.5f);
        double d0 = x * 1.0;
        double d1 = x * 1.0625;
        double d2 = x * 1.125;
        double d3 = x * 1.1875;
        double d4 = x * 1.25;
        double d5 = x * 1.3125;
        double d6 = x * 1.375;
        double d7 = x * 1.4375;
        double d8 = x * 1.5;
        double d9 = x * 1.5625;
        double d10 = x * 1.625;
        double d11 = x * 1.6875;
        for (int i = 0; i < hs.length; i++) {
            float h;
            try {
                h = hs[i].v;
            } catch (NullPointerException e) {
                h = -3.25f;
            }
            FloatVector m = FloatVector.fromArray(S, a, (i & 3) * S.length());
            FloatVector hv = FloatVector.broadcast(S, h);
            v0 = v0.mul(0.75f).add(m.mul(1.0f)).add(hv);
            v1 = v1.mul(0.76f).add(m.mul(1.125f)).add(hv);
            v2 = v2.mul(0.77f).add(m.mul(1.25f)).add(hv);
            v3 = v3.mul(0.78f).add(m.mul(1.375f)).add(hv);
            v4 = v4.mul(0.79f).add(m.mul(1.5f)).add(hv);
            v5 = v5.mul(0.8f).add(m.mul(1.625f)).add(hv);
            v6 = v6.mul(0.81f).add(m.mul(1.75f)).add(hv);
            v7 = v7.mul(0.8200000000000001f).add(m.mul(1.875f)).add(hv);
            v8 = v8.mul(0.83f).add(m.mul(2.0f)).add(hv);
            v9 = v9.mul(0.84f).add(m.mul(2.125f)).add(hv);
            d0 = d0 * 0.999 + h * 1.0;
            d1 = d1 * 0.998 + h * 1.01;
            d2 = d2 * 0.997 + h * 1.02;
            d3 = d3 * 0.996 + h * 1.03;
            d4 = d4 * 0.995 + h * 1.04;
            d5 = d5 * 0.994 + h * 1.05;
            d6 = d6 * 0.993 + h * 1.06;
            d7 = d7 * 0.992 + h * 1.07;
            d8 = d8 * 0.991 + h * 1.08;
            d9 = d9 * 0.99 + h * 1.09;
            d10 = d10 * 0.989 + h * 1.1;
            d11 = d11 * 0.988 + h * 1.11;
        }
        v0.intoArray(out, 0 * S.length());
        v1.intoArray(out, 1 * S.length());
        v2.intoArray(out, 2 * S.length());
        v3.intoArray(out, 3 * S.length());
        v4.intoArray(out, 4 * S.length());
        v5.intoArray(out, 5 * S.length());
        v6.intoArray(out, 6 * S.length());
        v7.intoArray(out, 7 * S.length());
        v8.intoArray(out, 8 * S.length());
        v9.intoArray(out, 9 * S.length());
        out[10 * S.length() + 0] = (float) d0;
        out[10 * S.length() + 1] = (float) d1;
        out[10 * S.length() + 2] = (float) d2;
        out[10 * S.length() + 3] = (float) d3;
        out[10 * S.length() + 4] = (float) d4;
        out[10 * S.length() + 5] = (float) d5;
        out[10 * S.length() + 6] = (float) d6;
        out[10 * S.length() + 7] = (float) d7;
        out[10 * S.length() + 8] = (float) d8;
        out[10 * S.length() + 9] = (float) d9;
        out[10 * S.length() + 10] = (float) d10;
        out[10 * S.length() + 11] = (float) d11;
    }

    static void reference(H[] hs, float[] a, float x, float[] out) {
        FloatVector v0 = FloatVector.broadcast(S, x + 0.5f);
        FloatVector v1 = FloatVector.broadcast(S, x + 1.5f);
        FloatVector v2 = FloatVector.broadcast(S, x + 2.5f);
        FloatVector v3 = FloatVector.broadcast(S, x + 3.5f);
        FloatVector v4 = FloatVector.broadcast(S, x + 4.5f);
        FloatVector v5 = FloatVector.broadcast(S, x + 5.5f);
        FloatVector v6 = FloatVector.broadcast(S, x + 6.5f);
        FloatVector v7 = FloatVector.broadcast(S, x + 7.5f);
        FloatVector v8 = FloatVector.broadcast(S, x + 8.5f);
        FloatVector v9 = FloatVector.broadcast(S, x + 9.5f);
        double d0 = x * 1.0;
        double d1 = x * 1.0625;
        double d2 = x * 1.125;
        double d3 = x * 1.1875;
        double d4 = x * 1.25;
        double d5 = x * 1.3125;
        double d6 = x * 1.375;
        double d7 = x * 1.4375;
        double d8 = x * 1.5;
        double d9 = x * 1.5625;
        double d10 = x * 1.625;
        double d11 = x * 1.6875;
        for (int i = 0; i < hs.length; i++) {
            float h;
            try {
                h = hs[i].v;
            } catch (NullPointerException e) {
                h = -3.25f;
            }
            FloatVector m = FloatVector.fromArray(S, a, (i & 3) * S.length());
            FloatVector hv = FloatVector.broadcast(S, h);
            v0 = v0.mul(0.75f).add(m.mul(1.0f)).add(hv);
            v1 = v1.mul(0.76f).add(m.mul(1.125f)).add(hv);
            v2 = v2.mul(0.77f).add(m.mul(1.25f)).add(hv);
            v3 = v3.mul(0.78f).add(m.mul(1.375f)).add(hv);
            v4 = v4.mul(0.79f).add(m.mul(1.5f)).add(hv);
            v5 = v5.mul(0.8f).add(m.mul(1.625f)).add(hv);
            v6 = v6.mul(0.81f).add(m.mul(1.75f)).add(hv);
            v7 = v7.mul(0.8200000000000001f).add(m.mul(1.875f)).add(hv);
            v8 = v8.mul(0.83f).add(m.mul(2.0f)).add(hv);
            v9 = v9.mul(0.84f).add(m.mul(2.125f)).add(hv);
            d0 = d0 * 0.999 + h * 1.0;
            d1 = d1 * 0.998 + h * 1.01;
            d2 = d2 * 0.997 + h * 1.02;
            d3 = d3 * 0.996 + h * 1.03;
            d4 = d4 * 0.995 + h * 1.04;
            d5 = d5 * 0.994 + h * 1.05;
            d6 = d6 * 0.993 + h * 1.06;
            d7 = d7 * 0.992 + h * 1.07;
            d8 = d8 * 0.991 + h * 1.08;
            d9 = d9 * 0.99 + h * 1.09;
            d10 = d10 * 0.989 + h * 1.1;
            d11 = d11 * 0.988 + h * 1.11;
        }
        v0.intoArray(out, 0 * S.length());
        v1.intoArray(out, 1 * S.length());
        v2.intoArray(out, 2 * S.length());
        v3.intoArray(out, 3 * S.length());
        v4.intoArray(out, 4 * S.length());
        v5.intoArray(out, 5 * S.length());
        v6.intoArray(out, 6 * S.length());
        v7.intoArray(out, 7 * S.length());
        v8.intoArray(out, 8 * S.length());
        v9.intoArray(out, 9 * S.length());
        out[10 * S.length() + 0] = (float) d0;
        out[10 * S.length() + 1] = (float) d1;
        out[10 * S.length() + 2] = (float) d2;
        out[10 * S.length() + 3] = (float) d3;
        out[10 * S.length() + 4] = (float) d4;
        out[10 * S.length() + 5] = (float) d5;
        out[10 * S.length() + 6] = (float) d6;
        out[10 * S.length() + 7] = (float) d7;
        out[10 * S.length() + 8] = (float) d8;
        out[10 * S.length() + 9] = (float) d9;
        out[10 * S.length() + 10] = (float) d10;
        out[10 * S.length() + 11] = (float) d11;
    }

    public static void main(String[] args) throws Exception {
        int[] code = new int[1];
        Runnable body = () -> code[0] = run(args);
        if (args.length > 1 && args[1].equals("thread")) {
            Thread t = new Thread(body, "worker");
            t.start();
            t.join();
        } else {
            body.run();
        }
        System.exit(code[0]);
    }

    static int run(String[] args) {
        long seconds = Long.parseLong(args[0]);
        H[] good = new H[64];
        for (int i = 0; i < good.length; i++) good[i] = new H(i * 0.25f);
        H[] bad = good.clone();
        bad[40] = null;
        float[] a = new float[4 * S.length()];
        for (int i = 0; i < a.length; i++) a[i] = i * 0.5f - 7f;
        int n = 10 * S.length() + 12;
        float[] o1 = new float[n], o2 = new float[n];
        long end = System.nanoTime() + seconds * 1_000_000_000L;
        long cycles = 0, mismatches = 0, goodMismatches = 0, goodChecks = 0;
        while (System.nanoTime() < end) {
            for (int k = 0; k < 2000; k++) {
                kernel(good, a, k, o1);
                if ((k & 63) == 0) {
                    reference(good, a, k, o2);
                    goodChecks++;
                    if (!Arrays.equals(o1, o2)) {
                        goodMismatches++;
                        if (goodMismatches <= 3) System.out.println("GOOD MISMATCH k=" + k);
                    }
                }
            }
            kernel(bad, a, 1.0f, o1);
            reference(bad, a, 1.0f, o2);
            cycles++;
            if (!Arrays.equals(o1, o2)) {
                mismatches++;
                if (mismatches <= 5) System.out.println("MISMATCH cycle=" + cycles + " first diff at " + Arrays.mismatch(o1, o2));
            }
        }
        System.out.println("RESULT cycles=" + cycles + " npe_mismatches=" + mismatches + " good_mismatches=" + goodMismatches + "/" + goodChecks);
        return mismatches + goodMismatches > 0 ? 3 : 0;
    }
}
