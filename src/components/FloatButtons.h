#pragma once

#include <Icon.h>

#include <cstdint>

class GfxRenderer;

// Floating chrome (Pulp on touch boards): pills and round buttons drawn
// straight into the framebuffer over a page or list. Callers own layout and
// hit testing.
namespace float_ui {

constexpr int kButton = 56;  // round button diameter and pill height
constexpr int kMargin = 16;
constexpr int kGap = 8;

// The reader's and Home's chrome: larger and flat, filled with kReaderFill.
constexpr int kReaderButton = 80;
constexpr int kReaderMargin = 20;  // from the right edge
constexpr int kReaderEdge = 30;    // from the top and bottom edges

enum class Fill : uint8_t {
  Outline,  // white with an outline and a dithered drop shadow
  Black,    // solid black; icons and labels go on it in white
  Dither,   // 25% black dither that reads as light gray (no gray pass); black ink
  Ring,     // white, inside a band of that light-gray dither; black ink
};
// The reader's and Home's buttons. Ink goes on in inkOn(kReaderFill).
constexpr Fill kReaderFill = Fill::Ring;
constexpr int kRingWidth = 6;  // Fill::Ring's dithered band
// Ink color on a shape of this fill: white on Black, black otherwise.
constexpr bool inkOn(const Fill fill) { return fill != Fill::Black; }

struct Box {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  // Taps land low on this panel (Tap Target Test), so the area reaches further down.
  bool contains(int px, int py) const;
};

// A pill (or circle when w == h).
void drawShape(const GfxRenderer& renderer, int x, int y, int w, int h, Fill fill = Fill::Outline);
// freeink::Icon bits at logical (x, y), in black or (on a Fill::Black shape) white.
// `halo` clears a white outline around the ink first, so it reads on a dither.
void drawIcon(const GfxRenderer& renderer, const freeink::Icon& icon, int x, int y, bool black = true,
              bool halo = false);
// A label on a shape of this fill: ink as inkOn(fill), outlined in white on a dither.
void drawLabel(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, Fill fill);
// A round button with the icon centred.
void drawRound(const GfxRenderer& renderer, const Box& box, const freeink::Icon& icon, Fill fill = Fill::Outline);

}  // namespace float_ui
