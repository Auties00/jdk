/*
 * Copyright (c) 2020, 2021, Microsoft Corporation. All rights reserved.
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
 *
 */

#include "logging/log.hpp"
#include "os_windows.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/os.hpp"
#include "runtime/vm_version.hpp"

// Since PF_ARM_SVE_INSTRUCTIONS_AVAILABLE and related constants were added in
// Windows 11 (version 24H2) and in Windows Server 2025, we define them here for
// compatibility with older SDK versions.
#ifndef PF_ARM_SVE_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SVE_INSTRUCTIONS_AVAILABLE 46
#endif

#ifndef PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE 47
#endif

#ifndef PF_ARM_SVE_BITPERM_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SVE_BITPERM_INSTRUCTIONS_AVAILABLE 51
#endif

#ifndef PF_ARM_SHA3_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SHA3_INSTRUCTIONS_AVAILABLE 64
#endif

#ifndef PF_ARM_SHA512_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SHA512_INSTRUCTIONS_AVAILABLE 65
#endif

#ifndef PF_ARM_V82_FP16_INSTRUCTIONS_AVAILABLE
#define PF_ARM_V82_FP16_INSTRUCTIONS_AVAILABLE 67
#endif

// Assembly function to get SVE vector length using RDVL instruction
extern "C" int get_sve_vector_length();

int VM_Version::get_current_sve_vector_length() {
  assert(VM_Version::supports_sve(), "should not call this");
  return VM_Version::supports_sve() ? get_sve_vector_length() : 0;
}

int VM_Version::set_and_get_current_sve_vector_length(int length) {
  assert(VM_Version::supports_sve(), "should not call this");

  // Unlike Linux, Windows does not present a way to modify the VL (the
  // rationale is that the OS expects the application to use the maximum vector
  // length supported by the hardware), so we simply return the current VL.  If
  // the user sets `MaxVectorSize` that is not the same as the maximum possible
  // vector length, then the caller (`VM_Version::initialize()`) will print a
  // warning, set `MaxVectorSize` to the value returned by this function, and
  // move on.
  return VM_Version::supports_sve() ? get_sve_vector_length() : 0;
}

// The Arm64 kernel of Windows is built from the sources of the x64 one and has the defect of
// JDK-8365196 in the same revisions (see classify_exception_xstate_kernel() in vm_version_x86.cpp):
// RtlpCopyXStateChunk trusts a header of the extended-state area that is already at the destination,
// which for a user-mode exception context is stale stack memory, and a stale header that looks like
// a compacted one without the SVE component leaves the SVE state out of the context. Windows
// reports SVE since build 26100 only. Of the 85 Arm64 kernels of the builds 26100 and 28000 that
// were examined, the ones up to 26100.6584 do nothing about it, the ones from 26100.6725 to 7824
// copy the kernel's own header first only when feature 58474193 is enabled, and the ones from
// 26100.7920 and of build 28000 always do. That a kernel without the copy loses the SVE state was
// not measured: no such Arm64 system was available; on a fixed one (26100.9974, 128-bit SVE) the
// predicate registers, FFR and the Z registers survive exceptions taken with header-like values
// below the stack pointer. So SVE is used only where the running kernel is known to copy the
// header: elsewhere the JVM uses NEON, whose registers are not in the extended-state area.
//
// The revisions are the QFE the running kernel reports (the revision of the update it booted
// with) of the released updates whose Arm64 kernel always copies the header, matched exactly as
// on x64 and for the same reasons (a hotpatch revision runs its baseline's kernel). They are the
// revisions that keep the state on x64. Builds 26200 and 26300 (Windows 11 25H2 and 26H2) install
// the updates of 26100, 26300 since its revision 9278; its earlier flights were not examined.
// A kernel update released after this list, Windows Server 2025 (not examined on Arm64) and every
// pre-release build run without SVE.
static const int sve_state_kept_26100[] = {
   7922,  8037,  8039,  8116,  8117,  8246,  8328,  8457,  8524,  8655,  8737,  8875,
   8894,  8973,  9168,  9278,  9445,  9457,  9550,  9711,  9725,  9818,  9974, 10075
};
static const int sve_state_kept_28000[] = {
   1575,  1643,  1719,  1764,  1836,  2113,  2179,  2269,  2340,  2525,  2608,  2704,
   2804,  2954,  2956,  3086,  3231,  3365,  3501,  3620
};

static const int sve_state_26300_first_shared_revision = 9278;

static int sve_state_kernel_line(int build) {
  return (build == 26200 || build == 26300) ? 26100 : build;
}

bool VM_Version::windows_kernel_keeps_sve_state(int running_build, bool version_read, int version_build,
                                                int version_revision) {
  const int line = sve_state_kernel_line(running_build);
  const int* revisions;
  size_t count;
  if (line == 26100) {
    revisions = sve_state_kept_26100;
    count = ARRAY_SIZE(sve_state_kept_26100);
  } else if (line == 28000) {
    revisions = sve_state_kept_28000;
    count = ARRAY_SIZE(sve_state_kept_28000);
  } else {
    // A build that was not examined, or one that does not report SVE.
    return false;
  }
  if (!version_read || sve_state_kernel_line(version_build) != line) {
    return false;
  }
  if ((running_build == 26300 || version_build == 26300) &&
      version_revision < sve_state_26300_first_shared_revision) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (revisions[i] == version_revision) {
      return true;
    }
  }
  return false;
}

void VM_Version::get_os_cpu_info() {

  if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) {
    set_feature(CPU_CRC32);
  }
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) {
    set_feature(CPU_AES);
    set_feature(CPU_SHA1);
    set_feature(CPU_SHA2);
    set_feature(CPU_PMULL);
  }
  if (IsProcessorFeaturePresent(PF_ARM_VFP_32_REGISTERS_AVAILABLE)) {
    set_feature(CPU_ASIMD);
  }
  if (IsProcessorFeaturePresent(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE)) {
    set_feature(CPU_LSE);
  }
  if (IsProcessorFeaturePresent(PF_ARM_SVE_INSTRUCTIONS_AVAILABLE)) {
    // With AvoidLosableStateAtTraps compiled code takes no resumed exception with SVE state live,
    // so SVE is used on every kernel and the kernel is not asked for its version.
    int running_build = -1;
    int build = -1;
    int revision = -1;
    bool version_read = false;
    // Without ExceptionXStateMitigation nothing protects SVE state either (a comparison mode).
    bool use_sve = AvoidLosableStateAtTraps || !ExceptionXStateMitigation;
    if (!use_sve) {
      running_build = os::win32::running_kernel_build();
      version_read = os::win32::running_kernel_version(&build, &revision);
      use_sve = windows_kernel_keeps_sve_state(running_build, version_read, build, revision);
    }
    if (use_sve) {
      set_feature(CPU_SVE);
      if (IsProcessorFeaturePresent(PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE)) {
        set_feature(CPU_SVE2);
      }
      if (IsProcessorFeaturePresent(PF_ARM_SVE_BITPERM_INSTRUCTIONS_AVAILABLE)) {
        set_feature(CPU_SVEBITPERM);
      }
    } else {
      if (version_read) {
        log_info(os, cpu)("Windows kernel 10.0.%d.%d (build %d): not known to keep SVE state when resuming from an "
                          "exception; SVE is not used", build, revision, running_build);
      } else {
        log_info(os, cpu)("Windows build %d: not known to keep SVE state when resuming from an exception; SVE is "
                          "not used", running_build);
      }
      if (!FLAG_IS_DEFAULT(UseSVE) && UseSVE > 0) {
        warning("UseSVE specified, but this Windows kernel is not known to keep SVE state when resuming from an "
                "exception (JDK-8365196). Disabling SVE.");
        FLAG_SET_DEFAULT(UseSVE, 0);
      }
    }
  }
  if (IsProcessorFeaturePresent(PF_ARM_V82_FP16_INSTRUCTIONS_AVAILABLE)) {
    set_feature(CPU_FPHP);
    set_feature(CPU_ASIMDHP);
  }
  if (IsProcessorFeaturePresent(PF_ARM_SHA3_INSTRUCTIONS_AVAILABLE)) {
    set_feature(CPU_SHA3);
  }
  if (IsProcessorFeaturePresent(PF_ARM_SHA512_INSTRUCTIONS_AVAILABLE)) {
    set_feature(CPU_SHA512);
  }

  __int64 dczid_el0 = _ReadStatusReg(0x5807 /* ARM64_DCZID_EL0 */);

  if (!(dczid_el0 & 0x10)) {
    _zva_length = 4 << (dczid_el0 & 0xf);
  }

  {
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = nullptr;
    DWORD returnLength = 0;

    // See https://docs.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformation
    GetLogicalProcessorInformation(nullptr, &returnLength);
    assert(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Unexpected return from GetLogicalProcessorInformation");

    buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)os::malloc(returnLength, mtInternal);
    BOOL rc = GetLogicalProcessorInformation(buffer, &returnLength);
    assert(rc, "Unexpected return from GetLogicalProcessorInformation");

    _icache_line_size = _dcache_line_size = -1;
    for (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = buffer; ptr < buffer + returnLength / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ptr++) {
      switch (ptr->Relationship) {
      case RelationCache:
        // Cache data is in ptr->Cache, one CACHE_DESCRIPTOR structure for each cache.
        PCACHE_DESCRIPTOR Cache = &ptr->Cache;
        if (Cache->Level == 1) {
            _icache_line_size = _dcache_line_size = Cache->LineSize;
        }
        break;
      }
    }
    os::free(buffer);
  }

  {
    DWORD key_type = 0;
    uint64_t value = 0;
    DWORD value_size = sizeof(value);
    // Read registry value "CP 4000" which maps to MIDR_EL1.
    if ((RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "CP 4000",
                      RRF_RT_REG_QWORD, &key_type, &value, &value_size) == ERROR_SUCCESS) && (value_size == sizeof(value))) {
      _cpu = value >> 24 & 0xFF;
      _variant = value >> 20 & 0x0F;
      _model = value >> 4 & 0x0FFF;
      _revision = value & 0x0F;
    }
  }

  if (_cpu == 0) {
    char* buf = ::getenv("PROCESSOR_IDENTIFIER");
    if (buf && strstr(buf, "Ampere(TM)") != nullptr) {
      _cpu = CPU_AMCC;
    } else if (buf && strstr(buf, "Cavium Inc.") != nullptr) {
      _cpu = CPU_CAVIUM;
    } else if (buf && strstr(buf, "Qualcomm Technologies Inc") != nullptr) {
      _cpu = CPU_QUALCOMM;
    } else {
      log_info(os)("VM_Version: unknown CPU model");
    }

    if (_cpu) {
      SYSTEM_INFO si;
      GetSystemInfo(&si);
      _model = si.wProcessorLevel;
      _variant = (si.wProcessorRevision >> 8) & 0xFF; // Variant is the upper byte of wProcessorRevision
      _revision = si.wProcessorRevision & 0xFF; // Revision is the lower byte of wProcessorRevision
    }
  }
}

void VM_Version::get_compatible_board(char *buf, int buflen) {
  assert(buf != nullptr, "invalid argument");
  assert(buflen >= 1, "invalid argument");
  *buf = '\0';
}
