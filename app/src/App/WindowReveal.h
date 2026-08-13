// Phase E: the main window animates open rather than appearing. Not a splash
// - the window itself expands/reveals its content. See the design plan
// (docs/superpowers/plans/2026-08-11-design-update-super-plan.md, Phase E)
// for the choreography this implements.
//
// UI THREAD ONLY, and it must add ZERO latency to first interaction: Arm()
// writes the start pose synchronously so the first composed frame is already
// correct, and Start() only ever plays a Composition animation, never blocks.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <vector>

#include <windows.h>

#include <winrt/Microsoft.UI.Xaml.h>

namespace urnw {

// One ripple-ring element and the delay (ms) its opacity fade starts at,
// relative to Start(). See MainWindow's constructor for how the rings are
// built from the window's own named elements.
struct RevealRing {
  winrt::Microsoft::UI::Xaml::UIElement element{nullptr};
  int64_t delayMs = 0;
};

// E1: the HWND never animates; content springs inside it. `plate` is the
// opaque, never-moving background (WindowPlate in MainWindow.xaml);
// `revealRoot` is the surface that actually springs (RevealRoot). See
// WindowReveal.cpp's file comment for why the HWND itself is off limits.
class WindowReveal {
 public:
  // Bind once, right after the window's content exists (MainWindow's
  // constructor). `rings` is the E3 opacity ripple, in playback order.
  void Bind(winrt::Microsoft::UI::Xaml::FrameworkElement const& plate,
            winrt::Microsoft::UI::Xaml::FrameworkElement const& revealRoot,
            std::vector<RevealRing> rings);

  // Write the START pose synchronously, BEFORE Window.Activate() (E4) - the
  // reveal must add zero latency, so the first composed frame has to already
  // be correct rather than cloaked-then-corrected.
  //
  // `enabled` folds together every reason this show should not reveal:
  // reduce-motion and the presentation gate are checked here; "this is an
  // un-minimize, which the OS's own restore animation already owns" is the
  // CALLER's decision (AppController::ShowWindowImpl latches IsIconic before
  // it calls SW_RESTORE, because IsIconic after that call always answers
  // false). `originScreen` is the best available anchor in screen
  // coordinates - the tray icon's rect, falling back to the click point, or
  // nullopt for a plain centred scale (E2's three fallbacks; never fail to
  // open). `windowScreenRect` is the window's own screen rect, used only to
  // turn that anchor into a left/right direction.
  void Arm(bool enabled, std::optional<POINT> originScreen, RECT const& windowScreenRect);

  // Start the spring + opacity ripple. Call AFTER Activate() returns. A no-op
  // unless the matching Arm() armed a reveal.
  void Start();

  // Cancel whatever is in flight and snap straight to the settled pose (Scale
  // 1, Offset 0, full opacity everywhere). Two callers: Arm() itself, which
  // settles any reveal still running from a previous show before writing the
  // next start pose, and MainWindow::SetPresentationActive(false) - hiding
  // mid-reveal must not leave RevealRoot pinned at ~0.96 scale forever, which
  // is E6's "nastiest failure mode, found not hypothetical".
  void CancelToFinal();

 private:
  winrt::Microsoft::UI::Xaml::FrameworkElement plate_{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement revealRoot_{nullptr};
  std::vector<RevealRing> rings_;
  bool armed_ = false;
};

}  // namespace urnw
