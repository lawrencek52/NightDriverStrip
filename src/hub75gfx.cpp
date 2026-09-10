//+--------------------------------------------------------------------------
//
// File:        hub75gfx.cpp
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// Unified HUB75 backend using ESP32-HUB75-MatrixPanel-DMA. The library uses
// the original ESP32 I2S parallel peripheral and the ESP32-S3 LCD/GDMA
// peripheral behind one API.
//
//---------------------------------------------------------------------------

#include "globals.h"

#if USE_HUB75

#include <algorithm>
#include <cstring>

#include "deviceconfig.h"
#include "effectmanager.h"
#include "hub75gfx.h"
#include "ledstripeffect.h"
#include "soundanalyzer.h"
#include "systemcontainer.h"
#include "values.h"

std::unique_ptr<MatrixPanel_I2S_DMA> HUB75GFX::matrix;
CRGB HUB75GFX::frameBuffers[2][HUB75GFX::kMatrixWidth * HUB75GFX::kMatrixHeight] = {};
uint8_t HUB75GFX::drawBufferIndex = 0;
uint32_t HUB75GFX::lastSwapMs = 0;

HUB75GFX::HUB75GFX(size_t w, size_t h) : GFXBase(w, h)
{
}

HUB75GFX::~HUB75GFX() = default;

void HUB75GFX::InitializeHardware(std::vector<std::shared_ptr<GFXBase>>& devices)
{
    StartMatrix();

    for (int i = 0; i < NUM_CHANNELS; i++)
    {
        auto tmpMatrix = make_shared_psram<HUB75GFX>(MATRIX_WIDTH, MATRIX_HEIGHT);
        devices.push_back(tmpMatrix);
        tmpMatrix->loadPalette(0);
    }

    static_cast<HUB75GFX&>(*devices[0]).setLeds(GetMatrixBackBuffer());
}

void HUB75GFX::SetBrightness(byte amount)
{
    if (matrix)
        matrix->setBrightness8(amount);
}

int HUB75GFX::GetRefreshRate()
{
    return matrix ? matrix->calculated_refresh_rate : 0;
}

int HUB75GFX::EstimatePowerDraw()
{
    constexpr auto kBaseLoad       = 1500;
    constexpr auto mwPerPixelRed   = 4.10f;
    constexpr auto mwPerPixelGreen = 0.82f;
    constexpr auto mwPerPixelBlue  = 1.75f;

    float totalPower = kBaseLoad;
    for (int i = 0; i < NUM_LEDS; i++)
    {
        const auto pixel = leds[i];
        totalPower += pixel.r * mwPerPixelRed   / 255.0f;
        totalPower += pixel.g * mwPerPixelGreen / 255.0f;
        totalPower += pixel.b * mwPerPixelBlue  / 255.0f;
    }
    return static_cast<int>(totalPower);
}

void HUB75GFX::setLeds(CRGB* pLeds)
{
    leds = pLeds;
}

void HUB75GFX::fillLeds(const CRGB* pLEDs)
{
    memcpy(leds, pLEDs, sizeof(CRGB) * GetLEDCount());
}

void HUB75GFX::Clear(CRGB color)
{
    std::fill_n(frameBuffers[0], _ledcount, color);
    std::fill_n(frameBuffers[1], _ledcount, color);
    leds = frameBuffers[drawBufferIndex];
}

const String& HUB75GFX::GetCaption()
{
    return strCaption;
}

float HUB75GFX::GetCaptionTransparency()
{
    const unsigned long now = millis();
    if (strCaption.isEmpty() || now > captionStartTime + totalCaptionDuration)
        return 0.0f;

    const float elapsed = now - captionStartTime;
    if (elapsed < captionFadeInTime)
        return elapsed / captionFadeInTime;
    if (elapsed > captionFadeInTime + captionDuration)
        return std::max(0.0f, 1.0f - ((elapsed - captionFadeInTime - captionDuration) / captionFadeOutTime));
    return 1.0f;
}

void HUB75GFX::SetCaption(const String& str, uint32_t duration)
{
    captionDuration = static_cast<float>(duration);
    totalCaptionDuration = captionFadeInTime + captionDuration + captionFadeOutTime;
    strCaption = str;
    captionStartTime = millis();
}

void HUB75GFX::MoveInwardX(int startY, int endY)
{
    const int halfWidth = MATRIX_WIDTH / 2;
    if (!leds || halfWidth <= 1)
        return;

    startY = std::max(0, startY);
    endY = std::min(MATRIX_HEIGHT - 1, endY);
    for (int y = startY; y <= endY; y++)
    {
        auto pLine = leds + y * MATRIX_WIDTH;
        auto pLine2 = pLine + halfWidth;
        memmove(pLine + 1, pLine, sizeof(CRGB) * (halfWidth - 1));
        memmove(pLine2, pLine2 + 1, sizeof(CRGB) * (halfWidth - 1));
    }
}

void HUB75GFX::MoveOutwardsX(int startY, int endY)
{
    const int halfWidth = MATRIX_WIDTH / 2;
    if (!leds || halfWidth <= 1)
        return;

    startY = std::max(0, startY);
    endY = std::min(MATRIX_HEIGHT - 1, endY);
    for (int y = startY; y <= endY; y++)
    {
        auto pLine = leds + y * MATRIX_WIDTH;
        auto pLine2 = pLine + halfWidth;
        memmove(pLine, pLine + 1, sizeof(CRGB) * (halfWidth - 1));
        memmove(pLine2 + 1, pLine2, sizeof(CRGB) * (halfWidth - 1));
    }
}

void HUB75GFX::StartMatrix()
{
    HUB75_I2S_CFG::i2s_pins pins = {
        R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
        A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN
    };

    // Chain length is however many HUB75_PANEL_RES_X-wide modules it takes to span
    // MATRIX_WIDTH; only horizontal (left-to-right) chaining is supported - each
    // module must be the full canvas height. See the HUB75_PANEL_RES_X/Y comment in
    // globals.h.
    static_assert(MATRIX_WIDTH % HUB75_PANEL_RES_X == 0,
                   "MATRIX_WIDTH must be an exact multiple of HUB75_PANEL_RES_X");
    static_assert(HUB75_PANEL_RES_Y == MATRIX_HEIGHT,
                   "HUB75_PANEL_RES_Y must equal MATRIX_HEIGHT - only horizontal panel chaining is supported");
    constexpr int kPanelChainLength = MATRIX_WIDTH / HUB75_PANEL_RES_X;

    HUB75_I2S_CFG config(HUB75_PANEL_RES_X, HUB75_PANEL_RES_Y, kPanelChainLength, pins);
    // The library's own DMA framebuffer is forced into internal SRAM (not
    // PSRAM - see ESP32-HUB75-MatrixPanel-I2S-DMA's rowBitStruct allocator),
    // and double_buff doubles it. At this board's 128x64 (4x the pixels of
    // the 64x32 config double_buff was tuned for), leaving it on starves the
    // onboard mic's I2S DMA init of internal RAM (ESP_ERR_NO_MEM abort in
    // SoundAnalyzerBase::InitI2S_Modern, confirmed on real hardware). Other
    // HUB75 targets (Mesmerizer and friends) have smaller matrices with
    // headroom to spare, so this is scoped to just this board rather than
    // disabled for everyone.
    #if defined(WAVESHARE_ESP32S3_RGB_MATRIX) && WAVESHARE_ESP32S3_RGB_MATRIX
        config.double_buff = false;
        // DMA descriptor count scales with pixel_color_depth_bits (each extra
        // bit needs more BCM passes per refresh - see setupDMA()'s
        // dma_descriptors_per_row calculation). The library's default of 8
        // bits, combined with min_refresh_rate forcing lsbMsbTransitionBit up
        // to hit 180Hz on this 4x-larger-than-typical matrix, left this board
        // with as little as 800 bytes of free DMA-capable RAM after boot -
        // confirmed on real hardware to wedge the WiFi/network stack (device
        // kept rendering, but became completely unreachable) after ~5 seconds
        // of sustained frame traffic from LED-Central. Dropping to 6 bits
        // (64 levels/channel instead of 256) cuts descriptor count enough to
        // give the rest of the system real headroom again; still smooth
        // enough for typical effects. Revisit if a real product needs more
        // color depth than RAM allows at this size.
        config.setPixelColorDepthBits(6);
    #else
        config.double_buff = true;
    #endif
    config.i2sspeed = HUB75_I2S_CFG::HZ_20M;
    config.min_refresh_rate = MATRIX_REFRESH_RATE;

    matrix = std::make_unique<MatrixPanel_I2S_DMA>(config);
    if (!matrix->begin())
        throw std::runtime_error("HUB75 DMA matrix initialization failed");

    matrix->setBrightness8(kDefaultBrightness);
    matrix->clearScreen();
    matrix->flipDMABuffer();
    matrix->clearScreen();

    std::fill_n(frameBuffers[0], kMatrixWidth * kMatrixHeight, CRGB::Black);
    std::fill_n(frameBuffers[1], kMatrixWidth * kMatrixHeight, CRGB::Black);
    lastSwapMs = millis();

    Serial.printf("Matrix Refresh Rate: %d\n", GetRefreshRate());
}

void HUB75GFX::PrepareFrame()
{
    EVERY_N_MILLIS(MILLIS_PER_FRAME)
    {
        auto& graphics = g_ptrSystem->GetEffectManager().g();
        static_cast<HUB75GFX&>(graphics).setLeds(GetMatrixBackBuffer());
    }
}

void HUB75GFX::PostProcessFrame(uint16_t localPixelsDrawn, uint16_t wifiPixelsDrawn)
{
    if (localPixelsDrawn + wifiPixelsDrawn == 0)
        return;

    auto& pMatrix = static_cast<HUB75GFX&>(g_ptrSystem->GetEffectManager().g());

    const auto& effectManager = g_ptrSystem->GetEffectManager();
    const bool showCaption = effectManager.HasCurrentEffect() &&
                             effectManager.GetCurrentEffect().ShouldShowTitle() &&
                             pMatrix.GetCaptionTransparency() > 0.0f;

    constexpr auto kCaptionPower = 500;
    g_Values.MatrixPowerMilliwatts = pMatrix.EstimatePowerDraw();
    if (showCaption)
        g_Values.MatrixPowerMilliwatts += kCaptionPower;

    const double kMaxPower = g_ptrSystem->GetDeviceConfig().GetPowerLimit();
    const uint8_t scaledBrightness = std::clamp(kMaxPower / g_Values.MatrixPowerMilliwatts, 0.0, 1.0) * 255;

    constexpr auto kWeightedAverageAmount = 10;
    if (scaledBrightness <= g_Values.MatrixScaledBrightness)
        g_Values.MatrixScaledBrightness = scaledBrightness;
    else
        g_Values.MatrixScaledBrightness = std::max(
            g_Values.MatrixScaledBrightness + 1,
            ((g_Values.MatrixScaledBrightness * (kWeightedAverageAmount - 1)) + scaledBrightness) / kWeightedAverageAmount);

    // The nightly schedule (see DeviceConfig::GetScheduleDimFactor255()) is applied here,
    // downstream of both local effects and LED-Central frames, so it can't be bypassed by
    // either source. The per-effect brightness override is a property of whichever effect
    // the rotation currently has selected (same "current effect" notion as showCaption
    // above), so it's meaningful only for locally rendered content.
    const auto& deviceConfig = g_ptrSystem->GetDeviceConfig();
    uint8_t effectBrightnessFactor255 = 255;
    if (effectManager.HasCurrentEffect() && effectManager.GetCurrentEffect().HasBrightnessOverride())
        effectBrightnessFactor255 = (uint8_t)std::lround(255.0 * effectManager.GetCurrentEffect().BrightnessOverride() / 100.0);

    const auto scheduledBrightness = scale8(scale8(deviceConfig.GetBrightness(), deviceConfig.GetScheduleDimFactor255()), effectBrightnessFactor255);
    const auto targetBrightness = min({
        scheduledBrightness,
        g_Values.Fader,
        g_Values.MatrixScaledBrightness
    });
    pMatrix.SetBrightness(targetBrightness);

    const bool requiresDoubleBuffering = effectManager.HasCurrentEffect() && effectManager.GetCurrentEffect().RequiresDoubleBuffering();
    MatrixSwapBuffers(wifiPixelsDrawn > 0 || requiresDoubleBuffering || showCaption);
    FastLED.countFPS();
}

CRGB* HUB75GFX::GetMatrixBackBuffer()
{
    for (auto& device : g_ptrSystem->GetDevices())
        device->UpdatePaletteCycle();
    return frameBuffers[drawBufferIndex];
}

void HUB75GFX::FlushFrameToMatrix()
{
    if (!matrix)
        return;

    const CRGB* frame = frameBuffers[drawBufferIndex];
    for (int y = 0; y < MATRIX_HEIGHT; ++y)
    {
        for (int x = 0; x < MATRIX_WIDTH; ++x)
        {
            const CRGB& pixel = frame[y * MATRIX_WIDTH + x];
            matrix->drawPixelRGB888(x, y, pixel.r, pixel.g, pixel.b);
        }
    }

    auto& gfx = static_cast<HUB75GFX&>(g_ptrSystem->GetEffectManager().g());
    const auto& effectManager = g_ptrSystem->GetEffectManager();
    const bool shouldShowTitle = effectManager.HasCurrentEffect() &&
                                 effectManager.GetCurrentEffect().ShouldShowTitle();
    const float captionAlpha = shouldShowTitle ? gfx.GetCaptionTransparency() : 0.0f;
    if (captionAlpha > 0.0f)
    {
        const String caption = gfx.GetCaption();
        constexpr int charWidth = 6;
        constexpr int charHeight = 8;
        const int textWidth = caption.length() * charWidth;
        const unsigned long elapsed = millis() - gfx.captionStartTime;
        const int x = textWidth > MATRIX_WIDTH
            ? MATRIX_WIDTH - static_cast<int>((elapsed / gfx.totalCaptionDuration) * (textWidth + MATRIX_WIDTH))
            : (MATRIX_WIDTH - textWidth) / 2;
        const int y = MATRIX_HEIGHT - charHeight - 1;
        const uint8_t brightness = static_cast<uint8_t>(captionAlpha * 255.0f);

        matrix->setTextWrap(false);
        matrix->setTextSize(1);
        matrix->setTextColor(matrix->color565(brightness, brightness, brightness));
        matrix->setCursor(x, y);
        matrix->print(caption);
    }

    #if SHOW_FPS_ON_MATRIX
        auto& taskManager = g_ptrSystem->GetTaskManager();
        matrix->setTextWrap(false);
        matrix->setTextSize(1);
        matrix->setTextColor(matrix->color565(255, 255, 255), matrix->color565(0, 0, 0));
        matrix->setCursor(0, MATRIX_HEIGHT - 16);
        matrix->printf("LED:%lu AUD:%d", static_cast<unsigned long>(g_Values.FPS), g_Analyzer.AudioFPS());
        matrix->setCursor(0, MATRIX_HEIGHT - 8);
        matrix->printf("C0:%d C1:%d", static_cast<int>(taskManager.GetCPUUsagePercent(0)), static_cast<int>(taskManager.GetCPUUsagePercent(1)));
    #endif
}

void HUB75GFX::MatrixSwapBuffers(bool copyPresentedFrame)
{
    if (!matrix || !WaitForMatrixSwap())
        return;

    const uint8_t presentedIndex = drawBufferIndex;
    FlushFrameToMatrix();
    matrix->flipDMABuffer();
    lastSwapMs = millis();

    drawBufferIndex ^= 1;
    if (copyPresentedFrame)
        memcpy(frameBuffers[drawBufferIndex], frameBuffers[presentedIndex], sizeof(frameBuffers[0]));
}

bool HUB75GFX::WaitForMatrixSwap(uint32_t timeoutMs)
{
    if (!matrix || GetRefreshRate() <= 0)
        return true;

    const uint32_t frameMs = std::max<uint32_t>(1, (1000U + GetRefreshRate() - 1) / GetRefreshRate());
    const uint32_t elapsed = millis() - lastSwapMs;
    if (elapsed >= frameMs)
        return true;

    const uint32_t waitMs = frameMs - elapsed;
    if (waitMs > timeoutMs)
        return false;

    delay(waitMs);
    return true;
}

#endif
