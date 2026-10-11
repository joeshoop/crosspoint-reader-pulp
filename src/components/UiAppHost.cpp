#include "UiAppHost.h"

#include "UiAppHelpers.h"

namespace fui = freeink::ui;

UiAppHost::UiAppHost(const GfxRenderer& renderer)
    : uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()) {
  boundBodyFontId = uiScaleSpec().bodyFontId;
}

void UiAppHost::resetUi() {
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
}

void UiAppHost::renderUi() {
  // The old font may already be unloaded, so a stale binding would draw no text.
  const int bodyFontId = uiScaleSpec().bodyFontId;
  if (bodyFontId != boundBodyFontId) {
    bindUiFonts(uiTarget);
    boundBodyFontId = bodyFontId;
    applySharedUiTheme(app, uiTarget);  // row heights follow the new line height
  }
  app.setDevice(uiTarget.deviceContext());
  app.render();
  uiReady = true;
}

UiAppHost::TouchRoute UiAppHost::routeTouch(const MappedInputManager& input, const bool withLongPress,
                                            const bool routeHeld) {
  TouchRoute result;  // named apart from route() — cppcheck flags the shadow
  if (!uiReady) return result;
  result.snap = touchSnapshotFrom(input, withLongPress);
  if (!result.snap.touchPressed && !result.snap.touchReleased && !(routeHeld && result.snap.touchHeld)) {
    return result;
  }
  result.routed = true;
  result.event = route(result.snap);
  return result;
}

fui::ActionEvent UiAppHost::route(const fui::InputSnapshot& snap) {
  if (!uiReady) return {};
  const auto event = app.route(snap);
  // Releases activate controls; held/pressed frames only highlight or drag.
  // Empty/disabled hit regions produce no event and therefore no feedback.
  if (event && (snap.touchReleased || snap.swipeLeft || snap.swipeRight)) {
    haptic_feedback::touchAction(event.longPress);
  }
  return event;
}
