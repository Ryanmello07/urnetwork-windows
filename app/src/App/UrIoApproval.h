// The attempt behind "Continue in browser": a sign-in the person approves on a
// web page they are already signed in to (the web manager's /login-app today)
// while this app waits, signed out, for the page's answer.
//
// What the app does (the contract is docs/app-approval-contract.md;
// the page's half is the web manager's README, "Approving a native app"):
//   * One attempt at a time, started only by a click. It holds a fresh state (43
//     base64url characters of 32 random bytes), a fresh key pair (the secret key
//     never leaves the app), the second it started, the number of the browser
//     flow it belongs to and a deadline five minutes on, on the steady clock AND
//     the wall clock, in memory only. A second click re-opens the same request
//     instead of starting another (Peek), but only while the page would still
//     accept it (Reopenable); later it starts a new attempt, which replaces this
//     one. Only the end of an attempt, or its replacement, starts a new one.
//   * Every answer that comes back as urnetwork://auth passes Check, which says
//     what the link is to this attempt and changes nothing it need not: it
//     consumes an attempt only for a denial, and wipes one only because the
//     attempt is already dead (past its deadline, or superseded). An approval
//     is Accepted but not consumed, so a box that does not open (the caller
//     tries it with SecretKey) leaves the attempt alive for the real answer.
//     The caller consumes it once the box has opened and held this attempt's
//     state (Consume).
//   * The attempt ends, and its secret is wiped, on a match, a cancel, a timeout,
//     any other sign-in starting or succeeding, a sign-out, a network-space
//     change and quit (ClearReason).
//
// Everything here is pure: the random source and the clocks are handed in, and
// the SDK's crypto (the key pair, the unseal) is the caller's. UI thread only,
// as AppController::HandleDeepLink is; the Slot asserts it in a debug build.
//
// The request the page expects is BuildRequestUrl, and only that: five keys in
// a fixed order for the official network (version 1), or those and `api` for
// another network (version 2, ApprovalNetwork.h), values that are never
// percent-encoded because none needs it. The attempt carries the `api` it was
// started with, so a second click re-opens the very same request, and a network
// change ends the attempt like it ends any other. There is no check code: the
// contract reserves a formula for one and this app neither computes nor shows it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ApprovalNetwork.h"
#include "AuthLink.h"

namespace urnw::approval {

// 32 random bytes make a 43-character base64url state.
inline constexpr std::size_t kStateBytes = 32;
static_assert((kStateBytes * 8 + 5) / 6 == authlink::kStateChars);

// One attempt lives five minutes, on both clocks, and re-opening does not extend it.
inline constexpr std::chrono::seconds kLife{300};
// The page refuses a request whose ts is more than 240 s old (contract v1, section 2), at load and again at
// Approve, so a second click re-opens the same request only while it is younger than this, a margin inside
// that window. Later the click starts a new attempt instead of sending the person to a page that says expired.
inline constexpr std::chrono::seconds kReopenWindow{200};
static_assert(kReopenWindow < kLife);
// A wall clock that went back by more than this since the attempt began ends it.
inline constexpr std::chrono::seconds kWallSkew{60};
// The neutral line a stray earns is shown at most this often.
inline constexpr std::chrono::seconds kNeutralEvery{30};
// A public key must be 43 or 44 base58 characters (a key with a leading zero byte
// writes shorter and the page refuses it), so the app draws another pair, this often at most.
inline constexpr int kKeyPairTries = 8;
inline constexpr std::size_t kMinPublicKeyChars = 43;
inline constexpr std::size_t kMaxPublicKeyChars = 44;

// The two clocks an attempt is judged by. The steady clock does not move with the
// wall clock but may not count a sleep; the wall clock counts the sleep but moves
// when the person or the time service sets it. Either one past the deadline ends it.
struct Clocks {
  std::chrono::steady_clock::time_point steady;
  std::chrono::system_clock::time_point wall;
};

inline Clocks Now() { return Clocks{std::chrono::steady_clock::now(), std::chrono::system_clock::now()}; }

// What the gate needs that is not in the link.
struct Context {
  // The app is signed in (SdkHost::IsLoggedIn, or the controller's own LoggedIn).
  // Not "Authenticating": a Google, Apple or wallet attempt that was abandoned
  // leaves that state behind, and a gate that waited for it to clear would be
  // closed for good. Starting any other sign-in ends the attempt instead.
  bool signedIn = false;
  // SdkHost::CurrentBrowserFlow(): the number of the flow that started last. Every
  // browser or wallet flow, a sign-out and a server change take the next one.
  std::uint64_t flow = 0;
};

// What the gate decided about a link. A closed set, so a switch over it that
// misses one is a build error (AppController.cpp turns C4062 on).
enum class Verdict {
  Ignored,   // the app is signed in: nothing is consumed, nothing shown
  Stray,     // not an answer to the live attempt: raises nothing, resets nothing, disturbs nothing
  Denied,    // the live attempt's own access_denied: the attempt is consumed
  Accepted,  // the live attempt's own sealed answer: open it, then Consume
};

// Why a link is what the verdict says. For the log; see ToString.
enum class Why {
  None,
  SignedIn,      // Ignored
  NoAttempt,     // Stray: nothing is waiting for an answer
  Expired,       // Stray: the attempt was past its deadline, and is wiped
  Superseded,    // Stray: another flow started since, and the attempt is wiped
  InvalidShape,  // Stray: not a sealed or denied answer (the parser's reason is logged beside it)
  WrongState,    // Stray: an answer to some other attempt
  BadSeal,       // Stray: the box did not open with this attempt's key, or held another state
};

// What ended an attempt. For the log.
enum class ClearReason {
  Begin,          // another attempt replaced it
  Cancel,         // the person cancelled
  TimedOut,       // five minutes passed
  SignInStarted,  // another sign-in began
  SignedIn,       // some sign-in succeeded
  SignedOut,      // the account signed out
  Superseded,     // another browser or wallet flow, or a server change, took the flow
  Consumed,       // its answer was opened and used
  Denied,         // its answer was access_denied
  Shutdown,       // the app is quitting
};

// The log tokens: closed, lowercase, and written so that none is the id of a
// string resource ("none" and "cancel" are, which is why the draft's were not).
constexpr std::string_view ToString(Verdict verdict) {
  switch (verdict) {
    case Verdict::Ignored: return "ignored";
    case Verdict::Stray: return "stray";
    case Verdict::Denied: return "denied";
    case Verdict::Accepted: return "accepted";
  }
  return "unknown-verdict";
}

constexpr std::string_view ToString(Why why) {
  switch (why) {
    case Why::None: return "ok";
    case Why::SignedIn: return "signed-in";
    case Why::NoAttempt: return "no-attempt";
    case Why::Expired: return "expired";
    case Why::Superseded: return "superseded";
    case Why::InvalidShape: return "invalid-shape";
    case Why::WrongState: return "wrong-state";
    case Why::BadSeal: return "bad-seal";
  }
  return "unknown-why";
}

constexpr std::string_view ToString(ClearReason reason) {
  switch (reason) {
    case ClearReason::Begin: return "replaced";
    case ClearReason::Cancel: return "cancelled";
    case ClearReason::TimedOut: return "timed-out";
    case ClearReason::SignInStarted: return "sign-in-started";
    case ClearReason::SignedIn: return "signed-in";
    case ClearReason::SignedOut: return "signed-out";
    case ClearReason::Superseded: return "superseded";
    case ClearReason::Consumed: return "consumed";
    case ClearReason::Denied: return "denied";
    case ClearReason::Shutdown: return "shutdown";
  }
  return "unknown-clear-reason";
}

struct Decision {
  Verdict verdict = Verdict::Stray;
  Why why = Why::None;
};

// Overwrite, then empty: a secret a string held is not left readable in the buffer the string gives back
// to the allocator (a copy made earlier is not reachable from here, and the app makes none it could not
// reach).
inline void Wipe(std::string& text) {
  volatile char* bytes = text.empty() ? nullptr : &text[0];
  for (std::size_t i = 0; bytes != nullptr && i < text.size(); ++i) bytes[i] = 0;
  text.clear();
}

inline void Wipe(std::vector<std::uint8_t>& data) {
  volatile std::uint8_t* bytes = data.empty() ? nullptr : data.data();
  for (std::size_t i = 0; bytes != nullptr && i < data.size(); ++i) bytes[i] = 0;
  data.clear();
}

namespace detail {

// RFC 4648 section 5, no padding.
inline std::string Base64Url(const unsigned char* data, std::size_t length) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((length * 4 + 2) / 3);
  for (std::size_t i = 0; i < length; i += 3) {
    const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (i + 1 < length ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0u) |
                                (i + 2 < length ? static_cast<std::uint32_t>(data[i + 2]) : 0u);
    out.push_back(kAlphabet[(chunk >> 18) & 63]);
    out.push_back(kAlphabet[(chunk >> 12) & 63]);
    if (i + 1 < length) out.push_back(kAlphabet[(chunk >> 6) & 63]);
    if (i + 2 < length) out.push_back(kAlphabet[chunk & 63]);
  }
  return out;
}

// The alphabets are the parser's own (AuthLink.h), so that what the page may send back and what
// the app takes and asks for are one thing, and the parser's spec sweeps every character of both.
using authlink::detail::IsBase58Char;
using authlink::detail::IsBase64UrlChar;

}  // namespace detail

// The state is a secret until its answer comes back: compare without an early exit.
inline bool SameSecret(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char difference = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference = static_cast<unsigned char>(difference | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
  }
  return difference == 0;
}

// The attempt's X25519 key pair as the sdk writes it, base58. The secret key
// opens the box the page seals the code in.
struct KeyPair {
  std::string publicKey;
  std::string secretKey;
};

// A new pair each call (urnet::generateWalletKeyPair); nullopt when the sdk fails.
using KeyPairSource = std::function<std::optional<KeyPair>()>;

// A pair whose public key the page accepts: 43 or 44 base58 characters. A key
// whose leading byte is zero is written shorter and the page refuses it (about
// one in 460,000), so this draws another pair, up to kKeyPairTries times, and
// wipes each secret key it throws away. nullopt after that: no attempt starts.
inline std::optional<KeyPair> FreshKeyPair(const KeyPairSource& source) {
  for (int attempt = 0; attempt < kKeyPairTries; ++attempt) {
    std::optional<KeyPair> pair = source ? source() : std::nullopt;
    if (!pair) continue;
    bool usable = pair->publicKey.size() >= kMinPublicKeyChars && pair->publicKey.size() <= kMaxPublicKeyChars &&
                  !pair->secretKey.empty();
    for (const char c : pair->publicKey) usable = usable && detail::IsBase58Char(c);
    if (usable) return pair;
    Wipe(pair->secretKey);
  }
  return std::nullopt;
}

class Slot {
 public:
  // BCryptGenRandom in the app, a counter in the tests. false means no entropy,
  // and an attempt that cannot have a fresh state does not start.
  using Random = std::function<bool(unsigned char* out, std::size_t length)>;

  struct Params {
    std::string publicKey;  // base58, 43 or 44 characters (FreshKeyPair)
    std::string secretKey;  // base58; wiped when the attempt ends
    Clocks now;
    std::uint64_t flow = 0;  // SdkHost::BeginBrowserApproval(): the flow this attempt belongs to
    std::string api{};       // the host a version 2 request names (ApprovalNetwork.h); empty for the official network, version 1
  };

  // What the sheet and the request need of a live attempt. A copy: the state is a
  // secret, so a View is kept no longer than the request it builds.
  struct View {
    std::string state;
    std::string publicKey;
    std::int64_t startedUnix = 0;  // the wall clock's second the attempt began: the request's ts
    std::chrono::steady_clock::time_point deadline;
    std::string api{};  // as Params::api: empty means a version 1 request
  };

  Slot() = default;
  Slot(const Slot&) = delete;
  Slot& operator=(const Slot&) = delete;
  ~Slot() { WipeAll(); }

  // Starts an attempt, replacing and wiping whatever was live. The only way one starts.
  std::optional<View> Begin(Params params, const Random& random) {
    AssertOnItsThread();
    Clear(ClearReason::Begin);
    unsigned char raw[kStateBytes] = {};
    const bool drawn = random && random(raw, sizeof raw);
    state_ = drawn ? detail::Base64Url(raw, sizeof raw) : std::string();
    for (unsigned char& byte : raw) byte = 0;
    if (!drawn) {
      Wipe(params.secretKey);
      return std::nullopt;
    }
    publicKey_ = std::move(params.publicKey);
    secretKey_ = std::move(params.secretKey);
    api_ = std::move(params.api);
    begun_ = params.now;
    flow_ = params.flow;
    live_ = true;
    return ViewOf();
  }

  bool Live() const {
    AssertOnItsThread();
    return live_;
  }

  // The live, unexpired attempt as the sheet and a second click see it.
  std::optional<View> Peek(const Clocks& now) const {
    AssertOnItsThread();
    if (!live_ || ExpiredAt(now)) return std::nullopt;
    return ViewOf();
  }

  // Whether a second click may open this attempt's request again: the attempt is live and unexpired, and
  // the request is still younger than kReopenWindow on both clocks, as the deadline is judged. An older one
  // is no longer a request the page accepts, and the caller starts a new attempt.
  bool Reopenable(const Clocks& now) const {
    AssertOnItsThread();
    return live_ && !ExpiredAt(now) && now.steady - begun_.steady < kReopenWindow &&
           now.wall - begun_.wall < kReopenWindow;
  }

  // The gate, as a decision (contract v1, section 5, rule 2, in its order):
  //   signed in                         -> Ignored
  //   no live attempt                   -> Stray (none; or past its deadline on either clock, which wipes it)
  //   another flow started since        -> Stray, and the attempt is wiped
  //   not a sealed or denied shape      -> Stray, the attempt untouched
  //   another attempt's state           -> Stray, the attempt untouched
  //   the live state and access_denied  -> Denied, and the attempt is consumed
  //   the live state, sealed            -> Accepted, the attempt still live
  // A stray never raises a window, resets a screen or disturbs an attempt that is alive.
  Decision Check(const authlink::Return& link, const Clocks& now, const Context& context) {
    AssertOnItsThread();
    if (context.signedIn) return {Verdict::Ignored, Why::SignedIn};
    if (!live_) return {Verdict::Stray, Why::NoAttempt};
    if (ExpiredAt(now)) {
      Clear(ClearReason::TimedOut);
      return {Verdict::Stray, Why::Expired};
    }
    if (context.flow != flow_) {
      Clear(ClearReason::Superseded);
      return {Verdict::Stray, Why::Superseded};
    }
    if (link.kind == authlink::Kind::Invalid) return {Verdict::Stray, Why::InvalidShape};
    if (!SameSecret(link.state, state_)) return {Verdict::Stray, Why::WrongState};
    if (link.kind == authlink::Kind::Denied) {
      Clear(ClearReason::Denied);
      return {Verdict::Denied, Why::None};
    }
    return {Verdict::Accepted, Why::None};
  }

  // Ends the attempt `state` names, and wipes it. False, and nothing ended, when
  // no attempt is live or the state is another's: the caller passes the state the
  // box held, which is how an answer sealed under some other state is refused.
  bool Consume(std::string_view state) {
    AssertOnItsThread();
    if (!live_ || !SameSecret(state, state_)) return false;
    Clear(ClearReason::Consumed);
    return true;
  }

  // The secret key that opens the answer, for as long as the attempt is live.
  std::string_view SecretKey() const {
    AssertOnItsThread();
    return live_ ? std::string_view(secretKey_) : std::string_view();
  }

  // The flow the live attempt belongs to, and 0 with none. The caller sets it against the number
  // of the flow that started last (SdkHost::CurrentBrowserFlow): a server change takes the next
  // number without pushing any sign-in state, so an attempt begun before it is dead although
  // nothing has put a link to the gate yet, and must not be read back (Peek) as if it were live.
  std::uint64_t Flow() const {
    AssertOnItsThread();
    return live_ ? flow_ : 0;
  }

  // Ends the attempt, whatever its state, and wipes its secrets. The reason is
  // the caller's to log (ToString).
  void Clear(ClearReason) {
    AssertOnItsThread();
    WipeAll();
  }

 private:
  bool ExpiredAt(const Clocks& now) const {
    if (now.steady - begun_.steady >= kLife) return true;
    if (now.wall - begun_.wall >= kLife) return true;     // the machine slept through it
    if (begun_.wall - now.wall > kWallSkew) return true;  // the wall clock went back
    return false;
  }

  View ViewOf() const {
    return View{state_, publicKey_,
                std::chrono::duration_cast<std::chrono::seconds>(begun_.wall.time_since_epoch()).count(),
                begun_.steady + kLife, api_};
  }

  void AssertOnItsThread() const { assert(owner_ == std::this_thread::get_id()); }

  void WipeAll() {
    Wipe(state_);
    Wipe(publicKey_);
    Wipe(secretKey_);
    Wipe(api_);
    live_ = false;
  }

  const std::thread::id owner_ = std::this_thread::get_id();
  bool live_ = false;
  std::string state_, publicKey_, secretKey_, api_;
  Clocks begun_;
  std::uint64_t flow_ = 0;
};

// The request the page expects (contract v1, section 2; v2, section 11):
//   <page URL>#v=1&platform=windows&state=<S>&pk=<PK>&ts=<T>
//   <page URL>#v=2&platform=windows&state=<S>&pk=<PK>&ts=<T>&api=<A>
// Version 2 when the view names an `api` (a network other than the official one),
// version 1 when it does not. Exactly these keys, in this order. Every value
// already matches [A-Za-z0-9_-] (`api` has its own grammar, ApprovalNetwork.h:
// lower-case letters, digits, hyphens, dots and a port, which need no escape
// either), so nothing is percent-encoded: this checks that it does, and
// that each has the length the page requires (state 43, pk 43 or 44, ts 10
// digits), and returns an empty string when anything is off, never a request
// that needed an escape or whose values are not of the shape the page asks for.
// That the key is 32 bytes behind its base58 is checked where the key is made
// (ApprovalUnseal.h, NewKeyPair): this header knows the text, not the sdk.
// `pageUrl` is the build constant of the page and carries no fragment of its own.
inline std::string BuildRequestUrl(std::string_view pageUrl, const Slot::View& view) {
  const std::string ts = std::to_string(view.startedUnix);
  const auto allowed = [](std::string_view value) {
    for (const char c : value) {
      if (!detail::IsBase64UrlChar(c)) return false;
    }
    return true;
  };
  bool pageOk = !pageUrl.empty() && pageUrl.size() <= 200;
  for (const char c : pageUrl) {
    const auto byte = static_cast<unsigned char>(c);
    pageOk = pageOk && byte > 0x20 && byte < 0x7F && c != '#' && c != '%';
  }
  bool publicKeyOk = view.publicKey.size() >= kMinPublicKeyChars && view.publicKey.size() <= kMaxPublicKeyChars;
  for (const char c : view.publicKey) publicKeyOk = publicKeyOk && detail::IsBase58Char(c);
  bool tsOk = ts.size() == 10;
  for (const char c : ts) tsOk = tsOk && c >= '0' && c <= '9';
  const bool named = !view.api.empty();
  if (!pageOk || view.state.size() != authlink::kStateChars || !allowed(view.state) || !publicKeyOk ||
      !allowed(view.publicKey) || !tsOk || (named && !IsValidApiHost(view.api))) {
    return {};
  }
  std::string url(pageUrl);
  url += named ? "#v=2&platform=windows&state=" : "#v=1&platform=windows&state=";
  url += view.state;
  url += "&pk=";
  url += view.publicKey;
  url += "&ts=";
  url += ts;
  if (named) {
    url += "&api=";
    url += view.api;
  }
  return url;
}

// The neutral line a stray earns the first launch of a fresh process, at most
// once in kNeutralEvery.
class NeutralLine {
 public:
  bool Allow(std::chrono::steady_clock::time_point now) {
    if (shown_ && now - last_ < kNeutralEvery) return false;
    shown_ = true;
    last_ = now;
    return true;
  }

 private:
  bool shown_ = false;
  std::chrono::steady_clock::time_point last_{};
};

}  // namespace urnw::approval
