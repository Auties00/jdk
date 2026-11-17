/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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
 * @summary UseBranchLoopSafepointPolls is on and ImplicitNullChecks off by default exactly on
 *          Windows on processors with AVX-512, whatever UseAVX is, and both can be overridden.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.flagless
 * @library /test/lib
 * @run driver compiler.runtime.safepoints.TestBranchLoopSafepointPollsDefault
 */

package compiler.runtime.safepoints;

import jdk.test.lib.Platform;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class TestBranchLoopSafepointPollsDefault {
    static String flag(OutputAnalyzer out, String name) {
        Matcher m = Pattern.compile("\\s" + name + "\\s+=\\s+(\\S+)").matcher(out.getStdout());
        if (!m.find()) {
            throw new RuntimeException(name + " not printed");
        }
        return m.group(1);
    }

    static OutputAnalyzer flags(String... extra) throws Exception {
        String[] args = new String[extra.length + 3];
        args[0] = "-XX:+UnlockDiagnosticVMOptions";
        System.arraycopy(extra, 0, args, 1, extra.length);
        args[extra.length + 1] = "-XX:+PrintFlagsFinal";
        args[extra.length + 2] = "-version";
        OutputAnalyzer out = ProcessTools.executeTestJava(args);
        out.shouldHaveExitValue(0);
        return out;
    }

    public static void main(String[] args) throws Exception {
        OutputAnalyzer out = flags();
        int useAVX = Integer.parseInt(flag(out, "UseAVX"));
        // UseAVX=3 is kept only where the processor and the OS support AVX-512.
        boolean avx512 = Integer.parseInt(flag(flags("-XX:UseAVX=3"), "UseAVX")) == 3;
        boolean expected = Platform.isWindows() && avx512;
        String actual = flag(out, "UseBranchLoopSafepointPolls");
        if (!actual.equals(Boolean.toString(expected))) {
            throw new RuntimeException("UseBranchLoopSafepointPolls = " + actual + ", expected " + expected
                                       + " (UseAVX = " + useAVX + ")");
        }
        String nullChecks = flag(out, "ImplicitNullChecks");
        if (!nullChecks.equals(Boolean.toString(!expected))) {
            throw new RuntimeException("ImplicitNullChecks = " + nullChecks + ", expected " + !expected);
        }
        if (!flag(flags("-XX:+ImplicitNullChecks"), "ImplicitNullChecks").equals("true")) {
            throw new RuntimeException("cannot enable ImplicitNullChecks");
        }
        if (!flag(flags("-XX:+UseBranchLoopSafepointPolls"), "UseBranchLoopSafepointPolls").equals("true")) {
            throw new RuntimeException("cannot enable UseBranchLoopSafepointPolls");
        }
        if (!flag(flags("-XX:-UseBranchLoopSafepointPolls"), "UseBranchLoopSafepointPolls").equals("false")) {
            throw new RuntimeException("cannot disable UseBranchLoopSafepointPolls");
        }
        // A lower UseAVX does not change the default: the ymm state is lost too.
        if (!flag(flags("-XX:UseAVX=2"), "UseBranchLoopSafepointPolls").equals(Boolean.toString(expected))) {
            throw new RuntimeException("UseBranchLoopSafepointPolls with -XX:UseAVX=2 should be " + expected);
        }
    }
}
