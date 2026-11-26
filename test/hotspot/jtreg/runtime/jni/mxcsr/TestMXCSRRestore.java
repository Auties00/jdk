/*
 * Copyright (c) 2026, Alessandro Autiero. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * @test id=interpreted
 * @bug 8365196
 * @summary With AvoidLosableStateAtTraps, Java code runs with the standard MXCSR after native
 *          code unmasked floating point exceptions: on return from JNI, from FFM downcalls
 *          (critical ones included), and inside an FFM upcall.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @run main/othervm/native -XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps -Xint
 *      --enable-native-access=ALL-UNNAMED runtime.jni.mxcsr.TestMXCSRRestore
 */

/*
 * @test id=compiled
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @run main/othervm/native -XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps -Xbatch
 *      -XX:-TieredCompilation --enable-native-access=ALL-UNNAMED runtime.jni.mxcsr.TestMXCSRRestore
 */

/*
 * @test id=c1
 * @bug 8365196
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @run main/othervm/native -XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps -Xbatch
 *      -XX:TieredStopAtLevel=1 --enable-native-access=ALL-UNNAMED runtime.jni.mxcsr.TestMXCSRRestore
 */

/*
 * @test id=statusbits
 * @bug 8365196
 * @summary The standard MXCSR with its status bits set (EnableX86ECoreOpts).
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @run main/othervm/native -XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps -XX:+EnableX86ECoreOpts
 *      -Xbatch -XX:-TieredCompilation --enable-native-access=ALL-UNNAMED runtime.jni.mxcsr.TestMXCSRRestore
 */

package runtime.jni.mxcsr;

import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

public class TestMXCSRRestore {
    static {
        System.loadLibrary("MXCSRRestore");
    }

    // The control bits: the exception masks, the rounding mode, flush-to-zero and
    // denormals-are-zero. The status bits (0x3F) may be set by any computation.
    static final int CONTROL = ~0x3F;
    // The standard MXCSR of Java code: every exception masked, round to nearest.
    static final int STANDARD = 0x1F80;

    static native int jniCurrent();
    static native void jniUnmask();

    static final Linker LINKER = Linker.nativeLinker();
    static final SymbolLookup LOOKUP = SymbolLookup.loaderLookup();
    static final MethodHandle CURRENT = downcall("mxcsr_current", FunctionDescriptor.of(ValueLayout.JAVA_INT), false);
    static final MethodHandle CURRENT_CRITICAL = downcall("mxcsr_current", FunctionDescriptor.of(ValueLayout.JAVA_INT), true);
    static final MethodHandle UNMASK = downcall("mxcsr_unmask", FunctionDescriptor.ofVoid(), false);
    static final MethodHandle UNMASK_CRITICAL = downcall("mxcsr_unmask", FunctionDescriptor.ofVoid(), true);
    static final MethodHandle CALL_WITH =
        downcall("mxcsr_call_with", FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                                                          ValueLayout.JAVA_INT, ValueLayout.JAVA_INT), false);

    // The divide-by-zero and invalid exception masks.
    static final int TWO_MASKS = 0x0280;
    // Rounding toward zero.
    static final int ROUND_TO_ZERO = 0x6000;

    static volatile int seenInUpcall;

    static MethodHandle downcall(String name, FunctionDescriptor descriptor, boolean critical) {
        MemorySegment symbol = LOOKUP.find(name).orElseThrow();
        return critical ? LINKER.downcallHandle(symbol, descriptor, Linker.Option.critical(false))
                        : LINKER.downcallHandle(symbol, descriptor);
    }

    // An upcall target may not declare checked exceptions.
    static void upcall() {
        // Java code entered from native code with the exceptions unmasked: the MXCSR it runs with
        // is the one the next native call starts with.
        try {
            seenInUpcall = (int)CURRENT_CRITICAL.invokeExact();
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    static void expectStandard(String where, int mxcsr) {
        if ((mxcsr & CONTROL) != STANDARD) {
            throw new RuntimeException(where + ": Java code ran with MXCSR 0x" + Integer.toHexString(mxcsr));
        }
    }

    static int divide(int iterations, double divisor) {
        // Divisions by zero that would trap if the exception were unmasked.
        int infinities = 0;
        for (int i = 0; i < iterations; i++) {
            if (Double.isInfinite((i + 1) / divisor)) {
                infinities++;
            }
        }
        return infinities;
    }

    static void callUpcall(MemorySegment stub, int clear, int set) throws Throwable {
        seenInUpcall = -1;
        int after = (int)CALL_WITH.invokeExact(stub, clear, set);
        String caller = "a caller with 0x" + Integer.toHexString(clear) + " cleared and 0x" + Integer.toHexString(set) + " set";
        expectStandard("in an FFM upcall from " + caller, seenInUpcall);
        if ((after & CONTROL) != ((STANDARD & ~clear) | set)) {
            throw new RuntimeException("the upcall did not give " + caller + " back its MXCSR: 0x" +
                                       Integer.toHexString(after));
        }
        if (divide(4, 0.0) != 4) {
            throw new RuntimeException("1 / 0.0 is not infinite");
        }
    }

    public static void main(String[] args) throws Throwable {
        MemorySegment stub = LINKER.upcallStub(
            MethodHandles.lookup().findStatic(TestMXCSRRestore.class, "upcall", MethodType.methodType(void.class)),
            FunctionDescriptor.ofVoid(), java.lang.foreign.Arena.global());
        for (int round = 0; round < 20_000; round++) {
            jniUnmask();
            expectStandard("after a JNI call", jniCurrent());
            if (divide(4, 0.0) != 4) {
                throw new RuntimeException("1 / 0.0 is not infinite");
            }

            UNMASK.invokeExact();
            expectStandard("after an FFM downcall", (int)CURRENT.invokeExact());

            UNMASK_CRITICAL.invokeExact();
            expectStandard("after a critical FFM downcall", (int)CURRENT_CRITICAL.invokeExact());

            // A caller with the standard control bits (the upcall stub loads and restores nothing),
            // one with exceptions unmasked, and one with every exception masked but another
            // rounding mode: each gets its own control bits back, and Java code never sees them.
            callUpcall(stub, 0, 0);
            callUpcall(stub, TWO_MASKS, 0);
            callUpcall(stub, 0, ROUND_TO_ZERO);
            callUpcall(stub, TWO_MASKS, ROUND_TO_ZERO);
        }
    }
}
