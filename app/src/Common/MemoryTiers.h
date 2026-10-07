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
// THE BAR: every machine sold as 8 GiB or more takes the large tier. The same
// bar on macOS, Windows and the Linux daemon.
//
// This is a product decision rather than a memory one: the throughput the
// larger window buys is the product, and it is wanted on ordinary machines
// rather than on workstations alone. It is a deliberate trade, and worth
// stating as one. The 256 MiB target permits a 768 MiB process budget, and that
// budget is the go soft limit -- not a ceiling the process avoids, but the
// level the collector lets live heap climb toward before it works hard. On a
// machine just over this bar, a steady-state service approaching that figure is
// a real share of the machine. The program spends that memory because the
// throughput is what it is buying.
//
// Two things a later reader will need if the bar is ever revisited. Host memory
// is a weak proxy for a link fast enough to make the receive window bind, so
// some hosts over the bar -- laptops on wireless, mostly -- pay the memory and
// never reach the throughput it buys. And the instrument that would size this
// on the thing that actually predicts the need is an explicit opt-in, or
// promotion on measured throughput, rather than any RAM threshold; either is a
// different change from this one.
//
// WHY THE NUMBER BELOW IS 7 AND NOT 8 -- do not round it up. A probe reports
// usable memory, and usable memory is below the size a machine is sold as:
// firmware, the kernel and an integrated GPU's carve-out come off before
// GlobalMemoryStatusEx reports ullTotalPhys, so a machine sold as 8 GiB reads
// roughly 7.6-7.8 GiB. A bar set at the nominal 8 GiB would exclude exactly the
// machines it exists to include, and would silently send every base-model 8 GiB
// machine back to the small tier. The threshold sits a whole GiB below nominal
// so that cannot happen, while anything genuinely smaller -- a 6 GiB machine
// reads about 5.7 -- stays base.
//
// The comparison is strict -- measured memory must EXCEED 7 GiB -- which sits
// naturally with a threshold chosen below the nominal size. Rounding this up is
// not left to review: the static_asserts below require an exact 8 GiB reading
// and a realistic 7.68 GiB one to take the large tier, so the build fails.
inline constexpr int64_t kLargeHostMemoryByteCount = 7ll * 1024 * 1024 * 1024;

// What machines sold as 8 GiB actually report, for those assertions: installed
// memory exactly (as macOS's hw.memsize reports it), and a realistic usable
// reading after firmware, kernel and integrated-graphics reservations (a Linux
// MemTotal of 8048972 kB, about 7.68 GiB).
inline constexpr int64_t kNominal8GiBHostMemoryByteCount = 8ll * 1024 * 1024 * 1024;
inline constexpr int64_t kUsable8GiBHostMemoryByteCount = 8048972ll * 1024;

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
// (unknown) measurement takes the base tier, which is now the RARE path --
// genuinely small hosts, and hosts whose probe failed -- and so the one to keep
// pinned by assertions rather than by practice.
constexpr MemoryTier MemoryTierForHost(int64_t hostMemoryByteCount) {
  if (kLargeHostMemoryByteCount < hostMemoryByteCount) {
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
// Both constraints on the tiers either side of the bar itself, not merely on
// some small and some large host.
static_assert(MemoryTierIsBacked(MemoryTierForHost(kLargeHostMemoryByteCount)),
              "the tier at the bar is not backed by its process budget");
static_assert(MemoryTierIsCollectorSafe(MemoryTierForHost(kLargeHostMemoryByteCount)),
              "the tier at the bar has a process budget too close to its target");
static_assert(MemoryTierIsBacked(MemoryTierForHost(kLargeHostMemoryByteCount + 1)),
              "the large-host device memory target is not backed by its process budget");
static_assert(MemoryTierIsCollectorSafe(MemoryTierForHost(kLargeHostMemoryByteCount + 1)),
              "the large-host process budget is too close to its device memory target");
// The base tier is the rare path now, so its WHOLE pair is asserted rather than
// left to practice: an unknown host gets the base target and the base budget.
static_assert(MemoryTierForHost(0).device_target_byte_count == kDeviceMemoryTargetByteCount,
              "an unknown host must take the base device memory target");
static_assert(MemoryTierForHost(0).process_budget_byte_count == kProcessMemoryBudgetByteCount,
              "an unknown host must take the base process budget");
static_assert(MemoryTierForHost(-1).device_target_byte_count == kDeviceMemoryTargetByteCount &&
                  MemoryTierForHost(-1).process_budget_byte_count == kProcessMemoryBudgetByteCount,
              "an unmeasurable host must take the base memory tier");
// The comparison is strict, and the three rows around the bar pin it: one byte
// under and exactly at the bar are base, one byte over is large.
static_assert(MemoryTierForHost(kLargeHostMemoryByteCount - 1).device_target_byte_count ==
                  kDeviceMemoryTargetByteCount,
              "a host one byte under the bar must take the base memory tier");
static_assert(MemoryTierForHost(kLargeHostMemoryByteCount).device_target_byte_count ==
                  kDeviceMemoryTargetByteCount,
              "a host exactly at the bar must take the base memory tier");
static_assert(MemoryTierForHost(kLargeHostMemoryByteCount + 1).device_target_byte_count ==
                  kLargeHostDeviceMemoryTargetByteCount,
              "a host one byte over the bar must take the large memory tier");
// The machines the decision is about. These are what fail if the bar is ever
// rounded up to the nominal 8 GiB.
static_assert(MemoryTierForHost(kNominal8GiBHostMemoryByteCount).device_target_byte_count ==
                      kLargeHostDeviceMemoryTargetByteCount &&
                  MemoryTierForHost(kNominal8GiBHostMemoryByteCount).process_budget_byte_count ==
                      kLargeHostProcessMemoryBudgetByteCount,
              "a machine reporting exactly 8 GiB must take the large memory tier");
static_assert(MemoryTierForHost(kUsable8GiBHostMemoryByteCount).device_target_byte_count ==
                      kLargeHostDeviceMemoryTargetByteCount &&
                  MemoryTierForHost(kUsable8GiBHostMemoryByteCount).process_budget_byte_count ==
                      kLargeHostProcessMemoryBudgetByteCount,
              "a machine sold as 8 GiB, reporting its usable 7.68 GiB, must take the large tier");

}  // namespace urnw
