#pragma once

#include "globals.h"

#if USE_M5LCD

#include <memory>
#include <vector>

#include "gfxbase.h"

// Mesmerizer effects render into a logical CRGB surface. The backend expands
// it by an exact integer factor into the Tab5's native DSI scanout buffer.
class M5TabGFX final : public GFXBase
{
  public:
    M5TabGFX(size_t width, size_t height);
    ~M5TabGFX() override;

    static void InitializeHardware(std::vector<std::shared_ptr<GFXBase>> &devices);
    void PostProcessFrame(uint16_t localPixelsDrawn, uint16_t wifiPixelsDrawn) override;
    void fillRectangle(int x0, int y0, int x1, int y1, CRGB color) override;

    // Not an override: captioning is not part of GFXBase. HUB75GFX declares its own
    // non-virtual SetCaption too, and both call sites (EffectManager::StartEffect and
    // the OTA progress handler) are #if USE_HUB75 and static_cast to HUB75GFX. If a
    // second backend ever needs captions, hoist this into GFXBase rather than adding
    // a third copy.
    void SetCaption(const String &caption, uint32_t duration);

    __attribute__((always_inline)) uint16_t xy(uint16_t x, uint16_t y) const noexcept override
    {
        return x < _width && y < _height ? y * _width + x : 0;
    }

  private:
    uint16_t *_displayBuffer = nullptr;
    uint16_t _displayWidth = 0;
    uint16_t _displayHeight = 0;
    uint16_t _physicalWidth = 0;
    uint16_t _physicalHeight = 0;
    uint16_t _scaleX = 1;
    uint16_t _scaleY = 1;
    uint8_t _lastBrightness = 0;
    String _caption;
    unsigned long _captionStartTime = 0;
    uint32_t _captionDuration = 0;
    std::unique_ptr<GFXcanvas1> _captionCanvas;

    float CaptionTransparency() const;
};

#endif
