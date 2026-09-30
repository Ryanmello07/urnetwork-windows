// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// urnetworkd.exe and the app link the DLL CRT (/MD), and a self-contained
// unpackaged Windows App SDK app does not bring the VC++ runtime with it. On
// a machine without the VC++ Redistributable the service cannot load, so the
// MSI's ServiceControl start fails the whole install with error 1920
// ("Service 'URnetwork Service' (urnetworkd) failed to start"). The Release
// build must stage the VC++ runtime DLLs next to the binaries (BinDir, which
// the MSI harvests and the portable zip ships), and the MSI payload check
// must require them.
func TestReleaseBuildStagesTheVCRuntimeAppLocal(t *testing.T) {
	root := repositoryRoot(t)
	project := parseXML(t, filepath.Join(root, "app", "src", "Service", "Service.vcxproj"))

	dynamicCRT := false
	for _, library := range project.descendants(msbuildNamespace, "RuntimeLibrary") {
		if strings.TrimSpace(library.Text) == "MultiThreadedDLL" {
			dynamicCRT = true
		}
	}
	if !dynamicCRT {
		t.Skip("urnetworkd no longer links the DLL CRT; the app-local runtime is not needed for it")
	}

	var stage *xmlNode
	for _, target := range project.descendants(msbuildNamespace, "Target") {
		if name, _ := target.attribute("Name"); name == "UrnStageVCRuntime" {
			stage = target
		}
	}
	if stage == nil {
		t.Fatal("Service.vcxproj must define the UrnStageVCRuntime target that copies the VC++ runtime next to urnetworkd.exe")
	}
	if after, _ := stage.attribute("AfterTargets"); after != "Build" {
		t.Errorf("UrnStageVCRuntime must run after Build, got AfterTargets=%q", after)
	}
	if condition, _ := stage.attribute("Condition"); !strings.Contains(condition, "Release") {
		t.Errorf("UrnStageVCRuntime must run for Release builds, got Condition=%q", condition)
	}

	includes := stage.descendants(msbuildNamespace, "UrnVCRuntime")
	if len(includes) != 1 {
		t.Fatalf("UrnStageVCRuntime must collect exactly one UrnVCRuntime item group, got %d", len(includes))
	}
	include, _ := includes[0].attribute("Include")
	if !strings.HasPrefix(include, "$(VCToolsRedistInstallDir)") ||
		!strings.HasSuffix(include, `\Microsoft.VC*.CRT\*.dll`) {
		t.Errorf("UrnVCRuntime must take the toolset's redistributable CRT DLLs, got %q", include)
	}

	errors := stage.descendants(msbuildNamespace, "Error")
	if len(errors) != 1 {
		t.Fatal("UrnStageVCRuntime must fail the build when the VC++ runtime DLLs are not found")
	}
	if condition, _ := errors[0].attribute("Condition"); condition != "'@(UrnVCRuntime)'==''" {
		t.Errorf("the missing-runtime error must fire on an empty UrnVCRuntime, got %q", condition)
	}

	copies := stage.descendants(msbuildNamespace, "Copy")
	if len(copies) != 1 {
		t.Fatal("UrnStageVCRuntime must copy the VC++ runtime DLLs")
	}
	if source, _ := copies[0].attribute("SourceFiles"); source != "@(UrnVCRuntime)" {
		t.Errorf("Copy SourceFiles = %q, want @(UrnVCRuntime)", source)
	}
	if destination, _ := copies[0].attribute("DestinationFolder"); destination != "$(OutDir)" {
		t.Errorf("Copy DestinationFolder = %q, want $(OutDir) (the MSI's BinDir)", destination)
	}

	verify, err := os.ReadFile(filepath.Join(root, "app", "tools", "verify-msi-payload.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"} {
		if !strings.Contains(string(verify), `"`+name+`"`) {
			t.Errorf("verify-msi-payload.ps1 must require %s in the MSI payload", name)
		}
	}
}
