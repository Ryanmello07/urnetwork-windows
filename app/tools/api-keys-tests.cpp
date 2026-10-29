// Executable spec for the Account > API keys page (App/ApiKeysPresentation.h,
// owner decision 2026-10-29): the sdk's three answers read as plain values
// (a failed call, a server error, keys with no id, every create_time the sdk
// can carry, and the list's order), the name a key is created with, every
// row's lines and the "Unnamed key" fallback, progress, failed, empty and the
// rows, a reload over the list and a failed one, the delete states, the
// confirmation, the model's fence and its latest-load rule, and the create
// sheet's steps with the one copy of the key: taken from the sdk's result,
// shown, and wiped when the sheet closes by any route or when an answer
// finds it closed. The WinUI half (App/ApiKeysPage.cpp) cannot be built off
// Windows; this is every decision it makes before it touches a XAML object,
// run on the same header the app compiles.
//
//   c++ -std=c++20 -I ../src/App api-keys-tests.cpp -o /tmp/api-keys-tests && /tmp/api-keys-tests
//
// With URNW_API_KEYS_TESTS_SDK the readers also run on the generated header's
// own result types, and the whole binding path runs under them: the
// wrapper's Api::listApiKeys, Api::createApiKey and Api::deleteApiKey over a
// fake of the C ABI functions they call, which records the JSON the wrapper
// sends and answers with the JSON the sdk's cgo layer writes, so the page's
// calls are the sdk's calls and its readers read what the sdk delivers. The
// header needs nlohmann/json; both are system includes because the generated
// code does not build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_API_KEYS_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> api-keys-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ApiKeysPresentation.h"

#if defined(URNW_API_KEYS_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

namespace ak = urnw::apikeys;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

void CheckText(const std::string& want, const std::string& got, const std::string& what) {
  Check(want == got, what + ": want \"" + want + "\", got \"" + got + "\"");
}

void CheckEq(int64_t want, int64_t got, const std::string& what) {
  Check(want == got, what + ": want " + std::to_string(want) + ", got " + std::to_string(got));
}

// ---- the store's English ------------------------------------------------------
//
// The catalog's own English for every key the presentation reads, with its
// placeholder as the catalog lowers it ({}). tests/api_keys_test.go checks
// each entry against Strings/en/Resources.resw, so the cases below read the
// words the app shows.
const std::map<std::string, std::string, std::less<>> kEnglish = {
    {"api_keys_unnamed", "Unnamed key"},
    {"api_keys_created_at", "Created {}"},
    {"api_keys_id", "ID {}"},
    {"api_keys_delete_confirm_title", "Delete this API key?"},
    {"api_keys_delete_confirm_body",
     "\"{}\" will stop working right away, and anything using it will lose access."},
};

std::string English(std::string_view key) {
  const auto found = kEnglish.find(key);
  if (found == kEnglish.end()) {
    Check(false, "the spec has no English for " + std::string(key));
    return std::string(key);
  }
  return found->second;
}

std::string Replace(std::string text, std::string_view placeholder, const std::string& value) {
  const auto at = text.find(placeholder);
  if (at != std::string::npos) text.replace(at, placeholder.size(), value);
  return text;
}

// what a platform date reads as here: the millis it was asked for, so a case
// can tell which time a line used
std::string FakeDate(int64_t millis) { return "<date " + std::to_string(millis) + ">"; }
std::string FakeDateTime(int64_t millis) { return "<full " + std::to_string(millis) + ">"; }

ak::Text EnglishText() {
  ak::Text text;
  text.localized = [](std::string_view key) { return English(key); };
  text.format = [](std::string_view key, const std::string& value) {
    return Replace(English(key), "{}", value);
  };
  text.date = FakeDate;
  text.dateTime = FakeDateTime;
  return text;
}

// ---- fixtures -------------------------------------------------------------------
//
// Synthetic ids and a visibly synthetic key in the server's shape (urn_ and 52
// base32hex characters).

constexpr const char* kKeyA = "0193c4a1-7d1e-7b8a-9f00-1c2d3e4f5a6b";
constexpr const char* kKeyB = "0193c4a2-8e2f-7c9b-8e11-2d3e4f5a6b7c";
constexpr const char* kKeyC = "0193c4a3-9f30-7dac-9d22-3e4f5a6b7c8d";
constexpr const char* kSecret = "urn_TESTKEYTESTKEYTESTKEYTESTKEYTESTKEYTESTKEYTESTKEY0000";

// 2026-10-29T15:04:05Z and 2026-10-28T09:00:00Z, in Unix seconds
constexpr int64_t kNewerSeconds = 1'793'286'245;
constexpr int64_t kOlderSeconds = 1'793'178'000;

ak::Key MakeKey(std::string id, std::string name, int64_t createTimeMillis) {
  return ak::Key{.id = std::move(id), .name = std::move(name), .createTimeMillis = createTimeMillis};
}

ak::ListOutcome Listed(std::vector<ak::Key> keys) {
  ak::ListOutcome outcome;
  outcome.ok = true;
  outcome.keys = std::move(keys);
  ak::SortKeys(outcome.keys);
  return outcome;
}

ak::ListOutcome ListFailed() {
  ak::ListOutcome outcome;
  outcome.error = "Get \"https://api.example/account/api-keys\": context deadline exceeded";
  return outcome;
}

ak::CreateOutcome Created(std::string secret = kSecret) {
  ak::CreateOutcome outcome;
  outcome.ok = true;
  outcome.id = kKeyA;
  outcome.name = "home server";
  outcome.secret = std::move(secret);
  return outcome;
}

// ---- fakes of the generated header's result types -------------------------------
//
// The fields the readers read, by the generated names and types.

struct FakeApiError {
  std::string message;
};

struct FakeKey {
  std::optional<std::string> id;
  std::string name;
  std::optional<std::string> create_time;
};

struct FakeListResult {
  std::optional<std::vector<FakeKey>> api_keys;
  std::optional<FakeApiError> error;
};

struct FakeCreateResult {
  std::optional<std::string> id;
  std::optional<std::string> api_key;
  std::optional<std::string> name;
  std::optional<FakeApiError> error;
};

struct FakeDeleteResult {
  std::optional<FakeApiError> error;
};

struct FakeTypes {
  using ApiError = FakeApiError;
  using Key = FakeKey;
  using ListResult = FakeListResult;
  using CreateResult = FakeCreateResult;
  using DeleteResult = FakeDeleteResult;
};

#if defined(URNW_API_KEYS_TESTS_SDK)
struct SdkTypes {
  using ApiError = urnet::ApiError;
  using Key = urnet::PublicAccountApiKey;
  using ListResult = urnet::ListApiKeysResult;
  using CreateResult = urnet::CreateApiKeyResult;
  using DeleteResult = urnet::DeleteApiKeyResult;
};
#endif

template <typename T>
typename T::Key ResultKey(std::optional<std::string> id, std::string name,
                          std::optional<std::string> createTime) {
  typename T::Key key{};
  key.id = std::move(id);
  key.name = std::move(name);
  key.create_time = std::move(createTime);
  return key;
}

// ---- the cases --------------------------------------------------------------------

void TestCreateTime() {
  const struct {
    std::optional<std::string> text;
    int64_t wantMillis;
    const char* what;
  } cases[] = {
      {"2026-10-29T15:04:05Z", kNewerSeconds * 1000, "Go's UTC time"},
      {"2026-10-29T15:04:05.123456Z", kNewerSeconds * 1000,
       "with the microseconds a postgres timestamp carries"},
      {"2026-10-29T15:04:05.123456789Z", kNewerSeconds * 1000, "with nanoseconds"},
      {"2026-10-29T15:04:05+02:00", (kNewerSeconds - 2 * 3600) * 1000, "with an offset"},
      {"2026-10-29T15:04:05-05:30", (kNewerSeconds + 5 * 3600 + 30 * 60) * 1000,
       "with a negative offset"},
      {std::nullopt, 0, "a null create_time is unknown"},
      {"", 0, "an empty create_time is unknown"},
      {"0001-01-01T00:00:00Z", 0, "Go's zero time is unknown, not year 1"},
      {"1970-01-01T00:00:00Z", 0, "the epoch itself is unknown"},
      {"2026-10-29", 0, "a date with no time does not parse"},
      {"2026-10-29T15:04:05", 0, "a time with no zone does not parse"},
      {"2026-13-29T15:04:05Z", 0, "a 13th month does not parse"},
      {"not a time", 0, "garbage does not parse"},
  };
  for (const auto& c : cases) CheckEq(c.wantMillis, ak::CreateTimeMillis(c.text), c.what);
}

void TestOrder() {
  std::vector<ak::Key> keys = {
      MakeKey(kKeyC, "unknown time", 0),
      MakeKey(kKeyB, "older", kOlderSeconds * 1000),
      MakeKey("0193c4a5-0000-7000-8000-000000000005", "b same second", kNewerSeconds * 1000),
      MakeKey("0193c4a4-0000-7000-8000-000000000004", "a same second", kNewerSeconds * 1000),
      MakeKey(kKeyA, "a same second", kNewerSeconds * 1000),
  };
  ak::SortKeys(keys);
  const std::vector<std::string> want = {kKeyA, "0193c4a4-0000-7000-8000-000000000004",
                                         "0193c4a5-0000-7000-8000-000000000005", kKeyB, kKeyC};
  bool same = keys.size() == want.size();
  for (std::size_t i = 0; same && i < want.size(); ++i) same = keys[i].id == want[i];
  Check(same,
        "newest first, the same second by name then id, and the keys with no time last");
}

template <typename T>
void TestReaders(const std::string& which) {
  using ListResult = typename T::ListResult;
  using CreateResult = typename T::CreateResult;
  using DeleteResult = typename T::DeleteResult;

  // ---- list
  {
    const ak::ListOutcome failed =
        ak::ListOutcomeFrom(std::optional<ListResult>{}, std::optional<std::string>{"timeout"});
    Check(!failed.ok && failed.error == "timeout", which + ": a failed call fails the list");
    const ak::ListOutcome none =
        ak::ListOutcomeFrom(std::optional<ListResult>{}, std::optional<std::string>{});
    Check(!none.ok && !none.error.empty(), which + ": no result and no error is a failure");
    const ak::ListOutcome emptyErr =
        ak::ListOutcomeFrom(std::optional<ListResult>{ListResult{}}, std::optional<std::string>{""});
    Check(!emptyErr.ok && !emptyErr.error.empty(),
          which + ": an empty error string is still a failure, with words for the log");

    ListResult refused{};
    refused.error = typename T::ApiError{};
    refused.error->message = "Error getting account api keys";
    const ak::ListOutcome serverError =
        ak::ListOutcomeFrom(std::optional<ListResult>{refused}, std::nullopt);
    Check(!serverError.ok && serverError.error == "Error getting account api keys",
          which + ": the server's error fails the list, its message for the log");

    const ak::ListOutcome nothing =
        ak::ListOutcomeFrom(std::optional<ListResult>{ListResult{}}, std::nullopt);
    Check(nothing.ok && nothing.keys.empty(), which + ": no api_keys is an empty list");

    ListResult listed{};
    listed.api_keys.emplace();
    listed.api_keys->push_back(
        ResultKey<T>(std::string(kKeyB), "older", std::string("2026-10-28T09:00:00Z")));
    listed.api_keys->push_back(ResultKey<T>(std::nullopt, "no id", std::string("2026-10-29T15:04:05Z")));
    listed.api_keys->push_back(ResultKey<T>(std::string(""), "empty id", std::nullopt));
    listed.api_keys->push_back(
        ResultKey<T>(std::string(kKeyA), "home server", std::string("2026-10-29T15:04:05.123456Z")));
    listed.api_keys->push_back(ResultKey<T>(std::string(kKeyC), "", std::nullopt));
    const ak::ListOutcome read = ak::ListOutcomeFrom(std::optional<ListResult>{listed}, std::nullopt);
    Check(read.ok, which + ": a list reads");
    if (read.keys.size() != 3) {
      Check(false, which + ": the keys with no id are left out, three remain");
    } else {
      Check(read.keys[0] == MakeKey(kKeyA, "home server", kNewerSeconds * 1000),
            which + ": the newest key first, its time in milliseconds");
      Check(read.keys[1] == MakeKey(kKeyB, "older", kOlderSeconds * 1000), which + ": then the older");
      Check(read.keys[2] == MakeKey(kKeyC, "", 0), which + ": and the key with no time and no name last");
    }
  }

  // ---- create
  {
    std::optional<CreateResult> failedResult = CreateResult{};
    failedResult->api_key = std::string(kSecret);
    const ak::CreateOutcome failed =
        ak::CreateOutcomeFrom(failedResult, std::optional<std::string>{"timeout"});
    Check(!failed.ok && failed.secret.empty() && failed.error == "timeout",
          which + ": a failed call creates nothing");
    Check(failedResult->api_key && failedResult->api_key->empty(),
          which + ": a key on a failed answer is wiped, never shown");
    Check(failed.error.find("urn_") == std::string::npos, which + ": and never logged");

    std::optional<CreateResult> refused = CreateResult{};
    refused->error = typename T::ApiError{};
    refused->error->message = "Name is too long (limit 128 characters).";
    const ak::CreateOutcome serverError = ak::CreateOutcomeFrom(refused, std::nullopt);
    Check(!serverError.ok && serverError.error == "Name is too long (limit 128 characters).",
          which + ": the server's refusal, its message for the log");

    std::optional<CreateResult> keyless = CreateResult{};
    keyless->id = std::string(kKeyA);
    const ak::CreateOutcome noKey = ak::CreateOutcomeFrom(keyless, std::nullopt);
    Check(!noKey.ok && !noKey.error.empty(), which + ": an answer with no key is a failure");
    std::optional<CreateResult> blank = CreateResult{};
    blank->api_key = std::string();
    Check(!ak::CreateOutcomeFrom(blank, std::nullopt).ok, which + ": so is an empty key");
    std::optional<CreateResult> none;
    Check(!ak::CreateOutcomeFrom(none, std::nullopt).ok, which + ": and no answer at all");

    std::optional<CreateResult> made = CreateResult{};
    made->id = std::string(kKeyA);
    made->api_key = std::string(kSecret);
    made->name = std::string("home server");
    const ak::CreateOutcome created = ak::CreateOutcomeFrom(made, std::nullopt);
    Check(created.ok, which + ": a created key reads");
    CheckText(kSecret, created.secret, which + ": the key, whole");
    CheckText(kKeyA, created.id, which + ": its id");
    CheckText("home server", created.name, which + ": its name");
    Check(made->api_key && made->api_key->empty(),
          which + ": the key moved out of the sdk's result, nothing of it left there");
  }

  // ---- delete
  {
    Check(!ak::DeleteOutcomeFrom(std::optional<DeleteResult>{}, std::optional<std::string>{"timeout"}).ok,
          which + ": a failed call deletes nothing");
    DeleteResult refused{};
    refused.error = typename T::ApiError{};
    refused.error->message = "Error deleting api key";
    const ak::DeleteOutcome serverError =
        ak::DeleteOutcomeFrom(std::optional<DeleteResult>{refused}, std::nullopt);
    Check(!serverError.ok && serverError.error == "Error deleting api key",
          which + ": the server's error fails the delete");
    Check(!ak::DeleteOutcomeFrom(std::optional<DeleteResult>{}, std::nullopt).ok,
          which + ": no answer is a failure");
    Check(ak::DeleteOutcomeFrom(std::optional<DeleteResult>{DeleteResult{}}, std::nullopt).ok,
          which + ": an answer with no error is a delete");
  }
}

void TestNames() {
  Check(!ak::NameToCreate(""), "an empty name creates nothing");
  Check(!ak::NameToCreate(" \t\r\n "), "nor does one of spaces");
  Check(ak::NameToCreate("  home server \n") == std::optional<std::string>{"home server"},
        "a name is sent trimmed");
  Check(ak::NameToCreate(std::string(128, 'x')).has_value(), "128 characters are the server's limit");
  Check(!ak::NameToCreate(std::string(129, 'x')), "129 are refused here, not by the server");
  std::string accented;
  for (int i = 0; i < 128; ++i) accented += "\xC3\xA9";  // é, two bytes each
  Check(ak::NameToCreate(accented).has_value(), "the limit counts characters, not bytes");
  Check(!ak::NameToCreate(accented + "\xC3\xA9"), "129 two-byte characters are refused");
  Check(!ak::NameToCreate(std::string("home\0server", 11)), "a NUL, which the server cannot store");
  CheckEq(128, static_cast<int64_t>(ak::kMaxNameLength), "server model.MaxApiKeyNameLength");
}

void TestRows() {
  const ak::Text text = EnglishText();
  ak::Model model;
  model.Open();
  const uint64_t load = model.BeginLoad();
  model.ApplyLoad(load, Listed({MakeKey(kKeyA, "home server", kNewerSeconds * 1000),
                                MakeKey(kKeyB, "   ", kOlderSeconds * 1000),
                                MakeKey(kKeyC, "", 0)}));
  const ak::View view = ak::ViewFor(model.state(), text);
  Check(view.body == ak::Body::Rows, "a loaded list shows its rows");
  if (view.rows.size() != 3) {
    Check(false, "three keys, three rows");
    return;
  }
  const ak::Row& named = view.rows[0];
  CheckText(kKeyA, named.id, "the row keeps the whole id for the delete");
  CheckText("home server", named.name, "the name");
  CheckText("Created " + FakeDate(kNewerSeconds * 1000), named.created, "the date it was created");
  CheckText("Created " + FakeDateTime(kNewerSeconds * 1000), named.createdSpoken,
            "read out with the full date and time");
  CheckText(std::string("ID ") + kKeyA, named.idText, "the whole id, for use with the API");
  Check(named.del == ak::DeleteState::Idle, "Delete is offered");

  CheckText("Unnamed key", view.rows[1].name, "a name of spaces is no name");
  CheckText("Unnamed key", view.rows[2].name, "a key made through the API with no name");
  CheckText("", view.rows[2].created, "no time: no created line");
  CheckText("", view.rows[2].createdSpoken, "and nothing read out for it");
  CheckText(std::string("ID ") + kKeyC, view.rows[2].idText, "the id line, always");

  // long names are carried whole: the page trims, a screen reader reads all
  const std::string longName(128, 'n');
  CheckText(longName, ak::RowFor(MakeKey(kKeyA, longName, 0), model.state(), text).name,
            "a long name is kept whole");
}

void TestStates() {
  const ak::Text text = EnglishText();
  const auto body = [&](const ak::Model& model) { return ak::ViewFor(model.state(), text).body; };

  ak::Model model;
  model.Open();
  Check(body(model) == ak::Body::Progress, "never loaded: progress");
  uint64_t load = model.BeginLoad();
  Check(body(model) == ak::Body::Progress, "the first load running: progress");
  model.ApplyLoad(load, ListFailed());
  Check(body(model) == ak::Body::LoadFailed, "the first load failed: failed, with Retry");
  load = model.BeginLoad();
  Check(body(model) == ak::Body::Progress, "Retry: progress again, not the failure");
  model.ApplyLoad(load, Listed({}));
  Check(body(model) == ak::Body::Empty, "loaded and none: empty");
  load = model.BeginLoad();
  ak::View reloading = ak::ViewFor(model.state(), text);
  Check(reloading.body == ak::Body::Empty && reloading.refreshing,
        "a reload over an empty list keeps it, with the header's indicator");
  model.ApplyLoad(load, Listed({MakeKey(kKeyA, "home server", kNewerSeconds * 1000)}));
  ak::View rows = ak::ViewFor(model.state(), text);
  Check(rows.body == ak::Body::Rows && !rows.refreshing && !rows.refreshFailed, "the rows");
  load = model.BeginLoad();
  model.ApplyLoad(load, ListFailed());
  ak::View failed = ak::ViewFor(model.state(), text);
  Check(failed.body == ak::Body::Rows && failed.rows.size() == 1 && failed.refreshFailed,
        "a failed reload keeps the list, with its notice");
  load = model.BeginLoad();
  model.ApplyLoad(load, Listed({MakeKey(kKeyA, "home server", kNewerSeconds * 1000)}));
  Check(!ak::ViewFor(model.state(), text).refreshFailed, "the next load clears the notice");
}

void TestModel() {
  const ak::Text text = EnglishText();

  // the fence: an answer applies only to the open it went out under
  ak::Model model;
  Check(!model.Admits(0), "nothing open admits nothing");
  CheckEq(0, static_cast<int64_t>(model.Current()), "nothing open is 0");
  const uint64_t first = model.Open();
  Check(model.Admits(first) && model.Current() == first, "the open page's answers apply");
  model.Close();
  Check(!model.Admits(first), "a closed page's queued answer is dropped");
  Check(!model.Admits(model.Current()), "a confirmation from a closed page deletes nothing");
  const uint64_t second = model.Open();
  Check(second != first && !model.Admits(first) && model.Admits(second),
        "after a sign-out and a sign-in the older account's answer cannot repaint the page");
  const uint64_t third = model.Open();
  Check(!model.Admits(second) && model.Admits(third), "reopening without a close fences too");

  // reopening forgets the list
  uint64_t load = model.BeginLoad();
  model.ApplyLoad(load, Listed({MakeKey(kKeyA, "home server", kNewerSeconds * 1000)}));
  model.Open();
  Check(!model.state().loaded && model.state().keys.empty(), "a page opened again starts over");
  model.ApplyLoad(load, Listed({MakeKey(kKeyA, "home server", kNewerSeconds * 1000)}));
  Check(model.state().keys.empty(), "a load from before the reopen is not the latest");

  // only the latest load's answer applies
  ak::Model latest;
  latest.Open();
  const uint64_t older = latest.BeginLoad();
  const uint64_t newer = latest.BeginLoad();
  Check(latest.ApplyLoad(newer, Listed({MakeKey(kKeyB, "b", kOlderSeconds * 1000)})),
        "the latest load applies");
  Check(!latest.ApplyLoad(older, Listed({MakeKey(kKeyA, "a", kNewerSeconds * 1000),
                                          MakeKey(kKeyB, "b", kOlderSeconds * 1000)})),
        "an older load, answering after it, is dropped");
  Check(latest.state().keys.size() == 1 && latest.state().keys[0].id == kKeyB,
        "and the list is the latest load's");

  // deletes
  ak::Model deletes;
  deletes.Open();
  load = deletes.BeginLoad();
  deletes.ApplyLoad(load, Listed({MakeKey(kKeyA, "home server", kNewerSeconds * 1000),
                                  MakeKey(kKeyB, "laptop", kOlderSeconds * 1000)}));
  Check(!deletes.BeginDelete(kKeyC), "a key not listed is not deleted");
  Check(deletes.BeginDelete(kKeyA), "a confirmed delete goes out");
  Check(!deletes.BeginDelete(kKeyA), "a second delete of the same key does not, while one is out");
  ak::View pending = ak::ViewFor(deletes.state(), text);
  Check(pending.rows[0].del == ak::DeleteState::Pending, "the row shows its delete running");
  Check(pending.rows[1].del == ak::DeleteState::Idle, "and only that row");

  // a reload while the delete is out keeps it running on the row
  load = deletes.BeginLoad();
  deletes.ApplyLoad(load, Listed({MakeKey(kKeyA, "home server", kNewerSeconds * 1000),
                                  MakeKey(kKeyB, "laptop", kOlderSeconds * 1000)}));
  Check(ak::ViewFor(deletes.state(), text).rows[0].del == ak::DeleteState::Pending,
        "a reload does not forget a delete that is out");

  deletes.ApplyDelete(kKeyA, /*ok=*/false);
  Check(ak::ViewFor(deletes.state(), text).rows[0].del == ak::DeleteState::Failed,
        "a failed delete says so on its row");
  Check(deletes.BeginDelete(kKeyA), "and Delete is offered again");
  const uint64_t beforeDelete = deletes.BeginLoad();
  deletes.ApplyDelete(kKeyA, /*ok=*/true);
  Check(deletes.state().keys.size() == 1 && deletes.state().keys[0].id == kKeyB,
        "a deleted key leaves the list at once");
  Check(deletes.state().deletes.empty(), "and its delete state goes with it");
  deletes.ApplyDelete(kKeyA, /*ok=*/true);
  Check(deletes.state().keys.size() == 1, "a second answer for it changes nothing");

  // the reload after the delete; the load that went out before it, answering
  // late with the deleted key still in it, cannot bring the key back
  const uint64_t afterDelete = deletes.BeginLoad();
  Check(!deletes.ApplyLoad(beforeDelete, Listed({MakeKey(kKeyA, "home server", kNewerSeconds * 1000),
                                                 MakeKey(kKeyB, "laptop", kOlderSeconds * 1000)})),
        "a load overtaken by the reload after a delete is dropped");
  Check(deletes.state().keys.size() == 1, "and the deleted key stays gone");
  deletes.ApplyLoad(afterDelete, Listed({MakeKey(kKeyB, "laptop", kOlderSeconds * 1000)}));
  Check(deletes.state().keys.size() == 1 && deletes.state().keys[0].id == kKeyB,
        "the reload confirms it");

  // a failed delete of a key deleted elsewhere: the reload drops its state
  Check(deletes.BeginDelete(kKeyB), "another delete");
  deletes.ApplyDelete(kKeyB, /*ok=*/false);
  load = deletes.BeginLoad();
  deletes.ApplyLoad(load, Listed({}));
  Check(deletes.state().deletes.empty(), "a delete's state stays only on a key still listed");

  // closing drops what is running
  deletes.Close();
  Check(deletes.state().keys.empty() && deletes.state().deletes.empty(), "a closed page keeps nothing");
}

void TestConfirmation() {
  const ak::Text text = EnglishText();
  ak::Model model;
  model.Open();
  const uint64_t load = model.BeginLoad();
  model.ApplyLoad(load, Listed({MakeKey(kKeyA, "home server", kNewerSeconds * 1000),
                                MakeKey(kKeyB, "", kOlderSeconds * 1000)}));
  const ak::View view = ak::ViewFor(model.state(), text);
  const ak::Confirmation named = ak::DeleteConfirmationFor(view.rows[0], text);
  CheckText("Delete this API key?", named.title, "the title");
  CheckText("\"home server\" will stop working right away, and anything using it will lose access.",
            named.body, "the body names the key");
  CheckText("\"Unnamed key\" will stop working right away, and anything using it will lose access.",
            ak::DeleteConfirmationFor(view.rows[1], text).body,
            "a key with no name is named Unnamed key");
}

void TestCreateFlow() {
  // the name step
  {
    ak::CreateFlow flow;
    Check(flow.step() == ak::CreateStep::Name && !flow.HoldsClose(), "the sheet opens on the name");
    Check(!flow.CanCreate(""), "Create is not offered for no name");
    Check(!flow.CanCreate("   "), "nor for spaces");
    Check(flow.CanCreate("home server"), "it is for a name");
    Check(!flow.Begin("  "), "and nothing goes out without one");
    Check(flow.step() == ak::CreateStep::Name, "the sheet stays on the name");
    const std::optional<std::string> name = flow.Begin("  home server ");
    Check(name == std::optional<std::string>{"home server"}, "Create sends the trimmed name");
    Check(flow.step() == ak::CreateStep::Creating, "and waits");
    Check(flow.HoldsClose(), "Esc is refused while the request is out");
    Check(!flow.CanCreate("home server") && !flow.Begin("home server"),
          "a second Create does not go out while one is");

    ak::CreateOutcome outcome = Created();
    flow.Apply(outcome);
    Check(flow.step() == ak::CreateStep::Created, "the key comes back: the key step");
    CheckText(kSecret, flow.secret(), "the key is shown whole");
    Check(outcome.secret.empty(), "it moved into the flow, nothing of it left in the answer");
    Check(!flow.HoldsClose(), "the key step closes freely");
    Check(!flow.CanCreate("another"), "and creates nothing more");

    ak::CreateOutcome stray = Created();
    flow.Apply(stray);
    CheckText(kSecret, flow.secret(), "a stray answer does not replace the key on screen");
    Check(stray.secret.empty(), "and is wiped");

    flow.Close();
    Check(flow.closed() && flow.secret().empty(), "Done, Esc, or any close wipes the key");
  }

  // a failure goes back to the name
  {
    ak::CreateFlow flow;
    flow.Begin("home server");
    ak::CreateOutcome failed;
    failed.error = "Name is too long (limit 128 characters).";
    flow.Apply(failed);
    Check(flow.step() == ak::CreateStep::Name && flow.failed(), "a failure: the name, with its words");
    Check(flow.secret().empty(), "no key");
    Check(flow.CanCreate("home server"), "Create is offered again");
    flow.Begin("home server");
    Check(!flow.failed(), "a new attempt clears the failure");
    ak::CreateOutcome keyless = Created("");
    flow.Apply(keyless);
    Check(flow.step() == ak::CreateStep::Name && flow.failed(), "an answer with no key is a failure");
  }

  // the sheet closed before the answer: a sign-out's Dismiss
  {
    ak::CreateFlow flow;
    flow.Begin("home server");
    flow.Close();
    Check(!flow.HoldsClose(), "a closed sheet holds nothing open");
    ak::CreateOutcome late = Created();
    flow.Apply(late);
    Check(late.secret.empty(), "an answer for a closed sheet is wiped");
    Check(flow.secret().empty() && flow.step() != ak::CreateStep::Created,
          "and never shown");
    Check(!flow.Begin("home server"), "a closed sheet sends nothing");
  }

  // an answer the sheet did not wait for
  {
    ak::CreateFlow flow;
    ak::CreateOutcome unasked = Created();
    flow.Apply(unasked);
    Check(unasked.secret.empty() && flow.secret().empty(), "an answer before any Create is wiped");
  }
}

void TestOutcomeHoldsTheKeyOnce() {
  ak::CreateOutcome first = Created();
  ak::CreateOutcome moved(std::move(first));
  CheckText(kSecret, moved.secret, "a move carries the key");
  Check(first.secret.empty(), "and leaves none behind");
  ak::CreateOutcome assigned;
  assigned = std::move(moved);
  CheckText(kSecret, assigned.secret, "a move assignment carries it");
  Check(moved.secret.empty(), "and leaves none behind either");

  std::string text = kSecret;
  ak::Wipe(text);
  Check(text.empty(), "Wipe empties the string");
}

#if defined(URNW_API_KEYS_TESTS_SDK)
// ---- a fake of the C ABI the generated Api calls ----------------------------------
//
// One Api handle; each call records the JSON the wrapper sent and answers at
// once, on the calling thread, with the JSON the sdk's cgo layer writes (Go's
// json.Marshal of the result) or its error string.
namespace abi {

constexpr uint64_t kApi = 7;

struct Answer {
  const char* resultJson = nullptr;
  const char* err = nullptr;
};

Answer gAnswer;
std::string gSentArgs;
uint64_t gSentTo = 0;
int64_t gCalls = 0;
int64_t gReleased = 0;

}  // namespace abi
#endif

}  // namespace

#if defined(URNW_API_KEYS_TESTS_SDK)
extern "C" {

void urnet_free_string(char* s) { std::free(s); }

bool urnet_release(uint64_t handle) {
  if (handle == abi::kApi) ++abi::gReleased;
  return true;
}

void urnet_api_list_api_keys(uint64_t self, urnet_list_api_keys_cb callback_result,
                             void* callback_user_data) {
  ++abi::gCalls;
  abi::gSentTo = self;
  abi::gSentArgs.clear();
  if (callback_result) callback_result(callback_user_data, abi::gAnswer.resultJson, abi::gAnswer.err);
}

void urnet_api_create_api_key(uint64_t self, const char* args_json,
                              urnet_create_api_key_cb callback_result, void* callback_user_data) {
  ++abi::gCalls;
  abi::gSentTo = self;
  abi::gSentArgs = args_json ? args_json : "";
  if (callback_result) callback_result(callback_user_data, abi::gAnswer.resultJson, abi::gAnswer.err);
}

void urnet_api_delete_api_key(uint64_t self, const char* args_json,
                              urnet_delete_api_key_cb callback_result, void* callback_user_data) {
  ++abi::gCalls;
  abi::gSentTo = self;
  abi::gSentArgs = args_json ? args_json : "";
  if (callback_result) callback_result(callback_user_data, abi::gAnswer.resultJson, abi::gAnswer.err);
}

}  // extern "C"

namespace {

// The page's three calls, made as ApiKeysPage.cpp makes them, through the
// generated wrapper and over the fake above.
void TestSdkCalls() {
  {
    const urnet::Api api(abi::kApi);

    // listApiKeys: what the sdk delivers for the server's list, as Go writes it
    abi::gAnswer = {
        R"({"api_keys":[)"
        R"({"id":"0193c4a2-8e2f-7c9b-8e11-2d3e4f5a6b7c","name":"older","create_time":"2026-10-28T09:00:00Z"},)"
        R"({"id":null,"name":"no id","create_time":null},)"
        R"({"id":"0193c4a1-7d1e-7b8a-9f00-1c2d3e4f5a6b","name":"home server","create_time":"2026-10-29T15:04:05.123456Z"},)"
        R"({"id":"0193c4a3-9f30-7dac-9d22-3e4f5a6b7c8d","name":"","create_time":null}]})",
        nullptr};
    std::optional<ak::ListOutcome> listed;
    api.listApiKeys([&listed](std::optional<urnet::ListApiKeysResult> result,
                              std::optional<std::string> err) {
      listed = ak::ListOutcomeFrom(result, err);
    });
    Check(abi::gSentTo == abi::kApi, "listApiKeys calls on the page's Api");
    Check(listed && listed->ok, "the sdk's list reads");
    if (listed && listed->keys.size() == 3) {
      Check(listed->keys[0] == MakeKey(kKeyA, "home server", kNewerSeconds * 1000) &&
                listed->keys[1] == MakeKey(kKeyB, "older", kOlderSeconds * 1000) &&
                listed->keys[2] == MakeKey(kKeyC, "", 0),
            "every field the sdk carries, newest first, the id-less key left out");
    } else {
      Check(false, "three keys from the sdk's list");
    }
    abi::gAnswer = {R"({"api_keys":[]})", nullptr};
    listed.reset();
    api.listApiKeys([&listed](std::optional<urnet::ListApiKeysResult> result,
                              std::optional<std::string> err) {
      listed = ak::ListOutcomeFrom(result, err);
    });
    Check(listed && listed->ok && listed->keys.empty(), "an empty list is empty, not a failure");
    abi::gAnswer = {R"({"error":{"message":"Error getting account api keys"}})", nullptr};
    listed.reset();
    api.listApiKeys([&listed](std::optional<urnet::ListApiKeysResult> result,
                              std::optional<std::string> err) {
      listed = ak::ListOutcomeFrom(result, err);
    });
    Check(listed && !listed->ok && listed->error == "Error getting account api keys",
          "the server's error, delivered by the sdk");
    abi::gAnswer = {nullptr, "Get \"https://api.example/account/api-keys\": EOF"};
    listed.reset();
    api.listApiKeys([&listed](std::optional<urnet::ListApiKeysResult> result,
                              std::optional<std::string> err) {
      listed = ak::ListOutcomeFrom(result, err);
    });
    Check(listed && !listed->ok, "a failed request, delivered by the sdk");

    // createApiKey: the name the page sends, and the key that comes back
    urnet::CreateApiKeyArgs createArgs;
    createArgs.name = *ak::NameToCreate("  home server ");
    abi::gAnswer = {
        R"({"id":"0193c4a1-7d1e-7b8a-9f00-1c2d3e4f5a6b","api_key":"urn_TESTKEYTESTKEYTESTKEYTESTKEYTESTKEYTESTKEYTESTKEY0000","name":"home server"})",
        nullptr};
    ak::CreateFlow flow;
    flow.Begin("  home server ");
    std::optional<ak::CreateOutcome> created;
    api.createApiKey(createArgs, [&created](std::optional<urnet::CreateApiKeyResult> result,
                                            std::optional<std::string> err) {
      created.emplace(ak::CreateOutcomeFrom(result, err));
      Check(result && result->api_key && result->api_key->empty(),
            "the key moved out of the sdk's result in the callback");
    });
    CheckText(R"({"name":"home server"})", abi::gSentArgs, "createApiKey sends the trimmed name");
    Check(created && created->ok, "the created key reads");
    if (created) {
      flow.Apply(*created);
      CheckText(kSecret, flow.secret(), "the sheet shows the key the sdk delivered");
      Check(created->secret.empty(), "and holds the only copy");
      flow.Close();
      Check(flow.secret().empty(), "until it closes");
    }
    abi::gAnswer = {R"({"error":{"message":"Name is too long (limit 128 characters)."}})", nullptr};
    created.reset();
    api.createApiKey(createArgs, [&created](std::optional<urnet::CreateApiKeyResult> result,
                                            std::optional<std::string> err) {
      created.emplace(ak::CreateOutcomeFrom(result, err));
    });
    Check(created && !created->ok && created->secret.empty(), "the server's refusal creates nothing");

    // deleteApiKey: the id the page sends
    urnet::DeleteApiKeyArgs deleteArgs;
    deleteArgs.id = std::string(kKeyB);
    abi::gAnswer = {"{}", nullptr};
    std::optional<ak::DeleteOutcome> deleted;
    api.deleteApiKey(deleteArgs, [&deleted](std::optional<urnet::DeleteApiKeyResult> result,
                                            std::optional<std::string> err) {
      deleted = ak::DeleteOutcomeFrom(result, err);
    });
    CheckText(std::string(R"({"id":")") + kKeyB + R"("})", abi::gSentArgs,
              "deleteApiKey sends the key's id");
    Check(deleted && deleted->ok, "a delete the server took");
    abi::gAnswer = {R"({"error":{"message":"Error deleting api key"}})", nullptr};
    deleted.reset();
    api.deleteApiKey(deleteArgs, [&deleted](std::optional<urnet::DeleteApiKeyResult> result,
                                            std::optional<std::string> err) {
      deleted = ak::DeleteOutcomeFrom(result, err);
    });
    Check(deleted && !deleted->ok, "a delete the server refused");
    CheckEq(8, abi::gCalls, "every call reached the sdk once");
  }
  CheckEq(1, abi::gReleased, "the Api's handle is released once");
}

}  // namespace
#endif

int main() {
  TestCreateTime();
  TestOrder();
  TestReaders<FakeTypes>("fakes");
  TestNames();
  TestRows();
  TestStates();
  TestModel();
  TestConfirmation();
  TestCreateFlow();
  TestOutcomeHoldsTheKeyOnce();
#if defined(URNW_API_KEYS_TESTS_SDK)
  TestReaders<SdkTypes>("sdk header");
  TestSdkCalls();
#endif
  if (gFailures != 0) {
    std::cout << gFailures << " of " << gCases << " api keys checks failed\n";
    return 1;
  }
  std::cout << gCases << "/" << gCases << " api keys checks passed\n";
  return 0;
}
