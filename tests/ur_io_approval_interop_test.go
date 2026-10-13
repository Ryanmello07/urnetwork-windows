// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// The page's own conformance vector (app/tools/approval-vector.h: fixed keys that protect nothing), opened by the
// sdk's GenerateSharedSecret and DecryptData and by the app's own function that wraps them, approval::Unseal
// (App/ApprovalUnseal.h), as a C++ program that links the sdk. It needs the sdk's header, nlohmann/json and the
// sdk library itself (URnetworkSdk.dll, which the wrapper calls through the C ABI), so it runs on Windows where
// the repository's vendored sdk (app/third_party/urnetwork-sdk/<arch>, or URNETWORK_SDK_INCLUDE) is, and skips
// elsewhere, as the other tests against the sdk's header do. The page's side of the vector is the manager's
// tests/fixtures/app-auth-vector.json and its tests/app-auth-vector.test.mjs.

// The directory of an sdk that has the header AND the library, or "".
func approvalInteropSdkDir(t *testing.T, root string) string {
	t.Helper()
	candidates := []string{}
	if explicit := os.Getenv("URNETWORK_SDK_INCLUDE"); explicit != "" {
		candidates = append(candidates, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		candidates = append(candidates, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	for _, dir := range candidates {
		header, headerErr := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		_, libraryErr := os.Stat(filepath.Join(dir, "URnetworkSdk.dll"))
		if headerErr == nil && libraryErr == nil && strings.Contains(string(header), "inline std::vector<uint8_t> generateSharedSecret(") {
			return dir
		}
	}
	return ""
}

// Why a program that uses nothing of the sdk cannot be linked against its library by this toolchain, or "" when it can.
// That is the one reason to skip. A real program that fails to build where this one links is a failure (a header that is
// ahead of the library asks for a symbol it lacks, or a type no longer fits), and a skip would hide it behind a package
// that passes.
func approvalInteropLinkProbe(compiler, sdkDir, dir string) string {
	source := filepath.Join(dir, "probe.cpp")
	if err := os.WriteFile(source, []byte("int main() { return 0; }\n"), 0600); err != nil {
		return err.Error()
	}
	output, err := exec.Command(compiler, "-std=c++20", source, filepath.Join(sdkDir, "URnetworkSdk.dll"),
		"-o", filepath.Join(dir, "probe.exe")).CombinedOutput()
	if err == nil {
		return ""
	}
	if line := strings.TrimSpace(strings.SplitN(strings.TrimSpace(string(output)), "\n", 2)[0]); line != "" {
		return line
	}
	return err.Error()
}

// Build the interop program against the headers in `includeDir` (copies of the app's, so a negative control can
// change one) and the sdk in `sdkDir`. The build skips the test only when the toolchain cannot link the sdk library
// at all (approvalInteropLinkProbe); any other failure to build is a failure.
func approvalInteropProgram(t *testing.T, sdkDir, jsonDir string, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("approval interop tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, name := range []string{"ApprovalNetwork.h", "AuthLink.h", "UrIoApproval.h", "ApprovalUnseal.h"} {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", name))
		if err != nil {
			t.Fatal(err)
		}
		content := lf(string(source))
		if mutate != nil && name == "ApprovalUnseal.h" {
			changed := mutate(content)
			if changed == content {
				t.Fatal("negative control did not change the production ApprovalUnseal.h")
			}
			content = changed
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "approval-interop-tests.exe")
	args := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir, "-isystem", sdkDir}
	if jsonDir != "" {
		args = append(args, "-isystem", jsonDir)
	}
	args = append(args, filepath.Join(root, "app", "tools", "approval-interop-tests.cpp"),
		filepath.Join(sdkDir, "URnetworkSdk.dll"), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		if why := approvalInteropLinkProbe(compiler, sdkDir, t.TempDir()); why != "" {
			t.Skipf("the toolchain cannot link the sdk library (%s); run the program through the project's toolchain: %s", why, program)
		}
		t.Fatalf("build approval interop tests: %v\n%s", err, output)
	}
	return program
}

// Run the program with the sdk's directory first on the path, where the library is found.
func runApprovalInterop(t *testing.T, program, sdkDir string) ([]byte, error) {
	t.Helper()
	command := exec.Command(program)
	command.Env = append(os.Environ(), "PATH="+sdkDir+string(os.PathListSeparator)+os.Getenv("PATH"))
	return command.CombinedOutput()
}

func approvalInteropEnvironment(t *testing.T) (sdkDir, jsonDir string) {
	t.Helper()
	if runtime.GOOS != "windows" {
		t.Skip("the sdk library in this repository is a Windows DLL")
	}
	root := repositoryRoot(t)
	sdkDir = approvalInteropSdkDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with URnetworkSdk.dll beside it (set URNETWORK_SDK_INCLUDE)")
	}
	dir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	return sdkDir, dir
}

func TestApprovalPageVectorOpensWithTheSdk(t *testing.T) {
	sdkDir, jsonDir := approvalInteropEnvironment(t)
	t.Logf("against %s", sdkDir)
	program := approvalInteropProgram(t, sdkDir, jsonDir, nil)
	output, err := runApprovalInterop(t, program, sdkDir)
	if err != nil {
		t.Fatalf("approval interop: %v\n%s", err, output)
	}
	t.Logf("%s", output)
}

// Each defect puts a looser reader of the box back into a copy of ApprovalUnseal.h; the program must fail, naming it.
func TestApprovalUnsealSpecDetectsEachDefectPutBack(t *testing.T) {
	sdkDir, jsonDir := approvalInteropEnvironment(t)
	cases := []struct {
		name string
		old  string
		new  string
		want string
	}{
		{
			name: "the inner state is not compared with the link's",
			old:  "if (!SameSecret(state->get<std::string>(), link.state)) return std::nullopt;",
			new:  "(void)state;",
			want: "unseal: an answer to some other attempt's state",
		},
		{
			name: "what the sdk throws is not caught",
			old:  "    return answer;\n  } catch (const std::exception&) {\n    return std::nullopt;\n  }",
			new:  "    return answer;\n  } catch (const std::logic_error&) {\n    return std::nullopt;\n  }",
			want: "lets an exception out",
		},
		{
			name: "a public key of small order opens a box sealed under the constant it gives",
			old:  "if (shared.empty() || detail::SameBytes(shared, constant)) return std::nullopt;",
			new:  "if (shared.empty()) return std::nullopt;",
			want: "small order: the app's Unseal refuses the point",
		},
		{
			name: "only the zero point is taken for one of small order",
			old:  "constant = urnet::generateSharedSecret(*secret, std::vector<uint8_t>(32, 0));",
			new:  "constant = urnet::generateSharedSecret(*secret, std::vector<uint8_t>(32, 0));\n    constant[0] ^= 1;",
			want: "small order: the app's Unseal refuses the point",
		},
		{
			name: "every shared secret is taken for the constant of a small-order key",
			old:  "  return SameSecret(std::string_view(reinterpret_cast<const char*>(a.data()), a.size()),\n                    std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));",
			new:  "  (void)a;\n  (void)b;\n  return true;",
			want: "unseal: the page's vector gives the code, and only the code",
		},
		{
			name: "no shared secret is taken for the constant of a small-order key",
			old:  "  return SameSecret(std::string_view(reinterpret_cast<const char*>(a.data()), a.size()),\n                    std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));",
			new:  "  (void)a;\n  (void)b;\n  return false;",
			want: "small order: the app's Unseal refuses the point",
		},
		{
			name: "the request carries the secret half of the pair",
			old:  "return KeyPair{made.PublicKeyBase58, made.PrivateKeyBase58};",
			new:  "return KeyPair{made.PrivateKeyBase58, made.PublicKeyBase58};",
			want: "key pair: the request carries the sdk's public half",
		},
		{
			name: "the request carries the secret half of the pair, and the round trip says so too",
			old:  "return KeyPair{made.PublicKeyBase58, made.PrivateKeyBase58};",
			new:  "return KeyPair{made.PrivateKeyBase58, made.PublicKeyBase58};",
			want: "round trip: every box the page side sealed opened on the app's side",
		},
		{
			name: "a pair whose public half is not 32 bytes is taken",
			old:  "const bool whole = secretBytes && secretBytes->size() == 32 && publicBytes && publicBytes->size() == 32;",
			new:  "const bool whole = secretBytes && secretBytes->size() == 32 && publicBytes;",
			want: "a public key of 31 bytes is no pair",
		},
		{
			name: "a pair whose secret half is not 32 bytes is taken",
			old:  "const bool whole = secretBytes && secretBytes->size() == 32 && publicBytes && publicBytes->size() == 32;",
			new:  "const bool whole = secretBytes && publicBytes && publicBytes->size() == 32;",
			want: "a private key of 31 bytes is no pair",
		},
		{
			name: "an empty code is a code",
			old:  "if (answer.empty() || answer.size() > 2048) return std::nullopt;",
			new:  "if (answer.size() > 2048) return std::nullopt;",
			want: "inside: an empty code",
		},
		{
			name: "a code of any length is a code",
			old:  "if (answer.empty() || answer.size() > 2048) return std::nullopt;",
			new:  "if (answer.empty()) return std::nullopt;",
			want: "inside: a code over the cap",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			program := approvalInteropProgram(t, sdkDir, jsonDir, func(source string) string {
				if strings.Count(source, c.old) != 1 {
					t.Fatalf("negative control expects exactly one %q in ApprovalUnseal.h", c.old)
				}
				return strings.Replace(source, c.old, c.new, 1)
			})
			output, err := runApprovalInterop(t, program, sdkDir)
			if err == nil || !strings.Contains(string(output), c.want) {
				t.Fatalf("negative control was not detected (want %q): %v\n%s", c.want, err, output)
			}
		})
	}
}
