// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The slow-internet tunnel (App/TunnelIndicator.h) as a C++ spec, with
// negative controls that rewrite a copy of the header; tools/make-icons'
// constants held to the header's; and the tray's tunnel icons, from their
// files through the resource script to TrayIcon's pick.

// Compile `spec` (app/tools) against a copy of the App header `header`,
// rewritten by `mutate` when it is set. The copy's directory is searched
// first, then the app's own headers.
func slowSpecProgram(t *testing.T, spec, header string, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("the slow-internet specs require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source := readAppSource(t, header)
	content := source
	if mutate != nil {
		content = mutate(source)
		if content == source {
			t.Fatalf("negative control did not change the production %s", header)
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, header), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, strings.TrimSuffix(spec, ".cpp"))
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", spec), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build %s: %v\n%s", spec, err, output)
	}
	return program
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming each of wants.
func requireSlowSpecFailure(t *testing.T, spec, header string, mutate func(string) string, wants ...string) {
	t.Helper()
	program := slowSpecProgram(t, spec, header, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil {
		t.Fatalf("negative control was not detected:\n%s", output)
	}
	for _, want := range wants {
		if !strings.Contains(string(output), want) {
			t.Errorf("negative control did not fail %q:\n%s", want, output)
		}
	}
}

// The source with the one occurrence of old rewritten; the control fails when
// the header no longer has exactly one.
func replaceSlowOnce(t *testing.T, header, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("%s no longer has exactly one %q; update this control", header, old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// Execute the spec: the depth table and its interpolation, the tray's four
// icons, the geometry at every size drawn, and the fades between depths.
func TestTunnelIndicator(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="TunnelIndicator.h" />`) {
		t.Error("App.vcxproj does not list TunnelIndicator.h")
	}
	program := slowSpecProgram(t, "tunnel-indicator-tests.cpp", "TunnelIndicator.h", nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("tunnel indicator: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireTunnelFailure(t *testing.T, old, replacement string, wants ...string) {
	t.Helper()
	requireSlowSpecFailure(t, "tunnel-indicator-tests.cpp", "TunnelIndicator.h", func(source string) string {
		return replaceSlowOnce(t, "TunnelIndicator.h", source, old, replacement)
	}, wants...)
}

// A heavier outer stroke no longer fits the box at 0.094 S.
func TestTunnelIndicatorRejectsAnotherOuterStroke(t *testing.T) {
	requireTunnelFailure(t, "inline constexpr double kOuterStroke = 0.094;",
		"inline constexpr double kOuterStroke = 0.1;",
		"FAIL the outer square fits the box", "at 24.0000: the outer stroke is 2.4000",
		"the outer stroke is 2.256 at 24")
}

// The vanishing point at the centre gives even gaps: no tunnel.
func TestTunnelIndicatorRejectsACentredVanishingPoint(t *testing.T) {
	requireTunnelFailure(t, "inline constexpr double kVanishingPoint = 0.55;",
		"inline constexpr double kVanishingPoint = 0.5;",
		"FAIL the gaps", "the left gap is the wider")
}

// A tunnel built from the inside outward.
func TestTunnelIndicatorRejectsBuildingFromTheInside(t *testing.T) {
	requireTunnelFailure(t, "      const int order = index - fromDepth;\n",
		"      const int order = toDepth - 1 - index;\n",
		"FAIL deeper fades in from the outside")
}

// The fade-out's stagger on the fade-in.
func TestTunnelIndicatorRejectsTheWrongStagger(t *testing.T) {
	requireTunnelFailure(t, "inline constexpr int64_t kFadeInStaggerMillis = 110;",
		"inline constexpr int64_t kFadeInStaggerMillis = 90;",
		"FAIL deeper fades in from the outside", "0 to 6 builds in 870 ms, not 970")
}

// A small quality at another step drew nothing.
func TestTunnelIndicatorRejectsDroppingTheOneSquareFloor(t *testing.T) {
	requireTunnelFailure(t, "  return std::clamp(static_cast<int>(std::lround(depth)), 1, kMaxDepth);\n",
		"  return std::clamp(static_cast<int>(std::lround(depth)), 0, kMaxDepth);\n",
		"FAIL another step interpolates", "quality 0.1000 interpolates to 0 squares, not 1")
}

// A depth between two rows read as the shallower icon.
func TestTunnelIndicatorRejectsTheShallowerIcon(t *testing.T) {
	requireTunnelFailure(t, "    if (depth <= kDepthByLevel[level]) return level;\n",
		"    if (depth < kDepthByLevel[level]) return level - 1;\n",
		"FAIL the tray icons", "3 squares take icon 2, not 3")
}

// The pill faded with the first square to fade instead of the last.
func TestTunnelIndicatorRejectsThePillLeavingEarly(t *testing.T) {
	requireTunnelFailure(t, "  return fades.empty() ? 0 : fades.back().beginMillis;\n",
		"  return fades.empty() ? 0 : fades.front().beginMillis;\n",
		"FAIL the pill waits for the last square")
}

// The value of `name` in the header's `inline constexpr <type> name = value;`.
func tunnelHeaderConstant(t *testing.T, header, name string) string {
	t.Helper()
	match := regexp.MustCompile(`inline constexpr \w+ ` + name + ` = ([^;]+);`).FindStringSubmatch(header)
	if match == nil {
		t.Fatalf("TunnelIndicator.h no longer declares %s; update this contract", name)
	}
	return strings.TrimSpace(match[1])
}

// The value of `name` in tunnel.go's const block.
func tunnelGeneratorConstant(t *testing.T, generator, name string) string {
	t.Helper()
	match := regexp.MustCompile(`(?m)^\s*` + name + `\s*=\s*([^\s/]+)`).FindStringSubmatch(generator)
	if match == nil {
		t.Fatalf("tools/make-icons/tunnel.go no longer declares %s; update this contract", name)
	}
	return match[1]
}

// The tray's icons and the pill draw one tunnel: the generator's constants and
// depth table are the header's.
func TestTunnelIndicatorMatchesTheIconGenerator(t *testing.T) {
	header := stripComments(readAppSource(t, "TunnelIndicator.h"))
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "tools", "make-icons", "tunnel.go"))
	if err != nil {
		t.Fatal(err)
	}
	generator := string(data)
	for headerName, generatorName := range map[string]string{
		"kOuterStroke":    "tunnelOuterStroke",
		"kStrokeRatio":    "tunnelStrokeRatio",
		"kScaleRatio":     "tunnelScaleRatio",
		"kVanishingPoint": "tunnelVanishingPoint",
		"kOpacityStep":    "tunnelOpacityStep",
	} {
		if a, b := tunnelHeaderConstant(t, header, headerName), tunnelGeneratorConstant(t, generator, generatorName); a != b {
			t.Errorf("TunnelIndicator.h's %s is %s, tools/make-icons' %s is %s", headerName, a, generatorName, b)
		}
	}
	table := regexp.MustCompile(`kDepthByLevel = \{([^}]*)\}`).FindStringSubmatch(header)
	generatorTable := regexp.MustCompile(`tunnelDepthByLevel = \[\]int\{([^}]*)\}`).FindStringSubmatch(generator)
	if table == nil || generatorTable == nil {
		t.Fatal("the depth table is missing from TunnelIndicator.h or tools/make-icons; update this contract")
	}
	if strings.ReplaceAll(table[1], " ", "") != strings.ReplaceAll(generatorTable[1], " ", "") {
		t.Errorf("the depth tables differ: TunnelIndicator.h {%s}, tools/make-icons {%s}", table[1], generatorTable[1])
	}
	if got := strings.ReplaceAll(table[1], " ", ""); got != "0,1,2,4,6" {
		t.Errorf("the depth table is {%s}, want §2.1's {0, 1, 2, 4, 6}", table[1])
	}
}

// The ICO at path decoded: each frame's size and its pixels.
func tunnelIconFrames(t *testing.T, path string) map[int]*image.NRGBA {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(data) < 6 || binary.LittleEndian.Uint16(data[2:4]) != 1 {
		t.Fatalf("%s is not an icon", path)
	}
	frames := map[int]*image.NRGBA{}
	count := int(binary.LittleEndian.Uint16(data[4:6]))
	for index := 0; index < count; index++ {
		entry := data[6+16*index : 6+16*(index+1)]
		length := int(binary.LittleEndian.Uint32(entry[8:12]))
		offset := int(binary.LittleEndian.Uint32(entry[12:16]))
		frame, err := png.Decode(bytes.NewReader(data[offset : offset+length]))
		if err != nil {
			t.Fatalf("%s frame %d: %v", path, index, err)
		}
		nrgba, ok := frame.(*image.NRGBA)
		if !ok {
			t.Fatalf("%s frame %d is %T, want 8-bit RGBA", path, index, frame)
		}
		frames[frame.Bounds().Dx()] = nrgba
	}
	return frames
}

// Icons 1 to 4 for each taskbar: files with every tray size, a resource id
// each in resource.h's order, a line each in App.rc, and TrayIcon taking them
// in the state art's place while the tray says slow.
func TestTunnelIndicatorTrayIcons(t *testing.T) {
	root := repositoryRoot(t)
	resource := stripComments(readAppSource(t, "resource.h"))
	script := stripComments(readAppSource(t, "App.rc"))
	defines := map[string]int{}
	for _, match := range regexp.MustCompile(`#define (IDI_TRAY_\w+) (\d+)`).FindAllStringSubmatch(resource, -1) {
		var value int
		fmt.Sscanf(match[2], "%d", &value)
		defines[match[1]] = value
	}
	ids := map[int]string{}
	for name, value := range defines {
		if other, ok := ids[value]; ok && !strings.HasSuffix(name, "_BASE") && !strings.HasSuffix(other, "_BASE") {
			t.Errorf("resource.h gives %s and %s the same id %d", name, other, value)
		}
		if !strings.HasSuffix(name, "_BASE") {
			ids[value] = name
		}
	}
	for _, taskbar := range []string{"LIGHT", "DARK"} {
		base, ok := defines["IDI_TRAY_"+taskbar+"_SLOW_BASE"]
		if !ok {
			t.Fatalf("resource.h does not define IDI_TRAY_%s_SLOW_BASE", taskbar)
		}
		for level := 1; level <= 4; level++ {
			name := fmt.Sprintf("IDI_TRAY_%s_SLOW_%d", taskbar, level)
			if defines[name] != base+level-1 {
				t.Errorf("resource.h: %s is %d, want base + %d = %d", name, defines[name], level-1, base+level-1)
			}
			file := fmt.Sprintf("tray_%s_slow_%d.ico", strings.ToLower(taskbar), level)
			line := regexp.MustCompile(`(?m)^` + name + `\s+ICON\s+"Assets\\\\` + regexp.QuoteMeta(file) + `"`)
			if !line.MatchString(script) {
				t.Errorf("App.rc does not map %s to Assets\\%s", name, file)
			}
			frames := tunnelIconFrames(t, filepath.Join(root, "app", "src", "App", "Assets", file))
			for _, size := range []int{16, 20, 24, 32, 48} {
				if frames[size] == nil {
					t.Errorf("%s has no %dx%d frame", file, size, size)
				}
			}
		}
	}
	tray := stripComments(readAppSource(t, "TrayIcon.cpp"))
	icon := definitionBody(t, "TrayIcon.cpp", tray, "HICON TrayIcon::CurrentIcon() {")
	provideRequire(t, "TrayIcon::CurrentIcon", icon,
		"(darkTaskbar_ ? IDI_TRAY_DARK_SLOW_BASE : IDI_TRAY_LIGHT_SLOW_BASE) + slowIcon_ - 1",
		"(darkTaskbar_ ? IDI_TRAY_DARK_BASE : IDI_TRAY_LIGHT_BASE) + static_cast<int>(state_)")
	set := definitionBody(t, "TrayIcon.cpp", tray, "void TrayIcon::SetState(TrayState state, int slowIcon) {")
	provideRequire(t, "TrayIcon::SetState", set, "slowIcon_ = 0 < slowIcon && slowIcon <= 4 ? slowIcon : 0;", "UpdateIcon();")
}

// The committed icons are what the generator draws now: regenerated into a
// scratch folder, every frame's pixels match (one step of alpha apart at most,
// for floating point that differs between machines).
func TestTunnelIndicatorTrayIconsAreCurrent(t *testing.T) {
	root := repositoryRoot(t)
	output := t.TempDir()
	generate := exec.Command("go", "run", "./app/tools/make-icons", "-only-tunnel", "-out", output)
	generate.Dir = root
	if log, err := generate.CombinedOutput(); err != nil {
		t.Fatalf("make-icons -only-tunnel: %v\n%s", err, log)
	}
	for _, taskbar := range []string{"light", "dark"} {
		for level := 1; level <= 4; level++ {
			file := fmt.Sprintf("tray_%s_slow_%d.ico", taskbar, level)
			committed := tunnelIconFrames(t, filepath.Join(root, "app", "src", "App", "Assets", file))
			fresh := tunnelIconFrames(t, filepath.Join(output, file))
			if len(committed) != len(fresh) {
				t.Errorf("%s has %d frames, the generator draws %d: run go run ./tools/make-icons -only-tunnel",
					file, len(committed), len(fresh))
				continue
			}
			for size, want := range fresh {
				got := committed[size]
				if got == nil {
					t.Errorf("%s has no %d frame: run go run ./tools/make-icons -only-tunnel", file, size)
					continue
				}
				for index := range want.Pix {
					difference := int(got.Pix[index]) - int(want.Pix[index])
					if difference < -1 || 1 < difference {
						t.Errorf("%s at %d differs from the generator's drawing: run go run ./tools/make-icons -only-tunnel",
							file, size)
						break
					}
				}
			}
		}
	}
}
