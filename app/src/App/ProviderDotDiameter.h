// The connect widget's dot diameter, by the canvas's own rule
// (ConnectCanvas::Layout): the globe's side over the larger grid dimension, so
// a non-square grid still fits inside the globe. Header-only standard C++ so
// tools/ip-family-tests.cpp pins the rule on any host; ConnectCanvas reads it
// for PointDiameterFor. (It lived in IpFamilyGroups.h while the drawer's IP
// family section drew provider dots at the hero's size; that section is a text
// row now — IpFamilyStatus.h — and the canvas is the rule's only consumer.)
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw {

// iOS's 256pt canvas, standing in for a side that has not been measured yet
inline constexpr double kProviderDotDefaultCanvasSide = 256.0;
// the default column count, for a grid with no shape yet
inline constexpr int64_t kProviderDotDefaultGridWidth = 14;

// `canvasSide` is the live side (168..288 on windows) or 0 before the first
// layout, when iOS's 256pt canvas stands in; a grid with no shape yet uses the
// default column count. Never zero, so a dot always has a size to be drawn at.
inline double ProviderDotDiameter(double canvasSide, int64_t gridWidth, int64_t gridHeight) {
  const double side = 0 < canvasSide ? canvasSide : kProviderDotDefaultCanvasSide;
  // ConnectCanvas::Layout: iOS scales by gridWidth alone; taking the larger of
  // the two keeps a non-square grid inside the globe
  int64_t cols = 0 < gridWidth ? gridWidth : 0;
  if (0 < gridHeight && cols < gridHeight) cols = gridHeight;
  if (cols <= 0) cols = kProviderDotDefaultGridWidth;
  return side / static_cast<double>(cols);
}

}  // namespace urnw
