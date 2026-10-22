#pragma once

//+--------------------------------------------------------------------------
//
// File:        cct_effects.h
//
// NightDriverStrip - (c) 2026 Plummer's Software LLC.  All Rights Reserved.
//
// Description:
//
//   Reference effects that drive the CCT whites plane (cool-white /
//   warm-white) directly via GFXBase::setPixelCCT(). Useful as a known-
//   good test on 4-channel SK6812 strips and future 5-channel SM16825/WS2805
//   strips. On plain 3-channel WS2812 builds setPixelCCT() is a no-op
//   (whites plane isn't allocated), so the effect falls back to drawing
//   a Kelvin-tinted CRGB approximation through the RGB channels - the strip
//   stays useful even where the dedicated white LED isn't present.
//
//---------------------------------------------------------------------------

#include "effectsupport.h"

#include <algorithm>

// WarmGlowEffect
//
// Lights every pixel with a configurable color temperature and brightness.
// The default 4000K is neutral-ish white. On 4-channel SK6812 RGBW hardware
// Kelvin only expresses intent: both CW and WW components collapse into the
// one physical W channel in PixelFormat, so 2700K and 6500K are not distinct
// unless RGB fallback is being used. Future 5-channel formats can render the
// CW/WW split directly.
//
// This effect is intended as a maximum-output white scene. It fills RGB with
// the brightest RGB approximation of the requested CCT and, when a whites
// plane exists, adds the brightest CCT-correct CW/WW mix the hardware can
// represent.
//
// Check your power supplies and power limits!  Monitor temperatures and 
// current draw when running this effect, especially at high brightness levels.  
// It can easily draw more current than the strip or power supply can handle, 
// which can cause damage.  Start with low brightness and work your way up 
// while monitoring the strip's behavior and temperature.

class WarmGlowEffect : public EffectWithId<WarmGlowEffect>
{
protected:

    uint16_t _kelvin     = 4000;     // default: neutral-ish single-white intent
    uint8_t  _brightness = 255;      // 0..255 scene intensity

    static String NameForKelvin(uint16_t kelvin)
    {
        // Return a string of the format "Warm White: 3000K" based on the temp

        String result("White");
        if (kelvin <= 3000)
            result = "Warm White";
        if (kelvin >= 4500)
            result = "Cool White";
        
        return result + ": " + String(kelvin) + "K";
    }

public:

    WarmGlowEffect()
        : EffectWithId<WarmGlowEffect>(NameForKelvin(_kelvin)) {}

    WarmGlowEffect(uint16_t kelvin, uint8_t brightness)
        : EffectWithId<WarmGlowEffect>(NameForKelvin(kelvin)),
          _kelvin(kelvin),
          _brightness(brightness) {}

    WarmGlowEffect(const JsonObjectConst& jsonObject)
        : EffectWithId<WarmGlowEffect>(jsonObject),
          _kelvin    (jsonObject["kelvin"]     | 4000),
          _brightness(jsonObject["brightness"] |  255)
    {
    }

    bool SerializeToJSON(JsonObject& jsonObject) override
    {
        auto jsonDoc = CreateJsonDocument();
        JsonObject root = jsonDoc.to<JsonObject>();
        LEDStripEffect::SerializeToJSON(root);
        jsonDoc["kelvin"]     = _kelvin;
        jsonDoc["brightness"] = _brightness;
        return SetIfNotOverflowed(jsonDoc, jsonObject, __PRETTY_FUNCTION__);
    }

    void Draw() override
    {
        const CRGB rgb = GFXBase::MaximumRgbForKelvin(_kelvin, _brightness);
        const CRGBW white = GFXBase::MaximumWhiteForKelvin(_kelvin, _brightness);

        // Each channel's own LED count, not _cLEDs (channel 0's): with individual strips of
        // different lengths, filling a shorter channel to channel 0's length ran off the end of
        // its buffer and corrupted the heap (deck1, 210/210/210/201 LEDs, panicked switching to
        // this effect).
        for (auto& device : _GFX)
        {
            const size_t ledCount = device->GetLEDCount();
            fill_solid(device->leds, ledCount, rgb);
            if (device->whites)
            {
                for (size_t i = 0; i < ledCount; ++i)
                    device->whites[i] = white;
            }
        }
    }

    size_t DesiredFramesPerSecond() const override { return 5; }   // static scene; no need to redraw fast
};
