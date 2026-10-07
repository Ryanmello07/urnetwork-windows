// The ur connector mark, as a XAML path.
//
// One 32x32 outline -- Assets.xcassets/Icons/ur.symbols.globe.svg, which is
// android's connector_globe and the 256-box GlobeMask/GlobeConnector assets
// divided by eight -- used by every surface in this app that draws the brand
// shape:
//
//   ConnectCanvas    the hero's globe silhouette, its connector backdrop, the
//                    inverse mask and the focus ring
//   LoginCarousel    the sign-in globe the slides are clipped to
//   ExtenderSheets   the glyph at the centre of the extender share QR (K7)
//
// It lived as a byte-identical copy in the first two of those; a third copy for
// the share code is what made it worth having one. The STRING is shared, not
// the element: each surface parses it with its own Stretch and alignment (the
// hero fills its box exactly, the carousel letterboxes into a slot), and
// Path.Data's mini-language has no runtime parser a C++ caller can reach -- no
// Geometry.Parse, and PathGeometry takes a figure collection rather than a
// string -- so each one loads a one-element document through XamlReader.
//
// Plain wide-string constants, no WinRT, so this header costs nothing to
// include.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw::glyph {

inline constexpr const wchar_t* kConnectorPath =
    L"M30 8C28.8955 8 28 7.10453 28 6C28 4.89547 27.1045 4 26 4C24.8955 4 24 3.10453 24 2C24 "
    L"0.895469 23.1045 0 22 0H10C8.89547 0 8 0.895469 8 2C8 3.10453 7.10453 4 6 4C4.89547 4 4 "
    L"4.89547 4 6C4 7.10453 3.10453 8 2 8C0.895469 8 0 8.89547 0 10V22C0 23.1045 0.895469 24 2 "
    L"24C3.10453 24 4 24.8955 4 26C4 27.1045 4.89547 28 6 28C7.10453 28 8 28.8955 8 30C8 31.1045 "
    L"8.89547 32 10 32H22C23.1045 32 24 31.1045 24 30C24 28.8955 24.8955 28 26 28C27.1045 28 28 "
    L"27.1045 28 26C28 24.8955 28.8955 24 30 24C31.1045 24 32 23.1045 32 22V10C32 8.89547 31.1045 "
    L"8 30 8Z";

// The box the path above is authored in, so a caller scaling it by hand (the
// share code's outline) does not have to rediscover it.
inline constexpr double kConnectorViewBox = 32.0;

}  // namespace urnw::glyph
