#pragma once

//+--------------------------------------------------------------------------
//
// File:        hub75gfx.h
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of the NightDriver software project.
//
//    NightDriver is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    NightDriver is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Nightdriver.  It is normally found in copying.txt
//    If not, see <https://www.gnu.org/licenses/>.
//
//
// Description:
//
//   Provides a HUB75 GFX implementation for our RGB LED panel so that
//   we can use primitives such as lines and fills on it.
//
// History:     Oct-9-2018         Davepl      Created from other projects
//
//---------------------------------------------------------------------------

#include "globals.h"

#if USE_HUB75

#include "MatrixHardware_ESP32_Custom.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#include <cmath>
#include <memory>

#include "gfxbase.h"
#include "types.h"

//
// Matrix Panel
//

class HUB75GFX : public GFXBase
{
protected:
    String strCaption;
    unsigned long captionStartTime = 0;
    float captionDuration = 0;
    const float captionFadeInTime = 500;
    float captionFadeOutTime = 1000;
    float totalCaptionDuration = 0;

public:
    static const uint8_t kDefaultBrightness = 255; // full (100%) brightness

    static std::unique_ptr<MatrixPanel_I2S_DMA> matrix;

    HUB75GFX(size_t w, size_t h);

    ~HUB75GFX() override;

    static void InitializeHardware(std::vector<std::shared_ptr<GFXBase>>& devices);

    static void SetBrightness(byte amount);

    // EstimatePowerDraw
    //
    // Estimate the total power load for the board and matrix by walking the pixels and adding our previously measured
    // power draw per pixel based on what color and brightness each pixel is

    int EstimatePowerDraw();

    __attribute__((always_inline)) uint16_t xy(uint16_t x, uint16_t y) const noexcept override
    {
        // Note the x,y are unsigned so can't be less than zero
        if (x < _width && y < _height)
            return y * MATRIX_WIDTH + x;

        return 0;
    }

    // Whereas an WS281xGFX would track its own memory for the CRGB array, we simply point to the buffer already used for
    // the matrix display memory.  That also eliminated having a local draw buffer that is then copied, because the effects
    // can render directly to the right back buffer automatically.

    void setLeds(CRGB *pLeds);

    void fillLeds(const CRGB* pLEDs) override;

    void Clear(CRGB color = CRGB::Black) override;

    const String & GetCaption();

    float GetCaptionTransparency();

    void SetCaption(const String & str, uint32_t duration);

    void MoveInwardX(int startY = 0, int endY = MATRIX_HEIGHT - 1) override;

    void MoveOutwardsX(int startY = 0, int endY = MATRIX_HEIGHT - 1) override;

    // PrepareFrame
    //
    // Gets the matrix ready for the effect or wifi to render into

    void PrepareFrame() override;

    // PostProcessFrame
    //
    // Things we do with the matrix after rendering a frame, such as setting the brightness and swapping the backbuffer forward

    void PostProcessFrame(uint16_t localPixelsDrawn, uint16_t wifiPixelsDrawn) override;

    // Matrix interop

    static void StartMatrix();
    static int GetRefreshRate();
    static CRGB *GetMatrixBackBuffer();
    static bool WaitForMatrixSwap(uint32_t timeoutMs = 100);
    static void MatrixSwapBuffers(bool copyPresentedFrame);

    // TEMPORARY: MatrixSwapBuffers() accumulates these each call instead of
    // logging directly - it runs on the render task, and debugI() from there
    // was making the render task go silent forever the first time it fired
    // (or crash-looping it, judging by the display continuing to render
    // while every logging task-wide went dead). Read+reset from main.cpp's
    // already-safe periodic status print instead. Remove once the 32fps
    // ceiling investigation is done.
    struct SwapStats { uint32_t swaps, waitAvgUs, flushAvgUs, flipAvgUs; };
    static SwapStats GetAndResetSwapStats();

private:
    // PSRAM rather than a plain static array: at 192x64 (3 chained panels)
    // this pair costs 2 * 192 * 64 * sizeof(CRGB) = 72KB, which was enough to
    // push the board into an ESP_ERR_NO_MEM boot-crash loop competing with
    // the HUB75 library's own DMA descriptor buffer for the same scarce
    // internal RAM (confirmed live on real hardware after adding the third
    // panel). Only the library's own DMA buffer needs to be internal/DMA-
    // capable RAM - this is just CPU-side staging that FlushFrameToMatrix()
    // reads before pushing into that real DMA buffer via drawPixelRGB888(),
    // so PSRAM (16MB, barely touched) is free capacity for it.
    static allocated_unique_ptr<CRGB[]> frameBuffers[2];
    static uint8_t drawBufferIndex;
    static uint32_t lastSwapMs;
    static void FlushFrameToMatrix();
};
#endif
