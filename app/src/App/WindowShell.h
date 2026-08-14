// The native Windows shell around the main window: backdrop, title-bar chrome,
// a compact default size, and placement that survives a restart.
//
// None of this is brand work. The palette, the faces, the connect canvas and
// the colour dots are untouched by this file; what it does is make the window
// behave like a Windows window rather than like a page that happens to have a
// frame around it. Before it, the app never called Resize at all and opened at
// ~1920x1094 — a tray app filling the entire work area.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <windows.h>

#include <winrt/Microsoft.UI.Xaml.h>

namespace urnw::shell {

// The compact default, in DIPs; scaled by the window's own DPI before use.
// A tray flyout, not a workspace.
// The first-run default is a DESKTOP reading, not the phone-shaped 480x760
// compact this shipped with from the tray-flyout era. 480dip is far below
// kWideBreakpointDip, so a first launch rendered the narrow login with the
// brand art collapsed — the beta report was "opens on top left of screen,
// sized weirdly so our branding doesn't show". 1120dip keeps the client
// comfortably past the wide breakpoint at any scale; the default branch in
// ApplyNativeShell clamps to the work area, so small screens shrink
// gracefully (and a shrunk-below-breakpoint window correctly reads narrow).
inline constexpr int kDefaultWidthDips = 1120;
inline constexpr int kDefaultHeightDips = 820;
// Below this the drawer's card column has nothing left to give.
inline constexpr int kMinWidthDips = 400;
inline constexpr int kMinHeightDips = 480;

// Apply the shell to a just-created window. Call once, after the window exists
// and before it is first activated.
//
// - System backdrop: Mica where the OS supports it (Windows 11), which also
//   means clearing the root element's opaque background, or the Mica is drawn
//   and then painted over — a mechanism with no signal. On Windows 10 the
//   solid #101010 stays and nothing else changes. Mica also carries the brand
//   background as its FallbackColor, for the several ways it can degrade at
//   runtime while still being "supported".
// - Title bar: the caption buttons are drawn by the system over the extended
//   content, so their colours are set to match the brand surface. The drag
//   region itself is the window's own AppTitleBar element (MainWindow sets it).
// - Placement: restores the last saved size and position, else the compact
//   default centred on the current monitor; either way clamped onto a monitor
//   that actually exists. A saved position that does not overlap ANY monitor
//   Windows currently reports (most often: the monitor it was saved on has
//   since been unplugged, so the rect just sits in space that used to be a
//   screen) is discarded outright rather than clamped - the compact default,
//   centred on the primary, is used instead.
//
// RETURNS true when a SAVED placement was restored, which the caller must
// honour: the tray-anchor flyout move would otherwise overwrite the position
// the user chose, one statement after this function applied it. The anchor is
// a default, not an override — see AppController::ShowWindowImpl.
bool ApplyNativeShell(winrt::Microsoft::UI::Xaml::Window const& window, HWND hwnd);

// Record the window's current size and position. Called on the two ways the
// window goes away — hidden to the tray, and quit — rather than on every frame
// of a drag. Returns whether a complete placement was written.
bool SaveWindowPlacement(HWND hwnd);

}  // namespace urnw::shell
