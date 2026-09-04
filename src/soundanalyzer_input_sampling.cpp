//+--------------------------------------------------------------------------
//
// File:        soundanalyzer_input_sampling.cpp
//
// This file is part of soundanalyzer.cpp; see that file header for additional context.
//
// Split scope: SoundAnalyzer sampling routines for M5, I2S, and ADC backends.
//---------------------------------------------------------------------------


#include "globals.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>

#include "soundanalyzer.h"
#include "systemcontainer.h"
#include "values.h"

#if ENABLE_AUDIO

#if USE_M5
    #include <M5Unified.h>
#endif

size_t SoundAnalyzerBase::SampleM5()
{
    size_t bytesRead = 0;
#if USE_M5
    constexpr auto bytesExpected = MAX_SAMPLES * sizeof(ptrSampleBuffer[0]);
    if (M5.Mic.record((int16_t *)ptrSampleBuffer.get(), MAX_SAMPLES, SAMPLING_FREQUENCY, false))
    {
        bytesRead = bytesExpected;
    }

#endif

    return bytesRead;
}

size_t SoundAnalyzerBase::SampleI2S_Modern()
{
    size_t bytesReadTotal = 0;
#if (USE_I2S_AUDIO || ELECROW) && IS_IDF5
    static int32_t tempBuffer[MAX_SAMPLES * 2];
    constexpr int kChannels = 2;
    size_t bytesToRead = MAX_SAMPLES * kChannels * sizeof(int32_t);
    size_t bytesRead = 0;

    esp_err_t err = i2s_channel_read(_rx_handle, (void *)tempBuffer, bytesToRead, &bytesRead, 100 / portTICK_PERIOD_MS);
    if (err != ESP_OK)
        return 0;

    for (int i = 0; i < MAX_SAMPLES; i++)
    {
        if (i * kChannels >= (bytesRead / 4))
            break;
        int32_t s32 = tempBuffer[i * kChannels]; // Left channel
        ptrSampleBuffer[i] = (int16_t)std::clamp(s32 >> 15, -32768, 32767);
    }
    bytesReadTotal = bytesRead / kChannels / 2; // Rough approximation of output samples converted to bytes
#endif

    return bytesReadTotal;
}

size_t SoundAnalyzerBase::SampleI2S_Legacy()
{
    size_t bytesRead = 0;
#if (USE_I2S_AUDIO || ELECROW) && !IS_IDF5
    constexpr int kChannels = 2; // RIGHT + LEFT
    constexpr auto wordsToRead = MAX_SAMPLES * kChannels;
    constexpr auto bytesExpected32 = wordsToRead * sizeof(int32_t);
    static int32_t raw32[wordsToRead];

    ESP_ERROR_CHECK(i2s_read(I2S_NUM_0, (void *)raw32, bytesExpected32, &bytesRead, 100 / portTICK_PERIOD_MS));
    if (bytesRead != bytesExpected32)
    {
        debugW("Only read %u of %u bytes from I2S\n", bytesRead, bytesExpected32);
        return bytesRead;
    }

    static int s_chanIndex = -1;
    if (s_chanIndex < 0)
    {
        long long sumAbs[2] = {0, 0};
        for (int i = 0; i < MAX_SAMPLES; ++i)
        {
            int32_t r0 = raw32[i * kChannels + 0];
            int32_t r1 = raw32[i * kChannels + 1];
            sumAbs[0] += llabs((long long)r0);
            sumAbs[1] += llabs((long long)r1);
        }
        s_chanIndex = (sumAbs[1] > sumAbs[0]) ? 1 : 0;
    }

    for (int i = 0; i < MAX_SAMPLES; i++)
    {
        int32_t v = raw32[i * kChannels + s_chanIndex];
        int32_t scaled = (v >> 15);
        ptrSampleBuffer[i] = (int16_t)std::clamp(scaled, (int32_t)INT16_MIN, (int32_t)INT16_MAX);
    }
    bytesRead = MAX_SAMPLES * sizeof(int16_t); // effectively valid now
#endif

    return bytesRead;
}

// Both PDM backends capture the data line on both clock phases as a stereo pair
// and pick the one the mic actually drives, so neither has to assume a sampling
// edge. They also apply a dynamic AGC gain; see GetAndUpdatePDMGain() below and
// the PDM_AGC_* macros in globals.h.

#if USE_PDM_AUDIO
namespace
{
    constexpr int kPDMPhases = 2;

    // A mono PDM mic drives the data line during only one half of each clock
    // period - which half is fixed by how its L/R select is strapped, and the
    // ESP32 gives us no way to ask. Decode both and keep the one with signal in
    // it; the other is the mic sitting in high-Z. Same trick SampleI2S_Legacy
    // uses to sort out an INMP441's channel.
    //
    // Deliberately does not latch until one phase clearly wins, so booting into
    // a silent room cannot pin the wrong one for the life of the session.
    int SelectPDMPhase(const int16_t * frames, size_t frameCount)
    {
        static int decided = -1;
        if (decided >= 0)
            return decided;

        long long level[kPDMPhases] = {0, 0};
        for (size_t i = 0; i < frameCount; ++i)
            for (int phase = 0; phase < kPDMPhases; ++phase)
                level[phase] += llabs(frames[i * kPDMPhases + phase]);

        const int louder = (level[1] > level[0]) ? 1 : 0;

        // Needs to beat the other phase 2:1 and clear a mean of ~8 counts, which
        // room tone manages easily but digital silence does not.
        if (level[louder] > 2 * level[1 - louder] && level[louder] > (long long)frameCount * 8)
        {
            decided = louder;
            debugI("Audio: PDM mic decoded on the %s clock phase (levels %lld vs %lld)",
                   louder == 0 ? "first (rising)" : "second (falling)", level[0], level[1]);
        }

        return louder;
    }

    // De-interleave the chosen phase into the analyzer's buffer, applying a
    // dynamic input gain on the way. The gain is chosen per-frame from this
    // frame's own peak so loud passages never clip: fast attack (snap down
    // immediately on overload) protects this frame, slow release (creep back
    // up while there's headroom) recovers gain gradually. Release is further
    // gated on crest factor (frame RMS vs. a tracked ambient RMS floor):
    // steady wideband noise (HVAC hiss, typing) has RMS close to its own
    // floor, so it never clears the gate and gain holds; music's transients
    // sit well above the floor and do clear it, so gain still climbs to use
    // the available headroom. Downstream band normalization is scale-
    // invariant (see the file header comment near PDM_GAIN in globals.h), so
    // this only trades off clipping avoidance against bit depth - it doesn't
    // change how "loud" the analyzer perceives anything.
    float GetAndUpdatePDMGain(const int16_t * frames, size_t frameCount, int phase)
    {
        static float gain = (float)PDM_GAIN;
        static float noiseFloorRms = 1.0f;

        // Widen to int: abs(INT16_MIN) doesn't fit back in an int16_t.
        int peak = 0;
        double sumSquares = 0.0;
        for (size_t i = 0; i < frameCount; ++i)
        {
            const int sample = std::abs((int)frames[i * kPDMPhases + phase]);
            peak = std::max(peak, sample);
            sumSquares += (double)sample * (double)sample;
        }
        const float rms = (frameCount > 0) ? sqrtf((float)(sumSquares / (double)frameCount)) : 0.0f;

        // Same rise-while-above/decay-while-below tracker used for the per-band
        // noise floors in ProcessPeaksEnergy: slow to rise so a single transient
        // can't masquerade as the new ambient level, slow to decay so it doesn't
        // just echo whatever played last.
        if (rms > noiseFloorRms)
            noiseFloorRms = noiseFloorRms * (1.0f - PDM_AGC_NOISE_ADAPT) + rms * PDM_AGC_NOISE_ADAPT;
        else
            noiseFloorRms *= PDM_AGC_NOISE_DECAY;
        noiseFloorRms = std::max(noiseFloorRms, 1.0f);

        constexpr float kTarget = PDM_AGC_TARGET_PEAK_FRACTION * (float)INT16_MAX;
        if (peak > 0)
        {
            const float neededGain = std::clamp(kTarget / (float)peak, PDM_AGC_MIN_GAIN, PDM_AGC_MAX_GAIN);
            const bool aboveAmbient = rms > noiseFloorRms * PDM_AGC_RELEASE_SNR_GATE;
            if (neededGain < gain)
                gain = neededGain;                                                   // fast attack: snap down now
            else if (aboveAmbient)
                gain += (neededGain - gain) * PDM_AGC_RELEASE_PER_FRAME;             // slow release: creep back up
        }

        gain = std::clamp(gain, (float)PDM_AGC_MIN_GAIN, (float)PDM_AGC_MAX_GAIN);
        return gain;
    }

    // De-interleave the chosen phase into the analyzer's buffer, applying the
    // dynamic input gain computed above. The clamp remains as a last-resort
    // safety net; the AGC should keep genuine samples well inside it.
    void ExtractPDMPhase(const int16_t * frames, size_t frameCount, int16_t * out)
    {
        const int phase = SelectPDMPhase(frames, frameCount);
        const float gain = GetAndUpdatePDMGain(frames, frameCount, phase);

        for (size_t i = 0; i < frameCount; ++i)
            out[i] = static_cast<int16_t>(std::clamp(frames[i * kPDMPhases + phase] * gain,
                                                     (float)INT16_MIN, (float)INT16_MAX));
    }
}
#endif

size_t SoundAnalyzerBase::SamplePDM_Modern()
{
    size_t bytesRead = 0;
#if USE_PDM_AUDIO && IS_IDF5
    static int16_t frames[MAX_SAMPLES * kPDMPhases];
    constexpr auto bytesExpected = sizeof(frames);

    if (i2s_channel_read(_rx_handle, frames, bytesExpected, &bytesRead, 100 / portTICK_PERIOD_MS) != ESP_OK)
        return 0;

    const size_t framesRead = bytesRead / (kPDMPhases * sizeof(frames[0]));
    ExtractPDMPhase(frames, framesRead, ptrSampleBuffer.get());
    bytesRead = framesRead * sizeof(ptrSampleBuffer[0]);
#endif

    return bytesRead;
}

size_t SoundAnalyzerBase::SamplePDM_Legacy()
{
    size_t bytesRead = 0;
#if USE_PDM_AUDIO && !IS_IDF5
    static int16_t frames[MAX_SAMPLES * kPDMPhases];
    constexpr auto bytesExpected = sizeof(frames);

    ESP_ERROR_CHECK(i2s_read(I2S_NUM_0, frames, bytesExpected, &bytesRead, 100 / portTICK_PERIOD_MS));
    if (bytesRead != bytesExpected)
    {
        debugW("Only read %u of %u bytes from PDM mic\n", bytesRead, bytesExpected);
        return bytesRead;
    }

    ExtractPDMPhase(frames, MAX_SAMPLES, ptrSampleBuffer.get());
    bytesRead = MAX_SAMPLES * sizeof(ptrSampleBuffer[0]);
#endif

    return bytesRead;
}

size_t SoundAnalyzerBase::SampleADC_Modern()
{
    size_t ret_num = 0;
#if !USE_M5 && !USE_I2S_AUDIO && !USE_PDM_AUDIO && IS_IDF5
    constexpr size_t bytesToRead = MAX_SAMPLES * sizeof(uint16_t);
    esp_err_t err = adc_continuous_read(_adc_handle, (uint8_t *)ptrSampleBuffer.get(), bytesToRead, (uint32_t *)&ret_num, 0);

    if (err == ESP_OK)
    {
        for (int i = 0; i < MAX_SAMPLES; i++)
        {
            if (i * 2 >= ret_num)
                break;
            uint16_t val = ptrSampleBuffer[i];
            uint16_t data = val & 0xFFF; // Keep 12 bits
            ptrSampleBuffer[i] = (int16_t)((data - 2048) * 16);
        }
    }
#endif

    return ret_num;
}

size_t SoundAnalyzerBase::SampleADC_Legacy()
{
    size_t bytesRead = 0;
#if !USE_M5 && !USE_I2S_AUDIO && !USE_PDM_AUDIO && !IS_IDF5 && defined(SOC_I2S_SUPPORTS_ADC)
    constexpr auto bytesExpected16 = MAX_SAMPLES * sizeof(ptrSampleBuffer[0]);
    ESP_ERROR_CHECK(i2s_read(I2S_NUM_0, (void *)ptrSampleBuffer.get(), bytesExpected16, &bytesRead, 100 / portTICK_PERIOD_MS));
    if (bytesRead != bytesExpected16)
    {
        debugW("Could only read %u bytes of %u in FillBufferI2S()\n", bytesRead, bytesExpected16);
        return bytesRead;
    }
#endif

    return bytesRead;
}

#endif
