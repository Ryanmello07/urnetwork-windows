// Everything the VLESS settings sheet decides BEFORE it touches a XAML object
// (Settings > VLESS and the VLESS button of the login screen's Change Network
// API sheet; sdk vless_settings.go): the form and its new-form defaults, which
// fields show for the chosen transport and security, the form <-> VlessSettings
// mapping, the store key of an sdk error id, and the picker options.
//
// And the one rule that keeps the settings at all. SdkHost writes a space's
// values WHOLE in two places, BuildNetworkSpace at every launch and
// ApplyNetworkServer when the login sheet applies a network: both build a FRESH
// urnet::NetworkSpaceValues, and updateNetworkSpaceValues replaces the stored
// set with it. Neither knows about VLESS, so without the carry-over below every
// launch would drop the bundled space's VLESS server, and every apply the
// applied space's.
//
// It is all pure for the reason ExtenderPresentation.h gives: a WinUI 3 app
// cannot be built off Windows, so every decision expressed on plain values is
// verified by tools/vless-presentation-tests.cpp on any host, and only the
// drawing is unverified. The SDK header is not included: the mapping and the
// carry-over are templates over urnet::VlessSettings, urnet::NetworkSpaceKey
// and urnet::NetworkSpaceValues by the SDK's field names, so the tests hand
// them stand-ins with the same fields (and the SDK's own types when they are
// built against urnetwork_sdk.hpp), and the app hands them the SDK's types
// (VlessSheet.cpp, SdkHost.cpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::vless {

// ---- the sdk's tokens ---------------------------------------------------------

// connect vless.go VlessNetwork*, VlessSecurity* and VlessFlow*, mirrored so a
// build that has not seen the SDK header still agrees with it. The sdk reads an
// empty transport as tcp and an empty security as none.
inline constexpr const char* kNetworkTcp = "tcp";
inline constexpr const char* kNetworkWs = "ws";
inline constexpr const char* kNetworkHttpUpgrade = "httpupgrade";
inline constexpr const char* kSecurityNone = "none";
inline constexpr const char* kSecurityTls = "tls";
inline constexpr const char* kSecurityReality = "reality";
inline constexpr const char* kFlowNone = "";
inline constexpr const char* kFlowVision = "xtls-rprx-vision";

// ---- errors -----------------------------------------------------------------

// The sdk's error ids (urnet::VlessError*). Each IS the store key of its
// message, so the sheet says what every other app says.
inline constexpr const char* kErrorLinkInvalid = "vless_error_link_invalid";
inline constexpr const char* kErrorLinkUnsupported = "vless_error_link_unsupported";
inline constexpr const char* kErrorAddressInvalid = "vless_error_address_invalid";
inline constexpr const char* kErrorPortInvalid = "vless_error_port_invalid";
inline constexpr const char* kErrorIdInvalid = "vless_error_id_invalid";
inline constexpr const char* kErrorNetworkUnsupported = "vless_error_network_unsupported";
inline constexpr const char* kErrorSecurityUnsupported = "vless_error_security_unsupported";
inline constexpr const char* kErrorFlowInvalid = "vless_error_flow_invalid";
inline constexpr const char* kErrorServerNameRequired = "vless_error_server_name_required";
inline constexpr const char* kErrorFingerprintUnsupported = "vless_error_fingerprint_unsupported";
inline constexpr const char* kErrorPublicKeyInvalid = "vless_error_public_key_invalid";
inline constexpr const char* kErrorShortIdInvalid = "vless_error_short_id_invalid";

inline constexpr const char* kErrorIds[] = {
    kErrorLinkInvalid,
    kErrorLinkUnsupported,
    kErrorAddressInvalid,
    kErrorPortInvalid,
    kErrorIdInvalid,
    kErrorNetworkUnsupported,
    kErrorSecurityUnsupported,
    kErrorFlowInvalid,
    kErrorServerNameRequired,
    kErrorFingerprintUnsupported,
    kErrorPublicKeyInvalid,
    kErrorShortIdInvalid,
};

// What the C ABI answers when the call could not run (URNET_ERROR_ID_INTERNAL:
// an unknown handle, json that did not decode, a recovered panic). It says
// nothing about the settings, so it is never shown as a refusal of them.
inline constexpr const char* kErrorInternal = "internal_error";

// The store key for an error id from ParseVlessLink, SetVlessSettings or
// ValidateVlessSettings. An id this build does not know -- kErrorInternal, or
// an id of a newer sdk -- reads as something_went_wrong, never as the bare id
// on screen and never as a refusal the user did not cause.
const char* ErrorKey(std::string_view errorId);

// ---- the form ---------------------------------------------------------------

// What the sheet's controls hold. A default Form is the sdk's reading of no
// settings at all; NewForm() is what a new form starts from.
struct Form {
  bool enabled = false;
  std::string name;
  std::string address;
  // the port box: digits, or empty for none
  std::string port;
  std::string id;
  std::string network = kNetworkTcp;
  std::string security = kSecurityNone;
  std::string flow;
  std::string serverName;
  std::string fingerprint;
  std::string alpn;
  bool allowInsecure = false;
  std::string publicKey;
  std::string shortId;
  std::string path;
  std::string host;
  // REALITY's spiderX: never edited, only carried, so a link read into the form
  // keeps it through a save and a copy
  std::string spiderX;

  bool operator==(const Form&) const = default;
};

// The sdk's NewVlessSettings as a form: a REALITY server over raw TCP with the
// Vision flow and a chrome hello on 443, not enabled. What FormFrom gives for
// no settings at all, and the shape the sheet shows, disabled, until the
// space's settings are read.
Form NewForm();

// Which of the conditional fields show (the editor's rules 7 to 11). The tokens
// are read as the sdk reads them: trimmed, any case, empty as tcp and none.
struct FieldVisibility {
  // Flow: raw tcp under tls or REALITY only (vision needs a raw stream under
  // tls); while hidden it saves as no flow
  bool flow = false;
  // Server name (SNI) and TLS fingerprint: any security but none
  bool serverNameAndFingerprint = false;
  // ALPN and Allow an insecure certificate: tls
  bool alpnAndAllowInsecure = false;
  // Public key and Short ID: REALITY
  bool publicKeyAndShortId = false;
  // Path and Host header: ws and httpupgrade
  bool pathAndHost = false;

  bool operator==(const FieldVisibility&) const = default;
};

FieldVisibility VisibilityFor(std::string_view network, std::string_view security);

// The port box's text for a port: empty for none (zero, or no port at all).
std::string PortText(std::int64_t port);

// The port a box holds: 0 for an empty box, nullopt for one that is not a
// number of at most five digits. Whitespace around it is ignored. A number out
// of range is the sdk's to refuse, like every other value.
std::optional<std::int64_t> ParsePort(std::string_view text);

// What the port box lets the user type: nothing, or up to five ASCII digits.
bool IsPortInput(std::string_view text);

// What Paste link parses: the clipboard's text, trimmed, when it has any (it
// also goes into the link box), else what is in the box, so a link typed or
// pasted into the box with the keyboard parses the same way. `clipboard` is
// nullopt when the clipboard holds no text or could not be read.
std::string LinkToParse(std::optional<std::string> const& clipboard, std::string const& typed);

// The settings a form describes, or the store key of what kept it from
// describing any (a port box that does not hold a number). Everything else is
// the sdk's to judge: SetVlessSettings validates enabled settings and keeps
// settings that are off as typed.
template <class Settings>
struct SettingsResult {
  Settings settings{};
  std::string errorKey;
};

namespace detail {

// Empty and false are left unset, which is how the sdk hands them back (its
// json omits them), so settings built here compare equal to the sdk's own.
inline std::optional<std::string> Text(std::string const& value) {
  if (value.empty()) return std::nullopt;
  return value;
}
inline std::optional<bool> Flag(bool value) {
  if (!value) return std::nullopt;
  return true;
}

// A picker token as the sdk normalizes it (trimmed, lower case), or `fallback`
// for an unset or empty one.
std::string Token(std::optional<std::string> const& value, std::string_view fallback);

}  // namespace detail

// The form for the settings the space holds. No settings (a read that failed)
// is the new form; settings are read field by field, an unset field as the sdk
// reads it.
template <class Settings>
Form FormFrom(std::optional<Settings> const& settings) {
  if (!settings) return NewForm();
  Form form;
  form.enabled = settings->enabled.value_or(false);
  form.name = settings->name.value_or(std::string());
  form.address = settings->address.value_or(std::string());
  form.port = PortText(settings->port.value_or(0));
  form.id = settings->id.value_or(std::string());
  form.network = detail::Token(settings->network, kNetworkTcp);
  form.security = detail::Token(settings->security, kSecurityNone);
  form.flow = detail::Token(settings->flow, kFlowNone);
  form.serverName = settings->server_name.value_or(std::string());
  form.fingerprint = detail::Token(settings->fingerprint, "");
  form.alpn = settings->alpn.value_or(std::string());
  form.allowInsecure = settings->allow_insecure.value_or(false);
  form.publicKey = settings->public_key.value_or(std::string());
  form.shortId = settings->short_id.value_or(std::string());
  form.path = settings->path.value_or(std::string());
  form.host = settings->host.value_or(std::string());
  form.spiderX = settings->spider_x.value_or(std::string());
  return form;
}

// The settings to save (or copy as a link) from the form. Every field the form
// holds goes across, the hidden ones included, so switching a picker back and
// forth loses nothing typed, and spiderX is carried untouched. The one
// exception is the flow: hidden, it is no flow, because a vision flow left
// behind by a transport or security that cannot carry it would be refused
// (vless_error_flow_invalid) for a field the user can no longer see.
template <class Settings>
SettingsResult<Settings> SettingsFrom(Form const& form) {
  SettingsResult<Settings> result;
  const std::optional<std::int64_t> port = ParsePort(form.port);
  if (!port) {
    result.errorKey = kErrorPortInvalid;
    return result;
  }
  const FieldVisibility shown = VisibilityFor(form.network, form.security);
  Settings& settings = result.settings;
  settings.enabled = detail::Flag(form.enabled);
  settings.name = detail::Text(form.name);
  settings.address = detail::Text(form.address);
  if (*port != 0) settings.port = *port;
  settings.id = detail::Text(form.id);
  settings.flow = shown.flow ? detail::Text(form.flow) : std::optional<std::string>();
  settings.network = detail::Text(form.network);
  settings.security = detail::Text(form.security);
  settings.server_name = detail::Text(form.serverName);
  settings.fingerprint = detail::Text(form.fingerprint);
  settings.alpn = detail::Text(form.alpn);
  settings.allow_insecure = detail::Flag(form.allowInsecure);
  settings.public_key = detail::Text(form.publicKey);
  settings.short_id = detail::Text(form.shortId);
  settings.spider_x = detail::Text(form.spiderX);
  settings.path = detail::Text(form.path);
  settings.host = detail::Text(form.host);
  return result;
}

// ---- the pickers --------------------------------------------------------------

// One picker entry: the value it saves, and the store key of its label (empty:
// the value is shown as it is, as the TLS fingerprints are).
struct Option {
  std::string value;
  std::string labelKey;

  bool operator==(const Option&) const = default;
};

// The sdk's VlessNetworks, VlessSecurities, VlessFlows and VlessFingerprints,
// in its order, with their labels. The empty flow and the empty fingerprint
// are None.
std::vector<Option> NetworkOptions();
std::vector<Option> SecurityOptions();
std::vector<Option> FlowOptions();
std::vector<Option> FingerprintOptions();

// `options` with `current` appended, shown as it is, when no entry has it: a
// stored value this build does not offer (a newer sdk's fingerprint) still shows
// in its picker, and saves back unchanged.
std::vector<Option> OptionsWith(std::vector<Option> options, std::string const& current);

// The index of the entry with `value`, or -1.
int IndexOf(std::vector<Option> const& options, std::string_view value);

// The value of the entry at `index`, or "" (the sdk's reading of an unset
// field) for an index with no entry, such as a picker with nothing selected.
std::string ValueAt(std::vector<Option> const& options, int index);

// ---- keeping the server across a write of the whole space -------------------

// Whether two NetworkSpaceKeys name the same space. An unset field is the empty
// one, as the sdk's json reads it.
template <class Key>
bool SameSpaceKey(Key const& a, Key const& b) {
  return a.host_name.value_or(std::string()) == b.host_name.value_or(std::string()) &&
         a.env_name.value_or(std::string()) == b.env_name.value_or(std::string());
}

// The values a space's export (NetworkSpace::toJson: {"key": ..., "values":
// ...}) holds for `key`, read with the json library's conversions for the SDK
// types (SdkHost::SetNetExtender reads it the same way). nullopt when the export
// is for another space, so another space's server is never carried. A template
// over the document so this header needs no json library: SdkHost hands it an
// nlohmann::json, as do the host tests when built against the SDK header.
template <class Key, class Values, class Json>
std::optional<Values> StoredValuesFor(Key const& key, Json const& document) {
  if (!document.is_object()) return std::nullopt;
  Key storedKey{};
  if (auto it = document.find("key"); it != document.end() && !it->is_null()) {
    it->get_to(storedKey);
  }
  if (!SameSpaceKey(storedKey, key)) return std::nullopt;
  Values values{};
  if (auto it = document.find("values"); it != document.end() && !it->is_null()) {
    it->get_to(values);
  }
  return values;
}

// `next`, the values about to be written whole for a space, with the VLESS
// server of the values stored for that same space (`stored`; nullopt when the
// space has none yet). Values that name a server of their own keep it.
template <class Values>
Values WithStoredVless(Values next, std::optional<Values> const& stored) {
  if (!next.vless && stored && stored->vless) next.vless = stored->vless;
  return next;
}

}  // namespace urnw::vless
