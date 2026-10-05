// Executable spec for the VLESS settings sheet's pure logic
// (App/VlessPresentation.h): the new form, which fields show for each
// transport and security, the form <-> VlessSettings mapping (spiderX carried,
// a hidden flow cleared), the store key of every sdk error id, the picker
// options, and the carry-over that keeps a space's VLESS server when SdkHost
// writes the space's values whole (BuildNetworkSpace, ApplyNetworkServer).
// Run against the SAME sources the app compiles, on any host with a C++20
// compiler. The sheet itself (VlessSheet.cpp) cannot be built off Windows;
// what is verified here is every decision it makes before it touches a XAML
// object.
//
// By default the templates are instantiated with stand-ins that carry the
// fields of urnet::VlessSettings, urnet::NetworkSpaceKey and
// urnet::NetworkSpaceValues under the generated wrapper's names and types, so
// the spec needs neither the SDK nor a json library:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App \
//       vless-presentation-tests.cpp ../src/App/VlessPresentation.cpp \
//       -o /tmp/vless-presentation-tests && /tmp/vless-presentation-tests ..
//
// With URNW_VLESS_TESTS_SDK it is built against the generated header itself,
// so the mapping and the carry-over compile against the SDK's own types and
// the carry-over reads a space export through the header's json conversions
// (the header needs nlohmann/json; it is a system include because the
// generated code does not build with -Wextra -Werror):
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_VLESS_TESTS_SDK -I ../src/App \
//       -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> \
//       vless-presentation-tests.cpp ../src/App/VlessPresentation.cpp ...
//
// The argument is the app directory (default ".."): the label keys are checked
// against the generated English strings.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "VlessPresentation.h"

#if defined(URNW_VLESS_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

namespace vless = urnw::vless;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Case(const std::string& name) {
  ++gCases;
  gCurrentCase = name;
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Fail("cannot read " + path);
    return {};
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool HasString(const std::string& resw, const std::string& key) {
  return resw.find("<data name=\"" + key + "\"") != std::string::npos;
}

#if defined(URNW_VLESS_TESTS_SDK)
using Settings = urnet::VlessSettings;
using SpaceKey = urnet::NetworkSpaceKey;
using SpaceValues = urnet::NetworkSpaceValues;
#else
// The fields of urnet::VlessSettings, by the generated wrapper's names and
// types (every field optional, the port an int64_t).
struct Settings {
  std::optional<bool> enabled;
  std::optional<std::string> name;
  std::optional<std::string> address;
  std::optional<std::int64_t> port;
  std::optional<std::string> id;
  std::optional<std::string> flow;
  std::optional<std::string> network;
  std::optional<std::string> security;
  std::optional<std::string> server_name;
  std::optional<std::string> fingerprint;
  std::optional<std::string> alpn;
  std::optional<bool> allow_insecure;
  std::optional<std::string> public_key;
  std::optional<std::string> short_id;
  std::optional<std::string> spider_x;
  std::optional<std::string> path;
  std::optional<std::string> host;
};

// As much of urnet::NetworkSpaceKey and urnet::NetworkSpaceValues as the
// carry-over touches, plus two values SdkHost writes, to show they are left
// alone.
struct SpaceKey {
  std::optional<std::string> host_name;
  std::optional<std::string> env_name;
};
struct SpaceValues {
  std::optional<bool> bundled;
  std::optional<std::string> api_url;
  std::optional<Settings> vless;
};
#endif

// Field by field, so the same comparison serves the stand-in and the SDK's
// struct (which has no operator==), and a field renamed in the SDK fails to
// compile here.
bool SameSettings(const Settings& a, const Settings& b) {
  return a.enabled == b.enabled && a.name == b.name && a.address == b.address &&
         a.port == b.port && a.id == b.id && a.flow == b.flow && a.network == b.network &&
         a.security == b.security && a.server_name == b.server_name &&
         a.fingerprint == b.fingerprint && a.alpn == b.alpn &&
         a.allow_insecure == b.allow_insecure && a.public_key == b.public_key &&
         a.short_id == b.short_id && a.spider_x == b.spider_x && a.path == b.path &&
         a.host == b.host;
}

bool SameSettings(const std::optional<Settings>& a, const std::optional<Settings>& b) {
  if (!a || !b) return !a && !b;
  return SameSettings(*a, *b);
}

// A shared REALITY link, as ParseVlessLink hands it back (enabled), spiderX and all.
Settings RealitySettings() {
  Settings settings;
  settings.enabled = true;
  settings.name = "home";
  settings.address = "vless.example.com";
  settings.port = 443;
  settings.id = "b831381d-6324-4d53-ad4f-8cda48b30811";
  settings.flow = vless::kFlowVision;
  settings.network = vless::kNetworkTcp;
  settings.security = vless::kSecurityReality;
  settings.server_name = "www.example.com";
  settings.fingerprint = "chrome";
  settings.public_key = "Z84J2IelR9ch3k8VtlVhhs5ycBUlXA7wHBWcBrjqnAw";
  settings.short_id = "6ba85179e30d4fc2";
  settings.spider_x = "/";
  return settings;
}

// A tls server behind a WebSocket CDN, with the tls-only fields set.
Settings TlsWebSocketSettings() {
  Settings settings;
  settings.enabled = true;
  settings.name = "cdn";
  settings.address = "203.0.113.7";
  settings.port = 8443;
  settings.id = "b831381d-6324-4d53-ad4f-8cda48b30811";
  settings.network = vless::kNetworkWs;
  settings.security = vless::kSecurityTls;
  settings.server_name = "cdn.example.com";
  settings.fingerprint = "firefox";
  settings.alpn = "h2,http/1.1";
  settings.allow_insecure = true;
  settings.path = "/ws?ed=2048";
  settings.host = "cdn.example.com";
  return settings;
}

std::string Describe(const vless::FieldVisibility& shown) {
  std::ostringstream out;
  out << "flow=" << shown.flow << " sni/fp=" << shown.serverNameAndFingerprint
      << " alpn/insecure=" << shown.alpnAndAllowInsecure
      << " pbk/sid=" << shown.publicKeyAndShortId << " path/host=" << shown.pathAndHost;
  return out.str();
}

void CheckOptionLabels(const std::string& resw, const std::vector<vless::Option>& options,
                       const std::string& what) {
  for (const vless::Option& option : options) {
    if (!option.labelKey.empty() && !HasString(resw, option.labelKey)) {
      Fail(what + " label " + option.labelKey + " is not in en/Resources.resw");
    }
  }
}

void CheckOptionValues(const std::vector<vless::Option>& options,
                       const std::vector<std::string>& values, const std::string& what) {
  std::vector<std::string> got;
  for (const vless::Option& option : options) got.push_back(option.value);
  Check(got == values, what + " options are not the sdk's, in the sdk's order");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string appDir = 1 < argc ? argv[1] : "..";
  const std::string resw = ReadFile(appDir + "/src/App/Strings/en/Resources.resw");

  // ---- the form -------------------------------------------------------------

  Case("the new form is the sdk's NewVlessSettings");
  {
    const vless::Form form = vless::NewForm();
    Check(!form.enabled, "a new form is enabled");
    Check(form.port == "443", "a new form's port is " + form.port);
    Check(form.network == "tcp", "a new form's transport is " + form.network);
    Check(form.security == "reality", "a new form's security is " + form.security);
    Check(form.flow == "xtls-rprx-vision", "a new form's flow is " + form.flow);
    Check(form.fingerprint == "chrome", "a new form's fingerprint is " + form.fingerprint);
    Check(form.name.empty() && form.address.empty() && form.id.empty() &&
              form.serverName.empty() && form.alpn.empty() && !form.allowInsecure &&
              form.publicKey.empty() && form.shortId.empty() && form.path.empty() &&
              form.host.empty() && form.spiderX.empty(),
          "a new form names a server");
  }

  Case("no settings read as the new form");
  Check(vless::FormFrom(std::optional<Settings>{}) == vless::NewForm(),
        "a read that failed does not show the new form");

  Case("the sdk's defaults for a space with no server read as the new form");
  {
    // GetVlessSettings hands back NewVlessSettings when nothing is stored
    Settings defaults;
    defaults.port = 443;
    defaults.flow = vless::kFlowVision;
    defaults.network = vless::kNetworkTcp;
    defaults.security = vless::kSecurityReality;
    defaults.fingerprint = "chrome";
    Check(vless::FormFrom(std::optional<Settings>{defaults}) == vless::NewForm(),
          "the sdk's new-form defaults do not fill the new form");
  }

  Case("unset fields read as the sdk reads them");
  {
    const vless::Form form = vless::FormFrom(std::optional<Settings>{Settings{}});
    Check(form == vless::Form{}, "unset settings do not read as an empty form");
    Check(form.network == "tcp" && form.security == "none" && form.flow.empty() &&
              form.port.empty() && form.fingerprint.empty() && !form.enabled,
          "an unset transport is not tcp, or an unset security not none");
  }

  Case("picker tokens read normalized, as the sdk normalizes them");
  {
    Settings settings;
    settings.network = " WS ";
    settings.security = "TLS";
    settings.flow = "";
    settings.fingerprint = "Chrome";
    const vless::Form form = vless::FormFrom(std::optional<Settings>{settings});
    Check(form.network == "ws" && form.security == "tls" && form.fingerprint == "chrome",
          "tokens are not trimmed and lower cased");
  }

  // ---- which fields show -----------------------------------------------------

  Case("the fields each transport and security show");
  {
    struct Row {
      const char* network;
      const char* security;
      vless::FieldVisibility want;
    };
    // flow, sni+fingerprint, alpn+insecure, public key+short id, path+host
    const Row rows[] = {
        {"tcp", "none", {false, false, false, false, false}},
        {"tcp", "tls", {true, true, true, false, false}},
        {"tcp", "reality", {true, true, false, true, false}},
        {"ws", "none", {false, false, false, false, true}},
        {"ws", "tls", {false, true, true, false, true}},
        {"ws", "reality", {false, true, false, true, true}},
        {"httpupgrade", "none", {false, false, false, false, true}},
        {"httpupgrade", "tls", {false, true, true, false, true}},
        {"httpupgrade", "reality", {false, true, false, true, true}},
        // the sdk's readings: empty is tcp and none, any case, trimmed
        {"", "", {false, false, false, false, false}},
        {"", "reality", {true, true, false, true, false}},
        {" TCP ", "Reality", {true, true, false, true, false}},
        {"HTTPUpgrade", "TLS", {false, true, true, false, true}},
        // a token this build does not offer shows nothing it cannot judge
        {"grpc", "xtls", {false, true, false, false, false}},
    };
    for (const Row& row : rows) {
      const vless::FieldVisibility got = vless::VisibilityFor(row.network, row.security);
      if (got != row.want) {
        Fail(std::string("'") + row.network + "' + '" + row.security + "': got " +
             Describe(got) + ", want " + Describe(row.want));
      }
    }
  }

  // ---- the mapping -------------------------------------------------------------

  Case("a REALITY server round-trips through the form unchanged, spiderX included");
  {
    const Settings settings = RealitySettings();
    const auto result = vless::SettingsFrom<Settings>(
        vless::FormFrom(std::optional<Settings>{settings}));
    Check(result.errorKey.empty(), "the round trip failed: " + result.errorKey);
    Check(SameSettings(result.settings, settings), "the REALITY settings changed in the round trip");
    Check(result.settings.spider_x == std::optional<std::string>{"/"}, "spiderX was dropped");
  }

  Case("a tls WebSocket server round-trips through the form unchanged");
  {
    const Settings settings = TlsWebSocketSettings();
    const auto result = vless::SettingsFrom<Settings>(
        vless::FormFrom(std::optional<Settings>{settings}));
    Check(result.errorKey.empty(), "the round trip failed: " + result.errorKey);
    Check(SameSettings(result.settings, settings), "the tls ws settings changed in the round trip");
  }

  Case("the sdk's defaults round-trip, so a form saved untouched writes nothing new");
  {
    const auto result = vless::SettingsFrom<Settings>(vless::NewForm());
    Settings defaults;
    defaults.port = 443;
    defaults.flow = vless::kFlowVision;
    defaults.network = vless::kNetworkTcp;
    defaults.security = vless::kSecurityReality;
    defaults.fingerprint = "chrome";
    Check(SameSettings(result.settings, defaults), "the new form does not save as NewVlessSettings");
  }

  Case("spiderX is carried through an edit it is not part of");
  {
    vless::Form form = vless::FormFrom(std::optional<Settings>{RealitySettings()});
    form.name = "office";
    form.serverName = "www.example.org";
    form.enabled = false;
    const auto result = vless::SettingsFrom<Settings>(form);
    Check(result.settings.spider_x == std::optional<std::string>{"/"},
          "spiderX did not survive an edit");
    Check(result.settings.name == std::optional<std::string>{"office"} &&
              result.settings.server_name == std::optional<std::string>{"www.example.org"},
          "the edited fields were not written");
    Check(!result.settings.enabled, "a form switched off saves enabled");
  }

  Case("a hidden flow saves as no flow");
  {
    vless::Form form = vless::FormFrom(std::optional<Settings>{RealitySettings()});
    form.network = vless::kNetworkWs;  // vision cannot ride a WebSocket
    Check(!vless::SettingsFrom<Settings>(form).settings.flow,
          "a flow hidden by the transport was saved");
    form.network = vless::kNetworkHttpUpgrade;
    Check(!vless::SettingsFrom<Settings>(form).settings.flow,
          "a flow hidden by httpupgrade was saved");
    form.network = vless::kNetworkTcp;
    form.security = vless::kSecurityNone;  // nor a stream without tls
    Check(!vless::SettingsFrom<Settings>(form).settings.flow,
          "a flow hidden by security none was saved");
    form.security = vless::kSecurityTls;
    Check(vless::SettingsFrom<Settings>(form).settings.flow ==
              std::optional<std::string>{vless::kFlowVision},
          "a shown flow was not saved");
  }

  Case("the other hidden fields keep what was typed");
  {
    // REALITY's key and short id stay through a look at tls and back, and a
    // path typed for ws stays when the transport goes back to tcp
    vless::Form form = vless::FormFrom(std::optional<Settings>{RealitySettings()});
    form.security = vless::kSecurityTls;
    form.path = "/ws";
    const auto result = vless::SettingsFrom<Settings>(form);
    Check(result.settings.public_key == RealitySettings().public_key &&
              result.settings.short_id == RealitySettings().short_id,
          "REALITY's key and short id were dropped while hidden");
    Check(result.settings.path == std::optional<std::string>{"/ws"},
          "a path was dropped while hidden");
  }

  Case("empty and false fields are left unset, as the sdk hands them back");
  {
    vless::Form form;
    form.port = "";
    const auto result = vless::SettingsFrom<Settings>(form);
    Check(!result.settings.enabled && !result.settings.name && !result.settings.address &&
              !result.settings.port && !result.settings.id && !result.settings.flow &&
              !result.settings.server_name && !result.settings.fingerprint &&
              !result.settings.alpn && !result.settings.allow_insecure &&
              !result.settings.public_key && !result.settings.short_id &&
              !result.settings.spider_x && !result.settings.path && !result.settings.host,
          "an empty form set a field");
    Check(result.settings.network == std::optional<std::string>{"tcp"} &&
              result.settings.security == std::optional<std::string>{"none"},
          "an empty form did not save tcp and none");
  }

  // ---- the port --------------------------------------------------------------

  Case("the port box");
  {
    Check(vless::ParsePort("443") == std::optional<std::int64_t>{443}, "443 did not parse");
    Check(vless::ParsePort("") == std::optional<std::int64_t>{0}, "an empty box is not no port");
    Check(vless::ParsePort(" 8443 ") == std::optional<std::int64_t>{8443}, "whitespace was not ignored");
    Check(vless::ParsePort("65536") == std::optional<std::int64_t>{65536},
          "a number out of range was refused here; the sdk refuses it");
    Check(!vless::ParsePort("abc"), "letters parsed as a port");
    Check(!vless::ParsePort("-1"), "a sign parsed as a port");
    Check(!vless::ParsePort("4 43"), "inner whitespace parsed as a port");
    Check(!vless::ParsePort("123456"), "six digits parsed as a port");
    Check(vless::PortText(443) == "443", "443 did not print");
    Check(vless::PortText(0).empty() && vless::PortText(-5).empty(), "no port printed a number");
    Check(vless::IsPortInput("") && vless::IsPortInput("0") && vless::IsPortInput("65535"),
          "the box refused digits");
    Check(!vless::IsPortInput("123456") && !vless::IsPortInput("12a") &&
              !vless::IsPortInput(" 1") && !vless::IsPortInput("-1"),
          "the box took something other than five digits");

    vless::Form form = vless::NewForm();
    form.port = "4x3";
    const auto result = vless::SettingsFrom<Settings>(form);
    Check(result.errorKey == "vless_error_port_invalid",
          "a port box that is not a number gave '" + result.errorKey + "'");
  }

  // ---- paste link ----------------------------------------------------------------

  Case("Paste link parses the clipboard's text, else the link box");
  {
    const std::string typed = "vless://typed@example.com:443";
    Check(vless::LinkToParse(std::optional<std::string>{"  vless://pasted@example.com:443\r\n"},
                             typed) == "vless://pasted@example.com:443",
          "the clipboard's text was not taken, trimmed");
    Check(vless::LinkToParse(std::optional<std::string>{}, typed) == typed,
          "no text on the clipboard did not fall back to the box");
    Check(vless::LinkToParse(std::optional<std::string>{" \n\t"}, typed) == typed,
          "a blank clipboard replaced the box");
    Check(vless::LinkToParse(std::optional<std::string>{}, "").empty(),
          "nothing anywhere was not handed to the sdk as nothing (an invalid link)");
  }

  // ---- errors -----------------------------------------------------------------

  Case("every sdk error id is its own store key, and the key exists");
  {
    const std::vector<std::string> ids = {
        "vless_error_link_invalid",          "vless_error_link_unsupported",
        "vless_error_address_invalid",       "vless_error_port_invalid",
        "vless_error_id_invalid",            "vless_error_network_unsupported",
        "vless_error_security_unsupported",  "vless_error_flow_invalid",
        "vless_error_server_name_required",  "vless_error_fingerprint_unsupported",
        "vless_error_public_key_invalid",    "vless_error_short_id_invalid",
    };
    Check(std::size(vless::kErrorIds) == ids.size(), "the sdk has 12 error ids");
    for (const std::string& id : ids) {
      Check(std::string(vless::ErrorKey(id)) == id, id + " does not map to itself");
      Check(HasString(resw, id), id + " is not in en/Resources.resw");
    }
#if defined(URNW_VLESS_TESTS_SDK)
    // and they are the sdk's own constants
    const std::vector<std::string> sdkIds = {
        urnet::VlessErrorLinkInvalid,          urnet::VlessErrorLinkUnsupported,
        urnet::VlessErrorAddressInvalid,       urnet::VlessErrorPortInvalid,
        urnet::VlessErrorIdInvalid,            urnet::VlessErrorNetworkUnsupported,
        urnet::VlessErrorSecurityUnsupported,  urnet::VlessErrorFlowInvalid,
        urnet::VlessErrorServerNameRequired,   urnet::VlessErrorFingerprintUnsupported,
        urnet::VlessErrorPublicKeyInvalid,     urnet::VlessErrorShortIdInvalid,
    };
    Check(sdkIds == ids, "the error ids are not the sdk's urnet::VlessError* constants");
#endif
  }

  Case("an id this build does not know, the C ABI's internal one included, reads as something went wrong");
  {
    // the C ABI answers it for a call that could not run, which is no refusal
    // of the settings: a save that never ran must not read as an invalid link
    Check(std::string(vless::kErrorInternal) == "internal_error",
          "the C ABI's internal id is not internal_error");
    Check(std::string(vless::ErrorKey(vless::kErrorInternal)) == "something_went_wrong",
          "the C ABI's internal id is not shown as something went wrong");
    Check(std::string(vless::ErrorKey("vless_error_from_a_newer_sdk")) == "something_went_wrong",
          "an unknown id is not shown as something went wrong");
    Check(std::string(vless::ErrorKey("")) == "something_went_wrong",
          "an empty id is not shown as something went wrong");
    Check(std::string(vless::ErrorKey("VLESS_ERROR_PORT_INVALID")) == "something_went_wrong",
          "ids are matched loosely");
    Check(HasString(resw, "something_went_wrong"), "something_went_wrong is not in en/Resources.resw");
#if defined(URNW_VLESS_TESTS_SDK) && defined(URNET_ERROR_ID_INTERNAL)
    Check(std::string(URNET_ERROR_ID_INTERNAL) == vless::kErrorInternal,
          "the internal id is not the sdk's URNET_ERROR_ID_INTERNAL");
#endif
  }

  // ---- the pickers -------------------------------------------------------------

  Case("the pickers offer the sdk's options, in its order, with stored labels");
  {
    CheckOptionValues(vless::NetworkOptions(), {"tcp", "ws", "httpupgrade"}, "transport");
    CheckOptionValues(vless::SecurityOptions(), {"none", "tls", "reality"}, "security");
    CheckOptionValues(vless::FlowOptions(), {"", "xtls-rprx-vision"}, "flow");
    CheckOptionValues(vless::FingerprintOptions(),
                      {"", "chrome", "firefox", "safari", "ios", "android", "edge", "360", "qq",
                       "random", "randomized"},
                      "fingerprint");
    Check(vless::NetworkOptions()[2].labelKey == "vless_network_httpupgrade" &&
              vless::SecurityOptions()[0].labelKey == "none" &&
              vless::SecurityOptions()[2].labelKey == "vless_security_reality" &&
              vless::FlowOptions()[0].labelKey == "none" &&
              vless::FlowOptions()[1].labelKey == "vless_flow_vision",
          "an option carries the wrong label");
    // the empty fingerprint is None, the rest show their own names
    const auto fingerprints = vless::FingerprintOptions();
    Check(fingerprints[0].labelKey == "none", "the empty fingerprint is not None");
    for (std::size_t i = 1; i < fingerprints.size(); ++i) {
      Check(fingerprints[i].labelKey.empty(), fingerprints[i].value + " has a store label");
    }
    CheckOptionLabels(resw, vless::NetworkOptions(), "transport");
    CheckOptionLabels(resw, vless::SecurityOptions(), "security");
    CheckOptionLabels(resw, vless::FlowOptions(), "flow");
    CheckOptionLabels(resw, vless::FingerprintOptions(), "fingerprint");
  }

  Case("a stored value the pickers do not offer still shows, and saves back");
  {
    const auto withUnknown = vless::OptionsWith(vless::FingerprintOptions(), "chrome_120");
    Check(withUnknown.size() == vless::FingerprintOptions().size() + 1 &&
              withUnknown.back() == vless::Option{"chrome_120", ""},
          "an unknown fingerprint was not appended as itself");
    Check(vless::OptionsWith(vless::FingerprintOptions(), "safari") ==
              vless::FingerprintOptions(),
          "a known value was appended twice");
    Check(vless::OptionsWith(vless::FlowOptions(), "") == vless::FlowOptions(),
          "the empty flow was appended though None offers it");
    const int at = vless::IndexOf(withUnknown, "chrome_120");
    Check(at == static_cast<int>(withUnknown.size()) - 1 &&
              vless::ValueAt(withUnknown, at) == "chrome_120",
          "the appended value does not select and read back");
    Check(vless::IndexOf(vless::NetworkOptions(), "grpc") == -1, "an absent value has an index");
    Check(vless::ValueAt(vless::NetworkOptions(), -1).empty() &&
              vless::ValueAt(vless::NetworkOptions(), 3).empty(),
          "no selection does not read as the sdk's unset field");
  }

  // ---- keeping the server across a write of the whole space -------------------

  Case("the same space key, an unset field as the empty one");
  {
    SpaceKey a;
    a.host_name = "bringyour.com";
    a.env_name = "main";
    SpaceKey b = a;
    Check(vless::SameSpaceKey(a, b), "a key is not its own space");
    b.host_name = "example.com";
    Check(!vless::SameSpaceKey(a, b), "another host is the same space");
    b = a;
    b.env_name = "beta";
    Check(!vless::SameSpaceKey(a, b), "another env is the same space");
    SpaceKey unset;
    SpaceKey empty;
    empty.host_name = "";
    empty.env_name = "";
    Check(vless::SameSpaceKey(unset, empty), "an unset key field is not the empty one");
  }

  Case("fresh values keep the stored space's VLESS server");
  {
    // what ApplyNetworkServer and BuildNetworkSpace write: everything but vless
    SpaceValues fresh;
    fresh.bundled = false;
    fresh.api_url = "https://api.example.com";
    SpaceValues stored;
    stored.bundled = true;
    stored.api_url = "https://old.example.com";
    stored.vless = RealitySettings();
    const SpaceValues written = vless::WithStoredVless(fresh, std::optional<SpaceValues>{stored});
    Check(SameSettings(written.vless, stored.vless), "the stored VLESS server was dropped");
    Check(written.api_url == fresh.api_url && written.bundled == fresh.bundled,
          "the carry-over touched a value the writer owns");
  }

  Case("a server that is off is carried too, so the space keeps what was typed");
  {
    SpaceValues stored;
    Settings off = TlsWebSocketSettings();
    off.enabled = std::nullopt;
    stored.vless = off;
    const SpaceValues written =
        vless::WithStoredVless(SpaceValues{}, std::optional<SpaceValues>{stored});
    Check(SameSettings(written.vless, stored.vless), "a server that is off was dropped");
  }

  Case("no stored values, or none with a server, carry nothing");
  {
    Check(!vless::WithStoredVless(SpaceValues{}, std::optional<SpaceValues>{}).vless,
          "a space never written gained a server");
    Check(!vless::WithStoredVless(SpaceValues{}, std::optional<SpaceValues>{SpaceValues{}}).vless,
          "a space with no server gained one");
  }

  Case("values that name a server keep their own");
  {
    SpaceValues fresh;
    fresh.vless = TlsWebSocketSettings();
    SpaceValues stored;
    stored.vless = RealitySettings();
    const SpaceValues written = vless::WithStoredVless(fresh, std::optional<SpaceValues>{stored});
    Check(SameSettings(written.vless, fresh.vless), "the stored server replaced an explicit one");
  }

#if defined(URNW_VLESS_TESTS_SDK)
  // ---- against the SDK header: the json the C ABI and toJson carry -----------

  Case("settings built from the form serialize as the sdk marshals them");
  {
    // sdk NewVlessSettings, as Go's json.Marshal writes it (omitempty)
    const nlohmann::json expected = nlohmann::json::parse(
        R"({"port":443,"flow":"xtls-rprx-vision","network":"tcp","security":"reality","fingerprint":"chrome"})");
    const nlohmann::json got = vless::SettingsFrom<Settings>(vless::NewForm()).settings;
    Check(got == expected, "the new form serializes as " + got.dump());

    // and a REALITY server read back through the header round-trips the form
    const nlohmann::json reality = RealitySettings();
    const Settings parsed = reality.get<Settings>();
    Check(SameSettings(parsed, RealitySettings()), "the header did not round-trip the settings");
    Check(vless::FormFrom(std::optional<Settings>{parsed}) ==
              vless::FormFrom(std::optional<Settings>{RealitySettings()}),
          "the form differs after the header's json");
  }

  Case("a parsed link reads into the form as the sheet reads it");
  {
    // urnet_parse_vless_link's json: sdk VlessLinkResult has no json tags
    const nlohmann::json parsed = nlohmann::json::parse(R"({
      "Settings": {"enabled": true, "name": "home", "address": "vless.example.com",
                   "port": 443, "id": "b831381d-6324-4d53-ad4f-8cda48b30811",
                   "flow": "xtls-rprx-vision", "network": "tcp", "security": "reality",
                   "server_name": "www.example.com", "fingerprint": "chrome",
                   "public_key": "Z84J2IelR9ch3k8VtlVhhs5ycBUlXA7wHBWcBrjqnAw",
                   "short_id": "6ba85179e30d4fc2", "spider_x": "/"},
      "Error": ""})");
    const urnet::VlessLinkResult result = parsed.get<urnet::VlessLinkResult>();
    Check(result.Error.empty() && result.Settings, "the link result did not read");
    const vless::Form form = vless::FormFrom(result.Settings);
    Check(form.enabled && form.spiderX == "/" &&
              form == vless::FormFrom(std::optional<Settings>{RealitySettings()}),
          "a parsed link does not fill the form, switched on");

    const urnet::VlessLinkResult refused = nlohmann::json::parse(
        R"({"Error": "vless_error_link_unsupported"})").get<urnet::VlessLinkResult>();
    Check(!refused.Settings &&
              std::string(vless::ErrorKey(refused.Error)) == "vless_error_link_unsupported",
          "a refused link does not show its error");
  }

  Case("the stored server is read out of a space export and carried into fresh values");
  {
    // NetworkSpace::toJson: {"key": ..., "values": ...} (sdk ExportNetworkSpace)
    const nlohmann::json document = nlohmann::json::parse(R"({
      "key": {"host_name": "bringyour.com", "env_name": "main"},
      "values": {
        "bundled": true,
        "net_expose_server_ips": true,
        "link_host_name": "ur.io",
        "wallet": "circle",
        "vless": {
          "enabled": true,
          "name": "home",
          "address": "vless.example.com",
          "port": 443,
          "id": "b831381d-6324-4d53-ad4f-8cda48b30811",
          "flow": "xtls-rprx-vision",
          "network": "tcp",
          "security": "reality",
          "server_name": "www.example.com",
          "fingerprint": "chrome",
          "public_key": "Z84J2IelR9ch3k8VtlVhhs5ycBUlXA7wHBWcBrjqnAw",
          "short_id": "6ba85179e30d4fc2",
          "spider_x": "/"
        }
      }
    })");
    SpaceKey key;
    key.host_name = "bringyour.com";
    key.env_name = "main";
    const std::optional<SpaceValues> stored =
        vless::StoredValuesFor<SpaceKey, SpaceValues>(key, document);
    Check(stored && SameSettings(stored->vless, std::optional<Settings>{RealitySettings()}),
          "the export's VLESS server was not read");

    SpaceValues fresh;
    fresh.bundled = true;
    fresh.link_host_name = "ur.io";
    fresh.api_url = "";
    const SpaceValues written = vless::WithStoredVless(fresh, stored);
    const nlohmann::json writtenJson = written;
    Check(writtenJson.contains("vless") &&
              writtenJson["vless"] == document["values"]["vless"],
          "the values written do not carry the server: " + writtenJson.dump());

    SpaceKey other = key;
    other.host_name = "example.com";
    Check(!vless::StoredValuesFor<SpaceKey, SpaceValues>(other, document),
          "another space's export was read as this one's");
    Check(!vless::StoredValuesFor<SpaceKey, SpaceValues>(key, nlohmann::json()),
          "an empty export was read as values");

    const nlohmann::json noServer = nlohmann::json::parse(
        R"({"key": {"host_name": "bringyour.com", "env_name": "main"}, "values": {"bundled": true}})");
    const std::optional<SpaceValues> withoutServer =
        vless::StoredValuesFor<SpaceKey, SpaceValues>(key, noServer);
    Check(withoutServer && !vless::WithStoredVless(SpaceValues{}, withoutServer).vless,
          "a space with no server gained one");
  }
#endif

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " vless presentation"
#if defined(URNW_VLESS_TESTS_SDK)
            << " (against urnetwork_sdk.hpp)"
#endif
            << ": " << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
