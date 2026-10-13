#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "ReaderToolbarUi.h"
#include "components/UITheme.h"
#include "components/UiSliderDialog.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_SLIDER = 1;
constexpr fui::ActionId ACTION_STEP = 2;
constexpr fui::ActionId ACTION_OK = 4;
constexpr fui::ActionId ACTION_CHROME = 5;  // absorbs taps on the dialog body; no handler
// Fine/coarse step sizes for percent adjustments (buttons and -/+ tap zones).
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;

// The side buttons step as they turn pages: the one that turns back steps
// back. Up turns back unless the side layout or the orientation flips it
// (layouts with both buttons alike keep Up as back).
int upStep(const MappedInputManager& mappedInput) {
  const bool nextPrev = SETTINGS.sideButtonLayout == CrossPointSettings::NEXT_PREV;
  return nextPrev == mappedInput.isNavDirectionSwapped() ? -kLargeStep : kLargeStep;
}
}  // namespace

EpubReaderPercentSelectionActivity::EpubReaderPercentSelectionActivity(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput,
                                                                       const int initialPercent)
    : Activity("EpubReaderPercentSelection", renderer, mappedInput), UiAppHost(renderer), percent(initialPercent) {}

void EpubReaderPercentSelectionActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_SLIDER, &EpubReaderPercentSelectionActivity::onSliderEvent, this);
  app.on(ACTION_STEP, &EpubReaderPercentSelectionActivity::onStepEvent, this);
  app.on(ACTION_OK, &EpubReaderPercentSelectionActivity::onOkEvent, this);
  app.setScreen(&EpubReaderPercentSelectionActivity::percentScreen, this);
  // Set up rendering task and mark first frame dirty.
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  // Wrap using a 100-value ring (0% and 100% are the same wrap point), but keep 100 as the
  // natural landing value when reached without crossing the boundary (e.g. 90 + 10 = 100).
  const int raw = percent + delta;
  moved = true;
  if (raw > 0 && raw % 100 == 0) {
    percent = 100;
  } else {
    percent = ((raw % 100) + 100) % 100;
  }
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::setPercent(const int value) {
  const int clamped = std::clamp(value, 0, 100);
  if (clamped == percent) return;
  percent = clamped;
  moved = true;
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onSliderEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderPercentSelectionActivity*>(user);
  if (event.dragPermille < 0) return;
  self->setPercent((static_cast<int>(event.dragPermille) * 100 + 500) / 1000);
}

void EpubReaderPercentSelectionActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<EpubReaderPercentSelectionActivity*>(user)->adjustPercent(event.value * kSmallStep);
}

void EpubReaderPercentSelectionActivity::onOkEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<EpubReaderPercentSelectionActivity*>(user);
  self->app.clearTapFlash();  // the tap leaves this screen
  self->confirm();
}

void EpubReaderPercentSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderPercentSelectionActivity::confirm() {
  setResult(PercentResult{percent});
  finish();
}

void EpubReaderPercentSelectionActivity::loop() {
  // Touch goes through the FreeInkApp: render() registered the slider and -/+ hit
  // rects; the slider follows the finger via InputDrag (dragPermille per held frame).
  // Runs before the Back handler because the release of a drag can also register as a
  // swipe (e.g. the left-edge rightward back gesture) — the drag must consume it so it
  // can't cancel the dialog or step the percent.
  const auto route = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (route.routed && app.invalidated()) requestUpdate();
  if (route) {
    if (route.event.dragPermille >= 0) draggingSlider = true;
    return;
  }
  if (routingReady() && draggingSlider) {
    // Drag ended (possibly off the slider): swallow the tap/swipe events it produced.
    if (!route.snap.touchHeld) draggingSlider = false;
    return;
  }
  // Tap released outside the dialog (inside-taps are absorbed by the chrome
  // guard): cancel. Swipe-end releases arrive with -1,-1 coords and fall
  // through — same rule as OptionPopup.
  if (route.routed && route.snap.touchReleased && route.snap.touchX >= 0) {
    cancel();
    return;
  }

  // Back cancels, confirm selects, arrows adjust the percent.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Right) {
    adjustPercent(kLargeStep);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Left) {
    adjustPercent(-kLargeStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustPercent(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustPercent(kSmallStep); });

  const int upDelta = upStep(mappedInput);
  const int downDelta = -upDelta;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, upDelta] { adjustPercent(upDelta); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                       [this, downDelta] { adjustPercent(downDelta); });
}

void EpubReaderPercentSelectionActivity::percentScreen(UiScreen& screen, void* user) {
  static_cast<EpubReaderPercentSelectionActivity*>(user)->buildPercentScreen(screen);
}

void EpubReaderPercentSelectionActivity::buildPercentScreen(UiScreen& screen) {
  // One line, "Go to 67%": the value is the title. Until the value first
  // moves it reads "Currently at 61%" instead.
  char readout[48];
  snprintf(readout, sizeof(readout), !moved ? tr(STR_CURRENTLY_AT_PERCENT_VALUE) : tr(STR_GO_TO_PERCENT_VALUE),
           percent);
  // Where the side buttons carry their own labels, the button hints say it all.
  const bool sideHints = UITheme::getInstance().getMetrics().homeSideButtonHints;
  char hint1[64];
  snprintf(hint1, sizeof(hint1), "%s %d%%", I18N.get(StrId::STR_STEP_HINT_FRONT), kSmallStep);
  char hint2[64];
  snprintf(hint2, sizeof(hint2), "%s %d%%", I18N.get(StrId::STR_STEP_HINT_SIDE), kLargeStep);

  UiSliderDialogSpec spec;
  spec.title = nullptr;
  spec.readout = readout;
  spec.value = percent;
  spec.max = 100;
  spec.minLabel = "0%";
  spec.maxLabel = "100%";
  spec.sliderAction = ACTION_SLIDER;
  spec.stepAction = ACTION_STEP;
  spec.okAction = ACTION_OK;
  spec.chromeAction = ACTION_CHROME;
  spec.hintLine1 = sideHints ? nullptr : hint1;
  spec.hintLine2 = sideHints ? nullptr : hint2;
  // Opened from the reader menu sheet: cover it.
  spec.sheetTop = ReaderToolbarUi::lastMenuSheetTop();
  buildSliderDialogScreen(screen, uiTarget, mappedInput, spec);
}

void EpubReaderPercentSelectionActivity::render(RenderLock&&) {
  // No clearScreen: the dialog is a popup — it dims the frame it opened over
  // and draws the card on top. Everything renders through the app so the
  // slider, -/+ zones, and Cancel/OK register touch hit rects.
  renderUi();

  // Button hints follow the current front button layout.
  char less[8];
  char more[8];
  snprintf(less, sizeof(less), "-%d%%", kSmallStep);
  snprintf(more, sizeof(more), "+%d%%", kSmallStep);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), less, more);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (UITheme::getInstance().getMetrics().homeSideButtonHints) {
    // Up and Down step by the large step, the way they turn pages.
    char up[8];
    char down[8];
    const int upDelta = upStep(mappedInput);
    snprintf(up, sizeof(up), "%+d%%", upDelta);
    snprintf(down, sizeof(down), "%+d%%", -upDelta);
    GUI.drawSideButtonHints(renderer, up, down);
  }

  renderer.displayBuffer();
}
