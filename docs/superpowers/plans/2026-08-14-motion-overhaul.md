# Motion Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the approved motion overhaul
(`docs/superpowers/specs/2026-08-14-motion-overhaul-design.md`): four
primitives added to the UrMotion token system, then every surface
re-choreographed on them — the 760ms Hero Bloom window open (both auth
states), DirectionalSwap page transitions, the connect success bloom, and
micro/carousel unification. One motion language, "branded & cinematic",
across Simple and Advanced.

**Architecture:** Approach A — extend, don't replace. `UrMotion.h/.cpp` is
the one vocabulary (tokens, easings, springs, `ShouldAnimate()`); the four
primitives (`HeroBloom`, `RiseIn`, `RippleGroup`, `DirectionalSwap`) land
there. `WindowReveal` is rewritten from a whole-root spring + flat opacity
ripple into a two-table hero machine (per-auth-state ring tables, hero chosen
at `Arm()` from the tree, union `CancelToFinal`). `AppController` keeps the
show-window sequencing (latch `wasIconic` → place → Arm → Activate → Start →
Reconcile) but loses the reveal's origin plumbing. ConnectPage gains the one
celebratory beat; LoginCarousel and the hero hover join the token system last.
Geometry rides Composition visuals, opacity rides XAML Storyboard DPs —
never compounded alpha, never the HWND.

**Tech Stack:** C++/WinRT, WinUI 3 (Microsoft.UI.Composition +
Microsoft.UI.Xaml.Media.Animation). No new dependencies. Build via
`app\tools\build-local.ps1`.

## Global Constraints

The correctness spine (spec §8) — every task is reviewed against all of it:

- **`ShouldAnimate()`** (UISettings.AnimationsEnabled) gates every animation;
  reduce-motion means instant, fully-correct UI. Unarmed = ZERO property
  writes.
- **AnchorPoint is never set by any code** except to restore `{0,0}`.
  CenterPoint ExpressionAnimations only — AnchorPoint (0.5,0.5) displaces
  content by half its size and shipped as a real bug this week.
- **One union CancelToFinal**: every property any choreography touches has a
  named settled value; the spec's 52-entry set (§8.1) is the baseline and
  later waves extend it, never fork it. `StopAnimation` before every
  Composition write; Storyboards stopped before every XAML write.
- **Geometry on Composition visuals, opacity on XAML Storyboard DPs** — the
  two clocks are started in the same UI-thread call, never spread across
  ticks.
- **Skip-if-Collapsed** for optional elements; never force-Visible a
  Collapsed element; ring Visibility is never touched by CancelToFinal.
- **The reveal is one-shot per show and never plays on un-minimize**
  (`wasIconic` stays the caller's decision, latched BEFORE `SW_RESTORE`).

Process constraints:

- **Line endings: this repo is a MIX of LF and CRLF files.** Before editing
  ANY file, check its endings byte-wise and preserve exactly what it has —
  e.g. `[System.IO.File]::ReadAllText($f).Contains("`r`n")` in PowerShell, or
  `Format-Hex` on a slice. Do not let an editor or tool normalize a file; a
  whole-file-rewrite that flips endings is a rejected diff.
- **Single-writer discipline.** This codebase gives every UI property one
  writing function (`ApplyConnectStatus`, `ApplyStatusStrip`,
  `ApplyLoginLayout`, ...). Choreography may write TRANSIENT values
  (opacity/translation mid-flight) but every settled value must be the
  owner's; never add a second steady-state writer, and never touch
  Visibility that an Apply* function owns.
- **Never run the app or service elevated; never install/start/stop the real
  service.** Nothing in this plan needs either.
- **Build:** `powershell -File app\tools\build-local.ps1` from the repo root;
  expect 0 errors. Implementers ONLY build.
- **The controller session does ALL screenshot verification** (isolated
  `URNETWORK_APP_ROOT` launch + PrintWindow frame burst at ~90ms intervals +
  visual inspection, per spec §9). Every task ends with a CONTROLLER GATE
  checkbox that the implementer does NOT check.

---

## Task 1: UrMotion tokens + HeroBloom + RiseIn + RippleGroup primitives

**Files:**
- `app/src/App/UrMotion.h` (tokens + declarations)
- `app/src/App/UrMotion.cpp` (implementations)

**Interfaces added:** `kBrandBeatMs`, `kHeroHoldMs`, `kHeroScaleFrom`;
`enum class Rise`; `ArmHeroBloom`, `StartHeroBloom`, `EnableTranslation`,
`RiseIn`, `RippleEntry`, `RippleGroup`. Pure additions — no call sites change
in this task, so it is unit-verifiable by compile alone plus the
static_asserts below (the Bind-level asserts land with the tables in
Tasks 2/3).

**Steps:**

- [ ] Check `UrMotion.h` / `UrMotion.cpp` line endings byte-wise; preserve.
- [ ] In `UrMotion.h`, directly under the existing distances block (after
  `inline constexpr double kPressScale = 0.97;`), add the new tokens with
  their relationships compile-checked:

```cpp
// ---- Hero Bloom (motion-overhaul spec §2.1) ---------------------------------
// The held brand beat: the hero owns the frame for ~two visual fixations
// before the unfold. =6*kStaggerMs so it sits on the existing stagger grid;
// shorter reads as no beat, longer reads as a stall.
inline constexpr int64_t kHeroHoldMs = 240;
// =kHeroHoldMs/2 =3*kStaggerMs: the wordmark (and avatar) join mid-hero-settle
// so logo+hero read as one brand moment.
inline constexpr int64_t kBrandBeatMs = 120;
// ~15dip visible travel on the <=190dip globe, ~40dip on the ~512dip art
// card; deep enough to register as the composition's origin, and the 0.86/60
// spring damps both sizes without visible overshoot.
inline constexpr float kHeroScaleFrom = 0.92f;
static_assert(kBrandBeatMs * 2 == kHeroHoldMs, "the brand beat is half the hold");
static_assert(kHeroHoldMs == 6 * kStaggerMs, "the hold sits on the stagger grid");
```

- [ ] In `UrMotion.h`, after the `CrossfadePageSwap` declaration, add the
  primitive declarations:

```cpp
// ---- the four motion primitives (motion-overhaul spec §2.2) -----------------
// All gated on ShouldAnimate(); all keep the house split — geometry on
// Composition visuals, opacity on XAML Storyboard DPs, never compounded
// alpha. UI THREAD ONLY, like everything else in this file.

// Which way a RiseIn element travels as it settles INTO place. Up starts
// distDip BELOW its final slot (Translation.Y = +dist) and settles up; Down
// starts above (-dist) and settles down. The window open uses both: the
// outward bloom.
enum class Rise { Up, Down };

// Hero Bloom, split at the Arm/Start seam the window reveal needs (pose
// written synchronously pre-Activate, animation post-Activate). ArmHeroBloom
// writes the start pose: visual Scale = kHeroScaleFrom, XAML Opacity = 0,
// AnchorPoint {0,0} — ALWAYS {0,0}; (0.5,0.5) displaces content by half its
// size and shipped as v2026.8.13's off-screen bug — and a CenterPoint
// ExpressionAnimation bound to this.Target.Size*0.5, left running (it
// re-evaluates as XAML measures, so the center is right before layout and
// stays right across resize). StartHeroBloom plays the 0.86/60 reveal spring
// on Scale to (1,1,1) under a kHeroMs kStandard opacity fade, and RETURNS the
// fade's Storyboard so the caller can retain and stop it (CancelToFinal must
// release the DP). dampingRatio exists for the one sanctioned override: 0.90
// for the signed-out art card if it visibly wobbles (spec §3.7 risk 7).
void ArmHeroBloom(winrt::Microsoft::UI::Xaml::FrameworkElement const& hero);
winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard StartHeroBloom(
    winrt::Microsoft::UI::Xaml::FrameworkElement const& hero,
    float dampingRatio = kRevealSpringDamping);

// SetIsTranslationEnabled(true), once, idempotent. Forgetting it before the
// first Translation write is a SILENT no-op that reads as "the stagger feels
// flat", never as an error.
void EnableTranslation(winrt::Microsoft::UI::Xaml::UIElement const& element);

// RiseIn: XAML opacity 0->1 over fadeMs riding a Composition Translation.Y
// ±distDip->0 over riseMs — motion outlives alpha (riseMs > fadeMs), so the
// element is readable while it settles the last dip into place. Immediate
// mode: writes its own start pose, enables translation itself, then plays.
// Skip-if-Collapsed lives HERE, at the choke point. distDip 0 degrades to a
// pure fade (how opacity-only riders share the primitive).
void RiseIn(winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
            Rise direction, double distDip, int64_t delayMs,
            int64_t fadeMs = kBaseMs, int64_t riseMs = kSlowMs);

// RippleGroup: staggered RiseIns. Entry i plays at baseDelayMs + i*staggerMs
// by LISTING position — a Collapsed entry is skipped but holds its slot, so
// delays never shift with auth-conditional visibility. staggerMs 0 moves a
// group as one row (the status-row rule: riders move WITH the row or it
// shears).
struct RippleEntry {
  winrt::Microsoft::UI::Xaml::FrameworkElement element{nullptr};
  Rise direction = Rise::Up;
  double distDip = kDist8;
};
void RippleGroup(std::vector<RippleEntry> const& elements, int64_t baseDelayMs,
                 int64_t staggerMs = kStaggerMs);
```

  and add `#include <vector>` beside the existing `#include <cstdint>`.

- [ ] In `UrMotion.cpp`, add the implementations at the end of the namespace
  (after `CrossfadePageSwap`). `anim` and the winrt usings from the file-top
  anonymous namespace are already in scope:

```cpp
void EnableTranslation(winrt::Microsoft::UI::Xaml::UIElement const& element) {
  if (!element) return;
  winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::SetIsTranslationEnabled(
      element, true);
}

void ArmHeroBloom(winrt::Microsoft::UI::Xaml::FrameworkElement const& hero) {
  if (!hero || !ShouldAnimate()) return;
  auto visual =
      winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(hero);
  // AnchorPoint {0,0} ALWAYS; the scale origin is CenterPoint, bound to the
  // visual's own Size (see the header comment for the shipped bug this rule
  // exists to prevent).
  visual.AnchorPoint({0.0f, 0.0f});
  auto centerBind = visual.Compositor().CreateExpressionAnimation(
      L"Vector3(this.Target.Size.X * 0.5f, this.Target.Size.Y * 0.5f, 0.0f)");
  visual.StartAnimation(L"CenterPoint", centerBind);
  visual.Scale({kHeroScaleFrom, kHeroScaleFrom, 1.0f});
  hero.Opacity(0.0);
}

winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard StartHeroBloom(
    winrt::Microsoft::UI::Xaml::FrameworkElement const& hero, float dampingRatio) {
  if (!hero || !ShouldAnimate()) return nullptr;
  auto visual =
      winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(hero);
  auto compositor = visual.Compositor();
  using winrt::Windows::Foundation::Numerics::float3;
  auto spring = compositor.CreateSpringVector3Animation();
  spring.DampingRatio(dampingRatio);
  spring.Period(Ms(kRevealSpringPeriodMs));
  spring.FinalValue(float3{1.0f, 1.0f, 1.0f});
  visual.StartAnimation(L"Scale", spring);

  anim::Storyboard sb;
  auto fade = MakeSplineDouble(0.0, 1.0, kHeroMs, 0, kStandardP1, kStandardP2);
  anim::Storyboard::SetTarget(fade, hero);
  anim::Storyboard::SetTargetProperty(fade, L"Opacity");
  sb.Children().Append(fade);
  sb.Begin();
  return sb;
}

void RiseIn(winrt::Microsoft::UI::Xaml::FrameworkElement const& element, Rise direction,
            double distDip, int64_t delayMs, int64_t fadeMs, int64_t riseMs) {
  namespace xaml = winrt::Microsoft::UI::Xaml;
  if (!element) return;
  // Skip-if-Collapsed, at the choke point: never force-Visible, never pose a
  // hidden element — its owner controls its Visibility.
  if (element.Visibility() == xaml::Visibility::Collapsed) return;
  if (!ShouldAnimate()) return;  // current pose IS the settled pose

  if (distDip != 0.0) {
    using winrt::Windows::Foundation::Numerics::float3;
    EnableTranslation(element);  // idempotent; a silent no-op otherwise
    const float fromY = static_cast<float>(direction == Rise::Up ? distDip : -distDip);
    auto visual = xaml::Hosting::ElementCompositionPreview::GetElementVisual(element);
    element.Translation(float3{0.0f, fromY, 0.0f});  // pre-delay frames correct
    auto rise = visual.Compositor().CreateScalarKeyFrameAnimation();
    rise.InsertKeyFrame(1.0f, 0.0f,
                        MakeCompositionEasing(visual.Compositor(), kStandardP1, kStandardP2));
    rise.Duration(Ms(riseMs));
    if (0 < delayMs) rise.DelayTime(Ms(delayMs));
    visual.StartAnimation(L"Translation.Y", rise);
  }

  // Alpha on the XAML clock (BeginTime = the same delay). Both clocks start
  // in this one call — sub-frame drift is fine, spread starts are not.
  element.Opacity(0.0);
  anim::Storyboard sb;
  auto fade = MakeSplineDouble(0.0, 1.0, fadeMs, delayMs, kStandardP1, kStandardP2);
  anim::Storyboard::SetTarget(fade, element);
  anim::Storyboard::SetTargetProperty(fade, L"Opacity");
  sb.Children().Append(fade);
  sb.Begin();
}

void RippleGroup(std::vector<RippleEntry> const& elements, int64_t baseDelayMs,
                 int64_t staggerMs) {
  int64_t slot = 0;
  for (auto const& entry : elements) {
    // slot advances for EVERY listed entry, present or not: delays are a
    // property of the composition, not of what happens to be visible.
    RiseIn(entry.element, entry.direction, entry.distDip, baseDelayMs + slot * staggerMs);
    ++slot;
  }
}
```

- [ ] Build: `powershell -File app\tools\build-local.ps1` — expect 0 errors.
- [ ] CONTROLLER GATE: frame-burst capture + visual inspection. (Implementer
  does NOT check this box.)

## Task 2: MainWindow.xaml x:Names + the two reveal ring tables

**Files:**
- `app/src/App/MainWindow.xaml` (two x:Name additions, nothing structural)
- `app/src/App/WindowReveal.h` (new ring/table types + Bind signature)
- `app/src/App/WindowReveal.cpp` (Bind stores the tables; transitional
  Arm/Start/CancelToFinal so this task builds and runs green on its own)
- `app/src/App/MainWindow.xaml.cpp` (the Bind call, lines 87–97)

**Interfaces:** `RevealRing` gains `riseDip/fadeMs/riseMs`; new `RevealState`;
`Bind(plate, revealRoot, homeNav, loginRoot, signedIn, signedOut)`. Arm/Start
keep their current signatures until Task 3 — this task's .cpp changes are the
minimal transitional versions that keep today's behavior (root spring + flat
ring fades, now at the new delays) so the build stays green and the tables are
Bind-level-asserted early.

**Steps:**

- [ ] Check line endings of all four files byte-wise; preserve per-file.
- [ ] `MainWindow.xaml`: name the two currently-anonymous panels (spec §3.5).
  The email group (currently line ~147):

```xml
                    <StackPanel Spacing="0">
```
  becomes
```xml
                    <StackPanel x:Name="EmailGroup" Spacing="0">
```
  and the seedphrase pair (currently line ~268):
```xml
                    <StackPanel Spacing="8" Margin="0,16,0,0">
```
  becomes
```xml
                    <StackPanel x:Name="SecondaryAuthRow" Spacing="8" Margin="0,16,0,0">
```
  No other XAML change in this task.

- [ ] `WindowReveal.h`: replace the `RevealRing` struct and the `Bind`
  declaration (Arm/Start/CancelToFinal declarations stay for now), and add
  the members. `#include "UrMotion.h"` joins the includes:

```cpp
// One ripple-ring element: signed rise start in dip (+ = starts below its
// final slot and settles UP; − = starts above and settles DOWN; 0 =
// opacity-only), the delay BOTH of its clocks start at, and its two
// durations. See MainWindow's constructor for the two per-state tables.
struct RevealRing {
  winrt::Microsoft::UI::Xaml::FrameworkElement element{nullptr};
  float riseDip = 0.0f;
  int64_t delayMs = 0;
  int64_t fadeMs = urnw::motion::kBaseMs;
  int64_t riseMs = urnw::motion::kSlowMs;
};

// One first frame: the hero that blooms and the rings that unfold around it.
struct RevealState {
  winrt::Microsoft::UI::Xaml::FrameworkElement hero{nullptr};
  std::vector<RevealRing> rings;
};
```

```cpp
  // Bind once, right after the window's content exists. Two tables, one
  // machine: Arm() picks signedIn/signedOut from the tree's own
  // HomeNav/LoginRoot visibility. `revealRoot` remains bound ONLY for the
  // legacy defensive restore in CancelToFinal — the root itself no longer
  // animates after Task 3.
  void Bind(winrt::Microsoft::UI::Xaml::FrameworkElement const& plate,
            winrt::Microsoft::UI::Xaml::FrameworkElement const& revealRoot,
            winrt::Microsoft::UI::Xaml::FrameworkElement const& homeNav,
            winrt::Microsoft::UI::Xaml::FrameworkElement const& loginRoot,
            RevealState signedIn, RevealState signedOut);
```

  members become:

```cpp
 private:
  winrt::Microsoft::UI::Xaml::FrameworkElement plate_{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement revealRoot_{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement homeNav_{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement loginRoot_{nullptr};
  RevealState signedIn_;
  RevealState signedOut_;
  bool signedInArmed_ = false;  // which table Arm() latched
  bool armed_ = false;
  // Retained so CancelToFinal can STOP them: a running Storyboard holds its
  // DP, and the union restore must release it before writing.
  std::vector<winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard> boards_;
```

  (add `#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>`; `<optional>`
  and `<windows.h>` stay until Task 4 removes the old Arm signature).

- [ ] `WindowReveal.cpp`: new `Bind` — stores everything, enables translation
  once per translated element, asserts the table invariants (`#include
  <cassert>`):

```cpp
void WindowReveal::Bind(winrt::Microsoft::UI::Xaml::FrameworkElement const& plate,
                        winrt::Microsoft::UI::Xaml::FrameworkElement const& revealRoot,
                        winrt::Microsoft::UI::Xaml::FrameworkElement const& homeNav,
                        winrt::Microsoft::UI::Xaml::FrameworkElement const& loginRoot,
                        RevealState signedIn, RevealState signedOut) {
  plate_ = plate;
  revealRoot_ = revealRoot;
  homeNav_ = homeNav;
  loginRoot_ = loginRoot;
  signedIn_ = std::move(signedIn);
  signedOut_ = std::move(signedOut);
  // SetIsTranslationEnabled ONCE per translated element, here — forgetting it
  // makes every Translation write a silent no-op that reads as "the stagger
  // feels flat", never as an error (spec §3.7 risk 3).
  for (auto const* state : {&signedIn_, &signedOut_}) {
    for (auto const& ring : state->rings) {
      if (ring.element && ring.riseDip != 0.0f) urnw::motion::EnableTranslation(ring.element);
    }
  }
#ifdef _DEBUG
  // The ancestor-alpha rule, enforced where the tables land (spec §3.7 risk
  // 1): HomeNav is the signed-in hero's alpha ancestor — stage beat only,
  // opacity-only, never delayed, never translated. LoginRoot (the signed-out
  // hero's ancestor) is never listed at all.
  for (auto const& ring : signedIn_.rings) {
    if (ring.element == homeNav_) {
      assert(ring.riseDip == 0.0f && ring.delayMs == 0 &&
             "HomeNav is the STAGE: opacity-only, delay 0");
    }
  }
  for (auto const* state : {&signedIn_, &signedOut_}) {
    for (auto const& ring : state->rings) {
      assert(ring.element != loginRoot_ && "LoginRoot must never be animated");
    }
  }
#endif
}
```

- [ ] `WindowReveal.cpp`, transitional `Arm`/`Start`/`CancelToFinal` (full
  choreography is Task 3; this keeps the app green): `Arm` keeps its 3-arg
  signature and its root-spring/origin logic verbatim, but replaces the old
  flat `rings_` loop with the ACTIVE table, latched from the tree:

```cpp
  signedInArmed_ = homeNav_ && homeNav_.Visibility() == Visibility::Visible;
  auto const& active = signedInArmed_ ? signedIn_ : signedOut_;
  for (auto const& ring : active.rings) {
    if (!ring.element) continue;
    if (ring.element.Visibility() == Visibility::Collapsed) continue;  // spec §3.7 risk 4
    ring.element.Opacity(0.0);
  }
```

  `Start` keeps the plate fade + root spring verbatim and swaps the ring loop
  to the active table with per-ring `fadeMs` (`ring.delayMs` as BeginTime),
  dropping the old `ring.element.Visibility(Visibility::Visible)` write
  (never force-Visible), and retaining every board in `boards_`.
  `CancelToFinal` keeps the plate/root restore verbatim and replaces the
  `rings_` loop with the UNION: stop + clear `boards_` first, then
  `Opacity(1.0)` over `signedIn_.rings`, `signedOut_.rings`, and both heroes
  (`if (signedIn_.hero) signedIn_.hero.Opacity(1.0);` etc.).

- [ ] `MainWindow.xaml.cpp` lines 87–97: replace the old Bind call with the
  two tables (spec §3.2/§3.3 — every entry, in beat order):

```cpp
  // The window reveal (motion overhaul, Surface 1 "Hero Bloom"): two
  // per-state tables, one machine — Arm() picks the table from the tree's
  // own HomeNav/LoginRoot visibility. Ring = {element, riseDip (+ starts
  // below final and settles UP, − above and settles DOWN, 0 opacity-only),
  // delayMs, fadeMs, riseMs}. Delays sit on the stagger grid:
  // kBrandBeatMs=120, kHeroHoldMs=240, then +kStaggerMs steps to 360.
  //
  // HomeNav is the STAGE, not a mover: it is the signed-in hero's alpha
  // ancestor, so its XAML opacity CAPS every descendant (products compound).
  // Opacity-only, delay 0, kFastMs — and LoginRoot/LoginPanel (signed-out
  // hero ancestors) are never listed at all. Bind() asserts both.
  {
    namespace mo = urnw::motion;
    urnw::RevealState signedIn{
        ConnectCanvasHost(),
        {
            {HomeNav(), 0.0f, 0, mo::kFastMs, 0},
            {AppTitleBar(), +8.0f, mo::kBrandBeatMs, mo::kBaseMs, mo::kSlowMs},
            {AccountMenuButton(), +8.0f, mo::kBrandBeatMs, mo::kBaseMs, mo::kSlowMs},
            {StatusDot(), +8.0f, mo::kHeroHoldMs, mo::kBaseMs, mo::kSlowMs},
            {StatusText(), +8.0f, mo::kHeroHoldMs, mo::kBaseMs, mo::kSlowMs},
            {ProtectionText(), +8.0f, mo::kHeroHoldMs, mo::kBaseMs, mo::kSlowMs},
            {TrafficHeldText(), +8.0f, mo::kHeroHoldMs, mo::kBaseMs, mo::kSlowMs},
            {StatusReasonText(), +8.0f, mo::kHeroHoldMs, mo::kBaseMs, mo::kSlowMs},
            {LocationRow(), -8.0f, mo::kHeroHoldMs, mo::kBaseMs, mo::kSlowMs},
            {ConnectButton(), -8.0f, mo::kHeroHoldMs + mo::kStaggerMs, mo::kBaseMs, mo::kSlowMs},
            {StatusStrip(), -12.0f, mo::kHeroHoldMs + 3 * mo::kStaggerMs, mo::kBaseMs,
             mo::kSlowMs},
            // data panes: opacity-only (large surfaces never slide), the
            // closing beat; the 1px rules stay unringed as skeleton
            {ConnectPaneB(), 0.0f, mo::kHeroHoldMs + 3 * mo::kStaggerMs, mo::kSlowMs, 0},
            {ConnectPaneC(), 0.0f, mo::kHeroHoldMs + 3 * mo::kStaggerMs, mo::kSlowMs, 0},
        }};
    urnw::RevealState signedOut{
        LoginCarouselHost(),
        {
            {AppTitleBar(), +8.0f, mo::kBrandBeatMs, mo::kBaseMs, mo::kSlowMs},
            {EmailGroup(), -8.0f, mo::kHeroHoldMs, mo::kBaseMs, mo::kSlowMs},
            {GetStartedButton(), -8.0f, mo::kHeroHoldMs + mo::kStaggerMs, mo::kBaseMs,
             mo::kSlowMs},
            {OrDivider(), -8.0f, mo::kHeroHoldMs + mo::kStaggerMs, mo::kBaseMs, mo::kSlowMs},
            {GoogleSignInButton(), -12.0f, mo::kHeroHoldMs + 2 * mo::kStaggerMs, mo::kBaseMs,
             mo::kSlowMs},
            {BittensorSignInButton(), -12.0f, mo::kHeroHoldMs + 2 * mo::kStaggerMs,
             mo::kBaseMs, mo::kSlowMs},
            {SolanaSignInButton(), -12.0f, mo::kHeroHoldMs + 2 * mo::kStaggerMs, mo::kBaseMs,
             mo::kSlowMs},
            {AuthCodeButton(), -12.0f, mo::kHeroHoldMs + 2 * mo::kStaggerMs, mo::kBaseMs,
             mo::kSlowMs},
            {SecondaryAuthRow(), -12.0f, mo::kHeroHoldMs + 3 * mo::kStaggerMs, mo::kBaseMs,
             mo::kSlowMs},
            {NetworkServerLink(), -12.0f, mo::kHeroHoldMs + 3 * mo::kStaggerMs, mo::kBaseMs,
             mo::kSlowMs},
        }};
    reveal_.Bind(WindowPlate(), RevealRoot(), HomeNav(), LoginRoot(),
                 std::move(signedIn), std::move(signedOut));
  }
```

- [ ] Build: `powershell -File app\tools\build-local.ps1` — expect 0 errors.
- [ ] CONTROLLER GATE: frame-burst capture + visual inspection. (Implementer
  does NOT check this box.)

## Task 2a: Wide login layout — the signed-out screen's desktop reading

The signed-out screen has NO wide layout today: `LoginRoot`'s inner Grid
(MainWindow.xaml:99–103) has no ColumnDefinitions, `LoginPanel` is a
MaxWidth=512 centred column at every width, and `ApplyBreakpoint`
(MainWindow.xaml.cpp:393) never touches the login tree. At the app-wide
`kWideBreakpointDip` (UrComponents.h, 1000dip) that is ~359dip of empty
plate per side (~58% of the window, measured on a 1230dip-wide window when
the bug was logged), with the art capped at 220dip (`kGlobeMaxSide`,
LoginCarousel.cpp:61). The Hero Bloom signed-out would bloom a small card
in an ocean of empty plate — so this lands in Wave 1, before the
choreography is judged. The Hero Bloom timeline itself is UNCHANGED: same
`LoginCarouselHost` handle, same beats — the bloom simply happens in the
wide-pane position when wide. Below `kWideBreakpointDip` the layout must
remain EXACTLY today's.

**Files:**
- `app/src/App/MainWindow.xaml` (login Grid columns + art pane)
- `app/src/App/MainWindow.xaml.cpp` (`ApplyBreakpoint` login block + a
  one-element reparent helper)
- `app/src/App/LoginPage.cpp` (`ApplyLoginLayout` wide branch; `ShowLoginStep`
  call site)
- `app/src/App/LoginCarousel.cpp` (`kGlobeMaxSide` 220 → 400)

**Interfaces:** new x:Names `LoginArtColumn`, `LoginFormColumn`,
`LoginArtPane`; file-local `ReparentTo` helper. No public API changes.

**Steps:**

- [ ] Check line endings of all four files byte-wise; preserve per-file.
- [ ] `MainWindow.xaml`: give the login Grid (the direct child of the
  `LoginRoot` ScrollViewer, currently a bare `<Grid>`) two named columns and
  an art pane, and pin every existing child to the form column:

```xml
            <Grid>
                <Grid.ColumnDefinitions>
                    <!-- wide login (motion overhaul Task 2a): art | form.
                         Narrow keeps the art column at 0 and the form on the
                         star, which renders EXACTLY the old single-column
                         layout; ApplyBreakpoint owns the flip at the app-wide
                         kWideBreakpointDip (UrComponents.h, 1000dip) — the
                         same gate every other pane group in ApplyBreakpoint
                         keys off. -->
                    <ColumnDefinition x:Name="LoginArtColumn" Width="0" />
                    <ColumnDefinition x:Name="LoginFormColumn" Width="*" />
                </Grid.ColumnDefinitions>
                <!-- the wide art pane: empty until ApplyBreakpoint reparents
                     LoginCarouselHost here at >=kWideBreakpointDip (1000dip).
                     Padding keeps the globe off the window edge and the pane
                     rule of thumb (content never touches chrome). -->
                <Grid x:Name="LoginArtPane" Grid.Column="0" Padding="24"
                      Visibility="Collapsed" />
```

  then add `Grid.Column="1"` to each of the seven step panels' root elements
  — `LoginPanel`, `PasswordPanel`, `CreatePanel`, `VerifyPanel`,
  `SeedphrasePanel`, `InstantPanel`, `ResetPanel` (e.g.
  `<StackPanel x:Name="LoginPanel" Grid.Column="1" Margin="16" ...>`).
  Nothing else in the login tree changes.

- [ ] `MainWindow.xaml.cpp`: add the one reparent helper to the anonymous
  namespace beside `Place`/`SetWidth` (~line 48). The old Reparent dance died
  with R3 (see the comment at lines 63–69); this is deliberately a
  one-element, two-parent version, not its return:

```cpp
// Move `child` under `parent`, if it is not already there. The ONE reparent
// left in this window (R3 deleted the general dance): the login art moves
// between the form column's flow (narrow: first child of LoginPanel) and the
// wide art pane. Index is clamped, so "first" is stable even if the panel's
// child list changes around it.
void ReparentTo(winrt::Microsoft::UI::Xaml::UIElement const& child,
                Controls::Panel const& parent, uint32_t index) {
  if (!child || !parent) return;
  auto current = child.try_as<FrameworkElement>().Parent().try_as<Controls::Panel>();
  if (current == parent) return;
  if (current) {
    uint32_t at = 0;
    if (current.Children().IndexOf(child, at)) current.Children().RemoveAt(at);
  }
  parent.Children().InsertAt((std::min)(index, parent.Children().Size()), child);
}
```

- [ ] `MainWindow.xaml.cpp`, `ApplyBreakpoint`: insert the login block after
  the Developer block (after the `Place(DeveloperSideStack(), ...)` statement
  at ~line 589), using the same wide flag Support/Developer key off:

```cpp
  // ---- Login: the art beside the form at desktop widths --------------------
  // The seven groups above all skip the login tree, so signed out past the
  // app-wide kWideBreakpointDip (UrComponents.h, 1000dip) ~58% of the window
  // was empty plate and the carousel read as a small card floating in
  // blackness (512+32dip column, ~359dip of empty background per side,
  // measured on a 1230dip-wide window at the time). Wide: art pane takes the
  // star, the form column takes its content width (LoginPanel MaxWidth 512 +
  // 16+16 margin). Narrow: EXACTLY today's — art column 0, form on the star,
  // the host back at the top of LoginPanel's flow. LoginPage::ApplyLoginLayout
  // knows which parent the host has and only runs its elastic-height
  // arithmetic in the narrow one.
  //
  // An earlier draft of this comment (and the plan doc it came from) called
  // 1230dip "the wide breakpoint" — it never was one. 1230 is only the width
  // the window happened to be in the log line that reported this bug
  // ("layout: wide at 1230dip"); the actual gate has always been
  // kWideBreakpointDip, same as Support and Developer above. Reference the
  // constant, not the number, so this can't drift from the code again.
  if (wide) {
    SetStar(LoginArtColumn(), 1);
    SetWidth(LoginFormColumn(), 544);
    ReparentTo(LoginCarouselHost(), LoginArtPane(), 0);
    LoginArtPane().Visibility(Visibility::Visible);
  } else {
    LoginArtPane().Visibility(Visibility::Collapsed);
    ReparentTo(LoginCarouselHost(), LoginPanel(), 0);
    SetWidth(LoginArtColumn(), 0);
    SetStar(LoginFormColumn(), 1);
  }
```

  The existing `LoginRoot`/`LoginPanel` SizeChanged hooks (LoginPage.cpp:113–
  118) re-run `ApplyLoginLayout` after the reparent — no new call site.

- [ ] `LoginPage.cpp`, `ApplyLoginLayout` (line 134): add the wide branch
  BEFORE the panel-visibility early-out, so the art pane's host mirrors the
  initial step even when `LoginPanel` is collapsed:

```cpp
  if (inLayoutPass_) return;  // re-entrancy: setting a height raises SizeChanged
  // Wide login (Task 2a): the host lives in LoginArtPane and the PANE sizes
  // it — the elastic-slot arithmetic below is the narrow column's, and its
  // 200dip cap must not squash the art pane. The host mirrors LoginPanel's
  // visibility (the art belongs to the initial step, exactly as it did when
  // it lived inside the panel), and the carousel gate keys off it as before.
  if (host.Parent().try_as<StackPanel>() != panel) {
    inLayoutPass_ = true;
    host.ClearValue(FrameworkElement::HeightProperty());
    host.Visibility(panel.Visibility());
    inLayoutPass_ = false;
    UpdateCarouselRunning();
    return;
  }
```

  and in `ShowLoginStep` (line ~404), make the layout call unconditional so
  the wide branch also runs when LEAVING the initial step (narrow behavior is
  unchanged — the panel-visibility early-out fires exactly where the old
  `if` did):

```cpp
  ApplyLoginLayout();  // wide: mirrors the art pane on every step; narrow: early-outs as before
```

- [ ] `LoginCarousel.cpp` line 61: raise the art cap for the wide slot.
  Narrow is unaffected: `ApplyMetrics` sizes to
  `min(slotW, slotH, kGlobeMaxSide)` (line 245) and the narrow slot is ≤200
  tall, so the slot governs there:

```cpp
// The globe never grows past this, and the type is derived from whatever the
// globe actually ends up at (ApplyMetrics), so the headline stays INSIDE the
// mask at every slot size. 400, not 220, since the wide login pane (Task 2a):
// the narrow slot (<=200dip tall) still governs itself, and past 400 the
// JPEG art goes soft before the pane runs out of room.
constexpr double kGlobeMaxSide = 400;
```

- [ ] Build: `powershell -File app\tools\build-local.ps1` — expect 0 errors.
- [ ] CONTROLLER GATE: frame-burst capture + visual inspection — BOTH sides
  of `kWideBreakpointDip` (1000dip), plus a resize across the breakpoint
  mid-carousel, plus the narrow layout pixel-compared against a pre-change
  capture (it must be identical). (Implementer does NOT check this box.)

## Task 3: WindowReveal rewrite — the Hero Bloom machine

**Files:**
- `app/src/App/WindowReveal.h` (Arm(bool); doc comments)
- `app/src/App/WindowReveal.cpp` (the full choreography)
- `app/src/App/MainWindow.xaml.cpp` (`ArmReveal(bool)` wrapper; mid-reveal
  cancel on the root swap)
- `app/src/App/MainWindow.xaml.h` (wrapper declaration)

**Interfaces:** `void Arm(bool enabled)` replaces the origin/rect form; a
deprecated 3-arg forwarding shim keeps AppController compiling until Task 4
deletes both ends. `kOffsetDip` and all dirX/origin logic are deleted — this
is where the JSON's "delete kOffsetDip" lands (the constant lives in
WindowReveal.cpp's anonymous namespace, not UrMotion.h).

**Steps:**

- [ ] Check line endings byte-wise; preserve per-file.
- [ ] `WindowReveal.h`: replace the Arm declarations:

```cpp
  // Write the START pose synchronously, BEFORE Window.Activate() — the reveal
  // must add zero latency, so the first composed frame is already correct.
  // Arm derives SignedIn/SignedOut ITSELF from the current HomeNav/LoginRoot
  // visibility: it runs synchronously pre-Activate, after every root-
  // visibility write, so the tree already reflects auth state and there is no
  // state parameter to race. `enabled` folds together every caller-side
  // reason not to reveal; "this is an un-minimize, which the OS's own restore
  // animation owns" stays the CALLER's decision (wasIconic latched before
  // SW_RESTORE). ShouldAnimate() is checked here: unarmed = zero property
  // writes = instant, fully-correct UI.
  void Arm(bool enabled);
  // TRANSITIONAL (deleted in Task 4 with its caller): forwards to Arm(bool);
  // the origin/rect are dead — no direction decision remains.
  void Arm(bool enabled, std::optional<POINT>, RECT const&) { Arm(enabled); }
```

- [ ] `WindowReveal.cpp`: delete `kOffsetDip` and the whole dirX/origin block;
  replace Arm/Start with the bloom (transitional Task 2 bodies give way to
  the final ones):

```cpp
void WindowReveal::Arm(bool enabled) {
  if (!plate_ || !homeNav_ || !loginRoot_) return;
  // A reveal still running from a PREVIOUS show settles before this one
  // writes its own start pose — two overlapping springs on the same visual is
  // how E6's "nastiest failure mode" starts.
  CancelToFinal();
  armed_ = enabled && urnw::motion::ShouldAnimate();
  if (!armed_) return;

  // Which first frame is this? Read the tree, don't be told.
  signedInArmed_ = homeNav_.Visibility() == Visibility::Visible;
  auto const& state = signedInArmed_ ? signedIn_ : signedOut_;

  ElementCompositionPreview::GetElementVisual(plate_).Opacity(0.0f);
  // Hero pose: Scale kHeroScaleFrom, XAML opacity 0, AnchorPoint {0,0}
  // ALWAYS, CenterPoint expression bound to Size (left running).
  urnw::motion::ArmHeroBloom(state.hero);
  for (auto const& ring : state.rings) {
    if (!ring.element) continue;
    if (ring.element.Visibility() == Visibility::Collapsed) continue;
    ring.element.Opacity(0.0);
    if (ring.riseDip != 0.0f) {
      ring.element.Translation({0.0f, ring.riseDip, 0.0f});
    }
  }
}

void WindowReveal::Start() {
  if (!armed_ || !plate_) return;
  auto const& state = signedInArmed_ ? signedIn_ : signedOut_;
  auto plateVisual = ElementCompositionPreview::GetElementVisual(plate_);
  auto compositor = plateVisual.Compositor();
  auto standard = urnw::motion::MakeCompositionEasing(compositor, urnw::motion::kStandardP1,
                                                      urnw::motion::kStandardP2);

  // The stage's floor: plate alpha 0->1 over kFastMs, kStandard — unchanged
  // from the shipped reveal.
  auto plateFade = compositor.CreateScalarKeyFrameAnimation();
  plateFade.InsertKeyFrame(1.0f, 1.0f, standard);
  plateFade.Duration(urnw::motion::Ms(urnw::motion::kFastMs));
  plateVisual.StartAnimation(L"Opacity", plateFade);

  // The hero: 0.86/60 spring on Scale under a kHeroMs fade. The returned
  // board is retained so CancelToFinal can release the hero's Opacity DP.
  if (auto heroBoard = urnw::motion::StartHeroBloom(state.hero)) boards_.push_back(heroBoard);

  // The rings: two clocks per entry, BOTH started inside this one UI-thread
  // call — rises on Composition DelayTime, fades on Storyboard BeginTime.
  // Sub-frame drift between the clocks is fine; spreading starts across
  // ticks is not (spec §3.7 risk 6).
  for (auto const& ring : state.rings) {
    if (!ring.element) continue;
    // Same guard as Arm — and NEVER a Visibility write: a Collapsed ring
    // element belongs to its owner (spec §3.7 risk 4).
    if (ring.element.Visibility() == Visibility::Collapsed) continue;
    if (ring.riseDip != 0.0f) {
      auto visual = ElementCompositionPreview::GetElementVisual(ring.element);
      auto rise = compositor.CreateScalarKeyFrameAnimation();
      rise.InsertKeyFrame(1.0f, 0.0f, standard);
      rise.Duration(urnw::motion::Ms(ring.riseMs));
      if (0 < ring.delayMs) rise.DelayTime(urnw::motion::Ms(ring.delayMs));
      visual.StartAnimation(L"Translation.Y", rise);
    }
    anim::Storyboard sb;
    auto fade = urnw::motion::MakeSplineDouble(0.0, 1.0, ring.fadeMs, ring.delayMs,
                                               urnw::motion::kStandardP1,
                                               urnw::motion::kStandardP2);
    anim::Storyboard::SetTarget(fade, ring.element);
    anim::Storyboard::SetTargetProperty(fade, L"Opacity");
    sb.Children().Append(fade);
    sb.Begin();
    boards_.push_back(sb);
  }
  armed_ = false;  // one-shot: a second Start() without an Arm() does nothing
}
```

- [ ] `WindowReveal.cpp`, the union `CancelToFinal` — the spec's 52-entry set
  (§8.1), stop-then-write throughout, Visibility never touched, CenterPoint
  expressions left running:

```cpp
void WindowReveal::CancelToFinal() {
  if (!plate_) return;
  using winrt::Windows::Foundation::Numerics::float3;
  // Boards first: a running Storyboard HOLDS its DP; stop releases it so the
  // XAML writes below actually land.
  for (auto const& board : boards_) board.Stop();
  boards_.clear();

  // BOTH heroes, unconditionally — the restore is auth-state-independent, so
  // it survives an auth flip between Arm and Cancel. CenterPoint expressions
  // are deliberately left running: they evaluate to the settled center.
  auto settleHero = [](winrt::Microsoft::UI::Xaml::FrameworkElement const& hero) {
    if (!hero) return;
    auto visual = ElementCompositionPreview::GetElementVisual(hero);
    visual.StopAnimation(L"Scale");
    visual.AnchorPoint({0.0f, 0.0f});
    visual.Scale({1.0f, 1.0f, 1.0f});
    hero.Opacity(1.0);
  };
  settleHero(signedIn_.hero);
  settleHero(signedOut_.hero);

  // Legacy defensive: the root no longer animates, but a prior build's
  // spring may have left Scale/Offset behind, and "cancel to final" includes
  // the origin geometry is measured from.
  if (revealRoot_) {
    auto rootVisual = ElementCompositionPreview::GetElementVisual(revealRoot_);
    rootVisual.StopAnimation(L"Scale");
    rootVisual.StopAnimation(L"Offset");
    rootVisual.AnchorPoint({0.0f, 0.0f});
    rootVisual.Scale({1.0f, 1.0f, 1.0f});
    rootVisual.Offset({0.0f, 0.0f, 0.0f});
  }

  auto plateVisual = ElementCompositionPreview::GetElementVisual(plate_);
  plateVisual.StopAnimation(L"Opacity");
  plateVisual.Opacity(1.0f);

  // The UNION of both tables regardless of which armed: opacity 1.0 /
  // Translation 0 on Collapsed elements is inert, and Visibility is NEVER
  // touched — step panels and home views own their own.
  auto settleRing = [](RevealRing const& ring) {
    if (!ring.element) return;
    if (ring.riseDip != 0.0f) {
      auto visual = ElementCompositionPreview::GetElementVisual(ring.element);
      visual.StopAnimation(L"Translation.Y");
      ring.element.Translation(float3{0.0f, 0.0f, 0.0f});
    }
    ring.element.Opacity(1.0);
  };
  for (auto const& ring : signedIn_.rings) settleRing(ring);
  for (auto const& ring : signedOut_.rings) settleRing(ring);

  armed_ = false;
}
```

- [ ] `WindowReveal.cpp`: rewrite the file-top comment — E1's "the HWND never
  animates" and the plate rationale stay; the E2 origin-direction paragraphs
  and the AnchorPoint war story are condensed to the ban + the pointer to
  `ArmHeroBloom` (the mechanism now lives in UrMotion).
- [ ] `MainWindow.xaml.h` line 53: add `void ArmReveal(bool enabled);`
  (keep the 3-arg overload beside it until Task 4).
  `MainWindow.xaml.cpp` (~line 324): `void MainWindow::ArmReveal(bool
  enabled) { reveal_.Arm(enabled); }` — the 3-arg wrapper forwards to it.
- [ ] `MainWindow.xaml.cpp`, `ApplyAuthState` (line ~1595): a root swap under
  a reveal in flight must not inherit half-animated rings (spec §3.7 risk 9;
  the union restore makes even a missed call safe, but do not miss it).
  Insert between the `showHome` computation and the two Visibility writes:

```cpp
  // A login<->home swap mid-reveal settles the reveal first: no element may
  // be left translated/transparent under the page entrance that follows.
  // Only ShowWindowImpl ever STARTS a reveal; this only ends one early.
  if (showHome != wasVisible) reveal_.CancelToFinal();
```

  and the same one-line guard at the top of `ShowLoginRoot()` /
  `ShowHomeRoot()` (lines 1005/1011), conditioned on the visibility actually
  changing:

```cpp
  if (LoginRoot().Visibility() != Visibility::Visible) reveal_.CancelToFinal();   // ShowLoginRoot
  if (HomeNav().Visibility() != Visibility::Visible) reveal_.CancelToFinal();     // ShowHomeRoot
```

- [ ] Build: `powershell -File app\tools\build-local.ps1` — expect 0 errors.
- [ ] CONTROLLER GATE: frame-burst capture + visual inspection — the full
  760ms open, BOTH auth states, wide and narrow, plus reduce-motion, plus
  hide-to-tray mid-bloom then immediate re-open (the CancelToFinal path).
  (Implementer does NOT check this box.)

## Task 4: AppController call-site cleanup

**Files:**
- `app/src/App/AppController.cpp` (`ShowWindowImpl`, ~lines 747–777)
- `app/src/App/MainWindow.xaml.h` / `.cpp` (delete the 3-arg wrapper)
- `app/src/App/WindowReveal.h` (delete the shim; drop `<optional>` /
  `<windows.h>` includes)

**Steps:**

- [ ] Check line endings byte-wise; preserve per-file.
- [ ] `AppController.cpp`: the `wasIconic` latch-BEFORE-`SW_RESTORE` (line
  729), the Arm-before-`Activate()`, the `StartReveal()` after, and
  `ReconcileWindowPresentation()` LAST all stay exactly where they are — the
  idle pulse must begin under an already-blooming hero, and the reveal is
  triggered ONLY from this path, never from Reconcile's replay handlers. The
  tray-anchor Move/clamp block (lines 669–717) stays: it is PLACEMENT, and
  always also was. What goes is the reveal's origin plumbing. Replace lines
  755–771 with:

```cpp
  // The window reveal: Arm() writes the start pose BEFORE Activate(), so the
  // first composed frame is already correct and the reveal adds ZERO
  // latency; Start() runs after. Never on an un-minimize (wasIconic above —
  // the OS's own restore animation owns that moment) and never from anywhere
  // but here. Hero Bloom needs no origin: the tray anchor above is PLACEMENT
  // only, and the reveal's only geometry is the hero's centred bloom plus
  // the per-element rises — GetIconRect, the click-point fallback and the
  // window rect all left with dirX.
  if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
    self->ArmReveal(!wasIconic);
  }
```

- [ ] Delete the 3-arg `ArmReveal` from MainWindow.xaml.h/.cpp and the
  forwarding `Arm` shim from WindowReveal.h; drop WindowReveal.h's now-unused
  `#include <optional>` and `#include <windows.h>`. `TrayIcon::GetIconRect`
  itself stays (the API is not this plan's to remove); only its reveal-path
  caller is gone.
- [ ] Grep-verify no survivor references:
  `originScreen|windowScreenRect|kOffsetDip|dirX` must have zero hits under
  `app/src/App`.
- [ ] Build: `powershell -File app\tools\build-local.ps1` — expect 0 errors.
- [ ] CONTROLLER GATE: frame-burst capture + visual inspection — tray-click
  open (bloom plays), minimize→tray-click (NO bloom: the wasIconic
  exclusion), reduce-motion open. Wave 1 beta checkpoint build for the owner
  after this gate. (Implementer does NOT check this box.)

## Task 5 (Wave 2): DirectionalSwap + call sites + drawer

**Files:**
- `app/src/App/UrMotion.h` / `.cpp` (the primitive; delete
  `CrossfadePageSwap` + `RunCrossfade` once call sites are moved)
- `app/src/App/MainWindow.xaml.cpp` (three call sites: 1088, 1159, 1647)
- `app/src/App/ConnectPage.h` (stale comment at line ~73)

**Interfaces:** `void DirectionalSwap(FrameworkElement const& outgoing,
FrameworkElement const& incoming);` — CrossfadePageSwap's exact contract
(null outgoing = first entrance; outgoing == incoming no-ops; reduce-motion =
instant swap), plus vertical language.

**Steps:**

- [ ] Check line endings byte-wise; preserve per-file.
- [ ] `UrMotion.h`: declare beside the other primitives:

```cpp
// DirectionalSwap: the page transition (motion-overhaul Surface 2), replacing
// the flat CrossfadePageSwap. The incoming page IS a RiseIn (up, kDist8 —
// which also covers the drawer's null-outgoing first entrance); the outgoing
// page fades DOWN-and-out in kFastMs on the Exit easing — one step faster,
// dismissing must never feel slower than presenting. Same contract as the
// crossfade it replaces: safe with outgoing == incoming, falls back to an
// instant swap when ShouldAnimate() is false.
void DirectionalSwap(winrt::Microsoft::UI::Xaml::FrameworkElement const& outgoing,
                     winrt::Microsoft::UI::Xaml::FrameworkElement const& incoming);
```

- [ ] `UrMotion.cpp`: implement:

```cpp
void DirectionalSwap(winrt::Microsoft::UI::Xaml::FrameworkElement const& outgoing,
                     winrt::Microsoft::UI::Xaml::FrameworkElement const& incoming) {
  namespace xaml = winrt::Microsoft::UI::Xaml;
  using winrt::Windows::Foundation::Numerics::float3;
  if (!incoming) return;
  if (outgoing == incoming) {
    incoming.Opacity(1.0);
    incoming.Visibility(xaml::Visibility::Visible);
    return;
  }
  if (!ShouldAnimate()) {
    if (outgoing) {
      outgoing.Visibility(xaml::Visibility::Collapsed);
      outgoing.Opacity(1.0);
    }
    incoming.Opacity(1.0);
    incoming.Visibility(xaml::Visibility::Visible);
    return;
  }

  // Entrance half: literally RiseIn — the drawer's one-shot first appearance
  // (null outgoing) and every navigation share one primitive. Visible first:
  // RiseIn's skip-if-Collapsed guard is for OPTIONAL elements; a page being
  // navigated to is not optional.
  incoming.Visibility(xaml::Visibility::Visible);
  RiseIn(incoming, Rise::Up, kDist8, 0);
  if (!outgoing) return;

  // Exit half: down-and-out over kFastMs on the Exit easing.
  EnableTranslation(outgoing);
  auto outgoingVisual =
      xaml::Hosting::ElementCompositionPreview::GetElementVisual(outgoing);
  auto drop = outgoingVisual.Compositor().CreateScalarKeyFrameAnimation();
  drop.InsertKeyFrame(1.0f, static_cast<float>(kDist8),
                      MakeCompositionEasing(outgoingVisual.Compositor(), kExitP1, kExitP2));
  drop.Duration(Ms(kFastMs));
  outgoingVisual.StartAnimation(L"Translation.Y", drop);

  anim::Storyboard sb;
  auto fadeOut = MakeSplineDouble(1.0, 0.0, kFastMs, 0, kExitP1, kExitP2);
  anim::Storyboard::SetTarget(fadeOut, outgoing);
  anim::Storyboard::SetTargetProperty(fadeOut, L"Opacity");
  sb.Children().Append(fadeOut);
  sb.Completed([outgoing, outgoingVisual](auto const&, auto const&) {
    // Same double-swap guard the crossfade carried: only collapse it if it
    // is still the one fading out. The Translation reset is NEW — a page
    // must never re-enter 8dip low.
    if (outgoing.Opacity() <= 0.01) {
      outgoing.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
      outgoing.Opacity(1.0);  // restored for its NEXT entrance
      outgoingVisual.StopAnimation(L"Translation.Y");
      outgoing.Translation(float3{0.0f, 0.0f, 0.0f});
    }
  });
  sb.Begin();
}
```

- [ ] `MainWindow.xaml.cpp`: swap the three call sites —
  line 1159 `urnw::motion::CrossfadePageSwap(outgoingView, incomingView);` →
  `urnw::motion::DirectionalSwap(outgoingView, incomingView);`, and the two
  fallback drawer entrances (1088 and 1647)
  `urnw::motion::CrossfadePageSwap(nullptr, ConnectView());` →
  `urnw::motion::DirectionalSwap(nullptr, ConnectView());`.
- [ ] Delete `CrossfadePageSwap` + `RunCrossfade` from UrMotion.cpp and the
  declaration from UrMotion.h; update the two comments that name it —
  UrMotion.cpp's file-top family comment ("CrossfadePageSwap below joins that
  family" → DirectionalSwap's entrance half does, through RiseIn) and
  ConnectPage.h:73 ("it is now urnw::motion::DirectionalSwap, called from
  MainWindow ..."). Grep-verify `CrossfadePageSwap` has zero hits.
- [ ] Build: `powershell -File app\tools\build-local.ps1` — expect 0 errors.
- [ ] CONTROLLER GATE: frame-burst capture + visual inspection — nav swaps in
  both directions, rapid double-swap (the Completed guard), the sign-in
  drawer entrance, reduce-motion swap. Wave 2 beta checkpoint. (Implementer
  does NOT check this box.)

## Task 6 (Wave 3): the connect success bloom

Read `ConnectCanvas.h` FIRST (the implementer, not just this plan): the
canvas owns its own careful, shipped motion — bounded idle pulse, 500ms state
cross-fades (`Fade`), blob slides — all inside the host Grid. The pulse must
not fight any of it, so it animates the HOST's Composition visual (which no
canvas storyboard touches — the canvas animates its children's transforms and
opacities), and it is handed off at the `SetState` call site so both start in
the same instant. No canvas constant changes (spec §3.7 risk 2).

**Files:**
- `app/src/App/ConnectPage.cpp` (the edge detection + `PlaySuccessBloom`)
- `app/src/App/ConnectPage.h` (declaration + `presentationActive_` member)

**Interfaces:** private `void PlaySuccessBloom();` on ConnectPage; a
`bool presentationActive_ = false;` member (ConnectPage currently forwards
the flag without retaining it).

**Steps:**

- [ ] Check line endings byte-wise; preserve per-file.
- [ ] `ConnectPage.h`: in the private section add
  `void PlaySuccessBloom();  // Surface 3: the Connecting->Connected beat`
  and `bool presentationActive_ = false;`.
- [ ] `ConnectPage.cpp`, `SetPresentationActive` (line 112): first line
  `presentationActive_ = active;`.
- [ ] `ConnectPage.cpp`, `ApplyConnectStatus` (line ~838) — detect the edge
  at the existing hand-off, BEFORE `SetState` consumes it:

```cpp
  // --preview-ui drives the walk itself; letting the real status overwrite it
  // would pin the preview to Disconnected forever (there is no session).
  if (canvas_ && !PreviewHeroActive()) {
    // The success bloom (Surface 3): exactly the Connecting->Connected edge,
    // exactly once per connect — read off the canvas's own state so it
    // cannot disagree with what is on screen. Handed off HERE so the pulse
    // and the canvas's internal 500ms cross-fade start in the same instant
    // instead of fighting (the pulse rides the HOST visual, which no canvas
    // storyboard touches).
    const bool successEdge =
        canvas_->state() == urnw::ConnectCanvas::State::Connecting &&
        heroState == urnw::ConnectCanvas::State::Connected;
    canvas_->SetState(heroState);
    if (successEdge) PlaySuccessBloom();
  }
```

- [ ] `ConnectPage.cpp`: implement, next to `ApplyConnectStatus`:

```cpp
// Surface 3: the one celebratory beat in the app — a single host-level pulse
// 1->1.02->1 on the connect spring (0.75/40 — RESERVED in UrMotion.h since
// Phase C, finally spent on the moment it was named for) plus a rise-refresh
// of the status row above the hero. Disconnect deliberately gets nothing:
// exits run one step faster, and the quietest exit is none.
void ConnectPage::PlaySuccessBloom() {
  namespace mo = urnw::motion;
  namespace xaml = winrt::Microsoft::UI::Xaml;
  using winrt::Windows::Foundation::Numerics::float3;
  if (!mo::ShouldAnimate() || !presentationActive_) return;

  auto host = w_.ConnectCanvasHost();
  if (!host || host.Visibility() != xaml::Visibility::Visible) return;
  auto visual = xaml::Hosting::ElementCompositionPreview::GetElementVisual(host);
  // Same origin discipline as the hero bloom: AnchorPoint {0,0} ALWAYS,
  // CenterPoint bound to Size and left running (idempotent to restart).
  visual.AnchorPoint({0.0f, 0.0f});
  auto centerBind = visual.Compositor().CreateExpressionAnimation(
      L"Vector3(this.Target.Size.X * 0.5f, this.Target.Size.Y * 0.5f, 0.0f)");
  visual.StartAnimation(L"CenterPoint", centerBind);
  // Pose at 1.02 and let the spring do the WHOLE return: the 2% step is
  // sub-frame, so the eye reads one soft pulse — and the settled value is
  // exactly the union restore's (Scale 1), so a hide mid-pulse is already
  // covered by WindowReveal::CancelToFinal's ConnectCanvasHost entry.
  visual.Scale({1.02f, 1.02f, 1.0f});
  auto spring = visual.Compositor().CreateSpringVector3Animation();
  spring.DampingRatio(mo::kConnectSpringDamping);
  spring.Period(mo::Ms(mo::kConnectSpringPeriodMs));
  spring.FinalValue(float3{1.0f, 1.0f, 1.0f});
  visual.StartAnimation(L"Scale", spring);

  // The status row settles freshly under the new state: same elements,
  // distance and skip-if-Collapsed guard as the reveal's 240ms beat, zero
  // stagger (the riders move WITH the row or it shears), zero delay — the
  // user is already looking here.
  mo::RippleGroup({{w_.StatusDot(), mo::Rise::Up, mo::kDist8},
                   {w_.StatusText(), mo::Rise::Up, mo::kDist8},
                   {w_.ProtectionText(), mo::Rise::Up, mo::kDist8},
                   {w_.TrafficHeldText(), mo::Rise::Up, mo::kDist8},
                   {w_.StatusReasonText(), mo::Rise::Up, mo::kDist8}},
                  /*baseDelayMs=*/0, /*staggerMs=*/0);
}
```

- [ ] Verify against the canvas contract (reading, not editing,
  ConnectCanvas.cpp): `SetState` early-outs on same-state (line 651), so the
  edge fires once; the connected blobs animate transforms inside the mask and
  the host pulse composes over them without touching them; hover
  (`SetHovered`, host-CHILD `globeScale_` at 1.03) composes transiently with
  the 1.02 pulse to ~1.05 for a few frames — acceptable, verify visually at
  the gate.
- [ ] Build: `powershell -File app\tools\build-local.ps1` — expect 0 errors.
- [ ] CONTROLLER GATE: frame-burst capture + visual inspection — a real
  connect (Connecting→Connected pulse + row refresh), a disconnect (NO
  ceremony), a failed connect (no pulse), reduce-motion connect. Wave 3 beta
  checkpoint. (Implementer does NOT check this box.)

## Task 7 (Wave 4): micro unification + carousel crossfade

**Files:**
- `app/src/App/ConnectCanvas.cpp` (`SetHovered`, lines ~887–891 only — no
  other canvas line)
- `app/src/App/LoginCarousel.cpp` (`kCrossfadeMs` + `CrossfadeTo`)

**Steps:**

- [ ] Check line endings byte-wise; preserve per-file.
- [ ] `ConnectCanvas.cpp`, `SetHovered`: the one hand-rolled micro outside
  the token system — 180ms platform EaseOut becomes kFastMs on the kStandard
  spline (timing −30ms, easing joins the family). The canvas's shipped
  iOS-parity constants (kStateFadeMs, kBlobMs, the pulse) are NOT touched.
  Replace the storyboard tail:

```cpp
  anim::Storyboard sb;
  // Micro tier (motion-overhaul Surface 4): kFastMs on the Standard spline,
  // replacing the 180ms platform EaseOut — the one hand-rolled micro that
  // predated the token system. ScaleTransform properties are dependent, so
  // the flag stays, exactly as MakeDouble set it.
  auto scaleX = urnw::motion::MakeSplineDouble(globeScale_.ScaleX(), target,
                                               urnw::motion::kFastMs, 0,
                                               urnw::motion::kStandardP1,
                                               urnw::motion::kStandardP2);
  scaleX.EnableDependentAnimation(true);
  anim::Storyboard::SetTarget(scaleX, globeScale_);
  anim::Storyboard::SetTargetProperty(scaleX, L"ScaleX");
  sb.Children().Append(scaleX);
  auto scaleY = urnw::motion::MakeSplineDouble(globeScale_.ScaleY(), target,
                                               urnw::motion::kFastMs, 0,
                                               urnw::motion::kStandardP1,
                                               urnw::motion::kStandardP2);
  scaleY.EnableDependentAnimation(true);
  anim::Storyboard::SetTarget(scaleY, globeScale_);
  anim::Storyboard::SetTargetProperty(scaleY, L"ScaleY");
  sb.Children().Append(scaleY);
  sb.Begin();
```

  (`#include "UrMotion.h"` if the file does not already have it.)
- [ ] `LoginCarousel.cpp`: the image swap joins the token system — 700ms
  linear becomes kSlowMs (400) on the kStandard spline; the 5s cadence
  (`kSlideIntervalMs`) and the iOS text timings
  (`kTextOutMs`/`kTextInMs`/`kBottomDelayMs`) are deliberately untouched.
  Delete `constexpr int kCrossfadeMs = 700;` (and amend the "iOS timings"
  comment above the block: the crossfade now rides the app token rather than
  the iOS diff table). In `CrossfadeTo` (line ~316) replace the two `Anim`
  lines:

```cpp
  Storyboard board;
  // Surface 4: kSlowMs (large-surface tier) on the Standard spline. Both
  // fades on the SAME curve, so total alpha stays ~constant through the
  // swap. EnableDependentAnimation mirrors Anim()'s caution — Opacity here
  // rides shapes inside a code-built tree.
  auto fadeOut = urnw::motion::MakeSplineDouble(1, 0, urnw::motion::kSlowMs, 0,
                                                urnw::motion::kStandardP1,
                                                urnw::motion::kStandardP2);
  fadeOut.EnableDependentAnimation(true);
  Storyboard::SetTarget(fadeOut, currentImage_);
  Storyboard::SetTargetProperty(fadeOut, L"Opacity");
  board.Children().Append(fadeOut);
  auto fadeIn = urnw::motion::MakeSplineDouble(0, 1, urnw::motion::kSlowMs, 0,
                                               urnw::motion::kStandardP1,
                                               urnw::motion::kStandardP2);
  fadeIn.EnableDependentAnimation(true);
  Storyboard::SetTarget(fadeIn, nextImage_);
  Storyboard::SetTargetProperty(fadeIn, L"Opacity");
  board.Children().Append(fadeIn);
```

  The swap-sources-on-`Completed` handler, the `crossfade_.Stop()`
  stop-before-restart rule and `crossfade_ = board;` all stay byte-identical.
  (`#include "UrMotion.h"` if absent.)
- [ ] Closing audit: `rg -n "Duration\{|DurationMs|, 1[0-9][0-9],|, [0-9][0-9],"
  app/src/App --glob "*.cpp"` (and judgment) for any remaining hand-built
  sub-200ms storyboard outside the token system; unify stragglers onto
  `kMicroMs`/`kFastMs` + `kStandard` or list them in the completion report as
  deliberately platform-owned.
- [ ] Build: `powershell -File app\tools\build-local.ps1` — expect 0 errors.
- [ ] CONTROLLER GATE: frame-burst capture + visual inspection — hero hover
  in/out, a full carousel cycle (image swap cadence unchanged at 5s, swap
  itself 400ms), reduce-motion carousel. Wave 4 beta checkpoint — the full
  overhaul ships. (Implementer does NOT check this box.)

---

## Plan self-review

**Spec coverage.** Spec §2.1 tokens → Task 1 (with static_asserts; kDist8/12
reused, not aliased). §2.2 primitives → Tasks 1 and 5. §3 Hero Bloom: both
timelines → Task 2's tables (every table entry maps to a spec row; delays
expressed in token arithmetic, 120/240/280/320/360); deletions → Tasks 3–4
(dirX, origin, root spring, kOffsetDip, horizontal motion); new handles →
Task 2 (EmailGroup, SecondaryAuthRow) and Bind-time handles in the tables;
§3.6 wide login → Task 2a; §3.7 risks → each is either coded against (1:
Bind asserts; 3: EnableTranslation in Bind + RiseIn; 4: the two guards; 9:
ApplyAuthState/ShowLoginRoot/ShowHomeRoot cancels; 10: unchanged contracts in
Task 4) or named in a controller gate (2, 5, 6, 7, 8). §4 → Task 5. §5 →
Task 6. §6/§7 → Task 7. §8 spine → Global Constraints + Task 3's
CancelToFinal (all 52 entries: 8 Composition entries in settleHero/root/plate,
19 Translations + 24 XAML opacities via the union tables + both heroes,
armed_=false). §9 verification → every task's gate. §10 phasing → task
ordering and the checkpoint notes on Tasks 4–7.

**Sequencing.** Every task builds green on its own: Task 2 ships the new
Bind against transitional internals; Task 3 keeps a forwarding Arm shim;
Task 4 deletes both ends of the shim in one step. Task 2a is independent of
2/3 (layout, not choreography) and must land before Wave 1's gate because it
changes what the signed-out gate captures.

**No placeholders.** No TBDs; every step contains the literal code or the
exact edit (old line → new line). The two deliberate reading steps (Task 6's
canvas-contract check, Task 7's audit) are verification steps with concrete
commands or line references, not deferred design.

**Type consistency.** Durations are `int64_t` ms everywhere (`RevealRing`
delays/durations, primitive params — matching `Ms()`/`MakeSplineDouble`).
Distances: tokens are `double` dip (`kDist8`); `RevealRing.riseDip` and
Composition keyframe values are `float` — casts are written at the
Composition boundary (`static_cast<float>` in RiseIn/DirectionalSwap), and
the table literals are float (`+8.0f`). `kHeroScaleFrom` is `float` because
it only ever feeds `Visual.Scale`. `RippleEntry.distDip` is `double` to match
`RiseIn`. Storyboard retention uses the concrete `Storyboard` type so
`Stop()` is available.

**Known deviations, deliberate.** (a) The judge JSON files the `kOffsetDip`
deletion under UrMotion.h; the constant actually lives in WindowReveal.cpp's
anonymous namespace and dies in Task 3 — noted there. (b) The design brief
lists kDist8/kDist12 among "new tokens"; they exist already and the JSON
says reuse-no-aliases — this plan reuses. (c) `StartHeroBloom` returns the
fade Storyboard (not in the four-primitive signatures as briefed) — required
by the JSON's own rule that Storyboards be stopped before every XAML restore
write; a fire-and-forget hero fade would make the 52-entry restore
unenforceable. (d) Task 2a (wide login) is an addition to the briefed task
list from the layout investigation; without it the signed-out flagship
choreography blooms into ~58% empty plate.
