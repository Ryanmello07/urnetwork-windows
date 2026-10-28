// SPDX-License-Identifier: MPL-2.0

package main

import (
	"bytes"
	"encoding/binary"
	"image/color"
	"image/png"
	"math"
	"os"
	"path/filepath"
	"testing"
)

// The geometry is §2.1's: a 0.094 S outer stroke ("2.25 pt at 24 pt") inset by
// half of it, each nested square 0.70 of the outer one about (0.55 S, 0.55 S),
// each nested stroke 0.60 of the one outside it, opacity 1 - 0.08 i.
func TestTunnelSquaresFollowTheGeometry(t *testing.T) {
	const size = 24.0
	outer := tunnelSquareAt(size, 0)
	if math.Abs(outer.stroke-2.256) > 1e-9 {
		t.Errorf("outer stroke %v, want 2.256", outer.stroke)
	}
	if math.Abs(outer.x-outer.stroke/2) > 1e-9 || math.Abs(outer.x+outer.side+outer.stroke/2-size) > 1e-9 {
		t.Errorf("the outer stroke does not fit the box: %+v", outer)
	}
	vanishing := 0.55 * size
	for index := 1; index < 6; index++ {
		square := tunnelSquareAt(size, index)
		scale := math.Pow(0.70, float64(index))
		if math.Abs((square.x-vanishing)-(outer.x-vanishing)*scale) > 1e-9 ||
			math.Abs((square.x+square.side-vanishing)-(outer.x+outer.side-vanishing)*scale) > 1e-9 {
			t.Errorf("square %d is not the outer square scaled about the vanishing point: %+v", index, square)
		}
		if math.Abs(square.stroke-tunnelSquareAt(size, index-1).stroke*0.60) > 1e-9 {
			t.Errorf("square %d stroke %v is not 0.60 of the one outside it", index, square.stroke)
		}
		if math.Abs(square.opacity-(1-0.08*float64(index))) > 1e-9 {
			t.Errorf("square %d opacity %v", index, square.opacity)
		}
	}
}

// The drawing puts ink on the strokes, at each square's opacity, and none in
// the gaps or the middle.
func TestRenderTunnelInksTheStrokes(t *testing.T) {
	ink := color.NRGBA{R: 0xF5, G: 0xC2, B: 0x42, A: 0xFF}
	const size = 48
	image := renderTunnel(size, 6, ink)
	outer := tunnelSquareAt(size, 0)
	// the middle of the outer square's left stroke, halfway down
	stroke := image.NRGBAAt(int(outer.x), size/2)
	if stroke.A < 0xF0 || stroke.R != ink.R || stroke.G != ink.G || stroke.B != ink.B {
		t.Errorf("the outer stroke is %+v, want opaque %+v", stroke, ink)
	}
	// between the outer square and the next, on the wide left side
	next := tunnelSquareAt(size, 1)
	gapX := int((outer.x + outer.stroke/2 + next.x - next.stroke/2) / 2)
	if gap := image.NRGBAAt(gapX, size/2); gap.A != 0 {
		t.Errorf("the gap at x=%d is %+v, want clear", gapX, gap)
	}
	// the innermost square's centre is inside every square and on none
	inner := tunnelSquareAt(size, 5)
	centre := int(inner.x + inner.side/2)
	if middle := image.NRGBAAt(centre, centre); middle.A != 0 {
		t.Errorf("the middle is %+v, want clear", middle)
	}
	// the second square's stroke draws at most its 0.92 opacity
	secondX := int(next.x)
	if second := image.NRGBAAt(secondX, size/2); second.A == 0 || second.A > 0xEB {
		t.Errorf("the second square's stroke is %+v, want ink at most 0.92 opaque", second)
	}
}

// Each icon draws its row's squares: more squares, more ink, and the first
// square identical in every icon.
func TestTunnelIconsDeepenByLevel(t *testing.T) {
	ink := color.NRGBA{R: 0xF5, G: 0xC2, B: 0x42, A: 0xFF}
	previous := -1.0
	for level := 1; level < len(tunnelDepthByLevel); level++ {
		total := 0.0
		image := renderTunnel(32, tunnelDepthByLevel[level], ink)
		for index := 3; index < len(image.Pix); index += 4 {
			total += float64(image.Pix[index])
		}
		if total <= previous {
			t.Errorf("icon %d draws %v of ink, no more than icon %d's %v", level, total, level-1, previous)
		}
		previous = total
	}
	if want := []int{0, 1, 2, 4, 6}; len(tunnelDepthByLevel) != len(want) {
		t.Fatalf("the depth table is %v, want %v", tunnelDepthByLevel, want)
	} else {
		for index := range want {
			if tunnelDepthByLevel[index] != want[index] {
				t.Errorf("icon %d draws %d squares, want %d", index, tunnelDepthByLevel[index], want[index])
			}
		}
	}
}

func relativeLuminance(c color.NRGBA) float64 {
	linear := func(value uint8) float64 {
		channel := float64(value) / 255
		if channel <= 0.04045 {
			return channel / 12.92
		}
		return math.Pow((channel+0.055)/1.055, 2.4)
	}
	return 0.2126*linear(c.R) + 0.7152*linear(c.G) + 0.0722*linear(c.B)
}

func hue(c color.NRGBA) float64 {
	r, g, b := float64(c.R)/255, float64(c.G)/255, float64(c.B)/255
	high := math.Max(r, math.Max(g, b))
	low := math.Min(r, math.Min(g, b))
	if high == low {
		return 0
	}
	switch high {
	case r:
		return math.Mod(60*(g-b)/(high-low)+360, 360)
	case g:
		return 60*(b-r)/(high-low) + 120
	default:
		return 60*(r-g)/(high-low) + 240
	}
}

// The dark taskbar's ink is the contract's amber; the light taskbar's is the
// same hue, darkened to hold 3:1 against a light bar (#F3F3F3).
func TestTunnelInksReadOnTheirTaskbars(t *testing.T) {
	amber := color.NRGBA{R: 0xF5, G: 0xC2, B: 0x42, A: 0xFF}
	lightBar := color.NRGBA{R: 0xF3, G: 0xF3, B: 0xF3, A: 0xFF}
	found := map[string]color.NRGBA{}
	for _, variant := range tunnelInks {
		found[variant.prefix] = variant.ink
	}
	if found["tray_dark_slow_"] != amber {
		t.Errorf("the dark taskbar's ink is %+v, want the amber %+v", found["tray_dark_slow_"], amber)
	}
	light, ok := found["tray_light_slow_"]
	if !ok {
		t.Fatal("no light taskbar ink")
	}
	if difference := math.Abs(hue(light) - hue(amber)); difference > 1.5 {
		t.Errorf("the light taskbar's ink is %.1f° from the amber's hue", difference)
	}
	contrast := (relativeLuminance(lightBar) + 0.05) / (relativeLuminance(light) + 0.05)
	if contrast < 3 {
		t.Errorf("the light taskbar's ink holds %.2f:1 against the bar, want 3:1", contrast)
	}
}

// The generated files: icons 1 to 4 for each taskbar, each with every tray
// size, drawn at that size.
func TestGenerateTunnelIconsWritesEveryIcon(t *testing.T) {
	output := t.TempDir()
	if err := generateTunnelIcons(output); err != nil {
		t.Fatal(err)
	}
	for level := 1; level <= 4; level++ {
		for _, prefix := range []string{"tray_light_slow_", "tray_dark_slow_"} {
			name := tunnelIconName(prefix, level)
			data, err := os.ReadFile(filepath.Join(output, name))
			if err != nil {
				t.Fatal(err)
			}
			if count := int(binary.LittleEndian.Uint16(data[4:6])); count != len(traySizes) {
				t.Fatalf("%s has %d images, want %d", name, count, len(traySizes))
			}
			for index, size := range traySizes {
				entry := data[6+16*index : 6+16*(index+1)]
				length := int(binary.LittleEndian.Uint32(entry[8:12]))
				offset := int(binary.LittleEndian.Uint32(entry[12:16]))
				frame, err := png.Decode(bytes.NewReader(data[offset : offset+length]))
				if err != nil {
					t.Fatalf("%s frame %d: %v", name, index, err)
				}
				if frame.Bounds().Dx() != size || frame.Bounds().Dy() != size || int(entry[0]) != size {
					t.Errorf("%s frame %d is %v, want %dx%d", name, index, frame.Bounds(), size, size)
				}
			}
		}
	}
}
