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
    // Every revision of every line from Windows 10 to Windows 11 23H2, examined or not.
    { 10240, -1, XS_K }, { 14393, -1, XS_K }, { 17763, -1, XS_K }, { 17763, 9479, XS_K }, { 17763, 9999, XS_K },
    { 18362, -1, XS_K }, { 19041, -1, XS_K }, { 20348, -1, XS_K }, { 20348, 5870, XS_K }, { 20349, -1, XS_K },
    { 22000, -1, XS_K }, { 22621, -1, XS_K }, { 22621, 7714, XS_K }, { 22631, -1, XS_K },
    // The builds between Windows 11 23H2 and 26100 were not examined: Windows Server 23H2 and the
    // Insider builds that led to 26100.
    { 22632, -1, XS_U }, { 22635, -1, XS_U }, { 23000, -1, XS_U }, { 25398, -1, XS_U }, { 25398, 1000, XS_U },
    { 26052, -1, XS_U }, { 26090, -1, XS_U }, { 26099, -1, XS_U }, { 26099, 9974, XS_U },
    // No valid build.
    { -1, -1, XS_U }, { 0, -1, XS_U }, { 9600, -1, XS_U }, { 10239, -1, XS_U }, { min_jint, -1, XS_U },
    { max_jint, -1, XS_U }
  };
  check_cases(cases, ARRAY_SIZE(cases));
}

TEST(ExceptionXStateKernels, build_26100) {
  static const ExceptionXStateCase cases[] = {
    // No revision, or below the oldest update.
    { 26100, -1, XS_U }, { 26100, 0, XS_U },
    // Updates with an affected kernel, and the revisions next to them, which are not updates.
    { 26100, 1, XS_L }, { 26100, 2, XS_U }, { 26100, 862, XS_U }, { 26100, 863, XS_L }, { 26100, 864, XS_U },
    { 26100, 4946, XS_L }, { 26100, 5000, XS_U }, { 26100, 6584, XS_L },
    // 26100.6588 (KB5068221) ships the affected kernel 6584.
    { 26100, 6585, XS_U }, { 26100, 6588, XS_L }, { 26100, 6724, XS_U },
    // Gated: protected whatever the feature configuration.
    { 26100, 6725, XS_G }, { 26100, 7000, XS_U }, { 26100, 7623, XS_G }, { 26100, 7705, XS_G },
    // 26100.7840 (KB5077181) ships the gated kernel 7824, whose revision is not the one of an update.
    { 26100, 7824, XS_U }, { 26100, 7825, XS_U }, { 26100, 7840, XS_G }, { 26100, 7919, XS_U },
    // Fixed: only the revisions of the updates. 26100.7922 (KB5077241) ships kernel 7920, 26100.8037
    // kernel 8036, 26100.9725 kernel 9711; the kernels' own revisions are not updates.
    { 26100, 7920, XS_U }, { 26100, 7921, XS_U }, { 26100, 7922, XS_K }, { 26100, 7923, XS_U },
    { 26100, 8036, XS_U }, { 26100, 8037, XS_K }, { 26100, 8039, XS_K }, { 26100, 9000, XS_U },
    { 26100, 9444, XS_U }, { 26100, 9445, XS_K }, { 26100, 9711, XS_K }, { 26100, 9725, XS_K },
    { 26100, 9818, XS_K }, { 26100, 9973, XS_U }, { 26100, 9974, XS_K },
    // Above the newest one: the kernel 10068 (26100.10075) that was not examined, and everything after it.
    { 26100, 9975, XS_U }, { 26100, 10067, XS_U }, { 26100, 10068, XS_U }, { 26100, 10075, XS_U },
    { 26100, 12974, XS_U }, { 26100, 29999, XS_U },
    // The Windows Server 2025 branch: only the examined revision, and it is gated.
    { 26100, 30000, XS_U }, { 26100, 32230, XS_U }, { 26100, 33827, XS_U }, { 26100, 33828, XS_G },
    { 26100, 33829, XS_U }, { 26100, 65535, XS_U }
  };
  check_cases(cases, ARRAY_SIZE(cases));
}

// A hotpatch update ships patch images only: at its revision runs the kernel of its baseline
// update, patched in memory, which was not examined. Every hotpatch revision of build 26100
// released until November 2026 (Windows 11 Enterprise and Windows Server 2025) protects the state,
// wherever it lies: among the affected, gated or fixed updates, or on the server branch.
TEST(ExceptionXStateKernels, hotpatch_revisions) {
  static const int hotpatches[] = {
    2240, 2528, 3107, 3403, 3981, 3983, 4270, 4851, 6508, 7092, 7093, 7392, 7634, 7781,
    // March 2026, on the gated baseline 7623, numbered between the fixed updates 7922 and 8037.
    7979, 7982, 7984, 7985,
    // On fixed baselines.
    8390, 8893, 9106, 9448, 9911,
    // Windows Server 2025 in 2026.
    32313, 32463, 32704, 32772, 33222, 33730
  };
  for (size_t i = 0; i < ARRAY_SIZE(hotpatches); i++) {
    EXPECT_EQ(XS_U, VM_Version::classify_exception_xstate_kernel(26100, hotpatches[i])) << hotpatches[i];
  }
}

// Only the revisions of the updates with a fixed kernel leave the state unprotected: 23 of build
// 26100 and 17 of build 28000, and no other revision that a kernel can report. The lists repeat the
// table, with the kernel each update ships where its revision differs, so they catch an edit of the
// table, not an error in its source.
TEST(ExceptionXStateKernels, keeps_only_listed_updates) {
  static const int keeps_26100[] = {
    7922 /* 7920 */, 8037 /* 8036 */, 8039 /* 8036 */, 8116 /* 8115 */, 8117, 8246, 8328, 8457, 8524 /* 8521 */,
    8655, 8737, 8875, 8894, 8973 /* 8972 */, 9168, 9278, 9445 /* 9444 */, 9457, 9550 /* 9549 */, 9711,
    9725 /* 9711 */, 9818, 9974
  };
  static const int keeps_28000[] = {
    1575, 1643, 1719, 1764 /* 1761 */, 1836 /* 1830 */, 2113, 2269, 2340 /* 2336 */, 2525, 2608 /* 2605 */, 2704,
    2804, 2954 /* 2952 */, 2956 /* 2952 */, 3086, 3231, 3365
  };
  size_t next_26100 = 0;
  size_t next_28000 = 0;
  for (int revision = -1; revision <= 70000; revision++) {
    bool listed_26100 = next_26100 < ARRAY_SIZE(keeps_26100) && keeps_26100[next_26100] == revision;
    bool listed_28000 = next_28000 < ARRAY_SIZE(keeps_28000) && keeps_28000[next_28000] == revision;
    EXPECT_EQ(listed_26100, VM_Version::classify_exception_xstate_kernel(26100, revision) == XS_K) << revision;
    EXPECT_EQ(listed_28000, VM_Version::classify_exception_xstate_kernel(28000, revision) == XS_K) << revision;
    next_26100 += listed_26100 ? 1 : 0;
    next_28000 += listed_28000 ? 1 : 0;
  }
  EXPECT_EQ(ARRAY_SIZE(keeps_26100), next_26100);
  EXPECT_EQ(ARRAY_SIZE(keeps_28000), next_28000);
  EXPECT_EQ(XS_U, VM_Version::classify_exception_xstate_kernel(26100, max_jint));
  EXPECT_EQ(XS_U, VM_Version::classify_exception_xstate_kernel(28000, max_jint));
  EXPECT_EQ(XS_U, VM_Version::classify_exception_xstate_kernel(26100, min_jint));
}

TEST(ExceptionXStateKernels, build_28000_and_later) {
  static const ExceptionXStateCase cases[] = {
    // Below the oldest update.
    { 28000, -1, XS_U }, { 28000, 0, XS_U }, { 28000, 1, XS_U }, { 28000, 1574, XS_U },
    // Updates with an examined kernel, and the revisions next to them.
    { 28000, 1575, XS_K }, { 28000, 1576, XS_U }, { 28000, 1761, XS_U }, { 28000, 1764, XS_K },
    // 28000.1836 (KB5083768) ships the examined kernel 1830; the kernels 1896 and 2179 were not examined.
    { 28000, 1830, XS_U }, { 28000, 1836, XS_K }, { 28000, 1895, XS_U }, { 28000, 1896, XS_U }, { 28000, 2112, XS_U },
    { 28000, 2113, XS_K }, { 28000, 2114, XS_U }, { 28000, 2179, XS_U }, { 28000, 2268, XS_U },
    { 28000, 2269, XS_K }, { 28000, 2336, XS_U }, { 28000, 2340, XS_K }, { 28000, 3365, XS_K },
    // Above the newest update.
    { 28000, 3366, XS_U }, { 28000, 3501, XS_U }, { 28000, 3620, XS_U }, { 28000, 6365, XS_U },
    { 28000, 30000, XS_U },
    // Lines that were not examined: between 26100 and 28000 (Insider builds), and later.
    { 26120, -1, XS_U }, { 26200, -1, XS_U }, { 26300, -1, XS_U }, { 26300, 9711, XS_U }, { 27000, 1, XS_U }, { 28001, 1, XS_U }, { 28001, -1, XS_U },
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
  EXPECT_EQ(26300, VM_Version::exception_xstate_kernel_line(26300));
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
  // The kernel reports a hotpatch revision (March 2026 on the gated January baseline, November
  // 2026 on the fixed October baseline), or a revision between two fixed updates that is no update.
  fake_version_revision = 7979;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_EQ(7979, d.revision);
  EXPECT_EQ(nullptr, d.reason);
  fake_version_revision = 9911;
  EXPECT_EQ(XS_U, decide(&d));
  fake_version_revision = 9000;
  EXPECT_EQ(XS_U, decide(&d));
  // The revision of a fixed kernel's file that is not the revision of an update.
  fake_version_revision = 7920;
  EXPECT_EQ(XS_U, decide(&d));
  fake_version_revision = 8036;
  EXPECT_EQ(XS_U, decide(&d));
  // Revisions no kernel reports.
  fake_version_revision = -1;
  EXPECT_EQ(XS_U, decide(&d));
  fake_version_revision = 0;
  EXPECT_EQ(XS_U, decide(&d));
  fake_version_revision = 65536;
  EXPECT_EQ(XS_U, decide(&d));
  fake_version_revision = max_jint;
  EXPECT_EQ(XS_U, decide(&d));
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
  // The same on 25H2 for a hotpatch revision (26200.9911).
  fake_version_build = 26200;
  fake_version_revision = 9911;
  EXPECT_EQ(XS_U, decide(&d));
  fake_version_revision = 9974;
  // 26300 is not the line: Insider builds of it carry revisions of their own. Not read for 26300,
  // and a 26100 or 26200 kernel whose version reports 26300 reports two builds.
  fake_running_build = 26300;
  fake_version_build = 26300;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("a build that was not examined", d.reason);
  EXPECT_EQ(3, version_reads);
  fake_running_build = 26200;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("the running kernel reports two builds", d.reason);
  fake_running_build = 26100;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("the running kernel reports two builds", d.reason);
  EXPECT_EQ(-1, d.revision);
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
  // The reverse: a 28000 kernel whose version reports 26100 with a revision fixed there.
  reset_fake_system();
  fake_running_build = 28000;
  fake_version_build = 26100;
  fake_version_revision = 9974;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("the running kernel reports two builds", d.reason);
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
  EXPECT_EQ(nullptr, d.reason);
  // Windows Server 23H2 and the Insider builds before 26100 were not examined: protected, without
  // reading the revision.
  fake_running_build = 25398;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("a build that was not examined", d.reason);
  fake_running_build = 26090;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("a build that was not examined", d.reason);
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

// Without a verification stub, no fault is taken for one of its traps, a fault at pc 0 included.
TEST(ExceptionXStateKernels, no_trap_without_stub) {
  EXPECT_FALSE(VM_Version::is_exception_xstate_segv_addr(nullptr));
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
