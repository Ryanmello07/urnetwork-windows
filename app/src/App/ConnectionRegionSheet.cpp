// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ConnectionRegionSheet.h"

#include <cctype>

#include <winrt/Windows.Globalization.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include "Localization.h"
#include "SettingsSheets.h"  // the row kit: MakeSheet / Lookup
#include "Strings.h"         // Widen
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Windows::Foundation;
using namespace urnw::rows;

// Captures as in ControlDohSettings.cpp: control handlers capture the block
// weakly; the coroutines take the root's DispatcherQueue, a weak reference and
// a pointer to the host before they leave the UI thread, never touch `this`
// off it, and come back through the queue.

namespace urnw {
namespace {

namespace automation = winrt::Microsoft::UI::Xaml::Automation;
namespace region = connection_region;

hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

TextBlock MakeProse(Panel const& host, hstring const& text, double size) {
  TextBlock prose;
  prose.Text(text);
  prose.FontSize(size);
  prose.TextWrapping(TextWrapping::Wrap);
  prose.Foreground(colors::MutedBrush());
  prose.Margin(ThicknessHelper::FromLengths(12, 0, 12, 0));
  host.Children().Append(prose);
  return prose;
}

std::wstring JoinLines(const std::vector<std::string>& values) {
  std::wstring out;
  for (const auto& value : values) {
    out += L"\n  ";
    out += Widen(value);
  }
  return out;
}

}  // namespace

std::wstring RegionDisplayName(const std::string& code) {
  if (code.empty()) return {};
  try {
    // ISO 3166-1 alpha-2, as the platform spells it
    std::string upper = code;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const hstring id = winrt::to_hstring(upper);
    if (!winrt::Windows::Globalization::GeographicRegion::IsSupported(id)) return {};
    return std::wstring{winrt::Windows::Globalization::GeographicRegion(id).DisplayName()};
  } catch (...) {
    return {};
  }
}

hstring RegionLabel(const std::string& stored) {
  if (stored.empty()) return Loc(region::kUndisclosedKey);
  std::wstring name = RegionDisplayName(stored);
  if (name.empty()) {
    for (char c : stored) name.push_back(static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(c))));
  }
  return hstring{name};
}

std::shared_ptr<ConnectionRegionBlock> ConnectionRegionBlock::Create(
    SdkHost& sdk, std::function<void(std::string const&)> onSaved) {
  auto block = std::shared_ptr<ConnectionRegionBlock>(new ConnectionRegionBlock(sdk));
  block->onSaved_ = std::move(onSaved);
  block->Build();
  return block;
}

void ConnectionRegionBlock::Build() {
  root_ = StackPanel();
  root_.Spacing(8);

  MakeProse(root_, Loc(region::kDescriptionKey), 12);

  picker_ = ComboBox();
  picker_.HorizontalAlignment(HorizontalAlignment::Stretch);
  picker_.Margin(ThicknessHelper::FromLengths(12, 0, 12, 0));
  picker_.IsEnabled(false);
  automation::AutomationProperties::SetName(picker_, Loc(region::kTitleKey));
  picker_.SelectionChanged([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self || self->updating_) return;
    const int index = self->picker_.SelectedIndex();
    if (index < 0 || static_cast<size_t>(index) >= self->rows_.size()) return;
    self->Save(self->rows_[index]);
  });
  root_.Children().Append(picker_);

  resolvers_ = MakeProse(root_, Loc("loading"), 11);
}

winrt::fire_and_forget ConnectionRegionBlock::Load() {
  auto weak = weak_from_this();
  auto queue = root_.DispatcherQueue();
  SdkHost* const sdk = &sdk_;
  const std::wstring undisclosed{Localized(region::kUndisclosedKey)};

  co_await winrt::resume_background();
  auto rows = region::PickerRows(sdk->IngressCountryCodes(), undisclosed, RegionDisplayName);
  std::optional<std::string> stored = sdk->GetIngressCountryCode();

  queue.TryEnqueue([weak, rows = std::move(rows), stored = std::move(stored)]() mutable {
    if (auto self = weak.lock()) self->ApplyLoaded(std::move(rows), std::move(stored));
  });
}

void ConnectionRegionBlock::ApplyLoaded(std::vector<region::Row> rows,
                                        std::optional<std::string> stored) {
  rows_ = std::move(rows);
  updating_ = true;
  picker_.Items().Clear();
  for (const auto& row : rows_) picker_.Items().Append(winrt::box_value(hstring{row.label}));
  const std::string code = stored.value_or("");
  picker_.SelectedIndex(static_cast<int32_t>(region::SelectedRow(rows_, code)));
  updating_ = false;
  // no space to read from: nothing to save to either
  picker_.IsEnabled(stored.has_value());
  if (!stored) {
    resolvers_.Text(Loc("something_went_wrong"));
    return;
  }
  ShowResolvers(rows_[region::SelectedRow(rows_, code)].code);
}

winrt::fire_and_forget ConnectionRegionBlock::Save(region::Row row) {
  auto weak = weak_from_this();
  auto queue = root_.DispatcherQueue();
  picker_.IsEnabled(false);

  co_await winrt::resume_background();
  std::optional<std::string> stored;
  if (auto self = weak.lock()) stored = self->selection_.Select(row);

  queue.TryEnqueue([weak, stored] {
    auto self = weak.lock();
    if (!self) return;
    self->picker_.IsEnabled(true);
    if (!stored) {
      self->resolvers_.Text(Loc("something_went_wrong"));
      return;
    }
    // show what was stored, which an unknown code makes ""
    self->updating_ = true;
    self->picker_.SelectedIndex(static_cast<int32_t>(region::SelectedRow(self->rows_, *stored)));
    self->updating_ = false;
    self->ShowResolvers(*stored);
    if (self->onSaved_) self->onSaved_(*stored);
  });
}

winrt::fire_and_forget ConnectionRegionBlock::ShowResolvers(std::string code) {
  auto weak = weak_from_this();
  auto queue = root_.DispatcherQueue();
  SdkHost* const sdk = &sdk_;

  co_await winrt::resume_background();
  region::ResolverSummary summary;
  summary.encrypted = sdk->IngressDohUrls(code);
  summary.fallback = sdk->IngressPlainDnsServers(code);

  queue.TryEnqueue([weak, summary] {
    auto self = weak.lock();
    if (!self) return;
    if (summary.Defaults()) {
      self->resolvers_.Text(Loc(region::kDefaultResolversKey));
      return;
    }
    std::wstring text;
    if (!summary.encrypted.empty()) {
      text += std::wstring{Localized(region::kEncryptedResolversKey)} + JoinLines(summary.encrypted);
    }
    if (!summary.fallback.empty()) {
      if (!text.empty()) text += L"\n";
      text += std::wstring{Localized(region::kFallbackResolversKey)} + JoinLines(summary.fallback);
    }
    self->resolvers_.Text(hstring{text});
  });
}

std::shared_ptr<ConnectionRegionSheet> ConnectionRegionSheet::Create(
    XamlRoot const& root, SdkHost& sdk, std::function<void(std::string const&)> onSaved) {
  auto sheet = std::shared_ptr<ConnectionRegionSheet>(new ConnectionRegionSheet());
  sheet->block_ = ConnectionRegionBlock::Create(sdk, std::move(onSaved));
  sheet->dialog_ = MakeSheet(root, Loc(region::kPickerTitleKey));

  StackPanel content;
  content.MinWidth(380);
  content.Children().Append(sheet->block_->Root());
  sheet->dialog_.Content(content);

  sheet->block_->Load();
  return sheet;
}

}  // namespace urnw
