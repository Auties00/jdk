import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;

/**
 * Runs xstate_block_probe (built as a DLL) inside a JVM process: java ProbeHost <dll> "<probe args>" [load]
 * With "load", a Java thread runs a SuperWord float loop (zmm in use) during the probe, so the JVM keeps
 * compiling and, with -XX:+SafepointALot, keeps taking polling-page traps.
 */
public final class ProbeHost {
    static volatile boolean stop;

    public static void main(String[] args) throws Throwable {
        Thread load = null;
        if (args.length > 2 && args[2].equals("load")) {
            load = new Thread(() -> {
                float[] a = new float[4096];
                float s = 0;
                while (!stop) {
                    for (int i = 0; i < a.length; i++) a[i] = a[i] * 0.5f + 15.0f;
                    s += a[17];
                }
                System.out.println("load done " + (s != 0));
            });
            load.start();
        }
        SymbolLookup lib = SymbolLookup.libraryLookup(Path.of(args[0]), Arena.global());
        MethodHandle entry = Linker.nativeLinker().downcallHandle(lib.find("probe_entry").orElseThrow(),
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
        int code;
        try (Arena arena = Arena.ofConfined()) {
            code = (int) entry.invokeExact(arena.allocateFrom(args[1]));
        }
        stop = true;
        if (load != null) load.join();
        System.out.println("probe exit " + code);
        System.exit(code);
    }
}
