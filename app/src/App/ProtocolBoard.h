// The Protocol leaderboard (owner decision 2026-10-29, LEADERTOP;
// apple/LEADERTOP.md is the design and Apple the reference): the first and
// default board of the Earnings leaderboard, ahead of Points and Data. It
// tells a provider whether claiming a Top 200 miner UID on SN25 is worth it:
// where the validator would rank the network if it claimed now.
//
//   ledger pane (B)   the top 400 (GET /sn/top, paged 50 at a time on a
//                     cursor pinned to one snapshot): the rows by position,
//                     the Top 200 line between 200 and 201 with the tint above
//                     it, UID badges, dimmed rows the validator cannot score,
//                     the caller's row, the draggable position indicator over
//                     1-400, and the "you" bar under the list
//   network pane (C)  the banners (claiming closed, a stale snapshot), the
//                     own-status card (GET /sn/top/me) with its progress chart
//                     and claim prompt, and the explainer. Below the
//                     three-pane width the pane folds and this block moves to
//                     the top of the list's scroll (MainWindow::ApplyWalletFold),
//                     the design's compact order: banner, card, explainer, list.
//
// The card's answer is the Earnings Top 200 tile's too (WalletPage::
// ApplyTop200), so the tile and the board agree. A claimed row opens its
// detail sheet. The "Show my network name" switch on the card is the Points
// board's (points_leaderboard_public, Q12): WalletPage owns its state and its
// write, and both switches show it.
//
// Every request goes out on the in-process Api, whose answer comes back on an
// sdk thread: ProtocolBoardPresentation.h's readers copy it into plain values
// there, and the value is marshalled to the UI thread through the
// DispatcherQueue, resolved through the window's weak reference, and dropped
// once the page is gone (alive_) or the board's open it went out under has
// been reset (the model's fence: a sign-out). Everything else runs on the UI
// thread. ProtocolBoardPresentation.h decides everything before the XAML.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "ProtocolBoardPresentation.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

// A label or selection chip: its words in the tone's colour inside a rounded
// outline. It reads as its words (colour is never the only signal). The card,
// the list's own row, the detail sheet and the Earnings tile all use it.
winrt::Microsoft::UI::Xaml::FrameworkElement MakeProtocolChip(std::string const& text,
                                                              protocolboard::Tone tone);

class ProtocolBoard {
 public:
  // `alive` is the page's: false once it is gone
  ProtocolBoard(winrt::URnetwork::implementation::MainWindow& window, std::shared_ptr<bool> alive);
  ~ProtocolBoard();

  ProtocolBoard(ProtocolBoard const&) = delete;
  ProtocolBoard& operator=(ProtocolBoard const&) = delete;

  // the scroller, the retry, the "you" bar and the position indicator
  void Initialize();
  // the board's words, and the card and the list again in them
  void ApplyStrings();

  // The board is on screen (its tab, on the leaderboard tab of the ledger
  // pane) or not. Its first show with a session loads it; a show long after
  // the last load reloads it from a fresh snapshot (protocolboard::Model::
  // ShouldRefresh).
  void SetShowing(bool showing);
  // GET /sn/top/me, for the card and the tile: LoadWallet asks on sign-in and
  // on navigation to Earnings, whatever board shows.
  void LoadMe();
  // A tab activated (the active one included): the list to the top
  // (mmm/DESIGNSTYLE.md "Long ranked lists").
  void ScrollToTop();
  // The network's name switch (points_leaderboard_public) as WalletPage holds
  // it, and whether a write of it is out.
  void ApplyNamePublic(bool isPublic, bool writing);
  // The switch changed on the server: the card and the caller's row read again.
  void ReloadAfterNameChange();
  // Drop everything about the account that just signed out; the next sign-in
  // loads its own.
  void ResetForSignOut();
  // --preview-ui: the board settles on its empty state with no request (or,
  // with URNETWORK_PREVIEW_SAMPLE=1, on obviously synthetic rows).
  void ShowPreview(bool sample);

  protocolboard::State const& state() const { return model_.state(); }
  // the board's words: the store's strings, the locale's numbers and the
  // user's dates
  protocolboard::Text BoardText() const;

 private:
  // the one gate for every request: a session, and not --preview-ui
  bool HasSession() const;
  bool CanLoadList() const;

  // ---- requests
  void Reload();  // the first page, from a fresh snapshot
  void LoadNextPage(int64_t limit);
  void ApplyPage(uint64_t generation, protocolboard::Model::PageRequest request,
                 protocolboard::TopOutcome outcome);
  void ApplyMe(uint64_t generation, uint64_t load, protocolboard::MeOutcome outcome);
  // a seek's pages, then its scroll
  void ContinueSeek();

  // ---- rendering
  void Render();
  void RenderBanners(protocolboard::State const& state);
  void RenderCard(protocolboard::State const& state);
  void RenderList();
  void RenderFooter(protocolboard::ListView const& view);
  void RenderYouBar();
  void RenderTile();
  winrt::Microsoft::UI::Xaml::UIElement MakeRow(protocolboard::ListRow const& row);
  winrt::Microsoft::UI::Xaml::UIElement MakeLine(protocolboard::ListItem const& item);
  winrt::Microsoft::UI::Xaml::FrameworkElement MakeChart(protocolboard::Chart const& chart);
  winrt::Microsoft::UI::Xaml::FrameworkElement MakeClaimPrompt(protocolboard::ClaimPrompt const& prompt);
  void AppendPlaceholderRows(winrt::Microsoft::UI::Xaml::Controls::Panel const& host, int count);

  // ---- the claimed row's sheet, and the claim prompt's door to the tile
  winrt::fire_and_forget ShowDetail(int64_t position);
  void ClaimOnEarnings();
  void OnNamePublicToggled();

  // ---- the position indicator (LeaderboardIndicator.h's math)
  void BuildIndicator();
  void RefreshPosition();
  void UpdateIndicator();
  void OnThumbPressed(double y, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
  void OnThumbMoved(double y);
  void OnThumbReleased();
  void ShowDragLabel(int64_t position);
  void HideDragLabel(bool fade);
  // `position` in the loaded rows scrolls there; past them, the list loads
  // through it and scrolls once it lands
  void Seek(int64_t position);
  void ScrollToPosition(int64_t position);
  double RowsTop() const;
  void OnScroll();

  winrt::URnetwork::implementation::MainWindow& w_;
  std::shared_ptr<bool> alive_;
  protocolboard::Model model_;
  bool initialized_ = false;
  bool showing_ = false;
  bool preview_ = false;
  // the switch's state as WalletPage holds it, and a write of it out
  bool namePublic_ = false;
  bool nameWriting_ = false;
  bool applyingNameToggle_ = false;  // ECHO GUARD on the card's switch

  // the list as drawn: appended to while a page extends it, redrawn otherwise
  std::vector<protocolboard::ListItem> renderedItems_;
  protocolboard::ListBody renderedBody_ = protocolboard::ListBody::Placeholders;
  int64_t lineAfter_ = -1;  // the row index the line follows; -1 for none
  int64_t firstVisible_ = 1;
  int64_t total_ = 0;       // the indicator's range

  // the network pane's block (ProtocolNetworkHost): the banners, the card and
  // the explainer, built once and refilled
  winrt::Microsoft::UI::Xaml::Controls::StackPanel bannersHost_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel cardHost_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock explainer_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock groupTitle_{nullptr};
  // the card's live parts, rebuilt by RenderCard
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch nameToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock chartLabel_{nullptr};
  // the last failed refresh the notice was raised for, so it is raised once
  bool refreshNoticeShown_ = false;
  // the "you" bar as drawn, and which way its arrow points
  std::optional<protocolboard::YouBar> renderedYou_;
  bool renderedYouUp_ = false;

  // the position indicator: a slider over 1..N at the scroller's right edge,
  // the rank and tier beside the thumb while dragging
  winrt::Microsoft::UI::Xaml::Controls::Canvas indicator_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Rectangle track_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Rectangle thumb_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Slider slider_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border dragLabel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock dragRank_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock dragTier_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard labelFade_{nullptr};
  bool dragging_ = false;
  double dragGrab_ = 0;
  double dragTop_ = 0;
  int64_t dragPosition_ = 0;
  bool applyingSlider_ = false;  // ECHO GUARD on the slider's value
};

}  // namespace urnw
