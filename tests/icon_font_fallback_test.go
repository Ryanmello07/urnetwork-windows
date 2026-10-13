// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// The icon font. Windows 11 ships Segoe Fluent Icons and Windows 10 does not,
// so an icon drawn from "Segoe Fluent Icons" alone is an empty box on Windows
// 10: every row icon of the official build drew that way on a windows-2022
// runner (Windows Server 2022, the Windows 10 21H2 code base). Segoe MDL2
// Assets, which both ship, maps every glyph the app uses there. So every
// source that names an icon font names exactly the list below, the one WinUI
// itself gives FontIcon by default: App.xaml's UrIconFontFamily and
// UrComponents.h's kIconFontFamily, which everything else goes through. One
// family alone fails, and so does the list the other way round, which would
// draw Windows 11 from the older font.
const iconFontFallback = "Segoe Fluent Icons,Segoe MDL2 Assets"

// The Segoe icon families: a value that mentions one names an icon font.
var iconFontFamilies = []string{"Segoe Fluent Icons", "Segoe MDL2 Assets"}

// One place a source names an icon font: the value it names, where.
type iconFontNaming struct {
	file  string
	line  int
	value string
}

func (naming iconFontNaming) String() string {
	return fmt.Sprintf("%s:%d %q", naming.file, naming.line, naming.value)
}

func iconFontMentions(text string) int {
	lower := strings.ToLower(text)
	count := 0
	for _, family := range iconFontFamilies {
		count += strings.Count(lower, strings.ToLower(family))
	}
	return count
}

// Whether a FontFamily value is the fallback list: its families, trimmed, in
// that order. Family names are matched without regard to case, as DirectWrite
// matches them.
func isIconFontFallback(value string) bool {
	families := strings.Split(value, ",")
	for index := range families {
		families[index] = strings.TrimSpace(families[index])
	}
	return strings.EqualFold(strings.Join(families, ","), iconFontFallback)
}

// The namings in a XAML file, read as XML: every attribute value and every
// run of element text that mentions an icon family. A comment is not a naming.
func xamlIconFontNamings(file, source string) ([]iconFontNaming, error) {
	decoder := xml.NewDecoder(strings.NewReader(source))
	var namings []iconFontNaming
	for {
		token, err := decoder.Token()
		if errors.Is(err, io.EOF) {
			return namings, nil
		}
		if err != nil {
			return nil, fmt.Errorf("%s does not parse as XML: %v", file, err)
		}
		line, _ := decoder.InputPos()
		switch token := token.(type) {
		case xml.StartElement:
			for _, attribute := range token.Attr {
				if iconFontMentions(attribute.Value) > 0 {
					namings = append(namings, iconFontNaming{file, line, attribute.Value})
				}
			}
		case xml.CharData:
			if text := strings.TrimSpace(string(token)); iconFontMentions(text) > 0 {
				namings = append(namings, iconFontNaming{file, line, text})
			}
		}
	}
}

// A C++ string literal: its contents, and the offset of its opening quote.
type cppLiteral struct {
	at    int
	value string
}

// Lexes C++ (and IDL) source into its string literals, raw ones included, and
// returns the code with every comment, character literal and string literal
// blanked (line breaks kept).
func cppStringLiterals(code string) ([]cppLiteral, string) {
	out := []byte(code)
	blank := func(from, to int) {
		for ; from < to && from < len(out); from++ {
			if out[from] != '\n' {
				out[from] = ' '
			}
		}
	}
	// the end of a quoted run that opens at `at`: its closing quote, or the
	// line's end when it never closes
	closeQuoted := func(at int, quote byte) int {
		for at++; at < len(code) && code[at] != quote && code[at] != '\n'; at++ {
			if code[at] == '\\' {
				at++
			}
		}
		return at
	}
	var literals []cppLiteral
	for at := 0; at < len(code); at++ {
		switch {
		case strings.HasPrefix(code[at:], "//"):
			end := strings.IndexByte(code[at:], '\n')
			if end < 0 {
				end = len(code) - at
			}
			blank(at, at+end)
			at += end
		case strings.HasPrefix(code[at:], "/*"):
			end := strings.Index(code[at+2:], "*/")
			stop := len(code)
			if end >= 0 {
				stop = at + 2 + end + 2
			}
			blank(at, stop)
			at = stop - 1
		case code[at] == '\'' && opensCharacterLiteral(code, at):
			end := closeQuoted(at, '\'')
			blank(at, end+1)
			at = end
		case code[at] == '"':
			if open, ok := cppRawStringOpen(code, at); ok {
				delimiter := code[at+1 : open]
				closer := ")" + delimiter + `"`
				end := strings.Index(code[open+1:], closer)
				stop := len(code)
				value := code[open+1:]
				if end >= 0 {
					value = code[open+1 : open+1+end]
					stop = open + 1 + end + len(closer)
				}
				literals = append(literals, cppLiteral{at, value})
				blank(at, stop)
				at = stop - 1
				continue
			}
			end := closeQuoted(at, '"')
			literals = append(literals, cppLiteral{at, code[at+1 : min(end, len(code))]})
			blank(at, end+1)
			at = end
		}
	}
	return literals, string(out)
}

// Whether the quote at `at` opens a raw string literal (R"delimiter(...)",
// with any encoding prefix), and where its opening parenthesis is.
func cppRawStringOpen(code string, at int) (int, bool) {
	start := at
	for start > 0 && isIdentifierByte(code[start-1]) {
		start--
	}
	switch code[start:at] {
	case "R", "LR", "uR", "UR", "u8R":
	default:
		return 0, false
	}
	open := strings.IndexByte(code[at+1:], '(')
	if open < 0 || open > 16 || strings.ContainsAny(code[at+1:at+1+open], " \\)\"\n") {
		return 0, false
	}
	return at + 1 + open, true
}

// The namings in C++ (or IDL) source: every string literal that mentions an
// icon family. A comment is not a naming. A mention left in the code outside
// every literal (token pasting, a macro, a spelling this lexer misreads) is
// returned as a naming of its own, so it cannot pass as the fallback.
func cppIconFontNamings(file, source string) []iconFontNaming {
	code := strings.ReplaceAll(source, "\r\n", "\n")
	lineAt := func(at int) int { return strings.Count(code[:at], "\n") + 1 }
	literals, rest := cppStringLiterals(code)
	var namings []iconFontNaming
	for _, literal := range literals {
		if iconFontMentions(literal.value) > 0 {
			namings = append(namings, iconFontNaming{file, lineAt(literal.at), literal.value})
		}
	}
	lowerRest := strings.ToLower(rest)
	for _, family := range iconFontFamilies {
		needle := strings.ToLower(family)
		for from := 0; ; {
			found := strings.Index(lowerRest[from:], needle)
			if found < 0 {
				break
			}
			namings = append(namings, iconFontNaming{file, lineAt(from + found),
				"outside every string literal: " + family})
			from += found + len(needle)
		}
	}
	return namings
}

// The kinds of file the gate reads, by extension; documentation names fonts
// in prose, and is listed, not read.
var (
	iconFontXamlKinds = map[string]bool{".xaml": true}
	iconFontCppKinds  = map[string]bool{".cpp": true, ".h": true, ".hpp": true, ".cc": true,
		".cxx": true, ".inl": true, ".idl": true}
	iconFontProseKinds = map[string]bool{".md": true, ".txt": true}
)

// What the gate makes of a tree of sources (path -> contents): every naming,
// every violation, and the mentions it read and did not count as namings.
type iconFontReport struct {
	namings    []iconFontNaming
	violations []string
	notNamings []string
}

func checkIconFontNamings(files map[string]string) iconFontReport {
	var report iconFontReport
	paths := make([]string, 0, len(files))
	for path := range files {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	for _, path := range paths {
		source := files[path]
		mentions := iconFontMentions(source)
		if mentions == 0 {
			continue
		}
		kind := strings.ToLower(filepath.Ext(path))
		var namings []iconFontNaming
		switch {
		case iconFontXamlKinds[kind]:
			found, err := xamlIconFontNamings(path, source)
			if err != nil {
				report.violations = append(report.violations, err.Error())
				continue
			}
			namings = found
		case iconFontCppKinds[kind]:
			namings = cppIconFontNamings(path, source)
		case iconFontProseKinds[kind]:
			report.notNamings = append(report.notNamings,
				fmt.Sprintf("%s: %d mention(s) in documentation", path, mentions))
			continue
		default:
			report.violations = append(report.violations, fmt.Sprintf(
				"%s names an icon font in a kind of file this gate cannot read (%q); read it here or name the font from a source it reads",
				path, kind))
			continue
		}
		counted := 0
		for _, naming := range namings {
			counted += iconFontMentions(naming.value)
			report.namings = append(report.namings, naming)
			if !isIconFontFallback(naming.value) {
				report.violations = append(report.violations, fmt.Sprintf(
					"%s names an icon font without the fallback; name %q (kit::kIconFontFamily in code, {StaticResource UrIconFontFamily} in markup)",
					naming, iconFontFallback))
			}
		}
		if counted < mentions {
			report.notNamings = append(report.notNamings,
				fmt.Sprintf("%s: %d mention(s) in comments", path, mentions-counted))
		}
	}
	return report
}

// Every file under app/src, keyed by its slash-separated path from the
// repository root. Build output is not source: the directories a local build
// writes are left out, and printed when present.
func iconFontSourceTree(t *testing.T) map[string]string {
	t.Helper()
	root := repositoryRoot(t)
	scope := filepath.Join(root, "app", "src")
	buildOutput := map[string]bool{"Generated Files": true, "x64": true, "ARM64": true,
		"bin": true, "obj": true}
	files := map[string]string{}
	err := filepath.WalkDir(scope, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			if path != scope && buildOutput[entry.Name()] {
				t.Logf("not read (build output): %s", path)
				return filepath.SkipDir
			}
			return nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		relative, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		files[filepath.ToSlash(relative)] = string(data)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return files
}

// No source names an icon font without the fallback. The two definitions must
// be among the namings the gate reads, or a zero would only mean it read
// nothing; and every naming it found is printed with what it found it in.
func TestIconFontNamesCarryTheFallback(t *testing.T) {
	files := iconFontSourceTree(t)
	kinds := map[string]int{}
	for path := range files {
		kinds[strings.ToLower(filepath.Ext(path))]++
	}
	t.Logf("read %d files under app/src: %d XAML, %d C++/IDL", len(files),
		kinds[".xaml"], kinds[".cpp"]+kinds[".h"]+kinds[".hpp"]+kinds[".cc"]+kinds[".cxx"]+kinds[".inl"]+kinds[".idl"])

	report := checkIconFontNamings(files)
	for _, naming := range report.namings {
		t.Logf("naming: %s", naming)
	}
	for _, line := range report.notNamings {
		t.Logf("not a naming: %s", line)
	}
	for _, violation := range report.violations {
		t.Error(violation)
	}

	definitions := map[string]string{
		"app/src/App/App.xaml":       "UrIconFontFamily",
		"app/src/App/UrComponents.h": "kIconFontFamily",
	}
	for file, name := range definitions {
		found := 0
		for _, naming := range report.namings {
			if naming.file == file && isIconFontFallback(naming.value) {
				found++
			}
		}
		if found != 1 {
			t.Errorf("%s: want exactly one naming of %q (%s), the gate read %d", file, iconFontFallback, name, found)
		}
	}
	if !strings.Contains(files["app/src/App/App.xaml"],
		`<FontFamily x:Key="UrIconFontFamily">`+iconFontFallback+`</FontFamily>`) {
		t.Errorf("App.xaml no longer defines UrIconFontFamily as %q", iconFontFallback)
	}
	if !strings.Contains(files["app/src/App/UrComponents.h"],
		`inline constexpr wchar_t kIconFontFamily[] = L"`+iconFontFallback+`";`) {
		t.Errorf("UrComponents.h no longer defines kIconFontFamily as %q", iconFontFallback)
	}
}

// The gate fires: on each way of naming an icon font without the fallback,
// for its own reason (the file and the value it names), and on the real
// sources put back the way they were before the fallback, one at a time.
func TestIconFontGateFiresWithoutTheFallback(t *testing.T) {
	bad := []struct {
		name, file, source, value string
	}{
		{"element text", "a/App.xaml", `<ResourceDictionary xmlns="x"><FontFamily x:Key="K" xmlns:x="y">Segoe Fluent Icons</FontFamily></ResourceDictionary>`, "Segoe Fluent Icons"},
		{"attribute", "a/Page.xaml", `<Grid xmlns="x"><FontIcon FontFamily="Segoe MDL2 Assets" Glyph="&#xE713;"/></Grid>`, "Segoe MDL2 Assets"},
		{"setter", "a/Styles.xaml", `<Style xmlns="x"><Setter Property="FontFamily" Value="Segoe Fluent Icons"/></Style>`, "Segoe Fluent Icons"},
		{"reversed list", "a/Order.xaml", `<Grid xmlns="x"><FontIcon FontFamily="Segoe MDL2 Assets,Segoe Fluent Icons"/></Grid>`, "Segoe MDL2 Assets,Segoe Fluent Icons"},
		{"a third family", "a/Third.xaml", `<Grid xmlns="x"><FontIcon FontFamily="Segoe Fluent Icons,Segoe MDL2 Assets,Segoe UI Symbol"/></Grid>`, "Segoe Fluent Icons,Segoe MDL2 Assets,Segoe UI Symbol"},
		{"wide literal", "a/Icon.cpp", "icon.FontFamily(Media::FontFamily(L\"Segoe Fluent Icons\"));\n", "Segoe Fluent Icons"},
		{"the older family", "a/Clock.cpp", "clock.FontFamily(FontFamily(L\"Segoe MDL2 Assets\"));\n", "Segoe MDL2 Assets"},
		{"another case", "a/Case.h", "constexpr wchar_t kFont[] = L\"segoe fluent icons\";\n", "segoe fluent icons"},
		{"raw literal", "a/Raw.cpp", "auto font = LR\"(Segoe Fluent Icons)\";\n", "Segoe Fluent Icons"},
		{"after a comment", "a/After.cpp", "// a note\nauto quote = L'\"'; auto font = L\"Segoe MDL2 Assets\"; /* x */\n", "Segoe MDL2 Assets"},
		{"pasted tokens", "a/Macro.h", "#define ICON_FONT(x) L#x\nauto font = ICON_FONT(Segoe Fluent Icons);\n", "outside every string literal: Segoe Fluent Icons"},
		{"unread kind", "a/fonts.json", `{"icon_font": "Segoe Fluent Icons"}`, ""},
	}
	for _, c := range bad {
		report := checkIconFontNamings(map[string]string{c.file: c.source})
		if len(report.violations) != 1 {
			t.Errorf("%s: want one violation, got %d: %q", c.name, len(report.violations), report.violations)
			continue
		}
		violation := report.violations[0]
		if !strings.HasPrefix(violation, c.file) || (c.value != "" && !strings.Contains(violation, fmt.Sprintf("%q", c.value))) {
			t.Errorf("%s: the violation does not name %s and %q: %s", c.name, c.file, c.value, violation)
		}
		t.Logf("fires (%s): %s", c.name, violation)
	}

	good := []struct {
		name, file, source string
		namings            int
	}{
		{"the resource", "b/App.xaml", `<ResourceDictionary xmlns="x"><FontFamily x:Key="K" xmlns:x="y">Segoe Fluent Icons,Segoe MDL2 Assets</FontFamily></ResourceDictionary>`, 1},
		{"spaced", "b/Page.xaml", `<Grid xmlns="x"><FontIcon FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets"/></Grid>`, 1},
		{"a XAML comment", "b/Note.xaml", `<Grid xmlns="x"><!-- Segoe Fluent Icons ships with Windows 11 --></Grid>`, 0},
		{"the constant", "b/Kit.h", "inline constexpr wchar_t kIconFontFamily[] = L\"Segoe Fluent Icons,Segoe MDL2 Assets\";\n", 1},
		{"C++ comments", "b/Note.cpp", "// Segoe Fluent Icons\n/* Segoe MDL2 Assets */ auto x = L\"//\"; // Segoe MDL2 Assets\n", 0},
		{"documentation", "b/README.md", "Segoe Fluent Icons ships with Windows 11.\n", 0},
	}
	for _, c := range good {
		report := checkIconFontNamings(map[string]string{c.file: c.source})
		if len(report.violations) != 0 || len(report.namings) != c.namings {
			t.Errorf("%s: want %d naming(s) and no violation, got %v and %q", c.name, c.namings, report.namings, report.violations)
		}
	}

	// the real sources, each put back the way it was before the fallback
	reverts := []struct {
		file, fixed, before string
	}{
		{"app/src/App/App.xaml",
			`<FontFamily x:Key="UrIconFontFamily">Segoe Fluent Icons,Segoe MDL2 Assets</FontFamily>`,
			`<FontFamily x:Key="UrIconFontFamily">Segoe Fluent Icons</FontFamily>`},
		{"app/src/App/UrComponents.h",
			`kIconFontFamily[] = L"Segoe Fluent Icons,Segoe MDL2 Assets";`,
			`kIconFontFamily[] = L"Segoe Fluent Icons";`},
		{"app/src/App/AuthSheets.cpp",
			`copyGlyph.FontFamily(Media::FontFamily(kit::kIconFontFamily));`,
			`copyGlyph.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));`},
		{"app/src/App/ConnectCanvas.cpp",
			`glyph_.FontFamily(FontFamily(kit::kIconFontFamily));`,
			`glyph_.FontFamily(FontFamily(L"Segoe Fluent Icons"));`},
		{"app/src/App/BalanceSheets.cpp",
			`clock.FontFamily(FontFamily(kit::kIconFontFamily));`,
			`clock.FontFamily(FontFamily(L"Segoe MDL2 Assets"));`},
	}
	files := iconFontSourceTree(t)
	for _, revert := range reverts {
		source := strings.ReplaceAll(files[revert.file], "\r\n", "\n")
		if count := strings.Count(source, revert.fixed); count != 1 {
			t.Errorf("%s: want the fixed line once to put back, found it %d times: %s", revert.file, count, revert.fixed)
			continue
		}
		reverted := map[string]string{}
		for path, contents := range files {
			reverted[path] = contents
		}
		reverted[revert.file] = strings.Replace(source, revert.fixed, revert.before, 1)
		report := checkIconFontNamings(reverted)
		var own []string
		for _, violation := range report.violations {
			if strings.HasPrefix(violation, revert.file+":") {
				own = append(own, violation)
			}
		}
		if len(own) == 0 || len(own) != len(report.violations) {
			t.Errorf("%s put back: want violations in that file only, got %q", revert.file, report.violations)
			continue
		}
		t.Logf("fires (%s put back): %s", revert.file, own[0])
	}
}
