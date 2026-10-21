// The post-sign-up onboarding flow: five pages over the home view, with the
// step-bubbles-and-Skip top bar, the walking members on page 1 and the
// connector mark that flies from the route line into the header. Built in
// code over the OnboardingOverlay grid in MainWindow.xaml; shown once, right
// after a network is created (never for an existing account signing in).
//
// The language and design follow the Android flow (ui/introduction/*) page for
// page; Windows has no cellular toggle and no widget page.
//
// The welcome offer (mmm/onboarding/PLAN.md): the plan page issues the
// network's offer (POST /onboarding/offer/issue, surface intro_step) and
// prints it on the yearly card with the deadline and the trial timeline; the
// final page restates the same offer with one primary CTA and "Continue with
// the free plan", and everyone reaches it — Skip lands there once
// (OnboardingRouting.h). The offer.in_app holdout sees the four-page flow
// with the regular picker and no issue call. Every page transition, the
// offer surfaces and the decline are client events (ClientEvents.h).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "CryptoPayViews.h"
#include "OfferCard.h"
#include "OnboardingRouting.h"
#include "PlanPicker.h"
#include "ReferralCard.h"
#include "SubscriptionBalance.h"

namespace urnw {

// Crypto (crypto-pay CONTRACT §6.3): the plan page and the offer page skip the
// upgrade sheet's products page, so each carries the payment-method row of its
// own, directly above its CTA, while the server sells a coin. The two rows
// are one selection (the device's remembered method). A coin greys out
// monthly, says "Pay with {method}" and its fine print, and the CTA opens the
// coin's invoice instead of the card checkout.
class Onboarding : public std::enable_shared_from_this<Onboarding> {
 public:
  struct Actions {
    std::function<void()> finish;  // Get connected, and Skip
    // The CTA's purchase: Card's Stripe trial checkout for the plan, or the
    // coin's invoice (`method` is cryptopay::kMethodCard or a coin's asset)
    std::function<void(bool yearly, std::string const& method)> startCheckout;
    std::function<void()> redeemCode;  // the redeem balance code sheet
  };

  static std::shared_ptr<Onboarding> Create(winrt::Microsoft::UI::Xaml::Controls::Grid host,
                                            Actions actions);

  void Show();
  // The urnetwork://onboarding/offer destination: the offer page on its own
  // (no earlier pages; the page's link is the way out).
  void ShowOffer();
  void Hide();
  bool Visible() const { return visible_; }

  // Balance-store push, forwarded by the window: page 2's numbers and page 4's
  // referral code follow the store.
  void OnBalance(BalanceSnapshot const& snapshot);

 private:
  Onboarding(winrt::Microsoft::UI::Xaml::Controls::Grid host, Actions actions)
      : host_(host), actions_(std::move(actions)) {}

  int StepCount() const { return OnboardingStepCount(offerEnabled_); }

  void Build();
  void ShowStep(int step);
  void Skip();
  void ApplyTopBar();
  void ApplyBubbles();
  // the plan cards and the offer texts from the balance snapshot's tier/offer
  void ApplyPrices();
  // POST /onboarding/offer/issue from the plan page (once per showing)
  void IssueOffer();
  void StartCheckout(bool yearly);
  int64_t StepElapsedMs() const;

  winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildWelcome();
  void ApplyPlanCta(bool yearly);
  // the offer page's CTA: the trial's months for Card, the coin's otherwise
  void ApplyOfferCta();
  // ---- the payment method (crypto-pay §6.1) ----
  // both rows from the balance's coins, keeping the selection (the device's
  // remembered method until one is made here)
  void ApplyMethods();
  // the selection's consequences on both pages
  void ApplyMethod(std::string const& method);
  // a tap on either row: one selection, remembered on this device
  void ChooseMethod(std::string const& method);
  winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildBandwidth();
  winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildProviding();
  winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildReferral();
  winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildOffer();

  // page 1: the route line and its walker
  winrt::Microsoft::UI::Xaml::FrameworkElement BuildRoute();
  void StartTrip();
  void StopTrip();
  void PlaceWalker(double fraction);

  // the connector's flight between the route slot and the header slot
  void FlyConnector(bool toHeader);
  void PlaceFlyer(winrt::Windows::Foundation::Rect const& rect);
  winrt::Windows::Foundation::Rect SlotRect(winrt::Microsoft::UI::Xaml::FrameworkElement const& slot);
  void ParkFlyer();

  // page 2 / page 4 live content
  void ApplyBandwidth();
  void ApplyReferral();

  winrt::Microsoft::UI::Xaml::Controls::Grid host_;
  Actions actions_;
  bool built_ = false;
  bool visible_ = false;
  bool animations_ = true;
  int step_ = 1;
  bool offerEnabled_ = true;      // not the offer.in_app holdout
  bool offerIssued_ = false;      // the issue call went out this showing
  bool introOfferShown_ = false;  // offer.screen.shown(intro_step) emitted
  bool standalone_ = false;       // ShowOffer: the offer page alone
  std::chrono::steady_clock::time_point stepShownAt_{};

  // top bar
  winrt::Microsoft::UI::Xaml::Controls::Button backButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button skipButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border headerSlot_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel bubbles_{nullptr};

  // pages
  winrt::Microsoft::UI::Xaml::Controls::ScrollViewer scroll_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ContentControl pageHost_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel welcome_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel bandwidth_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel providing_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel referral_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel offer_{nullptr};

  // page 1: route line
  winrt::Microsoft::UI::Xaml::Controls::Canvas walkerCanvas_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid walker_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Image walkerImage_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border heroSlot_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Image routeConnector_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard trip_{nullptr};
  std::vector<int> deck_;
  int person_ = 0;
  double travel_ = 0;

  // page 1: plans (the picker shared with the upgrade sheet)
  PlanPicker plans_;
  winrt::Microsoft::UI::Xaml::Controls::Button checkoutButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard haloStoryboard_{nullptr};

  // the flying connector
  winrt::Microsoft::UI::Xaml::Controls::Canvas flightCanvas_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Image flyer_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard flight_{nullptr};
  bool flying_ = false;

  // page 2
  std::unique_ptr<class UsageBar> usageBar_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock dailyValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock dailyLine_{nullptr};
  BalanceSnapshot balance_;

  // page 4
  ReferralCard referralCard_;  // the shared referral progress box + gold panel
  winrt::Microsoft::UI::Xaml::Controls::Button referralDone_{nullptr};

  // the offer on the plan page (under the picker) and the final page
  OfferLines welcomeOffer_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock offerEyebrow_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock offerHeadline_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock offerPrice_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock offerThen_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock offerTrial_{nullptr};
  OfferLines offerLines_;
  winrt::Microsoft::UI::Xaml::Controls::Button offerCta_{nullptr};

  // the payment method on page 1 and on the offer page: one selection
  PaymentMethodRow welcomeMethods_;
  PaymentMethodRow offerMethods_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock welcomeFinePrint_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock offerFinePrint_{nullptr};
  std::string method_ = cryptopay::kMethodCard;  // the selection
  std::string wantedMethod_;  // picked here; "" until then (the remembered one)
};

}  // namespace urnw
