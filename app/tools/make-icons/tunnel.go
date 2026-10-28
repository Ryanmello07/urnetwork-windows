// The tray's slow-internet icons (App/TunnelIndicator.h, apple/DELAY.md §2.1):
// the tunnel drawn from its geometry at every tray size, not scaled from one
// source image, so the thin inner strokes are as sharp as each size allows.
// Icons 1 to 4 draw 1, 2, 4 and 6 squares, the rows of the depth table, each
// for a light and a dark taskbar.
//
// SPDX-License-Identifier: MPL-2.0
package main

import (
	"fmt"
	"image"
	"image/color"
	"io"
	"path/filepath"
)

// §2.1, relative to the box side. tests/tunnel_indicator_test.go holds these
// to App/TunnelIndicator.h's.
const (
	tunnelOuterStroke    = 0.094
	tunnelStrokeRatio    = 0.60
	tunnelScaleRatio     = 0.70
	tunnelVanishingPoint = 0.55
	tunnelOpacityStep    = 0.08
)

// the squares each icon draws, by icon level 1 to 4
var tunnelDepthByLevel = []int{0, 1, 2, 4, 6}

// The ink per taskbar. A dark taskbar takes the contract's amber, #F5C242. On
// a light taskbar that amber is 1.6:1 against the bar, so the light icons take
// the same hue darkened until it holds 3:1 there (HSL 43°, 90%, 36%), as the
// brand tray icons take dark ink on a light bar.
var tunnelInks = []struct {
	prefix string
	ink    color.NRGBA
}{
	{prefix: "tray_light_slow_", ink: color.NRGBA{R: 0xAE, G: 0x80, B: 0x09, A: 0xFF}},
	{prefix: "tray_dark_slow_", ink: color.NRGBA{R: 0xF5, G: 0xC2, B: 0x42, A: 0xFF}},
}

// samples per pixel along each axis
const tunnelSamples = 16

type tunnelSquare struct {
	// the outline's centre line: the square at (x, y) with sides side
	x, y, side float64
	stroke     float64
	opacity    float64
}

func tunnelPower(base float64, exponent int) float64 {
	value := 1.0
	for i := 0; i < exponent; i++ {
		value *= base
	}
	return value
}

// tunnelSquareAt is TunnelIndicator.h's SquareAt.
func tunnelSquareAt(size float64, index int) tunnelSquare {
	outerStroke := tunnelOuterStroke * size
	outerOrigin := outerStroke / 2
	outerSide := size - outerStroke
	vanishing := tunnelVanishingPoint * size
	scale := tunnelPower(tunnelScaleRatio, index)
	origin := vanishing + (outerOrigin-vanishing)*scale
	return tunnelSquare{
		x:       origin,
		y:       origin,
		side:    outerSide * scale,
		stroke:  outerStroke * tunnelPower(tunnelStrokeRatio, index),
		opacity: 1 - tunnelOpacityStep*float64(index),
	}
}

// inStroke reports whether a point is on the square's stroke: inside its
// outer edge and outside its inner one, which with mitered corners are both
// squares.
func (square tunnelSquare) inStroke(x, y float64) bool {
	half := square.stroke / 2
	outerLow, outerHigh := square.x-half, square.x+square.side+half
	if x < outerLow || outerHigh <= x || y < square.y-half || square.y+square.side+half <= y {
		return false
	}
	innerLow, innerHigh := square.x+half, square.x+square.side-half
	return !(innerLow <= x && x < innerHigh && square.y+half <= y && y < square.y+square.side-half)
}

// renderTunnel draws depth squares in a size by size image of ink, each pixel's
// alpha its covered area times the opacity of the square covering it. The
// squares are nested with gaps, so a point is on one stroke at most.
func renderTunnel(size, depth int, ink color.NRGBA) *image.NRGBA {
	destination := image.NewNRGBA(image.Rect(0, 0, size, size))
	squares := make([]tunnelSquare, 0, depth)
	for index := 0; index < depth; index++ {
		squares = append(squares, tunnelSquareAt(float64(size), index))
	}
	for py := 0; py < size; py++ {
		for px := 0; px < size; px++ {
			coverage := 0.0
			for sy := 0; sy < tunnelSamples; sy++ {
				y := float64(py) + (float64(sy)+0.5)/tunnelSamples
				for sx := 0; sx < tunnelSamples; sx++ {
					x := float64(px) + (float64(sx)+0.5)/tunnelSamples
					for _, square := range squares {
						if square.inStroke(x, y) {
							coverage += square.opacity
							break
						}
					}
				}
			}
			alpha := coverage / (tunnelSamples * tunnelSamples)
			destination.SetNRGBA(px, py, color.NRGBA{
				R: ink.R, G: ink.G, B: ink.B,
				A: uint8(clamp(alpha*float64(ink.A), 0, 255) + 0.5),
			})
		}
	}
	return destination
}

func tunnelIconName(prefix string, level int) string {
	return fmt.Sprintf("%s%d.ico", prefix, level)
}

func generateTunnelIcons(output string) error {
	fmt.Println("tunnel icons:")
	for level := 1; level < len(tunnelDepthByLevel); level++ {
		depth := tunnelDepthByLevel[level]
		for _, variant := range tunnelInks {
			name := tunnelIconName(variant.prefix, level)
			frames := make([]image.Image, 0, len(traySizes))
			for _, size := range traySizes {
				frames = append(frames, renderTunnel(size, depth, variant.ink))
			}
			err := writeFileAtomically(filepath.Join(output, name), func(writer io.Writer) error {
				return encodeICOImages(writer, frames)
			})
			if err != nil {
				return fmt.Errorf("generate %s: %w", name, err)
			}
			fmt.Printf("  %s  <- %d squares\n", name, depth)
		}
	}
	return nil
}
