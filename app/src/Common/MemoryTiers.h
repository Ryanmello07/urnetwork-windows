// The service's memory tiers: a per-device memory target and the process budget
// that backs it, which are only ever chosen together.
//
// connect draws the H3 carrier windows from the DEVICE target (the stream
// window is three quarters of the carrier's eighth, so 3 * target / 32), while
// SdkInit's budget sizes the message pools and the go soft limit. Two
// constraints bind them, and both are asserted below for both tiers:
//
//   backing    the device targets plus the message pools must fit the budget,
//              and the pools take 14 of 34 parts, so a target may be at most
//              20/34 of the budget.
//   collector  the budget is also the go soft limit, and live heap amplifies
//              about threefold at the runtime; a target too close to its soft
//              limit reproduces the measured mobile collection storm (23.6
//              collections per second). So the budget is at least three times
//              the target. This is the binding one at both sizes.
//
// The tier is chosen from MEASURED host memory (Sdk.h's HostMemoryByteCount,
// GlobalMemoryStatusEx), never from an assumption, and a host whose memory
// cannot be determined takes the base pair: an unknown host is not a large
// host.
//
// Nothing here includes windows.h or the SDK, so tools/memory-tier-tests.cpp
// runs this policy on any host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw {

inline constexpr int64_t kProcessMemoryBudgetByteCount = 384ll * 1024 * 1024;
inline constexpr int64_t kDeviceMemoryTargetByteCount = 128ll * 1024 * 1024;
inline constexpr int64_t kLargeHostProcessMemoryBudgetByteCount = 768ll * 1024 * 1024;
inline constexpr int64_t kLargeHostDeviceMemoryTargetByteCount = 256ll * 1024 * 1024;
inline constexpr int64_t kLargeHostMemoryByteCount = 16ll * 1024 * 1024 * 1024;

// The parts the two constraints are written in, so a pair cannot drift apart
// silently.
inline constexpr int64_t kMemoryPoolRatioParts = 14;
inline constexpr int64_t kMemoryBudgetRatioParts = 34;
inline constexpr int64_t kCollectorBudgetMultiple = 3;

struct MemoryTier {
  int64_t device_target_byte_count;
  int64_t process_budget_byte_count;
};

// The tier for a host with `hostMemoryByteCount` usable bytes. A nonpositive
// (unknown) measurement takes the base tier.
constexpr MemoryTier MemoryTierForHost(int64_t hostMemoryByteCount) {
  if (kLargeHostMemoryByteCount <= hostMemoryByteCount) {
    return MemoryTier{kLargeHostDeviceMemoryTargetByteCount,
                      kLargeHostProcessMemoryBudgetByteCount};
  }
  return MemoryTier{kDeviceMemoryTargetByteCount, kProcessMemoryBudgetByteCount};
}

constexpr bool MemoryTierIsBacked(MemoryTier tier) {
  return tier.device_target_byte_count * kMemoryBudgetRatioParts <=
         tier.process_budget_byte_count * (kMemoryBudgetRatioParts - kMemoryPoolRatioParts);
}

constexpr bool MemoryTierIsCollectorSafe(MemoryTier tier) {
  return kCollectorBudgetMultiple * tier.device_target_byte_count <=
         tier.process_budget_byte_count;
}

// Both constraints, on both tiers, at compile time: raising one number without
// the other fails the build rather than the fleet.
static_assert(MemoryTierIsBacked(MemoryTierForHost(0)),
              "the base device memory target is not backed by its process budget");
static_assert(MemoryTierIsCollectorSafe(MemoryTierForHost(0)),
              "the base process budget is too close to its device memory target");
static_assert(MemoryTierIsBacked(MemoryTierForHost(kLargeHostMemoryByteCount)),
              "the large-host device memory target is not backed by its process budget");
static_assert(MemoryTierIsCollectorSafe(MemoryTierForHost(kLargeHostMemoryByteCount)),
              "the large-host process budget is too close to its device memory target");
// An unknown host takes the base tier, and the gate is a floor rather than a
// window: everything at or above the threshold is large.
static_assert(MemoryTierForHost(0).device_target_byte_count == kDeviceMemoryTargetByteCount,
              "an unknown host must take the base memory tier");
static_assert(MemoryTierForHost(-1).device_target_byte_count == kDeviceMemoryTargetByteCount,
              "an unmeasurable host must take the base memory tier");
static_assert(MemoryTierForHost(kLargeHostMemoryByteCount - 1).device_target_byte_count ==
                  kDeviceMemoryTargetByteCount,
              "a host just under the threshold must take the base memory tier");

}  // namespace urnw
