// Review-only kernel for JDK-8365196 (review5): does a thread that used AMX need another L?
// Two threads run the same vector kernel after a native fill of 0x8000000000000000 below the
// stack; one of them first executes ldtilecfg + tilezero (AMX tile state in use). Run on an
// affected Windows Server 2025 host with AMX in XCR0.
import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorSpecies;

public class R5Amx {
    static final VectorSpecies<Double> S = DoubleVector.SPECIES_256;
    static final MethodHandle FILL, LDCFG, TZERO;
    static {
        Linker l = Linker.nativeLinker();
        SymbolLookup lib = SymbolLookup.libraryLookup(System.getProperty("r5.lib"), Arena.global());
        FILL = l.downcallHandle(lib.find("fill_below").orElseThrow(),
                FunctionDescriptor.ofVoid(ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));
        LDCFG = l.downcallHandle(lib.find("amx_ldtilecfg").orElseThrow(), FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
        TZERO = l.downcallHandle(lib.find("amx_tilezero0").orElseThrow(), FunctionDescriptor.ofVoid());
    }
    static volatile boolean stop;
    static volatile Object sink;

    static long kernel(double neg, Object[] refs) throws Throwable {
        FILL.invokeExact(4096L, Long.MIN_VALUE);
        DoubleVector v = DoubleVector.broadcast(S, neg);
        DoubleVector a0 = v, a1 = v, a2 = v, a3 = v;
        int mask = refs.length - 1, count = 0;
        for (int i = 0; i < 20_000; i++) {
            if (refs[i & mask] != null) count++;
            a0 = a0.add(v); a1 = a1.add(v); a2 = a2.add(v); a3 = a3.add(v);
        }
        sink = count;
        LongVector r = a0.reinterpretAsLongs().and(a1.reinterpretAsLongs()).and(a2.reinterpretAsLongs())
                         .and(a3.reinterpretAsLongs());
        for (int i = 0; i < 4; i++) if (r.lane(i) != Long.MIN_VALUE) return 1;
        return 0;
    }

    static long[] runIn(boolean amx, long millis) throws Exception {
        long[] res = new long[3];
        Thread t = new Thread(() -> {
            try {
                if (amx) {
                    try (Arena a = Arena.ofConfined()) {
                        MemorySegment cfg = a.allocate(64, 64);
                        cfg.set(ValueLayout.JAVA_BYTE, 0, (byte) 1);
                        cfg.set(ValueLayout.JAVA_SHORT_UNALIGNED, 16, (short) 64);
                        cfg.set(ValueLayout.JAVA_BYTE, 48, (byte) 16);
                        LDCFG.invokeExact(cfg);
                        TZERO.invokeExact();
                        res[2] = 1;
                    }
                }
                Object[] refs = new Object[1 << 12];
                for (int i = 0; i < refs.length; i++) refs[i] = new int[1 + (i & 7)];
                long end = System.nanoTime() + millis * 1_000_000L;
                while (System.nanoTime() < end) {
                    res[0] += kernel(-0.0, refs);
                    res[1]++;
                }
            } catch (Throwable e) {
                e.printStackTrace();
                res[2] = -1;
            }
        });
        t.start();
        t.join();
        return res;
    }

    public static void main(String[] args) throws Exception {
        Thread gc = new Thread(() -> {
            while (!stop) {
                for (int i = 0; i < 2000; i++) sink = new byte[256];
                System.gc();
                try { Thread.sleep(2); } catch (InterruptedException e) { return; }
            }
        });
        gc.setDaemon(true);
        gc.start();
        long ms = Long.parseLong(args.length > 0 ? args[0] : "8000");
        long[] plain = runIn(false, ms);
        long[] amx = runIn(true, ms);
        stop = true;
        System.out.println("R5AMX plain wrong=" + plain[0] + " calls=" + plain[1]);
        System.out.println("R5AMX amx wrong=" + amx[0] + " calls=" + amx[1] + " amx_ok=" + amx[2]);
    }
}
