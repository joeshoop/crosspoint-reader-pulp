#include "PulpTheme.h"

#include <Arduino.h>
#include <FreeInkUI.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "PulpBookFrame.h"
#include "PulpBookFrameSmall.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookProgress.h"

namespace {
constexpr int kCoverInset = 4;  // frame to cover: 2px outline + 2px white margin
constexpr int kCoverRadius = 2;
// The cover sits on its book: page edges and shadow come from a 9-slice
// image that stretches to the cover size, with the cover at its top left.
// Home's book has five pages (images/book-frame.png); the smaller Books grid
// covers have three (images/book-frame-small.png).
struct BookFrame {
  int width, height;
  int sliceLeft, sliceTop, sliceRight, sliceBottom;
  int coverRight, coverBottom;  // frame past the cover's right and bottom edges
  int backPageDepth;            // the back page's offset from the cover
  bool (*isBlack)(int x, int y);
};
namespace frame = pulp_book_frame;
namespace small_frame = pulp_book_frame_small;
constexpr BookFrame kHomeFrame{frame::kWidth,      frame::kHeight,      frame::kSliceLeft,
                               frame::kSliceTop,   frame::kSliceRight,  frame::kSliceBottom,
                               frame::kCoverRight, frame::kCoverBottom, 10,
                               frame::isBlack};
constexpr BookFrame kShelfFrame{small_frame::kWidth,      small_frame::kHeight,      small_frame::kSliceLeft,
                                small_frame::kSliceTop,   small_frame::kSliceRight,  small_frame::kSliceBottom,
                                small_frame::kCoverRight, small_frame::kCoverBottom, 6,
                                small_frame::isBlack};
constexpr int kBookExtraX = frame::kCoverRight;
constexpr int kBookExtraY = frame::kCoverBottom;
// Books grid: room beside each cover for the selection loop and the page
// edges and bookmark, and air above and below the book.
constexpr int kShelfSideZone = 24;
constexpr int kShelfTopPad = 20;  // room for the selection loop, above and below the book
constexpr int kShelfLoopThickness = 3;
// Ribbon tucked into the right page edge, lower as more is read.
constexpr int kRibbonHeight = 14;  // as the reader's progress ribbon
constexpr int kRibbonReach = 10;   // past the frame's right edge
constexpr int kRibbonNotch = 4;
constexpr int kRibbonMargin = 6;  // travel stops short of the cover corners
// Page layers in the frame images are 2px apart.
constexpr int kPageStep = 2;
constexpr int kMenuMaxWidth = 360;  // until a cover has set the width
constexpr int kMenuPaddingY = 8;
constexpr int kHomeMenuRows = 2;    // Books, Settings (homeMinimalMenu)
constexpr int kHintClearance = 22;  // air between the home menu and the button hints
constexpr int kHintGroupWidth = 176;
constexpr int kHintGap = 12;
constexpr int kHintRadius = 4;
constexpr int kHintPaddingX = 10;
constexpr int kGuideFontId = HINT_PIXEL_FONT_ID;
// Baseline height above the panel edge; the X4 bezel hides the bottom rows.
constexpr int kHintBaselineInset = 9;

// Cover width from the last parsed thumbnail; kept so a home redraw from the
// stored cover buffer can place the frame without reopening the BMP.
int coverWidth = 0;

std::string upperAscii(const char* text) {
  std::string out = text ? text : "";
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

// Hint labels in caps, without the "« " some carry ("« Back" reads BACK).
std::string hintLabel(const char* text) {
  constexpr char kGuillemet[] = "\xC2\xAB ";
  if (text && std::strncmp(text, kGuillemet, sizeof(kGuillemet) - 1) == 0) text += sizeof(kGuillemet) - 1;
  return upperAscii(text);
}

constexpr int kTabTextInset = 8;

// A tab on the right screen edge labelling the buttons beside it: the first
// label at its top, the second at its bottom, each textInset from its end.
void drawEdgeTab(const GfxRenderer& renderer, const int tabTop, const int tabHeight, const char* first,
                 const char* second, const int textInset = kTabTextInset) {
  const int tabWidth = PulpMetrics::values.buttonHintsHeight;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int x = screenW - tabWidth;
  renderer.fillRect(x, tabTop, tabWidth, tabHeight, false);
  renderer.drawRoundedRect(x, tabTop, tabWidth, tabHeight, 1, kHintRadius, true, false, true, false, true);
  renderer.fillRect(screenW - 1, tabTop + 1, 1, tabHeight - 2, false);

  // The renderer only rotates text clockwise (reading upward); flipping the
  // orientation turns that into text reading downward, glyph tops to the edge.
  auto& flipped = const_cast<GfxRenderer&>(renderer);
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  const int textH = renderer.getTextHeight(kGuideFontId);
  const int columnX = x + (tabWidth - textH) / 2;
  const std::string labels[2] = {hintLabel(first), hintLabel(second)};
  flipped.setOrientation(GfxRenderer::Orientation::PortraitInverted);
  for (int i = 0; i < 2; ++i) {
    if (labels[i].empty()) continue;
    const int textW = renderer.getTextWidth(kGuideFontId, labels[i].c_str());
    const int top = i == 0 ? tabTop + textInset : tabTop + tabHeight - textInset - textW;
    renderer.drawTextRotated90CW(kGuideFontId, screenW - columnX - textH, screenH - 1 - top, labels[i].c_str());
  }
  flipped.setOrientation(origOrientation);
}
// Maps a destination offset along one axis to the 9-slice source: corners
// copy as-is, the middle span repeats.
int nineSliceSource(const int d, const int destSize, const int srcSize, const int sliceStart, const int sliceEnd) {
  if (d < sliceStart) return d;
  if (d >= destSize - sliceEnd) return srcSize - (destSize - d);
  return sliceStart + (d - sliceStart) % (srcSize - sliceStart - sliceEnd);
}

// Page edges and shadow around a cover at (x, y, w, h). The cover goes on top.
void drawBookBody(const GfxRenderer& renderer, const BookFrame& f, const int x, const int y, const int w, const int h) {
  const int frameW = w + f.coverRight;
  const int frameH = h + f.coverBottom;
  for (int dy = 0; dy < frameH; dy++) {
    const int sy = nineSliceSource(dy, frameH, f.height, f.sliceTop, f.sliceBottom);
    const bool coverRow = dy < h;
    for (int dx = 0; dx < frameW; dx++) {
      if (coverRow && dx < w) {
        dx = w - 1;
        continue;
      }
      const int sx = nineSliceSource(dx, frameW, f.width, f.sliceLeft, f.sliceRight);
      renderer.drawPixel(x + dx, y + dy, f.isBlack(sx, sy));
    }
  }
}

// A paper ribbon with a pencil outline, shared by the cover bookmark and the
// reader's progress ribbon so it looks the same in both. A paper state of -1
// leaves the inside as it is (the caller has already cleared it).
void fillPaperRibbon(const GfxRenderer& renderer, const int left, const int right, const int top, const int height,
                     const int notch, const bool notchOnLeft, const int paperState = -1) {
  const int half = std::max(1, height / 2);
  if (paperState >= 0) {
    for (int row = 0; row < height; row++) {
      const int depth = notch * std::max(0, half - std::abs(row - half)) / half;
      const int from = notchOnLeft ? left + depth : left;
      const int to = notchOnLeft ? right : right - depth;
      for (int x = from; x <= to; x++) renderer.drawPixel(x, top + row, paperState != 0);
    }
  }
  // Pencil outline on the free sides, in the weight of the other pencil marks:
  // the long edges and the notch. The end it hangs from (screen edge, or
  // between the pages) stays open.
  constexpr int kWeight = 2;
  const int bottom = top + height - kWeight;
  const int tail = notchOnLeft ? left : right - kWeight + 1;
  const int inner = notchOnLeft ? left + notch : right - notch;
  const int head = notchOnLeft ? right : left;
  const auto dot = [&renderer](const int x, const int y) { renderer.drawPixel(x, y); };
  freeink::ui::pencilSegment(head, top, tail, top, 0x3C6EF372u, dot, kWeight);
  freeink::ui::pencilSegment(tail, top, inner, top + half, 0xA54FF53Au, dot, kWeight);
  freeink::ui::pencilSegment(inner, top + half, tail, bottom, 0x510E527Fu, dot, kWeight);
  freeink::ui::pencilSegment(tail, bottom, head, bottom, 0x9B05688Cu, dot, kWeight);
}

// Bookmark ribbon on the right of a cover at (x, y, w, h). Further in the book
// it sits lower and comes out from between later pages. Drawn before the cover.
void drawBookmark(const GfxRenderer& renderer, const BookFrame& f, const int x, const int y, const int w, const int h,
                  const int percent) {
  const int p = std::clamp(percent, 0, 100);
  const int travel = h - kRibbonHeight - 2 * kRibbonMargin;
  const int top = y + kRibbonMargin + travel * p / 100;
  // Starts just past the edge of the page in front of it.
  const int gaps = f.backPageDepth / kPageStep;
  const int depth = (p * gaps + 50) / 100 * kPageStep;
  const int left = x + w + depth;
  const int right = x + w + f.coverRight + kRibbonReach;
  renderer.fillRect(left, top - 1, right - left + 2, kRibbonHeight + 2, false);  // halo off the page lines
  fillPaperRibbon(renderer, left, right, top, kRibbonHeight, kRibbonNotch, false);
}

// A recent book centred in [boxX, boxX + boxW) with its cover top at coverY:
// page edges, bookmark and cover art, or a plain gray board with the title
// and author when there is no art. maxWidth caps wide art; plainWidth sizes
// the board. Returns the cover width.
int drawBook(GfxRenderer& renderer, const BookFrame& f, const int boxX, const int boxW, const int coverY,
             const int coverH, const int maxWidth, const int plainWidth, const RecentBook& book, const int fontId) {
  const int percent = loadBookProgress(book.path);
  if (!book.coverBmpPath.empty()) {
    HalFile file;
    if (Storage.openFileForRead("THEME", UITheme::getCoverThumbPath(book.coverBmpPath, coverH), file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const int w = std::min(bitmap.getWidth(), maxWidth);
        const int x = boxX + (boxW - w - f.coverRight) / 2;
        drawBookBody(renderer, f, x, coverY, w, coverH);
        if (percent >= 0) drawBookmark(renderer, f, x, coverY, w, coverH, percent);
        BaseTheme::drawCoverThumbFill(renderer, bitmap, Rect{x, coverY, w, coverH});
        renderer.maskRoundedRectOutsideCorners(x, coverY, w, coverH, kCoverRadius, Color::White);
        renderer.drawRoundedRect(x, coverY, w, coverH, 1, kCoverRadius, true);
        file.close();
        return w;
      }
      file.close();
    }
  }

  // Plain bound book: gray board with the title and author set on it.
  const int w = plainWidth;
  const int x = boxX + (boxW - w - f.coverRight) / 2;
  drawBookBody(renderer, f, x, coverY, w, coverH);
  if (percent >= 0) drawBookmark(renderer, f, x, coverY, w, coverH, percent);
  renderer.fillRectDither(x, coverY, w, coverH, Color::LightGray);
  renderer.drawRoundedRect(x, coverY, w, coverH, 1, kCoverRadius, true);
  const int inset = std::min(16, w / 8);
  const int textX = x + inset;
  const int textWidth = w - 2 * inset;
  const int lineHeight = renderer.getLineHeight(fontId);
  UITheme::drawCenteredWrappedText(renderer, Rect{textX, coverY, textWidth, coverH / 2 + lineHeight}, fontId,
                                   book.title.c_str(), 4, true, EpdFontFamily::BOLD,
                                   UITheme::TextVerticalAlignment::BOTTOM);
  UITheme::drawCenteredWrappedText(renderer, Rect{textX, coverY + coverH / 2 + lineHeight, textWidth, 2 * lineHeight},
                                   fontId, book.author.c_str(), 2, true, EpdFontFamily::ITALIC,
                                   UITheme::TextVerticalAlignment::TOP);
  return w;
}
}  // namespace

// Clippings and menus: an even pencil line under the baseline, like an
// editor's mark. It follows the screen, so a line looks the same whether it
// is drawn whole or a word at a time.
static int pencilThickness(const int lineHeight) { return std::clamp(lineHeight / 10 + 1, 3, 5); }
// Air between the baseline and the line.
static int pencilGap(const int lineHeight) { return std::max(5, lineHeight / 6); }
// How much of the pencil's grain lands: muted lines read as gray.
static uint8_t pencilDarkness(const bool muted) { return muted ? 45 : 100; }

static void drawPencilUnderline(const GfxRenderer& renderer, const int fontId, const int textX, const int lineTop,
                                const int textWidth, const uint32_t seed, const bool muted = false) {
  const int lineHeight = renderer.getLineHeight(fontId);
  const int reach = lineHeight / 8;
  const freeink::ui::Rect area{
      static_cast<int16_t>(textX - reach),
      static_cast<int16_t>(lineTop + renderer.getFontAscenderSize(fontId) + pencilGap(lineHeight)),
      static_cast<int16_t>(textWidth + 2 * reach), static_cast<int16_t>(pencilThickness(lineHeight))};
  freeink::ui::pencilStroke(
      area, seed, [&renderer](const int x, const int y) { renderer.drawPixel(x, y); }, true, pencilDarkness(muted));
}

// Menu selection: a pencil underline under one line of text whose line box
// starts at lineTop, as fui lists underline theirs.
static void drawHighlight(const GfxRenderer& renderer, const int fontId, const int textX, const int lineTop,
                          const std::string& text) {
  uint32_t seed = 2166136261u;
  for (const char c : text) seed = (seed ^ static_cast<uint8_t>(c)) * 16777619u;
  drawPencilUnderline(renderer, fontId, textX, lineTop, renderer.getTextWidth(fontId, text.c_str()), seed);
}

// A pencil asterisk left of the selection's underline, on centerY; `seed` picks its strokes.
static void drawSelectionArrow(const GfxRenderer& renderer, const int fontId, const int textX, const int centerY,
                               const uint32_t seed) {
  const int lineHeight = renderer.getLineHeight(fontId);
  const int size = std::max(14, lineHeight * 3 / 4);
  const int cx = textX - lineHeight * 3 / 4 - lineHeight / 4;
  const freeink::ui::Rect star{static_cast<int16_t>(cx - size / 2), static_cast<int16_t>(centerY - size / 2),
                               static_cast<int16_t>(size), static_cast<int16_t>(size)};
  freeink::ui::pencilAsterisk(star, seed, [&renderer](const int x, const int y) { renderer.drawPixel(x, y); });
}

// A big pencil arrow beside the label (either side), pointing up at the book,
// between `top` and `bottom` (clear of the book's pages): a gently curved
// shaft and a closed head. Side, curve and head size come from `seed`.
static void drawUpArrow(const GfxRenderer& renderer, const int fontId, const int textLeft, const int textRight,
                        const int top, const int bottom, uint32_t seed) {
  const auto random = [&seed](const int span) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return static_cast<int>(seed % static_cast<uint32_t>(span));
  };
  const int lineHeight = renderer.getLineHeight(fontId);
  const bool onRight = random(2) != 0;
  const float cx = static_cast<float>(onRight ? textRight + lineHeight : textLeft - lineHeight);
  const float headLen = lineHeight * (0.38f + random(8) / 100.0f);
  const float height = static_cast<float>(bottom - top) - headLen;
  if (height < 6) return;
  // Shaft: a cubic Bezier from the foot up to the head's base, bowed outward
  // so the tip leans back in toward the book.
  const float sweep = lineHeight * (0.12f + random(12) / 100.0f);
  const float side = onRight ? -1.0f : 1.0f;
  const float x0 = cx + side * sweep * 0.5f, y0 = static_cast<float>(bottom);
  const float x3 = cx, y3 = top + headLen;
  const float x1 = x0 - side * sweep, y1 = y0 - height * 0.4f;
  const float x2 = x3 - side * sweep * 0.3f, y2 = y3 + height * 0.35f;
  const auto at = [&](const float t, float& x, float& y) {
    const float u = 1 - t;
    x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
    y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
  };
  constexpr int kWeight = 2;
  const int steps = static_cast<int>(height * 3) + 8;
  for (int i = 0; i <= steps; ++i) {
    const float t = static_cast<float>(i) / steps;
    float x, y;
    at(t, x, y);
    const int px = static_cast<int>(x + 0.5f), py = static_cast<int>(y + 0.5f);
    for (int dy = 0; dy < kWeight; ++dy)
      for (int dx = 0; dx < kWeight; ++dx)
        if (freeink::ui::pencilGrain(px + dx, py + dy, t < 0.08f ? 60 : 88)) renderer.drawPixel(px + dx, py + dy);
  }
  // Closed head on the shaft's final direction.
  float dx = x3 - x2, dy = y3 - y2;
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.5f) return;
  dx /= len;
  dy /= len;
  const float half = headLen * (0.5f + random(10) / 100.0f);
  const int tipX = static_cast<int>(x3 + dx * headLen), tipY = static_cast<int>(y3 + dy * headLen);
  const int leftX = static_cast<int>(x3 + dy * half), leftY = static_cast<int>(y3 - dx * half);
  const int rightX = static_cast<int>(x3 - dy * half), rightY = static_cast<int>(y3 + dx * half);
  const auto plot = [&renderer](const int x, const int y) { renderer.drawPixel(x, y); };
  freeink::ui::pencilSegment(leftX, leftY, tipX, tipY, seed + 1, plot, kWeight, 0.5f);
  freeink::ui::pencilSegment(tipX, tipY, rightX, rightY, seed + 2, plot, kWeight, 0.5f);
  freeink::ui::pencilSegment(rightX, rightY, leftX, leftY, seed + 3, plot, kWeight, 0.5f);
}

// A small pencil book for the shelf list rows. Sketched once into a static
// 1-bit buffer, so every row shares it and no cover is read.
freeink::ui::BitmapRef PulpTheme::shelfBookIcon() const {
  constexpr int kSize = 28;
  constexpr int kStride = (kSize + 7) / 8;
  static uint8_t bits[kSize * kStride];
  static bool drawn = false;
  if (!drawn) {
    drawn = true;
    const auto plot = [](const int x, const int y) {
      if (x < 0 || y < 0 || x >= kSize || y >= kSize) return;
      bits[y * kStride + x / 8] |= static_cast<uint8_t>(0x80u >> (x % 8));
    };
    uint32_t seed = 0x6A09E667u;
    const auto line = [&](const int x0, const int y0, const int x1, const int y1, const int weight) {
      freeink::ui::pencilSegment(x0, y0, x1, y1, seed++, plot, weight, 0.5f);
    };
    // Cover, spine, and two title marks.
    line(6, 2, 22, 2, 2);
    line(22, 2, 22, 25, 2);
    line(22, 25, 6, 25, 2);
    line(6, 25, 6, 2, 2);
    line(10, 3, 10, 24, 1);
    line(13, 8, 19, 8, 1);
    line(13, 12, 17, 12, 1);
  }
  return freeink::ui::BitmapRef{bits, kSize, kSize, freeink::ui::BitmapFormat::BW1, false};
}

// A pencil sketch of books standing in a row, beside the label (either
// side). Side, count, heights and widths come from `seed`.
static void drawBookStack(const GfxRenderer& renderer, const int fontId, const int textLeft, const int textRight,
                          const int centerY, uint32_t seed) {
  const auto random = [&seed](const int span) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return static_cast<int>(seed % static_cast<uint32_t>(span));
  };
  const int lineHeight = renderer.getLineHeight(fontId);
  const int size = std::max(14, lineHeight * 3 / 4);
  const int cx = random(2) ? textRight + lineHeight : textLeft - lineHeight;
  constexpr int kMaxBooks = 4;
  const int count = 3 + random(2);
  int widths[kMaxBooks], heights[kMaxBooks];
  int total = 0;
  for (int b = 0; b < count; ++b) {
    widths[b] = size / 4 + random(size / 8 + 2);
    heights[b] = size - 4 + random(7);
    total += widths[b];
  }
  const int bottom = centerY + (size + 2) / 2;
  const int left = cx - total / 2;
  constexpr float kLooseness = 0.5f;
  const auto plot = [&renderer](const int x, const int y) { renderer.drawPixel(x, y); };
  const auto line = [&](const int x0, const int y0, const int x1, const int y1) {
    freeink::ui::pencilSegment(x0, y0, x1, y1, seed++, plot, 2, kLooseness);
  };
  // Left to right, standing on a shared shelf line.
  int l = left;
  for (int b = 0; b < count; ++b) {
    const int top = bottom - heights[b] + 1;
    line(l, top, l + widths[b], top);
    // A shared side runs up to the taller neighbour's top.
    const int sideTop = b > 0 ? std::min(top, bottom - heights[b - 1] + 1) : top;
    line(l, sideTop, l, bottom);
    l += widths[b];
  }
  line(l, bottom - heights[count - 1] + 1, l, bottom);
  line(left, bottom, l, bottom);
}

// A sketched wifi mark beside the label (either side): a dot under three
// rising arcs, each a little off in reach and radius. Side and wobble come
// from `seed`.
static void drawWifiSketch(const GfxRenderer& renderer, const int fontId, const int textLeft, const int textRight,
                           const int centerY, uint32_t seed) {
  const auto random = [&seed](const int span) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return static_cast<int>(seed % static_cast<uint32_t>(span));
  };
  const int lineHeight = renderer.getLineHeight(fontId);
  const int size = std::max(14, lineHeight * 3 / 4);
  const int cx = random(2) ? textRight + lineHeight : textLeft - lineHeight;
  // The arcs fan out from the dot, which sits at the foot of the mark.
  const int baseY = centerY + size / 2;
  const auto plot = [&renderer](const int x, const int y) { renderer.drawPixel(x, y); };
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) renderer.drawPixel(cx + dx, baseY - 1 + dy);
  constexpr float kPi = 3.14159265f;
  constexpr int kChords = 5;
  for (int arc = 1; arc <= 3; ++arc) {
    const float radius = size * arc / 3.0f + (random(3) - 1);
    const float from = kPi * (1.25f + (random(9) - 4) / 100.0f);
    const float to = kPi * (1.75f + (random(9) - 4) / 100.0f);
    int px = cx + static_cast<int>(radius * cosf(from));
    int py = baseY + static_cast<int>(radius * sinf(from));
    for (int c = 1; c <= kChords; ++c) {
      const float a = from + (to - from) * c / kChords;
      const int x = cx + static_cast<int>(radius * cosf(a));
      const int y = baseY + static_cast<int>(radius * sinf(a));
      freeink::ui::pencilSegment(px, py, x, y, seed++, plot, 2, 0.3f);
      px = x;
      py = y;
    }
  }
}

// The pencil line a word at a time: it follows the screen, so neighbouring
// words' lines join without a seam.
bool PulpTheme::drawTextHighlightBand(const GfxRenderer& renderer, const int x, const int lineTop, const int width,
                                      const int lineHeight, const bool muted) const {
  const int fontId = SETTINGS.getReaderFontId();
  const freeink::ui::Rect area{
      static_cast<int16_t>(x),
      static_cast<int16_t>(lineTop + renderer.getFontAscenderSize(fontId) + pencilGap(lineHeight)),
      static_cast<int16_t>(width), static_cast<int16_t>(pencilThickness(lineHeight))};
  freeink::ui::pencilStroke(
      area, 0, [&renderer](const int px, const int py) { renderer.drawPixel(px, py); }, true, pencilDarkness(muted));
  return true;
}

bool PulpTheme::drawTextHighlighter(const GfxRenderer& renderer, const int textX, const int lineTop,
                                    const int textWidth, const int lineHeight, const uint32_t seed,
                                    const bool muted) const {
  (void)lineHeight;
  drawPencilUnderline(renderer, SETTINGS.getReaderFontId(), textX, lineTop, textWidth, seed, muted);
  return true;
}

// A pencil box round the cursor's word, clipped to the box so erasing it
// leaves nothing behind. A thin line while browsing; once a selection starts
// from it, a heavy one filling the frame's margin.
bool PulpTheme::drawTextCursor(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                               const bool anchored) const {
  const int thickness = anchored ? 3 : 1;
  const int left = x;
  const int top = y;
  const int right = x + width - thickness;
  const int bottom = y + height - thickness;
  const auto dot = [&renderer, x, y, width, height](const int px, const int py) {
    if (px >= x && px < x + width && py >= y && py < y + height) renderer.drawPixel(px, py);
  };
  const uint32_t seed = static_cast<uint32_t>(x * 7919 + y * 104729) ^ (anchored ? 0x6A09E667u : 0u);
  freeink::ui::pencilSegment(left, top, right, top, seed ^ 0x3C6EF372u, dot, thickness);
  freeink::ui::pencilSegment(right, top, right, bottom, seed ^ 0xA54FF53Au, dot, thickness);
  freeink::ui::pencilSegment(right, bottom, left, bottom, seed ^ 0x510E527Fu, dot, thickness);
  freeink::ui::pencilSegment(left, bottom, left, top, seed ^ 0x9B05688Cu, dot, thickness);
  return true;
}

void PulpTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)bufferRestored;
  const int fontId = SETTINGS.getReaderFontId();
  const int coverHeight = homeCoverThumbHeight(renderer);
  if (coverWidth == 0) coverWidth = coverHeight * 5 / 8;

  if (recentBooks.empty()) {
    const int lineHeight = renderer.getLineHeight(fontId);
    const int y = rect.y + rect.height / 2 - lineHeight;
    renderer.drawCenteredText(fontId, y, tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(fontId, y + lineHeight, tr(STR_START_READING), true, EpdFontFamily::ITALIC);
    return;
  }

  const int cardWidth = std::min(rect.width, coverWidth + 2 * kCoverInset);
  const int cardX = rect.x + (rect.width - cardWidth) / 2;
  const int coverX = cardX + kCoverInset;
  const int coverY = rect.y + kCoverInset + frame::kCoverTop;

  // Only load from SD on first render, then reuse the stored buffer.
  if (!coverRendered) {
    coverWidth =
        drawBook(renderer, kHomeFrame, rect.x, rect.width, coverY, coverHeight,
                 rect.width - 2 * kCoverInset - kBookExtraX - 2 * kRibbonReach, coverWidth, recentBooks[0], fontId);

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
  }

  // Drawn after the snapshot so a restored buffer starts without the frame.
  const int labelTop = coverY + coverHeight + frame::kCoverBottom;
  const int labelHeight = rect.y + rect.height - labelTop;
  const std::string label =
      renderer.truncatedText(fontId, tr(STR_CONTINUE_READING), cardWidth - 2 * kHintPaddingX, EpdFontFamily::REGULAR);
  // A fresh arrow each time Continue Reading gains the selection; repaints while it stays selected keep it.
  static bool wasSelected = false;
  static uint32_t arrowSeed = 0x9E3779B9u;
  if (selectorIndex == 0) {
    if (!wasSelected) arrowSeed = (static_cast<uint32_t>(millis()) * 2654435761u) | 1u;
    const int labelX = (renderer.getScreenWidth() - renderer.getTextWidth(fontId, label.c_str())) / 2;
    drawHighlight(renderer, fontId, labelX, labelTop + (labelHeight - renderer.getLineHeight(fontId)) / 2, label);
    // Clear of the pages above, down to the foot of the label.
    const int lineHeight = renderer.getLineHeight(fontId);
    const int labelRight = labelX + renderer.getTextWidth(fontId, label.c_str());
    drawUpArrow(renderer, fontId, labelX, labelRight, labelTop + 6, labelTop + labelHeight / 2 + lineHeight / 2,
                arrowSeed);
  }
  wasSelected = selectorIndex == 0;
  renderer.drawCenteredText(fontId, labelTop + (labelHeight - renderer.getCenteringHeight(fontId)) / 2, label.c_str());
}

// Books grid: two columns with the content padding at the sides and between
// them (LibraryListActivity lays the cells out the same way); the cover is
// as wide as the column allows, at 2:3.
int PulpTheme::shelfCoverHeight(const GfxRenderer& renderer) const {
  const int side = PulpMetrics::values.contentSidePadding;
  const int column = (renderer.getScreenWidth() - 3 * side) / 2;
  return (column - 2 * kShelfSideZone) * 3 / 2;
}

int PulpTheme::shelfCellHeight(const GfxRenderer& renderer) const {
  return kShelfTopPad + shelfCoverHeight(renderer) + kShelfFrame.coverBottom + kShelfTopPad;
}

int PulpTheme::shelfBookWidth(const int coverHeight) const { return coverHeight * 2 / 3 + kShelfFrame.coverRight; }

int PulpTheme::shelfSideZone() const { return kShelfSideZone; }

// The selected book is circled in pencil, the loop drawn first so the book
// sits on top of it. A fresh loop each time a book gains the selection.
void PulpTheme::drawShelfBook(GfxRenderer& renderer, const Rect cell, const RecentBook& book, const int coverHeight,
                              const bool selected) const {
  // Plain boards here are half the size of Home's: about half the reader
  // font, the smallest built-in face (regular only).
  const int fontId = SMALL_FONT_ID;
  const int coverY = cell.y + kShelfTopPad;
  const int maxWidth = cell.width - 2 * kShelfSideZone;
  const int plainWidth = std::min(maxWidth, coverHeight * 2 / 3);
  const auto draw = [&] {
    return drawBook(renderer, kShelfFrame, cell.x, cell.width, coverY, coverHeight, maxWidth, plainWidth, book, fontId);
  };
  const int w = draw();
  if (!selected) return;
  static std::string markedPath;
  static uint32_t markSeed = 0x2545F491u;
  if (markedPath != book.path) {
    markedPath = book.path;
    markSeed = (static_cast<uint32_t>(millis()) * 2654435761u) | 1u;
  }
  // An ellipse a little wider and taller than the book: its corners still
  // reach over the line, but the sides stay clear.
  const int bookW = w + kShelfFrame.coverRight;
  const int bookH = coverHeight + kShelfFrame.coverBottom;
  const int loopW = bookW * 6 / 5;
  const int loopH = bookH + 2 * (kShelfTopPad - 6);
  const int centerX = cell.x + cell.width / 2;
  const int centerY = coverY + bookH / 2;
  const freeink::ui::Rect loop{static_cast<int16_t>(centerX - loopW / 2), static_cast<int16_t>(centerY - loopH / 2),
                               static_cast<int16_t>(loopW), static_cast<int16_t>(loopH)};
  freeink::ui::pencilLoop(
      loop, markSeed, [&renderer](const int px, const int py) { renderer.drawPixel(px, py); }, kShelfLoopThickness,
      true);
  // The book again, over the loop. Cover art only inks its dark pixels, so
  // clear the cover first or the loop shows through it.
  renderer.fillRect(cell.x + (cell.width - bookW) / 2, coverY, w, coverHeight, false);
  draw();
}

// Toasts set in the reader font, like the rest of the theme's text.
int PulpTheme::popupFontId() const { return SETTINGS.getReaderFontId(); }

// A white card centred on the screen, lifted off the page by a dithered
// shadow rather than a frame; the message is italic like the theme's notes.
Rect PulpTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  constexpr int kMargin = 8;  // keeps the page's text off the card
  constexpr int kShadow = 4;
  const auto& metrics = PulpMetrics::values;
  const int fontId = popupFontId();
  const int textWidth = renderer.getTextWidth(fontId, message, EpdFontFamily::ITALIC);
  const int w = textWidth + metrics.popupMarginX * 2;
  const int h = renderer.getLineHeight(fontId) + metrics.popupMarginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  const int y = (renderer.getScreenHeight() - h) / 2;
  renderer.fillRect(x - kMargin, y - kMargin, w + 2 * kMargin + kShadow, h + 2 * kMargin + kShadow, false);
  renderer.fillRectDither(x + kShadow, y + kShadow, w, h, Color::DarkGray);
  renderer.fillRect(x, y, w, h, false);
  renderer.drawText(fontId, x + metrics.popupMarginX, y + metrics.popupMarginY + metrics.popupTextBaselineOffsetY,
                    message, true, EpdFontFamily::ITALIC);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void PulpTheme::drawProgressRibbon(const GfxRenderer& renderer, const int left, const int right, const int top,
                                   const int height, const int notch, const bool paperState) const {
  // Over an image (the sleep cover) the ribbon brings its own paper.
  fillPaperRibbon(renderer, left, right, top, height, notch, true, paperState ? 1 : 0);
}

int PulpTheme::getMenuRowHeight(const GfxRenderer& renderer) const {
  return renderer.getLineHeight(SETTINGS.getReaderFontId()) + 2 * kMenuPaddingY;
}

// The cover tile fills the screen down to the menu, which sits just above
// the button hints. Both follow the reader font through the row height.
int PulpTheme::homeCoverTileHeight(const GfxRenderer& renderer) const {
  const auto& metrics = PulpMetrics::values;
  const int menuHeight = kHomeMenuRows * (getMenuRowHeight(renderer) + metrics.menuSpacing) - metrics.menuSpacing;
  return renderer.getScreenHeight() - metrics.homeTopPadding - metrics.homeMenuTopOffset - menuHeight -
         metrics.buttonHintsHeight - kHintClearance;
}

// The cover takes the tile minus its inset and a menu-row-high label band.
int PulpTheme::homeCoverThumbHeight(const GfxRenderer& renderer) const {
  return homeCoverTileHeight(renderer) - kCoverInset - kBookExtraY - getMenuRowHeight(renderer);
}

void PulpTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  const int fontId = SETTINGS.getReaderFontId();
  const int rowHeight = getMenuRowHeight(renderer);  // shared with HomeActivity's touch grid
  const int rowStep = rowHeight + PulpMetrics::values.menuSpacing;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int pageStartIndex = (std::max(0, selectedIndex) / pageItems) * pageItems;
  // Rows match the home cover card's width.
  const int rowWidth = std::min(coverWidth > 0 ? coverWidth + 2 * kCoverInset : kMenuMaxWidth, rect.width);
  const int rowX = rect.x + (rect.width - rowWidth) / 2;
  const int textOffsetY = (rowHeight - renderer.getCenteringHeight(fontId)) / 2;
  // A fresh mark each time a row gains the selection (including coming back
  // from Continue Reading, index -1); repaints while it stays selected keep it.
  static int markedIndex = -1;
  static uint32_t markSeed = 0x2545F491u;
  if (markedIndex != selectedIndex) markSeed = (static_cast<uint32_t>(millis()) * 2654435761u) | 1u;
  markedIndex = selectedIndex;

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const int rowY = rect.y + (i - pageStartIndex) * rowStep;
    const std::string label = buttonLabel(i);
    const std::string text =
        renderer.truncatedText(fontId, label.c_str(), rowWidth - 2 * PulpMetrics::values.listSidePadding);
    const int textWidth = renderer.getTextWidth(fontId, text.c_str());
    const int textX = rowX + (rowWidth - textWidth) / 2;
    if (i == selectedIndex) {
      drawHighlight(renderer, fontId, textX, rowY + (rowHeight - renderer.getLineHeight(fontId)) / 2, text);
      // Each destination has its own mark: a row of books for Books and a
      // wifi sketch for File Transfer (either side), the asterisk (left)
      // otherwise.
      if (rowIcon && rowIcon(i) == Library) {
        drawBookStack(renderer, fontId, textX, textX + textWidth, rowY + rowHeight / 2, markSeed);
      } else if (rowIcon && rowIcon(i) == Transfer) {
        drawWifiSketch(renderer, fontId, textX, textX + textWidth, rowY + rowHeight / 2, markSeed);
      } else {
        drawSelectionArrow(renderer, fontId, textX, rowY + rowHeight / 2, markSeed);
      }
    }
    renderer.drawText(fontId, textX, rowY + textOffsetY, text.c_str());
  }
}

void PulpTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (buttonHintsHidden()) {
    return;
  }

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int hintHeight = PulpMetrics::values.buttonHintsHeight;
  const int groupWidth = std::min(kHintGroupWidth, (pageWidth - kHintGap) / 2);
  const int leftX = (pageWidth - 2 * groupWidth - kHintGap) / 2;
  const int rightX = leftX + groupWidth + kHintGap;
  const int hintY = pageHeight - hintHeight;

  if (renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute()) {
    renderer.fillRect(leftX, hintY, groupWidth, hintHeight, true);
    renderer.fillRect(rightX, hintY, groupWidth, hintHeight, true);
    renderer.setOrientation(origOrientation);
    return;
  }

  const int textY = pageHeight - kHintBaselineInset - renderer.getFontAscenderSize(kGuideFontId);
  // Pairs are the physical buttons under each group: back/select, then up/down.
  const char* labels[4] = {btn1, btn2, btn3, btn4};
  for (int group = 0; group < 2; ++group) {
    const int x = group == 0 ? leftX : rightX;
    // Hints always win over anything drawn into this band.
    renderer.fillRect(x, hintY, groupWidth, hintHeight, false);
    // A tab open at the bottom: round the top corners and erase the bottom edge.
    renderer.drawRoundedRect(x, hintY, groupWidth, hintHeight, 1, kHintRadius, true, true, false, false, true);
    renderer.fillRect(x + 1, pageHeight - 1, groupWidth - 2, 1, false);

    const std::string leftText = hintLabel(labels[group * 2]);
    const std::string rightText = hintLabel(labels[group * 2 + 1]);
    if (!leftText.empty()) {
      renderer.drawText(kGuideFontId, x + kHintPaddingX, textY, leftText.c_str());
    }
    if (!rightText.empty()) {
      const int width = renderer.getTextWidth(kGuideFontId, rightText.c_str());
      renderer.drawText(kGuideFontId, x + groupWidth - kHintPaddingX - width, textY, rightText.c_str());
    }
  }

  renderer.setOrientation(origOrientation);
}

// One tab open at the right edge spanning the X4's two stacked side buttons,
// set like the bottom hints: Up reads from the top end, Down from the bottom.
// Edge-button boards keep the base layout.
void PulpTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (buttonHintsHidden()) return;
  if (gpio.hasEdgeSideButtons()) {
    BaseTheme::drawSideButtonHints(renderer, topBtn, bottomBtn);
    return;
  }
  // Physical button span, as in BaseTheme.
  constexpr int kTabTop = 345;
  constexpr int kTabHeight = 160;
  drawEdgeTab(renderer, kTabTop, kTabHeight, topBtn, bottomBtn);
}

void PulpTheme::drawPowerButtonHint(const GfxRenderer& renderer, const char* label) const {
  if (buttonHintsHidden() || gpio.hasEdgeSideButtons() || label == nullptr || label[0] == '\0') return;
  // X4 power button, near the top of the right edge.
  constexpr int kTabTop = 130;
  constexpr int kTextInset = kTabTextInset + 6;  // a little longer than the label needs, label centered
  const int height = renderer.getTextWidth(kGuideFontId, hintLabel(label).c_str()) + 2 * kTextInset;
  drawEdgeTab(renderer, kTabTop, height, label, nullptr, kTextInset);
}
