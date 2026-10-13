#include "TextSettingsPreview.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "ReaderExcerpt.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace textsettings {

PreviewLayout::PreviewLayout() = default;
PreviewLayout::~PreviewLayout() = default;

namespace {

// Map the paragraph-alignment setting to the engine's CssTextAlign (BOOK_STYLE = justified)
CssTextAlign toCssAlign(uint8_t align) {
  if (align == CrossPointSettings::BOOK_STYLE) return CssTextAlign::Justify;
  return static_cast<CssTextAlign>(align);
}

// The current book's page when the reader has drawn one, else the stock sample.
const char* previewText() {
  const auto& excerpt = readerexcerpt::current();
  return excerpt.text.empty() ? I18N.get(StrId::STR_FONT_PREVIEW_TEXT) : excerpt.text.c_str();
}

// Lay one paragraph out through the reader engine, appending to layout.lines.
void layoutParagraph(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth, const char* text,
                     const char* end, uint8_t indentSpaces) {
  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;  // honor the user's choice; RTL auto-detected from text

  ParsedText parsed(SETTINGS.hyphenationEnabled != 0, SETTINGS.focusReadingEnabled != 0, style, indentSpaces);

  // Feed one space-separated word at a time; addWord handles NFC/CJK/RTL/focus splitting
  std::string word;
  for (const char* p = text;; p++) {
    if (p == end || *p == ' ') {
      if (!word.empty()) {
        parsed.addWord(word, EpdFontFamily::REGULAR);
        word.clear();
      }
      if (p == end) break;
    } else {
      word.push_back(*p);
    }
  }

  parsed.layoutAndExtractLines(
      renderer, fontId, static_cast<uint16_t>(textWidth),
      [&layout](std::unique_ptr<TextBlock> line, uint32_t) {
        layout.lines.push_back(std::move(line));
        layout.paragraphEnds.push_back(false);
      },
      true, SETTINGS.getCharacterSpacing(), SETTINGS.wordSpacing);
  if (!layout.paragraphEnds.empty()) layout.paragraphEnds.back() = true;
}

// Lay the preview text out through the reader engine into layout.lines
void relayout(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth) {
  layout.lines.clear();
  layout.paragraphEnds.clear();
  const auto& excerpt = readerexcerpt::current();
  layout.fromBook = !excerpt.text.empty();
  if (!layout.fromBook) {
    const char* text = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
    layoutParagraph(layout, renderer, fontId, textWidth, text, text + strlen(text), SETTINGS.paragraphIndentSpaces);
    return;
  }

  // One engine pass per book paragraph; a page that opens mid-paragraph has no indent.
  const char* text = excerpt.text.c_str();
  const char* textEnd = text + excerpt.text.size();
  bool firstParagraph = true;
  while (text < textEnd) {
    const char* end = static_cast<const char*>(memchr(text, '\n', textEnd - text));
    if (!end) end = textEnd;
    const bool indent = !(firstParagraph && excerpt.startsMidParagraph);
    layoutParagraph(layout, renderer, fontId, textWidth, text, end, indent ? SETTINGS.paragraphIndentSpaces : 0);
    firstParagraph = false;
    text = end + 1;
  }
}

PreviewKey keyFor(const int fontId, const int textWidth) {
  return PreviewKey{.fontId = fontId,
                    .fontPointSize = SETTINGS.fontPointSize,
                    .screenMargin = SETTINGS.screenMargin,
                    .textWidth = textWidth,
                    .lineCompression = SETTINGS.getReaderLineCompression(),
                    .alignment = SETTINGS.paragraphAlignment,
                    .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                    .paragraphIndentSpaces = SETTINGS.paragraphIndentSpaces,
                    .characterSpacing = SETTINGS.getCharacterSpacing(),
                    .wordSpacingPercent = SETTINGS.wordSpacing,
                    .focusReading = SETTINGS.focusReadingEnabled != 0,
                    .hyphenation = SETTINGS.hyphenationEnabled != 0,
                    .excerptGeneration = readerexcerpt::current().generation};
}

// Lays out (when the key changed) and draws the preview lines from y down to bottomLimit.
void drawLines(const GfxRenderer& renderer, PreviewLayout& layout, const int fontId, const int textLeft,
               const int textWidth, int y, const int bottomLimit) {
  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const float compression = SETTINGS.getReaderLineCompression();
  const int lineAdvance = std::max(1, renderer.getLineHeight(fontId, compression));
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? lineAdvance / 2 : 0;

  // Re-lay-out (and re-prewarm glyphs) only when a layout-affecting setting or the
  // geometry changed; else reuse the cache. The prewarm inputs are (fontId, constant
  // sample text, styleMask<-focusReading), all of which are key fields, so a matching
  // key means an identical prewarm call. This relies on nothing else evicting the SD
  // glyph cache while this activity is up — true today: the only evictor is
  // FontCacheManager::PrewarmScope, used solely by the reader/dictionary activities.
  const PreviewKey key = keyFor(fontId, textWidth);
  if (key != layout.key) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, previewText(), SETTINGS.focusReadingEnabled ? 0x03 : 0x01);
    }
    relayout(layout, renderer, fontId, textWidth);
    layout.key = key;
  }

  if (layout.fromBook) {
    for (size_t i = 0; i < layout.lines.size(); i++) {
      if (y + lineH > bottomLimit) return;
      layout.lines[i]->render(renderer, fontId, textLeft, y);
      y += lineAdvance + (layout.paragraphEnds[i] ? paragraphGap : 0);
    }
    return;
  }

  // Draw the sample twice so the paragraph gap is visible
  for (int paragraph = 0; paragraph < 2; paragraph++) {
    for (const auto& line : layout.lines) {
      if (y + lineH > bottomLimit) return;
      line->render(renderer, fontId, textLeft, y);
      y += lineAdvance;
    }
    y += paragraphGap;
  }
}

// The reader's text box: left edge, width and first-line top.
struct PageBox {
  int left;
  int width;
  int top;
};

PageBox pageBox(const GfxRenderer& renderer) {
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  return PageBox{left + SETTINGS.screenMargin,
                 renderer.getScreenWidth() - left - right - 2 * SETTINGS.screenMargin,
                 top + SETTINGS.screenMargin + UITheme::getStatusBarTopHeight(renderer)};
}

}  // namespace

void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, int top,
                   int height, const char* familyName, const char* sizeName) {
  const int left = previewPadding;
  const int width = renderer.getScreenWidth() - (previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  // No family/size names: no caption, and the sample text keeps the space.
  const bool showLabel = familyName && sizeName;
  const int labelH = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelReserved = showLabel ? labelH + labelGap + previewPadding : previewPadding;

  if (showLabel) {
    char labelBuf[128];
    snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName, sizeName);
    const int labelY = top + height - previewPadding - labelH;
    renderer.drawText(UI_10_FONT_ID, left, labelY, labelBuf);
  }

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;

  const int textLeft = left + SETTINGS.screenMargin;
  const int textWidth = width - 2 * SETTINGS.screenMargin;
  if (textWidth <= 0) return;

  drawLines(renderer, layout, fontId, textLeft, textWidth, top + previewPadding, top + height - labelReserved);
}

void renderPagePreview(const GfxRenderer& renderer, PreviewLayout& layout, const int bottom) {
  const int fontId = SETTINGS.getReaderFontId();
  const PageBox box = pageBox(renderer);
  if (fontId == 0 || box.width <= 0) return;
  drawLines(renderer, layout, fontId, box.left, box.width, box.top, bottom);
}

PreviewKey pagePreviewKey(const GfxRenderer& renderer) {
  return keyFor(SETTINGS.getReaderFontId(), pageBox(renderer).width);
}

}  // namespace textsettings
