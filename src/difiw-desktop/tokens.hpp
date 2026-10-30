// Difiw design tokens: the single source of colors, sizes, radii, timings and rules.
// Do not scatter literal values through the code; add a token here instead.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace dg::tok {

// --- color palette (0xAARRGGBB, straight alpha) ---------------------------------------
constexpr std::uint32_t kGraphite900 = 0xFF0E1118;  // deepest background
constexpr std::uint32_t kGraphite800 = 0xFF151A24;  // window body
constexpr std::uint32_t kGraphite700 = 0xFF1D2330;  // raised surface
constexpr std::uint32_t kGraphite600 = 0xFF2A3142;  // buttons, fields
constexpr std::uint32_t kGraphite500 = 0xFF3A4358;  // hover surface
constexpr std::uint32_t kText = 0xFFE9EDF7;         // primary text
constexpr std::uint32_t kTextDim = 0xFF9AA4BD;      // secondary text
constexpr std::uint32_t kTextFaint = 0xFF6B7690;    // hints, disabled
constexpr std::uint32_t kAccentBlue = 0xFF4C8DFF;
constexpr std::uint32_t kAccentTeal = 0xFF2DD4BF;
constexpr std::uint32_t kAccentViolet = 0xFF8B5CF6;
constexpr std::uint32_t kOk = 0xFF59C27A;
constexpr std::uint32_t kWarn = 0xFFF2B84B;
constexpr std::uint32_t kDanger = 0xFFE5604F;

// --- glass panels -----------------------------------------------------------------------
constexpr std::uint32_t kGlassTint = 0xA00F131C;       // tint over the blurred wallpaper
constexpr std::uint32_t kGlassTintOpaque = 0xFF141925;  // when transparency is off
constexpr std::uint32_t kGlassBorder = 0x30FFFFFF;      // thin light border
constexpr std::uint32_t kGlassHighlight = 0x16FFFFFF;   // inner top highlight
constexpr std::uint32_t kShadow = 0x60000000;

// --- radii, spacing, sizes (logical pixels; multiplied by the UI scale) -------------------
constexpr int kRadiusWindow = 14;
constexpr int kRadiusPanel = 22;
constexpr int kRadiusDock = 28;
constexpr int kRadiusIcon = 14;
constexpr int kRadiusControl = 10;
constexpr int kGap = 8;
constexpr int kPad = 14;
constexpr int kDockHeight = 68;
constexpr int kDockMargin = 12;
constexpr int kDockIcon = 46;
constexpr int kDockSlot = 56;      // icon + gap
constexpr int kDockSideWidth = 330;  // width of the user-apps strip and of the system area
constexpr int kDockDWidth = 64;
constexpr int kTitleBar = 34;
constexpr int kResizeGrip = 7;
constexpr int kMenuCols = 5;
constexpr int kMenuIcon = 52;
constexpr int kMenuCellW = 94;     // width of one application cell in the menu
constexpr int kTasksPerSection = 25;  // background tasks: one section holds up to 25 items
constexpr int kTasksCols = 5;
constexpr int kMinWindowW = 220;
constexpr int kMinWindowH = 140;

// --- motion (milliseconds) ------------------------------------------------------------------
constexpr int kAnimHover = 140;
constexpr int kAnimPress = 110;
constexpr int kAnimPopup = 190;
constexpr int kAnimWindow = 160;
constexpr float kHoverLift = 5.0f;   // pixels an icon rises on hover
constexpr float kHoverScale = 1.14f;
constexpr float kPressScale = 0.90f;
constexpr float kGlowSpeed = 0.00035f;  // palette rotation per millisecond

// --- behaviour -------------------------------------------------------------------------------
constexpr int kDoubleClickMs = 450;
constexpr int kTermGraceMs = 2500;  // time given to an app to exit before SIGKILL
constexpr int kTaskRefreshMs = 2000;

}  // namespace dg::tok
