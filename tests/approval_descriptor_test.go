// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The page's descriptor (App/ApprovalDescriptor.h) as a C++ spec: when a response
// from the approval page makes the app offer "Continue in browser", and every way
// it does not. Skips without nlohmann/json, as the other readers of a server's
// JSON do. The request that fetches it is App/ApprovalCheck.cpp
// (ur_io_approval_wiring_test.go).

// Compile the spec (app/tools/approval-descriptor-tests.cpp) against
// App/ApprovalDescriptor.h and the header it includes (App/ApprovalNetwork.h).
// `mutate`, when set, rewrites a copy of ApprovalDescriptor.h for a negative control.
func approvalDescriptorTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("approval descriptor tests require a C++20 compiler: ", err)
	}
	jsonFlags := provideJsonFlags(t)
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, name := range []string{"ApprovalNetwork.h", "ApprovalDescriptor.h"} {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", name))
		if err != nil {
			t.Fatal(err)
		}
		content := lf(string(source))
		if mutate != nil && name == "ApprovalDescriptor.h" {
			changed := mutate(content)
			if changed == content {
				t.Fatal("negative control did not change the production ApprovalDescriptor.h")
			}
			content = changed
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "approval-descriptor-tests")
	args := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir}
	args = append(args, jsonFlags...)
	args = append(args, filepath.Join(root, "app", "tools", "approval-descriptor-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build approval descriptor tests: %v\n%s", err, output)
	}
	return program
}

func TestApprovalDescriptor(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="ApprovalDescriptor.h" />`) {
		t.Error("App.vcxproj does not list ApprovalDescriptor.h")
	}
	program := approvalDescriptorTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("approval descriptor: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Each defect below is a looser reader of the descriptor. Put it back, and the
// spec must fail naming the check it breaks.
func TestApprovalDescriptorSpecDetectsEachDefectPutBack(t *testing.T) {
	cases := []struct {
		name string
		old  string
		new  string
		want string
	}{
		{
			name: "any content type is taken",
			old:  "  if (!detail::IsJsonContentType(contentType)) return DescriptorVerdict::WrongContentType;\n",
			new:  "  (void)contentType;\n",
			want: "text/html",
		},
		{
			name: "a content type that merely starts with application/json is taken",
			old:  "return rest.empty() || rest.front() == ';';",
			new:  "return true;",
			want: "application/jsonp",
		},
		{
			name: "the content type is compared with its case",
			old:  "constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }",
			new:  "constexpr char Lower(char c) { return c; }",
			want: "upper case",
		},
		{
			name: "the media type is only measured, not compared",
			old:  "    if (Lower(contentType[i]) != kJson[i]) return false;\n",
			new:  "",
			want: "application/yaml (the same length)",
		},
		{
			name: "the media type is compared from its second character",
			old:  "  for (std::size_t i = 0; i < kJson.size(); ++i) {\n    if (Lower(contentType[i]) != kJson[i]) return false;",
			new:  "  for (std::size_t i = 1; i < kJson.size(); ++i) {\n    if (Lower(contentType[i]) != kJson[i]) return false;",
			want: "the first character differs",
		},
		{
			name: "the media type is compared to its second last character",
			old:  "  for (std::size_t i = 0; i < kJson.size(); ++i) {\n    if (Lower(contentType[i]) != kJson[i]) return false;",
			new:  "  for (std::size_t i = 0; i + 1 < kJson.size(); ++i) {\n    if (Lower(contentType[i]) != kJson[i]) return false;",
			want: "the last character differs",
		},
		{
			name: "a body of any size is read",
			old:  "  if (body.size() > kMaxDescriptorBytes) return DescriptorVerdict::TooLarge;\n",
			new:  "",
			want: "a body over the cap",
		},
		{
			name: "a document that is not an object is read",
			old:  "if (document.is_discarded() || !document.is_object()) return DescriptorVerdict::NotJson;",
			new:  "if (document.is_discarded()) return DescriptorVerdict::NotJson;",
			want: "an array holding the document",
		},
		{
			name: "the contract may be a float",
			old:  "if (entry.is_number_integer() && entry.get<std::int64_t>() == contract) listsContract = true;",
			new:  "if (entry.is_number() && entry.get<double>() == static_cast<double>(contract)) listsContract = true;",
			want: "a float is not the integer",
		},
		{
			name: "the contract may be a number on its own",
			old:  "  if (contracts != document.end() && contracts->is_array()) {",
			new:  "  if (contracts != document.end() && contracts->is_number_integer() && contracts->get<std::int64_t>() == contract) listsContract = true;\n  if (contracts != document.end() && contracts->is_array()) {",
			want: "a number, not a list",
		},
		{
			name: "the contract may be the string one",
			old:  "if (entry.is_number_integer() && entry.get<std::int64_t>() == contract) listsContract = true;",
			new:  "if (entry.dump() == std::to_string(contract) || entry.dump() == \"\\\"\" + std::to_string(contract) + \"\\\"\") listsContract = true;",
			want: "the number as a string",
		},
		{
			name: "a platform that only starts like ours is taken",
			old:  "if (entry.is_string() && entry.get<std::string>() == platform) listsPlatform = true;",
			new:  "if (entry.is_string() && entry.get<std::string>().rfind(platform, 0) == 0) listsPlatform = true;",
			want: "a longer name",
		},
		{
			name: "the platform is not required",
			old:  "return listsPlatform ? DescriptorVerdict::Offered : DescriptorVerdict::NoPlatform;",
			new:  "(void)listsPlatform;\n  return DescriptorVerdict::Offered;",
			want: "another platform",
		},
		{
			name: "a token that can be the id of a string resource",
			old:  `case DescriptorVerdict::NoContracts: return "no-contract";`,
			new:  `case DescriptorVerdict::NoContracts: return "no contract";`,
			want: "lowercase letters and hyphens only",
		},
		{
			// the cases are written against the constant, so only the constant's own value says how big a body may be
			name: "the cap on the body is a byte wider",
			old:  "inline constexpr std::size_t kMaxDescriptorBytes = 4096;",
			new:  "inline constexpr std::size_t kMaxDescriptorBytes = 4097;",
			want: "constants: the cap on the body is 4096 bytes",
		},
		{
			name: "a blank in front of the media type takes the first letter with it",
			old:  "contentType.front() == '\\t')) contentType.remove_prefix(1);",
			new:  "contentType.front() == '\\t')) contentType.remove_prefix(2);",
			want: "one leading blank",
		},
		{
			// what the page lists, per version (contract section 11): the official network needs 1, another network needs 2
			name: "version 2 is never read from the page",
			old:  "  if (named == DescriptorVerdict::Offered) out.contracts |= 1U << kContractNamed;\n",
			new:  "",
			want: "listing: a page that lists 2 only serves another network only",
		},
		{
			name: "version 1 is never read from the page",
			old:  "  if (official == DescriptorVerdict::Offered) out.contracts |= 1U << kContract;\n",
			new:  "",
			want: "listing: an older page lists the official network's version only, and is never sent version 2",
		},
		{
			name: "the two versions' bits are swapped",
			old:  "out.contracts |= 1U << kContract;",
			new:  "out.contracts |= 1U << kContractNamed;",
			want: "listing: an older page lists the official network's version only, and is never sent version 2",
		},
		{
			name: "version 2 is judged as version 1",
			old:  "const DescriptorVerdict named = CheckDescriptor(contentType, body, kContractNamed, platform);",
			new:  "const DescriptorVerdict named = CheckDescriptor(contentType, body, kContract, platform);",
			want: "listing: a page that lists 2 only serves another network only",
		},
		{
			name: "version 1 is judged as version 2",
			old:  "const DescriptorVerdict official = CheckDescriptor(contentType, body, kContract, platform);",
			new:  "const DescriptorVerdict official = CheckDescriptor(contentType, body, kContractNamed, platform);",
			want: "listing: an older page lists the official network's version only, and is never sent version 2",
		},
		{
			name: "the platform is not asked of the versions",
			old:  "const DescriptorVerdict named = CheckDescriptor(contentType, body, kContractNamed, platform);",
			new:  "const DescriptorVerdict named = CheckDescriptor(contentType, body, kContractNamed, \"linux\");",
			want: "listing: both versions, another platform",
		},
		{
			name: "a page is offered only when it lists the official network's version",
			old:  "  if (out.contracts != 0) {\n    out.verdict = DescriptorVerdict::Offered;",
			new:  "  if (official == DescriptorVerdict::Offered) {\n    out.verdict = DescriptorVerdict::Offered;",
			want: "listing: a page that lists 2 only serves another network only",
		},
		{
			name: "a page that lists no version is offered",
			old:  "  if (out.contracts != 0) {\n    out.verdict = DescriptorVerdict::Offered;",
			new:  "  if (true) {\n    out.verdict = DescriptorVerdict::Offered;",
			want: "listing: only a version the app does not speak",
		},
		{
			name: "a platform the page lacks is not the word when version 2 was in order",
			old:  "out.verdict = named == DescriptorVerdict::NoPlatform ? DescriptorVerdict::NoPlatform : official;",
			new:  "out.verdict = official;",
			want: "listing: version 2 and another platform: the word is the platform's, the list was in order",
		},
		{
			name: "the word is always the platform's",
			old:  "out.verdict = named == DescriptorVerdict::NoPlatform ? DescriptorVerdict::NoPlatform : official;",
			new:  "out.verdict = DescriptorVerdict::NoPlatform;",
			want: "listing: another version and another platform: the versions come first",
		},
		{
			name: "a version outside the bit set is read from it",
			old:  "return contract >= 0 && contract < 32 && ((contracts >> contract) & 1U) != 0;",
			new:  "return contract >= 0 && contract < 3 && ((contracts >> contract) & 1U) != 0;",
			want: "listing: a version outside 0 to 31 is not listed, however the bits stand",
		},
		{
			name: "the bit of a version is read from the wrong place",
			old:  "((contracts >> contract) & 1U) != 0;",
			new:  "((contracts >> (contract + 1)) & 1U) != 0;",
			want: "listing: the bit of a version is 1 shifted by it",
		},
		{
			name: "a page that was never read lists its versions",
			old:  "  std::uint32_t contracts = 0;                            // bit n is set when the page lists version n",
			new:  "  std::uint32_t contracts = 6;                            // bit n is set when the page lists version n",
			want: "listing: nothing is listed until a page has been read",
		},
		{
			name: "a page that was never read is offered",
			old:  "  DescriptorVerdict verdict = DescriptorVerdict::NotJson;  // Offered if the page lists this platform and either version",
			new:  "  DescriptorVerdict verdict = DescriptorVerdict::Offered;  // Offered if the page lists this platform and either version",
			want: "listing: nothing is listed until a page has been read",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			program := approvalDescriptorTestProgram(t, func(source string) string {
				if strings.Count(source, c.old) != 1 {
					t.Fatalf("negative control expects exactly one %q in ApprovalDescriptor.h", c.old)
				}
				return strings.Replace(source, c.old, c.new, 1)
			})
			output, err := exec.Command(program).CombinedOutput()
			if err == nil || !strings.Contains(string(output), c.want) {
				t.Fatalf("negative control was not detected (want %q): %v\n%s", c.want, err, output)
			}
		})
	}
}
