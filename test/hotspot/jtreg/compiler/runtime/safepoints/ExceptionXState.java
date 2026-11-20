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

package compiler.runtime.safepoints;

/**
 * JDK-8365196: native helpers, independent of the JVM's own startup check (libExceptionXState.c).
 */
public class ExceptionXState {
    static {
        System.loadLibrary("ExceptionXState");
    }

    /** Result of {@link #probe()}. */
    public record Probe(int status, int length, int scrubbed) {
        /** Whether resuming from an exception loses AVX state on this system. */
        public boolean loses() {
            return status == 1;
        }

        /** Whether the probe could tell. */
        public boolean known() {
            return status >= 0;
        }

        /** Whether clearing the stale header at the measured length avoids the loss. */
        public boolean scrubWorks() {
            return scrubbed == 1;
        }

        @Override
        public String toString() {
            return "status=" + status + " L=" + length + " scrubbed=" + scrubbed;
        }
    }

    /**
     * Takes access violations with 0x8000000000000000 below the stack pointer and tells whether
     * the exception context lost the AVX component, the length of its extended state, and whether
     * clearing the stale header at that length avoids the loss. Status -1 off Windows x64, or
     * without AVX.
     */
    public static Probe probe() {
        long[] r = probe0();
        return new Probe((int) r[0], (int) r[1], (int) r[2]);
    }

    private static native long[] probe0();

    /**
     * Writes 0x8000000000000000 into the 32 KiB below the stack pointer of the calling frame.
     */
    public static native void fillBelowStack();
}
