#include "globals.h"

#if USE_M5LCD

#include <cstring>

#include <esp_heap_caps.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <M5Unified.h>

#include "deviceconfig.h"
#include "effectmanager.h"
#include "ledstripeffect.h"
#include "m5tabgfx.h"
#include "systemcontainer.h"
#include "values.h"

namespace
{
constexpr uint32_t kCaptionFadeInTime = 500;
constexpr uint32_t kCaptionFadeOutTime = 1000;
}

M5TabGFX::M5TabGFX(size_t width, size_t height) : GFXBase(width, height)
{
    leds = static_cast<CRGB *>(heap_caps_calloc(width * height, sizeof(CRGB), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!leds)
        throw std::runtime_error("Unable to allocate Tab5 effect framebuffer");

    auto *panel = static_cast<lgfx::Panel_DSI *>(M5.Display.getPanel());
    _displayBuffer = static_cast<uint16_t *>(panel->config_detail().buffer);
    if (!_displayBuffer)
        throw std::runtime_error("Unable to access Tab5 DSI scanout buffer");

    // Rotation changes M5GFX's logical width and height, but it does not
    // rotate the DSI scanout allocation itself. The Tab5 buffer therefore
    // remains a portrait, row-major 720x1280 surface at rotation 1.
    if (M5.Display.getRotation() != 1)
        throw std::runtime_error("Tab5 effect framebuffer requires display rotation 1");
    _physicalWidth = panel->config().panel_width;
    _physicalHeight = panel->config().panel_height;
    _displayWidth = M5.Display.width();
    _displayHeight = M5.Display.height();
    if (_displayWidth != _physicalHeight || _displayHeight != _physicalWidth)
        throw std::runtime_error("Unexpected Tab5 DSI framebuffer orientation");

    if (width == 0 || height == 0 ||
        _displayWidth % width != 0 || _displayHeight % height != 0)
        throw std::runtime_error("Tab5 effect framebuffer must scale evenly to the native display");

    _scaleX = _displayWidth / width;
    _scaleY = _displayHeight / height;
    if (_scaleX != _scaleY)
        throw std::runtime_error("Tab5 effect framebuffer requires uniform scaling");

    _captionCanvas = std::make_unique<GFXcanvas1>(width, 8);
    if (!_captionCanvas || !_captionCanvas->getBuffer())
        throw std::runtime_error("Unable to allocate Tab5 caption mask");

    static_assert(sizeof(CRGB) == sizeof(lgfx::bgr888_t));
    debugI("Tab5 logical matrix surface: %zux%zu, scaling %ux to landscape %ux%u "
           "(physical buffer %ux%u)",
           width, height, _scaleX, _displayWidth, _displayHeight,
           _physicalWidth, _physicalHeight);
}

M5TabGFX::~M5TabGFX()
{
    _displayBuffer = nullptr;
    heap_caps_free(leds);
    leds = nullptr;
}

void M5TabGFX::InitializeHardware(std::vector<std::shared_ptr<GFXBase>> &devices)
{
    M5.Display.fillScreen(TFT_BLACK);

    auto display = make_shared_psram<M5TabGFX>(MATRIX_WIDTH, MATRIX_HEIGHT);
    display->loadPalette(0);
    devices.push_back(std::move(display));
}

void M5TabGFX::fillRectangle(int x0, int y0, int x1, int y1, CRGB color)
{
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::clamp(x1, 0, static_cast<int>(_width));
    y1 = std::clamp(y1, 0, static_cast<int>(_height));

    if (x0 >= x1 || y0 >= y1)
        return;

    for (int y = y0; y < y1; ++y)
        std::fill(leds + y * _width + x0, leds + y * _width + x1, color);
}

void M5TabGFX::SetCaption(const String &caption, uint32_t duration)
{
    _caption = caption;
    _captionStartTime = millis();
    _captionDuration = duration;
}

float M5TabGFX::CaptionTransparency() const
{
    const uint32_t elapsed = millis() - _captionStartTime;
    if (_caption.isEmpty() || elapsed >= kCaptionFadeInTime + _captionDuration + kCaptionFadeOutTime)
        return 0.0f;
    if (elapsed < kCaptionFadeInTime)
        return static_cast<float>(elapsed) / kCaptionFadeInTime;
    if (elapsed > kCaptionFadeInTime + _captionDuration)
        return 1.0f -
            static_cast<float>(elapsed - kCaptionFadeInTime - _captionDuration) / kCaptionFadeOutTime;
    return 1.0f;
}

void M5TabGFX::PostProcessFrame(size_t localPixelsDrawn, size_t wifiPixelsDrawn)
{
    if (localPixelsDrawn + wifiPixelsDrawn == 0)
        return;

    const auto &deviceConfig = g_ptrSystem->GetDeviceConfig();
    const uint8_t brightness = scale8(deviceConfig.GetBrightness(), g_Values.Fader);
    if (brightness != _lastBrightness)
    {
        M5.Display.setBrightness(brightness);
        _lastBrightness = brightness;
    }

    const auto &effectManager = g_ptrSystem->GetEffectManager();
    const float captionAlpha =
        effectManager.HasCurrentEffect() && effectManager.GetCurrentEffect().ShouldShowTitle()
            ? CaptionTransparency()
            : 0.0f;
    constexpr int kCaptionHeight = 8;
    const int captionY = static_cast<int>(_height) - kCaptionHeight - 1;
    if (captionAlpha > 0.0f)
    {
        const uint32_t totalCaptionTime =
            kCaptionFadeInTime + _captionDuration + kCaptionFadeOutTime;
        const uint32_t elapsed = millis() - _captionStartTime;
        const int textWidth = _caption.length() * 6;
        const int x = textWidth > static_cast<int>(_width)
            ? static_cast<int>(_width) -
                static_cast<int>((static_cast<uint64_t>(elapsed) *
                                  (textWidth + _width)) / totalCaptionTime)
            : (static_cast<int>(_width) - textWidth) / 2;

        _captionCanvas->fillScreen(0);
        _captionCanvas->setTextWrap(false);
        _captionCanvas->setTextSize(1);
        _captionCanvas->setTextColor(1);
        _captionCanvas->setCursor(x, 0);
        _captionCanvas->print(_caption);
    }

    const CRGB captionColor(
        static_cast<uint8_t>(captionAlpha * 255.0f),
        static_cast<uint8_t>(captionAlpha * 255.0f),
        static_cast<uint8_t>(captionAlpha * 255.0f));

    // At rotation 1, landscape (x,y) maps to physical
    // (physicalWidth - 1 - y, x). Build one physical scanline from each
    // logical column, then replicate it for the horizontal scale factor.
    // Using displayWidth as the raw stride would incorrectly treat the
    // portrait DSI allocation as landscape and scatter each logical row.
    for (size_t sourceX = 0; sourceX < _width; ++sourceX)
    {
        uint16_t *firstPhysicalRow =
            _displayBuffer + sourceX * _scaleX * _physicalWidth;
        uint16_t *outputPixel = firstPhysicalRow;

        for (size_t sourceY = _height; sourceY-- > 0;)
        {
            CRGB pixel = leds[sourceY * _width + sourceX];
            if (captionAlpha > 0.0f &&
                sourceY >= static_cast<size_t>(captionY) &&
                sourceY < static_cast<size_t>(captionY + kCaptionHeight) &&
                _captionCanvas->getPixel(
                    sourceX, static_cast<int>(sourceY) - captionY))
            {
                pixel = captionColor;
            }
            const uint16_t rgb565 = (static_cast<uint16_t>(pixel.r & 0xF8U) << 8U) |
                                    (static_cast<uint16_t>(pixel.g & 0xFCU) << 3U) |
                                    (pixel.b >> 3U);
            for (uint16_t copyY = 0; copyY < _scaleY; ++copyY)
                *outputPixel++ = rgb565;
        }

        for (uint16_t copyX = 1; copyX < _scaleX; ++copyX)
            std::memcpy(firstPhysicalRow + copyX * _physicalWidth,
                        firstPhysicalRow,
                        _physicalWidth * sizeof(uint16_t));
    }

    // Direct writes bypass M5GFX's dirty tracking. Mark the complete logical
    // display dirty so the P4 cache is written back before DSI scans it.
    M5.Display.display(0, 0, _displayWidth, _displayHeight);
    g_Values.Brite = 100.0 * brightness / 255;
    g_Values.Watts = 0;
}

#endif
