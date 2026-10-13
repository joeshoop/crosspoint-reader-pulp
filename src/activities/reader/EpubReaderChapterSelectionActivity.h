#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "activities/UiListActivity.h"

class EpubReaderChapterSelectionActivity final : public UiListActivity {
  std::shared_ptr<Epub> epub;
  int currentSpineIndex = 0;

  // Windowed row buffers: TOC entries are SD-backed (BookMetadataCache LUT
  // reads), so only the rows around the viewport are materialized. A
  // several-hundred-entry TOC (547 in a large collection) built up front cost
  // ~60KB of labels + ListItems — starving the CJK glyph arena into
  // SD-per-repaint — for rows that were never drawn. The window follows
  // nav.top via itemsWindowFirst (see fui::ListProps); refreshing it also
  // batch-prewarms the window's fallback glyphs, so each page of the list
  // pays one bounded SD pass and repaints stay RAM-only.
  static constexpr int TOC_WINDOW = 24;
  std::string windowLabels[TOC_WINDOW];
  freeink::ui::ListItem windowItems[TOC_WINDOW];
  int windowStart = -1;
  int windowCount = 0;
  void refreshTocWindow(int start);

  // Total TOC items count
  int listCount() const override { return epub ? epub->getTocItemsCount() : 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Back cancels with a result and Confirm activates on RELEASE here, and a
  // missing epub swallows everything past Back.
  bool handleButtons() override;
  // A rapid second press jumps between top-level entries.
  void navigateButtons() override;
  bool rowStartsSection(int row) const override;
  // Header is drawn inside the safe area (not full-width like the base).
  void drawChrome() override;
  void render(RenderLock&& lock) override;

  // Opened from the reader's menu sheet: the current chapter's row starts at
  // this screen y, where the sheet showed it, so only the rows around it
  // appear. -1 = normal layout. The shift is a blank band above the list,
  // dropped once the viewport moves.
  int anchorY = -1;
  int anchorShift = 0;
  int anchoredTop = -1;

  // Book-style lists title the contents with the book, wrapped like the menu
  // sheet's title; the header grows by this much for the extra lines.
  static constexpr int kBookTitleMaxLines = 4;
  int headerExtraHeight = 0;
  const char* headerTitle() const override;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, int currentSpineIndex,
                                              int anchorY = -1);
  void onEnter() override;
};
