#pragma once

//+--------------------------------------------------------------------------
//
// File:        ledcentraleffects.h
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
//    Effects ported from LED-Central (the server that drives NightDriver controllers over
//    WiFi), so a controller shows the same looks on its own: CandyCane, Halloween, and
//    LED-Central's Fire. Each channel is drawn as its own strip, with its own length and LED
//    density, the way LED-Central draws each parallel strip. Sizes and speeds are tuned in LEDs
//    on a 60 LED/m strip and scaled to the channel's configured density.
//
//    The per-LED loops stay in integer/fixed-point math: the ESP32-C5 these mostly run on has
//    no FPU, and soft-float per LED across four long strips would eat the frame budget. That
//    is also why Halloween's glow uses FastLED's inoise8() instead of LED-Central's own value
//    noise - same idea, slightly different texture.
//
//    Not ported: Fire's neighbor blend between strips. A controller's channels aren't
//    necessarily neighbors on the wall (Deck11 interleaves three controllers), and heat
//    can't be shared between controllers anyway.
//
//---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <vector>

#include "ledstripeffect.h"
#include "random_utils.h"
#include "systemcontainer.h"
#include "types.h"

namespace LEDCentral
{
    constexpr float kReferenceLedsPerMeter = 60.0f;

    // Channels that are both configured and backed by a GFX device.
    inline size_t ActiveChannels(const std::vector<std::shared_ptr<GFXBase>>& gfx)
    {
        return std::min(gfx.size(), g_ptrSystem->GetDeviceConfig().GetChannelCount());
    }

    // How much bigger a size tuned in LEDs at 60 LED/m is on this channel (1 if its density
    // isn't configured).
    inline float DensityScale(size_t channel)
    {
        const auto ledsPerMeter = g_ptrSystem->GetDeviceConfig().GetChannelLEDsPerMeter(channel);
        return ledsPerMeter ? ledsPerMeter / kReferenceLedsPerMeter : 1.0f;
    }

    // Same integer hash and generator as LED-Central's hashInts()/mulberry32(), for the
    // time-slot-keyed choices (eye cycles, lightning slots) that must come out the same on
    // every frame of a slot.
    inline uint32_t HashInts(uint32_t a, uint32_t b, uint32_t c)
    {
        uint32_t h = (a * 0x27d4eb2du) ^ (b * 0x165667b1u) ^ (c * 0x9e3779b1u);
        h = (h ^ (h >> 15)) * 0x85ebca6bu;
        h = (h ^ (h >> 13)) * 0xc2b2ae35u;
        return h ^ (h >> 16);
    }

    struct Mulberry32
    {
        uint32_t state;
        explicit Mulberry32(uint32_t seed) : state(seed) {}

        float Next()
        {
            uint32_t t = (state += 0x6d2b79f5u);
            t = (t ^ (t >> 15)) * (t | 1);
            t ^= t + ((t ^ (t >> 7)) * (t | 61));
            return ((t ^ (t >> 14))) / 4294967296.0f;
        }
    };

    // 0..255 smoothstep of value across [edge0, edge1], all in 0..255 units.
    inline uint8_t SmoothStep8(int edge0, int edge1, int value)
    {
        if (value <= edge0) return 0;
        if (value >= edge1) return 255;
        return ease8InOutCubic(static_cast<uint8_t>((value - edge0) * 255 / (edge1 - edge0)));
    }

    // Seconds into the current day of NTP-synced wall time, so every controller agrees on it
    // (and the float keeps its precision, unlike a full epoch timestamp).
    inline double WallSecondsToday()
    {
        return std::fmod(CAppTime::CurrentTime(), 86400.0);
    }
}

// ---------------------------------------------------------------------------------------------
// CandyCane
//
// A candy cane spinning about its own long axis: white, with a wide green and a narrow red stripe
// wound around it as a helix, so spinning it makes the stripes climb the strip. Each channel
// views the cane from a different angle around it (its share of viewSpread, plus viewOffset), so
// the stripes step diagonally across strips. The spin follows NTP wall time, so canes on
// different controllers turn in lockstep; give each controller its own viewOffset to continue
// the pattern across them.
//
// Phase is a uint32 where 2^32 is one full twist, so it wraps for free.
// ---------------------------------------------------------------------------------------------

class CandyCaneEffect : public EffectWithId<CandyCaneEffect>
{
    float twistsPerSecond = 0.5f;
    int   twistLength = 40;            // LEDs per twist on a 60 LED/m strip
    int   greenWidth = 30;             // percent of a twist
    int   redWidth = 8;                // percent of a twist
    int   viewSpread = 360;            // degrees spread across this controller's channels
    int   viewOffset = 0;              // degrees added to every channel's view
    CRGB  whiteColor = CRGB(255, 255, 255);
    CRGB  greenColor = CRGB(0, 192, 32);
    CRGB  redColor = CRGB(255, 0, 0);
    bool  reversed = false;

    DECLARE_EFFECT_SETTING_SPECS(mySettingSpecs);

    EffectSettingSpecs* FillSettingSpecs() override
    {
        if (mySettingSpecs.size() == 0)
        {
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(twistsPerSecond), .FriendlyName = "Twists per second",
                .Description = "How fast the cane spins.",
                .Type = SettingSpec::SettingType::Float, .MinimumValue = 0.0, .MaximumValue = 5.0 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(twistLength), .FriendlyName = "Twist length",
                .Description = "LEDs per full twist of the stripes on a 60 LED/m strip, scaled to each strip's density.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 4, .MaximumValue = 300 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(greenWidth), .FriendlyName = "Green stripe width",
                .Description = "Percent of each twist that is green.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 60 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(redWidth), .FriendlyName = "Red stripe width",
                .Description = "Percent of each twist that is red.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 30 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(viewSpread), .FriendlyName = "View spread",
                .Description = "Degrees around the cane this controller's strips are spread over. 0 shows the same view on every strip.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 360 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(viewOffset), .FriendlyName = "View offset",
                .Description = "Degrees added to every strip's view, to continue the pattern from one controller to the next.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 359 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(whiteColor), .FriendlyName = "White color",
                .Description = "Color of the cane between the stripes.", .Type = SettingSpec::SettingType::Color }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(greenColor), .FriendlyName = "Green color",
                .Description = "Color of the wide stripe.", .Type = SettingSpec::SettingType::Color }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(redColor), .FriendlyName = "Red color",
                .Description = "Color of the narrow stripe.", .Type = SettingSpec::SettingType::Color }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(reversed), .FriendlyName = "Reversed",
                .Description = "Climb toward LED 0 instead of away from it.", .Type = SettingSpec::SettingType::Boolean }));
        }
        return &mySettingSpecs;
    }

  public:

    CandyCaneEffect() : EffectWithId<CandyCaneEffect>("CandyCane") {}

    CandyCaneEffect(const JsonObjectConst& jsonObject) : EffectWithId<CandyCaneEffect>(jsonObject)
    {
        FieldAccess::AssignIfPresent(jsonObject, "tps", twistsPerSecond);
        FieldAccess::AssignIfPresent(jsonObject, "tl", twistLength);
        FieldAccess::AssignIfPresent(jsonObject, "gw", greenWidth);
        FieldAccess::AssignIfPresent(jsonObject, "rw", redWidth);
        FieldAccess::AssignIfPresent(jsonObject, "vs", viewSpread);
        FieldAccess::AssignIfPresent(jsonObject, "vo", viewOffset);
        FieldAccess::AssignIfPresent(jsonObject, "wc", whiteColor);
        FieldAccess::AssignIfPresent(jsonObject, "gc", greenColor);
        FieldAccess::AssignIfPresent(jsonObject, "rc", redColor);
        FieldAccess::AssignIfPresent(jsonObject, "rev", reversed);
    }

    bool SerializeToJSON(JsonObject& jsonObject) override
    {
        auto jsonDoc = CreateJsonDocument();
        JsonObject root = jsonDoc.to<JsonObject>();
        LEDStripEffect::SerializeToJSON(root);

        jsonDoc["tps"] = twistsPerSecond;
        jsonDoc["tl"] = twistLength;
        jsonDoc["gw"] = greenWidth;
        jsonDoc["rw"] = redWidth;
        jsonDoc["vs"] = viewSpread;
        jsonDoc["vo"] = viewOffset;
        jsonDoc["wc"] = whiteColor;
        jsonDoc["gc"] = greenColor;
        jsonDoc["rc"] = redColor;
        jsonDoc["rev"] = reversed;

        return SetIfNotOverflowed(jsonDoc, jsonObject, __PRETTY_FUNCTION__);
    }

    bool SerializeSettingsToJSON(JsonObject& jsonObject) override
    {
        auto jsonDoc = CreateJsonDocument();
        JsonObject root = jsonDoc.to<JsonObject>();
        LEDStripEffect::SerializeSettingsToJSON(root);

        jsonDoc[NAME_OF(twistsPerSecond)] = twistsPerSecond;
        jsonDoc[NAME_OF(twistLength)] = twistLength;
        jsonDoc[NAME_OF(greenWidth)] = greenWidth;
        jsonDoc[NAME_OF(redWidth)] = redWidth;
        jsonDoc[NAME_OF(viewSpread)] = viewSpread;
        jsonDoc[NAME_OF(viewOffset)] = viewOffset;
        jsonDoc[NAME_OF(whiteColor)] = whiteColor;
        jsonDoc[NAME_OF(greenColor)] = greenColor;
        jsonDoc[NAME_OF(redColor)] = redColor;
        jsonDoc[NAME_OF(reversed)] = reversed;

        return SetIfNotOverflowed(jsonDoc, jsonObject, __PRETTY_FUNCTION__);
    }

    bool SetSetting(const String& name, const String& value) override
    {
        return FieldAccess::AssignIfSelected(name, NAME_OF(twistsPerSecond), twistsPerSecond, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(twistLength), twistLength, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(greenWidth), greenWidth, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(redWidth), redWidth, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(viewSpread), viewSpread, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(viewOffset), viewOffset, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(whiteColor), whiteColor, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(greenColor), greenColor, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(redColor), redColor, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(reversed), reversed, value)
            || LEDStripEffect::SetSetting(name, value);
    }

    void Draw() override
    {
        constexpr double kTwist = 4294967296.0;     // 2^32 = one full twist
        constexpr int kSamples = 4;                 // per LED, so stripe edges glide

        const uint32_t spin = static_cast<uint32_t>(std::fmod(LEDCentral::WallSecondsToday() * std::clamp(twistsPerSecond, 0.0f, 5.0f), 1.0) * kTwist);
        const uint32_t greenEnd = static_cast<uint32_t>(std::clamp(greenWidth, 0, 60) / 100.0 * kTwist);
        const uint32_t redSpan = static_cast<uint32_t>(std::clamp(redWidth, 0, 30) / 100.0 * kTwist);
        // The red stripe sits centered in the white gap between greens.
        const uint32_t redStart = greenEnd + static_cast<uint32_t>((kTwist - greenEnd - redSpan) / 2);
        const size_t channels = LEDCentral::ActiveChannels(_GFX);

        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto& gfx = *_GFX[ch];
            const int count = gfx.GetLEDCount();
            const float twistCells = std::max(1.0f, std::clamp(twistLength, 4, 300) * LEDCentral::DensityScale(ch));
            const uint32_t step = static_cast<uint32_t>(kTwist / twistCells);
            const double viewTurn = double(ch) / channels * std::clamp(viewSpread, 0, 360) / 360.0 + std::clamp(viewOffset, 0, 359) / 360.0;
            const uint32_t view = static_cast<uint32_t>(std::fmod(viewTurn, 1.0) * kTwist);

            for (int i = 0; i < count; ++i)
            {
                const uint32_t along = static_cast<uint32_t>(reversed ? count - 1 - i : i) * step;
                uint16_t r = 0, g = 0, b = 0;
                for (int s = 0; s < kSamples; ++s)
                {
                    // Sample points at (s + 0.5) / kSamples across the LED.
                    const uint32_t phase = along + (step / (2 * kSamples)) * (2 * s + 1) - spin + view;
                    const CRGB& c = phase < greenEnd ? greenColor
                                  : (phase >= redStart && phase - redStart < redSpan) ? redColor
                                  : whiteColor;
                    r += c.r; g += c.g; b += c.b;
                }
                gfx.setPixel(i, CRGB(r / kSamples, g / kSamples, b / kSamples));
            }
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Halloween
//
// Orange and black, in three layers: jack-o'-lantern glow (noise thresholded into drifting pools
// of flickering orange that fade to ember red, black between), creepy eyes (a pair opens
// somewhere dark, stares, blinks once, fades), and lightning (short random multi-flash strikes).
//
// Credits: the eyes and lightning are inspired by WLED's "Halloween Eyes" and "Lightning"
// effects (wled00/FX.cpp in https://github.com/wled/WLED, whose effects code builds on Harm
// Aldick's WS2812FX library). The default glow orange (255,90,0) is Trimlight's "Jack-O-Lantern
// Glow" (https://trimlight.com/blog/trimlight-halloween-patterns). No code is taken from either.
//
// Glow and eyes are seeded per channel from RandomU32() at Start(), so they follow the
// true-random / pseudo-random device setting. Lightning is keyed off NTP wall time with a fixed
// seed instead, so every controller running this effect strikes at the same moment.
// ---------------------------------------------------------------------------------------------

class HalloweenEffect : public EffectWithId<HalloweenEffect>
{
    CRGB  glowColor = CRGB(255, 90, 0);
    int   glowAmount = 55;             // percent of the strip that glows
    int   glowSize = 16;               // LEDs per noise cell on a 60 LED/m strip
    float glowSpeed = 0.4f;            // noise cells per second
    int   eyeCount = 1;                // pairs that can watch at once, per channel
    CRGB  eyeColor = CRGB(127, 255, 0);
    int   eyeSize = 2;                 // LEDs per eye on a 60 LED/m strip
    int   eyeGap = 4;                  // LEDs between the eyes on a 60 LED/m strip
    int   lightningPerMinute = 3;
    CRGB  lightningColor = CRGB(216, 204, 255);

    uint32_t baseSeed = 0;
    unsigned long startMillis = 0;

    static constexpr float kEyeFadeIn = 0.35f;
    static constexpr float kEyeFadeOut = 0.6f;
    static constexpr float kEyeBlink = 0.16f;
    static constexpr int   kEyeDarkMargin = 2;
    static constexpr double kLightningSlotSeconds = 2.0;
    static constexpr uint32_t kLightningSeed = 0x11e7;

    DECLARE_EFFECT_SETTING_SPECS(mySettingSpecs);

    EffectSettingSpecs* FillSettingSpecs() override
    {
        if (mySettingSpecs.size() == 0)
        {
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(glowColor), .FriendlyName = "Glow color",
                .Description = "Color of the jack-o'-lantern pools.", .Type = SettingSpec::SettingType::Color }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(glowAmount), .FriendlyName = "Glow amount",
                .Description = "Percent of the strip that glows; the rest is black.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 100 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(glowSize), .FriendlyName = "Glow size",
                .Description = "Size of the glowing pools, in LEDs on a 60 LED/m strip.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 2, .MaximumValue = 100 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(glowSpeed), .FriendlyName = "Glow speed",
                .Description = "How fast the pools drift.",
                .Type = SettingSpec::SettingType::Float, .MinimumValue = 0.0, .MaximumValue = 5.0 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(eyeCount), .FriendlyName = "Eye pairs",
                .Description = "Pairs of eyes that can watch at once on each strip; 0 for none.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 6 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(eyeColor), .FriendlyName = "Eye color",
                .Description = "Color of the eyes.", .Type = SettingSpec::SettingType::Color }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(eyeSize), .FriendlyName = "Eye size",
                .Description = "LEDs per eye on a 60 LED/m strip.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 1, .MaximumValue = 6 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(eyeGap), .FriendlyName = "Eye gap",
                .Description = "Dark LEDs between the two eyes on a 60 LED/m strip.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 1, .MaximumValue = 12 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(lightningPerMinute), .FriendlyName = "Lightning per minute",
                .Description = "Average lightning strikes per minute; 0 turns lightning off.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 30 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(lightningColor), .FriendlyName = "Lightning color",
                .Description = "Color of the lightning flash.", .Type = SettingSpec::SettingType::Color }));
        }
        return &mySettingSpecs;
    }

    // 0..1 brightness of any lightning flash at wall time `seconds`: 2s slots, each striking with
    // probability lightningPerMinute/30 as 2-4 quick flashes of falling strength and a short
    // afterglow. A strike can run past its own slot, so the previous slot is checked too. Flashes
    // last at least 80ms so a frame can't fall between them.
    float LightningIntensity(double seconds) const
    {
        const int perMinute = std::clamp(lightningPerMinute, 0, 30);
        if (perMinute == 0)
            return 0.0f;

        const float chance = std::min(1.0, perMinute * kLightningSlotSeconds / 60.0);
        const auto slot = static_cast<int64_t>(std::floor(seconds / kLightningSlotSeconds));
        float intensity = 0.0f;
        for (int64_t strikeSlot = slot - 1; strikeSlot <= slot; ++strikeSlot)
        {
            LEDCentral::Mulberry32 rng(LEDCentral::HashInts(kLightningSeed, static_cast<uint32_t>(strikeSlot), 0xb017));
            if (rng.Next() >= chance)
                continue;
            double at = strikeSlot * kLightningSlotSeconds + rng.Next() * 1.2;
            const int flashes = 2 + static_cast<int>(rng.Next() * 3);
            const float peak = 0.75f + rng.Next() * 0.25f;
            for (int flash = 0; flash < flashes; ++flash)
            {
                const double lasts = 0.08 + rng.Next() * 0.06;
                if (seconds >= at && seconds < at + lasts)
                    intensity = std::max(intensity, peak * (1.0f - flash * 0.2f));
                at += lasts + 0.06 + rng.Next() * 0.14;
            }
            const double afterglow = (seconds - at) / 0.4;
            if (afterglow >= 0.0 && afterglow < 1.0)
                intensity = std::max(intensity, static_cast<float>(0.25f * peak * (1.0 - afterglow)));
        }
        return intensity;
    }

    // Each of the eyeCount watchers runs its own repeating cycle (length and phase fixed per
    // watcher, so they don't open in lockstep). In each cycle it may open a pair of eyes at a
    // random spot: they fade in while the darkness around them deepens, stare, blink once, then
    // fade out.
    void DrawEyes(GFXBase& gfx, int count, float densityScale, uint32_t seed, float seconds) const
    {
        const int size = std::max(1, static_cast<int>(std::lround(std::clamp(eyeSize, 1, 6) * densityScale)));
        const int gap = std::max(1, static_cast<int>(std::lround(std::clamp(eyeGap, 1, 12) * densityScale)));
        const int pairWidth = size * 2 + gap;
        if (count < pairWidth + kEyeDarkMargin * 2)
            return;

        for (int watcher = 0; watcher < std::clamp(eyeCount, 0, 6); ++watcher)
        {
            LEDCentral::Mulberry32 setup(LEDCentral::HashInts(seed, watcher, 0x3e7e));
            const float cycleSeconds = 4.5f + setup.Next() * 3.0f;
            const float shifted = seconds + setup.Next() * cycleSeconds;
            const auto cycle = static_cast<uint32_t>(shifted / cycleSeconds);
            const float intoCycle = shifted - cycle * cycleSeconds;

            LEDCentral::Mulberry32 rng(LEDCentral::HashInts(seed, watcher, cycle));
            if (rng.Next() > 0.75f)
                continue;
            const float openSeconds = 1.5f + rng.Next() * (cycleSeconds - 2.5f);
            const float opensAt = rng.Next() * (cycleSeconds - openSeconds);
            const float blinkAt = 0.5f + rng.Next() * (openSeconds - 1.2f);
            const int left = kEyeDarkMargin + static_cast<int>(rng.Next() * (count - pairWidth - kEyeDarkMargin * 2 + 1));

            const float open = intoCycle - opensAt;
            if (open < 0.0f || open > openSeconds)
                continue;
            const float darkness = std::min({ 1.0f, open / kEyeFadeIn, (openSeconds - open) / kEyeFadeOut });
            // The darkness stays while the eyes blink shut.
            const float level = std::fabs(open - blinkAt) < kEyeBlink / 2 ? 0.0f : darkness;
            const uint8_t keep = 255 - static_cast<uint8_t>(darkness * 255);
            const uint8_t eyeLevel = static_cast<uint8_t>(level * 255);

            for (int x = left - kEyeDarkMargin; x < left + pairWidth + kEyeDarkMargin; ++x)
            {
                CRGB pixel = gfx.leds[x];
                pixel.nscale8(keep);
                const bool inEye = x >= left && x < left + pairWidth && (x < left + size || x >= left + size + gap);
                gfx.setPixel(x, inEye ? blend(pixel, eyeColor, eyeLevel) : pixel);
            }
        }
    }

  public:

    HalloweenEffect() : EffectWithId<HalloweenEffect>("Halloween") {}

    HalloweenEffect(const JsonObjectConst& jsonObject) : EffectWithId<HalloweenEffect>(jsonObject)
    {
        FieldAccess::AssignIfPresent(jsonObject, "gc", glowColor);
        FieldAccess::AssignIfPresent(jsonObject, "ga", glowAmount);
        FieldAccess::AssignIfPresent(jsonObject, "gsz", glowSize);
        FieldAccess::AssignIfPresent(jsonObject, "gsp", glowSpeed);
        FieldAccess::AssignIfPresent(jsonObject, "ec", eyeCount);
        FieldAccess::AssignIfPresent(jsonObject, "ecl", eyeColor);
        FieldAccess::AssignIfPresent(jsonObject, "esz", eyeSize);     // not "es": LEDStripEffect's enabled flag
        FieldAccess::AssignIfPresent(jsonObject, "eg", eyeGap);
        FieldAccess::AssignIfPresent(jsonObject, "lpm", lightningPerMinute);
        FieldAccess::AssignIfPresent(jsonObject, "lc", lightningColor);
    }

    bool SerializeToJSON(JsonObject& jsonObject) override
    {
        auto jsonDoc = CreateJsonDocument();
        JsonObject root = jsonDoc.to<JsonObject>();
        LEDStripEffect::SerializeToJSON(root);

        jsonDoc["gc"] = glowColor;
        jsonDoc["ga"] = glowAmount;
        jsonDoc["gsz"] = glowSize;
        jsonDoc["gsp"] = glowSpeed;
        jsonDoc["ec"] = eyeCount;
        jsonDoc["ecl"] = eyeColor;
        jsonDoc["esz"] = eyeSize;
        jsonDoc["eg"] = eyeGap;
        jsonDoc["lpm"] = lightningPerMinute;
        jsonDoc["lc"] = lightningColor;

        return SetIfNotOverflowed(jsonDoc, jsonObject, __PRETTY_FUNCTION__);
    }

    bool SerializeSettingsToJSON(JsonObject& jsonObject) override
    {
        auto jsonDoc = CreateJsonDocument();
        JsonObject root = jsonDoc.to<JsonObject>();
        LEDStripEffect::SerializeSettingsToJSON(root);

        jsonDoc[NAME_OF(glowColor)] = glowColor;
        jsonDoc[NAME_OF(glowAmount)] = glowAmount;
        jsonDoc[NAME_OF(glowSize)] = glowSize;
        jsonDoc[NAME_OF(glowSpeed)] = glowSpeed;
        jsonDoc[NAME_OF(eyeCount)] = eyeCount;
        jsonDoc[NAME_OF(eyeColor)] = eyeColor;
        jsonDoc[NAME_OF(eyeSize)] = eyeSize;
        jsonDoc[NAME_OF(eyeGap)] = eyeGap;
        jsonDoc[NAME_OF(lightningPerMinute)] = lightningPerMinute;
        jsonDoc[NAME_OF(lightningColor)] = lightningColor;

        return SetIfNotOverflowed(jsonDoc, jsonObject, __PRETTY_FUNCTION__);
    }

    bool SetSetting(const String& name, const String& value) override
    {
        return FieldAccess::AssignIfSelected(name, NAME_OF(glowColor), glowColor, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(glowAmount), glowAmount, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(glowSize), glowSize, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(glowSpeed), glowSpeed, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(eyeCount), eyeCount, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(eyeColor), eyeColor, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(eyeSize), eyeSize, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(eyeGap), eyeGap, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(lightningPerMinute), lightningPerMinute, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(lightningColor), lightningColor, value)
            || LEDStripEffect::SetSetting(name, value);
    }

    void Start() override
    {
        baseSeed = RandomU32();
        startMillis = millis();
    }

    void Draw() override
    {
        const float seconds = (millis() - startMillis) / 1000.0f;
        const int amount = std::clamp(glowAmount, 0, 100);
        // inoise8() lands mostly around 128, so the pool threshold sweeps that middle range as
        // glowAmount goes from all-dark to all-lit.
        const int threshold = 128 + (50 - amount) * 8 / 5;
        const CRGB ember = CRGB(scale8(glowColor.r, 140), scale8(glowColor.g, 31), scale8(glowColor.b, 26));
        const uint16_t drift = static_cast<uint16_t>(seconds * std::clamp(glowSpeed, 0.0f, 5.0f) * 256.0f);
        const uint16_t flickerTime = static_cast<uint16_t>(seconds * 9.0f * 256.0f);
        const size_t channels = LEDCentral::ActiveChannels(_GFX);

        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto& gfx = *_GFX[ch];
            const int count = gfx.GetLEDCount();
            const float densityScale = LEDCentral::DensityScale(ch);
            const uint32_t seed = baseSeed ^ (ch * 0x9e3779b9u);
            // Noise coordinates are 8.8 fixed point: 256 = one noise cell.
            const uint16_t cellStep = static_cast<uint16_t>(256.0f / std::max(1.0f, std::clamp(glowSize, 2, 100) * densityScale));
            const uint16_t lane = static_cast<uint16_t>(seed);     // each channel its own noise row
            const uint16_t evolve = lane + drift * 3 / 5;

            for (int i = 0; i < count; ++i)
            {
                uint8_t level = 0;
                if (amount == 100)
                    level = 255;
                else if (amount > 0)
                    level = LEDCentral::SmoothStep8(threshold - 30, threshold + 30, inoise8(i * cellStep - drift, evolve));

                if (level == 0)
                {
                    gfx.setPixel(i, CRGB::Black);
                    continue;
                }
                const uint8_t flicker = 191 + (inoise8(i * 85, flickerTime + lane) >> 2);
                CRGB color = blend(ember, glowColor, level);
                color.nscale8(scale8(level, flicker));
                gfx.setPixel(i, color);
            }

            DrawEyes(gfx, count, densityScale, seed, seconds);
        }

        const float flash = LightningIntensity(LEDCentral::WallSecondsToday());
        if (flash > 0.0f)
        {
            const uint8_t amount8 = static_cast<uint8_t>(flash * 255);
            for (size_t ch = 0; ch < channels; ++ch)
            {
                auto& gfx = *_GFX[ch];
                for (size_t i = 0; i < gfx.GetLEDCount(); ++i)
                    gfx.leds[i] = blend(gfx.leds[i], lightningColor, amount8);
            }
        }
    }
};

// ---------------------------------------------------------------------------------------------
// LED-Central's Fire
//
// FireEffect's heat automaton (per-cell cooling, heat drifting toward LED 0 from the next two
// cells, sparks at the far end), as LED-Central extended it: all three steps every 20ms; a
// height envelope that slowly breathes the flame between minHeight and maxHeight, resting near
// averageHeight, so a long strip isn't always lit end to end; red, green or blue palettes; and a
// simulation sized to each channel's physical length (supersampled on short strips, then box-
// filtered back down), so a 30 LED/m and a 60 LED/m strip burn alike.
//
// The flame's base is at LED 0 (the controller end); reversed puts it at the far end. Mirrored
// burns from the middle outward, and mirrored plus reversed from both ends - LED-Central's (and
// FireEffect's) convention, so the same settings look the same on both.
// ---------------------------------------------------------------------------------------------

class LEDCentralFireEffect : public EffectWithId<LEDCentralFireEffect>
{
    int    cooling = 3;
    int    sparking = 100;
    int    sparks = 3;
    int    sparkHeight = 4;
    String palette = "red";
    bool   reversed = false;
    bool   mirrored = false;
    float  minHeight = 0.2f;
    float  averageHeight = 0.7f;
    float  maxHeight = 1.0f;
    float  heightWaveSeconds = 120.0f;

    struct Channel
    {
        std::vector<uint8_t> heat;
        uint32_t seed = 0;
    };
    std::vector<Channel> channelState;
    unsigned long startMillis = 0;
    uint32_t stepsDone = 0;

    static constexpr int kStepMs = 20;
    static constexpr int kMaxCatchUpSteps = 10;
    // Same as LED-Central: a flame axis this short (or sparse) is simulated at 256 cells.
    static constexpr int kSupersampleMinCells = 64;
    static constexpr int kSupersampleCells = 256;

    DECLARE_EFFECT_SETTING_SPECS(mySettingSpecs);

    EffectSettingSpecs* FillSettingSpecs() override
    {
        if (mySettingSpecs.size() == 0)
        {
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(cooling), .FriendlyName = "Cooling",
                .Description = "How fast the flame cools; higher makes a shorter, calmer fire.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 100 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(sparking), .FriendlyName = "Sparking",
                .Description = "Chance (out of 255) of each spark attempt igniting.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 255 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(sparks), .FriendlyName = "Sparks",
                .Description = "Spark attempts every 20ms.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 0, .MaximumValue = 20 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(sparkHeight), .FriendlyName = "Spark height",
                .Description = "How far from the base a spark can start, in simulation cells.",
                .Type = SettingSpec::SettingType::Integer, .MinimumValue = 1, .MaximumValue = 20 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(palette), .FriendlyName = "Palette",
                .Description = "Color family of the flame as it cools.",
                .Type = SettingSpec::SettingType::String,
                .Widget = SettingSpec::WidgetKind::Select,
                .OptionValues = { "red", "green", "blue" },
                .OptionLabels = { "Red (white/yellow/red)", "Green (white/yellow/green)", "Blue (white/cyan/blue)" } }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(reversed), .FriendlyName = "Reversed",
                .Description = "Burn from the far end of the strip instead of LED 0.", .Type = SettingSpec::SettingType::Boolean }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(mirrored), .FriendlyName = "Mirrored",
                .Description = "Burn from the middle outward; with Reversed, from both ends toward the middle.", .Type = SettingSpec::SettingType::Boolean }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(minHeight), .FriendlyName = "Minimum height",
                .Description = "Lowest the flame breathes down to, as a fraction of the strip.",
                .Type = SettingSpec::SettingType::Float, .MinimumValue = 0.0, .MaximumValue = 1.0 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(averageHeight), .FriendlyName = "Average height",
                .Description = "Height the flame mostly rests near, as a fraction of the strip.",
                .Type = SettingSpec::SettingType::Float, .MinimumValue = 0.0, .MaximumValue = 1.0 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(maxHeight), .FriendlyName = "Maximum height",
                .Description = "Highest the flame breathes up to, as a fraction of the strip.",
                .Type = SettingSpec::SettingType::Float, .MinimumValue = 0.0, .MaximumValue = 1.0 }));
            mySettingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                .Name = NAME_OF(heightWaveSeconds), .FriendlyName = "Breathing period",
                .Description = "Seconds for one slow rise and fall of the flame's height.",
                .Type = SettingSpec::SettingType::Float, .MinimumValue = 1.0, .MaximumValue = 300.0 }));
        }
        return &mySettingSpecs;
    }

    // Simulation cells for a channel: its physical length in 60 LED/m cells, supersampled when
    // that's too short to look like fire. Halved for mirrored, which simulates one half twice.
    static int SimCellsFor(int ledCount, float densityScale)
    {
        const int cells = std::max(1, static_cast<int>(std::lround(ledCount / densityScale)));
        return cells <= kSupersampleMinCells ? kSupersampleCells : cells;
    }

    // A smooth wander between minHeight and maxHeight resting near averageHeight, from three
    // incommensurate sine waves (the requested period dominant, two faster ones for texture),
    // phased by the channel's seed. Same shape as LED-Central's fireHeightFraction().
    float HeightFraction(float seconds, uint32_t seed) const
    {
        const float lo = std::clamp(minHeight, 0.0f, 1.0f);
        const float hi = std::clamp(maxHeight, 0.0f, 1.0f);
        const float avg = std::clamp(averageHeight, lo, hi);
        const float period = std::max(1.0f, heightWaveSeconds);
        auto phase = [seed](uint32_t n) { return ((seed * n) % 997) / 997.0f * TWO_PI; };
        const float wave = 0.7f * sinf(TWO_PI * seconds / period + phase(3))
                         + 0.2f * sinf(TWO_PI * seconds / (period * 0.37f) + phase(7))
                         + 0.1f * sinf(TWO_PI * seconds / (period * 0.13f) + phase(13));
        return wave >= 0.0f ? avg + wave * (hi - avg) : avg + wave * (avg - lo);
    }

    void Step(Channel& state, float seconds) const
    {
        auto& heat = state.heat;
        const int cells = heat.size();

        // Cool every cell a little.
        for (auto& h : heat)
            h = qsub8(h, random8(std::clamp(cooling, 0, 100)));

        // Drift heat toward LED 0 and diffuse it, from the next two cells (weights 1 and 2).
        for (int i = 0; i < cells; ++i)
            heat[i] = std::min(255, (heat[(i + 1) % cells] + heat[(i + 2) % cells] * 2) / 3);

        // Ignite sparks near the far (high-index) end, which is the flame's base.
        for (int spark = 0; spark < std::clamp(sparks, 0, 20); ++spark)
        {
            if (random8() < std::clamp(sparking, 0, 255))
            {
                const int y = cells - 1 - random8(std::clamp(sparkHeight, 1, 20));
                if (y >= 0)
                    heat[y] = 200 + random8(56);
            }
        }

        // Soft ceiling: fade heat out over a short falloff past the breathing height.
        const int ceilingQ8 = static_cast<int>(HeightFraction(seconds, state.seed) * cells * 256);
        const int falloff = std::max(3, cells / 20);
        for (int i = 0; i < cells; ++i)
        {
            const int overQ8 = ((cells - 1 - i) << 8) - ceilingQ8;
            if (overQ8 > 0)
                heat[i] = heat[i] * std::max(0, 256 - overQ8 / falloff) >> 8;
        }
    }

    // NightDriver's HeatColor() ramp (black -> cool -> mid -> white), with the cool and mid
    // colors chosen by palette.
    static CRGB HeatToColor(uint8_t heat, const CRGB& cool, const CRGB& mid)
    {
        const uint8_t t192 = scale8_video(heat, 191);
        const uint8_t ramp = (t192 & 0x3F) << 2;
        if (t192 & 0x80) return blend(mid, CRGB::White, ramp);
        if (t192 & 0x40) return blend(cool, mid, ramp);
        return blend(CRGB::Black, cool, ramp);
    }

  public:

    LEDCentralFireEffect() : EffectWithId<LEDCentralFireEffect>("Fire") {}

    LEDCentralFireEffect(const JsonObjectConst& jsonObject) : EffectWithId<LEDCentralFireEffect>(jsonObject)
    {
        FieldAccess::AssignIfPresent(jsonObject, "cool", cooling);
        FieldAccess::AssignIfPresent(jsonObject, "spk", sparking);
        FieldAccess::AssignIfPresent(jsonObject, "spc", sparks);
        FieldAccess::AssignIfPresent(jsonObject, "sph", sparkHeight);
        FieldAccess::AssignIfPresent(jsonObject, "pal", palette);
        FieldAccess::AssignIfPresent(jsonObject, "rev", reversed);
        FieldAccess::AssignIfPresent(jsonObject, "mir", mirrored);
        FieldAccess::AssignIfPresent(jsonObject, "hmin", minHeight);
        FieldAccess::AssignIfPresent(jsonObject, "havg", averageHeight);
        FieldAccess::AssignIfPresent(jsonObject, "hmax", maxHeight);
        FieldAccess::AssignIfPresent(jsonObject, "hws", heightWaveSeconds);
    }

    bool SerializeToJSON(JsonObject& jsonObject) override
    {
        auto jsonDoc = CreateJsonDocument();
        JsonObject root = jsonDoc.to<JsonObject>();
        LEDStripEffect::SerializeToJSON(root);

        jsonDoc["cool"] = cooling;
        jsonDoc["spk"] = sparking;
        jsonDoc["spc"] = sparks;
        jsonDoc["sph"] = sparkHeight;
        jsonDoc["pal"] = palette;
        jsonDoc["rev"] = reversed;
        jsonDoc["mir"] = mirrored;
        jsonDoc["hmin"] = minHeight;
        jsonDoc["havg"] = averageHeight;
        jsonDoc["hmax"] = maxHeight;
        jsonDoc["hws"] = heightWaveSeconds;

        return SetIfNotOverflowed(jsonDoc, jsonObject, __PRETTY_FUNCTION__);
    }

    bool SerializeSettingsToJSON(JsonObject& jsonObject) override
    {
        auto jsonDoc = CreateJsonDocument();
        JsonObject root = jsonDoc.to<JsonObject>();
        LEDStripEffect::SerializeSettingsToJSON(root);

        jsonDoc[NAME_OF(cooling)] = cooling;
        jsonDoc[NAME_OF(sparking)] = sparking;
        jsonDoc[NAME_OF(sparks)] = sparks;
        jsonDoc[NAME_OF(sparkHeight)] = sparkHeight;
        jsonDoc[NAME_OF(palette)] = palette;
        jsonDoc[NAME_OF(reversed)] = reversed;
        jsonDoc[NAME_OF(mirrored)] = mirrored;
        jsonDoc[NAME_OF(minHeight)] = minHeight;
        jsonDoc[NAME_OF(averageHeight)] = averageHeight;
        jsonDoc[NAME_OF(maxHeight)] = maxHeight;
        jsonDoc[NAME_OF(heightWaveSeconds)] = heightWaveSeconds;

        return SetIfNotOverflowed(jsonDoc, jsonObject, __PRETTY_FUNCTION__);
    }

    bool SetSetting(const String& name, const String& value) override
    {
        return FieldAccess::AssignIfSelected(name, NAME_OF(cooling), cooling, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(sparking), sparking, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(sparks), sparks, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(sparkHeight), sparkHeight, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(palette), palette, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(reversed), reversed, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(mirrored), mirrored, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(minHeight), minHeight, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(averageHeight), averageHeight, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(maxHeight), maxHeight, value)
            || FieldAccess::AssignIfSelected(name, NAME_OF(heightWaveSeconds), heightWaveSeconds, value)
            || LEDStripEffect::SetSetting(name, value);
    }

    size_t DesiredFramesPerSecond() const override
    {
        return 45;
    }

    void Start() override
    {
        channelState.clear();
        startMillis = millis();
        stepsDone = 0;
    }

    void Draw() override
    {
        const size_t channels = LEDCentral::ActiveChannels(_GFX);
        channelState.resize(channels);

        // Size (or resize, if the topology changed) each channel's simulation.
        for (size_t ch = 0; ch < channels; ++ch)
        {
            const int count = _GFX[ch]->GetLEDCount();
            const int simCells = SimCellsFor(count, LEDCentral::DensityScale(ch));
            const size_t heatCells = mirrored ? std::max(1, simCells / 2) : simCells;
            if (channelState[ch].heat.size() != heatCells)
            {
                channelState[ch].heat.assign(heatCells, 0);
                channelState[ch].seed = RandomU32();
            }
        }

        // Advance in fixed 20ms steps, catching up a few at most after a slow frame.
        const uint32_t dueSteps = (millis() - startMillis) / kStepMs;
        if (dueSteps - stepsDone > kMaxCatchUpSteps)
            stepsDone = dueSteps - kMaxCatchUpSteps;
        for (; stepsDone < dueSteps; ++stepsDone)
            for (auto& state : channelState)
                Step(state, stepsDone * kStepMs / 1000.0f);

        const CRGB cool = palette == "green" ? CRGB(0, 255, 0) : palette == "blue" ? CRGB(0, 0, 255) : CRGB(255, 0, 0);
        const CRGB mid = palette == "blue" ? CRGB(0, 255, 255) : CRGB(255, 255, 0);

        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto& gfx = *_GFX[ch];
            const auto& heat = channelState[ch].heat;
            const int count = gfx.GetLEDCount();
            const int heatCells = heat.size();
            const int simCells = mirrored ? heatCells * 2 : heatCells;

            // Heat index heatCells-1 is the flame's hot, spark-fed base. This is LED-Central's
            // fire plotting run backwards (simulation position -> heat index): unmirrored, the
            // base sits at position 0 (LED 0), or at the far end if reversed; mirrored, one half
            // is drawn twice, meeting base to base in the middle, or tip to tip if reversed.
            auto heatAt = [&](int pos) -> uint8_t
            {
                int i;
                if (!mirrored)
                    i = reversed ? pos : simCells - 1 - pos;
                else if (pos < heatCells)
                    i = reversed ? heatCells - 1 - pos : pos;
                else
                    i = reversed ? pos - heatCells : simCells - 1 - pos;
                return heat[std::clamp(i, 0, heatCells - 1)];
            };

            // Box-filter the simulation's cells down to this channel's LEDs.
            for (int i = 0; i < count; ++i)
            {
                const int from = i * simCells / count;
                const int to = std::max(from + 1, (i + 1) * simCells / count);
                uint16_t r = 0, g = 0, b = 0;
                for (int pos = from; pos < to; ++pos)
                {
                    const CRGB c = HeatToColor(heatAt(pos), cool, mid);
                    r += c.r; g += c.g; b += c.b;
                }
                const int n = to - from;
                gfx.setPixel(i, CRGB(r / n, g / n, b / n));
            }
        }
    }
};
