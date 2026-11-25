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
 * @summary The JVM protects the AVX state wherever an independent native probe finds that
 *          resuming from an exception loses it, by default and with the protection forced
 *          (ForceExceptionXStateProtection), and selects the mitigation: the scrub of the stale
 *          exception context header at the length the probe measures, or, without the scrub, no
 *          state that can be lost (UseAVX at most 2, MaxVectorSize at most 16, with a warning when
 *          that overrides the command line). APX is off where state can be lost.
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
        // The classification, the scrub and the cap are used only without AvoidLosableStateAtTraps.
        String[] args = new String[extra.length + 5];
        args[0] = "-XX:+UnlockDiagnosticVMOptions";
        args[1] = "-XX:-AvoidLosableStateAtTraps";
        args[2] = "-Xlog:os+cpu=info";
        System.arraycopy(extra, 0, args, 3, extra.length);
        args[extra.length + 3] = "-XX:+PrintFlagsFinal";
        args[extra.length + 4] = "-version";
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
            // The decision is made on Windows with AVX only.
            if (jvmLoses || jvmKeeps) {
                throw new RuntimeException("decision reported with UseAVX = " + useAVX);
            }
            expect(out, "ExceptionXStateLength", 0);
            return;
        }

        if (jvmLoses && jvmKeeps) {
            throw new RuntimeException("both decisions reported");
        }
        if (!jvmLoses && !jvmKeeps) {
            throw new RuntimeException("no decision reported");
        }
        // The oracle: the JVM protects the state wherever the native probe finds it lost. It may
        // protect it where the probe finds it kept: on a kernel it does not know to keep it.
        if (probe.known() && probe.loses() && !jvmLoses) {
            throw new RuntimeException("JVM reports \"" + KEEPS + "\" but the native probe found " + probe);
        }
        if (jvmLoses && probe.known() && !probe.loses()) {
            System.out.println("The JVM protects state that the native probe finds kept");
        }
        // With the protection forced, the JVM protects the state on every system.
        OutputAnalyzer forcedOut = flags("-XX:+ForceExceptionXStateProtection");
        forcedOut.shouldContain(LOSES);
        forcedOut.shouldNotContain(KEEPS);
        expect(forcedOut, "UseAPX", false);
        checkLength(forcedOut, probe);

        if (!jvmLoses) {
            expect(out, "ExceptionXStateLength", 0);
            return;
        }

        expect(out, "UseAPX", false);
        checkLength(out, probe);
        if (probe.loses() && probe.scrubWorks()) {
            // Where the scrub at the measured length works, the JVM's verified length is it.
            expect(out, "ExceptionXStateLength", probe.length());
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

    // A scrub is at the length the probe measures (the JVM's is verified the same way); without
    // one, the vectors are capped.
    static void checkLength(OutputAnalyzer out, ExceptionXState.Probe probe) {
        int length = intFlag(out, "ExceptionXStateLength");
        if (length == 0) {
            checkCapped(out);
        } else if (probe.known() && probe.length() > 0 && length != probe.length()) {
            throw new RuntimeException("ExceptionXStateLength = " + length + ", the native probe measured " + probe);
        }
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
