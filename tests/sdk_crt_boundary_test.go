// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// windows#17: a URnetworkSdk.dll linked to msvcrt.dll beside the UCRT app and
// service kept two C runtime heaps in one process, and urnetworkd died in
// RtlFreeHeap on a block freed across them. build-sdk.ps1 reads each DLL's
// imports and refuses one that loads msvcrt.dll, before it is zipped.
func TestBuildSdkRefusesAnMsvcrtLinkedDll(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "build-sdk.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	script := string(data)

	if !strings.Contains(script, "Require 'llvm-readobj'") {
		t.Error("build-sdk.ps1 does not require llvm-readobj, which the C runtime check runs")
	}
	readImports := strings.Index(script, "& llvm-readobj --coff-imports (Join-Path $outDir \"URnetworkSdk.dll\")")
	goBuild := strings.Index(script, "& go build ")
	zip := strings.Index(script, "Compress-Archive")
	if readImports < 0 {
		t.Fatal("build-sdk.ps1 never reads the built DLL's imports")
	}
	if !(goBuild < readImports && readImports < zip) {
		t.Fatalf("the import check must run on each built DLL before the zip: build=%d check=%d zip=%d", goBuild, readImports, zip)
	}

	match := regexp.MustCompile(`Select-String -Pattern '([^']+)' -Quiet\) \{\s*throw "URnetworkSdk\.dll for windows/\$arch imports msvcrt\.dll`).FindStringSubmatch(script)
	if match == nil {
		t.Fatal("build-sdk.ps1 does not refuse a DLL whose imports name msvcrt.dll")
	}
	// Select-String is case-insensitive by default.
	pattern := regexp.MustCompile("(?i)" + match[1])

	// llvm-readobj --coff-imports, abridged: a DLL from an msvcrt toolchain, and
	// the ucrt llvm-mingw build the release ships.
	msvcrt := []string{
		"Import {",
		"  Name: KERNEL32.dll",
		"  Symbol: GetProcAddress (0)",
		"}",
		"Import {",
		"  Name: msvcrt.dll",
		"  Symbol: free (0)",
		"  Symbol: malloc (0)",
		"}",
	}
	ucrt := []string{
		"Import {",
		"  Name: KERNEL32.dll",
		"}",
		"Import {",
		"  Name: api-ms-win-crt-heap-l1-1-0.dll",
		"  Symbol: free (0)",
		"  Symbol: malloc (0)",
		"}",
		"Import {",
		"  Name: api-ms-win-crt-runtime-l1-1-0.dll",
		"}",
	}
	matches := func(lines []string) bool {
		for _, line := range lines {
			if pattern.MatchString(line) {
				return true
			}
		}
		return false
	}
	if !matches(msvcrt) {
		t.Error("the check passes a DLL that imports msvcrt.dll")
	}
	if !matches([]string{"  Name: MSVCRT.DLL"}) {
		t.Error("the check passes msvcrt.dll spelled in upper case")
	}
	if matches(ucrt) {
		t.Error("the check refuses a UCRT DLL")
	}
}
