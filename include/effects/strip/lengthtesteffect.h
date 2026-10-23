#pragma once

//+--------------------------------------------------------------------------
//
// File:        lengthtesteffect.h
//
// NightDriverStrip - (c) 2026 Plummer's Software LLC.  All Rights Reserved.
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
//    Length Test: a ruler for finding each strip's exact LED count. On every channel the last
//    configured LED is full white, and the LEDs before it carry ruler ticks counted from LED 1
//    (the one nearest the controller): every 10th is 50% red, every other 5th is 30% blue,
//    the rest are off.
//
//    The length is read every frame, so changing a channel's strip length in the device
//    settings moves the white LED right away (topology applies live). If the white LED stops
//    short of the physical end, the strip is longer than configured, by the number of dark
//    LEDs past it. If no white LED is visible, the strip is shorter, and the ticks show how far
//    it gets.
//
//    It's a manual-only effect: rotation never switches to it or away from it, so it stays up
//    while you tune, until another effect is picked.
//
//---------------------------------------------------------------------------

#include "ledstripeffect.h"

class LengthTestEffect : public EffectWithId<LengthTestEffect>
{
  public:

    LengthTestEffect() : EffectWithId<LengthTestEffect>("Length Test") {}
    LengthTestEffect(const JsonObjectConst& jsonObject) : EffectWithId<LengthTestEffect>(jsonObject) {}

    bool IsManualOnly() const override { return true; }

    void Draw() override
    {
        static const CRGB kTenTick  = CRGB(128, 0, 0);     // 50% red
        static const CRGB kFiveTick = CRGB(0, 0, 77);      // 30% blue

        // Clears every channel at its own size, whites plane included
        setAllOnAllChannels(0, 0, 0);

        for (auto& device : _GFX)
        {
            const size_t count = device->GetLEDCount();
            if (count == 0)
                continue;

            for (size_t i = 0; i + 1 < count; ++i)
            {
                const size_t ledNumber = i + 1;
                if (ledNumber % 10 == 0)
                    device->setPixel(i, kTenTick);
                else if (ledNumber % 5 == 0)
                    device->setPixel(i, kFiveTick);
            }
            device->setPixel(count - 1, CRGB::White);
        }
    }
};
