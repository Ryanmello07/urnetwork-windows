// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"fmt"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// An event handler must not hold the element whose event it handles. The
// element owns its handlers, and a C++/WinRT lambda that captures the element
// (or a copy of the reference, or a struct holding it) owns the element back:
// a cycle no reference count can end, so the element, its handler and
// everything either holds stay alive after the element leaves the tree. The
// activity feed's Advanced-mode rows did this with their ContextRequested
// handler, so every row the feed dropped was never freed. A handler that
// needs its element reads it from the sender, or captures it weakly.
//
// The gate reads every lambda registered on a member of an object as the
// first argument of a call whose name is capitalized - the shape of a
// C++/WinRT event registration (Click, Toggled, ContextRequested, Loaded,
// Tick) - in app/src/App, with comments stripped.

// A lambda registered as a handler: the element it is registered on, the
// event, its captures and its body.
type selfCaptureHandler struct {
	file     string
	line     int
	target   string
	event    string
	captures []string
	body     string
	prefix   string // the source before the registration, back to the enclosing definition
}

// The index of the bracket that closes the one opened at `open`, skipping
// nested brackets of every kind and string and character literals; -1 when
// it never closes.
func selfCaptureClose(code string, open int) int {
	closer := map[byte]byte{'(': ')', '[': ']', '{': '}'}
	var stack []byte
	for at := open; at < len(code); at++ {
		switch c := code[at]; c {
		case '"', '\'':
			if c == '\'' && !opensCharacterLiteral(code, at) {
				continue
			}
			for at++; at < len(code) && code[at] != c && code[at] != '\n'; at++ {
				if code[at] == '\\' {
					at++
				}
			}
		case '(', '[', '{':
			stack = append(stack, closer[c])
		case ')', ']', '}':
			if len(stack) == 0 || stack[len(stack)-1] != c {
				return -1
			}
			stack = stack[:len(stack)-1]
			if len(stack) == 0 {
				return at
			}
		}
	}
	return -1
}

// The expression a call is made on, read backwards from the '.' before the
// call's name: identifiers, '.', '->', '::' and balanced (...) / [...] groups.
func selfCaptureTarget(code string, dot int) string {
	at := dot
	for at > 0 {
		c := code[at-1]
		switch {
		case isIdentifierByte(c) || c == '.' || c == ':':
			at--
		case c == '>' && at >= 2 && code[at-2] == '-':
			at -= 2
		case c == ')' || c == ']':
			open := map[byte]byte{')': '(', ']': '['}[c]
			depth := 0
			found := -1
			for back := at - 1; back >= 0; back-- {
				if code[back] == c {
					depth++
				} else if code[back] == open {
					depth--
					if depth == 0 {
						found = back
						break
					}
				}
			}
			if found < 0 {
				return strings.TrimSpace(code[at:dot])
			}
			at = found
		default:
			return strings.TrimSpace(code[at:dot])
		}
	}
	return strings.TrimSpace(code[at:dot])
}

var selfCaptureCall = regexp.MustCompile(`\.\s*([A-Z][A-Za-z0-9_]*)\s*\(\s*\[`)

// Every lambda registered as a handler in one source.
func selfCaptureHandlers(file, source string) []selfCaptureHandler {
	code := stripComments(lf(source))
	lineAt := func(at int) int { return strings.Count(code[:at], "\n") + 1 }
	var handlers []selfCaptureHandler
	for _, match := range selfCaptureCall.FindAllStringSubmatchIndex(code, -1) {
		dot, event := match[0], code[match[2]:match[3]]
		open := match[1] - 1 // the capture list's '['
		closeCaptures := selfCaptureClose(code, open)
		if closeCaptures < 0 {
			continue
		}
		after := strings.TrimLeft(code[closeCaptures+1:], " \t\n")
		if !strings.HasPrefix(after, "(") {
			continue // a subscript, not a lambda
		}
		params := closeCaptures + 1 + strings.Index(code[closeCaptures+1:], "(")
		closeParams := selfCaptureClose(code, params)
		if closeParams < 0 {
			continue
		}
		brace := strings.IndexByte(code[closeParams:], '{')
		if brace < 0 {
			continue
		}
		bodyOpen := closeParams + brace
		bodyClose := selfCaptureClose(code, bodyOpen)
		if bodyClose < 0 {
			continue
		}
		var captures []string
		list := code[open+1 : closeCaptures]
		depth, start := 0, 0
		for at := 0; at <= len(list); at++ {
			if at == len(list) || (list[at] == ',' && depth == 0) {
				if capture := strings.TrimSpace(list[start:at]); capture != "" {
					captures = append(captures, strings.Join(strings.Fields(capture), " "))
				}
				start = at + 1
				continue
			}
			switch list[at] {
			case '(', '[', '{', '<':
				depth++
			case ')', ']', '}', '>':
				depth--
			}
		}
		prefixStart := strings.LastIndex(code[:dot], "\n}\n")
		if prefixStart < 0 {
			prefixStart = 0
		}
		handlers = append(handlers, selfCaptureHandler{
			file: file, line: lineAt(dot), target: selfCaptureTarget(code, dot), event: event,
			captures: captures, body: code[bodyOpen : bodyClose+1], prefix: code[prefixStart:dot],
		})
	}
	return handlers
}

var selfCaptureIdentifier = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

// The first name of an expression: `row` for `row.root`, `w_` for
// `w_.SomeToggle()`.
func selfCaptureRoot(target string) string {
	end := 0
	for end < len(target) && isIdentifierByte(target[end]) {
		end++
	}
	return target[:end]
}

// The locals declared from the target before the registration, in the same
// definition: `const Button anchor = row.root;`, `ToggleSwitch t{toggle};`.
func selfCaptureAliases(prefix, target string) map[string]bool {
	aliases := map[string]bool{}
	declaration := regexp.MustCompile(`\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|\{|\()\s*` + regexp.QuoteMeta(target) + `\s*[;})]`)
	for _, match := range declaration.FindAllStringSubmatch(prefix, -1) {
		aliases[match[1]] = true
	}
	return aliases
}

// What a capture holds strongly of the handler's own element, or "" when it
// holds none of it.
func selfCaptureHolds(handler selfCaptureHandler, capture string) string {
	root := selfCaptureRoot(handler.target)
	if root == "" || root == "this" {
		return ""
	}
	// a raw pointer to what holds the element (`ListUi* ui`, then ui->input)
	// owns nothing; a smart pointer, a struct or the element itself does
	if root != handler.target && regexp.MustCompile(`[A-Za-z0-9_>]\s*\*\s*`+regexp.QuoteMeta(root)+`\s*(?:=|;|\{|\()`).MatchString(handler.prefix) {
		return ""
	}
	aliases := selfCaptureAliases(handler.prefix, handler.target)
	refersToTarget := func(expression string) bool {
		expression = strings.TrimSpace(expression)
		if inner := strings.TrimSuffix(strings.TrimPrefix(expression, "std::move("), ")"); inner != expression {
			expression = strings.TrimSpace(inner)
		}
		return expression == handler.target || expression == root || aliases[expression]
	}
	switch {
	case capture == "&" || strings.HasPrefix(capture, "&") || capture == "this" || capture == "*this":
		return "" // a reference or a raw pointer owns nothing
	case capture == "=":
		for name := range aliases {
			if regexp.MustCompile(`\b` + name + `\b`).MatchString(handler.body) {
				return fmt.Sprintf("[=] copies %s, a copy of %s", name, handler.target)
			}
		}
		if regexp.MustCompile(`\b` + regexp.QuoteMeta(root) + `\b`).MatchString(handler.body) {
			if root == handler.target {
				return fmt.Sprintf("[=] copies %s", root)
			}
			return fmt.Sprintf("[=] copies %s, which holds %s", root, handler.target)
		}
		return ""
	case selfCaptureIdentifier.MatchString(capture):
		if refersToTarget(capture) {
			if capture == root && root != handler.target {
				return fmt.Sprintf("%s holds %s", capture, handler.target)
			}
			if aliases[capture] {
				return fmt.Sprintf("%s is a copy of %s", capture, handler.target)
			}
			return capture
		}
		return ""
	}
	// an init capture: name = expression, name{expression}, name(expression)
	split := strings.IndexAny(capture, "={(")
	if split < 0 {
		return ""
	}
	expression := strings.TrimSpace(capture[split+1:])
	if capture[split] != '=' {
		expression = strings.TrimSpace(expression[:len(expression)-1])
	}
	if strings.Contains(strings.ToLower(expression), "weak") {
		return "" // get_weak(), make_weak(), weak_from_this()
	}
	if refersToTarget(expression) {
		return fmt.Sprintf("%s copies %s", capture, handler.target)
	}
	return ""
}

// The handlers in a tree of sources (path -> contents) that hold their own
// element, as "file:line: target.Event captures ... (why)".
func selfCaptureFindings(files map[string]string) ([]string, int) {
	var findings []string
	read := 0
	for _, name := range sortedNames(files) {
		for _, handler := range selfCaptureHandlers(name, files[name]) {
			read++
			for _, capture := range handler.captures {
				if why := selfCaptureHolds(handler, capture); why != "" {
					findings = append(findings, fmt.Sprintf("%s:%d: %s.%s's handler captures [%s] (%s)",
						handler.file, handler.line, handler.target, handler.event, capture, why))
				}
			}
		}
	}
	sort.Strings(findings)
	return findings, read
}

// The complement: registrations the gate cannot read, because they hand over
// an object and a member function (`{ this, &Page::OnClick }`,
// `{ get_strong(), &T::M }`) rather than a lambda, so there is no capture list.
var selfCaptureMemberHandler = regexp.MustCompile(`\.\s*[A-Z][A-Za-z0-9_]*\s*\(\s*\{\s*(?:this|get_weak\(\)|get_strong\(\)|[A-Za-z_][A-Za-z0-9_]*)\s*,\s*&`)

func TestHandlersDoNotHoldTheirOwnElement(t *testing.T) {
	files := appSourceFiles(t, ".cpp", ".h")
	findings, read := selfCaptureFindings(files)
	t.Logf("read %d handlers registered as lambdas in %d app sources", read, len(files))
	notRead := 0
	for _, name := range sortedNames(files) {
		notRead += len(selfCaptureMemberHandler.FindAllStringIndex(stripComments(lf(files[name])), -1))
	}
	t.Logf("not read: %d registrations that hand over a member function, not a lambda", notRead)
	if notRead != 0 {
		t.Errorf("%d registrations hand over a member function; read their owners here before trusting this gate", notRead)
	}
	for _, finding := range findings {
		t.Errorf("%s: the element owns the handler, so this is a cycle that keeps both alive; use the sender, or capture it weakly", finding)
	}
	if read < 100 {
		t.Errorf("the gate read only %d handlers; it is not reading the sources it claims to", read)
	}
}

// The gate fires: on each way a handler can hold its own element, for that
// reason, and not on the ways that hold nothing; and on the real activity row
// put back the way it was before this fix.
func TestHandlerSelfCaptureGateFires(t *testing.T) {
	wrap := func(body string) string { return "void F() {\n" + body + "\n}\n" }
	bad := []struct{ name, source, want string }{
		{"the element itself", wrap(`  Button copy;
  copy.Click([codeText, copy](auto const&, auto const&) { copy.Content(nullptr); });`), "[copy] (copy)"},
		{"a struct holding it", wrap(`  row.root.Click([row](auto const&, auto const&) {});`), "[row] (row holds row.root)"},
		{"a copy of the reference", wrap(`  const Controls::Button anchor = row.root;
  row.root.ContextRequested([weak = w_.get_weak(), key, anchor](auto const&, auto const& args) {
    args.Handled(true);
  });`), "[anchor] (anchor is a copy of row.root)"},
		{"an init capture", wrap(`  toggle.Toggled([t = toggle](auto const&, auto const&) {});`), "[t = toggle] (t = toggle copies toggle)"},
		{"a moved copy", wrap(`  ToggleSwitch other{toggle};
  toggle.Toggled([t = std::move(other)](auto const&, auto const&) {});`), "copies toggle"},
		{"a default copy", wrap(`  toggle.Toggled([=](auto const&, auto const&) { toggle.IsOn(); });`), "[=]"},
		{"a timer", wrap(`  auto timer = queue.CreateTimer();
  timer.Tick([timer](auto const&, auto const&) { timer.Stop(); });`), "[timer]"},
		{"a shared pointer to what holds it", wrap(`  auto ui = std::make_shared<ListUi>();
  ui->add.Click([weak, ui](auto const&, auto const&) {});`), "[ui] (ui holds ui->add)"},
	}
	for _, c := range bad {
		findings, _ := selfCaptureFindings(map[string]string{"Bad.cpp": c.source})
		if len(findings) != 1 || !strings.Contains(findings[0], c.want) {
			t.Errorf("%s: want one finding with %q, got %q", c.name, c.want, findings)
			continue
		}
		t.Logf("fires (%s): %s", c.name, findings[0])
	}
	good := []struct{ name, source string }{
		{"the sender", wrap(`  row.root.ContextRequested([weak = w_.get_weak(), key](IInspectable const& sender, auto const& args) {
    sender.as<FrameworkElement>();
  });`)},
		{"a weak capture", wrap(`  toggle.Toggled([weak = winrt::make_weak(toggle)](auto const&, auto const&) {});`)},
		{"another element", wrap(`  copy.Click([share](auto const&, auto const&) { share.Content(nullptr); });`)},
		{"by reference", wrap(`  toggle.Toggled([&](auto const&, auto const&) { toggle.IsOn(); });`)},
		{"this", wrap(`  w_.LeaderboardPublicToggle().Toggled([this](auto const&, auto const&) {});`)},
		{"a comment", wrap(`  // toggle.Toggled([toggle](auto const&, auto const&) {});`)},
		{"a raw pointer to what holds it", wrap(`  ListUi* ui = lists_.back().get();
  ui->add.Click([weak, ui](auto const&, auto const&) {});`)},
	}
	for _, c := range good {
		if findings, read := selfCaptureFindings(map[string]string{"Good.cpp": c.source}); len(findings) != 0 {
			t.Errorf("%s: want no finding, got %q (read %d)", c.name, findings, read)
		}
	}
	// the complement's reader counts what it says it counts
	members := "  button.Click({ this, &Page::OnClick });\n  timer.Tick({ get_strong(), &Page::OnTick });\n"
	if count := len(selfCaptureMemberHandler.FindAllStringIndex(members, -1)); count != 2 {
		t.Errorf("the member-function registrations read as %d, want 2", count)
	}

	// the activity row, put back the way it was: the gate names it, and nothing else
	files := appSourceFiles(t, ".cpp", ".h")
	fixed := strings.Join([]string{
		"    row.root.ContextRequested(",
		"        [weak = w_.get_weak(), key, group](",
	}, "\n")
	before := strings.Join([]string{
		"    const Controls::Button anchor = row.root;",
		"    row.root.ContextRequested(",
		"        [weak = w_.get_weak(), key, group, anchor](",
	}, "\n")
	source := lf(files["ConnectPage.cpp"])
	if count := strings.Count(source, fixed); count != 1 {
		t.Fatalf("ConnectPage.cpp: want the activity row's registration once to put back, found it %d times", count)
	}
	files["ConnectPage.cpp"] = strings.Replace(source, fixed, before, 1)
	findings, _ := selfCaptureFindings(files)
	if len(findings) != 1 || !strings.HasPrefix(findings[0], "ConnectPage.cpp:") || !strings.Contains(findings[0], "[anchor]") {
		t.Fatalf("the activity row put back: want one finding in ConnectPage.cpp naming anchor, got %q", findings)
	}
	t.Logf("fires (the activity row put back): %s", findings[0])
}
