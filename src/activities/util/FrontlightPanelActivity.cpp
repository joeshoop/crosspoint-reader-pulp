#include "FrontlightPanelActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SettingsActivity.h"
#include "components/BatteryStatus.h"
#include "components/FloatButtons.h"
#include "components/UIScale.h"
#include "components/icons/readerFloatIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TILE = 3;  // value = Tile

enum Tile : int16_t { kRotate, kNight, kLight, kRefresh, kSettings };

// The card sits kEdge from the screen's sides and its anchored edge; its
// controls are kRow tall, kGap apart, kPad inside it.
constexpr int kEdge = 8;
constexpr int kPad = 30;
constexpr int kSide = 24;  // card side to the controls
constexpr int kRow = 72;
constexpr int kGap = 16;
constexpr int kRadius = 24;
// Slider thumb centre travel, from the pill's left edge, at 0 and 100%.
constexpr int kThumbFrom = 100;
constexpr int kThumbEnd = 64;  // short of the right edge, before the icon
// The dimmest setting is 1%, not 0: Confirm (or the power double-click)
// turns the light off.
constexpr uint8_t MIN_BRIGHTNESS = 1;
constexpr int BRIGHTNESS_STEP = 1;

uint8_t percentFromPermille(const int16_t permille) {
  return static_cast<uint8_t>(std::clamp((static_cast<int>(permille) * 100 + 500) / 1000, 0, 100));
}

fui::Rect rect(const int x, const int y, const int w, const int h) {
  return fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h)};
}

void drawIconCentered(const GfxRenderer& renderer, const freeink::Icon& icon, const int cx, const int cy,
                      const bool black = true) {
  float_ui::drawIcon(renderer, icon, cx - icon.w / 2, cy - icon.h / 2, black);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 const Anchor anchor)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer), anchor(anchor) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  resetUi();
  useSansChrome();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

// The pill drags; its permille maps onto the thumb's travel.
static uint8_t valueFromPill(const int16_t permille, const int pillWidth) {
  const int x = permille * pillWidth / 1000;
  const int travel = pillWidth - kThumbFrom - kThumbEnd;
  return static_cast<uint8_t>(std::clamp(((x - kThumbFrom) * 100 + travel / 2) / travel, 0, 100));
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  const int pillWidth = self->renderer.getScreenWidth() - 2 * (kEdge + kSide);
  self->brightness = std::max(MIN_BRIGHTNESS, valueFromPill(event.dragPermille, pillWidth));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  const int pillWidth = self->renderer.getScreenWidth() - 2 * (kEdge + kSide);
  self->warmth = valueFromPill(event.dragPermille, pillWidth);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runTile(event.value);
}

void FrontlightPanelActivity::runTile(const int idx) {
  switch (idx) {
    case kNight:  // inverted output polarity, applied to the whole UI
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity.
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case kLight:
      toggleLight();
      break;
    case kRefresh:
      // Drop the panel first and let the repaint underneath carry the clean
      // waveform: refreshing the panel itself would clean a frame about to go.
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      close();
      break;
    case kRotate:
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % 4);
      SETTINGS.saveToFile();
      // Only the setting changes: the reader reflows on its next loop().
      requestUpdate();
      break;
    case kSettings:
      // Back from Settings returns here and the panel closes, so the screen
      // it was opened over (a book, Home) carries on.
      startActivityForResult(std::make_unique<SettingsActivity>(renderer, mappedInput, /*returnOnBack=*/true),
                             [this](const ActivityResult&) { close(); });
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // Swiping it back the way it came dismisses it. draggingSlider keeps a
    // fast slider flick from closing it.
    const auto swipe = mappedInput.wasSwipe();
    if (!draggingSlider && swipe == (anchor == Anchor::Top ? MappedInputManager::SwipeDir::Up
                                                           : MappedInputManager::SwipeDir::Down)) {
      close();
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 &&
        (touch.snap.touchY >= panelBottom || touch.snap.touchY < panelTop)) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) {
      draggingSlider = false;
      cleanRefreshPending = true;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::panelHeight() const {
  int rows = 2;  // the tiles, Settings; the battery line goes below
  if (Frontlight.present()) rows += Frontlight.hasColorTemperature() ? 2 : 1;
  return 2 * kPad + rows * kRow + (rows - 1) * kGap + kGap + battery_status::kBadgeHeight;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::drawSlider(UiScreen& screen, const int y, const uint8_t value, const bool on,
                                         const freeink::Icon& low, const freeink::Icon& high,
                                         const fui::ActionId action) {
  const int x = kEdge + kSide;
  const int w = renderer.getScreenWidth() - 2 * x;
  renderer.fillRoundedRect(x, y, w, kRow, kRow / 2, Color::White);
  drawIconCentered(renderer, low, x + 35, y + kRow / 2);
  drawIconCentered(renderer, high, x + w - 35, y + kRow / 2);
  const int cx = x + kThumbFrom + value * (w - kThumbFrom - kThumbEnd) / 100;
  // Off: an outlined thumb, so the level still shows.
  renderer.fillRoundedRect(cx - kRow / 2, y, kRow, kRow, kRow / 2, on ? Color::Black : Color::White);
  if (!on) renderer.drawRoundedRect(cx - kRow / 2, y, kRow, kRow, 2, kRow / 2, true);
  char number[4];
  snprintf(number, sizeof(number), "%u", static_cast<unsigned>(value));
  const int font = readerChromeSansFontId();
  const int textY = y + (kRow - renderer.getLineHeight(font)) / 2;
  renderer.drawText(font, cx - renderer.getTextWidth(font, number) / 2, textY, number, !on);
  screen.frame().hit(rect(x, y, w, kRow), action, 0, fui::InputDrag);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const int width = renderer.getScreenWidth();
  const int cardW = width - 2 * kEdge;
  const int cardH = panelHeight();
  panelTop = anchor == Anchor::Top ? kEdge : renderer.getScreenHeight() - kEdge - cardH;
  panelBottom = panelTop + cardH;
  // White first, so the dither reads as gray over the page.
  renderer.fillRoundedRect(kEdge, panelTop, cardW, cardH, kRadius, Color::White);
  renderer.fillRoundedRect(kEdge, panelTop, cardW, cardH, kRadius, Color::LightGray);

  const int x = kEdge + kSide;
  const int w = width - 2 * x;
  int y = panelTop + kPad;
  if (Frontlight.present()) {
    drawSlider(screen, y, brightness, lightOn, icon_float_sun_dim_28, icon_float_sun_28, ACTION_BRIGHTNESS);
    y += kRow + kGap;
    if (Frontlight.hasColorTemperature()) {
      drawSlider(screen, y, warmth, true, icon_float_cool_28, icon_float_warm_28, ACTION_WARMTH);
      y += kRow + kGap;
    }
  }

  // Rotate, night mode, and the light on/off (refresh on boards without a
  // light): round, spread across the row. A tile that is on is black.
  const freeink::Icon* icons[] = {&icon_float_rotate_28, &icon_float_night_28, &icon_float_refresh_28};
  const int16_t tiles[] = {kRotate, kNight, Frontlight.present() ? kLight : kRefresh};
  const int spacing = (w - 3 * kRow) / 2;
  for (int i = 0; i < 3; i++) {
    const int tx = x + i * (kRow + spacing);
    const bool active = (tiles[i] == kNight && SETTINGS.screenInverted) || (tiles[i] == kLight && lightOn);
    renderer.fillRoundedRect(tx, y, kRow, kRow, kRow / 2, active ? Color::Black : Color::White);
    drawIconCentered(renderer, *icons[i], tx + kRow / 2, y + kRow / 2, !active);
    screen.frame().hit(rect(tx, y, kRow, kRow), ACTION_TILE, tiles[i], fui::InputTouch);
  }
  y += kRow + kGap;

  // Settings.
  const int font = readerChromeSansFontId();
  renderer.fillRoundedRect(x, y, w, kRow, kRow / 2, Color::White);
  renderer.drawText(font, x + 32, y + (kRow - renderer.getLineHeight(font)) / 2, tr(STR_SETTINGS_TITLE));
  drawIconCentered(renderer, icon_float_settings_28, x + w - 32 - 14, y + kRow / 2);
  screen.frame().hit(rect(x, y, w, kRow), ACTION_TILE, kSettings, fui::InputTouch);
  y += kRow + kGap;

  battery_status::drawBadge(renderer, width / 2, y);
}

void FrontlightPanelActivity::render(RenderLock&&) {
  renderUi();
  // A tile that rewrote the whole frame (night mode) re-drives every pixel
  // once; ordinary repaints stay on the fast path. HALF flips polarity without
  // the FULL waveform's blackout flash.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
