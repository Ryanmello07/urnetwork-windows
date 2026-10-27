// Executable spec for handing the network space's ingress dns -- the
// connection region and the bootstrap DNS-over-HTTPS servers (sdk
// control_doh.go) -- from the app to the service: the set_ingress_dns verb on
// the control pipe (Common/Protocol.h), its tag, its strict request, the
// reply's `ingress_dns_held` and `ingress_dns_busy` and an older service's
// silence, the key the app names its space by and the service looks the space
// up with (SetIngressDnsRequestFor, SpaceKeyOf), and the re-send a busy service
// is owed (Common/IngressDnsSync.h). Run against the same sources the service
// and the app compile; it needs nlohmann/json, like the app.
//
//   c++ -std=c++20 -I ../src/Common -I <dir with nlohmann/json.hpp> ingress-dns-tests.cpp -o /tmp/ingress-dns-tests && /tmp/ingress-dns-tests
//
// With URNW_INGRESS_DNS_TESTS_SDK the key template runs on the generated
// header's own urnet::NetworkSpaceKey, and the values the service imports are
// read through its json: fields only, nothing here calls into the sdk library.
// The header is a system include, like nlohmann/json: the generated code does
// not build with -Wextra -Werror.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "IngressDnsSync.h"
#include "Protocol.h"

#if defined(URNW_INGRESS_DNS_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

using namespace urnw;
using namespace urnw::proto;

namespace {

#if defined(URNW_INGRESS_DNS_TESTS_SDK)
using SpaceKey = urnet::NetworkSpaceKey;
#else
// urnet::NetworkSpaceKey, by the generated wrapper's field names and types.
struct SpaceKey {
  std::optional<std::string> host_name;
  std::optional<std::string> env_name;
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

// Documentation addresses (RFC 5737, RFC 3849), as the sdk's tests use: no
// real resolver appears in the repository.
const std::vector<std::string> kServers = {"https://192.0.2.1/dns-query",
                                           "https://[2001:db8::1]/dns-query"};

SetIngressDns SampleRequest() {
  SetIngressDns request;
  request.host_name = "network.example";
  request.env_name = "main";
  request.ingress_country_code = "cn";
  request.control_doh_urls = kServers;
  return request;
}

// The request the service reads off the wire bytes of `request`, or nullopt
// where its parse refuses them.
std::optional<SetIngressDns> Served(const nlohmann::json& request) {
  try {
    return nlohmann::json::parse(DumpForWire(request)).get<SetIngressDns>();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// (a) the verb and its envelope
void TestTag() {
  Check(std::string(msg::kSetIngressDns) == "set_ingress_dns", "wire: the set_ingress_dns tag");
  const nlohmann::json request = Request(msg::kSetIngressDns, nlohmann::json(SampleRequest()));
  Check(TypeOf(request) == "set_ingress_dns", "wire: the request carries its type");
}

// (b) the request survives the wire whole, an undisclosed region and the
// built-in servers alone included
void TestRequestJson() {
  const std::optional<SetIngressDns> back =
      Served(Request(msg::kSetIngressDns, nlohmann::json(SampleRequest())));
  Check(back && back->host_name == "network.example" && back->env_name == "main" &&
            back->ingress_country_code == "cn" && back->control_doh_urls == kServers,
        "wire: the request round-trips its key, its region and its servers");

  SetIngressDns defaults = SampleRequest();
  defaults.ingress_country_code = "";
  defaults.control_doh_urls = {};
  const std::optional<SetIngressDns> defaultsBack =
      Served(Request(msg::kSetIngressDns, nlohmann::json(defaults)));
  Check(defaultsBack && defaultsBack->ingress_country_code.empty() &&
            defaultsBack->control_doh_urls.empty(),
        "wire: an undisclosed region and no servers round-trip as such");
}

// (c) a request that does not name its space exactly, or leaves out a value,
// is refused, never read with a default: a default key names another space, a
// default region or server list is a choice the user never made
void TestRequestStrict() {
  const nlohmann::json whole = nlohmann::json(SampleRequest());
  for (const char* field : {"host_name", "env_name"}) {
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
        (void)request.get<SetIngressDns>();
      } catch (const std::exception&) {
        threw = true;
      }
      Check(threw, std::string("wire: a request with ") + field + " " + name + " is refused");
    }
  }
  const std::vector<std::pair<std::string, nlohmann::json>> badRegions = {
      {"null", nullptr}, {"a number", 7}, {"an array", nlohmann::json::array()}};
  {
    nlohmann::json missing = whole;
    missing.erase("ingress_country_code");
    bool threw = false;
    try {
      (void)missing.get<SetIngressDns>();
    } catch (const std::exception&) {
      threw = true;
    }
    Check(threw, "wire: a request with ingress_country_code missing is refused");
  }
  for (const auto& [name, value] : badRegions) {
    nlohmann::json changed = whole;
    changed["ingress_country_code"] = value;
    bool threw = false;
    try {
      (void)changed.get<SetIngressDns>();
    } catch (const std::exception&) {
      threw = true;
    }
    Check(threw, "wire: a request with ingress_country_code " + name + " is refused");
  }
  const std::vector<std::pair<std::string, nlohmann::json>> badServers = {
      {"null", nullptr},
      {"a string", "https://192.0.2.1/dns-query"},
      {"an object", nlohmann::json::object()},
      {"a number in the list", nlohmann::json::array({"https://192.0.2.1/dns-query", 7})}};
  {
    nlohmann::json missing = whole;
    missing.erase("control_doh_urls");
    bool threw = false;
    try {
      (void)missing.get<SetIngressDns>();
    } catch (const std::exception&) {
      threw = true;
    }
    Check(threw, "wire: a request with control_doh_urls missing is refused");
  }
  for (const auto& [name, value] : badServers) {
    nlohmann::json changed = whole;
    changed["control_doh_urls"] = value;
    bool threw = false;
    try {
      (void)changed.get<SetIngressDns>();
    } catch (const std::exception&) {
      threw = true;
    }
    Check(threw, "wire: a request with control_doh_urls " + name + " is refused");
  }
}

// (d) the reply's `ingress_dns_held` and `ingress_dns_busy`, and what a
// service that held no such space, or does not know the verb, answers
void TestReply() {
  Reply reply;
  reply.ok = true;
  reply.in_reply_to = msg::kSetIngressDns;
  reply.ingress_dns_held = true;
  const Reply back = nlohmann::json::parse(DumpForWire(nlohmann::json(reply))).get<Reply>();
  Check(back.ok && back.ingress_dns_held && back.in_reply_to == "set_ingress_dns",
        "reply: ingress_dns_held round-trips");

  Reply notHeld;
  notHeld.ok = true;
  notHeld.in_reply_to = msg::kSetIngressDns;
  const nlohmann::json notHeldJson = nlohmann::json(notHeld);
  Check(!notHeldJson.contains("ingress_dns_held"),
        "reply: a reply that wrote nothing carries no ingress_dns_held");
  Check(notHeldJson.get<Reply>().ok && !notHeldJson.get<Reply>().ingress_dns_held,
        "reply: a service that held no such space answers ok, nothing held");

  const Reply older = nlohmann::json::parse(
                          R"({"type":"reply","ok":false,"error":"unknown request type: set_ingress_dns","in_reply_to":"set_ingress_dns"})")
                          .get<Reply>();
  Check(!older.ok && !older.ingress_dns_held && !older.ingress_dns_busy,
        "reply: a service too old for the verb wrote nothing and is not busy");

  Reply busy;
  busy.ok = false;
  busy.in_reply_to = msg::kSetIngressDns;
  busy.error = "a tunnel operation is in progress";
  busy.ingress_dns_busy = true;
  const Reply busyBack = nlohmann::json::parse(DumpForWire(nlohmann::json(busy))).get<Reply>();
  Check(!busyBack.ok && busyBack.ingress_dns_busy && !busyBack.ingress_dns_held,
        "reply: ingress_dns_busy round-trips");

  Reply refused;
  refused.ok = false;
  refused.error = "the ingress dns could not be applied";
  const nlohmann::json refusedJson = nlohmann::json(refused);
  Check(!refusedJson.contains("ingress_dns_busy") && !refusedJson.get<Reply>().ingress_dns_busy,
        "reply: a refusal that is not busy carries no ingress_dns_busy");
  // the extender reset's fields stay its own
  Check(!busyBack.reset && !busyBack.reset_busy,
        "reply: the verb's busy is not the extender reset's");
  Check(kProtocolVersion == 4, "wire: no protocol bump for the verb and its fields");
}

SpaceKey Key(std::optional<std::string> hostName, std::optional<std::string> envName) {
  SpaceKey key;
  key.host_name = std::move(hostName);
  key.env_name = std::move(envName);
  return key;
}

// (e) the app names the space whose values it hands over, and the service
// looks up the same key; a key that is missing sends nothing, since the
// service would refuse it, and the next import carries the values instead
void TestKeys() {
  const std::optional<SetIngressDns> request = SetIngressDnsRequestFor(
      std::optional<SpaceKey>(Key("network.example", "main")), "cn", kServers);
  Check(request && request->host_name == "network.example" && request->env_name == "main" &&
            request->ingress_country_code == "cn" && request->control_doh_urls == kServers,
        "keys: the request names the app's space, its region and its servers");
  // what the service parses off the pipe and looks the space up by
  const std::optional<SetIngressDns> served =
      request ? Served(Request(msg::kSetIngressDns, nlohmann::json(*request))) : std::nullopt;
  const std::optional<SpaceKey> looked =
      served ? std::optional<SpaceKey>(SpaceKeyOf<SpaceKey>(*served)) : std::nullopt;
  Check(looked && looked->host_name == std::optional<std::string>("network.example") &&
            looked->env_name == std::optional<std::string>("main"),
        "keys: the service looks up the key the app's space has");

  const std::optional<SetIngressDns> undisclosed = SetIngressDnsRequestFor(
      std::optional<SpaceKey>(Key("network.example", "main")), "", {});
  Check(undisclosed && undisclosed->ingress_country_code.empty() &&
            undisclosed->control_doh_urls.empty(),
        "keys: an undisclosed region and no servers are sent, not skipped");

  Check(!SetIngressDnsRequestFor(std::optional<SpaceKey>(), "cn", kServers),
        "keys: a space with no key sends nothing");
  Check(!SetIngressDnsRequestFor(std::optional<SpaceKey>(Key(std::nullopt, "main")), "cn",
                                 kServers),
        "keys: a key with no host sends nothing");
  Check(!SetIngressDnsRequestFor(std::optional<SpaceKey>(Key("network.example", std::nullopt)),
                                 "cn", kServers),
        "keys: a key with no env sends nothing");
  Check(!SetIngressDnsRequestFor(std::optional<SpaceKey>(Key("", "main")), "cn", kServers),
        "keys: an empty host sends nothing");
  Check(!SetIngressDnsRequestFor(std::optional<SpaceKey>(Key("network.example", "")), "cn",
                                 kServers),
        "keys: an empty env sends nothing");

  SetIngressDns other = SampleRequest();
  other.host_name = "other.example";
  other.env_name = "beta";
  const SpaceKey otherKey = SpaceKeyOf<SpaceKey>(other);
  Check(otherKey.host_name == std::optional<std::string>("other.example") &&
            otherKey.env_name == std::optional<std::string>("beta"),
        "keys: the host and the env each go to their own field");
}

// (f) the re-send a busy service is owed: owed on a busy answer, due at the
// first pushed status that ends the operation holding the service's lock, once
// per refusal, and replaced by newer values
void TestOwedResend() {
  using ingressdns::Owed;
  using ingressdns::ServiceAnswer;
  const SetIngressDns request = SampleRequest();
  {
    Owed owed;
    Check(!owed.TakeDue(TunnelState::Stopped), "owed: nothing is due before a refusal");
    owed.Answered(request, ServiceAnswer::Busy);
    Check(owed.IsOwed(), "owed: a busy refusal is owed");
    Check(!owed.TakeDue(TunnelState::Starting),
          "owed: a transition's status does not end the operation");
    Check(!owed.TakeDue(TunnelState::Stopping),
          "owed: a teardown's status does not end the operation");
    Check(owed.IsOwed(), "owed: still owed after a transition's status");
    // the stall the prompt is raised over: a session preparing its capture
    const std::optional<SetIngressDns> due = owed.TakeDue(TunnelState::Preparing);
    Check(due && due->host_name == request.host_name && due->env_name == request.env_name &&
              due->ingress_country_code == request.ingress_country_code &&
              due->control_doh_urls == request.control_doh_urls,
          "owed: due at the status that ends it, the same key and the same values");
    Check(!owed.IsOwed() && !owed.TakeDue(TunnelState::Up), "owed: due once");
    // the re-send refused as busy again is owed again
    owed.Answered(*due, ServiceAnswer::Busy);
    Check(owed.IsOwed(), "owed: a re-send refused as busy is owed again");
  }
  for (const ServiceAnswer answer : {ServiceAnswer::Taken, ServiceAnswer::NotTaken}) {
    Owed owed;
    owed.Answered(request, answer);
    Check(!owed.IsOwed() && !owed.TakeDue(TunnelState::Stopped),
          std::string("owed: values answered ") + ingressdns::ToString(answer) + " owe nothing");
  }
  {
    Owed owed;
    owed.Answered(request, ServiceAnswer::Busy);
    SetIngressDns newer = request;
    newer.ingress_country_code = "";
    newer.control_doh_urls = {};
    owed.Answered(newer, ServiceAnswer::Busy);
    const std::optional<SetIngressDns> due = owed.TakeDue(TunnelState::Up);
    Check(due && due->ingress_country_code.empty() && due->control_doh_urls.empty(),
          "owed: newer values refused as busy replace the owed ones");
    owed.Answered(request, ServiceAnswer::Busy);
    owed.Answered(newer, ServiceAnswer::Taken);
    Check(!owed.IsOwed(), "owed: newer values that were taken owe nothing");
  }
}

#if defined(URNW_INGRESS_DNS_TESTS_SDK)
// (g) the values travel in the space's values too, by the generated header's
// own json: what start_tunnel and start_provider hand the service, so a
// service that did not take the verb takes them at its next import
void TestValuesCarryTheIngressDns() {
  urnet::NetworkSpaceValues values;
  values.ingress_country_code = "cn";
  values.control_doh_urls_ipv4 = std::vector<std::string>{"https://192.0.2.1/dns-query"};
  const nlohmann::json valuesJson = values;
  Check(valuesJson.value("ingress_country_code", std::string()) == "cn",
        "values: the region is written as ingress_country_code");
  const urnet::NetworkSpaceValues back = valuesJson.get<urnet::NetworkSpaceValues>();
  Check(back.ingress_country_code == std::optional<std::string>("cn") &&
            back.control_doh_urls_ipv4 ==
                std::optional<std::vector<std::string>>(
                    std::vector<std::string>{"https://192.0.2.1/dns-query"}),
        "values: the region and the servers read back");
}
#endif

}  // namespace

int main() {
  TestTag();
  TestRequestJson();
  TestRequestStrict();
  TestReply();
  TestKeys();
  TestOwedResend();
#if defined(URNW_INGRESS_DNS_TESTS_SDK)
  TestValuesCarryTheIngressDns();
#endif
  std::cout << (gCases - gFailures) << "/" << gCases << " ingress dns checks passed"
#if defined(URNW_INGRESS_DNS_TESTS_SDK)
            << " (against urnetwork_sdk.hpp)"
#endif
            << std::endl;
  return gFailures == 0 ? 0 : 1;
}
