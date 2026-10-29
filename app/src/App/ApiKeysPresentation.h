// Everything the Account > API keys page (ApiKeysPage.h) decides before it
// touches a XAML object (owner decision 2026-10-29: an API keys screen on
// ur.io, apple, android, windows and linux): the sdk's answers read as plain
// values, the list's order, which of progress, the rows, empty and failed the
// body shows, every row's lines and spoken lines, a delete's state on its row,
// the create sheet's name check and steps, the delete confirmation, and the
// page's model, which drops an answer the page no longer wants.
//
// The page calls the in-process Api's listApiKeys, createApiKey and
// deleteApiKey (the sdk's cgo exports, through urnetwork_sdk.hpp). Each one
// answers once, on an sdk thread: the readers here copy the answer into plain
// values there, and the page applies them on the UI thread.
//
// The created key (urn_...) is a full account credential that the server
// keeps only as a hash, so the create sheet shows it once. It lives in exactly
// one place, the CreateOutcome that carries it from the sdk's thread and then
// the CreateFlow that shows it, and both wipe it: the flow when the sheet
// closes by any route, and an answer that finds the sheet closed at once. It
// is never logged, kept in settings or a file, or sent anywhere but the
// clipboard the user asks for.
//
// Pure, for the reason ExtenderPresentation.h gives: a WinUI 3 app cannot be
// built off Windows, so tools/api-keys-tests.cpp verifies every decision on
// any host with a C++20 compiler. The readers are templates over the generated
// header's ListApiKeysResult, CreateApiKeyResult and DeleteApiKeyResult, so
// the tests run them on fakes and compile them against urnetwork_sdk.hpp;
// ApiKeysPage.cpp passes the sdk's results straight in.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CryptoPayPresentation.h"  // ParseRfc3339: the sdk's create_time

namespace urnw::apikeys {

// The API reference the intro links to.
inline constexpr const wchar_t* kDocsUrl = L"https://ur.io/docs/api";

// The longest name the server stores (server model.MaxApiKeyNameLength), in
// characters. The name box counts UTF-16 units, which are never fewer.
inline constexpr std::size_t kMaxNameLength = 128;

// ---- the answers, as plain values -------------------------------------------

// PublicAccountApiKey: what the server lists of a key. The key itself is never
// listed; the server keeps only its hash.
struct Key {
  std::string id;
  std::string name;
  int64_t createTimeMillis = 0;  // 0 when the server sent none, or one that does not parse

  bool operator==(const Key&) const = default;
};

// A finished listApiKeys.
struct ListOutcome {
  bool ok = false;
  std::vector<Key> keys;  // newest first (SortKeys)
  std::string error;      // what failed, for the log; the page shows its own words
};

// Overwrite, then empty: a key a string held is not left readable in the
// buffer the string gives back to the allocator (approval::Wipe's idiom). The
// sdk's own transient copies (its JSON and the C string it answers with) are
// out of reach here.
inline void Wipe(std::string& text) noexcept {
  volatile char* bytes = text.empty() ? nullptr : &text[0];
  for (std::size_t i = 0; bytes != nullptr && i < text.size(); ++i) bytes[i] = 0;
  text.clear();
}

// A finished createApiKey. It carries the key from the sdk's thread to the
// sheet, so it cannot be copied, a move leaves nothing behind, and whatever is
// destroyed with it wipes the key: a value that is never delivered, or that a
// closed sheet refuses, takes the key with it.
struct CreateOutcome {
  bool ok = false;
  std::string id;
  std::string name;
  // the urn_ key, shown once
  std::string secret;
  std::string error;  // for the log, and never the key

  CreateOutcome() = default;
  CreateOutcome(const CreateOutcome&) = delete;
  CreateOutcome& operator=(const CreateOutcome&) = delete;
  CreateOutcome(CreateOutcome&& other) noexcept
      : ok(other.ok),
        id(std::move(other.id)),
        name(std::move(other.name)),
        secret(std::move(other.secret)),
        error(std::move(other.error)) {
    Wipe(other.secret);
  }
  CreateOutcome& operator=(CreateOutcome&& other) noexcept {
    if (this == &other) return *this;
    Wipe(secret);
    ok = other.ok;
    id = std::move(other.id);
    name = std::move(other.name);
    secret = std::move(other.secret);
    error = std::move(other.error);
    Wipe(other.secret);
    return *this;
  }
  ~CreateOutcome() { Wipe(secret); }
};

// A finished deleteApiKey.
struct DeleteOutcome {
  bool ok = false;
  std::string error;  // for the log
};

// The sdk's create_time (Go's time.Time as RFC 3339, in UTC) in Unix
// milliseconds; 0 for none, one that does not parse, and one at or before 1970.
inline int64_t CreateTimeMillis(const std::optional<std::string>& createTime) {
  if (!createTime) return 0;
  const std::optional<int64_t> seconds = cryptopay::ParseRfc3339(*createTime);
  if (!seconds || *seconds <= 0) return 0;
  return *seconds * 1000;
}

// The list's order: newest first, the keys with no time last, and the same
// order for keys made in the same second (by name, then id). The server lists
// them in no order.
inline void SortKeys(std::vector<Key>& keys) {
  std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
    const bool aKnown = 0 < a.createTimeMillis;
    const bool bKnown = 0 < b.createTimeMillis;
    if (aKnown != bKnown) return aKnown;
    if (a.createTimeMillis != b.createTimeMillis) return b.createTimeMillis < a.createTimeMillis;
    if (a.name != b.name) return a.name < b.name;
    return a.id < b.id;
  });
}

// ---- reading the sdk's results --------------------------------------------------
//
// By the generated header's field names. Run on the sdk's thread, where the
// callback hands the result over.

// An error's words for the log: the call's error string, or the server's
// message, or a placeholder for an answer with neither.
template <typename SdkResult>
std::optional<std::string> FailureOf(const std::optional<SdkResult>& result,
                                     const std::optional<std::string>& err) {
  if (err) return err->empty() ? std::string("error") : *err;
  if (!result) return std::string("no result");
  if (result->error) return result->error->message.empty() ? std::string("error") : result->error->message;
  return std::nullopt;
}

// A key the server sent with no id cannot be deleted by anything, and the
// server never sends one: it is left out.
template <typename SdkResult>
ListOutcome ListOutcomeFrom(const std::optional<SdkResult>& result,
                            const std::optional<std::string>& err) {
  ListOutcome out;
  if (const auto failure = FailureOf(result, err)) {
    out.error = *failure;
    return out;
  }
  out.ok = true;
  if (result->api_keys) {
    for (const auto& key : *result->api_keys) {
      if (!key.id || key.id->empty()) continue;
      out.keys.push_back(Key{.id = *key.id,
                             .name = key.name,
                             .createTimeMillis = CreateTimeMillis(key.create_time)});
    }
  }
  SortKeys(out.keys);
  return out;
}

// Moves the key out of the sdk's result into the outcome and wipes what is
// left of it there; a failed answer's key, should one ever carry it, is wiped
// and never shown.
template <typename SdkResult>
CreateOutcome CreateOutcomeFrom(std::optional<SdkResult>& result,
                                const std::optional<std::string>& err) {
  CreateOutcome out;
  const auto failure = FailureOf(result, err);
  if (failure || !result->api_key || result->api_key->empty()) {
    out.error = failure ? *failure : std::string("no key");
    if (result && result->api_key) Wipe(*result->api_key);
    return out;
  }
  out.ok = true;
  out.id = result->id ? *result->id : std::string();
  out.name = result->name ? *result->name : std::string();
  out.secret = std::move(*result->api_key);
  Wipe(*result->api_key);
  return out;
}

template <typename SdkResult>
DeleteOutcome DeleteOutcomeFrom(const std::optional<SdkResult>& result,
                                const std::optional<std::string>& err) {
  DeleteOutcome out;
  if (const auto failure = FailureOf(result, err)) {
    out.error = *failure;
    return out;
  }
  out.ok = true;
  return out;
}

// ---- the name a key is created with ---------------------------------------------

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline std::string_view TrimSpace(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
  return text;
}

// Characters, as the server counts them (UTF-8 lead bytes).
inline std::size_t CharacterCount(std::string_view text) {
  return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  }));
}

// The name to send for what was typed: trimmed, and only one the server
// stores. The app always asks for a name, although a key made through the API
// may have none (UnnamedKey). nullopt: Create is not offered.
inline std::optional<std::string> NameToCreate(std::string_view typed) {
  const std::string_view name = TrimSpace(typed);
  if (name.empty() || kMaxNameLength < CharacterCount(name)) return std::nullopt;
  // the server's column cannot hold a NUL
  if (name.find('\0') != std::string_view::npos) return std::nullopt;
  return std::string(name);
}

// ---- the words ------------------------------------------------------------------

// How the page's words are made: the store's strings and the user's dates, all
// UTF-8. ApiKeysPage passes urnw::Localized and Format and the Windows dates
// (StatsFormat.h); the tests pass English and fakes.
struct Text {
  std::function<std::string(std::string_view key)> localized;
  // a key with one placeholder
  std::function<std::string(std::string_view key, const std::string& value)> format;
  // the localized date a key was created on
  std::function<std::string(int64_t unixMillis)> date;
  // the full localized date and time, which the date is read out as
  std::function<std::string(int64_t unixMillis)> dateTime;
};

// A key's name on its row and in its confirmation; a key made through the API
// with no name (or only spaces) is "Unnamed key".
inline std::string NameText(const Key& key, const Text& text) {
  return TrimSpace(key.name).empty() ? text.localized("api_keys_unnamed") : key.name;
}

// ---- the view -------------------------------------------------------------------

// A delete's state on its row.
enum class DeleteState {
  Idle,     // "Delete" is offered
  Pending,  // the request is out: progress, the button disabled
  Failed,   // "Couldn't delete" under the lines, and "Delete" offered again
};

// One key's row: its name, when it was created and its id.
struct Row {
  std::string id;           // what the delete takes
  std::string name;         // the name, or "Unnamed key"
  std::string created;      // "Created 10/29/2026"; "" when the time is unknown
  std::string createdSpoken;  // the same, with the full date and time
  std::string idText;       // "ID 0193c4a1-..."
  DeleteState del = DeleteState::Idle;

  bool operator==(const Row&) const = default;
};

// What the page's body shows.
enum class Body {
  Progress,    // never loaded, and the load has not failed (or is being retried)
  LoadFailed,  // the first load failed: "Couldn't load" and Try again
  Empty,       // loaded, and there are none
  Rows,
};

struct View {
  Body body = Body::Progress;
  // the header's refresh indicator: a load is running over a list on screen
  bool refreshing = false;
  // the last load failed over a list on screen: the list stays, with the notice
  bool refreshFailed = false;
  std::vector<Row> rows;

  bool operator==(const View&) const = default;
};

// What the page knows. UI thread only.
struct State {
  bool loaded = false;   // a list came back since the page opened
  bool loading = false;  // a load is out
  bool failed = false;   // the last load failed
  std::vector<Key> keys;
  // the deletes that are running or failed, by key id
  std::map<std::string, DeleteState, std::less<>> deletes;
};

inline Row RowFor(const Key& key, const State& state, const Text& text) {
  Row row;
  row.id = key.id;
  row.name = NameText(key, text);
  if (0 < key.createTimeMillis) {
    row.created = text.format("api_keys_created_at", text.date(key.createTimeMillis));
    row.createdSpoken = text.format("api_keys_created_at", text.dateTime(key.createTimeMillis));
  }
  row.idText = text.format("api_keys_id", key.id);
  if (const auto found = state.deletes.find(key.id); found != state.deletes.end()) {
    row.del = found->second;
  }
  return row;
}

// The page from what it knows. A list that loaded stays through a failed
// reload, with its notice.
inline View ViewFor(const State& state, const Text& text) {
  View view;
  if (!state.loaded) {
    // a Try again re-runs the load with the failure still on the state
    view.body = state.failed && !state.loading ? Body::LoadFailed : Body::Progress;
    return view;
  }
  view.refreshing = state.loading;
  view.refreshFailed = state.failed;
  for (const auto& key : state.keys) view.rows.push_back(RowFor(key, state, text));
  view.body = view.rows.empty() ? Body::Empty : Body::Rows;
  return view;
}

// ---- the delete confirmation ----------------------------------------------------

struct Confirmation {
  std::string title;
  std::string body;

  bool operator==(const Confirmation&) const = default;
};

// Every delete asks first, naming the key; Delete is never the default.
inline Confirmation DeleteConfirmationFor(const Row& row, const Text& text) {
  return Confirmation{.title = text.localized("api_keys_delete_confirm_title"),
                      .body = text.format("api_keys_delete_confirm_body", row.name)};
}

// ---- the page's model -----------------------------------------------------------
//
// Each answer comes back on the sdk's thread and is applied on the UI thread
// later, and by then the page may have closed (a rail navigation, a sign-out)
// and opened again, for another account. Each open takes a new number, each
// request carries the number it went out under, and only the open page's
// answers apply (Admits). Within an open, only the latest load's answer
// applies, so a load overtaken by a newer one (a refresh, the reload after a
// create or a delete) cannot bring back a key deleted since. UI thread only.
class Model {
 public:
  // The page opens: nothing of an earlier list stays, and every answer still
  // out for an earlier open is dropped.
  uint64_t Open() {
    state_ = {};
    ++lastLoad_;  // no load from before is the latest
    open_ = ++lastOpen_;
    return open_;
  }
  // The page closes: every answer still out is dropped.
  void Close() {
    state_ = {};
    ++lastLoad_;
    open_ = 0;
  }
  // the open page's number, 0 with none open: what a confirmation or a sheet
  // that outlives its list is checked against
  uint64_t Current() const { return open_; }
  bool Admits(uint64_t generation) const { return generation != 0 && generation == open_; }

  // A load goes out: the number its answer carries.
  uint64_t BeginLoad() {
    state_.loading = true;
    return ++lastLoad_;
  }
  // A load's answer: applied only when it is the latest load's. A failure
  // keeps the list on screen.
  bool ApplyLoad(uint64_t load, ListOutcome outcome) {
    if (load != lastLoad_) return false;
    state_.loading = false;
    if (!outcome.ok) {
      state_.failed = true;
      return true;
    }
    state_.failed = false;
    state_.loaded = true;
    state_.keys = std::move(outcome.keys);
    // a delete's state stays only on a key still listed
    std::erase_if(state_.deletes, [this](const auto& entry) { return !Listed(entry.first); });
    return true;
  }

  // A confirmed delete goes out: false, and nothing goes out, for a key not
  // listed or one whose delete is already running.
  bool BeginDelete(std::string_view id) {
    if (!Listed(id)) return false;
    const auto found = state_.deletes.find(id);
    if (found != state_.deletes.end() && found->second == DeleteState::Pending) return false;
    state_.deletes.insert_or_assign(std::string(id), DeleteState::Pending);
    return true;
  }
  // A delete's answer: the deleted key leaves the list at once (the reload
  // that follows confirms it); a failure shows on its row.
  void ApplyDelete(std::string_view id, bool ok) {
    const auto found = state_.deletes.find(id);
    if (found == state_.deletes.end() || found->second != DeleteState::Pending) return;
    if (!ok) {
      found->second = DeleteState::Failed;
      return;
    }
    state_.deletes.erase(found);
    std::erase_if(state_.keys, [id](const Key& key) { return key.id == id; });
  }

  const State& state() const { return state_; }

 private:
  bool Listed(std::string_view id) const {
    return std::any_of(state_.keys.begin(), state_.keys.end(),
                       [id](const Key& key) { return key.id == id; });
  }

  State state_;
  uint64_t lastOpen_ = 0;
  uint64_t open_ = 0;
  uint64_t lastLoad_ = 0;
};

// ---- the create sheet -------------------------------------------------------------

// The sheet's steps: the name, the request, then the key, once.
enum class CreateStep { Name, Creating, Created };

// The create sheet (ApiKeyCreateSheet in ApiKeysPage.cpp) as values: what it
// shows and offers at each step, and the one copy of the key. UI thread only.
class CreateFlow {
 public:
  CreateFlow() = default;
  CreateFlow(const CreateFlow&) = delete;
  CreateFlow& operator=(const CreateFlow&) = delete;
  ~CreateFlow() { Wipe(secret_); }

  // Create is offered on the name step, for a name the server takes.
  bool CanCreate(std::string_view typed) const {
    return step_ == CreateStep::Name && !closed_ && NameToCreate(typed).has_value();
  }
  // Create pressed: the name to send, or nullopt when nothing goes out.
  std::optional<std::string> Begin(std::string_view typed) {
    if (!CanCreate(typed)) return std::nullopt;
    step_ = CreateStep::Creating;
    failed_ = false;
    return NameToCreate(typed);
  }
  // The answer. Its key moves into the flow to be shown; a failure goes back
  // to the name step with its words, the name kept. An answer for a sheet
  // already closed, or one that was not waited for, is wiped.
  void Apply(CreateOutcome& outcome) {
    if (closed_ || step_ != CreateStep::Creating) {
      Wipe(outcome.secret);
      return;
    }
    if (!outcome.ok || outcome.secret.empty()) {
      Wipe(outcome.secret);
      step_ = CreateStep::Name;
      failed_ = true;
      return;
    }
    secret_ = std::move(outcome.secret);
    Wipe(outcome.secret);
    step_ = CreateStep::Created;
  }
  // The sheet closes, by any route: the key is wiped, and so is any answer
  // that comes later.
  void Close() {
    closed_ = true;
    Wipe(secret_);
  }

  // A close by Esc is refused while the request is out, so the key it brings
  // is shown; the sheet's own close button is hidden then.
  bool HoldsClose() const { return step_ == CreateStep::Creating && !closed_; }
  // the key, on the Created step only
  const std::string& secret() const { return secret_; }
  CreateStep step() const { return step_; }
  bool failed() const { return failed_; }
  bool closed() const { return closed_; }

 private:
  CreateStep step_ = CreateStep::Name;
  bool failed_ = false;
  bool closed_ = false;
  std::string secret_;
};

}  // namespace urnw::apikeys
