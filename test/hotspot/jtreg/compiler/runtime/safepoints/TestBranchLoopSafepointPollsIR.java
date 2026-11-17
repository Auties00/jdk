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
 * @summary C2 emits loop safepoint polls as a poll word test and branch, with no polling page
 *          access, exactly when UseBranchLoopSafepointPolls is on, and with its platform default.
 * @requires os.arch == "amd64" | os.arch == "x86_64"
 * @requires vm.compiler2.enabled & vm.flavor != "zero"
 * @library /test/lib /
 * @run driver compiler.runtime.safepoints.TestBranchLoopSafepointPollsIR
 */

package compiler.runtime.safepoints;

import compiler.lib.ir_framework.*;

public class TestBranchLoopSafepointPollsIR {
    // Formats of safePoint_poll_branch and safePoint_poll_tls in x86.ad.
    static final String BRANCH_POLL = "testl\\s+rax, rax\\s+# Safepoint: poll site";
    static final String TRAP_POLL = "testl\\s+rax, \\[[^\\]]*\\]\\s+# Safepoint: poll for GC";

    static final class Node {
        Node next;
    }

    // Built in the test VM, which does not run main().
    static final Node LIST;
    static {
        Node n = null;
        for (int i = 0; i < 100; i++) {
            Node m = new Node();
            m.next = n;
            n = m;
        }
        LIST = n;
    }

    public static void main(String[] args) {
        TestFramework.run();
        TestFramework.runWithFlags("-XX:+UnlockDiagnosticVMOptions", "-XX:+UseBranchLoopSafepointPolls");
        TestFramework.runWithFlags("-XX:+UnlockDiagnosticVMOptions", "-XX:-UseBranchLoopSafepointPolls");
    }

    // A non-counted loop keeps its poll.
    @Test
    @IR(counts = {BRANCH_POLL, ">= 1"}, failOn = {TRAP_POLL},
        phase = CompilePhase.PRINT_OPTO_ASSEMBLY, applyIf = {"UseBranchLoopSafepointPolls", "true"})
    @IR(counts = {TRAP_POLL, ">= 1"}, failOn = {BRANCH_POLL},
        phase = CompilePhase.PRINT_OPTO_ASSEMBLY, applyIf = {"UseBranchLoopSafepointPolls", "false"})
    static int listLength() {
        int c = 0;
        for (Node n = LIST; n != null; n = n.next) {
            c++;
        }
        return c;
    }

    @Run(test = "listLength")
    static void runListLength() {
        if (listLength() != 100) {
            throw new RuntimeException("wrong length");
        }
    }
}
