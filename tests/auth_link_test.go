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

// The urnetwork://auth return (App/AuthLink.h) as a C++ spec: which links the
// strict parser accepts, why it refuses every other, which links route to the
// auth gate, where a command line holds a link, and what the log gets instead of
// a link. Each negative control puts one defect back into a copy of the header
// and requires the spec to fail naming it. How AppController uses the parser is
// in auth_link_wiring_test.go.

// Compile the spec (app/tools/auth-link-tests.cpp) against App/AuthLink.h.
// `mutate`, when set, rewrites a copy of the header for a negative control.
func authLinkTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("auth link tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "AuthLink.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := lf(string(source))
	if mutate != nil {
		content = mutate(content)
		if content == lf(string(source)) {
			t.Fatal("negative control did not change the production AuthLink.h")
		}
	}
	includeDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(includeDir, "AuthLink.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "auth-link-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "auth-link-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build auth link tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: the two answers the page sends (sealed and denied), with and
// without the browser's '/' after the host; the page's own conformance vector;
// every character of the base64url and base58 alphabets in every place of a
// value, and every other printable character refused; a percent sign refused
// wherever it stands, every byte as an escape among them; every other link
// refused for a named reason; one hostile character put in every place of every
// accepted link; the other contracts the parser can read; the routing, whatever
// the case of the scheme and the host; where a command line holds a link; the
// closed log tokens; and the redaction of a url and of a command line.
func TestAuthLinkParserAndRedaction(t *testing.T) {
	program := authLinkTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("auth link: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The header stays pure: the shared Go test compiles it with any C++20 compiler
// and the app includes it from the pch'd sources and from Startup.cpp, so it
// takes nothing from Windows, WinRT, the sdk or <format>.
func TestAuthLinkHeaderIsPlainStandardCpp(t *testing.T) {
	source := lf(readAppSource(t, "AuthLink.h"))
	allowed := map[string]bool{
		"<cstddef>": true, "<string>": true, "<string_view>": true, "<utility>": true, "<vector>": true,
	}
	includes := regexp.MustCompile(`(?m)^[ \t]*#[ \t]*include[ \t]*([<"][^>"]+[>"])`).FindAllStringSubmatch(source, -1)
	if len(includes) == 0 {
		t.Fatal("AuthLink.h includes nothing; it needs the standard string types")
	}
	for _, include := range includes {
		if !allowed[include[1]] {
			t.Errorf("AuthLink.h includes %s: only %s are allowed, so the header runs on any host", include[1], "<cstddef>, <string>, <string_view>, <utility> and <vector>")
		}
	}
	code := stripComments(source)
	for _, forbidden := range []string{"winrt::", "windows.h", "urnet::", "urnetwork_sdk", "std::format", "LogInfo", "LogWarn", "LogError", "LogDebug"} {
		if strings.Contains(code, forbidden) {
			t.Errorf("AuthLink.h mentions %q: the parser is pure, and nothing in it may log", forbidden)
		}
	}
}

// The parser reads two shapes and decodes nothing. A decoder, a plain-code shape or a
// reject token for a malformed escape coming back is a regression to the stateless
// reader the approval flow replaced: it read a code out of any link it was given.
func TestAuthLinkHasNoDecoderAndNoPlainShape(t *testing.T) {
	code := stripComments(lf(readAppSource(t, "AuthLink.h")))
	for _, gone := range []string{
		"Kind::Plain", "Plain,", "StrictDecode", "Hex(", "PercentDecode", "BadEscape", "bad-escape", "PlaceMismatch",
		"Place::", "hasCode", "IsAuthCode", "out.code", "std::string code;", "sawEscape",
	} {
		if strings.Contains(code, gone) {
			t.Errorf("AuthLink.h still has %q: the parser reads sealed and denied answers only, and decodes nothing", gone)
		}
	}
	for _, kept := range []string{
		"Sealed,", "Denied,", "UnexpectedEscape", `"unexpected-escape"`,
		`if (url.find('%') != std::string_view::npos) return fail(Reject::UnexpectedEscape);`,
	} {
		if !strings.Contains(code, kept) {
			t.Errorf("AuthLink.h is missing %q", kept)
		}
	}
	if count := strings.Count(code, "UnexpectedEscape"); count != 3 {
		t.Errorf("AuthLink.h names UnexpectedEscape %d times; want the enumerator, its token and the one place a '%%' is refused", count)
	}
	// every reject token is hyphenated, so none can ever be the id of a string resource
	for _, token := range regexp.MustCompile(`case Reject::\w+: return "([^"]*)";`).FindAllStringSubmatch(code, -1) {
		if !strings.Contains(token[1], "-") {
			t.Errorf("the reject token %q has no hyphen, so it could be the id of a string resource", token[1])
		}
	}
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireAuthLinkFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := authLinkTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old replaced; a control whose text no
// longer matches fails rather than passing vacuously.
func authLinkReplace(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("negative control expects exactly one %q in AuthLink.h", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// The one line of AuthLink.h that refuses a percent sign.
const authLinkPercentCheck = `if (url.find('%') != std::string_view::npos) return fail(Reject::UnexpectedEscape);`

// Each defect below is one a hand-written reader of a link tends to have. Put
// it back, and the spec must fail naming the check it breaks.
func TestAuthLinkSpecDetectsEachDefectPutBack(t *testing.T) {
	cases := []struct {
		name string
		old  string
		new  string
		want string
		// further edits of the same copy, when one defect takes two lines
		also []authLinkEdit
	}{
		{
			// the path after the host is skipped on the way to the query
			name: "a path is accepted",
			old:  "strippedSlash = true;",
			new:  "strippedSlash = true; if (const std::size_t cut = rest.find('?'); cut != std::string_view::npos) rest.remove_prefix(cut);",
			want: "a path",
		},
		{
			name: "the first of two equal keys is enough",
			old:  "if (*seen) return fail(Reject::Duplicate);",
			new:  "",
			want: "a key twice, even with the same value",
		},
		{
			name: "a percent sign is not refused",
			old:  authLinkPercentCheck,
			new:  "",
			want: "an escaped character in the state of a sealed link",
		},
		{
			// the escapes are looked for in the first part of the link only
			name: "a percent sign is refused only early in the link",
			old:  authLinkPercentCheck,
			new:  `if (url.substr(0, 100).find('%') != std::string_view::npos) return fail(Reject::UnexpectedEscape);`,
			want: "an escaped character in the sealed data",
		},
		{
			// only a malformed escape is refused, as a decoder that keeps the good ones would
			name: "only a malformed escape is refused",
			old:  authLinkPercentCheck,
			new: `for (std::size_t at = url.find('%'); at != std::string_view::npos; at = url.find('%', at + 1)) {
    const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); };
    if (at + 2 >= url.size() || !hex(url[at + 1]) || !hex(url[at + 2])) return fail(Reject::UnexpectedEscape);
  }`,
			want: "an escaped character in the state of a sealed link",
		},
		{
			// only an escape in capital hex is refused
			name: "an escape in lower-case hex slips through",
			old:  authLinkPercentCheck,
			new: `for (std::size_t at = url.find('%'); at != std::string_view::npos; at = url.find('%', at + 1)) {
    if (at + 2 < url.size() && !(url[at + 1] >= 'a' && url[at + 1] <= 'f') && !(url[at + 2] >= 'a' && url[at + 2] <= 'f')) return fail(Reject::UnexpectedEscape);
  }`,
			want: "escapes: every one of",
		},
		{
			name: "a fragment is tolerated after the query",
			old:  `if (rest.find_first_of("#?") != std::string_view::npos) return fail(Reject::Delimiter);`,
			new:  `if (rest.find('?') != std::string_view::npos) return fail(Reject::Delimiter);`,
			want: "a fragment after the query",
		},
		{
			// the percent sign is let through, and a decoder-less reader still takes %73tate for state
			name: "a key is an alias of its percent-encoded self",
			old:  `if (key == "state") {`,
			new:  `if (key == "state" || key == "%73tate") {`,
			want: "a percent-encoded key is not an alias of state",
			also: []authLinkEdit{{authLinkPercentCheck, ""}},
		},
		{
			name: "the plain code is read again",
			old:  `else if (key == "error") { seen = &hasError; target = &error; }`,
			new:  `else if (key == "error" || key == "code") { seen = &hasError; target = &error; }`,
			want: "a code in the clear beside the state",
		},
		{
			name: "the length of the state is not checked",
			old:  "if (out.state.size() != kStateChars) return fail(Reject::BadState);",
			new:  "",
			want: "a 42-character state",
		},
		{
			name: "any error value is a denial",
			old:  "if (error != kDeniedValue) return fail(Reject::BadError);",
			new:  "(void)kDeniedValue;",
			want: "the error value is exact",
		},
		{
			name: "the error beside a sealed answer is tolerated",
			old:  "if (hasEpk && hasNonce && hasData && !hasError) {",
			new:  "if (hasEpk && hasNonce && hasData) {",
			want: "an error beside a whole sealed answer",
		},
		{
			name: "any host that starts with auth is the auth host",
			old:  "return next == '?' || next == '/' || next == '#';",
			new:  "return next != '\\0';",
			want: "authenticator is another host",
		},
		{
			// the routing stops looking at the case of the scheme and the host
			name: "routing compares the case of the host",
			old:  "constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }",
			new:  "constexpr char Lower(char c) { return c; }",
			want: "route: an upper-case scheme",
		},
		{
			// Parse stops being strict: a variant spelling is read as the auth host
			name: "the parser folds the case of the host",
			old:  "if (url.substr(0, contract.prefix.size()) != contract.prefix) return fail(Reject::NotAuthHost);",
			new:  "if (!detail::EqualsLower(url.substr(0, contract.prefix.size()), contract.prefix)) return fail(Reject::NotAuthHost);",
			want: "the host is lowercase only",
		},
		{
			name: "a token that can be the id of a string resource",
			old:  `case Reject::Path: return "bad-path";`,
			new:  `case Reject::Path: return "path";`,
			want: "every one is hyphenated",
		},
		{
			name: "the redaction echoes the link",
			old:  `out.append("[redacted ");`,
			new:  `out.append(url); out.append("[redacted ");`,
			want: "no value survives",
		},
		{
			name: "the redaction echoes a userinfo token as the host",
			old:  "const bool plainStop = stop == std::string_view::npos || url[stop] == '/' || url[stop] == '?' || url[stop] == '#';",
			new:  "const bool plainStop = true;",
			want: "a userinfo token never survives either",
		},
		{
			name: "the redaction looks for a lowercase scheme only",
			old:  "return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;",
			new:  "return c;",
			want: "the scheme is matched whatever its case",
		},
		{
			// the redaction waits for the slashes, so urnetwork:auth?... is logged as it is
			name: "the redaction knows only the form with the slashes",
			old:  `RedactLinks<wchar_t>(line, L"urnetwork:",`,
			new:  `RedactLinks<wchar_t>(line, L"urnetwork://",`,
			want: "urnetwork:auth?... leaves no value",
		},
		{
			// the search of a command line is case-sensitive, so a variant spelling is no link
			name: "a command line is searched for the lowercase scheme only",
			old:  `const std::size_t begin = detail::FindSchemeIgnoringCase<wchar_t>(line, L"urnetwork://", 0);`,
			new:  `const std::size_t begin = line.find(L"urnetwork://");`,
			want: "command line: an upper-case scheme is a link",
		},
		{
			// the line break after a logged link is taken for part of it
			name: "a logged link runs on past the end of its line",
			old:  `return detail::RedactLinks<char>(text, "urnetwork:", " \t\"\r\n");`,
			new:  `return detail::RedactLinks<char>(text, "urnetwork:", " \t\"");`,
			want: "a link ends at the end of its line (CRLF)",
		},
		{
			// the exported log is copied as it is
			name: "the redaction of a log changes nothing",
			old:  `return detail::RedactLinks<char>(text, "urnetwork:", " \t\"\r\n");`,
			new:  `return std::string(text);`,
			want: "log text: no value survives",
		},
		{
			// the link is not cut at the quote the shell put round it
			name: "a link runs on past its quote",
			old:  `detail::LinkEnd<wchar_t>(line, begin, L" \t\"")`,
			new:  `detail::LinkEnd<wchar_t>(line, begin, L" \t")`,
			want: "a quoted link, without its quotes",
		},
		{
			// the router looks at the character after sixteen and not at the sixteen: any host of four letters is the auth host
			name: "the routing does not compare the host, only the character after it",
			old:  `if (!detail::EqualsLower(url.substr(0, kAuthPrefix.size()), kAuthPrefix)) return false;`,
			new:  `if (url.size() < kAuthPrefix.size()) return false;`,
			want: "route: a four-letter host that is not auth is not the auth host",
		},
		{
			// the sizes are the contract's own, to the byte, and each is the only thing wrong with its link in the spec
			name: "the raw size of a link is not capped",
			old:  `if (url.size() > contract.maxUrlBytes) return fail(Reject::TooLong);`,
			new:  ``,
			want: "an address one byte past the cap is too long",
		},
		{
			name: "the number of pairs is not capped",
			old:  `if (pairs.size() > contract.maxPairs) return fail(Reject::TooLong);`,
			new:  ``,
			want: "seven pairs are",
		},
		{
			name: "the size of a value is not capped",
			old:  `if (value.size() > contract.maxValueBytes) return fail(Reject::TooLong);`,
			new:  ``,
			want: "a value one byte past the cap is too long",
		},
		{
			// Export logs runs the redaction over a log that already holds redacted lines: they are copied as they are
			name: "a link that was redacted at the source is redacted again",
			old:  `if (IsRedactedAlready(narrow, AsciiOnly<Char>(text.substr(end, 40)))) {`,
			new:  `if (false) {`,
			want: "again: a line redacted at the source is exported as it is",
		},
		{
			// only the exact text RedactUrl writes is taken for its own work: a link with a query that has such words after it is not
			name: "a link followed by the words of a redaction is taken for a redacted one",
			old:  `if (host != "*" && !IsPlainToken(host, 32)) return false;`,
			new:  `(void)host;`,
			want: "again: a link with its query, followed by such words, is still reduced",
		},
		{
			// the routing and the search compare every letter of the scheme, the first included
			name: "the comparison of the host skips its first letter",
			old:  "  for (std::size_t i = 0; i < text.size(); ++i) {\n    if (Lower(text[i]) != lower[i]) return false;",
			new:  "  for (std::size_t i = 1; i < text.size(); ++i) {\n    if (Lower(text[i]) != lower[i]) return false;",
			want: "edges: a scheme that differs from the auth host's in its first letter only is not the auth host",
		},
		{
			name: "a view shorter than the host is taken for it",
			old:  "  if (text.size() != lower.size()) return false;\n  for (std::size_t i = 0; i < text.size(); ++i) {",
			new:  "  if (text.size() != lower.size()) return true;\n  for (std::size_t i = 0; i < text.size(); ++i) {",
			want: "edges: a view shorter than the host is not the host, whatever is stored after it",
		},
		{
			name: "a public key of 64 characters is refused",
			old:  "if (!detail::IsBase58(out.epk, 64) ||",
			new:  "if (!detail::IsBase58(out.epk, 63) ||",
			want: "edges: a public key and a nonce of 64 characters are read",
		},
		{
			name: "a nonce of 65 characters is read",
			old:  "!detail::IsBase58(out.nonce, 64) ||",
			new:  "!detail::IsBase58(out.nonce, 65) ||",
			want: "edges: a nonce of 65 characters is not",
		},
		{
			name: "a host of thirty-two letters is not echoed",
			old:  "if (plainStop && detail::IsPlainToken(host, 32)) {",
			new:  "if (plainStop && detail::IsPlainToken(host, 31)) {",
			want: "edges: a host of thirty-two letters is echoed",
		},
		{
			name: "a scheme of one letter is not echoed",
			old:  "schemeEnd > 0 && detail::IsPlainToken(url.substr(0, schemeEnd), 16)",
			new:  "schemeEnd > 1 && detail::IsPlainToken(url.substr(0, schemeEnd), 16)",
			want: "edges: a scheme of one letter is echoed",
		},
		{
			name: "a host that a '#' ends is not echoed",
			old:  "url[stop] == '?' || url[stop] == '#';",
			new:  "url[stop] == '?';",
			want: "edges: a host that a '#' ends is echoed",
		},
		{
			name: "words with a host of thirty-three letters are taken for a redaction",
			old:  `if (host != "*" && !IsPlainToken(host, 32)) return false;`,
			new:  `if (host != "*" && !IsPlainToken(host, 33)) return false;`,
			want: "edges: words after a host of thirty-three letters are no redaction of ours",
		},
		{
			name: "words with no length at all are taken for a redaction",
			old:  "return at > digits && after.substr(at, kAfter.size()) == kAfter;",
			new:  "return at >= digits && after.substr(at, kAfter.size()) == kAfter;",
			want: "edges: '' is no length",
		},
		{
			name: "the window after a link is wider than the words it holds",
			old:  "AsciiOnly<Char>(text.substr(end, 40))",
			new:  "AsciiOnly<Char>(text.substr(end, 41))",
			want: "edges: the window after a link holds twenty-two digits, and no more",
		},
		{
			// the scheme of a redaction it wrote is a plain token: a link whose scheme holds a colon is none of ours
			name: "words after a link whose scheme is not a plain token are taken for a redaction",
			old:  "if (schemeEnd == std::string_view::npos || schemeEnd == 0 || !IsPlainToken(link.substr(0, schemeEnd), 16)) return false;",
			new:  "if (schemeEnd == std::string_view::npos || schemeEnd == 0) return false;",
			want: "again: a scheme RedactUrl would not echo is not one it wrote",
		},
		{
			// and its words are exactly " [redacted ", a length and " chars]": any eleven characters in the first place do not make a redaction
			name: "any eleven characters are taken for the words that open a redaction",
			old:  "  if (after.substr(0, kBefore.size()) != kBefore) return false;\n",
			new:  "",
			want: "again: words with place 10 of ' [redacted ' off are no redaction of ours",
		},
		{
			name: "the words that open a redaction are compared without their last character",
			old:  "if (after.substr(0, kBefore.size()) != kBefore) return false;",
			new:  "if (after.substr(0, kBefore.size() - 1) != kBefore.substr(0, kBefore.size() - 1)) return false;",
			want: "again: words with place 10 of ' [redacted ' off are no redaction of ours",
		},
		{
			name: "the words that close a redaction are compared without their last character",
			old:  "return at > digits && after.substr(at, kAfter.size()) == kAfter;",
			new:  "return at > digits && after.substr(at, kAfter.size() - 1) == kAfter.substr(0, kAfter.size() - 1);",
			want: "again: words with place 6 of ' chars]' off are no redaction of ours",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			requireAuthLinkFailure(t, func(source string) string {
				source = authLinkReplace(t, source, c.old, c.new)
				for _, edit := range c.also {
					source = authLinkReplace(t, source, edit.old, edit.new)
				}
				return source
			}, c.want)
		})
	}
}

// One edit of a copy of AuthLink.h for a negative control: old must be in the
// header exactly once.
type authLinkEdit struct {
	old string
	new string
}

// The lines of AuthLink.h that say which characters a value is made of.
const (
	authLinkBase64UrlLine = `return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';`
	authLinkBase58Digit   = `const bool digit = c >= '1' && c <= '9';`
	authLinkBase58Lower   = `const bool lower = (c >= 'a' && c <= 'z') && c != 'l';`
	authLinkStateLoop     = `if (!detail::IsBase64UrlChar(ch)) return fail(Reject::BadState);`
	authLinkNonceCheck    = `!detail::IsBase58(out.nonce, 64)`
)

// The spec's accepted links used to be runs of one capital letter, so a parser
// whose alphabets stopped at the capitals passed every check and then refused
// every link the page sends (no state, code or base58 value of the page's is all
// capitals). Each defect below narrows an alphabet by a class or a character, or
// widens it by one, and the spec must fail naming the check that catches it.
// Those that leave the page's own address unreadable name that check too.
func TestAuthLinkSpecDetectsANarrowedOrWidenedAlphabet(t *testing.T) {
	const (
		base64urlRead   = "alphabets: every base64url character is read in every place of a state"
		base58Read      = "alphabets: every base58 character is read in every place of a key, a nonce and a box"
		outsiderRefused = "alphabets: every printable character outside an alphabet is refused where it stands"
		pageApprove     = "page vector: the approve address is read, value for value"
	)
	cases := []struct {
		name  string
		edits []authLinkEdit
		want  []string
	}{
		{
			name: "every alphabet stops at the capital letters",
			edits: []authLinkEdit{
				{authLinkBase64UrlLine, "return (c >= 'A' && c <= 'Z');"},
				{authLinkBase58Digit, "const bool digit = false;"},
				{authLinkBase58Lower, "const bool lower = false;"},
			},
			want: []string{base64urlRead, base58Read, pageApprove},
		},
		{
			name:  "base64url has no lower case",
			edits: []authLinkEdit{{"(c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'", "(c >= '0' && c <= '9') || c == '-'"}},
			want:  []string{base64urlRead},
		},
		{
			name:  "base64url has no digits",
			edits: []authLinkEdit{{"|| (c >= '0' && c <= '9') || c == '-' || c == '_';", "|| c == '-' || c == '_';"}},
			want:  []string{base64urlRead},
		},
		{
			name:  "base64url has no '-'",
			edits: []authLinkEdit{{"(c >= '0' && c <= '9') || c == '-' || c == '_';", "(c >= '0' && c <= '9') || c == '_';"}},
			want:  []string{base64urlRead},
		},
		{
			name:  "base64url has no '_'",
			edits: []authLinkEdit{{"|| c == '-' || c == '_';", "|| c == '-';"}},
			want:  []string{base64urlRead},
		},
		{
			name:  "the state alone stops at the capital letters",
			edits: []authLinkEdit{{authLinkStateLoop, "if (!(ch >= 'A' && ch <= 'Z')) return fail(Reject::BadState);"}},
			want:  []string{base64urlRead},
		},
		{
			name:  "base58 has no digits",
			edits: []authLinkEdit{{authLinkBase58Digit, "const bool digit = false;"}},
			want:  []string{base58Read, pageApprove},
		},
		{
			name:  "base58 has no lower case",
			edits: []authLinkEdit{{authLinkBase58Lower, "const bool lower = false;"}},
			want:  []string{base58Read, pageApprove},
		},
		{
			name: "the nonce alone stops at digits and capital letters",
			edits: []authLinkEdit{{authLinkNonceCheck,
				`out.nonce.find_first_not_of("123456789ABCDEFGHJKLMNPQRSTUVWXYZ") != std::string::npos`}},
			want: []string{base58Read},
		},
		{
			name:  "base58 lets the capital I through",
			edits: []authLinkEdit{{"&& c != 'I' && c != 'O';", "&& c != 'O';"}},
			want:  []string{"base58 leaves out I (epk)"},
		},
		{
			name:  "base58 lets the capital O through",
			edits: []authLinkEdit{{"&& c != 'I' && c != 'O';", "&& c != 'I';"}},
			want:  []string{"base58 leaves out O (epk)"},
		},
		{
			name:  "base58 lets the lower-case l through",
			edits: []authLinkEdit{{"&& c != 'l';", ";"}},
			want:  []string{"base58 leaves out l (epk)"},
		},
		{
			name:  "base58 lets the digit 0 through",
			edits: []authLinkEdit{{authLinkBase58Digit, "const bool digit = c >= '0' && c <= '9';"}},
			want:  []string{"base58 leaves out 0 (epk)"},
		},
		{
			name: "base64url reaches one character past the Z",
			edits: []authLinkEdit{{authLinkBase64UrlLine,
				"return (c >= 'A' && c <= '[') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';"}},
			want: []string{outsiderRefused},
		},
		{
			name:  "base58 reaches one character past the z",
			edits: []authLinkEdit{{authLinkBase58Lower, "const bool lower = (c >= 'a' && c <= '{') && c != 'l';"}},
			want:  []string{outsiderRefused},
		},
		{
			name:  "base58 takes the url-safe '-' and '_'",
			edits: []authLinkEdit{{"return digit || upper || lower;", "return digit || upper || lower || c == '-' || c == '_';"}},
			want:  []string{outsiderRefused},
		},
		{
			name: "base64url takes the padding character",
			edits: []authLinkEdit{{authLinkBase64UrlLine,
				"return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '=';"}},
			want: []string{outsiderRefused},
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			program := authLinkTestProgram(t, func(source string) string {
				for _, edit := range c.edits {
					source = authLinkReplace(t, source, edit.old, edit.new)
				}
				return source
			})
			output, err := exec.Command(program).CombinedOutput()
			if err == nil {
				t.Fatalf("negative control was not detected: the spec passed\n%s", output)
			}
			for _, want := range c.want {
				if !strings.Contains(string(output), want) {
					t.Errorf("negative control was detected, but not by %q\n%s", want, output)
				}
			}
		})
	}
}
