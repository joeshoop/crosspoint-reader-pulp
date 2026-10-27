#pragma once

#include <Icon.h>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// The control center: a gray card of rounded controls opened by a top-edge
// down-swipe, a status-bar tap, or a button bound to "Control Center".
// Brightness and warmth sliders (on boards with a
// light), a row of rotate, night mode and light on/off, and Settings. It hangs
// from the top when pulled down and rises from the bottom when opened from a
// button.
class FrontlightPanelActivity final : public Activity, private UiAppHost {
 public:
  enum class Anchor : uint8_t { Top, Bottom };

 private:
  ButtonNavigator buttonNavigator;
  Anchor anchor;

  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  // lightOn is seeded from the live hardware state (Frontlight.isOn()), which
  // can differ from the saved SETTINGS.frontlightOn preference (main.cpp's
  // restoreLightOn), so onExit() persists it only once the user touched it.
  bool lightOnChanged = false;
  bool draggingSlider = false;
  int panelTop = 0;
  int panelBottom = 0;

  static void panelScreen(UiScreen& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTileEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildPanelScreen(UiScreen& screen);
  // A white pill with an icon at each end and a round black thumb carrying
  // the value; the whole pill drags.
  void drawSlider(UiScreen& screen, int y, uint8_t value, bool on, const freeink::Icon& low,
                  const freeink::Icon& high, freeink::ui::ActionId action);
  int panelHeight() const;
  void adjustBrightness(int delta);
  void toggleLight();
  void runTile(int idx);
  // Copy the panel's live brightness/warmth/lightOn into SETTINGS and save if
  // anything actually changed. onExit() runs it on every way out.
  void persistLightSettings();
  void close();

  // One-shot: a tile that rewrote the whole frame (night mode) re-drives it
  // with the ghost-cleanup waveform on the next render.
  bool cleanRefreshPending = false;

 public:
  FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Anchor anchor = Anchor::Top);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
};
