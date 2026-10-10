// The Account > Sessions glyphs (server session/REVOKE-UI-FINAL.md §8): the
// side-facing head the Account list's Sessions row leads with, and the device
// logos a session row draws in white on its country circle.
//
// Material Design Icons 7.4.47 by Pictogrammers, Apache-2.0 (the notice is in
// THIRD-PARTY-NOTICES.txt). Each is the icon's one path, unmodified, on the
// 24x24 box the icon is drawn in, and fills nonzero as its SVG does.
// SessionsPresentation.h picks a device's logo; kit::MakeRowPathIcon
// (UrComponents.h) draws a path as a PathIcon at any size.
//
// Plain wide-string constants, no WinRT, so this header costs nothing to
// include (ConnectorGlyph.h's rule).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw::glyph {

// the box every path below is authored in
inline constexpr double kSessionGlyphViewBox = 24.0;

// session-face-profile (MDI head-outline): the Account > Sessions row
inline constexpr const wchar_t* kSessionFaceProfilePath =
    L"M13 1C8.4 1 4.6 4.4 4.1 8.9L2.5 11C2 11.8 1.9 12.8 2.3 13.6C2.7 14.3 3.3 14.8 4 "
    L"14.9V16C4 17.8 5.3 19.4 7 19.9V23H18V17.5C20.5 15.8 22 13.1 22 10C22 5 18 1 13 1M16 "
    L"16.3V21H9V18H8C6.9 18 6 17.1 6 16V13H4.5C4.1 13 3.8 12.5 4.1 12.2L6 9.7C6.2 6 9.2 3 13 "
    L"3C16.9 3 20 6.1 20 10C20 12.8 18.4 15.2 16 16.3Z";

// device-android (MDI android): android
inline constexpr const wchar_t* kDeviceAndroidPath =
    L"M16.61 15.15C16.15 15.15 15.77 14.78 15.77 14.32S16.15 13.5 16.61 13.5H16.61C17.07 "
    L"13.5 17.45 13.86 17.45 14.32C17.45 14.78 17.07 15.15 16.61 15.15M7.41 15.15C6.95 15.15 "
    L"6.57 14.78 6.57 14.32C6.57 13.86 6.95 13.5 7.41 13.5H7.41C7.87 13.5 8.24 13.86 8.24 "
    L"14.32C8.24 14.78 7.87 15.15 7.41 15.15M16.91 10.14L18.58 7.26C18.67 7.09 18.61 6.88 "
    L"18.45 6.79C18.28 6.69 18.07 6.75 18 6.92L16.29 9.83C14.95 9.22 13.5 8.9 12 8.91C10.47 "
    L"8.91 9 9.24 7.73 9.82L6.04 6.91C5.95 6.74 5.74 6.68 5.57 6.78C5.4 6.87 5.35 7.08 5.44 "
    L"7.25L7.1 10.13C4.25 11.69 2.29 14.58 2 18H22C21.72 14.59 19.77 11.7 16.91 10.14H16.91Z";

// device-apple (MDI apple): ios and macos
inline constexpr const wchar_t* kDeviceApplePath =
    L"M18.71,19.5C17.88,20.74 17,21.95 15.66,21.97C14.32,22 13.89,21.18 "
    L"12.37,21.18C10.84,21.18 10.37,21.95 9.1,22C7.79,22.05 6.8,20.68 5.96,19.47C4.25,17 "
    L"2.94,12.45 4.7,9.39C5.57,7.87 7.13,6.91 8.82,6.88C10.1,6.86 11.32,7.75 "
    L"12.11,7.75C12.89,7.75 14.37,6.68 15.92,6.84C16.57,6.87 18.39,7.1 19.56,8.82C19.47,8.88 "
    L"17.39,10.1 17.41,12.63C17.44,15.65 20.06,16.66 20.09,16.67C20.06,16.74 19.67,18.11 "
    L"18.71,19.5M13,3.5C13.73,2.67 14.94,2.04 15.94,2C16.07,3.17 15.6,4.35 "
    L"14.9,5.19C14.21,6.04 13.07,6.7 11.95,6.61C11.8,5.46 12.36,4.26 13,3.5Z";

// device-windows (MDI microsoft-windows): windows
inline constexpr const wchar_t* kDeviceWindowsPath =
    L"M3,12V6.75L9,5.43V11.91L3,12M20,3V11.75L10,11.9V5.21L20,3M3,13L9,13.09V19.9L3,18.75V13"
    L"M20,13.25V22L10,20.09V13.1L20,13.25Z";

// device-linux (MDI linux): linux
inline constexpr const wchar_t* kDeviceLinuxPath =
    L"M14.62,8.35C14.2,8.63 12.87,9.39 12.67,9.54C12.28,9.85 11.92,9.83 "
    L"11.53,9.53C11.33,9.37 10,8.61 9.58,8.34C9.1,8.03 9.13,7.64 9.66,7.42C11.3,6.73 "
    L"12.94,6.78 14.57,7.45C15.06,7.66 15.08,8.05 14.62,8.35M21.84,15.63C20.91,13.54 "
    L"19.64,11.64 18,9.97C17.47,9.42 17.14,8.8 16.94,8.09C16.84,7.76 16.77,7.42 "
    L"16.7,7.08C16.5,6.2 16.41,5.3 16,4.47C15.27,2.89 14,2.07 12.16,2C10.35,2.05 9,2.81 "
    L"8.21,4.4C8,4.83 7.85,5.28 7.75,5.74C7.58,6.5 7.43,7.29 7.25,8.06C7.1,8.71 6.8,9.27 "
    L"6.29,9.77C4.68,11.34 3.39,13.14 2.41,15.12C2.27,15.41 2.13,15.7 2.04,16C1.85,16.66 "
    L"2.33,17.12 3.03,16.96C3.47,16.87 3.91,16.78 4.33,16.65C4.74,16.5 4.9,16.6 "
    L"5,17C5.65,19.15 7.07,20.66 9.24,21.5C13.36,23.06 18.17,20.84 19.21,16.92C19.28,16.65 "
    L"19.38,16.55 19.68,16.65C20.14,16.79 20.61,16.89 21.08,17C21.57,17.09 21.93,16.84 "
    L"22,16.36C22.03,16.1 21.94,15.87 21.84,15.63";

// device-web (MDI web): web
inline constexpr const wchar_t* kDeviceWebPath =
    L"M16.36,14C16.44,13.34 16.5,12.68 16.5,12C16.5,11.32 16.44,10.66 "
    L"16.36,10H19.74C19.9,10.64 20,11.31 20,12C20,12.69 19.9,13.36 "
    L"19.74,14M14.59,19.56C15.19,18.45 15.65,17.25 15.97,16H18.92C17.96,17.65 16.43,18.93 "
    L"14.59,19.56M14.34,14H9.66C9.56,13.34 9.5,12.68 9.5,12C9.5,11.32 9.56,10.65 "
    L"9.66,10H14.34C14.43,10.65 14.5,11.32 14.5,12C14.5,12.68 14.43,13.34 "
    L"14.34,14M12,19.96C11.17,18.76 10.5,17.43 10.09,16H13.91C13.5,17.43 12.83,18.76 "
    L"12,19.96M8,8H5.08C6.03,6.34 7.57,5.06 9.4,4.44C8.8,5.55 8.35,6.75 "
    L"8,8M5.08,16H8C8.35,17.25 8.8,18.45 9.4,19.56C7.57,18.93 6.03,17.65 "
    L"5.08,16M4.26,14C4.1,13.36 4,12.69 4,12C4,11.31 4.1,10.64 4.26,10H7.64C7.56,10.66 "
    L"7.5,11.32 7.5,12C7.5,12.68 7.56,13.34 7.64,14M12,4.03C12.83,5.23 13.5,6.57 "
    L"13.91,8H10.09C10.5,6.57 11.17,5.23 12,4.03M18.92,8H15.97C15.65,6.75 15.19,5.55 "
    L"14.59,4.44C16.43,5.07 17.96,6.34 18.92,8M12,2C6.47,2 2,6.5 2,12A10,10 0 0,0 "
    L"12,22A10,10 0 0,0 22,12A10,10 0 0,0 12,2Z";

// device-cli (MDI console): cli
inline constexpr const wchar_t* kDeviceCliPath =
    L"M20,19V7H4V19H20M20,3A2,2 0 0,1 22,5V19A2,2 0 0,1 20,21H4A2,2 0 0,1 2,19V5C2,3.89 "
    L"2.9,3 4,3H20M13,17V15H18V17H13M9.58,13L5.57,9H8.4L11.7,12.3C12.09,12.69 12.09,13.33 "
    L"11.7,13.72L8.42,17H5.59L9.58,13Z";

// device-server (MDI server): server
inline constexpr const wchar_t* kDeviceServerPath =
    L"M4,1H20A1,1 0 0,1 21,2V6A1,1 0 0,1 20,7H4A1,1 0 0,1 3,6V2A1,1 0 0,1 4,1M4,9H20A1,1 0 "
    L"0,1 21,10V14A1,1 0 0,1 20,15H4A1,1 0 0,1 3,14V10A1,1 0 0,1 4,9M4,17H20A1,1 0 0,1 "
    L"21,18V22A1,1 0 0,1 20,23H4A1,1 0 0,1 3,22V18A1,1 0 0,1 "
    L"4,17M9,5H10V3H9V5M9,13H10V11H9V13M9,21H10V19H9V21M5,3V5H7V3H5M5,11V13H7V11H5M5,19V21H7V19H5Z";

// device-unknown (MDI help): unknown, empty and any other device type
inline constexpr const wchar_t* kDeviceUnknownPath =
    L"M10,19H13V22H10V19M12,2C17.35,2.22 19.68,7.62 16.5,11.67C15.67,12.67 14.33,13.33 "
    L"13.67,14.17C13,15 13,16 13,17H10C10,15.33 10,13.92 10.67,12.92C11.33,11.92 12.67,11.33 "
    L"13.5,10.67C15.92,8.43 15.32,5.26 12,5A3,3 0 0,0 9,8H6A6,6 0 0,1 12,2Z";

}  // namespace urnw::glyph
