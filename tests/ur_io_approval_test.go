// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The attempt behind "Continue in browser" (App/UrIoApproval.h) as a C++ spec:
// the state and the one-attempt slot, the gate's decision for every kind of link
// in every state of the attempt, the two clocks that end it, the key pair the page
// accepts, the request the page expects and the neutral line a stray earns. Each
// negative control puts one defect back into a copy of the header and requires
// the spec to fail naming it. How AppController uses the model is in
// ur_io_approval_wiring_test.go.

// Compile the spec (app/tools/ur-io-approval-tests.cpp) against App/UrIoApproval.h
// and the headers it includes (App/AuthLink.h, App/ApprovalNetwork.h). `mutate`, when
// set, rewrites a copy of UrIoApproval.h for a negative control.
func urIoApprovalTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("ur.io approval tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, name := range []string{"ApprovalNetwork.h", "AuthLink.h", "UrIoApproval.h"} {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", name))
		if err != nil {
			t.Fatal(err)
		}
		content := lf(string(source))
		if mutate != nil && name == "UrIoApproval.h" {
			changed := mutate(content)
			if changed == content {
				t.Fatal("negative control did not change the production UrIoApproval.h")
			}
			content = changed
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "ur-io-approval-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "ur-io-approval-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build ur.io approval tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec.
func TestUrIoApprovalAttemptModel(t *testing.T) {
	root := repositoryRoot(t)
	project := readAppSource(t, "App.vcxproj")
	if !strings.Contains(project, `<ClInclude Include="UrIoApproval.h" />`) {
		t.Error("App.vcxproj does not list UrIoApproval.h")
	}
	if _, err := os.Stat(filepath.Join(root, "app", "tools", "ur-io-approval-tests.cpp")); err != nil {
		t.Fatal(err)
	}
	program := urIoApprovalTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("ur.io approval: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The model is pure: it takes nothing from Windows, WinRT or the sdk, and its random
// source, key pair and unseal are handed in. It draws no randomness of its own, has no
// check code (the contract reserves the formula; the app neither computes nor shows
// it) and writes its request without a percent-encoder.
func TestUrIoApprovalHeaderIsPureAndHasNoCheckCode(t *testing.T) {
	source := lf(readAppSource(t, "UrIoApproval.h"))
	allowed := map[string]bool{
		"<cassert>": true, "<chrono>": true, "<cstddef>": true, "<cstdint>": true, "<functional>": true,
		"<optional>": true, "<string>": true, "<string_view>": true, "<thread>": true, "<utility>": true, "<vector>": true,
		`"ApprovalNetwork.h"`: true, `"AuthLink.h"`: true,
	}
	includes := regexp.MustCompile(`(?m)^[ \t]*#[ \t]*include[ \t]*([<"][^>"]+[>"])`).FindAllStringSubmatch(source, -1)
	if len(includes) == 0 {
		t.Fatal("UrIoApproval.h includes nothing")
	}
	for _, include := range includes {
		if !allowed[include[1]] {
			t.Errorf("UrIoApproval.h includes %s: the model is standard C++ and AuthLink.h, so it runs on any host", include[1])
		}
	}
	code := stripComments(source)
	for _, forbidden := range []string{
		"winrt::", "windows.h", "urnet::", "urnetwork_sdk", "std::format", "LogInfo", "LogWarn", "LogError", "LogDebug",
		"BCrypt", "rand(", "mt19937", "random_device", "UrlQuery", "PercentEncode", "PercentDecode",
	} {
		if strings.Contains(code, forbidden) {
			t.Errorf("UrIoApproval.h mentions %q: the model is pure, draws its randomness from what it is handed and encodes nothing", forbidden)
		}
	}
	// no check code, in any file of the app: the formula is the contract's to reserve, so no
	// function, constant or string of the app carries it (a comment may say there is none)
	for name, text := range authLinkSourceFiles(t) {
		for _, forbidden := range []string{"CheckCode", "checkCode", "check_code", "XJ4BNP", "check code", "Check code"} {
			if strings.Contains(text, forbidden) {
				t.Errorf("%s mentions %q: this app has no check code for the approval", name, forbidden)
			}
		}
	}
}

// The log tokens of the model are written so that none is the id of a string
// resource: the catalog tests read every narrow literal of the app spelled like a
// key id, and a token that is one ("none", "cancel", "unknown") would be counted as a
// lookup the store has to tag windows.
func TestUrIoApprovalTokensAreNotStringIds(t *testing.T) {
	document := parseXML(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "Strings", "en", "Resources.resw"))
	catalog := map[string]bool{}
	for _, node := range document.descendants("", "data") {
		if name, ok := node.attribute("name"); ok {
			catalog[strings.SplitN(name, ".", 2)[0]] = true
		}
	}
	if len(catalog) < 1000 {
		t.Fatalf("Strings/en/Resources.resw has %d keys", len(catalog))
	}
	code := stripComments(lf(readAppSource(t, "UrIoApproval.h")))
	tokens := regexp.MustCompile(`return "([^"]*)";`).FindAllStringSubmatch(code, -1)
	if len(tokens) < 20 {
		t.Fatalf("read %d log tokens of UrIoApproval.h; update this contract", len(tokens))
	}
	for _, token := range tokens {
		if catalog[token[1]] {
			t.Errorf("the log token %q is the id of a string resource: rename it (a hyphen is enough), or the catalog tests count it as a lookup", token[1])
		}
	}
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireUrIoApprovalFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := urIoApprovalTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old replaced; a control whose text no
// longer matches fails rather than passing vacuously.
func urIoApprovalReplace(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("negative control expects exactly one %q in UrIoApproval.h", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// Each defect below is one an attempt model tends to have. Put it back, and the
// spec must fail naming the check it breaks.
func TestUrIoApprovalSpecDetectsEachDefectPutBack(t *testing.T) {
	cases := []struct {
		name string
		old  string
		new  string
		want string
	}{
		{
			name: "a replay is accepted: consuming leaves the attempt alive",
			old:  "    Clear(ClearReason::Consumed);\n    return true;",
			new:  "    return true;",
			want: "Consume ends the attempt",
		},
		{
			name: "any inner state consumes the attempt",
			old:  "if (!live_ || !SameSecret(state, state_)) return false;",
			new:  "if (!live_) return false;\n    (void)state;",
			want: "an inner state that differs consumes nothing",
		},
		{
			name: "the deadline is the steady clock alone",
			old:  "    if (now.wall - begun_.wall >= kLife) return true;     // the machine slept through it\n",
			new:  "",
			want: "the machine slept through it",
		},
		{
			name: "the deadline is the wall clock alone",
			old:  "    if (now.steady - begun_.steady >= kLife) return true;\n",
			new:  "",
			want: "the clock was set back",
		},
		{
			name: "a wall clock moved back lengthens the attempt",
			old:  "    if (begun_.wall - now.wall > kWallSkew) return true;  // the wall clock went back\n",
			new:  "",
			want: "a wall clock moved back two minutes ends it",
		},
		{
			name: "a wall clock moved back a minute already ends it",
			old:  "if (begun_.wall - now.wall > kWallSkew) return true;",
			new:  "if (begun_.wall - now.wall >= kWallSkew) return true;",
			want: "nor one of exactly a minute",
		},
		{
			name: "an attempt lives six minutes",
			old:  "inline constexpr std::chrono::seconds kLife{300};",
			new:  "inline constexpr std::chrono::seconds kLife{360};",
			want: "300 s is past the deadline",
		},
		{
			name: "a stray after the deadline leaves the dead attempt",
			old:  "    if (ExpiredAt(now)) {\n      Clear(ClearReason::TimedOut);\n      return {Verdict::Stray, Why::Expired};\n    }",
			new:  "    if (ExpiredAt(now)) return {Verdict::Stray, Why::Expired};",
			want: "the dead attempt is wiped",
		},
		{
			name: "a wrong state kills the attempt",
			old:  "if (!SameSecret(link.state, state_)) return {Verdict::Stray, Why::WrongState};",
			new:  "if (!SameSecret(link.state, state_)) { Clear(ClearReason::Cancel); return {Verdict::Stray, Why::WrongState}; }",
			want: "the live attempt is untouched by all of them",
		},
		{
			name: "a link that is no answer kills the attempt",
			old:  "if (link.kind == authlink::Kind::Invalid) return {Verdict::Stray, Why::InvalidShape};",
			new:  "if (link.kind == authlink::Kind::Invalid) { Clear(ClearReason::Cancel); return {Verdict::Stray, Why::InvalidShape}; }",
			want: "the live attempt is untouched by all of them",
		},
		{
			name: "signed in does not stop the gate",
			old:  "    if (context.signedIn) return {Verdict::Ignored, Why::SignedIn};\n",
			new:  "",
			want: "signed in is Ignored even for the live state",
		},
		{
			name: "a denial leaves the attempt alive",
			old:  "      Clear(ClearReason::Denied);\n      return {Verdict::Denied, Why::None};",
			new:  "      return {Verdict::Denied, Why::None};",
			want: "access_denied for the live state is Denied, and consumes the attempt",
		},
		{
			name: "an approval is consumed by the check, before its box has opened",
			old:  "    return {Verdict::Accepted, Why::None};\n  }",
			new:  "    Clear(ClearReason::Consumed);\n    return {Verdict::Accepted, Why::None};\n  }",
			want: "Accepted does not consume it",
		},
		{
			name: "another flow does not end the attempt",
			old:  "    if (context.flow != flow_) {",
			new:  "    if (false) {",
			want: "a later flow ends the attempt",
		},
		{
			name: "a failed draw keeps the attempt it was to replace",
			old:  "    Clear(ClearReason::Begin);\n    unsigned char raw",
			new:  "    unsigned char raw",
			want: "a failed draw still ends the attempt it was to replace",
		},
		{
			name: "the state is standard base64, not the url alphabet",
			old:  `"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";`,
			new:  `"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";`,
			want: "32 bytes of 0xFF are 42 '_'",
		},
		{
			name: "a public key of 42 characters is taken",
			old:  "inline constexpr std::size_t kMinPublicKeyChars = 43;",
			new:  "inline constexpr std::size_t kMinPublicKeyChars = 42;",
			want: "42 and 45 are not",
		},
		{
			name: "a public key of 45 characters is taken",
			old:  "inline constexpr std::size_t kMaxPublicKeyChars = 44;",
			new:  "inline constexpr std::size_t kMaxPublicKeyChars = 45;",
			want: "42 and 45 are not",
		},
		{
			name: "a public key outside base58 is taken",
			old:  "    for (const char c : pair->publicKey) usable = usable && detail::IsBase58Char(c);\n",
			new:  "",
			want: "is not base58",
		},
		{
			name: "the key pair is drawn without a bound",
			old:  "inline constexpr int kKeyPairTries = 8;",
			new:  "inline constexpr int kKeyPairTries = 100;",
			want: "no ninth try",
		},
		{
			name: "a failing sdk is not tried again",
			old:  "    if (!pair) continue;\n",
			new:  "    if (!pair) return std::nullopt;\n",
			want: "the sdk failing three times in a row is tried again",
		},
		{
			name: "the request keeps the escapes of a state that needs one",
			old:  "      if (!detail::IsBase64UrlChar(c)) return false;\n",
			new:  "      (void)c;\n",
			want: "(it would need an escape, and nothing is escaped)",
		},
		{
			name: "the request puts the second before the key",
			old:  `  url += "&pk=";` + "\n" + `  url += view.publicKey;` + "\n" + `  url += "&ts=";` + "\n" + `  url += ts;` + "\n",
			new:  `  url += "&ts=";` + "\n" + `  url += ts;` + "\n" + `  url += "&pk=";` + "\n" + `  url += view.publicKey;` + "\n",
			want: "in that order",
		},
		{
			name: "the request takes a second of nine digits",
			old:  "bool tsOk = ts.size() == 10;",
			new:  "bool tsOk = ts.size() >= 9;",
			want: "a 9-digit second",
		},
		{
			name: "the request takes a key outside base58",
			old:  "  for (const char c : view.publicKey) publicKeyOk = publicKeyOk && detail::IsBase58Char(c);\n",
			new:  "",
			want: "a key with '0'",
		},
		{
			name: "the request takes a page url with a fragment of its own",
			old:  "byte < 0x7F && c != '#' && c != '%';",
			new:  "byte < 0x7F && c != '%';",
			want: "a page url with a fragment of its own",
		},
		{
			name: "the neutral line is shown every time",
			old:  "    if (shown_ && now - last_ < kNeutralEvery) return false;\n",
			new:  "",
			want: "not again inside 30 s",
		},
		{
			name: "a refused neutral line moves the window",
			old:  "    if (shown_ && now - last_ < kNeutralEvery) return false;\n    shown_ = true;\n    last_ = now;\n    return true;",
			new:  "    const bool refuse = shown_ && now - last_ < kNeutralEvery;\n    shown_ = true;\n    last_ = now;\n    return !refuse;",
			want: "a refused ask does not move the window",
		},
		{
			name: "a token other than the one the log checks read",
			old:  `case Why::NoAttempt: return "no-attempt";`,
			new:  `case Why::NoAttempt: return "no-live-attempt";`,
			want: "the words the owner's log checks read",
		},
		{
			// the state is compared at every place: an answer that differs from the attempt's only in its first character is another's
			name: "the states are compared from the second character",
			old:  "  unsigned char difference = 0;\n  for (std::size_t i = 0; i < a.size(); ++i) {",
			new:  "  unsigned char difference = 0;\n  for (std::size_t i = 1; i < a.size(); ++i) {",
			want: "a difference at place 0 is another state",
		},
		{
			name: "a state that only starts like the attempt's is the attempt's",
			old:  "  if (a.size() != b.size()) return false;\n  unsigned char difference = 0;",
			new:  "  if (a.size() > b.size()) return false;\n  unsigned char difference = 0;",
			want: "a different length is another state",
		},
		{
			// (emptying a string writes its first byte anyway, so it is the last byte that shows the overwrite)
			name: "a wiped string keeps its last byte",
			old:  "for (std::size_t i = 0; bytes != nullptr && i < text.size(); ++i) bytes[i] = 0;",
			new:  "for (std::size_t i = 0; bytes != nullptr && i + 1 < text.size(); ++i) bytes[i] = 0;",
			want: "wipe: a string is overwritten, then emptied",
		},
		{
			name: "a wiped byte vector keeps its first byte",
			old:  "for (std::size_t i = 0; bytes != nullptr && i < data.size(); ++i) bytes[i] = 0;",
			new:  "for (std::size_t i = 1; bytes != nullptr && i < data.size(); ++i) bytes[i] = 0;",
			want: "wipe: so is a byte vector",
		},
		{
			name: "a wiped string is overwritten but not emptied",
			old:  "  text.clear();\n}",
			new:  "}",
			want: "wipe: a string is overwritten, then emptied",
		},
		{
			name: "an attempt that ends keeps its secret key",
			old:  "    Wipe(secretKey_);\n    Wipe(api_);\n    live_ = false;",
			new:  "    Wipe(api_);\n    live_ = false;",
			want: "wipe: the secret key's buffer is zero once the attempt has ended",
		},
		{
			// the model takes the parser's alphabets, and its spec sweeps every character of both
			name: "the model's base58 stops short of the digit 9",
			old:  "using authlink::detail::IsBase58Char;",
			new:  "inline bool IsBase58Char(char c) { return authlink::detail::IsBase58Char(c) && c != '9'; }",
			want: "alphabet: '9' is a key character",
		},
		{
			name: "the model's base58 stops short of the capital A",
			old:  "using authlink::detail::IsBase58Char;",
			new:  "inline bool IsBase58Char(char c) { return authlink::detail::IsBase58Char(c) && c != 'A'; }",
			want: "alphabet: 'A' is a key character",
		},
		{
			name: "the model's base58 takes the lookalike 0",
			old:  "using authlink::detail::IsBase58Char;",
			new:  "inline bool IsBase58Char(char c) { return authlink::detail::IsBase58Char(c) || c == '0'; }",
			want: "alphabet: byte 48 is no key character",
		},
		{
			name: "the model's base64url stops short of the capital Z",
			old:  "using authlink::detail::IsBase64UrlChar;",
			new:  "inline bool IsBase64UrlChar(char c) { return authlink::detail::IsBase64UrlChar(c) && c != 'Z'; }",
			want: "alphabet: 'Z' is a state character at place 0",
		},
		{
			name: "the model's base64url takes the padding",
			old:  "using authlink::detail::IsBase64UrlChar;",
			new:  "inline bool IsBase64UrlChar(char c) { return authlink::detail::IsBase64UrlChar(c) || c == '='; }",
			want: "alphabet: byte 61 is no state character",
		},
		{
			// a server change takes the next flow number and pushes no sign-in state, so the caller compares the number the attempt
			// began in with the last one: a slot that forgets it, or reports it after the attempt has ended, makes a dead attempt look live
			name: "a finished attempt still reports the flow it began in",
			old:  "return live_ ? flow_ : 0;",
			new:  "return flow_;",
			want: "flow: nothing is left once the attempt has ended",
		},
		{
			name: "the flow an attempt began in is not kept",
			old:  "    flow_ = params.flow;\n",
			new:  "",
			want: "flow: the attempt carries the number of the flow it began in",
		},
		{
			// the state is 32 bytes, so only other lengths show whether the tail of the encoder reads what it was not given
			name: "the encoder reads a byte it was not given",
			old:  "(i + 1 < length ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0u)",
			new:  "(i + 1 <= length ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0u)",
			want: "base64url: 'f' with other bytes after it in memory is the same text",
		},
		{
			name: "the url alphabet's last two characters are swapped",
			old:  `"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";`,
			new:  `"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";`,
			want: "base64url: '-' and '_' stand for 62 and 63, not '+' and '/'",
		},
		{
			name: "a page url of exactly 200 characters is refused",
			old:  "pageUrl.size() <= 200",
			new:  "pageUrl.size() < 200",
			want: "request: a page url of exactly 200 characters is taken",
		},
		{
			name: "a page url may hold DEL",
			old:  "byte < 0x7F",
			new:  "byte <= 0x7F",
			want: "a page url with DEL in it",
		},
		{
			// the digits of the second are checked beside its length: a minus sign and nine digits are ten characters
			name: "the request takes a second that has a minus sign in it",
			old:  "  for (const char c : ts) tsOk = tsOk && c >= '0' && c <= '9';\n",
			new:  "",
			want: "a negative second of ten characters",
		},
		{
			// the page refuses a request more than 240 s old; a second click opens the same one again only well inside that
			name: "a second click opens the same request again until the page's own limit",
			old:  "inline constexpr std::chrono::seconds kReopenWindow{200};",
			new:  "inline constexpr std::chrono::seconds kReopenWindow{240};",
			want: "reopen: the window is 200 s",
		},
		{
			name: "the steady clock does not end the window for a second click",
			old:  "return live_ && !ExpiredAt(now) && now.steady - begun_.steady < kReopenWindow &&\n           now.wall - begun_.wall < kReopenWindow;",
			new:  "return live_ && !ExpiredAt(now) && now.wall - begun_.wall < kReopenWindow;",
			want: "reopen: the steady clock decides",
		},
		{
			name: "the wall clock does not end the window for a second click",
			old:  "return live_ && !ExpiredAt(now) && now.steady - begun_.steady < kReopenWindow &&\n           now.wall - begun_.wall < kReopenWindow;",
			new:  "return live_ && !ExpiredAt(now) && now.steady - begun_.steady < kReopenWindow;",
			want: "reopen: and the wall clock, which counts a sleep the steady clock does not",
		},
		{
			name: "a second click reaches one second past the window",
			old:  "now.steady - begun_.steady < kReopenWindow &&\n           now.wall - begun_.wall < kReopenWindow;",
			new:  "now.steady - begun_.steady <= kReopenWindow &&\n           now.wall - begun_.wall <= kReopenWindow;",
			want: "reopen: not at 200 s",
		},
		{
			name: "an attempt the clocks have ended is opened again",
			old:  "return live_ && !ExpiredAt(now) && now.steady",
			new:  "return live_ && now.steady",
			want: "reopen: one of two minutes does",
		},
		{
			name: "an attempt that has ended is opened again",
			old:  "return live_ && !ExpiredAt(now) && now.steady",
			new:  "return !ExpiredAt(now) && now.steady",
			want: "reopen: an attempt that ended (cancelled) is not opened again",
		},
		{
			// version 2 (contract section 11): the request names its network, and the version follows the name
			name: "a request that names a network is still version 1",
			old:  `url += named ? "#v=2&platform=windows&state=" : "#v=1&platform=windows&state=";`,
			new:  `url += "#v=1&platform=windows&state=";`,
			want: "request v2: the shared vector, byte for byte",
		},
		{
			name: "the official network's request is version 2",
			old:  `url += named ? "#v=2&platform=windows&state=" : "#v=1&platform=windows&state=";`,
			new:  `url += "#v=2&platform=windows&state=";`,
			want: "request v1: a view that names no network is the official network's request",
		},
		{
			name: "the request does not name the network",
			old:  "  if (named) {\n    url += \"&api=\";\n    url += view.api;\n  }\n",
			new:  "",
			want: "request v2: the shared vector, byte for byte",
		},
		{
			name: "the network comes before the second",
			old:  "  url += \"&ts=\";\n  url += ts;\n  if (named) {\n    url += \"&api=\";\n    url += view.api;\n  }\n  return url;",
			new:  "  if (named) {\n    url += \"&api=\";\n    url += view.api;\n  }\n  url += \"&ts=\";\n  url += ts;\n  return url;",
			want: "request v2: v, platform, state, pk, ts and api, in that order",
		},
		{
			name: "the network is named with the wrong key",
			old:  `url += "&api=";`,
			new:  `url += "&host=";`,
			want: "request v2: the shared vector, byte for byte",
		},
		{
			name: "a name outside the contract's grammar is put in a request",
			old:  " || (named && !IsValidApiHost(view.api))",
			new:  "",
			want: "request v2: the name 'API.beta-test.net' fails closed",
		},
		{
			name: "every request is taken for one that names a network",
			old:  "const bool named = !view.api.empty();",
			new:  "const bool named = true;",
			want: "request: the page url, then v, platform, state, pk and ts, in that order, and nothing else",
		},
		{
			name: "no request is taken for one that names a network",
			old:  "const bool named = !view.api.empty();",
			new:  "const bool named = false;",
			want: "request v2: the shared vector, byte for byte",
		},
		{
			// the attempt keeps the network it started in, and a second click re-opens the very same request
			name: "an attempt does not keep the network it started in",
			old:  "    api_ = std::move(params.api);\n",
			new:  "",
			want: "attempt: the view carries the network the attempt started in",
		},
		{
			name: "a view does not carry the network of its attempt",
			old:  "begun_.steady + kLife, api_};",
			new:  "begun_.steady + kLife};",
			want: "attempt: the view carries the network the attempt started in",
		},
		{
			name: "an attempt starts out naming a network",
			old:  "    std::string api{};  // as Params::api: empty means a version 1 request",
			new:  "    std::string api = \"api.example.com\";  // as Params::api: empty means a version 1 request",
			want: "defaults: an attempt names no network until it is told to",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			requireUrIoApprovalFailure(t, func(source string) string {
				return urIoApprovalReplace(t, source, c.old, c.new)
			}, c.want)
		})
	}
}
