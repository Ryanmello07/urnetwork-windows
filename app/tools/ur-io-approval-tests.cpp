// Executable spec for the attempt behind "Continue in browser"
// (App/UrIoApproval.h): the state and the one-attempt slot, the gate's decision
// for every kind of link in every state of the attempt, the two clocks that end
// it, what ends it, the key pair the page accepts, the request the page expects
// and the neutral line a stray earns. Run against the same header the app
// compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App ur-io-approval-tests.cpp -o /tmp/ur-io-approval-tests && /tmp/ur-io-approval-tests
//
// Every value is made up and obviously so (43 'S' for a state, "PUBKEY..." for a
// key); none is a real state, key or code. The one real-looking value, the public
// key in the page's request vector, is the public half of the fixed, documented
// test key pair of the page's conformance vector (the secret key is the bytes 1
// to 32), which protects nothing.
//
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The wipe checks read what a wipe left in the buffer of a string or vector it has cleared, which is past the
// container's size. Under AddressSanitizer that region is poisoned (container-overflow), so the spec makes it
// readable first. GCC says it builds with the sanitizer by defining __SANITIZE_ADDRESS__; clang, by __has_feature.
#if defined(__SANITIZE_ADDRESS__)
#define URN_SPEC_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define URN_SPEC_ASAN 1
#endif
#endif
#if defined(URN_SPEC_ASAN)
#include <sanitizer/asan_interface.h>
#endif

#include "UrIoApproval.h"
#include "approval-vector.h"

using namespace urnw;
using approval::ClearReason;
using approval::Clocks;
using approval::Context;
using approval::Slot;
using approval::Verdict;
using approval::Why;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << std::endl;  // flushed: a spec that dies a moment later still says what failed
  }
}

// Makes the first `size` bytes at `where` readable for the checks that look at what a wipe left behind; nothing
// outside a sanitizer build.
void ReadableAfterClear(const void* where, std::size_t size) {
#if defined(URN_SPEC_ASAN)
  __asan_unpoison_memory_region(where, size);
#else
  (void)where;
  (void)size;
#endif
}

using Steady = std::chrono::steady_clock;
using Wall = std::chrono::system_clock;
using std::chrono::seconds;

// The clocks `steady` and `wall` seconds after the attempt began; both start at a
// made-up instant (a wall second of 1790000000, the vector's ts).
Clocks At(seconds steady, seconds wall) {
  static const Steady::time_point s0 = Steady::time_point{} + std::chrono::hours(100);
  static const Wall::time_point w0 = Wall::time_point{} + seconds(1790000000);
  return Clocks{s0 + steady, w0 + wall};
}

Clocks At(int both) { return At(seconds(both), seconds(both)); }

// A random source that counts its draws, so every state is different and the test knows which was drawn.
int gDraw = 0;
Slot::Random CountingRandom() {
  return [](unsigned char* out, std::size_t length) {
    // a linear congruential generator seeded by the draw's number: nothing like random, but every draw differs
    std::uint32_t x = static_cast<std::uint32_t>(++gDraw) * 2654435761u;
    for (std::size_t i = 0; i < length; ++i) {
      x = x * 1664525u + 1013904223u;
      out[i] = static_cast<unsigned char>(x >> 24);
    }
    return true;
  };
}

// A pair of made-up keys of the lengths and alphabet the page accepts.
const std::string kPublicKey(43, 'P');
const std::string kSecretKey(44, 'K');

Slot::Params Attempt(std::uint64_t flow = 0) {
  Slot::Params params;
  params.publicKey = kPublicKey;
  params.secretKey = kSecretKey;
  params.now = At(0);
  params.flow = flow;
  return params;
}

const std::string kEpk(44, 'E');
const std::string kNonce(33, 'N');
const std::string kData(1036, 'D');

authlink::Return SealedFor(const std::string& state) {
  return authlink::Parse("urnetwork://auth?state=" + state + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + kData);
}
authlink::Return DeniedFor(const std::string& state) {
  return authlink::Parse("urnetwork://auth?state=" + state + "&error=access_denied");
}
std::string StateOf(char c) { return std::string(43, c); }

const Context kQuiet{false, 0};

void TestTheStateIsFreshAndBase64Url() {
  Slot slot;
  const auto first = slot.Begin(Attempt(), CountingRandom());
  Check(first && first->state.size() == 43, "begin: a 43-character state");
  bool alphabet = first.has_value();
  for (const char c : first->state) alphabet = alphabet && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                                              (c >= '0' && c <= '9') || c == '-' || c == '_');
  Check(alphabet, "begin: the state is base64url");
  const auto second = slot.Begin(Attempt(), CountingRandom());
  Check(second && second->state != first->state, "begin: a fresh state per attempt");

  // the state is the unpadded base64url of the 32 random bytes: known bytes, known text
  const auto of = [](unsigned char (*fill)(std::size_t)) {
    static unsigned char (*current)(std::size_t) = nullptr;
    current = fill;
    Slot s;
    const auto view = s.Begin(Attempt(), [](unsigned char* out, std::size_t length) {
      for (std::size_t i = 0; i < length; ++i) out[i] = current(i);
      return true;
    });
    return view ? view->state : std::string();
  };
  Check(of([](std::size_t) -> unsigned char { return 0; }) == StateOf('A'), "state: 32 zero bytes are 43 'A'");
  Check(of([](std::size_t) -> unsigned char { return 0xFF; }) == std::string(42, '_') + "8",
        "state: 32 bytes of 0xFF are 42 '_' and an '8' (the url alphabet, not '/' and '+')");
  Check(of([](std::size_t i) -> unsigned char { return static_cast<unsigned char>(i); }) ==
            "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8",
        "state: the bytes 0 to 31 are RFC 4648's text for them, without padding");
  Check(of([](std::size_t i) -> unsigned char { return static_cast<unsigned char>(224 + i); }) ==
            "4OHi4-Tl5ufo6err7O3u7_Dx8vP09fb3-Pn6-_z9_v8",
        "state: the bytes 224 to 255 use '-' and '_'");

  // and over many draws every character of the alphabet turns up, and no other
  std::set<char> seen;
  std::set<std::string> states;
  for (int i = 0; i < 400; ++i) {
    Slot s;
    const auto view = s.Begin(Attempt(), CountingRandom());
    if (!view) continue;
    states.insert(view->state);
    for (const char c : view->state) seen.insert(c);
  }
  Check(seen.size() == 64, "state: all 64 characters of the alphabet appear over many draws");
  Check(states.size() == 400, "state: 400 attempts, 400 different states");

  Slot dead;
  Check(!dead.Begin(Attempt(), [](unsigned char*, std::size_t) { return false; }) && !dead.Live(),
        "begin: no entropy, no attempt");
  Check(!dead.Begin(Attempt(), Slot::Random{}) && !dead.Live(), "begin: no random source, no attempt");
  Slot replaced;
  replaced.Begin(Attempt(), CountingRandom());
  Check(!replaced.Begin(Attempt(), [](unsigned char*, std::size_t) { return false; }) && !replaced.Live(),
        "begin: a failed draw still ends the attempt it was to replace");
}

void TestOneAttemptAtATime() {
  Slot slot;
  const auto first = slot.Begin(Attempt(), CountingRandom());
  Check(first.has_value() && slot.Live(), "slot: an attempt is live after Begin");
  const auto reopened = slot.Peek(At(10));
  Check(reopened && reopened->state == first->state && reopened->publicKey == first->publicKey &&
            reopened->startedUnix == first->startedUnix && reopened->deadline == first->deadline,
        "slot: a second click reads the same request back: same state, key, second and deadline");
  Check(first->publicKey == kPublicKey && first->startedUnix == 1790000000,
        "slot: the view carries the public key and the second the attempt began");
  Check(first->deadline == At(300).steady, "slot: the deadline is five minutes after the start, on the steady clock");
  Check(reopened && slot.Peek(At(299)) && slot.Peek(At(299))->deadline == first->deadline,
        "slot: reading it back late does not move the deadline");

  const auto second = slot.Begin(Attempt(), CountingRandom());
  Check(second && second->state != first->state, "slot: Begin replaces the attempt");
  const auto late = slot.Check(SealedFor(first->state), At(1), kQuiet);
  Check(late.verdict == Verdict::Stray && late.why == Why::WrongState,
        "slot: the replaced attempt's answer is a stray");
  Check(slot.Peek(At(1)) && slot.Peek(At(1))->state == second->state, "slot: and the new attempt is alive");
  Check(slot.SecretKey() == kSecretKey, "slot: the secret key is held for the unseal");
}

// The page refuses a request whose ts is more than 240 s old, at load and again at Approve (contract v1, section 2). A
// second click opens the same request again only while it is younger than a margin inside that, judged on both clocks as
// the attempt's deadline is, and later the caller starts a new attempt. The attempt itself lives until 300 s.
void TestASecondClickReopensOnlyWhileThePageWouldAcceptTheRequest() {
  Check(approval::kReopenWindow == seconds(200), "reopen: the window is 200 s");
  Check(approval::kReopenWindow < seconds(240), "reopen: and ends before the page's own 240 s");
  Check(approval::kReopenWindow < approval::kLife, "reopen: and before the attempt's deadline");

  Slot none;
  Check(!none.Reopenable(At(0)), "reopen: with no attempt there is nothing to open again");

  Slot slot;
  slot.Begin(Attempt(), CountingRandom());
  Check(slot.Reopenable(At(0)) && slot.Reopenable(At(1)) && slot.Reopenable(At(10)), "reopen: a request just made is opened again");
  Check(slot.Reopenable(At(199)), "reopen: at 199 s still");
  Check(!slot.Reopenable(At(200)), "reopen: not at 200 s");
  Check(!slot.Reopenable(At(239)) && !slot.Reopenable(At(240)) && !slot.Reopenable(At(299)),
        "reopen: nor between the window and the deadline, where the page would call the request expired");
  Check(slot.Peek(At(250)).has_value() && !slot.Reopenable(At(250)),
        "reopen: although the attempt is alive until 300 s (Peek reads it, and the caller replaces it)");
  Check(!slot.Reopenable(At(300)) && !slot.Reopenable(At(400)), "reopen: and not after the deadline");

  // the steady clock and the wall clock each end it
  Check(slot.Reopenable(At(seconds(199), seconds(0))) && !slot.Reopenable(At(seconds(200), seconds(0))),
        "reopen: the steady clock decides");
  Check(slot.Reopenable(At(seconds(0), seconds(199))) && !slot.Reopenable(At(seconds(0), seconds(200))),
        "reopen: and the wall clock, which counts a sleep the steady clock does not");
  // a wall clock put back a little does not, and one put back two minutes ends the attempt, so there is nothing to open
  Check(slot.Reopenable(At(seconds(5), seconds(-30))), "reopen: a 30 s correction of the wall clock does not end it");
  Check(!slot.Reopenable(At(seconds(5), seconds(-120))), "reopen: one of two minutes does");

  // an attempt that has ended is not opened again, whatever the clocks say
  for (const ClearReason reason : {ClearReason::Cancel, ClearReason::Superseded, ClearReason::TimedOut}) {
    Slot ended;
    ended.Begin(Attempt(), CountingRandom());
    ended.Clear(reason);
    Check(!ended.Reopenable(At(1)), std::string("reopen: an attempt that ended (") + std::string(approval::ToString(reason)) + ") is not opened again");
  }
  // and a new attempt starts the window again
  Slot again;
  again.Begin(Attempt(), CountingRandom());
  Check(!again.Reopenable(At(250)), "reopen: 250 s after the first attempt began, its request is too old");
  Slot::Params later = Attempt();
  later.now = At(250);
  again.Begin(std::move(later), CountingRandom());
  Check(again.Reopenable(At(260)) && again.Reopenable(At(449)) && !again.Reopenable(At(450)),
        "reopen: a new attempt has a window of its own, counted from its own start");
}

void TestTheGate() {
  const auto t1 = At(1);
  Slot slot;
  auto view = slot.Begin(Attempt(), CountingRandom());
  const auto accepted = slot.Check(SealedFor(view->state), t1, kQuiet);
  Check(accepted.verdict == Verdict::Accepted && accepted.why == Why::None,
        "gate: the live attempt's own sealed answer is Accepted");
  Check(slot.Live(), "gate: Accepted does not consume it (a box that does not open must leave it alive)");
  Check(slot.Check(SealedFor(view->state), t1, kQuiet).verdict == Verdict::Accepted,
        "gate: and the answer can be offered again");
  Check(!slot.Consume(StateOf('Z')) && slot.Live(), "gate: an inner state that differs consumes nothing");
  Check(!slot.Consume("") && slot.Live(), "gate: nor does an empty one");
  Check(slot.Consume(view->state) && !slot.Live(), "gate: Consume ends the attempt");
  Check(!slot.Consume(view->state), "gate: and only once");
  const auto replay = slot.Check(SealedFor(view->state), t1, kQuiet);
  Check(replay.verdict == Verdict::Stray && replay.why == Why::NoAttempt, "gate: a replay is a stray: nothing is waiting");
  Check(slot.SecretKey().empty(), "gate: the secret key is gone with the attempt");

  // strays leave a live attempt exactly as it was
  view = slot.Begin(Attempt(), CountingRandom());
  const auto before = slot.Peek(t1);
  const auto wrong = slot.Check(SealedFor(StateOf('Q')), t1, kQuiet);
  Check(wrong.verdict == Verdict::Stray && wrong.why == Why::WrongState, "gate: another state is a stray");
  const auto junk = slot.Check(authlink::Parse("urnetwork://auth/x?state=1"), t1, kQuiet);
  Check(junk.verdict == Verdict::Stray && junk.why == Why::InvalidShape, "gate: a link that is no answer is a stray");
  const auto plain = slot.Check(authlink::Parse("urnetwork://auth?state=" + view->state + "&code=abc"), t1, kQuiet);
  Check(plain.verdict == Verdict::Stray && plain.why == Why::InvalidShape,
        "gate: a code in the clear, even beside the live state, is a stray");
  const auto deniedOther = slot.Check(DeniedFor(StateOf('Q')), t1, kQuiet);
  Check(deniedOther.verdict == Verdict::Stray && deniedOther.why == Why::WrongState,
        "gate: access_denied for another state is a stray");
  const auto after = slot.Peek(t1);
  Check(slot.Live() && before && after && before->state == after->state && before->deadline == after->deadline &&
            slot.SecretKey() == kSecretKey,
        "gate: and the live attempt is untouched by all of them");

  // a denial for the live state is consumed at once
  const auto denied = slot.Check(DeniedFor(view->state), t1, kQuiet);
  Check(denied.verdict == Verdict::Denied && denied.why == Why::None && !slot.Live(),
        "gate: access_denied for the live state is Denied, and consumes the attempt");
  Check(slot.SecretKey().empty(), "gate: a denial wipes the key");
  const auto deniedAgain = slot.Check(DeniedFor(view->state), t1, kQuiet);
  Check(deniedAgain.verdict == Verdict::Stray && deniedAgain.why == Why::NoAttempt, "gate: a denial can be replayed no more than an approval");

  // nothing waiting: whatever arrives is a stray
  Slot empty;
  Check(empty.Check(SealedFor(StateOf('S')), t1, kQuiet).why == Why::NoAttempt, "gate: no attempt, a sealed answer");
  Check(empty.Check(DeniedFor(StateOf('S')), t1, kQuiet).why == Why::NoAttempt, "gate: no attempt, a denial");
  Check(empty.Check(authlink::Parse("junk"), t1, kQuiet).why == Why::NoAttempt && empty.Check(authlink::Parse("junk"), t1, kQuiet).verdict == Verdict::Stray,
        "gate: no attempt, no link at all");

  // signed in: ignored, whatever the link, and the attempt is left as it is
  Slot ignoring;
  view = ignoring.Begin(Attempt(), CountingRandom());
  Context signedIn;
  signedIn.signedIn = true;
  const auto ignored = ignoring.Check(SealedFor(view->state), t1, signedIn);
  Check(ignored.verdict == Verdict::Ignored && ignored.why == Why::SignedIn && ignoring.Live(),
        "gate: signed in is Ignored even for the live state, and consumes nothing");
  Check(ignoring.Check(DeniedFor(view->state), t1, signedIn).verdict == Verdict::Ignored && ignoring.Live(),
        "gate: signed in, a denial is ignored as well");
  Check(Slot().Check(SealedFor(StateOf('S')), t1, signedIn).verdict == Verdict::Ignored,
        "gate: signed in, with no attempt, is ignored and not a stray");
  Check(Slot().Check(authlink::Parse("junk"), t1, signedIn).verdict == Verdict::Ignored,
        "gate: signed in, with no link, is ignored");
  // what the context's other field means: only signed-in decides, nothing else about the app's state
  Context busy;
  busy.flow = 0;
  Check(ignoring.Check(SealedFor(view->state), t1, busy).verdict == Verdict::Accepted,
        "gate: with the app not signed in, the same answer is Accepted (an abandoned sign-in elsewhere does not close the gate)");
}

void TestExpiryOnBothClocks() {
  Slot slot;
  auto view = slot.Begin(Attempt(), CountingRandom());
  Check(slot.Check(SealedFor(view->state), At(299), kQuiet).verdict == Verdict::Accepted,
        "expiry: 299 s is inside the five minutes");
  Check(slot.Peek(At(299)).has_value(), "expiry: and the attempt can still be read back");
  auto late = slot.Check(SealedFor(view->state), At(300), kQuiet);
  Check(late.verdict == Verdict::Stray && late.why == Why::Expired && !slot.Live(),
        "expiry: 300 s is past the deadline: the answer is a stray, and the attempt is wiped");
  Check(slot.SecretKey().empty(), "expiry: and its key with it");

  // a denial that arrives late is no more than a stray either: nothing is waiting for it
  Slot lateDenial;
  view = lateDenial.Begin(Attempt(), CountingRandom());
  late = lateDenial.Check(DeniedFor(view->state), At(301), kQuiet);
  Check(late.verdict == Verdict::Stray && late.why == Why::Expired && !lateDenial.Live(),
        "expiry: a denial after the deadline is a stray, not a cancel");

  // the steady clock a second short, the wall clock on the dot, and the other way round
  Slot wallOnly;
  view = wallOnly.Begin(Attempt(), CountingRandom());
  Check(wallOnly.Check(SealedFor(view->state), At(seconds(1), seconds(299)), kQuiet).verdict == Verdict::Accepted,
        "expiry: wall 299 s, steady 1 s: still alive");
  Check(wallOnly.Check(SealedFor(view->state), At(seconds(1), seconds(300)), kQuiet).why == Why::Expired,
        "expiry: wall 300 s while the steady clock saw 1 s (the machine slept through it)");
  Slot steadyOnly;
  view = steadyOnly.Begin(Attempt(), CountingRandom());
  Check(steadyOnly.Check(SealedFor(view->state), At(seconds(300), seconds(1)), kQuiet).why == Why::Expired,
        "expiry: steady 300 s while the wall clock saw 1 s (the clock was set back)");
  Slot slept;
  view = slept.Begin(Attempt(), CountingRandom());
  Check(slept.Check(SealedFor(view->state), At(seconds(1), seconds(600)), kQuiet).why == Why::Expired,
        "expiry: the wall clock saw ten minutes, the steady clock one");

  // a wall clock moved back
  Slot rolled;
  view = rolled.Begin(Attempt(), CountingRandom());
  Check(rolled.Check(SealedFor(view->state), At(seconds(5), seconds(-120)), kQuiet).why == Why::Expired,
        "expiry: a wall clock moved back two minutes ends it");
  Slot nudged;
  view = nudged.Begin(Attempt(), CountingRandom());
  Check(nudged.Check(SealedFor(view->state), At(seconds(5), seconds(-30)), kQuiet).verdict == Verdict::Accepted,
        "expiry: a 30 s correction of the wall clock does not");
  Check(nudged.Check(SealedFor(view->state), At(seconds(5), seconds(-60)), kQuiet).verdict == Verdict::Accepted,
        "expiry: nor one of exactly a minute");
  Check(nudged.Check(SealedFor(view->state), At(seconds(5), seconds(-61)), kQuiet).why == Why::Expired,
        "expiry: a minute and a second does");

  // reading it back
  Slot peeked;
  peeked.Begin(Attempt(), CountingRandom());
  Check(peeked.Peek(At(299)).has_value() && !peeked.Peek(At(300)).has_value(),
        "expiry: Peek of an attempt past its deadline is empty");
  Check(!peeked.Peek(At(seconds(1), seconds(300))).has_value() && !peeked.Peek(At(seconds(300), seconds(0))).has_value(),
        "expiry: on either clock");

  // anything after the deadline wipes the dead attempt, and is a stray for that reason
  Slot wrongLate;
  view = wrongLate.Begin(Attempt(), CountingRandom());
  const auto strayLate = wrongLate.Check(SealedFor(StateOf('Q')), At(400), kQuiet);
  Check(strayLate.verdict == Verdict::Stray && strayLate.why == Why::Expired && !wrongLate.Live(),
        "expiry: a stray after the deadline is a stray, and the dead attempt is wiped");
  Slot junkLate;
  junkLate.Begin(Attempt(), CountingRandom());
  Check(junkLate.Check(authlink::Parse("junk"), At(400), kQuiet).why == Why::Expired && !junkLate.Live(),
        "expiry: so is a link that is no answer");
  // and signed in still beats it: the gate asks that first, and touches nothing
  Slot signedLate;
  signedLate.Begin(Attempt(), CountingRandom());
  Context signedIn;
  signedIn.signedIn = true;
  Check(signedLate.Check(authlink::Parse("junk"), At(400), signedIn).verdict == Verdict::Ignored && signedLate.Live(),
        "expiry: signed in is Ignored before the deadline is looked at");
}

void TestAnotherFlowEndsTheAttempt() {
  const auto t1 = At(1);
  Slot slot;
  auto view = slot.Begin(Attempt(/*flow=*/7), CountingRandom());
  Context sameFlow;
  sameFlow.flow = 7;
  Check(slot.Check(SealedFor(view->state), t1, sameFlow).verdict == Verdict::Accepted,
        "flow: the flow it began in is the flow it is answered in");
  Context later;
  later.flow = 8;  // another browser or wallet flow began, a sign-out happened, a server was applied
  auto d = slot.Check(SealedFor(view->state), t1, later);
  Check(d.verdict == Verdict::Stray && d.why == Why::Superseded && !slot.Live(),
        "flow: a later flow ends the attempt; its answer is a stray and the attempt is wiped");
  view = slot.Begin(Attempt(7), CountingRandom());
  d = slot.Check(DeniedFor(view->state), t1, later);
  Check(d.verdict == Verdict::Stray && d.why == Why::Superseded && !slot.Live(),
        "flow: a denial after a later flow is a stray as well, not a cancel");
  view = slot.Begin(Attempt(7), CountingRandom());
  d = slot.Check(authlink::Parse("junk"), t1, later);
  Check(d.why == Why::Superseded && !slot.Live(), "flow: so is any link at all");
  Context earlier;
  earlier.flow = 6;
  view = slot.Begin(Attempt(7), CountingRandom());
  Check(slot.Check(SealedFor(view->state), t1, earlier).why == Why::Superseded,
        "flow: a flow number that differs either way ends it");
}

void TestEveryReasonEndsTheAttempt() {
  const auto t1 = At(1);
  for (const ClearReason reason :
       {ClearReason::Begin, ClearReason::Cancel, ClearReason::TimedOut, ClearReason::SignInStarted,
        ClearReason::SignedIn, ClearReason::SignedOut, ClearReason::Superseded, ClearReason::Consumed,
        ClearReason::Denied, ClearReason::Shutdown}) {
    Slot slot;
    const auto view = slot.Begin(Attempt(), CountingRandom());
    slot.Clear(reason);
    Check(!slot.Live() && slot.SecretKey().empty() && !slot.Peek(t1) &&
              slot.Check(SealedFor(view->state), t1, kQuiet).why == Why::NoAttempt &&
              !slot.Consume(view->state),
          std::string("clear: ") + std::string(approval::ToString(reason)) + " ends the attempt and wipes its key");
  }
  Slot twice;
  twice.Clear(ClearReason::Cancel);
  twice.Clear(ClearReason::Shutdown);
  Check(!twice.Live(), "clear: with no attempt it is no harm");
}

void TestTheTokensAreClosed() {
  std::set<std::string> verdicts;
  for (const Verdict v : {Verdict::Ignored, Verdict::Stray, Verdict::Denied, Verdict::Accepted}) {
    verdicts.insert(std::string(approval::ToString(v)));
  }
  std::set<std::string> whys;
  for (const Why w : {Why::None, Why::SignedIn, Why::NoAttempt, Why::Expired, Why::Superseded, Why::InvalidShape,
                      Why::WrongState, Why::BadSeal}) {
    whys.insert(std::string(approval::ToString(w)));
  }
  std::set<std::string> reasons;
  for (const ClearReason r : {ClearReason::Begin, ClearReason::Cancel, ClearReason::TimedOut, ClearReason::SignInStarted,
                              ClearReason::SignedIn, ClearReason::SignedOut, ClearReason::Superseded,
                              ClearReason::Consumed, ClearReason::Denied, ClearReason::Shutdown}) {
    reasons.insert(std::string(approval::ToString(r)));
  }
  Check(verdicts.size() == 4 && whys.size() == 8 && reasons.size() == 10, "tokens: one per verdict, reason and cause, none repeated");
  bool lowercase = true;
  for (const std::set<std::string>* group : {&verdicts, &whys, &reasons}) {
    for (const std::string& token : *group) {
      for (const char c : token) lowercase = lowercase && ((c >= 'a' && c <= 'z') || c == '-');
    }
  }
  Check(lowercase, "tokens: lowercase letters and hyphens only");
  Check(approval::ToString(Verdict::Stray) == "stray" && approval::ToString(Why::NoAttempt) == "no-attempt" &&
            approval::ToString(Why::BadSeal) == "bad-seal" && approval::ToString(ClearReason::TimedOut) == "timed-out",
        "tokens: the words the owner's log checks read");
}

// ---- the key pair the page accepts ------------------------------------------

void TestAFreshKeyPairIsOneThePageAccepts() {
  int calls = 0;
  const auto good = [&calls] {
    ++calls;
    return std::optional<approval::KeyPair>(approval::KeyPair{std::string(43, 'P'), std::string(44, 'K')});
  };
  auto pair = approval::FreshKeyPair(good);
  Check(pair && calls == 1 && pair->publicKey.size() == 43 && pair->secretKey.size() == 44,
        "key pair: a usable pair is taken at once");

  calls = 0;
  pair = approval::FreshKeyPair([&calls] {
    ++calls;
    return std::optional<approval::KeyPair>(
        approval::KeyPair{std::string(calls < 8 ? 42 : 44, 'P'), std::string(44, 'K')});
  });
  Check(pair && calls == 8 && pair->publicKey.size() == 44, "key pair: seven short keys, then a good one on the eighth try");

  calls = 0;
  pair = approval::FreshKeyPair([&calls] {
    ++calls;
    return std::optional<approval::KeyPair>(approval::KeyPair{std::string(42, 'P'), std::string(44, 'K')});
  });
  Check(!pair && calls == 8, "key pair: eight short keys, no attempt, and no ninth try");

  calls = 0;
  pair = approval::FreshKeyPair([&calls] {
    ++calls;
    return calls < 4 ? std::optional<approval::KeyPair>() : std::optional<approval::KeyPair>(
                                                               approval::KeyPair{std::string(43, 'P'), std::string(44, 'K')});
  });
  Check(pair && calls == 4, "key pair: the sdk failing three times in a row is tried again");
  calls = 0;
  pair = approval::FreshKeyPair([&calls] {
    ++calls;
    return std::optional<approval::KeyPair>();
  });
  Check(!pair && calls == 8, "key pair: the sdk failing every time is eight tries and no attempt");
  Check(!approval::FreshKeyPair(approval::KeyPairSource{}), "key pair: no source, no pair");

  const auto with = [](std::string publicKey, std::string secretKey) {
    return approval::FreshKeyPair([&] {
      return std::optional<approval::KeyPair>(approval::KeyPair{publicKey, secretKey});
    });
  };
  Check(with(std::string(43, 'P'), "K").has_value() && with(std::string(44, 'P'), "K").has_value(),
        "key pair: 43 and 44 characters are both accepted");
  Check(!with(std::string(42, 'P'), "K") && !with(std::string(45, 'P'), "K"), "key pair: 42 and 45 are not");
  Check(!with(std::string(43, 'P'), "").has_value(), "key pair: a pair with no secret key is not a pair");
  for (const char bad : std::string("0OIl_-+/=.")) {
    std::string key(43, 'P');
    key[20] = bad;
    Check(!with(key, "K").has_value(), std::string("key pair: a public key with '") + bad + "' is not base58");
  }
}

// ---- the request the page expects -------------------------------------------

// The page's grammar for a request's fragment, re-stated here from its README
// ("Request: app to page") and the contract's section 11: at most 400 characters of
// A-Z a-z 0-9 _ = & - and, for the one key that may use them, . and :; five keys each
// once and v exactly 1, or six and v exactly 2 with the sixth, api, last of the keys the
// page reads; state 43 base64url, pk 43 or 44 base58, ts ten digits, api a name.
bool PageAccepts(const std::string& url, std::string_view page) {
  const std::size_t hash = url.find('#');
  if (hash == std::string::npos || url.substr(0, hash) != page) return false;
  const std::string fragment = url.substr(hash + 1);
  if (fragment.size() > 400) return false;
  for (const char c : fragment) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '=' || c == '&' || c == '-' || c == '.' || c == ':';
    if (!ok) return false;
  }
  std::vector<std::pair<std::string, std::string>> pairs;
  for (std::size_t start = 0;;) {
    const std::size_t amp = fragment.find('&', start);
    const std::string part = fragment.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
    const std::size_t eq = part.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == part.size()) return false;
    pairs.emplace_back(part.substr(0, eq), part.substr(eq + 1));
    if (amp == std::string::npos) break;
    start = amp + 1;
  }
  std::set<std::string> keys;
  for (const auto& pair : pairs) keys.insert(pair.first);
  if (keys.size() != pairs.size()) return false;
  std::string version;
  for (const auto& [key, value] : pairs) {
    if (key == "v") version = value;
  }
  // a version 1 request carries no api and a version 2 request exactly one: either mismatch makes the whole request invalid
  if (version == "1") {
    if (keys != std::set<std::string>{"v", "platform", "state", "pk", "ts"}) return false;
  } else if (version == "2") {
    if (keys != std::set<std::string>{"v", "platform", "state", "pk", "ts", "api"}) return false;
  } else {
    return false;
  }
  for (const auto& [key, value] : pairs) {
    // the characters that only api may hold
    if (key != "api" && value.find_first_of(".:") != std::string::npos) return false;
    if (key == "api") {
      if (value.empty() || value.size() > 200) return false;
      for (const char c : value) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':';
        if (!ok) return false;
      }
    }
    if (key == "platform" && value != "windows") return false;
    if (key == "state" && value.size() != 43) return false;
    if (key == "pk") {
      if (value.size() != 43 && value.size() != 44) return false;
      for (const char c : value) {
        if (std::string_view("123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz").find(c) == std::string_view::npos) {
          return false;
        }
      }
    }
    if (key == "ts") {
      if (value.size() != 10) return false;
      for (const char c : value) {
        if (c < '0' || c > '9') return false;
      }
    }
  }
  return true;
}

void TestTheRequestIsExactlyWhatThePageExpects() {
  // the page's own vector: the request its fixture builds for its fixed keys (the
  // manager's tests/fixtures/app-auth-vector.json, "request"), with this app's page url
  constexpr std::string_view kPage = "https://beta.app.ur.network/login-app";
  Slot::View view;
  view.state = StateOf('S');
  view.publicKey = "WpSggvhoqoVW4XJvfjsB52dvVunYSQWrz6XiuMQVsCf";
  view.startedUnix = 1790000000;
  const std::string url = approval::BuildRequestUrl(kPage, view);
  Check(url == std::string(kPage) + "#v=1&platform=windows&state=" + StateOf('S') +
                   "&pk=WpSggvhoqoVW4XJvfjsB52dvVunYSQWrz6XiuMQVsCf&ts=1790000000",
        "request: the page url, then v, platform, state, pk and ts, in that order, and nothing else");
  Check(PageAccepts(url, kPage), "request: and the page's grammar accepts it");
  Check(url.find('%') == std::string::npos && url.find('+') == std::string::npos, "request: nothing is percent-encoded");
  Check(url.size() < 400 + kPage.size() + 1, "request: well inside the page's 400 characters for the fragment");

  // the whole path: a slot's own view builds a request the page accepts
  Slot slot;
  Slot::Params params = Attempt();
  params.publicKey = "WpSggvhoqoVW4XJvfjsB52dvVunYSQWrz6XiuMQVsCf";
  const auto live = slot.Begin(std::move(params), CountingRandom());
  const std::string built = live ? approval::BuildRequestUrl(kPage, *live) : std::string();
  Check(!built.empty() && PageAccepts(built, kPage) && built.find("ts=1790000000") != std::string::npos &&
            built.find("state=" + live->state + "&") != std::string::npos,
        "request: built from a live attempt, it carries its state, key and second");
  const auto again = slot.Peek(At(5));
  Check(again && approval::BuildRequestUrl(kPage, *again) == built,
        "request: a second click builds the same request, to the byte");

  // a 44-character key, the other length the page takes
  view.publicKey = std::string(44, 'P');
  Check(PageAccepts(approval::BuildRequestUrl(kPage, view), kPage), "request: a 44-character key");

  // anything off fails closed: an empty string, never a request the page would refuse, never an escape
  const auto refusedAt = [](std::string_view page, const Slot::View& v, const std::string& what) {
    Check(approval::BuildRequestUrl(page, v).empty(), "request: " + what + " fails closed");
  };
  const auto refused = [&](const Slot::View& v, const std::string& what) { refusedAt(kPage, v, what); };
  Slot::View good = view;
  good.publicKey = "WpSggvhoqoVW4XJvfjsB52dvVunYSQWrz6XiuMQVsCf";
  Check(!approval::BuildRequestUrl(kPage, good).empty(), "request: the control view builds");
  Slot::View bad = good;
  bad.state = StateOf('S').substr(1);
  refused(bad, "a 42-character state");
  bad.state = StateOf('S') + "S";
  refused(bad, "a 44-character state");
  for (const char c : std::string(" +=%.&/#?\"<>\\!$'")) {
    bad = good;
    bad.state[10] = c;
    refused(bad, std::string("a state with '") + c + "' (it would need an escape, and nothing is escaped)");
  }
  bad = good;
  bad.state = "a b+c&d";
  refused(bad, "a state that is not a state");
  bad = good;
  bad.state.clear();
  refused(bad, "an empty state");
  bad = good;
  bad.publicKey = good.publicKey.substr(2);
  refused(bad, "a 41-character key");
  bad.publicKey = good.publicKey + "abc";
  refused(bad, "a 46-character key");
  bad.publicKey = good.publicKey.substr(1);
  refused(bad, "a 42-character key (a leading zero byte writes shorter, and the page refuses it)");
  for (const char c : std::string("0OIl_-+=%/ ")) {
    bad = good;
    bad.publicKey[20] = c;
    refused(bad, std::string("a key with '") + c + "'");
  }
  bad = good;
  bad.startedUnix = 999999999;
  refused(bad, "a 9-digit second");
  bad.startedUnix = 10000000000;
  refused(bad, "an 11-digit second");
  bad.startedUnix = -1790000000;
  refused(bad, "a negative second");
  // ten characters, so the length alone does not refuse it: the minus sign is no digit
  bad.startedUnix = -179000000;
  refused(bad, "a negative second of ten characters");
  bad.startedUnix = 0;
  refused(bad, "a second of zero (a clock that never ran)");
  bad.startedUnix = 1000000000;
  Check(!approval::BuildRequestUrl(kPage, bad).empty(), "request: ten digits from 1000000000 are fine");
  bad.startedUnix = 9999999999;
  Check(!approval::BuildRequestUrl(kPage, bad).empty(), "request: and up to 9999999999");
  refusedAt("", good, "an empty page url");
  refusedAt("https://example.test/login-app#x", good, "a page url with a fragment of its own");
  refusedAt("https://example.test/log in", good, "a page url with a space");
  refusedAt("https://example.test/log%20in", good, "a page url with an escape");
  refusedAt("https://example.test/login\n", good, "a page url with a line break");
  refusedAt(std::string("https://example.test/") + std::string(280, 'a'), good, "a page url of 300 characters");
  // the page url is at most 200 characters, and DEL is no character of one
  const std::string room = "https://example.test/";
  Check(!approval::BuildRequestUrl(room + std::string(200 - room.size(), 'a'), good).empty(),
        "request: a page url of exactly 200 characters is taken");
  refusedAt(room + std::string(201 - room.size(), 'a'), good, "a page url of 201 characters");
  refusedAt(room + "a" + static_cast<char>(0x7f), good, "a page url with DEL in it");
  Check(!approval::BuildRequestUrl(room + "a~", good).empty(), "request: and '~', the last printable character, is");
}

// ---- the request of version 2 (contract section 11) ---------------------------------------

constexpr std::string_view kPageUrl = "https://beta.app.ur.network/login-app";

// The view of the page's conformance vector: its state, its key, its second. `api` is the caller's to set.
Slot::View VectorView() {
  Slot::View view;
  view.state = std::string(approval_vector::kState);
  view.publicKey = std::string(approval_vector::kPublicKey);
  view.startedUnix = approval_vector::kStartedUnix;
  return view;
}

// Version 2 is version 1 plus api, which comes last, and the request vector both repositories commit is reproduced to the
// byte. A view that names nothing is the official network's request, and is the one it always was.
void TestTheRequestOfVersionTwoNamesTheNetwork() {
  Slot::View view = VectorView();
  view.api = std::string(approval_vector::kApi);
  const std::string url = approval::BuildRequestUrl(kPageUrl, view);
  Check(url == std::string(kPageUrl) + "#" + std::string(approval_vector::kRequestV2),
        "request v2: the shared vector, byte for byte, after the page url and the '#'");
  Check(approval_vector::kRequestV2.size() == 153, "request v2: the vector is 153 characters, as the contract says");
  Check(approval_vector::kApi == "api.beta-test.net" && approval_vector::kStartedUnix == 1790000000,
        "request v2: and it names api.beta-test.net at the second 1790000000");
  // the keys in the contract's order, put together here without the header's help
  Check(url == std::string(kPageUrl) + "#v=2&platform=windows&state=" + StateOf('S') +
                   "&pk=WpSggvhoqoVW4XJvfjsB52dvVunYSQWrz6XiuMQVsCf&ts=1790000000&api=api.beta-test.net",
        "request v2: v, platform, state, pk, ts and api, in that order, and nothing else");
  Check(PageAccepts(url, kPageUrl), "request v2: and the page's grammar accepts it");
  Check(url.find('%') == std::string::npos && url.find('+') == std::string::npos, "request v2: nothing is percent-encoded");

  // the same view without a network is the request it always was: version 1, five keys, no api
  Slot::View official = VectorView();
  const std::string v1 = approval::BuildRequestUrl(kPageUrl, official);
  Check(v1 == std::string(kPageUrl) + "#v=1&platform=windows&state=" + StateOf('S') +
                  "&pk=WpSggvhoqoVW4XJvfjsB52dvVunYSQWrz6XiuMQVsCf&ts=1790000000",
        "request v1: a view that names no network is the official network's request, byte for byte as before");
  Check(v1.find("api") == std::string::npos, "request v1: and says nothing of a network");
  Check(PageAccepts(v1, kPageUrl), "request v1: and the page's grammar accepts it");
  // version 2 is version 1 with the number changed and api added at the end
  Check(url.substr(url.find("&platform=")) == v1.substr(v1.find("&platform=")) + "&api=api.beta-test.net" &&
            url.substr(0, url.find('#') + 3) == std::string(kPageUrl) + "#v=" && url.at(url.find('#') + 3) == '2' &&
            v1.at(v1.find('#') + 3) == '1',
        "request v2: is version 1 with the version changed and api added");

  // a port is part of the name, and a 44-character key is the other length the page takes
  view.api = "api.example.com:8443";
  view.publicKey = std::string(44, 'P');
  const std::string withPort = approval::BuildRequestUrl(kPageUrl, view);
  Check(!withPort.empty() && PageAccepts(withPort, kPageUrl) && withPort.size() > 27 &&
            withPort.substr(withPort.size() - 25) == "&api=api.example.com:8443",
        "request v2: a name with a port, and a key of 44 characters");

  // the longest name there is still leaves the fragment inside the page's 400 characters
  const std::string name200 = std::string(63, 'a') + "." + std::string(63, 'a') + "." + std::string(63, 'a') + ".bcdefghi";
  view = VectorView();
  view.api = name200;
  const std::string longest = approval::BuildRequestUrl(kPageUrl, view);
  Check(name200.size() == 200 && !longest.empty() && PageAccepts(longest, kPageUrl) && longest.size() - kPageUrl.size() - 1 == 336,
        "request v2: the longest name (200 characters) makes a fragment of 336, inside the page's 400");
  view.api = name200 + "i";
  Check(approval::BuildRequestUrl(kPageUrl, view).empty(), "request v2: a name of 201 characters fails closed");

  // the model starts as the official network
  Check(Slot::Params{}.api.empty() && Slot::View{}.api.empty(), "defaults: an attempt names no network until it is told to");
}

// An api that is not what the contract allows fails closed: an empty string, never a request the page would refuse. The
// rest of the request is held to the same rules whether or not it names a network.
void TestARequestThatNamesNoValidNetworkIsRefused() {
  Slot::View good = VectorView();
  good.api = std::string(approval_vector::kApi);
  Check(!approval::BuildRequestUrl(kPageUrl, good).empty(), "request v2: the control view builds");
  const std::vector<std::string> bad = {
      "API.beta-test.net", "api.Beta-test.net", "https://api.beta-test.net", "http://api.beta-test.net", "api.beta-test.net/",
      "api.beta-test.net/v1", "user@api.beta-test.net", "api.beta-test.net:443", "api.beta-test.net:0",
      "api.beta-test.net:65536", "api.beta-test.net:08443", "api.beta-test.net:", "api.beta-test.net::8443", "localhost",
      "localhost:8443", "10.0.0.1", "10.0.0.1:8443", "api.beta-test.123", "a..b", "a.b.", "-a.b", "a-.b", ".a.b",
      "a.0x10", "1.2.3.0x4", "0x7f.0x0.0x0.0x1", "a.0x",
      "api.b\xC3\xA9ta-test.net", "api.beta-test.net?x=1", "api.beta-test.net#x", "api.beta-test.net&x=1",
      "api.beta-test.net=1", "api.beta%2Dtest.net", "api.beta+test.net", "api.beta test.net", " api.beta-test.net",
      "api.beta-test.net ", "api.beta-test.net\n", "[::1]:8443", "api.beta-test.net.", std::string("api.beta\0test.net", 17),
      std::string(3, ' '), std::string(1, '\0'),
  };
  for (const std::string& api : bad) {
    Slot::View view = good;
    view.api = api;
    std::string shown;
    for (const char c : api) shown += (c >= 0x20 && c < 0x7F) ? std::string(1, c) : std::string("?");
    Check(approval::BuildRequestUrl(kPageUrl, view).empty(), "request v2: the name '" + shown + "' fails closed");
  }
  // 200 characters in all, with a port, and one more
  Slot::View tooLong = good;
  tooLong.api = std::string(63, 'a') + "." + std::string(63, 'a') + "." + std::string(63, 'a') + ".bcd:8080";
  Check(tooLong.api.size() == 200 && !approval::BuildRequestUrl(kPageUrl, tooLong).empty(), "request v2: 200 characters with the port are taken");
  tooLong.api += "0";
  Check(approval::BuildRequestUrl(kPageUrl, tooLong).empty(), "request v2: 201 with the port fail closed");

  // a good name does not make up for anything else that is off
  Slot::View other = good;
  other.state = StateOf('S').substr(1);
  Check(approval::BuildRequestUrl(kPageUrl, other).empty(), "request v2: a 42-character state fails closed");
  other = good;
  other.publicKey = good.publicKey.substr(2);
  Check(approval::BuildRequestUrl(kPageUrl, other).empty(), "request v2: a 41-character key fails closed");
  other = good;
  other.publicKey[20] = '0';
  Check(approval::BuildRequestUrl(kPageUrl, other).empty(), "request v2: a key outside base58 fails closed");
  other = good;
  other.startedUnix = 999999999;
  Check(approval::BuildRequestUrl(kPageUrl, other).empty(), "request v2: a 9-digit second fails closed");
  other = good;
  other.startedUnix = -179000000;
  Check(approval::BuildRequestUrl(kPageUrl, other).empty(), "request v2: a negative second of ten characters fails closed");
  Check(approval::BuildRequestUrl("https://example.test/login-app#x", good).empty(), "request v2: a page url with a fragment fails closed");
  Check(approval::BuildRequestUrl("", good).empty(), "request v2: an empty page url fails closed");

  // an empty name is not a bad one: it is the official network, and the request is version 1
  Slot::View none = good;
  none.api.clear();
  const std::string v1 = approval::BuildRequestUrl(kPageUrl, none);
  Check(v1.find("#v=1&") != std::string::npos && v1.find("api") == std::string::npos, "request: no name is the official network's request");
}

// The attempt keeps the network it started in, in memory with the rest of its values, and hands it back with them.
void TestAnAttemptCarriesTheNetworkItStartedIn() {
  Slot slot;
  Slot::Params named = Attempt();
  named.publicKey = std::string(approval_vector::kPublicKey);
  named.api = "api.beta-test.net";
  const auto first = slot.Begin(std::move(named), CountingRandom());
  Check(first && first->api == "api.beta-test.net", "attempt: the view carries the network the attempt started in");
  const auto again = slot.Peek(At(5));
  Check(again && again->api == "api.beta-test.net", "attempt: a second click reads the network back with the rest");
  Check(first && again && approval::BuildRequestUrl(kPageUrl, *again) == approval::BuildRequestUrl(kPageUrl, *first) &&
            approval::BuildRequestUrl(kPageUrl, *again).find("#v=2&") != std::string::npos,
        "attempt: and a second click re-opens the very same request, to the byte, version 2");

  // a new attempt does not inherit the network of the one it replaces: the official network names nothing
  Slot::Params official = Attempt();
  official.publicKey = std::string(approval_vector::kPublicKey);
  const auto next = slot.Begin(std::move(official), CountingRandom());
  Check(next && next->api.empty(), "attempt: an attempt that names no network names none, whatever the one it replaced named");
  const std::string nextUrl = next ? approval::BuildRequestUrl(kPageUrl, *next) : std::string();
  Check(nextUrl.find("#v=1&") != std::string::npos && nextUrl.find("api") == std::string::npos,
        "attempt: and its request is version 1");
  // and the other way round
  Slot::Params named2 = Attempt();
  named2.publicKey = std::string(approval_vector::kPublicKey);
  named2.api = "api.example.com:8443";
  const auto last = slot.Begin(std::move(named2), CountingRandom());
  Check(last && last->api == "api.example.com:8443" && slot.Peek(At(1)) && slot.Peek(At(1))->api == "api.example.com:8443",
        "attempt: a new attempt names its own network, not the replaced one's");

  // a failed draw begins nothing, and names nothing
  Slot failed;
  Slot::Params withApi = Attempt();
  withApi.api = "api.beta-test.net";
  Check(!failed.Begin(std::move(withApi), [](unsigned char*, std::size_t) { return false; }) && !failed.Live() && !failed.Peek(At(1)),
        "attempt: a failed draw begins nothing, and the network it was to name is not kept");

  // every end of the attempt takes it with the rest
  for (const ClearReason reason : {ClearReason::Begin, ClearReason::Cancel, ClearReason::TimedOut, ClearReason::SignInStarted,
                                   ClearReason::SignedIn, ClearReason::SignedOut, ClearReason::Superseded, ClearReason::Consumed,
                                   ClearReason::Denied, ClearReason::Shutdown}) {
    Slot ended;
    Slot::Params params = Attempt();
    params.api = "api.beta-test.net";
    ended.Begin(std::move(params), CountingRandom());
    ended.Clear(reason);
    Check(!ended.Peek(At(1)).has_value() && !ended.Live(),
          std::string("attempt: ") + std::string(approval::ToString(reason)) + " ends an attempt that named a network");
    // and what a new attempt reads back afterwards is its own
    const auto fresh = ended.Begin(Attempt(), CountingRandom());
    Check(fresh && fresh->api.empty(), std::string("attempt: after ") + std::string(approval::ToString(reason)) + " a new one names nothing");
  }
}

// An attempt belongs to the network it started in. A network change (SdkHost::ApplyNetworkServer) takes the next flow
// number and pushes no sign-in state, so the attempt for the network it left is a stray when its answer comes, whatever
// the answer is, and nothing of it is read back.
void TestANetworkChangeEndsAnAttemptForAnotherNetwork() {
  Slot slot;
  Slot::Params params = Attempt(/*flow=*/7);
  params.api = "api.beta-test.net";
  auto view = slot.Begin(std::move(params), CountingRandom());
  Context sameNetwork;
  sameNetwork.flow = 7;
  Check(slot.Check(SealedFor(view->state), At(1), sameNetwork).verdict == Verdict::Accepted,
        "network change: before it, the attempt's own answer is accepted");
  Context changed;
  changed.flow = 8;
  const auto sealed = slot.Check(SealedFor(view->state), At(1), changed);
  Check(sealed.verdict == Verdict::Stray && sealed.why == Why::Superseded && !slot.Live(),
        "network change: after it, the attempt's answer is a stray, and the attempt is wiped");
  Check(!slot.Peek(At(1)) && slot.Flow() == 0 && slot.SecretKey().empty(),
        "network change: and nothing of it is read back, the network it named with the rest");
  Check(slot.Check(SealedFor(view->state), At(1), changed).why == Why::NoAttempt, "network change: the answer once more is a stray with nothing waiting");

  // a denial for it is no cancel, and a link of any kind is a stray
  Slot::Params second = Attempt(/*flow=*/7);
  second.api = "api.beta-test.net";
  view = slot.Begin(std::move(second), CountingRandom());
  const auto denied = slot.Check(DeniedFor(view->state), At(1), changed);
  Check(denied.verdict == Verdict::Stray && denied.why == Why::Superseded && !slot.Live(),
        "network change: a denial for the attempt it ended is a stray, not a cancel");
  Slot::Params third = Attempt(/*flow=*/7);
  third.api = "api.beta-test.net";
  slot.Begin(std::move(third), CountingRandom());
  const auto junk = slot.Check(authlink::Parse("junk"), At(1), changed);
  Check(junk.why == Why::Superseded && !slot.Live(), "network change: so is any link at all");

  // the controller compares the number the attempt began in with the last one (a second click, the auth-state hook)
  Slot::Params fourth = Attempt(/*flow=*/9);
  fourth.api = "api.example.com:8443";
  slot.Begin(std::move(fourth), CountingRandom());
  Check(slot.Flow() == 9 && slot.Peek(At(1)) && slot.Peek(At(1))->api == "api.example.com:8443",
        "network change: a live attempt reports the flow it began in, with the network it named");
}

// ---- the state's encoding ------------------------------------------------------

// RFC 4648 section 10's vectors, without the padding, and the url alphabet's two last characters. The state is always
// 32 bytes (ten whole groups and a tail of two), so the other tails are what keeps each branch of the encoder honest.
void TestTheStateIsRfc4648Base64UrlWithoutPadding() {
  const auto encode = [](const std::string& text) {
    return approval::detail::Base64Url(reinterpret_cast<const unsigned char*>(text.data()), text.size());
  };
  Check(encode("") == "" && encode("f") == "Zg" && encode("fo") == "Zm8" && encode("foo") == "Zm9v",
        "base64url: the vectors of RFC 4648 for zero to three bytes, without padding");
  Check(encode("foob") == "Zm9vYg" && encode("fooba") == "Zm9vYmE" && encode("foobar") == "Zm9vYmFy",
        "base64url: and for four to six");
  Check(encode(std::string("\xFB\xFF\xBF", 3)) == "-_-_", "base64url: '-' and '_' stand for 62 and 63, not '+' and '/'");
  Check(encode(std::string("\xFB", 1)) == "-w" && encode(std::string("\xFB\xFF", 2)) == "-_8",
        "base64url: the tails of one and two bytes use the same alphabet");
  Check(encode(std::string(32, 'f')).size() == 43, "base64url: 32 bytes are 43 characters");
  // and the encoder reads only the bytes it was given: the same inputs with 0xFF after them in memory
  for (const std::string text : {"f", "fo", "foo", "foob", "fooba"}) {
    std::vector<unsigned char> bytes(text.begin(), text.end());
    bytes.insert(bytes.end(), {0xFF, 0xFF, 0xFF});
    Check(approval::detail::Base64Url(bytes.data(), text.size()) == encode(text),
          "base64url: '" + text + "' with other bytes after it in memory is the same text");
  }
  // a context and an attempt that were never filled in are signed out, at no flow and at no second
  Check(!Context{}.signedIn && Context{}.flow == 0, "defaults: a context is signed out, at flow 0");
  Check(Slot::Params{}.flow == 0 && Slot::View{}.startedUnix == 0, "defaults: nothing has begun until it is told to");
}

// ---- the neutral line ------------------------------------------------------

void TestTheNeutralLineIsOnePerHalfAMinute() {
  approval::NeutralLine line;
  const auto t0 = Steady::time_point{} + std::chrono::hours(1);
  Check(line.Allow(t0), "neutral line: the first is allowed");
  Check(!line.Allow(t0 + seconds(1)) && !line.Allow(t0 + seconds(29)), "neutral line: not again inside 30 s");
  Check(line.Allow(t0 + seconds(30)), "neutral line: again at 30 s");
  Check(!line.Allow(t0 + seconds(59)) && line.Allow(t0 + seconds(60)),
        "neutral line: a refused ask does not move the window");
  approval::NeutralLine another;
  Check(another.Allow(Steady::time_point{}), "neutral line: the first is allowed even at the clock's origin");
}

// ---- the flow an attempt belongs to ------------------------------------------

// The caller, not the slot, judges a live attempt that a server change left behind (Peek is context-free): it asks the
// slot which flow the attempt began in, and compares that with the number of the flow that started last.
void TestTheFlowTheAttemptBelongsTo() {
  Slot slot;
  Check(slot.Flow() == 0, "flow: no attempt, no flow");
  slot.Begin(Attempt(/*flow=*/7), CountingRandom());
  Check(slot.Flow() == 7, "flow: the attempt carries the number of the flow it began in");
  slot.Begin(Attempt(/*flow=*/9), CountingRandom());
  Check(slot.Flow() == 9, "flow: and a new attempt the number it began in");
  slot.Check(DeniedFor(StateOf('Q')), At(1), Context{false, 9});
  Check(slot.Flow() == 9 && slot.Live(), "flow: a stray does not move it");
  slot.Clear(ClearReason::Superseded);
  Check(slot.Flow() == 0 && !slot.Live(), "flow: nothing is left once the attempt has ended");
  Slot failed;
  failed.Begin(Attempt(/*flow=*/5), [](unsigned char*, std::size_t) { return false; });
  Check(failed.Flow() == 0, "flow: a failed draw began nothing");
  Slot reopened;
  reopened.Begin(Attempt(/*flow=*/3), CountingRandom());
  Check(reopened.Peek(At(1)).has_value() && reopened.Flow() == 3,
        "flow: a live attempt is read back whatever number the app is at now (the caller compares it with Flow())");
  Slot consumed;
  const auto view = consumed.Begin(Attempt(/*flow=*/4), CountingRandom());
  consumed.Consume(view->state);
  Check(consumed.Flow() == 0, "flow: a consumed attempt has none either");
}

// ---- the comparisons, the alphabets and the wipes ----------------------------------------------------------
// What the checks above leave to the code beside them: the state is compared at every one of its places, every
// character of the two alphabets is let through and no other, and what an attempt held is overwritten.

const std::string kBase64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
const std::string kBase58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

void TestTheComparisonsTheAlphabetsAndTheWipeAreWhole() {
  // the state is compared byte for byte: a difference in any one of the 43 places is another attempt's state
  {
    const std::string live(43, 'Q');
    Check(approval::SameSecret(live, live), "compare: equal states are the same");
    for (std::size_t at = 0; at < live.size(); ++at) {
      std::string other = live;
      other[at] = 'R';
      Check(!approval::SameSecret(live, other), "compare: a difference at place " + std::to_string(at) + " is another state");
    }
    Check(!approval::SameSecret(live, live.substr(1)) && !approval::SameSecret(live.substr(1), live) && !approval::SameSecret(live, ""),
          "compare: a different length is another state");
    // and the same through the gate: an answer whose state differs in one place only is a stray, in every place
    Slot slot;
    const auto view = slot.Begin(Slot::Params{std::string(43, '2'), "k", At(0), 1}, CountingRandom());
    for (std::size_t at = 0; at < view->state.size(); ++at) {
      authlink::Return link;
      link.kind = authlink::Kind::Sealed;
      link.reject = authlink::Reject::None;
      link.state = view->state;
      link.state[at] = link.state[at] == 'A' ? 'B' : 'A';
      const approval::Decision decision = slot.Check(link, At(1), Context{false, 1});
      Check(decision.verdict == Verdict::Stray && decision.why == Why::WrongState,
            "gate: a state that differs only at place " + std::to_string(at) + " is a stray");
      Check(!slot.Consume(link.state), "gate: and Consume refuses it at place " + std::to_string(at));
    }
    Check(slot.Live(), "gate: the attempt is still alive after all of them");
  }

  // every base64url character is a state character, in every place, and nothing else is
  for (const char c : kBase64Url) {
    for (const std::size_t at : {std::size_t{0}, std::size_t{21}, std::size_t{42}}) {
      Slot::View view;
      view.state = std::string(43, 'x');
      view.state[at] = c;
      view.publicKey = std::string(43, '2');
      view.startedUnix = 1790000000;
      Check(!approval::BuildRequestUrl("https://example.test/login-app", view).empty(),
            std::string("alphabet: '") + c + "' is a state character at place " + std::to_string(at));
    }
  }
  for (int byte = 1; byte < 256; ++byte) {
    const char c = static_cast<char>(byte);
    if (kBase64Url.find(c) != std::string::npos) continue;
    Slot::View view;
    view.state = std::string(43, 'x');
    view.state[7] = c;
    view.publicKey = std::string(43, '2');
    view.startedUnix = 1790000000;
    Check(approval::BuildRequestUrl("https://example.test/login-app", view).empty(), "alphabet: byte " + std::to_string(byte) + " is no state character");
  }

  // every base58 character is a key character, in every place of a 43 and of a 44 character key, and nothing else is
  for (const char c : kBase58) {
    for (const std::size_t length : {std::size_t{43}, std::size_t{44}}) {
      for (const std::size_t at : {std::size_t{0}, length / 2, length - 1}) {
        std::string key(length, '2');
        key[at] = c;
        const auto pair = approval::FreshKeyPair([&] { return std::optional<approval::KeyPair>(approval::KeyPair{key, "s"}); });
        Check(pair && pair->publicKey == key, std::string("alphabet: '") + c + "' is a key character at place " + std::to_string(at) + " of " + std::to_string(length));
        Slot::View view;
        view.state = std::string(43, 'x');
        view.publicKey = key;
        view.startedUnix = 1790000000;
        Check(!approval::BuildRequestUrl("https://example.test/login-app", view).empty(), std::string("alphabet: the request takes '") + c + "' in a key");
      }
    }
  }
  for (int byte = 1; byte < 256; ++byte) {
    const char c = static_cast<char>(byte);
    if (kBase58.find(c) != std::string::npos) continue;
    std::string key(43, '2');
    key[10] = c;
    Check(!approval::FreshKeyPair([&] { return std::optional<approval::KeyPair>(approval::KeyPair{key, "s"}); }),
          "alphabet: byte " + std::to_string(byte) + " is no key character");
  }

  // an attempt that ends leaves nothing readable behind it: the buffers its secrets were in are zero
  {
    std::string secret(40, 'k');
    const char* buffer = secret.data();
    approval::Wipe(secret);
    bool zero = secret.empty();
    ReadableAfterClear(buffer, 40);
    for (std::size_t i = 0; i < 40; ++i) zero = zero && buffer[i] == 0;  // clear() keeps the buffer, so it can be looked at
    Check(zero, "wipe: a string is overwritten, then emptied");
    std::vector<std::uint8_t> bytes(40, 0x7f);
    const std::uint8_t* raw = bytes.data();
    approval::Wipe(bytes);
    bool zeroBytes = bytes.empty();
    ReadableAfterClear(raw, 40);
    for (std::size_t i = 0; i < 40; ++i) zeroBytes = zeroBytes && raw[i] == 0;
    Check(zeroBytes, "wipe: so is a byte vector");
    Slot slot;
    slot.Begin(Slot::Params{std::string(43, '2'), std::string(44, 'K'), At(0), 1}, CountingRandom());
    const std::string_view held = slot.SecretKey();
    const char* where = held.data();
    const std::size_t size = held.size();
    Check(size == 44, "wipe: the secret key is held while the attempt lives");
    slot.Clear(ClearReason::Cancel);
    bool gone = slot.SecretKey().empty();
    ReadableAfterClear(where, size);
    for (std::size_t i = 0; i < size; ++i) gone = gone && where[i] == 0;
    Check(gone, "wipe: the secret key's buffer is zero once the attempt has ended");
  }
}

}  // namespace

int main() {
  TestTheStateIsFreshAndBase64Url();
  TestOneAttemptAtATime();
  TestASecondClickReopensOnlyWhileThePageWouldAcceptTheRequest();
  TestTheGate();
  TestExpiryOnBothClocks();
  TestAnotherFlowEndsTheAttempt();
  TestTheFlowTheAttemptBelongsTo();
  TestEveryReasonEndsTheAttempt();
  TestTheTokensAreClosed();
  TestAFreshKeyPairIsOneThePageAccepts();
  TestTheRequestIsExactlyWhatThePageExpects();
  TestTheRequestOfVersionTwoNamesTheNetwork();
  TestARequestThatNamesNoValidNetworkIsRefused();
  TestAnAttemptCarriesTheNetworkItStartedIn();
  TestANetworkChangeEndsAnAttemptForAnotherNetwork();
  TestTheStateIsRfc4648Base64UrlWithoutPadding();
  TestTheNeutralLineIsOnePerHalfAMinute();
  TestTheComparisonsTheAlphabetsAndTheWipeAreWhole();
  std::cout << (gCases - gFailures) << "/" << gCases << " ur.io approval checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
