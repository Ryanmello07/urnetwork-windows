// The country of the mobile network this PC is on, as the sdk asks for it
// (urnet::setNetworkCountryCode; open bug P052).
//
// What the sdk does with it: extender dials front with a name from the spoof
// list of the country the device is in, which the extender hint from the api
// tells it. On a whitelist-only mobile network (Russian carriers) only domestic
// addresses and names are routable, so the hint cannot be fetched, and the sdk
// falls back to the network country the host reports. The sdk's contract for
// that value is the country of the mobile network the device is on (Android
// reports TelephonyManager.networkCountryIso while the default network is
// cellular) and "" on Wi-Fi or any other network whose country the platform
// does not report. It never leaves the process.
//
// What Windows can honestly say: only a mobile broadband adapter (a WWAN modem,
// built in or USB) knows a network country. The provider id of the network it
// is registered on (MbnApi IMbnRegistration::GetProviderID) opens with that
// network's mobile country code. So a country is reported exactly when such an
// adapter carries the default route and is registered on a 3GPP network, and
// "" otherwise: Wi-Fi, Ethernet, a phone tethered over USB or Wi-Fi (Windows
// sees Ethernet or Wi-Fi), a USB modem in router mode, a modem that is not
// registered or is on a CDMA network. Most PCs have no such adapter and always
// report "", which is the truth: they do not know.
//
// Never the locale or the region setting. GetUserGeoID and the display
// language say where the user says they are, not which network the PC is on,
// and the sdk's field is the network's. A wrong country fronts every extender
// dial with a list made for another network.
//
// It is the registered network, not the SIM's home network: a foreign SIM
// roaming on a Russian network reports "ru", as Android's networkCountryIso
// does. The mobile country code maps to a country with the table Android's
// networkCountryIso uses (AOSP MccTable, MCC-only, built from ITU-T E.212), so
// the two apps report the same country for the same network.
//
// Pure and portable, no Windows or SDK header: the app's reader
// (App/MobileBroadband.cpp) gathers the facts and ReadingFor decides, the
// service normalizes what the control pipe brings (Protocol.h
// SetNetworkCountry), and tools/network-country-tests.cpp runs all of it on any
// host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::netcountry {

// ---- the reading -------------------------------------------------------------

// Why the country is what it is. A closed vocabulary: it crosses the control
// pipe and goes into the logs, so nothing outside this list is passed on
// (SourceOrUnknown).
inline constexpr std::string_view kSourceMobileBroadband = "mobile-broadband";
inline constexpr std::string_view kSourceNoDefaultRoute = "no-default-route";
inline constexpr std::string_view kSourceNotMobileBroadband = "not-mobile-broadband";
inline constexpr std::string_view kSourceUnreadable = "mobile-broadband-unreadable";
inline constexpr std::string_view kSourceNot3gpp = "mobile-broadband-not-3gpp";
inline constexpr std::string_view kSourceNotRegistered = "mobile-broadband-not-registered";
inline constexpr std::string_view kSourceUnknownMcc = "mobile-broadband-unknown-mcc";
// A peer that sent no source, or one this build does not know.
inline constexpr std::string_view kSourceUnknown = "unknown";

// A country and why it is that one: what the app reads, the pipe carries and
// the sdk is given.
struct Reading {
  // lower-case ISO 3166-1 alpha-2, "" for none
  std::string code;
  // one of the kSource* tokens
  std::string source;

  bool operator==(const Reading&) const = default;
};

// The code as the sdk takes it: two ASCII letters, lower-cased. Anything else
// is no country, which is also what the sdk makes of it.
inline std::string CodeOrEmpty(std::string_view code) {
  if (code.size() != 2) return {};
  std::string out;
  for (const char c : code) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (c >= 'a' && c <= 'z') {
      out.push_back(c);
    } else {
      return {};
    }
  }
  return out;
}

// One of the tokens above, or kSourceUnknown for anything else.
constexpr std::string_view SourceOrUnknown(std::string_view source) {
  for (const std::string_view known :
       {kSourceMobileBroadband, kSourceNoDefaultRoute, kSourceNotMobileBroadband,
        kSourceUnreadable, kSourceNot3gpp, kSourceNotRegistered, kSourceUnknownMcc}) {
    if (source == known) return known;
  }
  return kSourceUnknown;
}

// What the service takes off the pipe, whatever a peer sent.
inline Reading Normalized(std::string_view code, std::string_view source) {
  return Reading{.code = CodeOrEmpty(code), .source = std::string(SourceOrUnknown(source))};
}

// ---- the mobile country code ---------------------------------------------------

// One assignment: a mobile country code and its two-letter country.
struct MccCountry {
  uint16_t mcc;
  char code[3];
};

// ITU-T E.212 mobile country codes and the ISO 3166-1 country each is assigned
// to, exactly as AOSP's MccTable maps them (android-36), in ascending order.
// Codes outside it -- 001 test networks, 901 international and satellite
// networks, 999 private networks -- are no country.
inline constexpr MccCountry kMccCountries[] = {
    {202, "gr"},  // Greece
    {204, "nl"},  // Netherlands
    {206, "be"},  // Belgium
    {208, "fr"},  // France
    {212, "mc"},  // Monaco
    {213, "ad"},  // Andorra
    {214, "es"},  // Spain
    {216, "hu"},  // Hungary
    {218, "ba"},  // Bosnia and Herzegovina
    {219, "hr"},  // Croatia
    {220, "rs"},  // Serbia
    {221, "xk"},  // Kosovo
    {222, "it"},  // Italy
    {225, "va"},  // Vatican City State
    {226, "ro"},  // Romania
    {228, "ch"},  // Switzerland
    {230, "cz"},  // Czechia
    {231, "sk"},  // Slovak Republic
    {232, "at"},  // Austria
    {234, "gb"},  // United Kingdom
    {235, "gb"},  // United Kingdom
    {238, "dk"},  // Denmark
    {240, "se"},  // Sweden
    {242, "no"},  // Norway
    {244, "fi"},  // Finland
    {246, "lt"},  // Lithuania
    {247, "lv"},  // Latvia
    {248, "ee"},  // Estonia
    {250, "ru"},  // Russian Federation
    {255, "ua"},  // Ukraine
    {257, "by"},  // Belarus
    {259, "md"},  // Moldova
    {260, "pl"},  // Poland
    {262, "de"},  // Germany
    {266, "gi"},  // Gibraltar
    {268, "pt"},  // Portugal
    {270, "lu"},  // Luxembourg
    {272, "ie"},  // Ireland
    {274, "is"},  // Iceland
    {276, "al"},  // Albania
    {278, "mt"},  // Malta
    {280, "cy"},  // Cyprus
    {282, "ge"},  // Georgia
    {283, "am"},  // Armenia
    {284, "bg"},  // Bulgaria
    {286, "tr"},  // Turkiye
    {288, "fo"},  // Faroe Islands
    {289, "ge"},  // Abkhazia (not in the ITU list; kept as Android has it)
    {290, "gl"},  // Greenland
    {292, "sm"},  // San Marino
    {293, "si"},  // Slovenia
    {294, "mk"},  // North Macedonia
    {295, "li"},  // Liechtenstein
    {297, "me"},  // Montenegro
    {302, "ca"},  // Canada
    {308, "pm"},  // Saint Pierre and Miquelon
    {310, "us"},  // United States of America
    {311, "us"},  // United States of America
    {312, "us"},  // United States of America
    {313, "us"},  // United States of America
    {314, "us"},  // United States of America
    {315, "us"},  // United States of America
    {316, "us"},  // United States of America
    {330, "pr"},  // Puerto Rico
    {332, "vi"},  // United States Virgin Islands
    {334, "mx"},  // Mexico
    {338, "jm"},  // Jamaica
    {340, "gp"},  // Guadeloupe
    {342, "bb"},  // Barbados
    {344, "ag"},  // Antigua and Barbuda
    {346, "ky"},  // Cayman Islands
    {348, "vg"},  // British Virgin Islands
    {350, "bm"},  // Bermuda
    {352, "gd"},  // Grenada
    {354, "ms"},  // Montserrat
    {356, "kn"},  // Saint Kitts and Nevis
    {358, "lc"},  // Saint Lucia
    {360, "vc"},  // Saint Vincent and the Grenadines
    {362, "cw"},  // Curacao
    {363, "aw"},  // Aruba
    {364, "bs"},  // Bahamas
    {365, "ai"},  // Anguilla
    {366, "dm"},  // Dominica
    {368, "cu"},  // Cuba
    {370, "do"},  // Dominican Republic
    {372, "ht"},  // Haiti
    {374, "tt"},  // Trinidad and Tobago
    {376, "tc"},  // Turks and Caicos Islands
    {400, "az"},  // Azerbaijan
    {401, "kz"},  // Kazakhstan
    {402, "bt"},  // Bhutan
    {404, "in"},  // India
    {405, "in"},  // India
    {406, "in"},  // India
    {410, "pk"},  // Pakistan
    {412, "af"},  // Afghanistan
    {413, "lk"},  // Sri Lanka
    {414, "mm"},  // Myanmar
    {415, "lb"},  // Lebanon
    {416, "jo"},  // Jordan
    {417, "sy"},  // Syrian Arab Republic
    {418, "iq"},  // Iraq
    {419, "kw"},  // Kuwait
    {420, "sa"},  // Saudi Arabia
    {421, "ye"},  // Yemen
    {422, "om"},  // Oman
    {423, "ps"},  // Palestine (not in the ITU list; kept as Android has it)
    {424, "ae"},  // United Arab Emirates
    {425, "il"},  // Israel
    {426, "bh"},  // Bahrain
    {427, "qa"},  // Qatar
    {428, "mn"},  // Mongolia
    {429, "np"},  // Nepal
    {430, "ae"},  // United Arab Emirates
    {431, "ae"},  // United Arab Emirates
    {432, "ir"},  // Iran
    {434, "uz"},  // Uzbekistan
    {436, "tj"},  // Tajikistan
    {437, "kg"},  // Kyrgyz Republic
    {438, "tm"},  // Turkmenistan
    {440, "jp"},  // Japan
    {441, "jp"},  // Japan
    {450, "kr"},  // Korea
    {452, "vn"},  // Viet Nam
    {454, "hk"},  // Hong Kong, China
    {455, "mo"},  // Macao, China
    {456, "kh"},  // Cambodia
    {457, "la"},  // Lao People's Democratic Republic
    {460, "cn"},  // China
    {461, "cn"},  // China
    {466, "tw"},  // Taiwan, China
    {467, "kp"},  // Democratic People's Republic of Korea
    {470, "bd"},  // Bangladesh
    {472, "mv"},  // Maldives
    {502, "my"},  // Malaysia
    {505, "au"},  // Australia
    {510, "id"},  // Indonesia
    {514, "tl"},  // Timor-Leste
    {515, "ph"},  // Philippines
    {520, "th"},  // Thailand
    {525, "sg"},  // Singapore
    {528, "bn"},  // Brunei Darussalam
    {530, "nz"},  // New Zealand
    {534, "mp"},  // Northern Mariana Islands (not in the ITU list; kept as Android has it)
    {535, "gu"},  // Guam (not in the ITU list; kept as Android has it)
    {536, "nr"},  // Nauru
    {537, "pg"},  // Papua New Guinea
    {539, "to"},  // Tonga
    {540, "sb"},  // Solomon Islands
    {541, "vu"},  // Vanuatu
    {542, "fj"},  // Fiji
    {543, "wf"},  // Wallis and Futuna
    {544, "as"},  // American Samoa
    {545, "ki"},  // Kiribati
    {546, "nc"},  // New Caledonia
    {547, "pf"},  // French Polynesia
    {548, "ck"},  // Cook Islands
    {549, "ws"},  // Samoa
    {550, "fm"},  // Micronesia
    {551, "mh"},  // Marshall Islands
    {552, "pw"},  // Palau
    {553, "tv"},  // Tuvalu
    {554, "tk"},  // Tokelau
    {555, "nu"},  // Niue
    {602, "eg"},  // Egypt
    {603, "dz"},  // Algeria
    {604, "ma"},  // Morocco
    {605, "tn"},  // Tunisia
    {606, "ly"},  // Libya
    {607, "gm"},  // Gambia
    {608, "sn"},  // Senegal
    {609, "mr"},  // Mauritania
    {610, "ml"},  // Mali
    {611, "gn"},  // Guinea
    {612, "ci"},  // Cote d'Ivoire
    {613, "bf"},  // Burkina Faso
    {614, "ne"},  // Niger
    {615, "tg"},  // Togo
    {616, "bj"},  // Benin
    {617, "mu"},  // Mauritius
    {618, "lr"},  // Liberia
    {619, "sl"},  // Sierra Leone
    {620, "gh"},  // Ghana
    {621, "ng"},  // Nigeria
    {622, "td"},  // Chad
    {623, "cf"},  // Central African Republic
    {624, "cm"},  // Cameroon
    {625, "cv"},  // Cabo Verde
    {626, "st"},  // Sao Tome and Principe
    {627, "gq"},  // Equatorial Guinea
    {628, "ga"},  // Gabon
    {629, "cg"},  // Congo
    {630, "cd"},  // Democratic Republic of the Congo
    {631, "ao"},  // Angola
    {632, "gw"},  // Guinea-Bissau
    {633, "sc"},  // Seychelles
    {634, "sd"},  // Sudan
    {635, "rw"},  // Rwanda
    {636, "et"},  // Ethiopia
    {637, "so"},  // Somalia
    {638, "dj"},  // Djibouti
    {639, "ke"},  // Kenya
    {640, "tz"},  // Tanzania
    {641, "ug"},  // Uganda
    {642, "bi"},  // Burundi
    {643, "mz"},  // Mozambique
    {645, "zm"},  // Zambia
    {646, "mg"},  // Madagascar
    {647, "re"},  // French Departments and Territories in the Indian Ocean
    {648, "zw"},  // Zimbabwe
    {649, "na"},  // Namibia
    {650, "mw"},  // Malawi
    {651, "ls"},  // Lesotho
    {652, "bw"},  // Botswana
    {653, "sz"},  // Eswatini
    {654, "km"},  // Comoros
    {655, "za"},  // South Africa
    {657, "er"},  // Eritrea
    {658, "sh"},  // Saint Helena, Ascension and Tristan da Cunha
    {659, "ss"},  // South Sudan
    {702, "bz"},  // Belize
    {704, "gt"},  // Guatemala
    {706, "sv"},  // El Salvador
    {708, "hn"},  // Honduras
    {710, "ni"},  // Nicaragua
    {712, "cr"},  // Costa Rica
    {714, "pa"},  // Panama
    {716, "pe"},  // Peru
    {722, "ar"},  // Argentina
    {724, "br"},  // Brazil
    {730, "cl"},  // Chile
    {732, "co"},  // Colombia
    {734, "ve"},  // Venezuela
    {736, "bo"},  // Bolivia
    {738, "gy"},  // Guyana
    {740, "ec"},  // Ecuador
    {742, "gf"},  // French Guiana
    {744, "py"},  // Paraguay
    {746, "sr"},  // Suriname
    {748, "uy"},  // Uruguay
    {750, "fk"},  // Falkland Islands
};

// The country a three-digit mobile country code is assigned to, "" for a code
// that is not three digits or is assigned to no country.
constexpr std::string_view CountryForMcc(std::string_view mcc) {
  if (mcc.size() != 3) return {};
  int value = 0;
  for (const char c : mcc) {
    if (c < '0' || c > '9') return {};
    value = value * 10 + (c - '0');
  }
  const auto* end = std::end(kMccCountries);
  const auto* it = std::lower_bound(
      std::begin(kMccCountries), end, value,
      [](const MccCountry& entry, int mccValue) { return entry.mcc < mccValue; });
  if (it == end || it->mcc != value) return {};
  return std::string_view(it->code, 2);
}

// The country of a 3GPP provider id: the network's mobile country code followed
// by a two- or three-digit network code, so five or six digits and nothing
// else. "" for anything else.
constexpr std::string_view CountryForProviderId(std::string_view providerId) {
  if (providerId.size() != 5 && providerId.size() != 6) return {};
  for (const char c : providerId) {
    if (c < '0' || c > '9') return {};
  }
  return CountryForMcc(providerId.substr(0, 3));
}

// ---- the default route -----------------------------------------------------------

// One default route (destination prefix length 0) out of the forward table.
struct DefaultRoute {
  uint32_t ifIndex = 0;
  // the route's metric plus its interface's, the order Windows routes by
  uint64_t metric = 0;
  // the interface's media is connected; a state that could not be read counts
  // as connected, as NetworkConfig::DiscoverEgress counts it
  bool connected = true;
};

// The interface that carries this PC's traffic: the default route with the
// lowest metric on a connected interface, the first of equals; 0 for none.
//
// NetworkConfig::DiscoverEgress elects the service's egress the same way, and
// then falls back to a route whose link is down, because its binding is better
// pinned somewhere than nowhere. A link that is down carries nothing, so it has
// no network country, and none is taken from it here. Nothing is excluded
// either: the tun never carries a default route (it captures with the longer
// prefixes of NetPolicy.h), so with the tunnel up this still finds the
// physical interface the sdk's sockets are pinned to.
inline uint32_t ElectDefaultRoute(const std::vector<DefaultRoute>& routes) {
  uint32_t best = 0;
  uint64_t bestMetric = 0;
  for (const DefaultRoute& route : routes) {
    if (!route.connected || route.ifIndex == 0) continue;
    if (best == 0 || route.metric < bestMetric) {
      best = route.ifIndex;
      bestMetric = route.metric;
    }
  }
  return best;
}

// ---- the mobile broadband adapter ----------------------------------------------

// IF_TYPE_WWANPP and IF_TYPE_WWANPP2 (ipifcons.h) and
// NdisPhysicalMediumWirelessWan (ntddndis.h), restated so this header needs no
// Windows header; App/MobileBroadband.cpp checks them against the SDK's.
inline constexpr uint32_t kIfTypeWwanPp = 243;
inline constexpr uint32_t kIfTypeWwanPp2 = 244;
inline constexpr uint32_t kPhysicalMediumWirelessWan = 8;

// A WWAN adapter, by its interface type or its physical medium.
constexpr bool IsMobileBroadbandInterface(uint32_t ifType, uint32_t physicalMedium) {
  return ifType == kIfTypeWwanPp || ifType == kIfTypeWwanPp2 ||
         physicalMedium == kPhysicalMediumWirelessWan;
}

// MBN_REGISTER_STATE (mbnapi.h): the three states in which the adapter is
// registered on a network, and therefore has a provider id to read.
inline constexpr uint32_t kRegisterStateHome = 3;
inline constexpr uint32_t kRegisterStateRoaming = 4;
inline constexpr uint32_t kRegisterStatePartner = 5;

// Home, roaming or partner.
constexpr bool IsRegistered(uint32_t registerState) {
  return registerState == kRegisterStateHome || registerState == kRegisterStateRoaming ||
         registerState == kRegisterStatePartner;
}

// MBN_DATA_CLASS (mbnapi.h, the MBIM data classes): the 3GPP classes, GPRS
// through 5G, and the 3GPP2 (CDMA) ones. A 3GPP2 provider id is a system id,
// not a mobile country code, so the id is read as one only while the current
// data class is 3GPP and nothing else.
inline constexpr uint32_t kDataClass3gpp = 0x000000FFu;
inline constexpr uint32_t kDataClass3gpp2 = 0x007F0000u;

// A 3GPP class with no 3GPP2 class beside it.
constexpr bool Is3gppDataClass(uint32_t dataClass) {
  return (dataClass & kDataClass3gpp) != 0 && (dataClass & kDataClass3gpp2) == 0;
}

// The two adapter identities compare as the GUID they spell: MbnApi names an
// adapter by its interface GUID as text, and the forward table by
// MIB_IF_ROW2::InterfaceGuid. Braces and case do not matter.
constexpr bool SameInterfaceGuid(std::string_view a, std::string_view b) {
  const auto bare = [](std::string_view s) {
    if (s.size() >= 2 && s.front() == '{' && s.back() == '}') s = s.substr(1, s.size() - 2);
    return s;
  };
  a = bare(a);
  b = bare(b);
  if (a.empty() || a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
    if (x != y) return false;
  }
  return true;
}

// ---- the decision -------------------------------------------------------------------

// What the reader found, in the order it looks.
struct MobileBroadbandFacts {
  // a connected interface carries a default route (ElectDefaultRoute)
  bool defaultRoute = false;
  // that interface is a mobile broadband adapter (IsMobileBroadbandInterface)
  bool mobileBroadband = false;
  // MbnApi answered for that adapter; the three fields below are its answers
  bool readable = false;
  uint32_t registerState = 0;
  uint32_t dataClass = 0;
  std::string providerId;
};

// The country the facts support, and else none with the first fact that was
// missing as its source.
inline Reading ReadingFor(const MobileBroadbandFacts& facts) {
  const auto none = [](std::string_view source) {
    return Reading{.code = {}, .source = std::string(source)};
  };
  if (!facts.defaultRoute) return none(kSourceNoDefaultRoute);
  if (!facts.mobileBroadband) return none(kSourceNotMobileBroadband);
  if (!facts.readable) return none(kSourceUnreadable);
  if (!IsRegistered(facts.registerState)) return none(kSourceNotRegistered);
  if (!Is3gppDataClass(facts.dataClass)) return none(kSourceNot3gpp);
  const std::string_view code = CountryForProviderId(facts.providerId);
  if (code.empty()) return none(kSourceUnknownMcc);
  return Reading{.code = std::string(code), .source = std::string(kSourceMobileBroadband)};
}

}  // namespace urnw::netcountry
