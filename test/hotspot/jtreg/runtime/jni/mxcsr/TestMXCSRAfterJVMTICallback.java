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
 * @test
 * @bug 8365196
 * @summary With AvoidLosableStateAtTraps, compiled code computes with the standard MXCSR after a
 *          JVMTI event callback, posted from a runtime call of that code, changed the rounding mode:
 *          the VM loads the standard MXCSR when native code it called itself returns.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.jvmti & vm.compiler2.enabled
 * @library /test/lib
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm/native -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *      -XX:+AvoidLosableStateAtTraps -Xbatch -XX:-TieredCompilation
 *      -XX:CompileCommand=dontinline,runtime.jni.mxcsr.TestMXCSRAfterJVMTICallback::step
 *      -agentlib:MXCSRJvmti runtime.jni.mxcsr.TestMXCSRAfterJVMTICallback
 */

package runtime.jni.mxcsr;

import java.lang.reflect.Method;

import jdk.test.whitebox.WhiteBox;

public class TestMXCSRAfterJVMTICallback {
    static final WhiteBox WB = WhiteBox.getWhiteBox();

    // 1 + 3/4 ulp: 1 + ulp when rounding to nearest, 1 when rounding toward zero.
    static final double ADDEND = 0x1.8p-53;
    static final double NEAREST = 0x1.0000000000001p0;

    static volatile Object sink;

    static native boolean sample(Thread thread, boolean on);
    static native int callbacks();

    // The allocation is sampled: its slow path, a runtime call of the compiled code, posts the event,
    // and the callback returns with rounding toward zero.
    static double step(double x, double y) {
        sink = new Object();
        return x + y;
    }

    public static void main(String[] args) throws Exception {
        Method step = TestMXCSRAfterJVMTICallback.class.getDeclaredMethod("step", double.class, double.class);
        for (int i = 0; i < 20_000; i++) {
            if (step(1.0, ADDEND) != NEAREST) {
                throw new RuntimeException("1 + 3/4 ulp is not 1 + ulp before any callback");
            }
        }
        if (!WB.isMethodCompiled(step)) {
            throw new RuntimeException("step is not compiled");
        }

        if (!sample(Thread.currentThread(), true)) {
            throw new RuntimeException("could not sample allocations");
        }
        double wrong = Double.NaN;
        int at = -1;
        for (int i = 0; i < 20_000 && at < 0; i++) {
            double sum = step(1.0, ADDEND);
            if (sum != NEAREST) {
                wrong = sum;
                at = i;
            }
        }
        boolean compiled = WB.isMethodCompiled(step);
        sample(Thread.currentThread(), false);

        if (at >= 0) {
            throw new RuntimeException("compiled code ran with the rounding mode of a JVMTI callback: call " + at +
                                       " returned " + Double.toHexString(wrong) + " after " + callbacks() + " callbacks");
        }
        if (callbacks() == 0) {
            throw new RuntimeException("no allocation was sampled");
        }
        if (!compiled) {
            throw new RuntimeException("step was deoptimized");
        }
        System.out.println("callbacks: " + callbacks());
    }
}
