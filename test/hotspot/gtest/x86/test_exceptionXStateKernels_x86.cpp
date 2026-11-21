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

#include "runtime/vm_version.hpp"
#include "unittest.hpp"

// The table of Windows kernels that decides whether resuming from an exception can lose AVX state
// (JDK-8365196), at every boundary of every rule.

static const VM_Version::ExceptionXStateKernel XS_U = VM_Version::exception_xstate_kernel_unknown;
static const VM_Version::ExceptionXStateKernel XS_K = VM_Version::exception_xstate_kernel_keeps;
static const VM_Version::ExceptionXStateKernel XS_L = VM_Version::exception_xstate_kernel_loses;
static const VM_Version::ExceptionXStateKernel XS_G = VM_Version::exception_xstate_kernel_gated;

struct ExceptionXStateCase {
  int build;
  int revision;
  VM_Version::ExceptionXStateKernel client;
  VM_Version::ExceptionXStateKernel server;
};

static void check_cases(const ExceptionXStateCase* cases, size_t count) {
  for (size_t i = 0; i < count; i++) {
    const ExceptionXStateCase& c = cases[i];
    EXPECT_EQ(c.client, VM_Version::classify_exception_xstate_kernel(c.build, c.revision, false))
        << "client " << c.build << "." << c.revision;
    EXPECT_EQ(c.server, VM_Version::classify_exception_xstate_kernel(c.build, c.revision, true))
        << "server " << c.build << "." << c.revision;
  }
}

TEST(ExceptionXStateKernels, lines_before_26100) {
  static const ExceptionXStateCase cases[] = {
    // Every revision of the lines whose routine clears the header, examined or not.
    { 17763, -1, XS_K, XS_K }, { 17763, 1, XS_K, XS_K }, { 17763, 9479, XS_K, XS_K }, { 17763, 9999, XS_K, XS_K },
    { 18362, -1, XS_K, XS_K }, { 19041, -1, XS_K, XS_K }, { 19041, 7844, XS_K, XS_K },
    { 20348, -1, XS_K, XS_K }, { 20348, 5866, XS_K, XS_K }, { 20348, 5870, XS_K, XS_K },
    { 22000, -1, XS_K, XS_K }, { 22621, -1, XS_K, XS_K }, { 22621, 7714, XS_K, XS_K },
    // Builds that were not examined.
    { 0, -1, XS_U, XS_U }, { 10240, -1, XS_U, XS_U }, { 14393, -1, XS_U, XS_U }, { 17134, -1, XS_U, XS_U },
    { 18363, -1, XS_U, XS_U }, { 19045, -1, XS_U, XS_U }, { 20349, -1, XS_U, XS_U }, { 22631, -1, XS_U, XS_U },
    { 25398, -1, XS_U, XS_U }, { 26099, -1, XS_U, XS_U }
  };
  check_cases(cases, ARRAY_SIZE(cases));
}

TEST(ExceptionXStateKernels, build_26100) {
  static const ExceptionXStateCase cases[] = {
    // No revision, or below the oldest examined.
    { 26100, -1, XS_U, XS_U }, { 26100, 0, XS_U, XS_U },
    // Affected, examined and between examined.
    { 26100, 1, XS_L, XS_L }, { 26100, 2, XS_L, XS_L }, { 26100, 4946, XS_L, XS_L }, { 26100, 5000, XS_L, XS_L }, { 26100, 6584, XS_L, XS_L },
    // The gap to the gated class.
    { 26100, 6585, XS_U, XS_U }, { 26100, 6724, XS_U, XS_U },
    // Gated.
    { 26100, 6725, XS_G, XS_G }, { 26100, 7000, XS_G, XS_G }, { 26100, 7824, XS_G, XS_G },
    // The gap to the fixed class.
    { 26100, 7825, XS_U, XS_U }, { 26100, 7919, XS_U, XS_U },
    // Fixed.
    { 26100, 7920, XS_K, XS_K }, { 26100, 9000, XS_K, XS_K }, { 26100, 9711, XS_K, XS_K }, { 26100, 9974, XS_K, XS_K },
    // Above every examined client revision: client installations only.
    { 26100, 9975, XS_K, XS_U }, { 26100, 10068, XS_K, XS_U }, { 26100, 12974, XS_K, XS_U },
    // Too far above it.
    { 26100, 12975, XS_U, XS_U }, { 26100, 29999, XS_U, XS_U },
    // The hotpatch branch: only the examined revision.
    { 26100, 30000, XS_U, XS_U }, { 26100, 32313, XS_U, XS_U }, { 26100, 33827, XS_U, XS_U },
    { 26100, 33828, XS_G, XS_G }, { 26100, 33829, XS_U, XS_U }, { 26100, 65535, XS_U, XS_U }
  };
  check_cases(cases, ARRAY_SIZE(cases));
}

TEST(ExceptionXStateKernels, build_28000_and_later) {
  static const ExceptionXStateCase cases[] = {
    // Below the oldest examined revision.
    { 28000, -1, XS_U, XS_U }, { 28000, 0, XS_U, XS_U }, { 28000, 1, XS_U, XS_U }, { 28000, 1574, XS_U, XS_U },
    // Examined and between examined.
    { 28000, 1575, XS_K, XS_K }, { 28000, 1576, XS_K, XS_K }, { 28000, 2000, XS_K, XS_K }, { 28000, 3365, XS_K, XS_K },
    // Above every examined revision: client installations only.
    { 28000, 3366, XS_K, XS_U }, { 28000, 6365, XS_K, XS_U }, { 28000, 6366, XS_U, XS_U }, { 28000, 29999, XS_U, XS_U },
    // A hotpatch branch, of which none was examined.
    { 28000, 30000, XS_U, XS_U }, { 28000, 40000, XS_U, XS_U },
    // Later builds.
    { 28001, 1, XS_U, XS_U }, { 28001, -1, XS_U, XS_U }, { 29000, 1, XS_U, XS_U }, { 30000, 1, XS_U, XS_U }
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

TEST(ExceptionXStateKernels, gated_feature) {
  // Enabled is left to the startup check; disabled, not configured, or no query loses the state.
  EXPECT_EQ(XS_U, VM_Version::decide_gated_exception_xstate_kernel(1));
  EXPECT_EQ(XS_L, VM_Version::decide_gated_exception_xstate_kernel(0));
  EXPECT_EQ(XS_L, VM_Version::decide_gated_exception_xstate_kernel(-1));
  EXPECT_EQ(XS_L, VM_Version::decide_gated_exception_xstate_kernel(2));
}

// A system whose sources the test sets, and which counts the reads the decision makes.
static bool fake_native;
static bool fake_container;
static int fake_file_build;
static int fake_running_build;
static bool fake_server;
static bool fake_reboot_pending;
static bool fake_kernel_readable;
static int fake_kernel_build;
static int fake_kernel_revision;
static int fake_feature;
static int kernel_reads;
static int feature_reads;

static bool fake_runs_natively() { return fake_native; }
static bool fake_in_container() { return fake_container; }
static int fake_file_build_fn() { return fake_file_build; }
static int fake_running_build_fn() { return fake_running_build; }
static bool fake_is_server() { return fake_server; }
static bool fake_reboot_pending_fn() { return fake_reboot_pending; }
static bool fake_kernel_file_version(int* build, int* revision) {
  kernel_reads++;
  if (!fake_kernel_readable) {
    return false;
  }
  *build = fake_kernel_build;
  *revision = fake_kernel_revision;
  return true;
}
static int fake_feature_state(uint32_t feature_id) {
  EXPECT_EQ(58474193u, feature_id);
  feature_reads++;
  return fake_feature;
}

static const VM_Version::ExceptionXStateSystem fake_system = {
  fake_runs_natively, fake_in_container, fake_file_build_fn, fake_running_build_fn, fake_is_server,
  fake_reboot_pending_fn, fake_kernel_file_version, fake_feature_state
};

// A native Windows Server 2025 host with the runners' gated kernel, feature not configured.
static void reset_fake_system() {
  fake_native = true;
  fake_container = false;
  fake_file_build = 26100;
  fake_running_build = 26100;
  fake_server = true;
  fake_reboot_pending = false;
  fake_kernel_readable = true;
  fake_kernel_build = 26100;
  fake_kernel_revision = 33828;
  fake_feature = -1;
  kernel_reads = 0;
  feature_reads = 0;
}

static VM_Version::ExceptionXStateKernel decide(VM_Version::ExceptionXStateDecision* decision) {
  return VM_Version::classify_exception_xstate_system(&fake_system, decision);
}

TEST(ExceptionXStateKernels, system_gated_feature) {
  VM_Version::ExceptionXStateDecision d;
  reset_fake_system();
  EXPECT_EQ(XS_L, decide(&d));
  EXPECT_EQ(33828, d.revision);
  EXPECT_EQ(-1, d.feature);
  EXPECT_EQ(1, feature_reads);
  EXPECT_EQ(nullptr, d.reason);
  reset_fake_system();
  fake_feature = 0;
  EXPECT_EQ(XS_L, decide(&d));
  EXPECT_EQ(0, d.feature);
  reset_fake_system();
  fake_feature = 1;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_EQ(1, d.feature);
  // A gated client revision.
  reset_fake_system();
  fake_server = false;
  fake_kernel_revision = 7705;
  fake_feature = 1;
  EXPECT_EQ(XS_U, decide(&d));
  fake_feature = 0;
  EXPECT_EQ(XS_L, decide(&d));
}

TEST(ExceptionXStateKernels, system_paths) {
  VM_Version::ExceptionXStateDecision d;
  // Affected and fixed client revisions, and the newest client one on a client and a server.
  reset_fake_system();
  fake_server = false;
  fake_kernel_revision = 4946;
  EXPECT_EQ(XS_L, decide(&d));
  EXPECT_EQ(0, feature_reads);
  fake_kernel_revision = 9711;
  EXPECT_EQ(XS_K, decide(&d));
  fake_kernel_revision = 9975;
  EXPECT_EQ(XS_K, decide(&d));
  fake_server = true;
  EXPECT_EQ(XS_U, decide(&d));
  // Windows 11 25H2: the kernel reports 26200, the files are of 26100.
  reset_fake_system();
  fake_server = false;
  fake_running_build = 26200;
  fake_kernel_revision = 9974;
  EXPECT_EQ(XS_K, decide(&d));
  EXPECT_EQ(26100, d.build);
  // A line before 26100 never reads ntoskrnl.exe.
  reset_fake_system();
  fake_file_build = 20348;
  fake_running_build = 20348;
  EXPECT_EQ(XS_K, decide(&d));
  EXPECT_EQ(0, kernel_reads);
  EXPECT_EQ(-1, d.revision);
  fake_file_build = 22621;
  fake_running_build = 22631;
  fake_server = false;
  EXPECT_EQ(XS_K, decide(&d));
  EXPECT_EQ(0, kernel_reads);
  // Emulated.
  reset_fake_system();
  fake_native = false;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("emulated", d.reason);
  EXPECT_EQ(0, kernel_reads);
  // A Windows Server 2022 image on a Windows Server 2025 host, detected as a container or not:
  // the running kernel's build is not the image's.
  reset_fake_system();
  fake_container = true;
  fake_file_build = 20348;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("the running kernel reports another build", d.reason);
  EXPECT_EQ(26100, d.running_build);
  fake_container = false;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("the running kernel reports another build", d.reason);
  // A Windows Server 2025 image with a fixed kernel file on an affected host: never read.
  reset_fake_system();
  fake_container = true;
  fake_kernel_revision = 9711;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("in a container", d.reason);
  EXPECT_EQ(0, kernel_reads);
  // A Windows Server 2022 image on a Windows Server 2022 host: decided by the running kernel's build.
  reset_fake_system();
  fake_container = true;
  fake_file_build = 20348;
  fake_running_build = 20348;
  EXPECT_EQ(XS_K, decide(&d));
  EXPECT_EQ(0, kernel_reads);
  // A kernel too old to report its build.
  reset_fake_system();
  fake_file_build = 17763;
  fake_running_build = 0;
  EXPECT_EQ(XS_U, decide(&d));
  // A pending reboot.
  reset_fake_system();
  fake_reboot_pending = true;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_STREQ("an update waits for a reboot", d.reason);
  EXPECT_EQ(0, kernel_reads);
  // ntoskrnl.exe unreadable, or of another build.
  reset_fake_system();
  fake_kernel_readable = false;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_EQ(-1, d.revision);
  reset_fake_system();
  fake_kernel_build = 20348;
  EXPECT_EQ(XS_U, decide(&d));
  EXPECT_EQ(-1, d.revision);
  // 28000, a later build, a build between the lines.
  reset_fake_system();
  fake_server = false;
  fake_file_build = 28000;
  fake_running_build = 28000;
  fake_kernel_build = 28000;
  fake_kernel_revision = 1000;
  EXPECT_EQ(XS_U, decide(&d));
  fake_kernel_revision = 3365;
  EXPECT_EQ(XS_K, decide(&d));
  fake_file_build = 28001;
  fake_running_build = 28001;
  EXPECT_EQ(XS_U, decide(&d));
  fake_file_build = 25398;
  fake_running_build = 25398;
  EXPECT_EQ(XS_U, decide(&d));
}

TEST(ExceptionXStateKernels, kernel_line) {
  EXPECT_EQ(18362, VM_Version::exception_xstate_kernel_line(18363));
  EXPECT_EQ(19041, VM_Version::exception_xstate_kernel_line(19041));
  EXPECT_EQ(19041, VM_Version::exception_xstate_kernel_line(19042));
  EXPECT_EQ(19041, VM_Version::exception_xstate_kernel_line(19045));
  EXPECT_EQ(19046, VM_Version::exception_xstate_kernel_line(19046));
  EXPECT_EQ(20348, VM_Version::exception_xstate_kernel_line(20348));
  EXPECT_EQ(20349, VM_Version::exception_xstate_kernel_line(20349));
  EXPECT_EQ(22621, VM_Version::exception_xstate_kernel_line(22631));
  EXPECT_EQ(26100, VM_Version::exception_xstate_kernel_line(26100));
  EXPECT_EQ(26100, VM_Version::exception_xstate_kernel_line(26200));
  EXPECT_EQ(28000, VM_Version::exception_xstate_kernel_line(28000));
  EXPECT_EQ(-1, VM_Version::exception_xstate_kernel_line(-1));
}

#endif
