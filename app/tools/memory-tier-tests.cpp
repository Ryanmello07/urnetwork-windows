// Executable spec for the service's memory tiers (Common/MemoryTiers.h): which
// device target and process budget a host gets, that an unmeasurable host is
// treated as a small one, and that both tiers satisfy the backing and collector
// constraints. Run against the SAME header the service compiles, on any host
// with a C++20 compiler -- the probe it feeds (GlobalMemoryStatusEx, Sdk.cpp)
// is Windows-only and is not exercised here.
//
//   c++ -std=c++20 -I ../src/Common memory-tier-tests.cpp \
//       -o /tmp/memory-tier-tests && /tmp/memory-tier-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdio>
#include <string>

#include "MemoryTiers.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(const std::string& what, bool ok) {
  ++gCases;
  if (!ok) {
    ++gFailures;
    std::printf("FAIL  %s\n", what.c_str());
  } else {
    std::printf("ok    %s\n", what.c_str());
  }
}

void CheckEq(const std::string& what, int64_t expected, int64_t actual) {
  ++gCases;
  if (expected != actual) {
    ++gFailures;
    std::printf("FAIL  %s: expected %lld, got %lld\n", what.c_str(),
                static_cast<long long>(expected), static_cast<long long>(actual));
  } else {
    std::printf("ok    %s\n", what.c_str());
  }
}

constexpr int64_t kGiB = 1024ll * 1024 * 1024;

}  // namespace

int main() {
  CheckEq("the base pair is 128 MiB in 384", 128ll * 1024 * 1024, kDeviceMemoryTargetByteCount);
  CheckEq("the base budget is 384 MiB", 384ll * 1024 * 1024, kProcessMemoryBudgetByteCount);
  CheckEq("the large pair is 256 MiB in 768", 256ll * 1024 * 1024,
          kLargeHostDeviceMemoryTargetByteCount);
  CheckEq("the large budget is 768 MiB", 768ll * 1024 * 1024,
          kLargeHostProcessMemoryBudgetByteCount);
  CheckEq("the large-host bar is 7 GiB", 7ll * kGiB, kLargeHostMemoryByteCount);

  // The gate over the measurement, including the failure case. With the bar
  // this low nearly every real machine is on one side of it, so an off-by-one
  // in the comparison would be invisible in practice: the three rows around
  // 7 GiB are the only thing that would catch it. The two rows after them are
  // the machines the decision is about, and fail if the bar is rounded up to 8.
  struct Row {
    const char* what;
    int64_t host;
    int64_t target;
  };
  const Row rows[] = {
      {"an unmeasurable host takes the base tier", 0, kDeviceMemoryTargetByteCount},
      {"a failed measurement takes the base tier", -1, kDeviceMemoryTargetByteCount},
      {"a 4 GiB host takes the base tier", 4 * kGiB, kDeviceMemoryTargetByteCount},
      {"one byte under 7 GiB takes the base tier", 7 * kGiB - 1, kDeviceMemoryTargetByteCount},
      {"exactly 7 GiB takes the base tier (strict)", 7 * kGiB, kDeviceMemoryTargetByteCount},
      {"one byte over 7 GiB takes the large tier", 7 * kGiB + 1,
       kLargeHostDeviceMemoryTargetByteCount},
      {"an exact 8 GiB reading takes the large tier", kNominal8GiBHostMemoryByteCount,
       kLargeHostDeviceMemoryTargetByteCount},
      {"an 8 GiB machine's usable 7.68 GiB takes the large tier", kUsable8GiBHostMemoryByteCount,
       kLargeHostDeviceMemoryTargetByteCount},
      {"a 12 GiB laptop takes the large tier", 12 * kGiB, kLargeHostDeviceMemoryTargetByteCount},
      {"a 64 GiB workstation takes the large tier", 64 * kGiB,
       kLargeHostDeviceMemoryTargetByteCount},
  };
  for (const Row& row : rows) {
    CheckEq(row.what, row.target, MemoryTierForHost(row.host).device_target_byte_count);
  }

  // The budget always moves with the target it backs, including on the rare
  // unknown-host path.
  CheckEq("an unknown host gets the base budget", kProcessMemoryBudgetByteCount,
          MemoryTierForHost(0).process_budget_byte_count);
  CheckEq("the base target is backed by the base budget", kProcessMemoryBudgetByteCount,
          MemoryTierForHost(7 * kGiB).process_budget_byte_count);
  CheckEq("the large target is backed by the large budget", kLargeHostProcessMemoryBudgetByteCount,
          MemoryTierForHost(7 * kGiB + 1).process_budget_byte_count);
  CheckEq("an 8 GiB machine gets the large budget too", kLargeHostProcessMemoryBudgetByteCount,
          MemoryTierForHost(kUsable8GiBHostMemoryByteCount).process_budget_byte_count);

  // Both constraints on both tiers and at the bar itself. MemoryTiers.h
  // static_asserts these too, so a regression fails the build; this says so out
  // loud where a reader looks.
  for (const int64_t host :
       {int64_t{0}, 7 * kGiB - 1, 7 * kGiB, 7 * kGiB + 1, 8 * kGiB, 128 * kGiB}) {
    const MemoryTier tier = MemoryTierForHost(host);
    Check("every tier's target is backed by its budget (20/34)", MemoryTierIsBacked(tier));
    Check("every tier's budget is three times its target", MemoryTierIsCollectorSafe(tier));
  }

  std::printf("\n%d passed, %d failed, %d total\n", gCases - gFailures, gFailures, gCases);
  return gFailures == 0 ? 0 : 1;
}
