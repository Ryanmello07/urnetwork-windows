// The urnetwork://auth return, read strictly, and the redaction that keeps
// links out of the log.
//
// A sign-in approved in the person's own browser, on the page that signs people
// in (the web manager's /login-app today), comes back to the app as a link:
//   urnetwork://auth?state=<s>&epk=<k>&n=<n>&d=<d>   approved: the auth code,
//                                                    sealed to a key only this
//                                                    app holds
//   urnetwork://auth?state=<s>&error=access_denied   cancelled in the browser
// Anything on the machine can fire a link of this scheme at the app, so a link
// means nothing until the sign-in it answers has matched its state
// (AppController::HandleAuthLink): this parser's job is to say exactly what a
// link is, or why it is not one, and never to guess.
//   * Two shapes and no others: {state, epk, n, d} and {state, error}. A code in
//     the clear is not one of them, so a link that carries one (`code`, or
//     anything else the page does not send) is refused. Another key set, an
//     unknown key, a key twice (even with the same value) and an empty pair are
//     refused as well.
//   * Nothing is decoded. Every value the page sends matches [A-Za-z0-9_-]+
//     (base64url for the state, base58 for the sealed values), so a '%' anywhere
//     in a link is refused whatever it would have decoded to, in a key as in a
//     value, and the parser has no decoder to get wrong ('+' is not a space,
//     %00 is not a NUL, a malformed escape is not kept as written).
//   * The host is exactly "auth". The one thing tolerated after it is a single
//     '/' before the query: a real browser launch hands the app
//     urnetwork://auth/?... (seen in the app's own log), so the page's
//     urnetwork://auth?... can arrive that way. A second '/', a path, userinfo,
//     a port or a fragment is refused.
//   * Sizes are capped on the raw string, and each value is checked for its
//     alphabet: state is 43 base64url characters, epk, n and d are base58.
// Routing (IsAuthHost) and the search of a command line for a link
// (LinkInCommandLine) ignore the case of the scheme and the host, so a link
// spelled URNETWORK://AUTH?... is still sent to the gate; Parse does not, and
// calls it not-auth-host, which makes it a stray.
//
// Nothing here logs. The values are secrets until the sign-in they answer
// consumes them: a refused link carries none, ToString(Reject) gives the gate
// closed tokens it may log, and RedactUrl and RedactCommandLine are what the log
// gets instead of a link.
//
// Pure, header-only and free of Windows, WinRT and SDK headers (and of
// <format>): tools/auth-link-tests.cpp runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace urnw::authlink {

// What differs between one version of the page's return and the next.
struct Contract {
  std::string_view prefix;    // scheme and host, lowercase, nothing after them
  bool tolerateEmptyPath;     // one '/' between the host and the '?'
  std::size_t maxUrlBytes;    // measured on the raw string
  std::size_t maxValueBytes;  // per value
  std::size_t maxPairs;
};

// The auth host. IsAuthHost and the contract share it, so routing and parsing
// cannot disagree about what the host is.
inline constexpr std::string_view kAuthPrefix = "urnetwork://auth";

// The page's return as built today (contract v1). tolerateEmptyPath is on
// because the app's own log shows a launch of the form urnetwork://auth/?..., so
// a browser (or the shell) puts the '/' there. The page keeps its whole address
// under 2,000 characters (a sealed answer is about 1,200); the caps here leave
// room and stop anything larger from being read at all.
inline constexpr Contract kApprovalV1{kAuthPrefix, true, 4096, 2048, 6};

// 32 random bytes, base64url, no padding.
inline constexpr std::size_t kStateChars = 43;
// The one error the page sends. Its value is checked and never kept.
inline constexpr std::string_view kDeniedValue = "access_denied";

enum class Kind {
  Invalid,  // see Return::reject
  Sealed,   // {state, epk, n, d}: the auth code sealed to the app's key
  Denied,   // {state, error=access_denied}
};

// Why a link is Invalid. The first reason found, in the order the parser looks.
enum class Reject {
  None,
  NotAuthHost,       // another scheme or host, or the host goes on (authenticator)
  Path,              // a second '/', or anything but the query after the host
  Authority,         // userinfo or a port after the host
  Delimiter,         // a fragment, or a second '?', where only the pairs belong
  TooLong,           // the url, a value or the number of pairs is over its cap
  Control,           // a control character, space, DEL or non-ASCII byte
  UnexpectedEscape,  // a '%' anywhere: the page sends nothing that needs one
  EmptyPair,         // no pairs, an empty segment, a pair without '=' or without a key
  Duplicate,         // a key twice
  UnknownKey,        // a key the contract does not have (a plain `code` among them)
  MixedShapes,       // a key set that is not one of the two shapes, or no state
  BadState,          // not 43 base64url characters
  BadValue,          // an epk, n or d outside the base58 alphabet, or empty
  BadError,          // an error value other than access_denied
};

// The tokens for the log: closed, lowercase and hyphenated, so a token can never
// be the id of a string resource, and never the link or any part of it.
constexpr std::string_view ToString(Reject reject) {
  switch (reject) {
    case Reject::None: return "not-rejected";
    case Reject::NotAuthHost: return "not-auth-host";
    case Reject::Path: return "bad-path";
    case Reject::Authority: return "bad-authority";
    case Reject::Delimiter: return "bad-delimiter";
    case Reject::TooLong: return "too-long";
    case Reject::Control: return "control-char";
    case Reject::UnexpectedEscape: return "unexpected-escape";
    case Reject::EmptyPair: return "empty-pair";
    case Reject::Duplicate: return "duplicate-key";
    case Reject::UnknownKey: return "unknown-key";
    case Reject::MixedShapes: return "mixed-shapes";
    case Reject::BadState: return "bad-state";
    case Reject::BadValue: return "bad-value";
    case Reject::BadError: return "bad-error";
  }
  return "unknown-reject";
}

// A parsed link. Every value is a secret until the sign-in it answers consumes
// it; none is ever logged or shown. An Invalid link carries no value at all.
struct Return {
  Kind kind = Kind::Invalid;
  Reject reject = Reject::NotAuthHost;  // why kind is Invalid; None otherwise
  std::string state;                    // 43 base64url characters
  std::string epk, nonce, data;         // Sealed: base58 of the page's public key, the nonce and the box
};

namespace detail {

constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

constexpr wchar_t Lower(wchar_t c) {
  return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

// Equal but for the case of ASCII letters; `lower` is already lowercase.
template <class Char>
constexpr bool EqualsLower(std::basic_string_view<Char> text, std::basic_string_view<Char> lower) {
  if (text.size() != lower.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (Lower(text[i]) != lower[i]) return false;
  }
  return true;
}

constexpr bool IsBase64UrlChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// Bitcoin's alphabet, which the sdk's encodeBase58 uses: no 0, O, I or l. The attempt model
// (UrIoApproval.h) judges the keys it takes and the request it builds by the same two
// predicates, so a state or a key is never one thing here and another there.
constexpr bool IsBase58Char(char c) {
  const bool digit = c >= '1' && c <= '9';
  const bool upper = (c >= 'A' && c <= 'Z') && c != 'I' && c != 'O';
  const bool lower = (c >= 'a' && c <= 'z') && c != 'l';
  return digit || upper || lower;
}

constexpr bool IsBase58(std::string_view value, std::size_t maxBytes) {
  if (value.empty() || value.size() > maxBytes) return false;
  for (const char c : value) {
    if (!IsBase58Char(c)) return false;
  }
  return true;
}

// A scheme or host the log may echo: short, and nothing but letters, digits and
// . _ + - (so it holds no delimiter, no line break and no secret-shaped text).
constexpr bool IsPlainToken(std::string_view token, std::size_t maxBytes) {
  if (token.size() > maxBytes) return false;
  for (const char c : token) {
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '_' || c == '+' || c == '-';
    if (!plain) return false;
  }
  return true;
}

// The first place at or after `from` where `line` has `lowerScheme` (lowercase,
// ASCII), whatever the case of the scheme in the line.
template <class Char>
std::size_t FindSchemeIgnoringCase(std::basic_string_view<Char> line, std::basic_string_view<Char> lowerScheme,
                                   std::size_t from) {
  for (std::size_t i = from; i + lowerScheme.size() <= line.size(); ++i) {
    if (EqualsLower(line.substr(i, lowerScheme.size()), lowerScheme)) return i;
  }
  return std::basic_string_view<Char>::npos;
}

// Where a link that starts at `begin` ends: at the first of `enders`, or the end
// of the line. The shell quotes the argument it hands over, and a browser
// percent-encodes a space and a quote in a link, so a real one has none.
template <class Char>
std::size_t LinkEnd(std::basic_string_view<Char> line, std::size_t begin, std::basic_string_view<Char> enders) {
  const std::size_t end = line.find_first_of(enders, begin);
  return end == std::basic_string_view<Char>::npos ? line.size() : end;
}

// `text` with every link that starts with `lowerScheme` reduced by RedactUrl, and
// the rest kept. A link ends at the first of `enders`.
template <class Char>
std::basic_string<Char> RedactLinks(std::basic_string_view<Char> text, std::basic_string_view<Char> lowerScheme,
                                    std::basic_string_view<Char> enders);

}  // namespace detail

// Routing only (AppController::HandleDeepLink): the authority is exactly "auth",
// whatever the case of the scheme and the host, so urnetwork://authenticator/...
// is not the auth host and URNETWORK://AUTH?... is. Whether the rest of the link
// is acceptable is Parse's question, not this one's.
//
// A host that only starts like it (auth.evil, auth:80, auth@x, authx, %61uth) is another
// host, and is routed as any other host is: it brings the window forward, as every link
// of another host does, and no sign-in return is read from it. That is kept on purpose
// and is not a gap: a page can bring the window forward with any host it likes, so
// sending look-alikes to the gate would protect nothing, and the gate's promise (a
// stray raises nothing) is about links that are the auth host's.
inline bool IsAuthHost(std::string_view url) {
  if (!detail::EqualsLower(url.substr(0, kAuthPrefix.size()), kAuthPrefix)) return false;
  if (url.size() == kAuthPrefix.size()) return true;
  const char next = url[kAuthPrefix.size()];
  return next == '?' || next == '/' || next == '#';
}

inline Return Parse(std::string_view url, const Contract& contract = kApprovalV1) {
  const auto fail = [](Reject why) {
    Return refused;
    refused.reject = why;
    return refused;
  };
  if (url.size() > contract.maxUrlBytes) return fail(Reject::TooLong);
  for (const char ch : url) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte <= 0x20 || byte >= 0x7F) return fail(Reject::Control);
  }
  if (url.substr(0, contract.prefix.size()) != contract.prefix) return fail(Reject::NotAuthHost);
  if (url.find('%') != std::string_view::npos) return fail(Reject::UnexpectedEscape);

  std::string_view rest = url.substr(contract.prefix.size());
  bool strippedSlash = false;
  if (contract.tolerateEmptyPath && !rest.empty() && rest.front() == '/') {
    rest.remove_prefix(1);
    strippedSlash = true;
  }
  if (rest.empty()) return fail(Reject::EmptyPair);
  if (rest.front() != '?') {
    const char first = rest.front();
    if (first == '#') return fail(Reject::Delimiter);
    if (strippedSlash || first == '/') return fail(Reject::Path);  // auth/x, auth//, or auth/ twice
    if (first == ':' || first == '@') return fail(Reject::Authority);
    return fail(Reject::NotAuthHost);  // the host goes on: urnetwork://authenticator
  }
  rest.remove_prefix(1);
  if (rest.find_first_of("#?") != std::string_view::npos) return fail(Reject::Delimiter);

  // the pairs, as written
  std::vector<std::pair<std::string_view, std::string_view>> pairs;
  for (std::size_t start = 0;;) {
    const std::size_t amp = rest.find('&', start);
    const std::string_view part =
        rest.substr(start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
    const std::size_t eq = part.find('=');
    if (part.empty() || eq == std::string_view::npos || eq == 0) return fail(Reject::EmptyPair);
    pairs.emplace_back(part.substr(0, eq), part.substr(eq + 1));
    if (pairs.size() > contract.maxPairs) return fail(Reject::TooLong);
    if (amp == std::string_view::npos) break;
    start = amp + 1;
  }

  Return out;
  bool hasState = false, hasEpk = false, hasNonce = false, hasData = false, hasError = false;
  std::string error;
  for (const auto& [key, value] : pairs) {
    bool* seen = nullptr;
    std::string* target = nullptr;
    if (key == "state") { seen = &hasState; target = &out.state; }
    else if (key == "epk") { seen = &hasEpk; target = &out.epk; }
    else if (key == "n") { seen = &hasNonce; target = &out.nonce; }
    else if (key == "d") { seen = &hasData; target = &out.data; }
    else if (key == "error") { seen = &hasError; target = &error; }
    else return fail(Reject::UnknownKey);
    if (*seen) return fail(Reject::Duplicate);
    *seen = true;
    if (value.size() > contract.maxValueBytes) return fail(Reject::TooLong);
    target->assign(value);
  }

  if (!hasState) return fail(Reject::MixedShapes);
  if (out.state.size() != kStateChars) return fail(Reject::BadState);
  for (const char ch : out.state) {
    if (!detail::IsBase64UrlChar(ch)) return fail(Reject::BadState);
  }
  if (hasEpk && hasNonce && hasData && !hasError) {
    if (!detail::IsBase58(out.epk, 64) || !detail::IsBase58(out.nonce, 64) ||
        !detail::IsBase58(out.data, contract.maxValueBytes)) {
      return fail(Reject::BadValue);
    }
    out.kind = Kind::Sealed;
  } else if (hasError && !hasEpk && !hasNonce && !hasData) {
    if (error != kDeniedValue) return fail(Reject::BadError);
    out.kind = Kind::Denied;
  } else {
    return fail(Reject::MixedShapes);
  }
  out.reject = Reject::None;
  return out;
}

// ---- finding and redacting a link in a command line, and in a log ------------

// The urnetwork:// link in a command line (the shell appends it as an argument,
// possibly quoted), from the scheme to the first space, tab or quote, or empty
// when there is none. The scheme is matched without regard to case, as IsAuthHost
// matches it. What this returns can carry a credential, so it never reaches a
// log or the UI.
inline std::wstring_view LinkInCommandLine(std::wstring_view line) {
  const std::size_t begin = detail::FindSchemeIgnoringCase<wchar_t>(line, L"urnetwork://", 0);
  if (begin == std::wstring_view::npos) return {};
  return line.substr(begin, detail::LinkEnd<wchar_t>(line, begin, L" \t\"") - begin);
}

// "urnetwork://auth/?state=..." -> "urnetwork://auth [redacted 412 chars]": the
// scheme, the host and a length, nothing else (the path and the query carry
// codes, id tokens, wallet signatures and campaign tokens). The host is echoed
// only when it is a short plain token that nothing follows but a '/', '?', '#' or
// the end; "user:secret@host" and a host with a line break in it print "*" for
// it, because a link's own text must not reach the log, not even as its host.
inline std::string RedactUrl(std::string_view url) {
  std::string out;
  const std::size_t schemeEnd = url.find("://");
  if (schemeEnd != std::string_view::npos && schemeEnd > 0 && detail::IsPlainToken(url.substr(0, schemeEnd), 16)) {
    out.append(url.substr(0, schemeEnd));
    out.append("://");
    const std::size_t hostStart = schemeEnd + 3;
    const std::size_t stop = url.find_first_of("/?#:@", hostStart);
    const std::size_t hostEnd = stop == std::string_view::npos ? url.size() : stop;
    const bool plainStop = stop == std::string_view::npos || url[stop] == '/' || url[stop] == '?' || url[stop] == '#';
    const std::string_view host = url.substr(hostStart, hostEnd - hostStart);
    if (plainStop && detail::IsPlainToken(host, 32)) {
      out.append(host);
    } else {
      out.push_back('*');
    }
    out.push_back(' ');
  }
  out.append("[redacted ");
  out.append(std::to_string(url.size()));
  out.append(" chars]");
  return out;
}

namespace detail {

// `text` as ASCII, every unit above 0x7F as '~'.
template <class Char>
std::string AsciiOnly(std::basic_string_view<Char> text) {
  std::string narrow;
  narrow.reserve(text.size());
  for (const Char unit : text) {
    // a byte of a signed char above 0x7F converts to a huge unsigned value: not ASCII
    const auto code = static_cast<unsigned long>(unit);
    narrow.push_back(code < 0x80 ? static_cast<char>(code) : '~');
  }
  return narrow;
}

// What RedactUrl wrote for a link is "scheme://host [redacted N chars]", and its first half looks like
// a link to the scanner: reduced again, the line would carry a second, false length in front of the
// real one. `link` is that first half and `after` what follows it. Only the exact text RedactUrl makes
// is taken for its own; a link that merely has words of that kind after it is still reduced.
inline bool IsRedactedAlready(std::string_view link, std::string_view after) {
  const std::size_t schemeEnd = link.find("://");
  if (schemeEnd == std::string_view::npos || schemeEnd == 0 || !IsPlainToken(link.substr(0, schemeEnd), 16)) return false;
  const std::string_view host = link.substr(schemeEnd + 3);
  if (host != "*" && !IsPlainToken(host, 32)) return false;
  constexpr std::string_view kBefore = " [redacted ";
  constexpr std::string_view kAfter = " chars]";
  if (after.substr(0, kBefore.size()) != kBefore) return false;
  std::size_t at = kBefore.size();
  const std::size_t digits = at;
  while (at < after.size() && after[at] >= '0' && after[at] <= '9') ++at;
  return at > digits && after.substr(at, kAfter.size()) == kAfter;
}

template <class Char>
std::basic_string<Char> RedactLinks(std::basic_string_view<Char> text, std::basic_string_view<Char> lowerScheme,
                                    std::basic_string_view<Char> enders) {
  std::basic_string<Char> out;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t found = FindSchemeIgnoringCase<Char>(text, lowerScheme, at);
    if (found == std::basic_string_view<Char>::npos) {
      out.append(text.substr(at));
      break;
    }
    out.append(text.substr(at, found - at));
    const std::size_t end = LinkEnd<Char>(text, found, enders);
    const std::string narrow = AsciiOnly<Char>(text.substr(found, end - found));
    if (IsRedactedAlready(narrow, AsciiOnly<Char>(text.substr(end, 40)))) {
      out.append(text.substr(found, end - found));
    } else {
      const std::string redacted = RedactUrl(narrow);
      out.append(redacted.begin(), redacted.end());
    }
    at = end;
  }
  return out;
}

}  // namespace detail

// A whole command line with every urnetwork: link reduced by RedactUrl. The
// scheme token is matched without regard to case and WITHOUT its slashes, so
// urnetwork:auth?... and urnetwork:/auth?... are redacted as well as the
// urnetwork://auth?... the shell passes: the app acts on only the last, but a
// line that is logged must not carry the others. A link ends where
// LinkInCommandLine ends it (at a space, a tab or a quote), so what the app acts
// on is never in the log; the rest of the line is kept.
inline std::wstring RedactCommandLine(std::wstring_view line) {
  return detail::RedactLinks<wchar_t>(line, L"urnetwork:", L" \t\"");
}

// The same for text that has been logged (UTF-8, a line or many): an earlier build
// wrote the whole command line of a protocol launch to the log, and the log is copied
// out as it is when a person exports it. A link ends at a space, a tab, a quote or the
// end of its line, so the line break after it is kept.
inline std::string RedactLogText(std::string_view text) {
  return detail::RedactLinks<char>(text, "urnetwork:", " \t\"\r\n");
}

}  // namespace urnw::authlink
