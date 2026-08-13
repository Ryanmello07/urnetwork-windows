# Smart Routing Phase 2 — Live Inputs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `scoredplacement=1` mean something — give the Phase 1 scorer a real
classifier, real per-exit telemetry, real demotion state, and a real reward tap, then
turn it on by default so the owner can observe it working.

**Architecture:** Phase 1 landed the scoring math (`routing_score.go`), the class model
(`routing_class.go`), the priors store (`routing_priors.go`) and the reward accumulator
(`routing_reward.go`) — all inert. Phase 2 supplies the four missing inputs and lights
the path. No new safety surface: `scoredPlacementReorder` only ever RE-ORDERS a candidate
list that `raceCandidates` has already gated. Membership stays owned by the safety layer.

**Tech Stack:** Go (connect, sdk), C++/WinRT (windows). No new production dependencies —
the light-tier classifier is pure Go over seams that already exist.

## Context: what exists and what does not

Verified against the tree at connect `98b3215` / sdk `855b93d`:

- **Exists, unused:** `ExitMetrics`, `exitScore`, `challengerWins`, `demotionState`,
  `lessLoadedTieBreak` (`routing_score.go`); `TrafficClass`, `FlowClass`,
  `FlowClassifier`, `classifyOrUnknown` (`routing_class.go`); `ProviderPrior`,
  `PriorsStore` + sdk `localStatePriorsStore` (`routing_priors.go`, sdk
  `local_state.go`); `rewardAccumulator` (`routing_reward.go`).
- **Exists, wired:** `appId` already reaches `scoredPlacementReorder` — the Windows
  `FlowOwner` feeds the SDK's `setFlowOwnerLookup` seam, which lands in
  `flowOwnerFunc` and is resolved (with a generation-checked cache) at
  `ip_remote_multi_client.go:1431-1467`. **No Windows-side work is required for app
  attribution.**
- **Exists, reusable:** `windowStatsWithCoalesce(false)` — the side-effect-free
  window-stats read the Phase 1 comment said did not exist. It DOES exist
  (`ip_remote_multi_client.go`, just under `WindowStats`). Use it; never call
  `WindowStats()` on the placement path, which coalesces buckets and perturbs the
  resize pass's cadence.
- **Exists, reusable:** `ServerNameLookup` seam (`ip_assoc.go:278`) and the IP→hostname
  reverse index (`ip_mux_upgrade.go`) — the light-tier classifier's inputs.
- **Missing entirely:** any `FlowClassifier` implementation. This is why
  `scoredplacement=1` is a no-op today: `classifyOrUnknown` returns `ClassUnknown` and
  `scoredPlacementReorder` returns `candidates` untouched.
- **Missing:** telemetry into `ExitMetrics` (Rtt/Goodput/Jitter/Stalls all zero),
  ownership of `demotionState`, any call site for `rewardAccumulator`, and a decay term
  on `quarantineReconvictionCount`.

## Global Constraints

- **Zero-value-off is the convention.** Every new `ReliabilitySettings` field must be
  inert at its zero value. Task 8 is the ONLY task that changes a default.
- **Never regress the safety layer.** `scoredPlacementReorder` re-orders; it must never
  add, remove, or unquarantine a candidate. A reviewer must be able to prove this from
  the diff.
- **No lock held across a syscall, and no blocking call on the placement path.** The
  placement path runs at new-flow frequency inside the packet pump's call chain.
  Classification must be a map/table lookup, never I/O, never a DNS query.
- **Do not call `WindowStats()`** (the coalescing variant) anywhere on the placement or
  scoring path. Use `windowStatsWithCoalesce(false)`.
- **Reflection-based banner.** `ReliabilitySettings` fields appear in the session banner
  automatically via `relSettingsFields`. Do not hand-edit a banner list; add the field
  and the banner follows. Field names render lowercased with no separators.
- **Observability is a deliverable, not a nicety.** The owner's standing complaint is
  "how do I know it's working". Every task that adds behavior adds a `[rel]` line
  proving it, using `relEvent(...)` from `ip_remote_multi_client_observability.go`.
- **Go tests:** always `-run <pattern> -timeout 60s`. Never a bare full-suite run.
- **gofmt only files you touched.** A repo-wide gofmt sweep modified 4 unrelated files
  in an earlier task and had to be reverted.
- **Commits:** conventional-commit subject, no UTF-8 BOM (PowerShell here-strings add
  one — verify with `git log -1 --format=%s | head -c 3 | xxd`). Verify
  `git ls-files | wc -l` matches `git ls-tree -r HEAD --name-only | wc -l` before every
  commit — this checkout has silently truncated commits before.
- **Branches:** connect + sdk worktrees are on `beta/algorithm-dpi`; CI pins both clones
  to that branch. Push there.
- **Privacy:** persist only coarse per-provider-identity priors. Never persist raw
  posteriors, hostnames, or per-app history — that is a poisoning and a privacy surface.

---

## Deferred minors — triage these at the final whole-branch review

Recorded as they were found, not fixed in-task. The final review must decide
which of these must be fixed before this branch merges.

**From Task 3 (`c2b1cb6`) review:**
1. `addSendRttSample` (~`ip_remote_multi_client.go:10797`) re-acquires `stateLock`
   immediately after `addSendAck` (~`:12420`) has already taken and released it
   inside the same `ackCallback` — two lock round-trips per acked packet where
   one would do. Correct, but this is a genuinely hot path; worth folding into
   one acquisition.
2. The ack callback's double-fire behavior could not be fully traced (dispatch
   lives in `transfer_contract_manager`/`transfer_control`, outside the diff). If
   it can fire twice, the second fire folds a stale `time.Since(sendTime)` into
   the RTT EWMA. This is inherited risk equal to what `addSendAck` already
   carries on the same callback — not new, but now it feeds a scorer input.
3. RTT collection runs unconditionally on every ack regardless of
   `ScoredPlacement`, while the read side is fully gated. Consistent with this
   file's existing "raw counters always-on, gate at consumption" pattern, so not
   a deviation — but it is a small always-on cost that did not exist before.

**From Task 5 fix (`dadf1c8`) re-review — fix ACCEPTED, these remain:**
7. `routing_reward.go:44-46` — the `rewardAccumulatorMaxKeys = 256` cap
   **silently rejects** new `(class, exitId)` keys once full. No counter, no log
   line, and no test exercises the 257th key. It is scoped to the
   `HeartbeatInterval=0` corner (never a shipped default), so it guards an
   unreachable path — but it is a second instance of the "no silent caps"
   pattern this project has already been bitten by. Wants a one-line counter or
   log on drop, plus a test, before it is trusted. **Fold into Task 7's
   dispatch** — it is observability work and Task 7 is already the
   observability-proof task.
8. The cap keys on `(class, exitId)` pairs, not `exitId` alone, so 256 is
   really "256 / number-of-classes providers" in the worst case (all traffic in
   one class). Not wrong, but the doc comment overstates the per-provider
   headroom it buys.

**From Task 4 (`53dec2d`) review:**
4. **Demotion's promotion target ignores the load tie-break.** `plainBestIndex`
   picks a strict argmax raw score, while the ordinary loop uses
   `lessLoadedTieBreak`. So a demotion can concentrate flows onto a marginally
   higher-scored exit that the load-aware path would have spread away from.
   This one is more than cosmetic: anti-herding was an explicit design goal
   (the measured 37-flows-on-one-exit case), and this is a path that bypasses
   it. Strong candidate to actually fix, not just note.
5. `scoredPlacementReorder` takes `stateLock` for `flowCounts` and
   `demotionObserve` takes it again — two acquisitions on a new-flow-frequency
   path. Same class as deferred minor #1; fix them together.
6. The Task 4 report claims the `[rel] event=demote` line fires only on the
   round demotion changes `bestIndex`. It actually fires every round the bad
   streak persists (the in-source comment on `demotionLogThrottle` is correct;
   the report is not). No behavioral impact — the 5s throttle does the real
   work — but do not trust the report's wording here.

## BLOCKERS on Task 8 (default-on) — must be fixed first

**B1. Task 5 (`634f768`): provider priors are keyed on the wrong identity.**
`recordFlowReward` keys on `client.ClientId()`, which is our own ephemeral
per-window-slot id, minted fresh by `POST /network/auth-client` on nearly every
channel reconnect (`ip_remote_multi_client_identity.go:8-15` states this
outright). The stable provider identity is `client.Destination().Tail()`, already
used in this file as `egressClientId`. Consequence: priors fail across an
ordinary in-session reconnect to the SAME provider, not merely across restarts —
the learning feature is inert from the first session while appearing complete.
Fix dispatched.

**B2. Task 6 (`2ce2655`): reconviction decay is not durable.**
Decay is a read-time lens over a raw counter that is never decremented. Raw 3
decays to a read of 0 over quiet time; a 4th conviction stamps
`quarantineLiftTime = now`, so the next read computes elapsed≈0 and returns
**4, not 1**. Forgiveness looks durable during quiet monitoring and then
evaporates entirely on the next single event. `m.StallEvents =
quarantineReconvictionCount()` (`:3179`) feeds this into `exitScore`'s
continuous, uncapped-until-30 stall term — the reviewer measured stallPenalty
jumping 0.1→0.4, ~10% of the ~3.0 positive score range, enough to flip a close
placement. `benchDuration` hides it only because its ≥2 cap makes it insensitive
to 1 vs 4; that is the bug being invisible to one consumer, not the design being
safe.
FIX: decay-then-increment in `clearQuarantineWithLock` — read the OLD
`quarantineLiftTime` before overwriting it, store
`quarantineReconvictions = decay(raw, now - oldLift) + 1`, then advance the
anchor. No new state, same anchor field, keeps reads idempotent and the
off-path short-circuit intact. (The Task 6 report's claim that a real leaky
bucket needs a second anchor is wrong — it needs only to read the existing
anchor once before overwriting.)

**Not a Phase 2 defect, but found by Phase 2 —** `multiClientWindow.Close()`
(`ip_remote_multi_client.go:~9394`) closes every remaining client directly while
`self.removeClients(removedClients)` sits commented out, so
`clientRemoveCallback` never runs for them. Tracked as issue #51. Task 4 is
unaffected (its map dies with the object it hangs off), but any future
per-channel state expected to be evicted via `removeClient` inherits this hole.

## Process rules learned during execution (do not repeat these mistakes)

- **Exactly ONE writing agent in `connect-algo` at a time.** Task 2's fix rounds
  were dispatched while Task 3 was mid-implementation in the same worktree. Both
  agents detected it; no work was lost only because Task 2's used
  `git apply --cached` to stage its own hunks without touching the working tree.
  Do not rely on that happening again.
- **Never reuse one detached worktree across reviews.** A reused review worktree
  silently held the wrong commit and stray edits despite a checkout reporting
  success. Create a fresh detached worktree per review, and tell every reviewer
  to ASSERT `git rev-parse HEAD` matches the expected sha before reviewing —
  and to fall back to `git archive` of the target commit into a scratch dir if
  it does not.

---

### Task 1: Light-tier flow classifier

**Files:**
- Create: `routing_classifier_light.go` (connect)
- Test: `routing_classifier_light_test.go` (connect)

**Interfaces:**
- Consumes: `TrafficClass`, `FlowClass`, `FlowClassifier` (`routing_class.go`); `IpPath`.
- Produces: `NewLightClassifier(names ServerNameResolver) *LightClassifier` implementing
  `FlowClassifier`; `type ServerNameResolver func(ip netip.Addr) (string, bool)`.

Pure-Go classifier, spec §2 light tier. Precedence, highest first:
manual override (not in this task) > app default (exe name) > server name > port.

- [ ] **Step 1: Write the failing table test.** Cover, at minimum: `443/tcp` with name
  `netflix.com` → `ClassStreaming`; `443/tcp` with no name → `ClassBrowsing`; `3478/udp`
  → `ClassLatency`; `51413/tcp` → `ClassBulk`; unknown high port, no name, no app →
  `ClassUnknown` (never guess); `appId="steam.exe"` on port 443 → `ClassBulk` (app beats
  name); nil resolver → must not panic.
- [ ] **Step 2: Run it and confirm it fails to compile** (`go test -run TestLightClassifier -timeout 60s`).
- [ ] **Step 3: Implement.** Static tables: a port→class map, a suffix→class list for
  server names, an exe→class map. Every table lookup, no I/O. Return
  `FlowClass{Class: ..., Confidence: ...}`; `ClassUnknown` when nothing matches — an
  unknown flow must fall through to the legacy order, never to a guessed class.
- [ ] **Step 4: Run the test to green.**
- [ ] **Step 5: Commit** `feat(routing): light-tier pure-Go flow classifier`.

---

### Task 2: Install the classifier + make classification visible

**Files:**
- Modify: `ip_remote_multi_client.go` (connect) — new setting, install site, `[rel]` line

**Interfaces:**
- Consumes: Task 1's `NewLightClassifier`; the existing `SetFlowClassifier` seam
  (`:1379`) and `ServerNameLookup` (`ip_assoc.go:278`).
- Produces: `ReliabilitySettings.LightClassifier bool`.

- [ ] **Step 1: Write the failing test** — with `LightClassifier` false, `flowClassifier`
  stays nil; with it true, a classifier is installed and `scoredPlacementReorder` on a
  known-streaming path returns a DIFFERENT order than the legacy one. Prove the test is
  not hollow by asserting the legacy order explicitly.
- [ ] **Step 2: Run, confirm fail.**
- [ ] **Step 3: Implement.** Add the field at all four sites the other knobs use
  (struct, ReliabilitySettings, the `2180` mirror, `ReliabilitySettingsFrom`). Install
  the classifier at session construction when the knob is on, resolving names through
  the existing reverse index. Emit a SAMPLED `[rel] event=classify class=… app=… port=…`
  — sampled, not per flow; a per-flow line would flood the log at flow-storm rates.
- [ ] **Step 4: Run to green + confirm the banner shows `lightclassifier=0` by default.**
- [ ] **Step 5: Commit** `feat(routing): install the light classifier behind LightClassifier`.

---

### Task 3: Real per-exit telemetry into ExitMetrics

**Files:**
- Modify: `ip_remote_multi_client.go` (connect) — `exitMetricsSnapshot`
- Test: `routing_telemetry_test.go` (connect)

**Interfaces:**
- Consumes: `windowStatsWithCoalesce(false)`, `flowCount()`, the channel's stall
  bookkeeping.
- Produces: a populated `ExitMetrics` from a live channel.

- [ ] **Step 1: Investigate the RTT source before writing code.** `GoodputBytesPerSec`,
  `StallEvents` and `Flows` have clean sources today. RTT/Jitter may not. Read the
  send-ack path (`SendDetailedMessage`'s ackCallback, `hasRecentSendAck`) and determine
  whether a send→ack round trip is already timed. **If it is, use it. If it is not, add
  a small EWMA updated on ack completion — do NOT fabricate a value, and do NOT leave
  RTT at zero silently:** `exitScore` sanitizes a zero RTT to the BEST sub-score, so a
  zero RTT is not neutral, it is a maximal bonus. Record what you found in the report.
- [ ] **Step 2: Write the failing test** — a channel with known window stats yields the
  expected `ExitMetrics`; and a channel with NO stats yields metrics that do not score
  as best-in-class (the zero-RTT trap above).
- [ ] **Step 3: Run, confirm fail.**
- [ ] **Step 4: Implement `exitMetricsSnapshot`.** Side-effect-free only.
- [ ] **Step 5: Run to green.**
- [ ] **Step 6: Add the telemetry to the heartbeat line** so the owner can see per-exit
  goodput/rtt without a debugger.
- [ ] **Step 7: Commit** `feat(routing): feed real per-exit telemetry into ExitMetrics`.

---

### Task 4: demotionState ownership

**Files:**
- Modify: `ip_remote_multi_client.go` (connect)
- Test: `routing_demotion_test.go` (connect)

**Interfaces:**
- Consumes: `demotionState` (`routing_score.go`), `PlacementDemoteConsecutive`.
- Produces: per-`(exit, class)` demotion state owned by the multi-client, consulted in
  `scoredPlacementReorder`.

- [ ] **Step 1: Write the failing test** — with `PlacementDemoteConsecutive=3`, an
  incumbent survives 2 bad intervals and is demoted on the 3rd; a good interval in
  between resets the streak. Assert the incumbent is still PRESENT after demotion —
  demotion re-ranks, it never removes.
- [ ] **Step 2: Run, confirm fail.**
- [ ] **Step 3: Implement.** A map keyed by `(clientId, class)` under the parent lock,
  **with eviction when a channel goes away** — an unbounded map keyed by exit identity
  is a leak across a long session with churn.
- [ ] **Step 4: Run to green.**
- [ ] **Step 5: Commit** `feat(routing): own N-of-M demotion state per exit and class`.

---

### Task 5: Reward tap + priors persistence

**Files:**
- Modify: `ip_remote_multi_client.go`, `routing_reward.go` (connect)
- Test: `routing_reward_tap_test.go` (connect)

**Interfaces:**
- Consumes: `rewardAccumulator`, `PriorsStore`, `RewardInstrumentation`.
- Produces: reward samples folded into `ProviderPrior.ScoreEwma` and persisted.

- [ ] **Step 1: Write the failing test** — a flow that completes with good goodput and no
  stalls raises that provider's prior; a flow that stalls lowers it; with
  `RewardInstrumentation=0` nothing is recorded and nothing is persisted.
- [ ] **Step 2: Run, confirm fail.**
- [ ] **Step 3: Implement the tap** at flow completion. Fold into the priors store on a
  timer, NOT per flow — per-flow persistence would write the dot-file at flow-close
  frequency. Persist only the coarse `ProviderPrior` shape that already exists.
- [ ] **Step 4: Run to green + verify `[rel] event=reward` appears.**
- [ ] **Step 5: Commit** `feat(routing): tap flow outcomes into provider priors`.

---

### Task 6: Reconviction decay

**Files:**
- Modify: `ip_remote_multi_client.go` (connect)
- Test: extend the quarantine tests

**Interfaces:**
- Consumes: `quarantineReconvictionCount()`, `benchDuration`, `QuarantineDampening`.

Today the reconviction count only climbs. With `QuarantineDampening` on, an exit that
misbehaved twice hours ago is benched as long as one misbehaving now. This is the
blocker on that knob.

- [ ] **Step 1: Write the failing test** — a reconviction count decays by one step after
  a quiet interval, and reaches zero after enough quiet time; a fresh conviction resets
  the decay clock.
- [ ] **Step 2: Run, confirm fail.**
- [ ] **Step 3: Implement** a decay keyed off the last conviction time.
- [ ] **Step 4: Run to green.**
- [ ] **Step 5: Commit** `fix(routing): decay quarantine reconviction count over quiet time`.

---

### Task 7: End-to-end proof test

**Files:**
- Create: `routing_e2e_test.go` (connect)

One test that turns every knob on against a synthetic window and asserts the whole chain:
classifier names a class → telemetry populates metrics → scorer picks a different exit
than the legacy order → the reward tap records a sample → the prior moves.

- [ ] **Step 1: Write it.** It must FAIL if any single task's knob is turned back off —
  that is what makes it a chain proof rather than five unit tests in a trench coat.
- [ ] **Step 2: Run to green.**
- [ ] **Step 3: Commit** `test(routing): end-to-end proof of the live scoring chain`.

---

### Task 8: Defaults + the owner-visible switch

**Files:**
- Modify: `ip_remote_multi_client.go` (connect)

The ONLY task that changes a default. Turn on: `LightClassifier`, `ScoredPlacement`,
`RewardInstrumentation`, `PlacementHysteresisPct=10`, `PlacementDemoteConsecutive=3`,
`QuarantineDampening` (now that Task 6 exists).

- [ ] **Step 1: Write the failing test** asserting the new defaults, INCLUDING that the
  session banner renders them as `1` / `10.00` / `3`.
- [ ] **Step 2: Run, confirm fail.**
- [ ] **Step 3: Set the defaults.**
- [ ] **Step 4: Run to green.**
- [ ] **Step 5: Commit** `feat(routing): enable class-aware scored placement by default`.
