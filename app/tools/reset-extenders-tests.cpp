// Executable spec for "Reset extenders" on Windows (connect EXTENDER.md E7):
// the reset_extenders verb on the control pipe (Common/Protocol.h) -- its tag,
// its strict request, the reply's `reset` and an older service's silence --
// the key the app names its space by and the service looks the space up with
// (ResetExtendersRequestFor, SpaceKeyOf), and what the Account page's
// extenders pane decides (App/ExtenderPresentation.h): the form a reset leaves,
// read back off the sdk's settings (ExtenderSettingsViewOf), and when its
// button is live (ExtenderResetEnabled). Run against the same sources the
// service and the app compile; it needs nlohmann/json, like the app.
//
//   c++ -std=c++20 -I ../src/Common -I ../src/App -I <dir with nlohmann/json.hpp> reset-extenders-tests.cpp ../src/App/ExtenderPresentation.cpp -o /tmp/reset-extenders-tests && /tmp/reset-extenders-tests
//
// With URNW_RESET_EXTENDERS_TESTS_SDK the key and settings templates run on
// the generated header's own urnet::NetworkSpaceKey and
// urnet::ExtenderSettings, and the space values the service imports are read
// through its json: fields only, nothing here calls into the sdk library. The
// header is a system include, like nlohmann/json: the generated code does not
// build with -Wextra -Werror.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "ExtenderPresentation.h"
#include "Protocol.h"

#if defined(URNW_RESET_EXTENDERS_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

using namespace urnw;
using namespace urnw::proto;

namespace {

#if defined(URNW_RESET_EXTENDERS_TESTS_SDK)
using SpaceKey = urnet::NetworkSpaceKey;
using Settings = urnet::ExtenderSettings;
#else
// urnet::NetworkSpaceKey and urnet::ExtenderSettings, by the generated
// wrapper's field names and types.
struct SpaceKey {
  std::optional<std::string> host_name;
  std::optional<std::string> env_name;
};

struct Settings {
  std::string DnsName;
  bool DnsNameDefault = false;
  std::string GossipUrl;
  bool GossipUrlDefault = false;
  std::optional<std::vector<std::string>> Hosts;
  std::string NetworkHost;
  std::optional<std::vector<std::string>> RootPublicKeys;
  bool RootPublicKeysDefault = false;
};
#endif

int gFailures = 0;
int gCases = 0;

// Counts a case, and prints `what` when it fails.
void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << std::endl;
  }
}

// A reset id as NetworkSpace::resetExtenders mints one (a ulid, in the uuid
// form connect ids print in).
constexpr const char* kResetId = "01926f3a-5b7c-7d8e-9f01-23456789abcd";

ResetExtenders SampleRequest() {
  ResetExtenders request;
  request.host_name = "network.example";
  request.env_name = "main";
  request.extender_reset_id = kResetId;
  return request;
}

// The request the service reads off the wire bytes of `request`, or nullopt
// where its parse refuses them.
std::optional<ResetExtenders> Served(const nlohmann::json& request) {
  try {
    return nlohmann::json::parse(DumpForWire(request)).get<ResetExtenders>();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// (a) the verb and its envelope
void TestTag() {
  Check(std::string(msg::kResetExtenders) == "reset_extenders", "wire: the reset_extenders tag");
  const nlohmann::json request = Request(msg::kResetExtenders, nlohmann::json(SampleRequest()));
  Check(TypeOf(request) == "reset_extenders", "wire: the request carries its type");
}

// (b) the request survives the wire whole
void TestRequestJson() {
  const std::optional<ResetExtenders> back =
      Served(Request(msg::kResetExtenders, nlohmann::json(SampleRequest())));
  Check(back && back->host_name == "network.example" && back->env_name == "main" &&
            back->extender_reset_id == kResetId,
        "wire: the request round-trips its key and its reset id");
}

// (c) a request that does not name its space and its reset exactly is refused,
// never read with a default: a default key names another space
void TestRequestStrict() {
  for (const char* field : {"host_name", "env_name", "extender_reset_id"}) {
    const nlohmann::json whole = nlohmann::json(SampleRequest());
    std::vector<std::pair<std::string, nlohmann::json>> malformed;
    {
      nlohmann::json missing = whole;
      missing.erase(field);
      malformed.emplace_back("missing", missing);
    }
    for (const auto& [name, value] :
         std::vector<std::pair<std::string, nlohmann::json>>{{"empty", ""},
                                                             {"null", nullptr},
                                                             {"a number", 7},
                                                             {"an object", nlohmann::json::object()}}) {
      nlohmann::json changed = whole;
      changed[field] = value;
      malformed.emplace_back(name, changed);
    }
    for (const auto& [name, request] : malformed) {
      bool threw = false;
      try {
        (void)request.get<ResetExtenders>();
      } catch (const std::exception&) {
        threw = true;
      }
      Check(threw, std::string("wire: a request with ") + field + " " + name + " is refused");
    }
  }
}

// (d) the reply's `reset`, and what a service that did not reset, or does not
// know the verb, answers
void TestReply() {
  Reply reply;
  reply.ok = true;
  reply.in_reply_to = msg::kResetExtenders;
  reply.reset = true;
  const Reply back = nlohmann::json::parse(DumpForWire(nlohmann::json(reply))).get<Reply>();
  Check(back.ok && back.reset && back.in_reply_to == "reset_extenders",
        "reply: reset round-trips");

  Reply held;
  held.ok = true;
  held.in_reply_to = msg::kResetExtenders;
  const nlohmann::json heldJson = nlohmann::json(held);
  Check(!heldJson.contains("reset"), "reply: a reply that reset nothing carries no reset");
  Check(heldJson.get<Reply>().ok && !heldJson.get<Reply>().reset,
        "reply: a service that held no such space answers ok, nothing reset");

  const Reply older = nlohmann::json::parse(
                          R"({"type":"reply","ok":false,"error":"unknown request type: reset_extenders","in_reply_to":"reset_extenders"})")
                          .get<Reply>();
  Check(!older.ok && !older.reset, "reply: a service too old for the verb reset nothing");

  Reply refused;
  refused.ok = false;
  refused.error = "a tunnel operation is in progress";
  const Reply refusedBack =
      nlohmann::json::parse(DumpForWire(nlohmann::json(refused))).get<Reply>();
  Check(!refusedBack.ok && !refusedBack.reset &&
            refusedBack.error == "a tunnel operation is in progress",
        "reply: a refusal carries its reason and no reset");
  Check(kProtocolVersion == 4, "wire: no protocol bump for the verb and its field");
}

SpaceKey Key(std::optional<std::string> hostName, std::optional<std::string> envName) {
  SpaceKey key;
  key.host_name = std::move(hostName);
  key.env_name = std::move(envName);
  return key;
}

// (e) the app names the space its reset was of, and the service looks up the
// same key; a key or id that is missing sends nothing, since the service would
// refuse it, and the next import carries the reset instead
void TestKeys() {
  const std::optional<ResetExtenders> request =
      ResetExtendersRequestFor(std::optional<SpaceKey>(Key("network.example", "main")), kResetId);
  Check(request && request->host_name == "network.example" && request->env_name == "main" &&
            request->extender_reset_id == kResetId,
        "keys: the request names the app's space and its reset");
  // what the service parses off the pipe and looks the space up by
  const std::optional<ResetExtenders> served =
      request ? Served(Request(msg::kResetExtenders, nlohmann::json(*request))) : std::nullopt;
  const std::optional<SpaceKey> looked =
      served ? std::optional<SpaceKey>(SpaceKeyOf<SpaceKey>(*served)) : std::nullopt;
  Check(looked && looked->host_name == std::optional<std::string>("network.example") &&
            looked->env_name == std::optional<std::string>("main"),
        "keys: the service looks up the key the app's space has");

  Check(!ResetExtendersRequestFor(std::optional<SpaceKey>(), kResetId),
        "keys: a space with no key sends nothing");
  Check(!ResetExtendersRequestFor(std::optional<SpaceKey>(Key(std::nullopt, "main")), kResetId),
        "keys: a key with no host sends nothing");
  Check(!ResetExtendersRequestFor(std::optional<SpaceKey>(Key("network.example", std::nullopt)),
                                  kResetId),
        "keys: a key with no env sends nothing");
  Check(!ResetExtendersRequestFor(std::optional<SpaceKey>(Key("", "main")), kResetId),
        "keys: an empty host sends nothing");
  Check(!ResetExtendersRequestFor(std::optional<SpaceKey>(Key("network.example", "")), kResetId),
        "keys: an empty env sends nothing");
  Check(!ResetExtendersRequestFor(std::optional<SpaceKey>(Key("network.example", "main")), ""),
        "keys: no reset id sends nothing");

  ResetExtenders other = SampleRequest();
  other.host_name = "other.example";
  other.env_name = "beta";
  const SpaceKey otherKey = SpaceKeyOf<SpaceKey>(other);
  Check(otherKey.host_name == std::optional<std::string>("other.example") &&
            otherKey.env_name == std::optional<std::string>("beta"),
        "keys: the host and the env each go to their own field");
}

// (f) the form the reset leaves, read back off the sdk's settings as a load
// reads them: every setting its default, so empty boxes naming the defaults
void TestResetForm() {
  Settings configured;
  configured.DnsName = "ext.example.test";
  configured.DnsNameDefault = false;
  configured.GossipUrl = "wss://gossip.example.test";
  configured.GossipUrlDefault = false;
  configured.Hosts = std::vector<std::string>{"192.0.2.10", "ext1.example.test"};
  configured.NetworkHost = "network.example";
  configured.RootPublicKeys = std::vector<std::string>{"root-key-1"};
  configured.RootPublicKeysDefault = false;
  const ExtenderSettingsView before = ExtenderSettingsViewOf(configured);
  Check(before.dnsName == "ext.example.test" && !before.dnsNameDefault,
        "settings: the dns name and its default flag are read");
  Check(before.gossipUrl == "wss://gossip.example.test" && !before.gossipUrlDefault,
        "settings: the gossip url and its default flag are read");
  Check(before.hosts == std::vector<std::string>{"192.0.2.10", "ext1.example.test"},
        "settings: the manual hosts are read, in order");
  Check(before.networkHost == "network.example", "settings: the network host is read");
  Check(before.rootPublicKeys == std::vector<std::string>{"root-key-1"} &&
            !before.rootPublicKeysDefault,
        "settings: the root keys and their default flag are read");
  const ExtenderSettingsForm beforeForm = ExtenderSettingsFormFor(before);
  Check(beforeForm.dnsNameText == "ext.example.test" &&
            beforeForm.hostsText == "192.0.2.10\next1.example.test",
        "settings: a configured form shows what was set");

  // ExtenderViewController::getSettings after NetworkSpace::resetExtenders
  Settings reset;
  reset.DnsName = "extender.network.example";
  reset.DnsNameDefault = true;
  reset.GossipUrl = "wss://gossip.network.example";
  reset.GossipUrlDefault = true;
  reset.NetworkHost = "network.example";
  reset.RootPublicKeysDefault = true;
  const ExtenderSettingsView after = ExtenderSettingsViewOf(reset);
  Check(after.dnsNameDefault && after.gossipUrlDefault && after.rootPublicKeysDefault &&
            after.hosts.empty() && after.rootPublicKeys.empty(),
        "settings: a reset's settings read as every default and no hosts");
  const ExtenderSettingsForm afterForm = ExtenderSettingsFormFor(after);
  Check(afterForm.dnsNameText.empty() && afterForm.gossipUrlText.empty() &&
            afterForm.hostsText.empty(),
        "reset form: the three boxes are empty, which is the default");
  Check(afterForm.dnsNameDefaultValue == "extender.network.example" &&
            afterForm.gossipUrlDefaultValue == "wss://gossip.network.example",
        "reset form: the placeholders name the defaults");
  Check(afterForm != beforeForm, "reset form: the reset form differs from the configured one");
}

// (g) when the pane's button is live: signed in, session or not, and not while
// another write of the pane runs
void TestResetEnabled() {
  Check(ExtenderResetEnabled(/*signedIn=*/true, /*writing=*/false),
        "button: live for a signed-in account, with or without a session");
  Check(!ExtenderResetEnabled(/*signedIn=*/false, /*writing=*/false),
        "button: off while signed out");
  Check(!ExtenderResetEnabled(/*signedIn=*/true, /*writing=*/true),
        "button: off while a save or a reset of the pane runs");
}

#if defined(URNW_RESET_EXTENDERS_TESTS_SDK)
// (h) the reset's id travels in the space's values, by the generated header's
// own json: what start_tunnel and start_provider hand the service, and what
// the app's whole-values writers read and write back
void TestValuesCarryTheResetId() {
  urnet::NetworkSpaceValues values;
  values.extender_reset_id = kResetId;
  const nlohmann::json valuesJson = values;
  Check(valuesJson.value("extender_reset_id", std::string()) == kResetId,
        "values: the reset id is written as extender_reset_id");
  const urnet::NetworkSpaceValues back = valuesJson.get<urnet::NetworkSpaceValues>();
  Check(back.extender_reset_id == std::optional<std::string>(kResetId),
        "values: the reset id reads back");
}
#endif

}  // namespace

int main() {
  TestTag();
  TestRequestJson();
  TestRequestStrict();
  TestReply();
  TestKeys();
  TestResetForm();
  TestResetEnabled();
#if defined(URNW_RESET_EXTENDERS_TESTS_SDK)
  TestValuesCarryTheResetId();
#endif
  std::cout << (gCases - gFailures) << "/" << gCases << " reset extenders checks passed"
#if defined(URNW_RESET_EXTENDERS_TESTS_SDK)
            << " (against urnetwork_sdk.hpp)"
#endif
            << std::endl;
  return gFailures == 0 ? 0 : 1;
}
