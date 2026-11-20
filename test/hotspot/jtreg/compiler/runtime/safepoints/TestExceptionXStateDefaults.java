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
 * @summary Whether resuming from an exception can lose AVX state, as the JVM decides it from its
 *          table of Windows kernels or from its startup check, agrees with an independent native
 *          probe, both by default and with the startup check forced (ExceptionXStateSelfTest), and
 *          selects the mitigation: the scrub of the stale exception context header at the length
 *          the probe measures, or, without the scrub, no state that can be lost (UseAVX at most 2,
 *          MaxVectorSize at most 16, with a warning when that overrides the command line). APX is
 *          off where state can be lost.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.flagless
 * @library /test/lib /
 * @build compiler.runtime.safepoints.ExceptionXState
 * @run main/othervm/native compiler.runtime.safepoints.TestExceptionXStateDefaults
 */

package compiler.runtime.safepoints;

import jdk.test.lib.Platform;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class TestExceptionXStateDefaults {
    static final String LOSES = "Resuming from an exception can lose AVX state";
    static final String KEEPS = "Resuming from an exception keeps AVX state";

    static String flag(OutputAnalyzer out, String name) {
        Matcher m = Pattern.compile("\\s" + name + "\\s+=\\s+(\\S+)").matcher(out.getStdout());
        if (!m.find()) {
            throw new RuntimeException(name + " not printed");
        }
        return m.group(1);
    }

    static OutputAnalyzer flags(String... extra) throws Exception {
        String[] args = new String[extra.length + 4];
        args[0] = "-XX:+UnlockDiagnosticVMOptions";
        args[1] = "-Xlog:os+cpu=info";
        System.arraycopy(extra, 0, args, 2, extra.length);
        args[extra.length + 2] = "-XX:+PrintFlagsFinal";
        args[extra.length + 3] = "-version";
        OutputAnalyzer out = ProcessTools.executeTestJava(args);
        out.shouldHaveExitValue(0);
        return out;
    }

    static void expect(OutputAnalyzer out, String name, Object expected) {
        String actual = flag(out, name);
        if (!actual.equals(expected.toString())) {
            throw new RuntimeException(name + " = " + actual + ", expected " + expected);
        }
    }

    static int intFlag(OutputAnalyzer out, String name) {
        return Integer.parseInt(flag(out, name));
    }

    public static void main(String[] args) throws Exception {
        ExceptionXState.Probe probe = ExceptionXState.probe();
        System.out.println("Native probe: " + probe);

        OutputAnalyzer out = flags();
        int useAVX = intFlag(out, "UseAVX");
        boolean jvmLoses = out.getStdout().contains(LOSES);
        boolean jvmKeeps = out.getStdout().contains(KEEPS);

        if (!(Platform.isWindows() && useAVX > 0)) {
            // The check runs on Windows with AVX only.
            if (jvmLoses || jvmKeeps) {
                throw new RuntimeException("startup check ran with UseAVX = " + useAVX);
            }
            expect(out, "ExceptionXStateLength", 0);
            return;
        }

        if (jvmLoses && jvmKeeps) {
            throw new RuntimeException("startup check reported both results");
        }
        if (!jvmLoses && !jvmKeeps) {
            throw new RuntimeException("no decision reported");
        }
        // The oracle: the JVM must agree with the native probe whenever the probe can tell.
        if (probe.known() && probe.loses() != jvmLoses) {
            throw new RuntimeException("JVM reports \"" + (jvmLoses ? LOSES : KEEPS)
                                       + "\" but the native probe found " + probe);
        }
        // The same with the startup check forced, which must also agree with the table.
        OutputAnalyzer selfTest = flags("-XX:+ExceptionXStateSelfTest");
        boolean selfTestLoses = selfTest.getStdout().contains(LOSES);
        if (selfTestLoses == selfTest.getStdout().contains(KEEPS)) {
            throw new RuntimeException("startup check: no single decision reported");
        }
        if (probe.known() && probe.loses() != selfTestLoses) {
            throw new RuntimeException("startup check reports \"" + (selfTestLoses ? LOSES : KEEPS)
                                       + "\" but the native probe found " + probe);
        }
        selfTest.shouldNotContain("disagrees with the table");
        if (selfTestLoses && jvmLoses) {
            // The length of the table's formula is the one the startup check measures.
            expect(selfTest, "ExceptionXStateLength", flag(out, "ExceptionXStateLength"));
        }

        if (!jvmLoses) {
            expect(out, "ExceptionXStateLength", 0);
            return;
        }

        expect(out, "UseAPX", false);
        int length = intFlag(out, "ExceptionXStateLength");
        if (probe.loses() && probe.scrubWorks()) {
            // The JVM measures the same context length and protects its traps with the scrub.
            expect(out, "ExceptionXStateLength", probe.length());
        }
        if (length == 0) {
            checkCapped(out);
        }

        // Without the scrub, the JVM uses no state that can be lost.
        OutputAnalyzer noScrub = flags("-XX:-ScrubExceptionXStateHeader");
        expect(noScrub, "ExceptionXStateLength", 0);
        checkCapped(noScrub);

        // An explicit UseAVX=3 that the cap overrides is reported.
        if (useAVX == 3) {
            OutputAnalyzer explicit = flags("-XX:-ScrubExceptionXStateHeader", "-XX:UseAVX=3");
            expect(explicit, "UseAVX", 2);
            explicit.shouldContain("UseAVX=3 reduced to 2");
        }

        // Without AVX there is no state to lose, and no check.
        OutputAnalyzer noAvx = flags("-XX:UseAVX=0");
        noAvx.shouldNotContain(LOSES);
        expect(noAvx, "ExceptionXStateLength", 0);
    }

    static void checkCapped(OutputAnalyzer out) {
        if (intFlag(out, "UseAVX") > 2 || intFlag(out, "MaxVectorSize") > 16
                || flag(out, "UseAPX").equals("true")) {
            throw new RuntimeException("without the scrub: UseAVX = " + flag(out, "UseAVX")
                                       + ", MaxVectorSize = " + flag(out, "MaxVectorSize")
                                       + ", UseAPX = " + flag(out, "UseAPX"));
        }
    }
}
