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

#if defined(AARCH64) && defined(_WINDOWS) && !defined(ZERO)

#include "os_windows.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/os.hpp"
#include "runtime/vm_version.hpp"
#include "unittest.hpp"
#include "utilities/globalDefinitions.hpp"

// Where the JVM on Windows/AArch64 uses SVE: only on a kernel known to keep SVE state when resuming
// from an exception (JDK-8365196), which is the revision of a released update with a fixed kernel.

static bool keeps(int running_build, int version_build, int revision) {
  return VM_Version::windows_kernel_keeps_sve_state(running_build, true, version_build, revision);
}

TEST(SveStateKernels, build_26100) {
  // Updates with a fixed kernel.
  EXPECT_TRUE(keeps(26100, 26100, 7922));
  EXPECT_TRUE(keeps(26100, 26100, 9974));
  EXPECT_TRUE(keeps(26100, 26100, 10075));
  // Affected and gated updates, the revisions of kernel files that are not updates, hotpatch
  // revisions, revisions that were not seen, the Windows Server 2025 branch.
  static const int others[] = { -1, 0, 1, 863, 4946, 6584, 6725, 7623, 7840, 7920, 7921, 7923, 7979, 8036, 9000, 9911,
                                9975, 10068, 10076, 32230, 33828, 65535 };
  for (size_t i = 0; i < ARRAY_SIZE(others); i++) {
    EXPECT_FALSE(keeps(26100, 26100, others[i])) << others[i];
  }
  // The version could not be read, or reports another line.
  EXPECT_FALSE(VM_Version::windows_kernel_keeps_sve_state(26100, false, 26100, 9974));
  EXPECT_FALSE(VM_Version::windows_kernel_keeps_sve_state(26100, false, -1, -1));
  EXPECT_FALSE(keeps(26100, 28000, 9974));
  EXPECT_FALSE(keeps(26100, 22631, 9974));
  EXPECT_FALSE(keeps(26100, 26220, 9974));
}

TEST(SveStateKernels, enablement_builds) {
  // Windows 11 25H2 and 26H2 install the updates of 26100, whichever build the version reports.
  static const int builds[] = { 26100, 26200, 26300 };
  for (size_t i = 0; i < ARRAY_SIZE(builds); i++) {
    for (size_t j = 0; j < ARRAY_SIZE(builds); j++) {
      EXPECT_TRUE(keeps(builds[i], builds[j], 9974)) << builds[i] << " " << builds[j];
      EXPECT_TRUE(keeps(builds[i], builds[j], 9278)) << builds[i] << " " << builds[j];
      EXPECT_FALSE(keeps(builds[i], builds[j], 7623)) << builds[i] << " " << builds[j];
      EXPECT_FALSE(keeps(builds[i], builds[j], 9911)) << builds[i] << " " << builds[j];
    }
  }
}

// 26300 takes the updates of 26100 since 26300.9278; its earlier flights were not examined.
TEST(SveStateKernels, build_26300_before_the_shared_updates) {
  EXPECT_TRUE(keeps(26300, 26300, 9278));
  EXPECT_FALSE(keeps(26300, 26300, 9168));
  EXPECT_FALSE(keeps(26300, 26300, 7922));
  EXPECT_FALSE(keeps(26300, 26100, 9168));
  EXPECT_FALSE(keeps(26200, 26300, 9168));
  EXPECT_FALSE(keeps(26100, 26300, 8037));
  EXPECT_TRUE(keeps(26200, 26200, 9168));
  EXPECT_TRUE(keeps(26100, 26100, 7922));
  EXPECT_TRUE(keeps(26200, 26100, 8037));
}

TEST(SveStateKernels, build_28000) {
  EXPECT_TRUE(keeps(28000, 28000, 1575));
  EXPECT_TRUE(keeps(28000, 28000, 2179));
  EXPECT_TRUE(keeps(28000, 28000, 3501));
  EXPECT_TRUE(keeps(28000, 28000, 3620));
  static const int others[] = { -1, 0, 1, 1574, 1830, 1896, 3237, 3614, 3621, 9974, 65535 };
  for (size_t i = 0; i < ARRAY_SIZE(others); i++) {
    EXPECT_FALSE(keeps(28000, 28000, others[i])) << others[i];
  }
  EXPECT_FALSE(keeps(28000, 26100, 3365));
}

// Exactly the listed revisions of exactly these lines keep the state.
TEST(SveStateKernels, only_listed_updates) {
  static const int keeps_26100[] = {
    7922, 8037, 8039, 8116, 8117, 8246, 8328, 8457, 8524, 8655, 8737, 8875, 8894, 8973, 9168, 9278, 9445, 9457,
    9550, 9711, 9725, 9818, 9974, 10075
  };
  static const int keeps_28000[] = {
    1575, 1643, 1719, 1764, 1836, 2113, 2179, 2269, 2340, 2525, 2608, 2704, 2804, 2954, 2956, 3086, 3231, 3365,
    3501, 3620
  };
  size_t next_26100 = 0;
  size_t next_28000 = 0;
  for (int revision = -1; revision <= 70000; revision++) {
    bool listed_26100 = next_26100 < ARRAY_SIZE(keeps_26100) && keeps_26100[next_26100] == revision;
    bool listed_28000 = next_28000 < ARRAY_SIZE(keeps_28000) && keeps_28000[next_28000] == revision;
    EXPECT_EQ(listed_26100, keeps(26100, 26100, revision)) << revision;
    EXPECT_EQ(listed_28000, keeps(28000, 28000, revision)) << revision;
    next_26100 += listed_26100 ? 1 : 0;
    next_28000 += listed_28000 ? 1 : 0;
  }
  EXPECT_EQ(ARRAY_SIZE(keeps_26100), next_26100);
  EXPECT_EQ(ARRAY_SIZE(keeps_28000), next_28000);
  for (int build = -1; build <= 70000; build++) {
    bool line = build == 26100 || build == 26200 || build == 26300;
    EXPECT_EQ(line, keeps(build, build, 9974)) << build;
    EXPECT_EQ(build == 28000, keeps(build, build, 3365)) << build;
  }
}

// The running system: where Windows reports SVE, the JVM uses it exactly on a kernel that keeps
// the state.
TEST(SveStateKernels, this_system) {
  int running_build = os::win32::running_kernel_build();
  EXPECT_GE(running_build, 10240);
  int build = -1;
  int revision = -1;
  bool version_read = os::win32::running_kernel_version(&build, &revision);
  bool kept = VM_Version::windows_kernel_keeps_sve_state(running_build, version_read, build, revision);
  if (!kept) {
    EXPECT_FALSE(VM_Version::supports_sve());
    EXPECT_FALSE(VM_Version::supports_sve2());
  }
  if (VM_Version::supports_sve()) {
    EXPECT_TRUE(kept);
  }
  // The version can be read on every Windows that reports SVE, and on a kernel that keeps the
  // state the JVM uses the SVE the processor has.
  bool reported = IsProcessorFeaturePresent(46 /* PF_ARM_SVE_INSTRUCTIONS_AVAILABLE */) != 0;
  if (reported) {
    EXPECT_TRUE(version_read);
    EXPECT_GE(running_build, 26100);
  }
  if (reported && kept && FLAG_IS_DEFAULT(UseSVE)) {
    EXPECT_TRUE(VM_Version::supports_sve());
  }
}

#endif
