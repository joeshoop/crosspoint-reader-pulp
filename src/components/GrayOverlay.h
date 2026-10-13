#pragma once

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Memory.h>

#include <cstdint>

// Displays the framebuffer, then lays gray planes over it, as the reader's
// anti-aliasing does. `drawGrays` runs once per plane in the current gray
// render mode and draws only content that has gray tones. The frame is shown
// either way; returns true when the grays reached the panel too.
//
// Where the panel allows it, both planes render into scratch while the B/W
// waveform runs, so the gray pass costs little more than its own refresh.
namespace grayoverlay {

template <typename DrawFn>
bool present(GfxRenderer& renderer, const HalDisplay::RefreshMode mode, DrawFn&& drawGrays) {
  const auto gray = renderer.grayscaleCapabilities();
  if (!gray.supported()) {
    renderer.displayBuffer(mode);
    return false;
  }

  if (gray.stripUploads && gray.asyncBase && gray.base != HalDisplay::GrayscaleBase::Combined) {
    // Same margins the reader keeps for its plane buffers.
    constexpr size_t kHeapHeadroom = 60000;
    constexpr size_t kMaxAllocReserve = 16 * 1024;
    const int gh = renderer.getDisplayHeight();
    const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * gh;
    const auto fits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + kHeapHeadroom && ESP.getMaxAllocHeap() >= planeBytes + kMaxAllocReserve;
    };
    // Two whole planes (48 KB each on X4), freed on return.
    auto lsb = fits() ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msb = (lsb && fits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    if (msb) {
      renderer.displayBufferAsync(mode);
      const auto renderPlane = [&](const GfxRenderer::RenderMode planeMode, uint8_t* buf) {
        renderer.setRenderMode(planeMode);
        renderer.beginStripTarget(buf, 0, gh);
        renderer.clearScreen(0x00);
        drawGrays();
        renderer.endStripTarget();
      };
      renderPlane(GfxRenderer::GRAYSCALE_LSB, lsb.get());
      renderPlane(GfxRenderer::GRAYSCALE_MSB, msb.get());
      renderer.setRenderMode(GfxRenderer::BW);

      renderer.waitRefreshComplete();
      renderer.writeGrayscalePlaneStrip(true, lsb.get(), 0, gh);
      renderer.writeGrayscalePlaneStrip(false, msb.get(), 0, gh);
      renderer.displayGrayBuffer();
      renderer.cleanupGrayscaleWithFrameBuffer();
      return true;
    }
  }

  // Sequential fallback through the stored-BW slot.
  renderer.displayBuffer(mode);
  if (!renderer.storeBwBuffer()) return false;
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  drawGrays();
  renderer.copyGrayscaleLsbBuffers();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  drawGrays();
  renderer.copyGrayscaleMsbBuffers();
  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
  return true;
}

}  // namespace grayoverlay
