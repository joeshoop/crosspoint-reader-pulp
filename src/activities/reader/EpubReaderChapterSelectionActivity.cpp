#include "EpubReaderChapterSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"

namespace fui = freeink::ui;

EpubReaderChapterSelectionActivity::EpubReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput,
                                                                       const std::shared_ptr<Epub>& epub,
                                                                       const int currentSpineIndex, const int anchorY)
    : UiListActivity("EpubReaderChapterSelection", renderer, mappedInput),
      epub(epub),
      currentSpineIndex(currentSpineIndex),
      anchorY(anchorY) {}

void EpubReaderChapterSelectionActivity::onEnter() {
  UiListActivity::onEnter();

  // The reader underneath pins its page-render glyph arenas while this
  // overlay is up. clearCache() is heap-adaptive: below the retention floor
  // it frees them (the next page render's PrewarmScope rebuilds them at
  // normal page-turn cost), giving this list room to keep every row's
  // fallback glyphs resident — otherwise each repaint re-reads the visible
  // rows' glyphs from SD.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }

  if (!epub) {
    return;
  }

  // Start with the current chapter at the top of the viewport; the first
  // screen build pulls the viewport to it (ListNav follow-on-build) and
  // materializes the row window there (refreshTocWindow in buildScreen).
  int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex == -1) {
    tocIndex = 0;
  }
  nav.selected = tocIndex;

  if (UITheme::getInstance().getMetrics().bookStyleLists) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int titleFontId = uiScaleSpec().titleFontId;
    const int lines = static_cast<int>(
        renderer
            .wrappedText(titleFontId, headerTitle(), safe.width - 2 * metrics.headerSidePadding, kBookTitleMaxLines,
                         metrics.headerTitleBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR)
            .size());
    headerExtraHeight = std::max(0, lines - 1) * renderer.getLineHeight(titleFontId);
  }
}

const char* EpubReaderChapterSelectionActivity::headerTitle() const {
  const bool bookTitle =
      UITheme::getInstance().getMetrics().bookStyleLists && epub && !epub->getTitle().empty();
  return bookTitle ? epub->getTitle().c_str() : tr(STR_SELECT_CHAPTER);
}

// Materialize the ListItem/label window starting at `start` (clamped). TOC
// entries are SD LUT reads (getTocItem), so this runs only when the viewport
// leaves the current window. Finishes with a batch prewarm of the window's
// CJK fallback glyphs -- one bounded SD pass per list page; repaints inside
// the window stay RAM-only.
void EpubReaderChapterSelectionActivity::refreshTocWindow(const int start) {
  const int total = listCount();
  int clamped = start;
  if (clamped > total - TOC_WINDOW) clamped = total - TOC_WINDOW;
  if (clamped < 0) clamped = 0;
  if (clamped == windowStart) return;

  windowCount = total - clamped < TOC_WINDOW ? total - clamped : TOC_WINDOW;
  const bool bookStyle = UITheme::getInstance().getMetrics().bookStyleLists;
  // Proportional body type: book-style lists need a wider step to read as nesting.
  const int indentPerLevel = bookStyle ? 4 : 2;
  for (int i = 0; i < windowCount; i++) {
    const auto tocItem = epub->getTocItem(clamped + i);
    std::string indent(tocItem.level > 0 ? (tocItem.level - 1) * indentPerLevel : 0, ' ');
    windowLabels[i] = indent + tocItem.title;
    fui::ListItem item;
    item.label = windowLabels[i].c_str();
    item.actionValue = static_cast<int16_t>(clamped + i);
    windowItems[i] = item;
  }
  windowStart = clamped;

  struct PrewarmCtx {
    const std::string* labels;
    int count;
  } prewarmCtx{windowLabels, windowCount};
  renderer.prewarmFallbackText(
      uiScaleSpec().bodyFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        const auto* c = static_cast<const PrewarmCtx*>(ctx);
        return i < static_cast<uint32_t>(c->count) ? c->labels[i].c_str() : nullptr;
      },
      &prewarmCtx, static_cast<uint32_t>(windowCount));
}

void EpubReaderChapterSelectionActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) {
    return;
  }
  // The activated row leaves this screen (finish); a lingering flash would gray
  // an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  const auto tocItem = epub->getTocItem(index);
  if (tocItem.spineIndex == -1) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  } else {
    setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
    finish();
  }
}

bool EpubReaderChapterSelectionActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (!epub) {
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderChapterSelectionActivity::navigateButtons() {
  const int count = listCount();
  if (count <= 0) return;
  buttonNavigator.onNextPress([this, count] { stepRowsWithSectionJumps(1, count); });
  buttonNavigator.onPreviousPress([this, count] { stepRowsWithSectionJumps(-1, count); });
  // Holding still pages through the list.
  buttonNavigator.onNextContinuous([this, count] {
    moveSelectionTo(ButtonNavigator::nextPageIndex(nav.selected, count, nav.inputPageRows()));
  });
  buttonNavigator.onPreviousContinuous([this, count] {
    moveSelectionTo(ButtonNavigator::previousPageIndex(nav.selected, count, nav.inputPageRows()));
  });
}

// One SD read per row checked; a jump walks only to the next top-level entry.
bool EpubReaderChapterSelectionActivity::rowStartsSection(const int row) const {
  return row == 0 || epub->getTocItem(row).level <= 1;
}

void EpubReaderChapterSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome paints the title in.
  // Book-style lists keep their rows clear of the button hints.
  const int hintsReserve = metrics.bookStyleLists ? metrics.buttonHintsHeight : 0;
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + headerExtraHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + hintsReserve),
                  static_cast<int16_t>(safe.x)});
  if (!metrics.bookStyleLists) screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (!epub) {
    return;
  }
  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }

  if (metrics.bookStyleLists) {
    // The list's heading, pinned under the title while the rows scroll.
    fui::ListProps headingProps;
    applySectionHeadingStyle(screen.theme(), headingProps);
    const fui::Rect row = screen.takeTop(static_cast<int16_t>(metrics.listRowHeight));
    const int16_t lh = screen.target().lineHeight(headingProps.headerText.font);
    screen.target().text(fui::Rect{row.x, static_cast<int16_t>(row.y + (row.height - lh) / 2), row.width, lh},
                         tr(STR_TABLE_OF_CONTENTS), headingProps.headerText);
  }

  if (anchoredTop >= 0 && nav.top != anchoredTop) anchorShift = 0;  // scrolled: use the full height
  if (anchorShift > 0) screen.spacer(static_cast<int16_t>(anchorShift));

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  applySectionHeadingStyle(screen.theme(), props);
  syncListViewport(screen, props);
  // Materialize the row window for the final viewport (syncListViewport just
  // applied follow/clamping to nav.top) and hand list() the window with its
  // absolute base index.
  refreshTocWindow(nav.top);
  props.items = windowItems;
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  screen.list(props);
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  const auto build = [this] {
    renderer.clearScreen();
    drawChrome();
    renderUi();
    for (int pass = 0; nav.consumeRebuildNeeded() && pass < 8; ++pass) {
      renderer.clearScreen();
      drawChrome();
      renderUi();
    }
  };
  build();

  // First frame from the sheet: move the viewport and the blank band until the
  // selected row sits at anchorY. Rows above it come from scrolling back;
  // what is left (or all of it near the start) from the band. Near the end
  // the list cannot scroll far enough and the row lands lower.
  if (anchorY >= 0 && anchoredTop < 0) {
    const int rowH = std::max(1, UITheme::getInstance().getMetrics().listRowHeight);
    for (int pass = 0; pass < 4 && nav.selectedRowY >= 0; ++pass) {
      const int delta = anchorY - nav.selectedRowY;
      if (delta == 0) break;
      if (delta > 0) {
        const int rows = std::min(nav.top, delta / rowH);
        if (rows > 0) {
          nav.top -= rows;
        } else {
          anchorShift += delta;
        }
      } else if (anchorShift >= -delta) {
        anchorShift += delta;
      } else {
        nav.top += (-delta - anchorShift + rowH - 1) / rowH;
        anchorShift = 0;
      }
      nav.followOnBuild = false;
      build();
    }
    anchoredTop = nav.top;
  }

  drawFooter();
  present();
}

void EpubReaderChapterSelectionActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer,
                 Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight + headerExtraHeight},
                 headerTitle(), nullptr, true, metrics.bookStyleLists ? kBookTitleMaxLines : 1);
}
