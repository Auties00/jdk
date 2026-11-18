import java.lang.classfile.ClassFile;
import java.lang.constant.ClassDesc;
import java.lang.constant.ConstantDescs;
import java.lang.invoke.MethodHandles;
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorSpecies;

// JDK-8365196 scrub7 experiment: which planter leaves a header-like value where the next trap of the
// calling compiled frame finds it, under Serial/Parallel. args: planter (none|native|interp|c2) trap (poll|npe) seconds
public class PlantExp {
    static final VectorSpecies<Double> S = DoubleVector.SPECIES_256;
    static final int ITERS = 200_000;
    static String planter;
    static boolean npe;
    static Object sink;
    static volatile boolean stop;
    static final Runnable FILLER = makeFiller();

    static final class Holder {
        int x = 1;
    }

    static Runnable makeFiller() {
        ClassDesc self = ClassDesc.of("PlantFiller");
        byte[] bytes = ClassFile.of().build(self, cb -> {
            cb.withFlags(ClassFile.ACC_PUBLIC | ClassFile.ACC_FINAL);
            cb.withInterfaceSymbols(ClassDesc.of("java.lang.Runnable"));
            cb.withMethodBody(ConstantDescs.INIT_NAME, ConstantDescs.MTD_void, ClassFile.ACC_PUBLIC,
                code -> code.aload(0).invokespecial(ConstantDescs.CD_Object, ConstantDescs.INIT_NAME, ConstantDescs.MTD_void).return_());
            cb.withMethodBody("run", ConstantDescs.MTD_void, ClassFile.ACC_PUBLIC, code -> {
                var nz = code.constantPool().doubleEntry(-0.0);
                for (int i = 0; i < 1536; i++) {
                    code.ldc(nz);
                    code.dstore(1 + 2 * i);
                }
                code.return_();
            });
        });
        try {
            return (Runnable) MethodHandles.lookup().defineClass(bytes).getConstructor().newInstance();
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
    }

    static native void fillBelowStack();

    static void callee() {
    }

    // 64 distinct -0.0 values live across a call: spilled into this frame.
    static double c2plant(double z) {
        DoubleVector v0 = DoubleVector.broadcast(S, z * 1.0), v1 = DoubleVector.broadcast(S, z * 2.0),
            v2 = DoubleVector.broadcast(S, z * 3.0), v3 = DoubleVector.broadcast(S, z * 4.0),
            v4 = DoubleVector.broadcast(S, z * 5.0), v5 = DoubleVector.broadcast(S, z * 6.0),
            v6 = DoubleVector.broadcast(S, z * 7.0), v7 = DoubleVector.broadcast(S, z * 8.0),
            v8 = DoubleVector.broadcast(S, z * 9.0), v9 = DoubleVector.broadcast(S, z * 10.0),
            v10 = DoubleVector.broadcast(S, z * 11.0), v11 = DoubleVector.broadcast(S, z * 12.0),
            v12 = DoubleVector.broadcast(S, z * 13.0), v13 = DoubleVector.broadcast(S, z * 14.0),
            v14 = DoubleVector.broadcast(S, z * 15.0), v15 = DoubleVector.broadcast(S, z * 16.0),
            v16 = DoubleVector.broadcast(S, z * 17.0), v17 = DoubleVector.broadcast(S, z * 18.0),
            v18 = DoubleVector.broadcast(S, z * 19.0), v19 = DoubleVector.broadcast(S, z * 20.0),
            v20 = DoubleVector.broadcast(S, z * 21.0), v21 = DoubleVector.broadcast(S, z * 22.0),
            v22 = DoubleVector.broadcast(S, z * 23.0), v23 = DoubleVector.broadcast(S, z * 24.0),
            v24 = DoubleVector.broadcast(S, z * 25.0), v25 = DoubleVector.broadcast(S, z * 26.0),
            v26 = DoubleVector.broadcast(S, z * 27.0), v27 = DoubleVector.broadcast(S, z * 28.0),
            v28 = DoubleVector.broadcast(S, z * 29.0), v29 = DoubleVector.broadcast(S, z * 30.0),
            v30 = DoubleVector.broadcast(S, z * 31.0), v31 = DoubleVector.broadcast(S, z * 32.0);
        callee();
        return v0.add(v1).add(v2).add(v3).add(v4).add(v5).add(v6).add(v7).add(v8).add(v9).add(v10).add(v11)
            .add(v12).add(v13).add(v14).add(v15).add(v16).add(v17).add(v18).add(v19).add(v20).add(v21).add(v22)
            .add(v23).add(v24).add(v25).add(v26).add(v27).add(v28).add(v29).add(v30).add(v31).lane(0);
    }

    static long kernel(double neg, Holder h) {
        switch (planter) {
            case "native": fillBelowStack(); break;
            case "interp": FILLER.run(); break;
            case "c2": sink = c2plant(neg); break;
            default: break;
        }
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = DoubleVector.broadcast(S, neg), a1 = DoubleVector.broadcast(S, neg);
        DoubleVector a2 = DoubleVector.broadcast(S, neg), a3 = DoubleVector.broadcast(S, neg);
        int count = 0;
        if (npe) {
            try {
                count += h.x; // an implicit null check that faults with a0-a3 live
            } catch (NullPointerException e) {
                count--;
            }
        }
        for (int i = 0; i < ITERS; i++) {
            a0 = a0.add(v);
            a1 = a1.add(v);
            a2 = a2.add(v);
            a3 = a3.add(v);
        }
        sink = count;
        LongVector r = a0.reinterpretAsLongs().and(a1.reinterpretAsLongs()).and(a2.reinterpretAsLongs()).and(a3.reinterpretAsLongs());
        long lost = 0;
        for (int l = 0; l < r.length(); l++) {
            if (r.lane(l) != Long.MIN_VALUE) {
                lost++;
            }
        }
        return lost;
    }

    public static void main(String[] args) {
        planter = args[0];
        npe = args[1].equals("npe");
        long seconds = Long.parseLong(args[2]);
        if (planter.equals("native")) {
            System.loadLibrary("PlantExp");
        }
        Holder h = new Holder();
        long end = System.nanoTime() + seconds * 1_000_000_000L;
        long calls = 0, wrong = 0;
        while (System.nanoTime() < end) {
            long lost = kernel(-0.0, npe ? null : h);
            calls++;
            if (lost != 0) {
                wrong++;
            }
        }
        System.out.println("RESULT planter=" + planter + " trap=" + args[1] + " calls=" + calls + " wrong=" + wrong);
    }
}
