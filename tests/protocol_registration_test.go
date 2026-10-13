// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The per-user urnetwork:// registration (Common/ProtocolRegistration.h) as a C++
// spec run against a fake registry, with negative controls that put each defect
// back into a copy of the header. The registry glue is App/ProtocolHandler.cpp
// (protocol_handler_wiring_test.go).

// Compile the spec (app/tools/protocol-registration-tests.cpp) against
// Common/ProtocolRegistration.h. `mutate`, when set, rewrites a copy of the header
// for a negative control.
func protocolRegistrationTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("protocol registration tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "Common", "ProtocolRegistration.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := lf(string(source))
	if mutate != nil {
		changed := mutate(content)
		if changed == content {
			t.Fatal("negative control did not change the production ProtocolRegistration.h")
		}
		content = changed
	}
	includeDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(includeDir, "ProtocolRegistration.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "protocol-registration-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "protocol-registration-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build protocol registration tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: an absent key is created with the marker first and the
// command last, in the MSI's shape; our own stale or half-written key is written
// again; another program's key is left alone; an unmarked key that already starts
// this exe, or names no handler, is adopted, and so is the owner's development
// key (the MSI's shape around another URnetwork.exe) when the policy says so; an
// unsafe exe path is refused, at the edges of every rule of it; a key whose command
// runs through a COM delegate is never rewritten; a launch refreshes only a key we
// marked and never creates one.
func TestProtocolRegistration(t *testing.T) {
	if !strings.Contains(readCommonSource(t, "Common.vcxproj"), `<ClInclude Include="ProtocolRegistration.h" />`) {
		t.Error("Common.vcxproj does not list ProtocolRegistration.h")
	}
	program := protocolRegistrationTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("protocol registration: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The header names what Ids.h and the installer name, so the per-user key and the
// machine's mean the same thing: the scheme, the description and the empty URL
// Protocol value, and the command's shape (Package.wxs).
func TestProtocolRegistrationNamesMatchTheInstaller(t *testing.T) {
	header := stripComments(lf(readCommonSource(t, "ProtocolRegistration.h")))
	ids := stripComments(lf(readCommonSource(t, "Ids.h")))
	provideRequire(t, "Ids.h", ids, `inline constexpr wchar_t kUriScheme[] = L"urnetwork";`)
	provideRequire(t, "ProtocolRegistration.h", header,
		`inline constexpr std::wstring_view kScheme = L"urnetwork";`,
		`inline constexpr std::wstring_view kClassesKey = L"Software\\Classes\\urnetwork";`,
		`inline constexpr std::wstring_view kDescription = L"URL:URnetwork Protocol";`,
		`inline constexpr std::wstring_view kUrlProtocolValue = L"URL Protocol";`,
		`inline constexpr std::wstring_view kCommandSubkey = L"shell\\open\\command";`,
		`inline constexpr std::wstring_view kExeName = L"URnetwork.exe";`,
		`command.append(L"\" \"%1\"");`)
	for _, text := range []string{"HKEY_LOCAL_MACHINE", "windows.h", "winreg.h", "RegSetKeyValueW", "RegCreateKeyExW"} {
		if strings.Contains(header, text) {
			t.Errorf("ProtocolRegistration.h mentions %q: the header decides, and the glue writes (HKEY_CURRENT_USER only)", text)
		}
	}
	// the installer's values are the ones the header copies
	packagePath := filepath.Join(repositoryRoot(t), "app", "installer", "Package.wxs")
	data, err := os.ReadFile(packagePath)
	if err != nil {
		t.Fatal(err)
	}
	installer := lf(string(data))
	provideRequire(t, "Package.wxs", installer, "URL:URnetwork Protocol",
		`Value="&quot;[INSTALLFOLDER]URnetwork.exe&quot; &quot;%1&quot;"`)
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireProtocolRegistrationFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := protocolRegistrationTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old replaced; a control whose text no
// longer matches fails rather than passing vacuously.
func protocolRegistrationReplace(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("negative control expects exactly one %q in ProtocolRegistration.h", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// Each defect below is one a registrar tends to have. Put it back, and the spec
// must fail naming the check it breaks.
func TestProtocolRegistrationSpecDetectsEachDefectPutBack(t *testing.T) {
	cases := []struct {
		name string
		old  string
		new  string
		want string
	}{
		{
			name: "another program's key is overwritten",
			old:  "  plan.decision = Decision::RefuseForeign;\n  return plan;\n}\n\n// At each launch",
			new:  "  plan.decision = Decision::Repoint;\n  plan.writes = AllWrites(exe, version);\n  return plan;\n}\n\n// At each launch",
			want: "another program's key is refused",
		},
		{
			name: "every unmarked key with a command is adopted",
			old:  "if (hollow || launchesThisExe || launchesAnotherUrnetwork) {",
			new:  "(void)hollow; (void)launchesThisExe; (void)launchesAnotherUrnetwork;\n  if (true) {",
			want: "another program's key is refused",
		},
		{
			name: "the policy is ignored",
			old:  "const bool launchesAnotherUrnetwork = policy.adoptOtherUrnetworkExe && key.command && NamesUrnetworkExe(*key.command);",
			new:  "(void)policy;\n  const bool launchesAnotherUrnetwork = key.command && NamesUrnetworkExe(*key.command);",
			want: "an unmarked key around another URnetwork.exe is refused by default",
		},
		{
			name: "a hollow key is refused",
			old:  "const bool hollow = !key.command.has_value();",
			new:  "const bool hollow = false;",
			want: "a key that names no handler is filled in",
		},
		{
			name: "a file whose name only ends like ours is adopted",
			old:  "return EqualsIgnoreCase(std::wstring_view(*exe).substr(slash + 1), kExeName);",
			new:  "(void)slash;\n  return exe->size() >= kExeName.size() && EqualsIgnoreCase(std::wstring_view(*exe).substr(exe->size() - kExeName.size()), kExeName);",
			want: "a file whose name only ends like ours is another program",
		},
		{
			name: "the command is unquoted",
			old:  "std::wstring command = L\"\\\"\";\n  command.append(exe);\n  command.append(L\"\\\" \\\"%1\\\"\");",
			new:  "std::wstring command;\n  command.append(exe);\n  command.append(L\" %1\");",
			want: "the MSI's shape",
		},
		{
			name: "the marker is written last",
			old: "      {L\"\", std::wstring(kOwnerValue), OwnerTag(exe, version)},\n" +
				"      {L\"\", L\"\", std::wstring(kDescription)},\n" +
				"      {L\"\", std::wstring(kUrlProtocolValue), L\"\"},\n" +
				"      {std::wstring(kCommandSubkey), L\"\", OpenCommand(exe)},",
			new: "      {L\"\", L\"\", std::wstring(kDescription)},\n" +
				"      {L\"\", std::wstring(kUrlProtocolValue), L\"\"},\n" +
				"      {std::wstring(kCommandSubkey), L\"\", OpenCommand(exe)},\n" +
				"      {L\"\", std::wstring(kOwnerValue), OwnerTag(exe, version)},",
			want: "the owner marker is written first",
		},
		{
			name: "the command is written before the rest",
			old: "      {L\"\", std::wstring(kOwnerValue), OwnerTag(exe, version)},\n" +
				"      {L\"\", L\"\", std::wstring(kDescription)},\n" +
				"      {L\"\", std::wstring(kUrlProtocolValue), L\"\"},\n" +
				"      {std::wstring(kCommandSubkey), L\"\", OpenCommand(exe)},",
			new: "      {L\"\", std::wstring(kOwnerValue), OwnerTag(exe, version)},\n" +
				"      {std::wstring(kCommandSubkey), L\"\", OpenCommand(exe)},\n" +
				"      {L\"\", L\"\", std::wstring(kDescription)},\n" +
				"      {L\"\", std::wstring(kUrlProtocolValue), L\"\"},",
			want: "the command is written last",
		},
		{
			name: "our key from another version is left stale",
			old:  "*key.owner == OwnerTag(exe, version);",
			new:  "true;",
			want: "our key from another version is rewritten",
		},
		{
			name: "our key for a moved exe is left stale",
			old:  "const bool current = key.command && EqualsIgnoreCase(*key.command, command) && key.description &&",
			new:  "const bool current = key.description &&",
			want: "a marked key whose command was changed by hand is written again",
		},
		{
			name: "a launch creates the key",
			old:  "  if (!key.exists) return plan;\n  if (!key.owner) {\n    plan.decision = Decision::RefuseForeign;\n    return plan;\n  }\n  return PlanEnsure(key, exe, version);",
			new:  "  if (!key.exists) return PlanEnsure(key, exe, version);\n  if (!key.owner) {\n    plan.decision = Decision::RefuseForeign;\n    return plan;\n  }\n  return PlanEnsure(key, exe, version);",
			want: "refresh: never creates a key",
		},
		{
			name: "a launch adopts a key it did not write",
			old:  "  if (!key.owner) {\n    plan.decision = Decision::RefuseForeign;\n    return plan;\n  }\n  return PlanEnsure(key, exe, version);",
			new:  "  return PlanEnsure(key, exe, version);",
			want: "nor an unmarked key, even one an attempt would adopt",
		},
		{
			name: "a key with a delegate is rewritten",
			old:  "  if (key.delegateExecute) {\n    plan.decision = Decision::RefuseForeign;\n    return plan;\n  }\n  if (key.owner) {",
			new:  "  if (key.owner) {",
			want: "a marked key that gained one is refused",
		},
		{
			name: "an unmarked key with a delegate is adopted",
			old:  "  if (key.delegateExecute) {\n    plan.decision = Decision::RefuseForeign;\n    return plan;\n  }\n  if (key.owner) {",
			new:  "  if (key.owner && key.delegateExecute) {\n    plan.decision = Decision::RefuseForeign;\n    return plan;\n  }\n  if (key.owner) {",
			want: "delegate: nor an unmarked key whose command is ours",
		},
		{
			name: "the drive letter A is no drive",
			old:  "(exe[0] >= L'A' && exe[0] <= L'Z')",
			new:  "(exe[0] > L'A' && exe[0] <= L'Z')",
			want: "the drive letter code 65 is a drive",
		},
		{
			name: "the drive letter Z is no drive",
			old:  "(exe[0] >= L'A' && exe[0] <= L'Z')",
			new:  "(exe[0] >= L'A' && exe[0] < L'Z')",
			want: "the drive letter code 90 is a drive",
		},
		{
			name: "the drive letter a is no drive",
			old:  "(exe[0] >= L'a' && exe[0] <= L'z')",
			new:  "(exe[0] > L'a' && exe[0] <= L'z')",
			want: "the drive letter code 97 is a drive",
		},
		{
			name: "the drive letter z is no drive",
			old:  "(exe[0] >= L'a' && exe[0] <= L'z')",
			new:  "(exe[0] >= L'a' && exe[0] < L'z')",
			want: "the drive letter code 122 is a drive",
		},
		{
			name: "a character beside the capitals is a drive",
			old:  "(exe[0] >= L'A' && exe[0] <= L'Z')",
			new:  "(exe[0] >= L'@' && exe[0] <= L'Z')",
			want: "the character code 64 is no drive",
		},
		{
			name: "the last control character is allowed in a path",
			old:  "if (c < 0x20 || c == L'\"'",
			new:  "if (c < 0x1f || c == L'\"'",
			want: "the last of them, 0x1f",
		},
		{
			name: "a space is a control character",
			old:  "if (c < 0x20 || c == L'\"'",
			new:  "if (c <= 0x20 || c == L'\"'",
			want: "a space is not a control character",
		},
		{
			name: "a path is safe at any length",
			old:  "exe.size() > 32000",
			new:  "exe.size() > 64000",
			want: "32001 are not",
		},
		{
			name: "a path of the longest length is not safe",
			old:  "exe.size() > 32000",
			new:  "exe.size() >= 32000",
			want: "32000 characters are the longest that is safe",
		},
		{
			name: "a file named only .exe is a handler",
			old:  "return exe.size() - slash - 1 > 4;",
			new:  "return exe.size() - slash - 1 >= 4;",
			want: "a file named just .exe is refused",
		},
		{
			name: "the capital Z is not lowered",
			old:  "(c >= L'A' && c <= L'Z') ? static_cast<wchar_t>",
			new:  "(c >= L'A' && c < L'Z') ? static_cast<wchar_t>",
			want: "the capitals A to Z are lowered",
		},
		{
			name: "the capital A is not lowered",
			old:  "(c >= L'A' && c <= L'Z') ? static_cast<wchar_t>",
			new:  "(c > L'A' && c <= L'Z') ? static_cast<wchar_t>",
			want: "the capitals A to Z are lowered",
		},
		{
			name: "paths are compared from their second character",
			old:  "  for (std::size_t i = 0; i < a.size(); ++i) {\n    if (Lower(a[i]) != Lower(b[i])) return false;",
			new:  "  for (std::size_t i = 1; i < a.size(); ++i) {\n    if (Lower(a[i]) != Lower(b[i])) return false;",
			want: "a different letter at the first place",
		},
		{
			name: "a relative or UNC path is safe",
			old:  "  if (!drive) return false;\n",
			new:  "  (void)drive;\n",
			want: "an unsafe exe path is refused",
		},
		{
			name: "a quote in the path is safe",
			old:  "if (c < 0x20 || c == L'\"' || c == L'%'",
			new:  "if (c < 0x20 || c == L'%'",
			want: "an unsafe exe path is refused",
		},
		{
			name: "a percent sign in the path is safe",
			old:  "|| c == L'%' || c == L'<'",
			new:  "|| c == L'<'",
			want: "an unsafe exe path is refused",
		},
		{
			name: "a stream name in the path is safe",
			old:  "    if (c == L':' && i != 1) return false;\n",
			new:  "",
			want: "an unsafe exe path is refused",
		},
		{
			name: "a dll is a handler",
			old:  "  if (!EqualsIgnoreCase(exe.substr(exe.size() - 4), L\".exe\")) return false;\n",
			new:  "",
			want: "an unsafe exe path is refused",
		},
		{
			name: "a token that can be the id of a string resource",
			old:  `case Decision::Create: return "created";`,
			new:  `case Decision::Create: return "create";`,
			want: "the words the log reads",
		},
		{
			name: "the shortest exe a command can hold is not read",
			old:  "command.size() <= kTail.size() + 1",
			new:  "command.size() <= kTail.size() + 2",
			want: "names: the shortest exe a command can hold comes back",
		},
		{
			name: "a command with no exe in it gives an empty one",
			old:  "command.size() <= kTail.size() + 1",
			new:  "command.size() < kTail.size() + 1",
			want: "names: a command with no exe in it gives none",
		},
		{
			name: "a key that was never read says it is a protocol",
			old:  "  bool urlProtocol = false;                 // the \"URL Protocol\" value is there",
			new:  "  bool urlProtocol = true;                  // the \"URL Protocol\" value is there",
			want: "key: a key that was never read says nothing",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			requireProtocolRegistrationFailure(t, func(source string) string {
				return protocolRegistrationReplace(t, source, c.old, c.new)
			}, c.want)
		})
	}
}
