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

#if defined(X86) && defined(_LP64) && defined(_WINDOWS) && !defined(ZERO)

#include "runtime/os.hpp"
#include "runtime/vm_version.hpp"
#include "unittest.hpp"

// The table of Windows kernels that decides whether resuming from an exception can lose AVX state
// (JDK-8365196), at every boundary of every rule. Only keeps leaves the state unprotected.

static const VM_Version::ExceptionXStateKernel XS_U = VM_Version::exception_xstate_kernel_unknown;
static const VM_Version::ExceptionXStateKernel XS_K = VM_Version::exception_xstate_kernel_keeps;
static const VM_Version::ExceptionXStateKernel XS_L = VM_Version::exception_xstate_kernel_loses;
static const VM_Version::ExceptionXStateKernel XS_G = VM_Version::exception_xstate_kernel_gated;

struct ExceptionXStateCase {
  int line;
  int revision;
  VM_Version::ExceptionXStateKernel kernel;
};

static void check_cases(const ExceptionXStateCase* cases, size_t count) {
  for (size_t i = 0; i < count; i++) {
    const ExceptionXStateCase& c = cases[i];
    EXPECT_EQ(c.kernel, VM_Version::classify_exception_xstate_kernel(c.line, c.revision))
        << c.line << "." << c.revision;
  }
}

TEST(ExceptionXStateKernels, lines_before_26100) {
  static const ExceptionXStateCase cases[] = {
    // Every revision of every line from Windows 10 to 26100, examined or not.
    { 10240, -1, XS_K }, { 14393, -1, XS_K }, { 17763, -1, XS_K }, { 17763, 9479, XS_K }, { 17763, 9999, XS_K },
    { 18362, -1, XS_K }, { 19041, -1, XS_K }, { 20348, -1, XS_K }, { 20348, 5870, XS_K }, { 20349, -1, XS_K },
    { 22000, -1, XS_K }, { 22621, -1, XS_K }, { 25398, -1, XS_K }, { 26099, -1, XS_K },
    // No valid build.
    { -1, -1, XS_U }, { 0, -1, XS_U }, { 9600, -1, XS_U }, { 10239, -1, XS_U }
  };
  check_cases(cases, ARRAY_SIZE(cases));
}

TEST(ExceptionXStateKernels, build_26100) {
  static const ExceptionXStateCase cases[] = {
    // No revision, or below the oldest examined.
    { 26100, -1, XS_U }, { 26100, 0, XS_U },
    // Affected, examined and between examined.
    { 26100, 1, XS_L }, { 26100, 2, XS_L }, { 26100, 4946, XS_L }, { 26100, 5000, XS_L }, { 26100, 6584, XS_L },
    // The gap to the gated class: 26100.6588 (KB5068221) runs kernel 6584.
    { 26100, 6585, XS_U }, { 26100, 6588, XS_U }, { 26100, 6724, XS_U },
    // Gated: protected whatever the feature configuration.
    { 26100, 6725, XS_G }, { 26100, 7000, XS_G }, { 26100, 7824, XS_G },
    // The gap to the fixed class: 26100.7840 (KB5077181) runs the gated kernel 7824.
    { 26100, 7825, XS_U }, { 26100, 7840, XS_U }, { 26100, 7919, XS_U },
    // Fixed: 7920 to 9974, 26100.7922 (KB5077241) runs kernel 7920, 26100.9725 kernel 9711.
    { 26100, 7920, XS_K }, { 26100, 7922, XS_K }, { 26100, 9000, XS_K }, { 26100, 9711, XS_K }, { 26100, 9725, XS_K },
    { 26100, 9974, XS_K },
    // Above the newest fixed one: the kernel 10068 that was not examined, and everything after it.
    { 26100, 9975, XS_U }, { 26100, 10067, XS_U }, { 26100, 10068, XS_U }, { 26100, 10075, XS_U },
    { 26100, 12974, XS_U }, { 26100, 29999, XS_U },
    // The Windows Server 2025 branch: only the examined revision, and it is gated.
    { 26100, 30000, XS_U }, { 26100, 32230, XS_U }, { 26100, 33827, XS_U }, { 26100, 33828, XS_G },
    { 26100, 33829, XS_U }, { 26100, 65535, XS_U }
  };
  check_cases(cases, ARRAY_SIZE(cases));
}

TEST(ExceptionXStateKernels, build_28000_and_later) {
  static const ExceptionXStateCase cases[] = {
    // Below the oldest examined revision.
    { 28000, -1, XS_U }, { 28000, 0, XS_U }, { 28000, 1, XS_U }, { 28000, 1574, XS_U },
    // Examined and between examined.
    { 28000, 1575, XS_K }, { 28000, 1576, XS_K }, { 28000, 1764, XS_K }, { 28000, 1830, XS_K },
    // Next to the kernels that were not examined, 1896 and 2179.
    { 28000, 1836, XS_U }, { 28000, 1895, XS_U }, { 28000, 1896, XS_U }, { 28000, 2112, XS_U },
    { 28000, 2113, XS_K }, { 28000, 2114, XS_U }, { 28000, 2179, XS_U }, { 28000, 2268, XS_U },
    { 28000, 2269, XS_K }, { 28000, 2340, XS_K }, { 28000, 3365, XS_K },
    // Above the newest examined revision.
    { 28000, 3366, XS_U }, { 28000, 3501, XS_U }, { 28000, 3620, XS_U }, { 28000, 6365, XS_U },
    { 28000, 30000, XS_U },
    // Lines that were not examined: between 26100 and 28000 (Insider builds), and later.
    { 26120, -1, XS_U }, { 26200, -1, XS_U }, { 27000, 1, XS_U }, { 28001, 1, XS_U }, { 28001, -1, XS_U },
    { 29000, 1, XS_U }, { 30000, 1, XS_U }
  };
  check_cases(cases, ARRAY_SIZE(cases));
}

TEST(ExceptionXStateKernels, needs_revision) {
  EXPECT_TRUE(VM_Version::exception_xstate_kernel_needs_revision(26100));
  EXPECT_TRUE(VM_Version::exception_xstate_kernel_needs_revision(28000));
  EXPECT_FALSE(VM_Version::exception_xstate_kernel_needs_revision(20348));
  EXPECT_FALSE(VM_Version::exception_xstate_kernel_needs_revision(22621));
  EXPECT_FALSE(VM_Version::exception_xstate_kernel_needs_revision(26200));
  EXPECT_FALSE(VM_Version::exception_xstate_kernel_needs_revision(28001));
}

TEST(ExceptionXStateKernels, kernel_line) {
  EXPECT_EQ(18362, VM_Version::exception_xstate_kernel_line(18363));
  EXPECT_EQ(19041, VM_Version::exception_xstate_kernel_line(19041));
  EXPECT_EQ(19041, VM_Version::exception_xstate_kernel_line(19042));
  EXPECT_EQ(19041, VM_Version::exception_xstate_kernel_line(19045));
  EXPECT_EQ(19046, VM_Version::exception_xstate_kernel_line(19046));
  EXPECT_EQ(20348, VM_Version::exception_xstate_kernel_line(20348));
  EXPECT_EQ(22621, VM_Version::exception_xstate_kernel_line(22631));
  EXPECT_EQ(26100, VM_Version::exception_xstate_kernel_line(26100));
  EXPECT_EQ(26100, VM_Version::exception_xstate_kernel_line(26200));
  EXPECT_EQ(26100, VM_Version::exception_xstate_kernel_line(26300));
  EXPECT_EQ(26120, VM_Version::exception_xstate_kernel_line(26120));
  EXPECT_EQ(26220, VM_Version::exception_xstate_kernel_line(26220));
  EXPECT_EQ(28000, VM_Version::exception_xstate_kernel_line(28000));
  EXPECT_EQ(-1, VM_Version::exception_xstate_kernel_line(-1));
}

// A system whose sources the test sets, and which counts the reads the decision makes.
static bool fake_native;
static int fake_running_build;
static bool fake_version_ok;
static int fake_version_build;
static int fake_version_revision;
static int build_reads;
static int version_reads;

static bool fake_runs_natively() { return fake_native; }
static int fake_running_build_fn() {
  build_reads++;
  return fake_running_build;
}
static bool fake_running_version(int* build, int* revision) {
  version_reads++;
  if (!fake_version_ok) {
    return false;
  }
  *build = fake_version_build;
  *revision = fake_version_revision;
  return true;
}

static const VM_Version::ExceptionXStateSystem fake_system = {
  fake_runs_natively, fake_running_build_fn, fake_running_version
};

// A native Windows Server 2025 host with the runners' gated kernel.
static void reset_fake_system() {
  fake_native = true;
  fake_running_build = 26100;
  fake_version_ok = true;
  fake_version_build = 26100;
  fake_version_revision = 33828;
  build_reads = 0;
  version_reads = 0;
}

static VM_Version::ExceptionXStateKernel decide(VM_Version::ExceptionXStateDecision* decision) {
  return VM_Version::classify_exception_xstate_system(&fake_system, decision);
}

TEST(ExceptionXStateKernels, system_26100) {
  VM_Version::ExceptionXStateDecision d;
  // The runners' Windows Server 2025 kernel, also in a Windows Server 2022 or 2025 container.
  reset_fake_system();
  EXPECT_EQ(XS_G, decide(&d));
  EXPECT_EQ(26100, d.running_build);
  EXPECT_EQ(26100, d.line);
  EXPECT_EQ(33828, d.revision);
  EXPECT_EQ(nullptr, d.reason);
  EXPECT_EQ(1, version_reads);
  // A gated, an affected and a fixed client revision.
  reset_fake_system();
  fake_version_revision = 7705;
  EXPECT_EQ(XS_G, decide(&d));
  fake_version_revision = 4946;
  EXPECT_EQ(XS_L, decide(&d));
  fake_version_revision = 9711;
  EXPECT_EQ(XS_K, decide(&d));
  EXPECT_EQ(9711, d.revision);
  // A revision that was not seen: above the newest examined one.
  fake_version_revision = 10075;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_EQ(10075, d.revision);
  // Windows 11 25H2: both sources report 26200, or the version reports the line.
  reset_fake_system();
  fake_running_build = 26200;
  fake_version_build = 26200;
  fake_version_revision = 9974;
  EXPECT_EQ(XS_K, decide(&d));
  EXPECT_EQ(26100, d.line);
  EXPECT_EQ(26200, d.running_build);
  fake_version_build = 26100;
  EXPECT_EQ(XS_K, decide(&d));
  fake_running_build = 26300;
  EXPECT_EQ(XS_K, decide(&d));
}

TEST(ExceptionXStateKernels, system_failures) {
  VM_Version::ExceptionXStateDecision d;
  // The revision cannot be read: protected.
  reset_fake_system();
  fake_version_revision = 9711;
  fake_version_ok = false;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_EQ(-1, d.revision);
  EXPECT_STREQ("the running kernel's revision could not be read", d.reason);
  // The two sources report different lines: protected.
  reset_fake_system();
  fake_version_revision = 9711;
  fake_version_build = 20348;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_EQ(-1, d.revision);
  EXPECT_STREQ("the running kernel reports two builds", d.reason);
  fake_version_build = 28000;
  EXPECT_EQ(XS_U, decide(&d));
  // Emulated: protected, nothing else read.
  reset_fake_system();
  fake_native = false;
  fake_version_revision = 9711;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("emulated, or not known to run natively", d.reason);
  EXPECT_EQ(0, build_reads);
  EXPECT_EQ(0, version_reads);
  EXPECT_EQ(-1, d.running_build);
  // A kernel too old to report its build.
  reset_fake_system();
  fake_running_build = 0;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("a build that was not examined", d.reason);
  EXPECT_EQ(0, version_reads);
}

TEST(ExceptionXStateKernels, system_other_lines) {
  VM_Version::ExceptionXStateDecision d;
  // A line before 26100 is decided by the build alone, also in a container on a newer host: never
  // reads the revision.
  reset_fake_system();
  fake_running_build = 20348;
  EXPECT_EQ(XS_K, decide(&d));
  EXPECT_EQ(0, version_reads);
  EXPECT_EQ(-1, d.revision);
  EXPECT_EQ(nullptr, d.reason);
  fake_running_build = 22631;
  EXPECT_EQ(XS_K, decide(&d));
  EXPECT_EQ(22621, d.line);
  fake_running_build = 25398;
  EXPECT_EQ(XS_K, decide(&d));
  EXPECT_EQ(0, version_reads);
  // 28000.
  reset_fake_system();
  fake_running_build = 28000;
  fake_version_build = 28000;
  fake_version_revision = 1000;
  EXPECT_EQ(XS_U, decide(&d));
  fake_version_revision = 3365;
  EXPECT_EQ(XS_K, decide(&d));
  fake_version_revision = 3620;
  EXPECT_EQ(XS_U, decide(&d));
  // Later builds, and builds between the lines: protected without reading the revision.
  reset_fake_system();
  fake_running_build = 28001;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("a build that was not examined", d.reason);
  fake_running_build = 26220;
  EXPECT_EQ(XS_U, decide(&d));
  fake_running_build = 30000;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_EQ(0, version_reads);
}

// The sources of a real system: the running kernel reports one line through both.
TEST(ExceptionXStateKernels, this_system) {
  int running_build = os::win32::running_kernel_build();
  EXPECT_GE(running_build, 10240);
  int build = -1;
  int revision = -1;
  if (os::win32::running_kernel_version(&build, &revision)) {
    EXPECT_EQ(VM_Version::exception_xstate_kernel_line(running_build), VM_Version::exception_xstate_kernel_line(build));
    EXPECT_GE(revision, 0);
  }
  VM_Version::ExceptionXStateDecision d;
  VM_Version::ExceptionXStateSystem system = {
    os::win32::runs_natively, os::win32::running_kernel_build, os::win32::running_kernel_version
  };
  VM_Version::ExceptionXStateKernel kernel = VM_Version::classify_exception_xstate_system(&system, &d);
  EXPECT_TRUE(kernel == XS_K || kernel == XS_L || kernel == XS_G || kernel == XS_U);
  if (kernel == XS_K) {
    EXPECT_EQ(nullptr, d.reason);
  }
}

#endif
