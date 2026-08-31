#pragma once
//+--------------------------------------------------------------------------
//
// File:        fireworkseffect.h
//
// NightDriverStrip - (c) 2026 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of the NightDriver software project.
//
// Description:
//
//   Beat-triggered strip fireworks adapted from Dave's NDSCPP FireworksEffect.
//   Unlike the original desktop effect, new bursts are not spawned randomly;
//   they are launched strictly from the shared audio beat detector.
//
//---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <deque>

#include "random_utils.h"
#include "soundanalyzer.h"

class FireworksEffect : public EffectWithId<FireworksEffect>
{
    struct Particle
    {
        CRGB  baseColor;
        float position;
        float velocity;
        float age;
        float preignitionTime;
        float ignitionTime;
        float holdTime;
        float fadeTime;
        float size;
        float drag;

        float TotalLifetime() const
        {
            return preignitionTime + ignitionTime + holdTime + fadeTime;
        }

        bool IsAlive() const
        {
            return age < TotalLifetime();
        }

        void Update(float deltaSeconds)
        {
            position += velocity * deltaSeconds;
            velocity -= velocity * drag * deltaSeconds;
            age += deltaSeconds;
        }

        CRGB CurrentColor() const
        {
            if (age < preignitionTime + ignitionTime)
            {
                CRGB white = CRGB::White;
                const float ignitionAge = std::max(0.0f, age - preignitionTime);
                const float ignitionBlend = ignitionTime > 0.0f ? std::clamp(ignitionAge / ignitionTime, 0.0f, 1.0f) : 1.0f;
                CRGB color = ignitionBlend < 0.60f
                    ? white
                    : BlendColor(white, baseColor, static_cast<uint8_t>((ignitionBlend - 0.60f) / 0.40f * 255.0f));
                return color;
            }

            CRGB color = baseColor;
            const float fadeStart = preignitionTime + ignitionTime + holdTime;
            if (age > fadeStart && fadeTime > 0.0f)
            {
                const float fade = std::clamp((age - fadeStart) / fadeTime, 0.0f, 1.0f);
                color.fadeToBlackBy(static_cast<uint8_t>(fade * 255.0f));
            }
            return color;
        }

        // I want to opportunistically pull the ignition phase towards pure white, so that if the beat is 
        // strong and the particle ignites fully, it has a bright white core.  This is a defining 
        // characteristic of fireworks and makes them pop visually, but it also means that at lower 
        // brightness levels, the colors can be very dim until the particle is well into its ignition phase.  
        // This method allows effects that want to maintain a more colorful look at low brightness to 
        // check whether the particle is still in that very-white ignition phase.

        bool IsPureWhiteIgnition() const
        {
            if (age >= preignitionTime + ignitionTime)
                return false;

            const float ignitionAge = std::max(0.0f, age - preignitionTime);
            const float ignitionBlend = ignitionTime > 0.0f ? std::clamp(ignitionAge / ignitionTime, 0.0f, 1.0f) : 1.0f;
            return ignitionBlend < 0.60f;
        }

      private:

        static CRGB BlendColor(const CRGB& a, const CRGB& b, uint8_t amountOfB)
        {
            CRGB mixed = a;
            nblend(mixed, b, amountOfB);
            return mixed;
        }
    };

    std::deque<Particle> _particles;
    uint32_t _lastBeatSequence = 0;
    size_t _maxParticles = 256;
    float _maxSpeed = 175.0f;
    float _particlePreignitionTime = 0.00f;
    float _particleIgnitionTime = 0.14f;
    float _particleHoldTime = 0.03f;
    float _particleFadeTime = 0.95f;
    float _particleSize = 1.0f;
    float _particleDrag = 1.8f;
    uint8_t _frameFade = 56;
    size_t _baseBurstParticles = 12;

    void LaunchBurst(const BeatInfo& beat, float startPos, const CRGB& color, size_t particleCount, float speedScale)
    {
        const float ledSpan = std::max(1.0f, static_cast<float>(_cLEDs));
        const float burstSize = std::max(_particleSize, (0.85f + beat.strength * 0.24f) * std::max(1.0f, ledSpan / 460.0f));

        for (size_t i = 0; i < particleCount; ++i)
        {
            Particle particle;
            particle.baseColor = color;
            particle.position = startPos;
            particle.velocity = random_range(-_maxSpeed * speedScale, _maxSpeed * speedScale);
            particle.age = 0.0f;
            particle.preignitionTime = _particlePreignitionTime;
            particle.ignitionTime = _particleIgnitionTime;
            particle.holdTime = _particleHoldTime;
            particle.fadeTime = _particleFadeTime;
            particle.size = std::max(1.0f, burstSize * random_range(0.8f, 1.2f));
            particle.drag = _particleDrag * random_range(0.85f, 1.15f);

            _particles.push_back(particle);
        }

        while (_particles.size() > _maxParticles)
            _particles.pop_front();
    }

  public:
    explicit FireworksEffect(const String& name = "Beat Fireworks")
        : EffectWithId<FireworksEffect>(name)
    {
    }

    explicit FireworksEffect(const JsonObjectConst& jsonObject)
        : EffectWithId<FireworksEffect>(jsonObject)
    {
    }

    bool Init(std::vector<std::shared_ptr<GFXBase>>& gfx) override
    {
        if (!LEDStripEffect::Init(gfx))
            return false;

        _maxParticles = std::max<size_t>(128, _cLEDs * 2);
        return true;
    }

    size_t DesiredFramesPerSecond() const override
    {
        return 60;
    }

    void Start() override
    {
        _particles.clear();
        _lastBeatSequence = 0;
        setAllOnAllChannels(0, 0, 0);
    }

    bool RequiresAudio() const override { return true; }   // launches fireworks entirely from OnBeat()

    void OnBeat(const BeatInfo& beat) override
    {
        LEDStripEffect::OnBeat(beat);

        if (beat.sequence == 0 || beat.sequence == _lastBeatSequence || _cLEDs == 0)
            return;

        _lastBeatSequence = beat.sequence;

        debugV("Fireworks OnBeat: seq=%lu strength=%.3f confidence=%.3f major=%d",
               static_cast<unsigned long>(beat.sequence),
               beat.strength,
               beat.confidence,
               beat.major ? 1 : 0);

        const size_t burstCount = std::clamp<size_t>(
            1 + (beat.major ? 1U : 0U) + (beat.strength >= 2.1f ? 1U : 0U),
            1,
            3);
        const size_t particleCount = std::clamp<size_t>(
            static_cast<size_t>(_baseBurstParticles + beat.confidence * 10.0f + beat.strength * 6.0f + (beat.major ? 6.0f : 0.0f)),
            4,
            12);
        const float speedScale = std::clamp(0.5f + beat.strength * 0.32f + beat.confidence * 0.18f, 0.25f, 1.75f);

        for (size_t burst = 0; burst < burstCount; ++burst)
        {
            const float startPos = random_range(0.0f, std::max(0.0f, static_cast<float>(_cLEDs - 1)));
            LaunchBurst(beat, startPos, RandomSaturatedColor(), particleCount, speedScale * random_range(0.90f, 1.15f));
        }
    }

    void Draw() override
    {
        fadeAllChannelsToBlackBy(_frameFade);
        clearWhitesOnAllChannels();

        const float deltaSeconds = static_cast<float>(g_Values.AppTime.LastFrameTime());
        if (deltaSeconds <= 0.0f)
            return;

        for (auto it = _particles.begin(); it != _particles.end(); )
        {
            it->Update(deltaSeconds);

            if (!it->IsAlive() || it->position < -it->size || it->position > static_cast<float>(_cLEDs) + it->size)
            {
                it = _particles.erase(it);
                continue;
            }

            const float start = it->position - it->size * 0.5f;
            setPixelsFOnAllChannels(start, it->size, it->CurrentColor(), true);
            if (it->IsPureWhiteIgnition())
                setWhiteOnAllChannels(start, it->size, CRGBW(255, 255), true);
            ++it;
        }
    }
};
