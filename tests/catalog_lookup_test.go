// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// Every string id the app looks up must be a resource of the generated neutral
// catalog, Strings/en/Resources.resw, which carries every live key of the
// localization store. A missing id fails quietly: Loc and Format render the raw
// id (the seedphrase sheets showed "seedphrase_saved_confirm" on a button), and
// Adv, Dev and the other English-fallback lookups render their English in every
// language. A fallback whose English differs from the catalog's fails as
// quietly the other way, because the catalog's text is what shows: a key that
// means something else shows that meaning (the developer page titled its
// Recovery section "Recovery time"), and a store English that a copy change
// left behind shows the old copy (the Auto transports footer's order). The scan
// reads each call of a lookup function, or of a page helper that hands its key
// to one, and treats every narrow string literal argument spelled like a key id
// as a lookup, with the wide literal after it as its English when the function
// falls back to one; a key literal assigned to a name ending in Key
// (row.labelKey = "...";), which a lookup reads later, is one as well.

// How a lookup function finds its resource.
type catalogLookupKind int

const (
	// the id itself
	catalogLookupPlain catalogLookupKind = iota
	// <id>.<category>, present for every plural key as <id>.other
	catalogLookupPlural
	// a key held in a name, which a plain or a plural lookup reads later
	catalogLookupEither
)

// The functions and helper lambdas that take a key id: the lookups of
// Localization.h and PageContext.h, the English-fallback variants, and the page
// helpers that pass their key to one of them.
var catalogLookupFunctionKinds = map[string]catalogLookupKind{
	"Loc":           catalogLookupPlain,
	"LocBox":        catalogLookupPlain,
	"Localized":     catalogLookupPlain,
	"Format":        catalogLookupPlain,
	"Plural":        catalogLookupPlural,
	"PluralRaw":     catalogLookupPlural,
	"PluralFormat":  catalogLookupPlural,
	"Adv":           catalogLookupPlain,
	"AdvW":          catalogLookupPlain,
	"Dev":           catalogLookupPlain,
	"DevW":          catalogLookupPlain,
	"TransportText": catalogLookupPlain,
	"Missing":       catalogLookupPlain,
	"KeyText":       catalogLookupPlain,
	"MakeAction":    catalogLookupPlain,
	"MakeTextField": catalogLookupPlain,
	"MakePicker":    catalogLookupPlain,
	"ErrorKey":      catalogLookupPlain,
	"metric":        catalogLookupPlain,
	"boolRow":       catalogLookupPlain,
	"numRow":        catalogLookupPlain,
	"millisRow":     catalogLookupPlain,
	"countRow":      catalogLookupPlain,
	"head":          catalogLookupPlain,
	"fault":         catalogLookupPlain,
	"header":        catalogLookupPlain,
	"section":       catalogLookupPlain,
	"field":         catalogLookupPlain,
	"action":        catalogLookupPlain,
	"value":         catalogLookupPlain,
	"count":         catalogLookupPlain,
	"refresh":       catalogLookupPlural,
}

// The lookups whose argument after the key is the English they render when the
// catalog lacks the key.
var catalogFallbackFunctionNames = map[string]bool{
	"Adv":           true,
	"AdvW":          true,
	"Dev":           true,
	"DevW":          true,
	"TransportText": true,
	"Missing":       true,
	"metric":        true,
	"boolRow":       true,
	"numRow":        true,
	"millisRow":     true,
	"countRow":      true,
	"head":          true,
	"fault":         true,
}

// Functions that take a key of something other than the catalog.
var catalogNonLookupFunctionReasons = map[string]string{
	"JsonFlag":   "reads a field of a release's JSON",
	"JsonString": "reads a field of a release's JSON",
}

// One lookup a source makes, at the line of its call.
type catalogLookup struct {
	file     string
	line     int
	function string
	key      string
	kind     catalogLookupKind
	// the English the call renders when the catalog lacks the key, or ""
	english string
}

// A call argument as the scan reads it: the value of its string literals, the
// kinds of literal it held, and whether it held anything else.
type catalogArgument struct {
	value  string
	narrow bool
	wide   bool
	other  bool
}

// Spelled like a store key id.
var catalogKeyPattern = regexp.MustCompile(`^[a-z][a-z0-9_]*$`)

// A key literal assigned whole to a name ending in Key.
var catalogAssignedKeyPattern = regexp.MustCompile(`\b(\w*Key)\s*=\s*"([a-z][a-z0-9_]*)"\s*;`)

// The lookups in one source. Comments are blanked first, so a call in a comment
// is none; a call through a member (x.count) or the standard library
// (std::count) is not one of these functions.
func scanCatalogLookups(file string, source string) []catalogLookup {
	code := stripComments(source)
	lookups := []catalogLookup{}
	isWordByte := func(at int) bool {
		return 0 <= at && at < len(code) && isIdentifierByte(code[at])
	}
	// the value of the literal opening at `at` (a quote), escapes decoded, and
	// the index after it
	readLiteral := func(at int) (string, int) {
		var value strings.Builder
		for at++; at < len(code) && code[at] != '"' && code[at] != '\n'; at++ {
			if code[at] != '\\' || at+1 >= len(code) {
				value.WriteByte(code[at])
				continue
			}
			at++
			switch code[at] {
			case 'n':
				value.WriteByte('\n')
			case 't':
				value.WriteByte('\t')
			case 'u', 'U':
				digitCount := 4
				if code[at] == 'U' {
					digitCount = 8
				}
				end := min(at+1+digitCount, len(code))
				if point, err := strconv.ParseUint(code[at+1:end], 16, 32); err == nil {
					value.WriteRune(rune(point))
				}
				at = end - 1
			default:
				value.WriteByte(code[at])
			}
		}
		return value.String(), at + 1
	}
	for at := 0; at < len(code); at++ {
		switch {
		case code[at] == '"':
			_, next := readLiteral(at)
			at = next - 1
			continue
		case code[at] == '\'' && !isWordByte(at-1):
			// a character literal; a quote after a digit is a digit separator
			for at++; at < len(code) && code[at] != '\''; at++ {
				if code[at] == '\\' {
					at++
				}
			}
			continue
		case !isIdentifierByte(code[at]) || isWordByte(at-1):
			continue
		}
		start := at
		for at < len(code) && isIdentifierByte(code[at]) {
			at++
		}
		name := code[start:at]
		kind, ok := catalogLookupFunctionKinds[name]
		open := at
		for open < len(code) && (code[open] == ' ' || code[open] == '\t') {
			open++
		}
		at--
		if !ok || open >= len(code) || code[open] != '(' {
			continue
		}
		// a member call, or a qualifier other than ours
		before := strings.TrimRight(code[:start], " \t\n")
		if strings.HasSuffix(before, ".") || strings.HasSuffix(before, "->") {
			continue
		}
		if strings.HasSuffix(before, "std::") {
			continue
		}
		line := strings.Count(code[:start], "\n") + 1
		// the call's top-level arguments; adjacent literals concatenate
		depth := 0
		arguments := []catalogArgument{}
		argument := catalogArgument{}
	call:
		for scan := open; scan < len(code); scan++ {
			switch character := code[scan]; {
			case character == '"':
				value, next := readLiteral(scan)
				if depth == 1 {
					argument.value += value
					if scan > 0 && isIdentifierByte(code[scan-1]) {
						argument.wide = true
					} else {
						argument.narrow = true
					}
				}
				scan = next - 1
			case character == '\'' && !isWordByte(scan-1):
				// a character literal argument, never a key
				for scan++; scan < len(code) && code[scan] != '\''; scan++ {
					if code[scan] == '\\' {
						scan++
					}
				}
				if depth == 1 {
					argument.other = true
				}
			case character == '(' || character == '[' || character == '{':
				depth++
				if depth > 1 {
					argument.other = true
				}
			case character == ')' || character == ']' || character == '}':
				depth--
				if depth == 0 {
					arguments = append(arguments, argument)
					break call
				}
			case character == ',' && depth == 1:
				arguments = append(arguments, argument)
				argument = catalogArgument{}
			case depth == 1 && character != ' ' && character != '\t' && character != '\n':
				// an identifier, a number, an operator; a wide literal's prefix
				// is read with its literal
				if !(character == 'L' && scan+1 < len(code) && code[scan+1] == '"') {
					argument.other = true
				}
			}
		}
		// a narrow literal argument spelled like a key id is a lookup, and the
		// wide literal argument after it is its English in a fallback function
		for index, key := range arguments {
			if key.other || key.wide || !key.narrow || !catalogKeyPattern.MatchString(key.value) {
				continue
			}
			english := ""
			if catalogFallbackFunctionNames[name] && index+1 < len(arguments) {
				next := arguments[index+1]
				if next.wide && !next.narrow && !next.other {
					english = next.value
				}
			}
			lookups = append(lookups, catalogLookup{
				file:     file,
				line:     line,
				function: name,
				key:      key.value,
				kind:     kind,
				english:  english,
			})
		}
	}
	for _, match := range catalogAssignedKeyPattern.FindAllStringSubmatchIndex(code, -1) {
		lookups = append(lookups, catalogLookup{
			file:     file,
			line:     strings.Count(code[:match[0]], "\n") + 1,
			function: code[match[2]:match[3]],
			key:      code[match[4]:match[5]],
			kind:     catalogLookupEither,
		})
	}
	return lookups
}

// Why the neutral catalog, `resourceValues` (each resource name's English),
// does not answer `lookup`, or "" when it does.
func catalogMiss(resourceValues map[string]string, lookup catalogLookup) string {
	where := lookup.file + ":" + strconv.Itoa(lookup.line) + ": " + lookup.function + "(\"" + lookup.key + "\")"
	resources := []string{lookup.key}
	switch lookup.kind {
	case catalogLookupPlural:
		resources = []string{lookup.key + ".other"}
	case catalogLookupEither:
		resources = append(resources, lookup.key+".other")
	}
	for _, resource := range resources {
		value, ok := resourceValues[resource]
		if !ok {
			continue
		}
		if lookup.english != "" && lookup.english != value {
			return where + " falls back to \"" + lookup.english + "\" but the catalog's English, which is what shows, is \"" +
				value + "\": align the two, or give a different meaning its own key"
		}
		return ""
	}
	return where + " is not in Strings/en/Resources.resw: add the key to the localization store and regenerate the catalogs"
}

// The scanner's rules on synthetic sources: what is a lookup, what is its
// English, and what is not a lookup; and how a miss is reported.
func TestCatalogLookupScannerRules(t *testing.T) {
	source := `
w_.Title().Text(Loc("sample_title"));
auto text = urnw::Format("sample_format", name);
note = pages::AdvW("sample_adv",
                   L"English " L"fallback");
field(box, "sample_label", "sample_help", value);
head(2, "sample_head", L"Head");
auto n = Plural("sample_plural", count);
// Loc("comment_key")
/* Dev("block_key", L"Block") */
const char* s = "Loc(\"string_key\")";
names.count("member_call");
std::count(a, b, 'x');
const auto key = bittensor::ErrorKey(code, "sample_fallback");
Loc(dynamicKey);
Loc(std::string("built_key") + suffix);
int ms = 21'600; Adv("after_separator", L"After");
fault("sample_fault", L"Line\none — \"two\"", FaultAction::Drop);
Adv("sample_composed", L"Composed " + suffix);
Loc("sample_loc", L"Not a fallback");
row.labelKey = "sample_field";
static constexpr std::string_view kSampleKey = "sample_constant";
if (row.labelKey == "sample_compared") {}
const std::string key = "sample_prefix_" + id;
`
	kindNames := map[catalogLookupKind]string{
		catalogLookupPlain:  "plain",
		catalogLookupPlural: "plural",
		catalogLookupEither: "either",
	}
	got := []string{}
	for _, lookup := range scanCatalogLookups("synthetic.cpp", source) {
		got = append(got, lookup.function+":"+lookup.key+":"+kindNames[lookup.kind]+":"+strconv.Itoa(lookup.line)+":"+lookup.english)
	}
	want := []string{
		"Loc:sample_title:plain:2:",
		"Format:sample_format:plain:3:",
		"AdvW:sample_adv:plain:4:English fallback",
		"field:sample_label:plain:6:",
		"field:sample_help:plain:6:",
		"head:sample_head:plain:7:Head",
		"Plural:sample_plural:plural:8:",
		"ErrorKey:sample_fallback:plain:14:",
		"Adv:after_separator:plain:17:After",
		"fault:sample_fault:plain:18:Line\none — \"two\"",
		"Adv:sample_composed:plain:19:",
		"Loc:sample_loc:plain:20:",
		"labelKey:sample_field:either:21:",
		"kSampleKey:sample_constant:either:22:",
	}
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Errorf("lookups:\n%s\nwant:\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}

	resourceValues := map[string]string{
		"sample_title":        "Title",
		"sample_plural.other": "{} items",
		"sample_held.other":   "{} held",
	}
	cases := []struct {
		lookup catalogLookup
		miss   string
	}{
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Loc", key: "sample_title", kind: catalogLookupPlain},
			miss:   "",
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Adv", key: "sample_title", kind: catalogLookupPlain, english: "Title"},
			miss:   "",
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Plural", key: "sample_plural", kind: catalogLookupPlural},
			miss:   "",
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "messageKey", key: "sample_held", kind: catalogLookupEither},
			miss:   "",
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Adv", key: "sample_absent", kind: catalogLookupPlain, english: "Absent"},
			miss:   `f.cpp:3: Adv("sample_absent") is not in Strings/en/Resources.resw: add the key to the localization store and regenerate the catalogs`,
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Dev", key: "sample_title", kind: catalogLookupPlain, english: "Heading"},
			miss: `f.cpp:3: Dev("sample_title") falls back to "Heading" but the catalog's English, which is what shows, is "Title": ` +
				`align the two, or give a different meaning its own key`,
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Loc", key: "sample_plural", kind: catalogLookupPlain},
			miss:   `f.cpp:3: Loc("sample_plural") is not in Strings/en/Resources.resw: add the key to the localization store and regenerate the catalogs`,
		},
	}
	for _, c := range cases {
		if miss := catalogMiss(resourceValues, c.lookup); miss != c.miss {
			t.Errorf("%s(%q): %q, want %q", c.lookup.function, c.lookup.key, miss, c.miss)
		}
	}
}

// Every id the app looks up is a resource of the neutral catalog, and every
// English fallback is the catalog's English.
func TestCatalogLookupEveryKeyIsInTheNeutralCatalog(t *testing.T) {
	document := parseXML(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "Strings", "en", "Resources.resw"))
	resourceValues := map[string]string{}
	for _, node := range document.descendants("", "data") {
		name, ok := node.attribute("name")
		if !ok {
			continue
		}
		value := ""
		if child := node.child("", "value"); child != nil {
			value = child.Text
		}
		resourceValues[name] = value
	}
	if len(resourceValues) < 1000 {
		t.Fatalf("Strings/en/Resources.resw has %d resources", len(resourceValues))
	}
	files := appSourceFiles(t, ".cpp", ".h")
	lookupCount := 0
	fallbackCount := 0
	assignedCount := 0
	for _, file := range sortedNames(files) {
		for _, lookup := range scanCatalogLookups(file, files[file]) {
			lookupCount++
			if lookup.english != "" {
				fallbackCount++
			}
			if lookup.kind == catalogLookupEither {
				assignedCount++
			}
			if miss := catalogMiss(resourceValues, lookup); miss != "" {
				t.Error(miss)
			}
		}
	}
	if lookupCount < 800 {
		t.Errorf("the scan saw %d lookups; the app makes about a thousand", lookupCount)
	}
	if fallbackCount < 250 {
		t.Errorf("the scan saw %d English fallbacks; the app has about three hundred", fallbackCount)
	}
	if assignedCount < 20 {
		t.Errorf("the scan saw %d keys assigned to a name; the app has about thirty", assignedCount)
	}
}

// A helper that takes a key and passes it to a lookup is scanned only when it
// is registered, so a new one has to be added to catalogLookupFunctionKinds (or,
// when its key is not a catalog key, to catalogNonLookupFunctionReasons).
func TestCatalogLookupHelpersAreRegistered(t *testing.T) {
	// `name = [...](... key` for a lambda, `name(... key` for a function, where
	// the parameter is a key id: a string_view or char pointer named key or
	// *Key
	definitionPattern := regexp.MustCompile(
		`(?:auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*|\b(\w+))\(([^()]*)\)\s*(?:->[^{;]*)?\{`)
	keyParameterPattern := regexp.MustCompile(`(?:std::string_view|const char\s*\*)\s*(?:key|\w+Key)\b`)
	files := appSourceFiles(t, ".cpp", ".h")
	unregistered := map[string]string{}
	for _, file := range sortedNames(files) {
		code := stripComments(files[file])
		for _, match := range definitionPattern.FindAllStringSubmatch(code, -1) {
			name := match[1] + match[2]
			if !keyParameterPattern.MatchString(match[3]) {
				continue
			}
			_, lookup := catalogLookupFunctionKinds[name]
			_, nonLookup := catalogNonLookupFunctionReasons[name]
			if !lookup && !nonLookup {
				unregistered[name] = file
			}
		}
	}
	unregisteredNames := []string{}
	for name, file := range unregistered {
		unregisteredNames = append(unregisteredNames, name+" ("+file+")")
	}
	sort.Strings(unregisteredNames)
	if len(unregisteredNames) > 0 {
		t.Errorf("key-taking functions not in catalogLookupFunctionKinds: %s", strings.Join(unregisteredNames, ", "))
	}
}
