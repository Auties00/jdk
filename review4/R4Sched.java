public class R4Sched {
    static volatile double sinkD;
    static double kernel(double x, int n) {
        double a0=x+0,a1=x+1,a2=x+2,a3=x+3,a4=x+4,a5=x+5,a6=x+6,a7=x+7,a8=x+8,a9=x+9,a10=x+10,a11=x+11,a12=x+12,a13=x+13,a14=x+14,a15=x+15,a16=x+16,a17=x+17,a18=x+18,a19=x+19,a20=x+20,a21=x+21,a22=x+22,a23=x+23;
        for (int i = 0; i < n; i++) {
            a0 += Math.sin(a0);
            a1 += a0 * 0.5;
            a2 += a1 * 0.5;
            a3 += a2 * 0.5;
            a4 += a3 * 0.5;
            a5 += a4 * 0.5;
            a6 += a5 * 0.5;
            a7 += a6 * 0.5;
            a8 += a7 * 0.5;
            a9 += a8 * 0.5;
            a10 += a9 * 0.5;
            a11 += a10 * 0.5;
            a12 += a11 * 0.5;
            a13 += a12 * 0.5;
            a14 += a13 * 0.5;
            a15 += a14 * 0.5;
            a16 += a15 * 0.5;
            a17 += a16 * 0.5;
            a18 += a17 * 0.5;
            a19 += a18 * 0.5;
            a20 += a19 * 0.5;
            a21 += a20 * 0.5;
            a22 += a21 * 0.5;
            a23 += a22 * 0.5;
        }
        return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23;
    }
    public static void main(String[] args) {
        long end = System.nanoTime() + Long.getLong("ms", 3000) * 1_000_000L;
        double r = 0;
        while (System.nanoTime() < end) {
            r += kernel(r * 1e-300, 1000);
        }
        sinkD = r;
        System.out.println("done " + r);
    }
}
