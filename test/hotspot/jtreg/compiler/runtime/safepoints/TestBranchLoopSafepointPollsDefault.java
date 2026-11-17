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
 * @summary UseBranchLoopSafepointPolls is on and ImplicitNullChecks off by default only on
 *          Windows, only together and only when the startup check reports that resuming from
 *          an exception can lose AVX state; both can be overridden.
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
        OutputAnalyzer out = flags("-Xlog:os+cpu=info");
        int useAVX = Integer.parseInt(flag(out, "UseAVX"));
        // The check runs on Windows with AVX only, and logs its result.
        boolean expected = out.getStdout().contains("Resuming from an exception can lose AVX state");
        if (expected && !(Platform.isWindows() && useAVX > 0)) {
            throw new RuntimeException("startup check ran with UseAVX = " + useAVX);
        }
        if (Platform.isWindows() && useAVX > 0 && !expected
                && !out.getStdout().contains("Resuming from an exception keeps AVX state")) {
            throw new RuntimeException("startup check did not run");
        }
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
        if (expected) {
            // Without AVX there is no state to lose.
            if (!flag(flags("-XX:UseAVX=0"), "UseBranchLoopSafepointPolls").equals("false")) {
                throw new RuntimeException("UseBranchLoopSafepointPolls should be off with -XX:UseAVX=0");
            }
        }
    }
}
