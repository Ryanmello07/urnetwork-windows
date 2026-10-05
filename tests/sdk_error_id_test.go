// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The C ABI's internal error id (URNET_ERROR_ID_INTERNAL: what an sdk
// error-id function answers when the call could not run) has one copy in the
// app, App/SdkErrorId.h. It was mirrored twice, as vless::kErrorInternal and
// kSdkErrorIdInternal, and two mirrors of one sdk value can drift apart
// unseen: both map an unknown id to something_went_wrong anyway.

// The id's one definition, by its value's spelling in the sources.
var sdkInternalErrorIdDefinition = regexp.MustCompile(
	`inline constexpr const char\* kSdkErrorIdInternal = ("[^"]*");`)

// The app's sources and the harnesses, comments stripped, spell the id as a
// string literal exactly once: SdkErrorId.h's definition. Anything else that
// needs it names kSdkErrorIdInternal.
func TestSdkInternalErrorIdHasOneCopy(t *testing.T) {
	root := repositoryRoot(t)
	definition := sdkInternalErrorIdDefinition.FindStringSubmatch(
		stripComments(readAppSource(t, "SdkErrorId.h")))
	if definition == nil {
		t.Fatal("App/SdkErrorId.h no longer defines kSdkErrorIdInternal; update this contract")
	}
	literal := definition[1]
	home := filepath.Join("app", "src", "App", "SdkErrorId.h")
	copies := 0
	for _, dir := range []string{
		filepath.Join("app", "src", "App"),
		filepath.Join("app", "src", "Common"),
		filepath.Join("app", "src", "Service"),
		filepath.Join("app", "tools"),
	} {
		entries, err := os.ReadDir(filepath.Join(root, dir))
		if err != nil {
			t.Fatal(err)
		}
		checked := 0
		for _, entry := range entries {
			switch filepath.Ext(entry.Name()) {
			case ".cpp", ".h", ".hpp", ".idl":
			default:
				continue
			}
			if entry.IsDir() {
				continue
			}
			name := filepath.Join(dir, entry.Name())
			source, err := os.ReadFile(filepath.Join(root, name))
			if err != nil {
				t.Fatal(err)
			}
			checked++
			count := strings.Count(stripComments(string(source)), literal)
			copies += count
			if count > 0 && name != home {
				t.Errorf("%s spells the sdk's internal error id %s %d time(s); name kSdkErrorIdInternal "+
					"(SdkErrorId.h), the app's one copy", name, literal, count)
			}
		}
		if checked == 0 {
			t.Errorf("%s: no C++ sources found; update this check", dir)
		}
	}
	if copies == 0 {
		t.Error("the scan found not even SdkErrorId.h's own definition; update this check")
	}
	// the former mirrors, by their names
	requireNone(t, "VlessPresentation.h", stripComments(readAppSource(t, "VlessPresentation.h")),
		"kErrorInternal")
	requireNone(t, "ExtenderPresentation.h", stripComments(readAppSource(t, "ExtenderPresentation.h")),
		"kSdkErrorIdInternal =")
	// SDK-free and dependency-free: the harnesses that include it build on any host
	if strings.Contains(stripComments(readAppSource(t, "SdkErrorId.h")), "#include") {
		t.Error("SdkErrorId.h must include nothing, so every harness can name it")
	}
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="SdkErrorId.h" />`) {
		t.Error("App.vcxproj does not list SdkErrorId.h")
	}
}

// The one copy is the C header's value. SdkHost.cpp includes that header for
// the calls that answer the id and holds the two equal at compile time, so
// every Windows build checks it; and the generated header on this host, when
// there is one (URNETWORK_SDK_INCLUDE, or the git-ignored copy fetch-deps
// unpacks), is read here too.
func TestSdkInternalErrorIdIsTheSdkHeaders(t *testing.T) {
	host := sdkHostSource(t)
	provideRequire(t, "SdkHost.cpp", host,
		"#include <urnetwork_sdk.h>",
		`#include "SdkErrorId.h"`,
		"static_assert(std::string_view{URNET_ERROR_ID_INTERNAL} == kSdkErrorIdInternal,")
	// a header from before the define still builds the app
	provideRequireOrder(t, "SdkHost.cpp", host, "#if defined(URNET_ERROR_ID_INTERNAL)",
		"static_assert(std::string_view{URNET_ERROR_ID_INTERNAL} == kSdkErrorIdInternal,")

	root := repositoryRoot(t)
	dir := sdkHeaderDirWith(t, root, "ErrorIdInternal")
	if dir == "" {
		t.Skip("no urnetwork_sdk.hpp with the internal error id (set URNETWORK_SDK_INCLUDE)")
	}
	data, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.h"))
	if err != nil {
		t.Fatal(err)
	}
	define := regexp.MustCompile(`(?m)^#define URNET_ERROR_ID_INTERNAL ("[^"]*")`).FindStringSubmatch(string(data))
	if define == nil {
		t.Fatalf("%s: urnetwork_sdk.h does not define URNET_ERROR_ID_INTERNAL", dir)
	}
	definition := sdkInternalErrorIdDefinition.FindStringSubmatch(
		stripComments(readAppSource(t, "SdkErrorId.h")))
	if definition == nil {
		t.Fatal("App/SdkErrorId.h no longer defines kSdkErrorIdInternal; update this contract")
	}
	if definition[1] != define[1] {
		t.Errorf("SdkErrorId.h's kSdkErrorIdInternal is %s, but the sdk's URNET_ERROR_ID_INTERNAL is %s",
			definition[1], define[1])
	}
	t.Logf("against %s", filepath.Join(dir, "urnetwork_sdk.h"))
}
