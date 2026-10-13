#include "ClipSelectionActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "clippings/ClippingText.h"
#include "clippings/SelectionGeometry.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "util/WordSelectionInput.h"

using Input = WordSelectionInput;

namespace {

constexpr size_t FONT_PREWARM_TEXT_MAX = 2048;
constexpr int TOUCH_DRAG_MOVEMENT_PX = 4;
constexpr unsigned long TOUCH_PAGE_ADVANCE_HOLD_MS = 1000;
constexpr int TOUCH_PAGE_END_DWELL_SLOP_PX = 8;
constexpr ClippingResult::Action SELECTION_ACTIONS[] = {ClippingResult::Action::Lookup, ClippingResult::Action::Clip,
                                                        ClippingResult::Action::Bookmark,
                                                        ClippingResult::Action::Remove};

const char* cleanWordStart(const char* text) {
  if (!text) return "";
  if (clippingText::hasEmSpacePrefix(text)) text += 3;
  while (*text != '\0' && (*text == ' ' || *text == '\r' || *text == '\n' || *text == '\t' ||
                           (static_cast<uint8_t>(text[0]) == 0xC2 && static_cast<uint8_t>(text[1]) == 0xA0))) {
    text += static_cast<uint8_t>(text[0]) == 0xC2 ? 2 : 1;
  }
  return text;
}

}  // namespace

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<std::unique_ptr<Page>> pages, const int marginLeft,
                                             const int marginTop, const int initialX, const int initialY,
                                             const Mode mode, SavedClippings saved)
    : Activity("ClipSelection", renderer, mappedInput),
      pages(std::move(pages)),
      marginLeft(marginLeft),
      marginTop(marginTop),
      initialX(initialX),
      initialY(initialY),
      mode(mode),
      saved(std::move(saved)) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  if (!extractWords() || wordCount == 0) {
    if (wordCount == 0) LOG_ERR("CLIP", "No selectable words on current page");
    cancel();
    return;
  }
  markSavedClippings();
  uint16_t firstPageRows = 0;
  for (size_t i = 0; i < wordCount; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != 0) break;
    firstPageRows = std::max<uint16_t>(firstPageRows, static_cast<uint16_t>(word.row + 1));
  }
  const int middle = closestInRow(firstPageRows / 2, renderer.getScreenWidth() / 2);
  if (middle >= 0) selected = middle;
  if (initialX >= 0) {
    const int hit = wordAt(initialX, initialY);
    if (hit < 0) {
      cancel();
      return;
    }
    if (!beginEdit(words[hit].clipping)) selected = rangeStart = hit;
    ignoreInitialTouch = true;
  }
  requestUpdate();
}

bool ClipSelectionActivity::extractWords() {
  wordCount = 0;
  words = makeUniqueNoThrow<WordBox[]>(MAX_SELECTABLE_WORDS);
  if (!words) {
    LOG_ERR("CLIP", "OOM: selection words (%u bytes)", static_cast<unsigned>(MAX_SELECTABLE_WORDS * sizeof(WordBox)));
    return false;
  }
  rowCount = 0;
  const bool needsFontPrewarm = renderer.isSdCardFont(fontId);
  auto pageText = needsFontPrewarm ? makeUniqueNoThrow<char[]>(FONT_PREWARM_TEXT_MAX) : nullptr;
  size_t pageTextLength = 0;
  if (needsFontPrewarm && !pageText) LOG_DBG("CLIP", "Skipping SD font prewarm: OOM");
  uint8_t styleMask = 0;
  bool paragraphStartPending = false;

  for (size_t pageOffset = 0; pageOffset < pages.size(); ++pageOffset) {
    uint16_t pageWordIndex = 0;
    for (const auto& element : pages[pageOffset]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = line.getBlock();
      if (!block || !block->valid()) continue;

      const size_t lineStart = wordCount;
      const bool isRtl = block->getBlockStyle().isRtl;
      const int8_t characterSpacing = block->getBlockStyle().characterSpacing;
      const size_t remaining = MAX_SELECTABLE_WORDS - lineStart;
      size_t rtlWordCount = 0;
      const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(fontId));
      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        if (block->wordStartsParagraph(i)) paragraphStartPending = true;
        const char* text = block->wordText(i);
        if (!clippingText::hasVisibleText(text)) continue;

        const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
        int width = renderer.getTextAdvanceX(fontId, text, style, characterSpacing);
        if (width <= 0) continue;
        if (i + 1 < block->wordCount() && block->wordXpos(i + 1) > block->wordXpos(i)) {
          width = std::min(width, static_cast<int>(block->wordXpos(i + 1) - block->wordXpos(i)));
        }

        if (!isRtl && wordCount == MAX_SELECTABLE_WORDS) break;

        WordBox& word = isRtl ? words[lineStart + (rtlWordCount < remaining ? rtlWordCount : rtlWordCount % remaining)]
                              : words[wordCount++];
        word.x = static_cast<int16_t>(marginLeft + line.xPos + block->wordXpos(i));
        word.y = static_cast<int16_t>(marginTop + line.yPos + rubyShift);
        word.width = static_cast<int16_t>(width);
        word.height = static_cast<int16_t>(lineHeight);
        word.row = rowCount;
        word.pageOffset = static_cast<uint8_t>(pageOffset);
        word.pageWordIndex = pageWordIndex++;
        word.startOffset = block->wordSourceRange(i).start;
        word.endOffset = block->wordSourceRange(i).end;
        word.text = text;
        word.style = style;
        word.characterSpacing = characterSpacing;
        word.paragraphStart = false;
        word.isRtl = isRtl;
        word.discretionaryHyphen = block->wordHasDiscretionaryHyphen(i);
        if (pageText) {
          for (const char* p = text; *p != '\0' && pageTextLength + 1 < FONT_PREWARM_TEXT_MAX; ++p) {
            pageText[pageTextLength++] = *p;
          }
          if (pageTextLength + 1 < FONT_PREWARM_TEXT_MAX) pageText[pageTextLength++] = ' ';
        }
        styleMask |= static_cast<uint8_t>(1U << (static_cast<uint8_t>(style) & 0x03));
        if (isRtl) ++rtlWordCount;
      }
      if (isRtl) {
        const size_t stored = std::min(remaining, rtlWordCount);
        wordCount = lineStart + stored;
        if (rtlWordCount > remaining) {
          std::rotate(words.get() + lineStart, words.get() + lineStart + rtlWordCount % remaining,
                      words.get() + wordCount);
        }
        std::reverse(words.get() + lineStart, words.get() + wordCount);
      }
      if (wordCount > lineStart && paragraphStartPending) {
        // Whitespace-only tokens are skipped; keep their boundary on the first selectable logical word.
        const auto first =
            std::min_element(words.get() + lineStart, words.get() + wordCount,
                             [](const WordBox& a, const WordBox& b) { return a.startOffset < b.startOffset; });
        first->paragraphStart = true;
        paragraphStartPending = false;
      }
      if (wordCount != lineStart) ++rowCount;
      if (wordCount == MAX_SELECTABLE_WORDS) {
        LOG_ERR("CLIP", "Selectable word cap hit (%u); multi-page selection was truncated",
                static_cast<unsigned>(MAX_SELECTABLE_WORDS));
        break;
      }
    }
    if (wordCount == MAX_SELECTABLE_WORDS) break;
  }

  if (styleMask == 0) styleMask = 0x01;
  if (pageText) {
    pageText[pageTextLength] = '\0';
    renderer.ensureSdCardFontReady(fontId, pageText.get(), styleMask);
  }

  return true;
}

int ClipSelectionActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

int ClipSelectionActivity::wordAt(const int x, int y) const {
  y -= textOffset();
  constexpr int SLOP = 4;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + word.height + SLOP) {
      return i;
    }
  }
  return -1;
}

int ClipSelectionActivity::dragWordAt(const int x, int y) const {
  y -= textOffset();
  int best = -1;
  Rect nearest{};
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    const Rect candidate{word.x, word.y, word.width, word.height};
    if (best < 0 || selectionGeometry::nearerWord(candidate, nearest, x, y)) {
      nearest = candidate;
      best = i;
    }
  }
  return best;
}

bool ClipSelectionActivity::selectionContains(const int x, const int y) const {
  const int offset = textOffset();
  const WordBox* previous = nullptr;
  for (int i = std::min(rangeStart, selected); i <= std::max(rangeStart, selected); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    int left = word.x;
    int right = word.x + word.width;
    if (previous && previous->row == word.row) {
      left = std::min(left, static_cast<int>(previous->x));
      right = std::max(right, previous->x + previous->width);
    }
    if (selectionGeometry::contains(Rect{left, word.y + offset, right - left, word.height}, x, y)) return true;
    previous = &word;
  }
  return false;
}

int ClipSelectionActivity::nextPageStartIndexForTouchDrag() const {
  if (!touchDragHasMoved || rangeStart < 0 || selected < rangeStart) return -1;

  const uint8_t currentPage = words[selected].pageOffset;
  for (int i = selected + 1; i < static_cast<int>(wordCount); ++i) {
    if (words[i].pageOffset > currentPage) return i;
  }
  return -1;
}

bool ClipSelectionActivity::isWithinCurrentPageEndDwellSlop(const int x, const int y) const {
  if (selected < 0 || selected >= static_cast<int>(wordCount)) return false;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  if (selectionGeometry::atBottomEdge(safe, lineHeight, x, y)) return true;
  if (selected + 1 < static_cast<int>(wordCount) && words[selected + 1].pageOffset == currentPageOffset) return false;
  const WordBox& word = words[selected];
  const int wordY = word.y + textOffset();
  return word.pageOffset == currentPageOffset && x >= word.x - TOUCH_PAGE_END_DWELL_SLOP_PX &&
         x < word.x + word.width + TOUCH_PAGE_END_DWELL_SLOP_PX && y >= wordY - TOUCH_PAGE_END_DWELL_SLOP_PX &&
         y < wordY + word.height + TOUCH_PAGE_END_DWELL_SLOP_PX;
}

void ClipSelectionActivity::moveVertical(const int direction) {
  const int targetRow = static_cast<int>(words[selected].row) + direction;
  if (targetRow < 0 || targetRow >= rowCount) return;
  const int next = closestInRow(static_cast<uint16_t>(targetRow), words[selected].x + words[selected].width / 2);
  if (next >= 0 && next != selected) {
    selectIndex(next);
  }
}

void ClipSelectionActivity::selectIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(wordCount) || index == selected) return;
  selected = index;
  currentPageOffset = words[selected].pageOffset;
  requestUpdate();
}

void ClipSelectionActivity::moveToPage(const int pageOffset) {
  if (pageOffset < 0 || pageOffset >= static_cast<int>(pages.size()) || pageOffset == currentPageOffset) return;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].pageOffset == pageOffset) {
      selectIndex(i);
      return;
    }
  }
}

bool ClipSelectionActivity::buildSelectedText(const int first, const int last, std::string& text) const {
  text.clear();
  text.reserve(CLIPPING_TEXT_MAX);
  const size_t count = last - first + 1;
  auto order = makeUniqueNoThrow<uint16_t[]>(count);
  if (!order) {
    LOG_ERR("CLIP", "OOM: selection export order");
    return false;
  }
  bool allSourcesKnown = true;
  for (size_t i = 0; i < count; ++i) {
    order[i] = static_cast<uint16_t>(first + i);
    allSourcesKnown &= words[order[i]].startOffset != UINT32_MAX && words[order[i]].endOffset != UINT32_MAX;
  }
  // Keep navigation in visual order and export in logical source order.
  if (allSourcesKnown) {
    std::sort(order.get(), order.get() + count, [this](const uint16_t a, const uint16_t b) {
      if (words[a].startOffset == words[b].startOffset) return a < b;
      return words[a].startOffset < words[b].startOffset;
    });
  }
  for (size_t i = 0; i < count; ++i) {
    const WordBox& current = words[order[i]];
    const char* word = cleanWordStart(current.text);
    char separator = '\0';
    if (i > 0) {
      const WordBox& previous = words[order[i - 1]];
      const bool sourceKnown = previous.endOffset != UINT32_MAX && current.startOffset != UINT32_MAX;
      const bool separated = sourceKnown ? current.startOffset > previous.endOffset
                                         : current.row != previous.row || current.x > previous.x + previous.width + 2;
      if (current.paragraphStart) {
        separator = '\n';
      } else if (separated) {
        separator = ' ';
      }
    }
    if (!clippingText::append(text, word, separator, CLIPPING_TEXT_MAX, current.discretionaryHyphen)) {
      return false;
    }
  }
  return true;
}

void ClipSelectionActivity::confirmSelection(const ClippingResult::Action action) {
  if (rangeStart < 0) {
    if (!beginEdit(words[selected].clipping)) rangeStart = selected;
    requestUpdate();
    return;
  }

  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  ClippingResult result;
  result.action = action;
  if (editing >= 0 && (action == ClippingResult::Action::Clip || action == ClippingResult::Action::Remove)) {
    if (action == ClippingResult::Action::Clip && first == editFirst && last == editLast) {
      cancel();  // unchanged
      return;
    }
    result.editedClipping = editing;
    if (action == ClippingResult::Action::Remove) {
      setResult(std::move(result));
      finish();
      return;
    }
  }
  const auto heap = HalMemory::getDefaultHeap();
  // reserve() aborts on OOM; leave room for the sort order and error popup too.
  const size_t requiredHeap = CLIPPING_TEXT_MAX + 1 + (last - first + 1) * sizeof(uint16_t) + 1024;
  const bool lowMemory = heap.largestBlockBytes < CLIPPING_TEXT_MAX + 1 || heap.freeBytes < requiredHeap;
  if (lowMemory || !buildSelectedText(first, last, result.text)) {
    static constexpr StrId OPTIONS[] = {StrId::STR_BACK};
    actionPopup.show(lowMemory ? StrId::STR_MEMORY_ERROR : StrId::STR_CLIPPING_TOO_LONG, OPTIONS, 1, 0, [](int) {});
    requestUpdate();
    return;
  }
  result.startPageOffset = words[first].pageOffset;
  result.endPageOffset = words[last].pageOffset;
  result.startWordIndex = words[first].pageWordIndex;
  result.endWordIndex = words[last].pageWordIndex;
  result.wordCount = static_cast<uint16_t>(last - first + 1);
  result.startOffset = UINT32_MAX;
  result.endOffset = 0;
  for (int i = first; i <= last; ++i) {
    if (words[i].startOffset == UINT32_MAX || words[i].endOffset == UINT32_MAX) {
      result.startOffset = result.endOffset = UINT32_MAX;
      break;
    }
    result.startOffset = std::min(result.startOffset, words[i].startOffset);
    result.endOffset = std::max(result.endOffset, words[i].endOffset);
  }
  setResult(std::move(result));
  finish();
}

void ClipSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool ClipSelectionActivity::handleHomeGesture() {
  cancel();
  return true;
}

void ClipSelectionActivity::loopButtons() {
  const uint8_t buttons = selectionInput.pollButtons(mappedInput, millis());
  if (buttons) {
    if (pendingButtonCount < pendingButtons.size()) {
      pendingButtons[pendingButtonCount++] = buttons;
    } else {
      LOG_ERR("CLIP", "Selection button queue full");
    }
  }

  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  size_t processed = 0;
  while (processed < pendingButtonCount) {
    if (handleButtons(pendingButtons[processed++])) break;
  }
  std::move(pendingButtons.begin() + processed, pendingButtons.begin() + pendingButtonCount, pendingButtons.begin());
  pendingButtonCount -= processed;
}

void ClipSelectionActivity::loop() {
  if (wordCount == 0) return;
  // The side-button action that opened this screen closes it again.
  const HomeButtonAction home = mappedInput.homeButtonAction();
  if ((mode == Mode::Clip && home == HomeButtonAction::CreateClipping) ||
      (mode == Mode::Lookup && home == HomeButtonAction::Dictionary)) {
    cancel();
    return;
  }
  if (!mappedInput.hasTouch()) {
    loopButtons();
    return;
  }

  RenderLock lock;
  if (actionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  int touchX = 0;
  int touchY = 0;
  if (ignoreInitialTouch) {
    if (!mappedInput.isScreenTouchHeld(touchX, touchY)) ignoreInitialTouch = false;
    return;
  }
  if (!touchDragSelecting && mappedInput.wasScreenTapped(touchX, touchY)) {
    if (rangeStart >= 0) {
      const int action = selectionGeometry::actionAt(actionRect(), UITheme::getInstance().getMetrics().menuSpacing,
                                                     touchX, touchY, actionCount());
      if (action >= 0) {
        confirmSelection(SELECTION_ACTIONS[action]);
        return;
      }
      const int first = std::min(rangeStart, selected);
      const int last = std::max(rangeStart, selected);
      const bool onStart = words[first].pageOffset == currentPageOffset &&
                           selectionGeometry::contains(handleRect(first, true), touchX, touchY);
      const bool onEnd = words[last].pageOffset == currentPageOffset &&
                         selectionGeometry::contains(handleRect(last, false), touchX, touchY);
      if (!onStart && !onEnd && !selectionContains(touchX, touchY)) cancel();
    } else {
      const int hit = wordAt(touchX, touchY);
      if (hit < 0) {
        cancel();
      } else {
        if (!beginEdit(words[hit].clipping)) selected = rangeStart = hit;
        requestUpdate();
      }
    }
    return;
  }
  if (touchDragSelecting) {
    if (mappedInput.isScreenTouchHeld(touchX, touchY)) {
      const int deltaX = touchX - touchDragStartX;
      const int deltaY = touchY - touchDragStartY;
      touchDragHasMoved = touchDragHasMoved || deltaX >= TOUCH_DRAG_MOVEMENT_PX || deltaX <= -TOUCH_DRAG_MOVEMENT_PX ||
                          deltaY >= TOUCH_DRAG_MOVEMENT_PX || deltaY <= -TOUCH_DRAG_MOVEMENT_PX;

      const int hit = dragWordAt(touchX + dragOffsetX, touchY + dragOffsetY);
      if (hit >= 0) {
        selectIndex(hit);
      }

      // Hold at the bottom edge or on the final word to extend onto the next page.
      const int nextPageStart = nextPageStartIndexForTouchDrag();
      if (nextPageStart >= 0 && (isWithinCurrentPageEndDwellSlop(touchX, touchY) ||
                                 isWithinCurrentPageEndDwellSlop(touchX + dragOffsetX, touchY + dragOffsetY))) {
        const unsigned long now = millis();
        if (touchDragPageEndIndex != nextPageStart) {
          touchDragPageEndIndex = nextPageStart;
          touchDragPageEndHeldSince = now;
        } else if (now - touchDragPageEndHeldSince >= TOUCH_PAGE_ADVANCE_HOLD_MS) {
          touchDragPageEndIndex = -1;
          selectIndex(nextPageStart);
          dragOffsetX = dragOffsetY = 0;
        }
      } else {
        touchDragPageEndIndex = -1;
      }
      return;
    }
    touchDragSelecting = false;
    touchDragHasMoved = false;
    touchDragPageEndIndex = -1;
    requestUpdate();
    return;
  } else if (mappedInput.wasScreenTouchPressed(touchX, touchY)) {
    if (rangeStart >= 0) {
      const Rect actions = actionRect();
      if (selectionGeometry::actionAt(actions, UITheme::getInstance().getMetrics().menuSpacing, touchX, touchY,
                                      actionCount()) >= 0)
        return;
      const int first = std::min(rangeStart, selected);
      const int last = std::max(rangeStart, selected);
      for (int endpoint = 0; endpoint < 2; ++endpoint) {
        const int index = endpoint == 0 ? first : last;
        if (words[index].pageOffset != currentPageOffset) continue;
        const Rect handle = handleRect(index, endpoint == 0);
        if (touchX < handle.x || touchX >= handle.x + handle.width || touchY < handle.y ||
            touchY >= handle.y + handle.height)
          continue;
        selected = index;
        rangeStart = endpoint == 0 ? last : first;
        dragOffsetX = words[index].x + words[index].width / 2 - touchX;
        dragOffsetY = words[index].y + textOffset() + words[index].height / 2 - touchY;
        touchDragSelecting = true;
        touchDragHasMoved = false;
        touchDragStartX = touchX;
        touchDragStartY = touchY;
        touchDragPageEndIndex = -1;
        return;
      }
      return;
    }
    const int hit = wordAt(touchX, touchY);
    if (hit >= 0) {
      selected = rangeStart = hit;
      dragOffsetX = 0;
      dragOffsetY = 0;
      touchDragSelecting = true;
      touchDragHasMoved = false;
      touchDragStartX = touchX;
      touchDragStartY = touchY;
      touchDragPageEndIndex = -1;
      requestUpdate();
    }
    return;
  }

  const uint8_t buttons = Input::buttonEdges(mappedInput);
  if (buttons & (Input::INPUT_BACK | Input::INPUT_CONFIRM)) {
    handleButtons(buttons);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
    moveToPage(static_cast<int>(currentPageOffset) + 1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
    moveToPage(static_cast<int>(currentPageOffset) - 1);
    return;
  }

  handleButtons(buttons);
}

bool ClipSelectionActivity::handleButtons(const uint8_t buttons) {
  if (actionPopup.handleButtons(buttons & Input::INPUT_PREVIOUS, buttons & Input::INPUT_NEXT,
                                buttons & Input::INPUT_CONFIRM, buttons & Input::INPUT_BACK,
                                [this] { requestUpdate(); }))
    return true;

  if (buttons & Input::INPUT_BACK) {
    if (editing >= 0) {
      endEdit();
    } else if (rangeStart >= 0) {
      rangeStart = -1;
      requestUpdate();
    } else {
      cancel();
    }
    return true;
  }

  if (buttons & Input::INPUT_CONFIRM) {
    if (editing >= 0 && !mappedInput.hasTouch()) {
      static constexpr StrId OPTIONS[] = {StrId::STR_DONE, StrId::STR_DELETE};
      actionPopup.show(StrId::STR_EDIT_CLIPPING, OPTIONS, 2, 0, [this](const int option) {
        confirmSelection(option == 0 ? ClippingResult::Action::Clip : ClippingResult::Action::Remove);
      });
      requestUpdate();
      return true;
    }
    confirmSelection(mode == Mode::Lookup ? ClippingResult::Action::Lookup : ClippingResult::Action::Clip);
    return true;
  }

  const int next =
      selectionGeometry::horizontalIndex(selected, static_cast<int>(wordCount), buttons & Input::INPUT_LEFT,
                                         buttons & Input::INPUT_RIGHT, words[selected].isRtl);
  if (next != selected) {
    selectIndex(next);
  } else if (buttons & Input::INPUT_UP) {
    moveVertical(-1);
  } else if (buttons & Input::INPUT_DOWN) {
    moveVertical(1);
  }
  return false;
}

Rect ClipSelectionActivity::handleRect(const int index, const bool start) const {
  const WordBox& word = words[index];
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int size = std::max(24, UITheme::getInstance().getMetrics().verticalSpacing * 2);
  const bool left = start != word.isRtl;
  const int edge = left ? word.x : word.x + word.width;
  return Rect{std::clamp(edge - (left ? size : 0), safe.x, safe.x + safe.width - size),
              std::clamp(word.y + textOffset() + word.height, safe.y, safe.y + safe.height - size), size, size};
}

int ClipSelectionActivity::selectionTop() const {
  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  int top = renderer.getScreenHeight();
  for (int i = first; i <= last; ++i) {
    if (words[i].pageOffset == currentPageOffset) top = std::min(top, static_cast<int>(words[i].y));
  }
  return top;
}

Rect ClipSelectionActivity::actionRect() const {
  Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int rightEdge = std::min(safe.x + safe.width, renderer.getScreenWidth() - right);
  const int bottomEdge = std::min(safe.y + safe.height, renderer.getScreenHeight() - bottom);
  safe.x = std::max(safe.x, left);
  safe.y = std::max(safe.y, top) + std::max(metrics.topPadding, metrics.verticalSpacing);
  safe.width = rightEdge - safe.x;
  safe.height = bottomEdge - safe.y;
  const int font = uiScaleSpec().smallFontId;
  int labelWidth =
      std::max({renderer.getTextWidth(font, tr(STR_LOOKUP)), renderer.getTextWidth(font, tr(STR_CLIP)),
                renderer.getTextWidth(font, tr(STR_BOOKMARK_OPTION))});
  if (editing >= 0) {
    labelWidth = std::max({labelWidth, renderer.getTextWidth(font, tr(STR_DONE)),
                           renderer.getTextWidth(font, tr(STR_DELETE))});
  }
  const int count = actionCount();
  const int padding = metrics.menuSpacing;
  const int width = std::min(safe.width, count * (labelWidth + padding * 2) + padding * (count + 1));
  const int lines = labelWidth > (width - padding * (count + 1)) / count ? 2 : 1;
  const int height = std::max(36, renderer.getLineHeight(font) * lines + padding * 2);
  int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  while (first < last && words[first].pageOffset < currentPageOffset) ++first;
  return selectionGeometry::actions(safe, selectionTop(), height + padding * 2, metrics.verticalSpacing, width,
                                    words[first].x - padding);
}

int ClipSelectionActivity::textOffset() const {
  if (!mappedInput.hasTouch() && wordCount) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const WordBox& cursor = words[selected];
    return selectionGeometry::keepVisible(cursor.y, cursor.height, safe.y, safe.height);
  }
  return 0;
}

int ClipSelectionActivity::textXOffset() const {
  if (mappedInput.hasTouch() || !wordCount) return 0;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const WordBox& cursor = words[selected];
  return selectionGeometry::keepVisible(cursor.x, cursor.width, safe.x, safe.width);
}

void ClipSelectionActivity::prewarmWord(const int index) const {
  if (index >= 0 && index < static_cast<int>(wordCount) && words[index].text) {
    renderer.getFontCacheManager()->prewarmCache(
        fontId, words[index].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[index].style) & 0x03)));
  }
}

void ClipSelectionActivity::ditherGapBetween(const WordBox& a, const WordBox& b, const int offsetX,
                                             const int offset) const {
  if (a.row != b.row || a.pageOffset != b.pageOffset) return;
  const int leftRight = std::min(a.x + a.width, b.x + b.width);
  const int rightLeft = std::max(a.x, b.x);
  if (leftRight < rightLeft) {
    fillSelected(leftRight + offsetX, a.y + offset, rightLeft - leftRight, a.height);
  }
}

void ClipSelectionActivity::clearGapBetween(const WordBox& a, const WordBox& b, const int offsetX,
                                            const int offset) const {
  if (a.row != b.row || a.pageOffset != b.pageOffset) return;
  const int leftRight = std::min(a.x + a.width, b.x + b.width);
  const int rightLeft = std::max(a.x, b.x);
  if (leftRight < rightLeft) {
    renderer.fillRect(leftRight + offsetX, a.y + offset, rightLeft - leftRight, a.height, false);
  }
}

void ClipSelectionActivity::drawWordClean(const int index, const int offsetX, const int offset) const {
  if (index < 0 || index >= static_cast<int>(wordCount)) return;
  const WordBox& word = words[index];
  if (word.pageOffset != currentPageOffset) return;

  prewarmWord(index);

  renderer.fillRect(word.x + offsetX, word.y + offset, word.width, word.height, false);
  const Rect frame = cursorRect(index, offsetX, offset);
  renderer.fillRect(frame.x, frame.y, frame.width, frame.height, false);

  if (index > 0) {
    clearGapBetween(words[index - 1], word, offsetX, offset);
  }
  if (index + 1 < static_cast<int>(wordCount)) {
    clearGapBetween(word, words[index + 1], offsetX, offset);
  }

  drawSavedBand(index, offsetX, offset);
  renderer.drawText(fontId, word.x + offsetX, word.y + offset, word.text, true, word.style,
                    BidiUtils::BidiBaseDir::AUTO, word.characterSpacing);
}

void ClipSelectionActivity::drawWordHighlight(const int index, const int firstSelected, const int lastSelected,
                                              const int offsetX, const int offset) const {
  if (index < 0 || index >= static_cast<int>(wordCount)) return;
  const WordBox& word = words[index];
  if (word.pageOffset != currentPageOffset) return;

  prewarmWord(index);

  // Clear a cursor frame this word may still carry.
  const Rect frame = cursorRect(index, offsetX, offset);
  renderer.fillRect(frame.x, frame.y, frame.width, frame.height, false);
  if (index > firstSelected) {
    ditherGapBetween(words[index - 1], word, offsetX, offset);
  }
  if (index < lastSelected) {
    ditherGapBetween(word, words[index + 1], offsetX, offset);
  }

  fillSelected(word.x + offsetX, word.y + offset, word.width, word.height);
  renderer.drawText(fontId, word.x + offsetX, word.y + offset, word.text, true, word.style,
                    BidiUtils::BidiBaseDir::AUTO, word.characterSpacing);
}

bool ClipSelectionActivity::renderIncremental() {
  if (actionPopup.isActive() || mappedInput.hasTouch()) return false;
  if (lastRenderedPageOffset < 0 || lastRenderedPageOffset != currentPageOffset) return false;
  const int offset = textOffset();
  const int offsetX = textXOffset();
  if (offset != lastRenderedTextOffset || offsetX != lastRenderedTextXOffset) return false;
  if (lastRenderedSelected < 0 || lastRenderedSelected >= static_cast<int>(wordCount)) return false;
  if (selected < 0 || selected >= static_cast<int>(wordCount)) return false;

  // Case 1: Moving cursor before range selection starts (rangeStart < 0)
  if (rangeStart < 0 && lastRenderedRangeStart < 0) {
    if (selected == lastRenderedSelected) return true;

    drawWordClean(lastRenderedSelected, offsetX, offset);
    drawWordHighlight(selected, selected, selected, offsetX, offset);
    drawCursor(selected, offsetX, offset);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    drawHints(labels);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    lastRenderedSelected = selected;
    return true;
  }

  // Case 2: Transition from rangeStart < 0 to rangeStart >= 0 (user just confirmed rangeStart)
  if (rangeStart >= 0 && lastRenderedRangeStart < 0 && rangeStart == selected && selected == lastRenderedSelected) {
    // The cursor's frame changes once anchored.
    drawWordHighlight(selected, selected, selected, offsetX, offset);
    drawCursor(selected, offsetX, offset);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), mode == Mode::Lookup ? tr(STR_LOOKUP) : tr(STR_DONE),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    drawHints(labels);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    lastRenderedRangeStart = rangeStart;
    return true;
  }

  // Case 3: Moving cursor to choose end word (rangeStart >= 0, expanding or shrinking selection)
  if (rangeStart >= 0 && lastRenderedRangeStart == rangeStart) {
    if (selected == lastRenderedSelected) return true;

    const int firstOld = std::min(rangeStart, lastRenderedSelected);
    const int lastOld = std::max(rangeStart, lastRenderedSelected);
    const int firstNew = std::min(rangeStart, selected);
    const int lastNew = std::max(rangeStart, selected);

    if (words[firstOld].pageOffset != currentPageOffset || words[lastOld].pageOffset != currentPageOffset ||
        words[firstNew].pageOffset != currentPageOffset || words[lastNew].pageOffset != currentPageOffset) {
      return false;
    }

    for (int i = firstOld; i <= lastOld; ++i) {
      if (i < firstNew || i > lastNew) {
        drawWordClean(i, offsetX, offset);
      }
    }

    for (int i = firstNew; i <= lastNew; ++i) {
      if (i < firstOld || i > lastOld) {
        drawWordHighlight(i, firstNew, lastNew, offsetX, offset);
      }
    }

    if (lastRenderedSelected >= firstNew && lastRenderedSelected <= lastNew) {
      drawWordHighlight(lastRenderedSelected, firstNew, lastNew, offsetX, offset);
    }

    drawCursor(selected, offsetX, offset);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), mode == Mode::Lookup ? tr(STR_LOOKUP) : tr(STR_DONE),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    drawHints(labels);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    lastRenderedSelected = selected;
    return true;
  }

  return false;
}

// Front buttons, plus the side buttons' line moves where the theme labels them.
void ClipSelectionActivity::drawHints(const MappedInputManager::Labels& labels) const {
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (UITheme::getInstance().getMetrics().homeSideButtonHints) {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  // A short power press that opens the selection also closes it.
  if (mode == Mode::Clip && SETTINGS.shortPwrBtn == CrossPointSettings::CREATE_CLIPPING) {
    GUI.drawPowerButtonHint(renderer, tr(STR_CANCEL));
  }
}

void ClipSelectionActivity::markSavedClippings() {
  if (saved.pageCount == 0 || !CLIPPINGS.hasClippings()) return;
  for (size_t i = 0; i < wordCount; ++i) {
    WordBox& word = words[i];
    word.clipped = false;
    word.clipping = -1;
    const auto page = static_cast<uint16_t>(saved.firstPage + word.pageOffset);
    for (size_t c = 0; c < CLIPPINGS.clippingCount() && word.clipping < 0; ++c) {
      if (static_cast<int>(c) == editing) continue;
      if (clippingContainsWord(*CLIPPINGS.clippingAt(c), saved.spineIndex, page, saved.pageCount,
                               saved.layoutSignature, word.pageWordIndex, word.startOffset, word.endOffset)) {
        word.clipping = static_cast<int16_t>(c);
      }
    }
    word.clipped = word.clipping >= 0;
  }
}

bool ClipSelectionActivity::beginEdit(const int index) {
  if (mode != Mode::Clip || index < 0) return false;
  const Clipping* clipping = CLIPPINGS.clippingAt(static_cast<size_t>(index));
  if (!clipping) return false;
  int first = -1;
  int last = -1;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].clipping != index) continue;
    if (first < 0) first = i;
    last = i;
  }
  if (first < 0) return false;
  // A clipping that runs past these pages would be cut short on save.
  const bool hasOffsets = clipping->startOffset != UINT32_MAX && words[first].startOffset != UINT32_MAX &&
                          words[last].endOffset != UINT32_MAX;
  const bool whole = hasOffsets ? words[first].startOffset <= clipping->startOffset &&
                                      words[last].endOffset >= clipping->endOffset
                                : last - first + 1 == clipping->wordCount;
  if (!whole) return false;

  editing = index;
  editFirst = first;
  editLast = last;
  // The cursor takes the end on this page, so the selection stays in view.
  const bool endHere = words[last].pageOffset == currentPageOffset;
  selected = endHere ? last : first;
  rangeStart = endHere ? first : last;
  currentPageOffset = words[selected].pageOffset;
  markSavedClippings();
  lastRenderedPageOffset = -1;
  return true;
}

void ClipSelectionActivity::endEdit() {
  editing = editFirst = editLast = -1;
  rangeStart = -1;
  markSavedClippings();
  lastRenderedPageOffset = -1;
  requestUpdate();
}

void ClipSelectionActivity::drawSavedClippings(const int offsetX, const int offset) const {
  int spanLeft = 0;
  int spanRight = -1;  // < spanLeft: no open run
  int spanTop = 0;
  int spanRow = -1;
  const auto flush = [&]() {
    if (spanRight < spanLeft) return;
    // Same seed as the reader's pass, so the strokes match the page.
    if (!GUI.drawTextHighlighter(renderer, spanLeft, spanTop, spanRight - spanLeft, lineHeight,
                                 static_cast<uint32_t>(spanLeft * 7919 + spanTop * 104729), true)) {
      renderer.fillRectDither(spanLeft, spanTop, spanRight - spanLeft, lineHeight, Color::LightGray);
    }
    spanRight = spanLeft - 1;
  };
  for (size_t i = 0; i < wordCount; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset || !word.clipped || word.row != spanRow) flush();
    if (word.pageOffset != currentPageOffset || !word.clipped) continue;
    const int x = word.x + offsetX;
    if (spanRight < spanLeft) {
      spanLeft = x;
      spanRight = x + word.width;
      spanTop = word.y + offset;
      spanRow = word.row;
    } else {
      spanLeft = std::min(spanLeft, x);
      spanRight = std::max(spanRight, x + word.width);
    }
  }
  flush();
}

void ClipSelectionActivity::drawSavedBand(const int index, const int offsetX, const int offset) const {
  const WordBox& word = words[index];
  if (!word.clipped) return;
  const auto band = [&](const int x, const int width) {
    if (width <= 0) return;
    if (!GUI.drawTextHighlightBand(renderer, x, word.y + offset, width, lineHeight, true)) {
      renderer.fillRectDither(x, word.y + offset, width, word.height, Color::LightGray);
    }
  };
  band(word.x + offsetX, word.width);
  for (const int n : {index - 1, index + 1}) {
    if (n < 0 || n >= static_cast<int>(wordCount)) continue;
    const WordBox& other = words[n];
    if (!other.clipped || other.row != word.row || other.pageOffset != word.pageOffset) continue;
    const int left = std::min(word.x + word.width, other.x + other.width);
    const int right = std::max(word.x, other.x);
    band(left + offsetX, right - left);
  }
}

void ClipSelectionActivity::fillSelected(const int x, const int y, const int width, const int height) const {
  if (!GUI.drawTextHighlightBand(renderer, x, y, width, renderer.getLineHeight(fontId))) {
    renderer.fillRectDither(x, y, width, height, Color::LightGray);
  }
}

Rect ClipSelectionActivity::cursorRect(const int index, const int offsetX, const int offset) const {
  constexpr int kMargin = 5;   // the frame plus a little air: it sits clear of the glyphs
  // The line above isn't redrawn when the frame is erased: above the word the
  // frame stays out of that line's underline.
  constexpr int kAbove = 3;
  constexpr int kReserve = 3;  // of a gap or the air, kept clear of what's beyond
  const WordBox& word = words[index];
  const auto sideRoom = [&](const int neighbour, const bool before) {
    if (neighbour < 0 || neighbour >= static_cast<int>(wordCount)) return kMargin;
    const WordBox& other = words[neighbour];
    if (other.row != word.row || other.pageOffset != word.pageOffset) return kMargin;
    const int gap = before ? word.x - (other.x + other.width) : other.x - (word.x + word.width);
    // Up to the gap less a few pixels: only one word is ever framed, so the
    // frame may reach past the gap's middle, but not onto the next glyphs.
    return std::clamp(gap - kReserve, 0, kMargin);
  };
  const int left = word.x + offsetX - sideRoom(index - 1, true);
  const int right = word.x + offsetX + word.width + sideRoom(index + 1, false);
  // Line spacing leaves air under each line's glyphs: the bottom margin takes
  // it first, the top margin takes what is left of the line above's.
  const int lineTop = word.y + offset;
  const int glyphs = std::min<int>(word.height, renderer.getCenteringHeight(fontId));
  const int air = word.height - glyphs;
  const int below = std::min(air, kMargin);
  const int above = std::clamp(air - kReserve, 0, kAbove);
  const int top = lineTop - above;
  const int bottom = lineTop + glyphs + below;
  return Rect{left, top, right - left, bottom - top};
}

void ClipSelectionActivity::drawCursor(const int index, const int offsetX, const int offset) const {
  constexpr int kBar = 3;
  const Rect r = cursorRect(index, offsetX, offset);
  if (GUI.drawTextCursor(renderer, r.x, r.y, r.width, r.height, rangeStart >= 0)) return;
  renderer.fillRect(r.x, r.y, r.width, kBar, true);
  renderer.fillRect(r.x, r.y + r.height - kBar, r.width, kBar, true);
  renderer.fillRect(r.x, r.y, kBar, r.height, true);
  renderer.fillRect(r.x + r.width - kBar, r.y, kBar, r.height, true);
}

void ClipSelectionActivity::drawSelection() const {
  const int offset = textOffset();
  const int offsetX = textXOffset();
  const int first = rangeStart < 0 ? selected : std::min(rangeStart, selected);
  const int last = rangeStart < 0 ? selected : std::max(rangeStart, selected);
  const WordBox* previous = nullptr;
  for (int i = first; i <= last; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (previous) ditherGapBetween(*previous, word, offsetX, offset);
    fillSelected(word.x + offsetX, word.y + offset, word.width, word.height);
    renderer.drawText(fontId, word.x + offsetX, word.y + offset, word.text, true, word.style,
                      BidiUtils::BidiBaseDir::AUTO, word.characterSpacing);
    previous = &word;
  }
  if (rangeStart >= 0 && mappedInput.hasTouch()) {
    if (words[first].pageOffset == currentPageOffset)
      GUI.drawSelectionHandle(renderer, handleRect(first, true), !words[first].isRtl);
    if (words[last].pageOffset == currentPageOffset)
      GUI.drawSelectionHandle(renderer, handleRect(last, false), words[last].isRtl);
    if (!touchDragSelecting) GUI.drawSelectionActions(renderer, actionRect(), editing >= 0);
  } else {
    drawCursor(selected, offsetX, offset);
  }
}

void ClipSelectionActivity::render(RenderLock&&) {
  if (actionPopup.processRender(renderer, mappedInput)) {
    lastRenderedPageOffset = -1;
    return;
  }

  if (renderIncremental()) {
    return;
  }

  const int offset = textOffset();
  const int offsetX = textXOffset();
  renderer.clearScreen();
  const auto clip = renderer.getClipRect();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  renderer.setClipRect(safe.x, safe.y, safe.width, safe.height);
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  pages[currentPageOffset]->render(renderer, fontId, marginLeft + offsetX, marginTop + offset);
  scope.endScanAndPrewarm();
  drawSavedClippings(offsetX, offset);  // under the text, as in the reader
  pages[currentPageOffset]->render(renderer, fontId, marginLeft + offsetX, marginTop + offset);
  if (wordCount != 0) drawSelection();
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
  if (saved.drawRunningHead) saved.drawRunningHead();

  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), rangeStart < 0 ? tr(STR_SELECT) : (mode == Mode::Lookup ? tr(STR_LOOKUP) : tr(STR_DONE)),
      tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  drawHints(labels);
  renderer.displayBuffer();

  lastRenderedPageOffset = currentPageOffset;
  lastRenderedTextOffset = offset;
  lastRenderedTextXOffset = offsetX;
  lastRenderedRangeStart = rangeStart;
  lastRenderedSelected = selected;
}
