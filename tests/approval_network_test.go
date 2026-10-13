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

// Which network a browser approval is for, and how a request names it (App/ApprovalNetwork.h; contract version 2, section 11)
// as a C++ spec: the grammar of `api` class by class and against a second reading of it, the host an API url names, the
// network a space is, and the contract version each network needs. Each negative control puts one defect back into a copy
// of the header and requires the spec to fail naming it. How the request is built from it is ur_io_approval_test.go, how
// the page's descriptor is read is approval_descriptor_test.go, and how the SDK host and the controller use it is
// ur_io_approval_wiring_test.go.

// Compile the spec (app/tools/approval-network-tests.cpp) against App/ApprovalNetwork.h. `mutate`, when set, rewrites a copy
// of the header for a negative control.
func approvalNetworkTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("approval network tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "ApprovalNetwork.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := lf(string(source))
	if mutate != nil {
		changed := mutate(content)
		if changed == content {
			t.Fatal("negative control did not change the production ApprovalNetwork.h")
		}
		content = changed
	}
	includeDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(includeDir, "ApprovalNetwork.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "approval-network-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "approval-network-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build approval network tests: %v\n%s", err, output)
	}
	return program
}

func TestApprovalNetwork(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="ApprovalNetwork.h" />`) {
		t.Error("App.vcxproj does not list ApprovalNetwork.h")
	}
	if _, err := os.Stat(filepath.Join(repositoryRoot(t), "app", "tools", "approval-network-tests.cpp")); err != nil {
		t.Fatal(err)
	}
	program := approvalNetworkTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("approval network: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The header is pure: standard C++ and nothing else, so the spec runs on any host, and it logs and decides nothing about
// Windows. It has no check code and no word for the log (the network is never named in a log: the lines that tell what
// was offered say a version, which is a number).
func TestApprovalNetworkHeaderIsPure(t *testing.T) {
	source := lf(readAppSource(t, "ApprovalNetwork.h"))
	allowed := map[string]bool{"<cstddef>": true, "<cstdint>": true, "<string>": true, "<string_view>": true, "<utility>": true}
	includes := regexp.MustCompile(`(?m)^[ \t]*#[ \t]*include[ \t]*([<"][^>"]+[>"])`).FindAllStringSubmatch(source, -1)
	if len(includes) == 0 {
		t.Fatal("ApprovalNetwork.h includes nothing")
	}
	for _, include := range includes {
		if !allowed[include[1]] {
			t.Errorf("ApprovalNetwork.h includes %s: it is standard C++ only, so that it runs on any host", include[1])
		}
	}
	code := stripComments(source)
	for _, forbidden := range []string{
		"winrt::", "windows.h", "urnet::", "urnetwork_sdk", "std::format", "std::regex", "LogInfo", "LogWarn", "LogError", "LogDebug",
		"nlohmann", "getenv", "ToString(",
	} {
		if strings.Contains(code, forbidden) {
			t.Errorf("ApprovalNetwork.h mentions %q: the grammar and the network are pure, and nothing here names a host in a log", forbidden)
		}
	}
}

func requireApprovalNetworkFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := approvalNetworkTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// Each defect below is one a reader of a host tends to have. Put it back, and the spec must fail naming the check it breaks.
func TestApprovalNetworkSpecDetectsEachDefectPutBack(t *testing.T) {
	cases := []struct {
		name string
		old  string
		new  string
		want string
	}{
		// the alphabet of a label
		{
			name: "upper case is taken",
			old:  "constexpr bool IsLowerOrDigit(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }",
			new:  "constexpr bool IsLowerOrDigit(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }",
			want: "upper case (API.example.com)",
		},
		{
			name: "the letter z is not a host character",
			old:  "(c >= 'a' && c <= 'z')",
			new:  "(c >= 'a' && c < 'z')",
			want: "alphabet: byte 122 inside a label is allowed",
		},
		{
			name: "the letter a is not a host character",
			old:  "(c >= 'a' && c <= 'z')",
			new:  "(c > 'a' && c <= 'z')",
			want: "alphabet: byte 97 inside a label is allowed",
		},
		{
			name: "the digit 9 is not a host character",
			old:  "(c >= '0' && c <= '9')",
			new:  "(c >= '0' && c < '9')",
			want: "alphabet: byte 57 inside a label is allowed",
		},
		{
			name: "the digit 0 is not a host character",
			old:  "(c >= '0' && c <= '9')",
			new:  "(c > '0' && c <= '9')",
			want: "alphabet: byte 48 inside a label is allowed",
		},
		{
			name: "the characters after the digits are taken",
			old:  "(c >= '0' && c <= '9')",
			new:  "(c >= '0' && c <= '@')",
			want: "a semicolon",
		},
		{
			name: "an underscore is taken",
			old:  "if (!IsLowerOrDigit(c) && c != '-') return false;",
			new:  "if (!IsLowerOrDigit(c) && c != '-' && c != '_') return false;",
			want: "an underscore",
		},
		{
			name: "any character is taken inside a label",
			old:  "    if (!IsLowerOrDigit(c) && c != '-') return false;\n",
			new:  "    (void)c;\n",
			want: "alphabet: byte 37 inside a label is refused",
		},
		{
			name: "a label may start with a hyphen",
			old:  "  if (!IsLowerOrDigit(label.front()) || !IsLowerOrDigit(label.back())) return false;",
			new:  "  if (!IsLowerOrDigit(label.back())) return false;",
			want: "a label that starts with a hyphen",
		},
		{
			name: "a label may end with a hyphen",
			old:  "  if (!IsLowerOrDigit(label.front()) || !IsLowerOrDigit(label.back())) return false;",
			new:  "  if (!IsLowerOrDigit(label.front())) return false;",
			want: "a label that ends with a hyphen",
		},
		{
			name: "a label of 64 characters is taken",
			old:  "label.size() > kMaxApiLabelChars",
			new:  "label.size() > kMaxApiLabelChars + 1",
			want: "a first label of 64 characters is no host",
		},
		{
			name: "a label of 63 characters is too long",
			old:  "inline constexpr std::size_t kMaxApiLabelChars = 63;",
			new:  "inline constexpr std::size_t kMaxApiLabelChars = 62;",
			want: "a first label of 63 characters is a host",
		},
		// the labels of a host
		{
			name: "a single label is a host",
			old:  "return labels >= 2 && !detail::IsAllDigits(last) && !detail::IsHexNumber(last);",
			new:  "return labels >= 1 && !detail::IsAllDigits(last) && !detail::IsHexNumber(last);",
			want: "a single label is no host",
		},
		{
			name: "three labels are needed",
			old:  "return labels >= 2 && !detail::IsAllDigits(last) && !detail::IsHexNumber(last);",
			new:  "return labels >= 3 && !detail::IsAllDigits(last) && !detail::IsHexNumber(last);",
			want: "two labels is a host",
		},
		{
			name: "an IP literal is a host",
			old:  "return labels >= 2 && !detail::IsAllDigits(last) && !detail::IsHexNumber(last);",
			new:  "return labels >= 2 && !detail::IsHexNumber(last);",
			want: "an IPv4 literal (the last label is all digits) is no host",
		},
		// the last label that a browser reads as a number: all digits, or 0x and hex digits
		{
			name: "a hex number is a last label like another",
			old:  "return labels >= 2 && !detail::IsAllDigits(last) && !detail::IsHexNumber(last);",
			new:  "return labels >= 2 && !detail::IsAllDigits(last);",
			want: "a last label that is 0x and hex digits is no host",
		},
		{
			name: "every label is held to being no hex number",
			old:  "    if (!detail::IsApiLabel(last)) return false;",
			new:  "    if (!detail::IsApiLabel(last) || detail::IsHexNumber(last)) return false;",
			want: "a first label that is 0x and hex digits is a host",
		},
		{
			name: "0x alone is a label",
			old:  "  if (text.size() < 2 || text[0] != '0' || text[1] != 'x') return false;",
			new:  "  if (text.size() < 3 || text[0] != '0' || text[1] != 'x') return false;",
			want: "a last label of 0x is no host",
		},
		{
			name: "the hex number starts with a different letter",
			old:  "  if (text.size() < 2 || text[0] != '0' || text[1] != 'x') return false;",
			new:  "  if (text.size() < 2 || text[0] != '0' || text[1] != 'y') return false;",
			want: "last label: 0x10 is a number, so no host",
		},
		{
			name: "any text that starts with a zero is a hex number",
			old:  "  if (text.size() < 2 || text[0] != '0' || text[1] != 'x') return false;",
			new:  "  if (text.size() < 2 || text[0] != '0') return false;",
			want: "is a name (a letter, and no 0x), so a host",
		},
		{
			name: "the digit 9 is no hex digit",
			old:  "    if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;",
			new:  "    if ((c < '0' || c > '8') && (c < 'a' || c > 'f')) return false;",
			want: "last label: 0x9 is a number, so no host",
		},
		{
			name: "the letter f is no hex digit",
			old:  "    if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;",
			new:  "    if ((c < '0' || c > '9') && (c < 'a' || c > 'e')) return false;",
			want: "last label: 0xf is a number, so no host",
		},
		{
			name: "the letter g is a hex digit",
			old:  "    if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;",
			new:  "    if ((c < '0' || c > '9') && (c < 'a' || c > 'g')) return false;",
			want: "a last label that is 0x and a letter that is not a hex digit is a host",
		},
		{
			name: "the letter a is no hex digit",
			old:  "    if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;",
			new:  "    if ((c < '0' || c > '9') && (c < 'b' || c > 'f')) return false;",
			want: "last label: 0xa is a number, so no host",
		},
		{
			name: "any character after 0x is a hex digit",
			old:  "    if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;\n",
			new:  "    (void)c;\n",
			want: "a last label that is 0x and a letter that is not a hex digit is a host",
		},
		{
			name: "an empty label is a label",
			old:  "    if (!detail::IsApiLabel(last)) return false;",
			new:  "    if (!last.empty() && !detail::IsApiLabel(last)) return false;",
			want: "a trailing dot is no host",
		},
		{
			name: "a host of 201 characters is taken",
			old:  "host.size() > kMaxApiChars",
			new:  "host.size() > kMaxApiChars + 1",
			want: "a host of 201 characters is no host",
		},
		{
			name: "the cap is 201 characters",
			old:  "inline constexpr std::size_t kMaxApiChars = 200;",
			new:  "inline constexpr std::size_t kMaxApiChars = 201;",
			want: "constants: 200 characters",
		},
		{
			name: "a host of 200 characters is too long",
			old:  "host.size() > kMaxApiChars",
			new:  "host.size() >= kMaxApiChars",
			want: "a host of exactly 200 characters is a host",
		},
		// the port
		{
			name: "the port is not looked at",
			old:  "    if (!detail::IsApiPort(host.substr(colon + 1))) return false;\n",
			new:  "",
			want: "port 443 is no host",
		},
		{
			name: "the port stays in the name",
			old:  "    name = host.substr(0, colon);\n",
			new:  "",
			want: "a host with a port (the contract's second example) is a host",
		},
		{
			name: "port 443 is taken",
			old:  "  return value <= kMaxApiPort && value != kHttpsPort;",
			new:  "  return value <= kMaxApiPort;",
			want: "port 443 is no host",
		},
		{
			name: "the port of https is another",
			old:  "inline constexpr unsigned kHttpsPort = 443;",
			new:  "inline constexpr unsigned kHttpsPort = 444;",
			want: "port 443 is no host",
		},
		{
			name: "port 65536 is taken",
			old:  "  return value <= kMaxApiPort && value != kHttpsPort;",
			new:  "  return value <= kMaxApiPort + 1 && value != kHttpsPort;",
			want: "port 65536 is no host",
		},
		{
			name: "port 65535 is too much",
			old:  "  return value <= kMaxApiPort && value != kHttpsPort;",
			new:  "  return value < kMaxApiPort && value != kHttpsPort;",
			want: "port 65535 is a host",
		},
		{
			name: "a leading zero is taken",
			old:  "  if (!IsAllDigits(port) || port.size() > 5 || port.front() == '0') return false;",
			new:  "  if (!IsAllDigits(port) || port.size() > 5) return false;",
			want: "a leading-zero port is no host",
		},
		{
			name: "port 0 is taken",
			old:  "  if (!IsAllDigits(port) || port.size() > 5 || port.front() == '0') return false;",
			new:  "  if (!IsAllDigits(port) || port.size() > 5 || (port.size() > 1 && port.front() == '0')) return false;",
			want: "port 0 is no host",
		},
		{
			name: "a port of five digits is too long",
			old:  "  if (!IsAllDigits(port) || port.size() > 5 || port.front() == '0') return false;",
			new:  "  if (!IsAllDigits(port) || port.size() > 4 || port.front() == '0') return false;",
			want: "port 10000 is a host",
		},
		{
			name: "a port of any length is added up",
			old:  "  if (!IsAllDigits(port) || port.size() > 5 || port.front() == '0') return false;",
			new:  "  if (!IsAllDigits(port) || port.front() == '0') return false;",
			want: "a ten-digit port that wraps around to 445",
		},
		{
			name: "a neighbour of the digit 9 is a digit",
			old:  "    if (c < '0' || c > '9') return false;\n  }\n  return !text.empty();",
			new:  "    if (c < '0' || c > ':') return false;\n  }\n  return !text.empty();",
			want: "port: byte 58 between two digits",
		},
		{
			name: "a port may have a letter in it",
			old:  "  if (!IsAllDigits(port) || port.size() > 5 || port.front() == '0') return false;",
			new:  "  if (port.empty() || port.size() > 5 || port.front() == '0') return false;",
			want: "a port with a letter is no host",
		},
		{
			name: "the digits of a port are summed wrong",
			old:  "value = value * 10 + static_cast<unsigned>(c - '0')",
			new:  "value = value * 9 + static_cast<unsigned>(c - '0')",
			want: "port 443 is no host",
		},
		{
			name: "the digit 9 is no digit",
			old:  "    if (c < '0' || c > '9') return false;\n  }\n  return !text.empty();",
			new:  "    if (c < '0' || c > '8') return false;\n  }\n  return !text.empty();",
			want: "port: 9",
		},
		{
			name: "any text is all digits",
			old:  "    if (c < '0' || c > '9') return false;\n  }\n  return !text.empty();",
			new:  "    (void)c;\n  }\n  return !text.empty();",
			want: "the conformance vector's api is a host",
		},
		// the url
		{
			name: "the api url is read as http",
			old:  `constexpr std::string_view kScheme = "https://";`,
			new:  `constexpr std::string_view kScheme = "http://";`,
			want: "url: the vector's api url names its host",
		},
		{
			name: "the scheme of an api url is not compared",
			old:  "  if (host.substr(0, kScheme.size()) != kScheme) return {};",
			new:  "  if (host.size() < kScheme.size()) return {};",
			want: "url: an http api names nothing",
		},
		{
			name: "capitals are a different scheme and host",
			old:  "    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');\n",
			new:  "    (void)c;\n",
			want: "url: a scheme in capitals names api.example.com",
		},
		{
			name: "the url is read as it was given, not as it was folded",
			old:  "  std::string_view host = url;\n",
			new:  "  std::string_view host = apiUrl;\n",
			want: "url: a scheme in capitals names api.example.com",
		},
		{
			name: "only the capitals of the first half of the alphabet are folded",
			old:  "if (c >= 'A' && c <= 'Z') c = ",
			new:  "if (c >= 'A' && c <= 'M') c = ",
			want: "url: an upper-case host names api.example.com",
		},
		{
			name: "port 443 stays in the name",
			old:  `constexpr std::string_view kOwnPort = ":443";`,
			new:  `constexpr std::string_view kOwnPort = ":444";`,
			want: "url: port 443 spelled out names api.example.com",
		},
		{
			name: "https' own port is looked for without its colon",
			old:  `constexpr std::string_view kOwnPort = ":443";`,
			new:  `constexpr std::string_view kOwnPort = "443";`,
			want: "url: port 443 spelled out names api.example.com",
		},
		{
			name: "port 443 is dropped after another port",
			old:  "    if (name.find(':') == std::string_view::npos) host = name;",
			new:  "    host = name;",
			want: "url: port 443 after another port names nothing",
		},
		{
			name: "the whole name is dropped with port 443",
			old:  "    if (name.find(':') == std::string_view::npos) host = name;",
			new:  "    if (name.find(':') == std::string_view::npos) host = host.substr(0, 0);",
			want: "url: port 443 spelled out names api.example.com",
		},
		{
			name: "a trailing slash is a different host",
			old:  "  if (!host.empty() && host.back() == '/') host.remove_suffix(1);\n",
			new:  "",
			want: "url: one trailing slash is allowed",
		},
		{
			name: "any number of trailing slashes is allowed",
			old:  "  if (!host.empty() && host.back() == '/') host.remove_suffix(1);",
			new:  "  while (!host.empty() && host.back() == '/') host.remove_suffix(1);",
			want: "url: two trailing slashes names nothing",
		},
		{
			name: "the host of an api url is not held to the grammar",
			old:  "  return IsValidApiHost(host) ? std::string(host) : std::string();",
			new:  "  return std::string(host);",
			want: "url: a path names nothing",
		},
		// the network
		{
			name: "the official network names itself",
			old:  "return Network{NetworkKind::Official, std::string()};",
			new:  "return Network{NetworkKind::Official, api};",
			want: "network: the official space is official and names nothing (url 'https://api.bringyour.com')",
		},
		{
			name: "the official space is another network",
			old:  "return Network{NetworkKind::Official, std::string()};",
			new:  "return Network{NetworkKind::Custom, std::string()};",
			want: "network: the official space is official and names nothing",
		},
		{
			name: "the operator's host decides alone, whatever its api is",
			old:  `  if (officialSpace && api == "api." + std::string(operatorHost)) return`,
			new: `  (void)operatorHost;
  if (officialSpace) return`,
			want: "the operator's space whose api url is 'https://dev-api.bringyour.com' is custom, and names that api",
		},
		{
			name: "a space is official by its api alone",
			old:  `  if (officialSpace && api == "api." + std::string(operatorHost)) return`,
			new: `  (void)officialSpace;
  if (api == "api." + std::string(operatorHost)) return`,
			want: "network: a space that is not the operator's but uses the operator's api names it",
		},
		{
			name: "the operator's api is another service name",
			old:  `api == "api." + std::string(operatorHost)`,
			new:  `api == "connect." + std::string(operatorHost)`,
			want: "network: the official space is official and names nothing",
		},
		{
			name: "the operator's host is one this header knows",
			old:  `api == "api." + std::string(operatorHost)`,
			new:  `api == "api.bringyour.com" && !operatorHost.empty()`,
			want: "network: another operator's own api is the official one for it",
		},
		{
			name: "any api under the operator's host is the operator's",
			old:  `api == "api." + std::string(operatorHost)`,
			new:  `api.size() > operatorHost.size() && api.compare(api.size() - operatorHost.size(), operatorHost.size(), operatorHost) == 0`,
			want: "the operator's space whose api url is 'https://dev-api.bringyour.com' is custom, and names that api",
		},
		{
			name: "an unreadable url of the official space is official",
			old:  "  std::string api = ApiHostOfUrl(liveApiUrl);\n  if (api.empty()) return Network{};\n  if (officialSpace &&",
			new:  "  std::string api = ApiHostOfUrl(liveApiUrl);\n  if (officialSpace && api.empty()) return Network{NetworkKind::Official, std::string()};\n  if (api.empty()) return Network{};\n  if (officialSpace &&",
			want: "the operator's space whose api url is 'garbage' names nothing",
		},
		{
			name: "a url that names nothing is a network",
			old:  "  if (api.empty()) return Network{};\n",
			new:  "",
			want: "network: a space whose api url is '' names nothing",
		},
		{
			name: "a network does not carry the name",
			old:  "  return Network{NetworkKind::Custom, std::move(api)};",
			new:  "  return Network{NetworkKind::Custom, std::string()};",
			want: "network: another space is custom and names its API host",
		},
		{
			name: "two networks are the same by their kind",
			old:  "  friend bool operator==(const Network&, const Network&) = default;",
			new:  "  friend bool operator==(const Network& a, const Network& b) { return a.kind == b.kind; }",
			want: "network: two networks are the same if the kind and the name are",
		},
		// the contract each network needs
		{
			name: "the official network needs version 2",
			old:  "    case NetworkKind::Official: return kContract;",
			new:  "    case NetworkKind::Official: return kContractNamed;",
			want: "contract: the official network is version 1",
		},
		{
			name: "another network needs version 1",
			old:  "    case NetworkKind::Custom: return kContractNamed;",
			new:  "    case NetworkKind::Custom: return kContract;",
			want: "contract: another network is version 2",
		},
		{
			name: "no network needs a version",
			old:  "    case NetworkKind::None: return 0;",
			new:  "    case NetworkKind::None: return kContract;",
			want: "contract: no network is no version",
		},
		{
			name: "the second version is the third",
			old:  "inline constexpr std::int64_t kContractNamed = 2;",
			new:  "inline constexpr std::int64_t kContractNamed = 3;",
			want: "contract: the versions this app speaks are 1 and 2",
		},
		{
			name: "the first version is the zeroth",
			old:  "inline constexpr std::int64_t kContract = 1;",
			new:  "inline constexpr std::int64_t kContract = 0;",
			want: "contract: the versions this app speaks are 1 and 2",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			requireApprovalNetworkFailure(t, func(source string) string {
				if strings.Count(source, c.old) != 1 {
					t.Fatalf("negative control expects exactly one %q in ApprovalNetwork.h", c.old)
				}
				return strings.Replace(source, c.old, c.new, 1)
			}, c.want)
		})
	}
}
