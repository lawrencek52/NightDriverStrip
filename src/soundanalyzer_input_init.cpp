//+--------------------------------------------------------------------------
//
// File:        soundanalyzer_input_init.cpp
//
// This file is part of soundanalyzer.cpp; see that file header for additional context.
//
// Split scope: SoundAnalyzer input backend initialization routines.
//---------------------------------------------------------------------------


#include "globals.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "soundanalyzer.h"
#include "systemcontainer.h"
#include "values.h"

#if ENABLE_AUDIO

#if USE_M5
    #include <M5Unified.h>
#endif

#if USE_AUDIO_CODEC
    #include <Wire.h>
#endif

namespace
{
    int GetConfiguredAudioInputPin()
    {
        if (g_ptrSystem)
            return g_ptrSystem->GetConfiguredAudioInputPin();

        return AUDIO_INPUT_PIN;
    }

#if USE_AUDIO_CODEC
    // ES7210 (4-channel mic ADC, only MIC1/MIC2 populated on this board - the
    // two onboard mics) and ES8311 (mono speaker codec) I2C register drivers.
    // Both chips need a real init sequence before they'll produce/accept valid
    // I2S audio, unlike the self-clocking digital mics USE_I2S_AUDIO otherwise
    // assumes. Sequences ported from Waveshare's own ES8311 Arduino example and
    // Espressif's esp-bsp ES7210 driver (both Apache-2.0 - compatible with this
    // file's GPLv3), trimmed to just what this board needs: fixed sample rates,
    // no runtime ALC/EQ. Local to this file - nothing outside audio input init
    // needs these chips.

    // Dual-mic array ADC. Fixed at 24 kHz/16-bit standard I2S over a 12.288 MHz
    // MCLK (512x ratio) - matches SoundAnalyzerBase::SAMPLING_FREQUENCY and the
    // MCLK InitI2S_Modern() configures below.
    class ES7210Codec
    {
    public:
        bool begin()
        {
            Wire.beginTransmission(kI2cAddr);
            if (Wire.endTransmission() != 0)
                return false;

            bool ok = true;
            // Software reset.
            ok &= WriteReg(0x00, 0xFF);
            ok &= WriteReg(0x00, 0x32);
            // Chip initial/power-up state timing.
            ok &= WriteReg(0x09, 0x30);
            ok &= WriteReg(0x0A, 0x30);
            // High-pass filter for ADC1-4.
            ok &= WriteReg(0x23, 0x2A);
            ok &= WriteReg(0x22, 0x0A);
            ok &= WriteReg(0x21, 0x2A);
            ok &= WriteReg(0x20, 0x0A);
            // 16-bit standard I2S, TDM off.
            ok &= WriteReg(0x11, 0x60);
            ok &= WriteReg(0x12, 0x00);
            // Analog power / VMID.
            ok &= WriteReg(0x40, 0xC3);
            // MIC1-4 bias 2.55V.
            ok &= WriteReg(0x41, 0x40);
            ok &= WriteReg(0x42, 0x40);
            // MIC1-4 gain 30dB.
            ok &= WriteReg(0x43, 0x1A);
            ok &= WriteReg(0x44, 0x1A);
            ok &= WriteReg(0x45, 0x1A);
            ok &= WriteReg(0x46, 0x1A);
            // Power on MIC1-4.
            ok &= WriteReg(0x47, 0x08);
            ok &= WriteReg(0x48, 0x08);
            ok &= WriteReg(0x49, 0x08);
            ok &= WriteReg(0x4A, 0x08);
            // 24kHz/512x MCLK coefficients (osr, adc_div|doubler|dll, lrck_h/l).
            ok &= WriteReg(0x07, 0x20);
            ok &= WriteReg(0x02, 0x81);
            ok &= WriteReg(0x04, 0x02);
            ok &= WriteReg(0x05, 0x00);
            // Power down DLL.
            ok &= WriteReg(0x06, 0x04);
            // Power on MIC bias & ADC & PGA for MIC1-4.
            ok &= WriteReg(0x4B, 0x0F);
            ok &= WriteReg(0x4C, 0x0F);
            // Enable device.
            ok &= WriteReg(0x00, 0x71);
            ok &= WriteReg(0x00, 0x41);

            return ok;
        }

    private:
        bool WriteReg(uint8_t reg, uint8_t val)
        {
            Wire.beginTransmission(kI2cAddr);
            Wire.write(reg);
            Wire.write(val);
            return Wire.endTransmission() == 0;
        }

        static constexpr uint8_t kI2cAddr = 0x40; // AD0/AD1 tied to GND
    };

    // Mono speaker codec (DAC + line driver feeding the onboard amp via
    // NS4150B). Fixed at 48 kHz/16-bit over the same 12.288 MHz MCLK - used
    // only for the boot self-test tone below, never concurrently with the mic:
    // the two chips share one physical LRCK/BCLK/MCLK bus, so only one sample
    // rate can be live on it at a time.
    class ES8311Codec
    {
    public:
        bool begin()
        {
            Wire.beginTransmission(kI2cAddr);
            if (Wire.endTransmission() != 0)
                return false;

            bool ok = true;
            ok &= WriteReg(0x00, 0x1F);
            delay(20);
            ok &= WriteReg(0x00, 0x00);
            ok &= WriteReg(0x00, 0x80);
            ok &= WriteReg(0x01, 0x3F);

            uint8_t reg = ReadReg(0x06);
            reg &= ~(1U << 5);
            ok &= WriteReg(0x06, reg);

            // 48kHz/256x MCLK coefficients (pre_div=1, pre_multi=0, adc_div=1,
            // dac_div=1, fs_mode=0, lrck=0x00FF, bclk_div=4, osr=0x10). reg02
            // needs no change here - pre_div=1/pre_multi=0 both contribute 0.
            ok &= WriteReg(0x03, 0x10);
            ok &= WriteReg(0x04, 0x10);
            ok &= WriteReg(0x05, 0x00);

            reg = ReadReg(0x06);
            reg &= 0xE0;
            reg |= 0x03;
            ok &= WriteReg(0x06, reg);

            reg = ReadReg(0x07);
            reg &= 0xC0;
            ok &= WriteReg(0x07, reg);
            ok &= WriteReg(0x08, 0xFF);

            // 16 bits per sample, both ADC and DAC word length fields.
            uint8_t reg09 = ReadReg(0x09) | (3 << 2);
            uint8_t reg0A = ReadReg(0x0A) | (3 << 2);
            ok &= WriteReg(0x09, reg09);
            ok &= WriteReg(0x0A, reg0A);

            ok &= WriteReg(0x0D, 0x01);
            ok &= WriteReg(0x0E, 0x02);
            ok &= WriteReg(0x12, 0x00);
            ok &= WriteReg(0x13, 0x10);
            ok &= WriteReg(0x1C, 0x6A);
            ok &= WriteReg(0x37, 0x08);

            ok &= setVolume(70);
            return ok;
        }

        bool setVolume(uint8_t volumePercent)
        {
            volumePercent = std::min<uint8_t>(volumePercent, 100);
            const int reg32 = volumePercent == 0 ? 0 : ((volumePercent * 256) / 100) - 1;
            return WriteReg(0x32, static_cast<uint8_t>(reg32));
        }

    private:
        bool WriteReg(uint8_t reg, uint8_t val)
        {
            Wire.beginTransmission(kI2cAddr);
            Wire.write(reg);
            Wire.write(val);
            return Wire.endTransmission() == 0;
        }

        uint8_t ReadReg(uint8_t reg)
        {
            Wire.beginTransmission(kI2cAddr);
            Wire.write(reg);
            Wire.endTransmission(false);
            Wire.requestFrom(static_cast<uint16_t>(kI2cAddr), static_cast<uint8_t>(1), true);
            return Wire.available() ? Wire.read() : 0;
        }

        static constexpr uint8_t kI2cAddr = 0x18;
    };

    // Plays a brief tone through the speaker to confirm the codec, amp-enable
    // GPIO, and wiring all work, using a transient I2S TX channel that's torn
    // down immediately after - InitI2S_Modern() then claims the persistent RX
    // channel for ongoing mic capture. Best-effort: logs and returns on any
    // failure rather than blocking audio bring-up over a speaker fault.
    void PlaySpeakerSelfTestTone()
    {
        constexpr uint32_t kSampleRate = 48000;
        constexpr uint32_t kToneHz = 440;
        constexpr uint32_t kDurationMs = 300;
        constexpr size_t kCycleSamples = kSampleRate / kToneHz;

        i2s_chan_handle_t txHandle = nullptr;
        i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
        if (i2s_new_channel(&chanCfg, &txHandle, nullptr) != ESP_OK)
        {
            debugW("Audio: speaker self-test skipped, could not allocate I2S TX channel");
            return;
        }

        i2s_std_config_t stdCfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = static_cast<gpio_num_t>(I2S_MCLK_PIN),
                .bclk = static_cast<gpio_num_t>(I2S_BCLK_PIN),
                .ws = static_cast<gpio_num_t>(I2S_WS_PIN),
                .dout = static_cast<gpio_num_t>(AUDIO_CODEC_SPEAKER_DATA_PIN),
                .din = I2S_GPIO_UNUSED,
            },
        };

        if (i2s_channel_init_std_mode(txHandle, &stdCfg) != ESP_OK || i2s_channel_enable(txHandle) != ESP_OK)
        {
            debugW("Audio: speaker self-test skipped, could not start I2S TX channel");
            i2s_del_channel(txHandle);
            return;
        }

        int16_t cycle[kCycleSamples * 2];
        for (size_t i = 0; i < kCycleSamples; ++i)
        {
            const auto sample = static_cast<int16_t>(8000.0f * sinf(2.0f * static_cast<float>(M_PI) * i / kCycleSamples));
            cycle[2 * i]     = sample;
            cycle[2 * i + 1] = sample;
        }

        const uint32_t cyclesNeeded = (kSampleRate * kDurationMs / 1000) / kCycleSamples;
        for (uint32_t c = 0; c < cyclesNeeded; ++c)
        {
            size_t bytesWritten = 0;
            i2s_channel_write(txHandle, cycle, sizeof(cycle), &bytesWritten, pdMS_TO_TICKS(100));
        }

        i2s_channel_disable(txHandle);
        i2s_del_channel(txHandle);
    }
#endif // USE_AUDIO_CODEC
}

void SoundAnalyzerBase::InitM5()
{
#if USE_M5
    debugI("Audio: Initializing M5Stack Microphone");
    // Can't use speaker and mic at the same time, and speaker defaults on, so turn it off
    M5.Speaker.setVolume(255);
    M5.Speaker.end();
    auto cfg = M5.Mic.config();
    cfg.sample_rate = SAMPLING_FREQUENCY;
    cfg.noise_filter_level = 0;
    cfg.magnification = 8;
    M5.Mic.config(cfg);
    M5.Mic.begin();
#endif
}

void SoundAnalyzerBase::InitAudioCodec()
{
#if USE_AUDIO_CODEC && IS_IDF5
    debugI("Audio: Initializing codec I2C bus on SDA:%d SCL:%d", AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN);
    Wire.begin(AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN);

    ES7210Codec mic;
    if (!mic.begin())
        debugW("Audio: ES7210 mic-array init failed - check I2C wiring/address");
    else
        debugI("Audio: ES7210 mic-array initialized");

    pinMode(AUDIO_CODEC_PA_ENABLE_PIN, OUTPUT);
    digitalWrite(AUDIO_CODEC_PA_ENABLE_PIN, LOW); // keep the amp muted until the codec is confirmed up

    ES8311Codec speaker;
    if (!speaker.begin())
    {
        debugW("Audio: ES8311 speaker codec init failed - check I2C wiring/address");
    }
    else
    {
        debugI("Audio: ES8311 speaker codec initialized, playing self-test tone");
        digitalWrite(AUDIO_CODEC_PA_ENABLE_PIN, HIGH);
        PlaySpeakerSelfTestTone();
    }
#endif
}

void SoundAnalyzerBase::InitI2S_Modern()
{
#if (USE_I2S_AUDIO || ELECROW) && IS_IDF5
    const auto audioInputPin = GetConfiguredAudioInputPin();
    debugI("Audio: Initializing I2S Digital Mic (Modern) on BCLK:%d WS:%d DIN:%d", I2S_BCLK_PIN, I2S_WS_PIN, audioInputPin);
    // Digital Microphones (INMP441, etc.) - Standard I2S Mode
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLING_FREQUENCY),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = static_cast<gpio_num_t>(I2S_MCLK_PIN),
            .bclk = static_cast<gpio_num_t>(I2S_BCLK_PIN),
            .ws = static_cast<gpio_num_t>(I2S_WS_PIN),
            .dout = I2S_GPIO_UNUSED,
            .din = static_cast<gpio_num_t>(audioInputPin),
        },
    };
#if USE_AUDIO_CODEC
    // ES7210 is only reachable at 24kHz with a 512x MCLK ratio (12.288MHz) -
    // see the coefficient comment in ES7210Codec::begin() above.
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_512;
#endif

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(_rx_handle));
#endif
}

void SoundAnalyzerBase::InitI2S_Legacy()
{
#if (USE_I2S_AUDIO || ELECROW) && !IS_IDF5
    const auto audioInputPin = GetConfiguredAudioInputPin();
    debugI("Audio: Initializing I2S Digital Mic (Legacy) on BCLK:%d WS:%d DIN:%d", I2S_BCLK_PIN, I2S_WS_PIN, audioInputPin);
    const i2s_config_t i2s_config = {.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
                                     .sample_rate = SAMPLING_FREQUENCY,
                                     .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
                                     .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
                                     .communication_format = I2S_COMM_FORMAT_STAND_I2S,
                                     .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
                                     .dma_buf_count = 4,
                                     .dma_buf_len = (int)MAX_SAMPLES,
                                     .use_apll = false,
                                     .tx_desc_auto_clear = false,
                                     .fixed_mclk = 0};

    pinMode(I2S_BCLK_PIN, OUTPUT);
    pinMode(I2S_WS_PIN, OUTPUT);
    pinMode(audioInputPin, INPUT);

    const i2s_pin_config_t pin_config = {.bck_io_num = I2S_BCLK_PIN,
                                         .ws_io_num = I2S_WS_PIN,
                                         .data_out_num = I2S_PIN_NO_CHANGE,
                                         .data_in_num = audioInputPin};

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
    ESP_ERROR_CHECK(i2s_start(I2S_NUM_0));
#endif
}

void SoundAnalyzerBase::InitPDM_Modern()
{
#if USE_PDM_AUDIO && IS_IDF5
    const auto audioInputPin = GetConfiguredAudioInputPin();
    debugI("Audio: Initializing PDM Mic (Modern) on CLK:%d DIN:%d", PDM_CLK_PIN, audioInputPin);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &_rx_handle));

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLING_FREQUENCY),
        // Stereo for the same reason as the legacy path below: take both clock
        // phases and resolve which one the mic drives at runtime. IDF 5 could
        // instead flip gpio_cfg.invert_flags.clk_inv, but detecting beats
        // guessing, and it keeps both backends on one code path.
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .clk = static_cast<gpio_num_t>(PDM_CLK_PIN),
            .din = static_cast<gpio_num_t>(audioInputPin),
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(_rx_handle, &pdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(_rx_handle));
#endif
}

void SoundAnalyzerBase::InitPDM_Legacy()
{
#if USE_PDM_AUDIO && !IS_IDF5
    const auto audioInputPin = GetConfiguredAudioInputPin();
    debugI("Audio: Initializing PDM Mic (Legacy) on CLK:%d DIN:%d", PDM_CLK_PIN, audioInputPin);

    // PDM RX borrows the I2S peripheral: WS carries the mic clock and there is
    // no bit clock at all.
    //
    // The receiver latches the data line on both clock phases and decodes them
    // as a stereo pair, so which edge a given mic lands on is decided by how its
    // L/R select is strapped, not by anything configurable - IDF 4.x exposes no
    // clock inversion for PDM RX (i2s_set_pdm_rx_down_sample is the only knob).
    // So capture both phases and let SamplePDM_Legacy pick the one carrying
    // signal; that costs one extra DMA word per frame and cannot be wrong.
    const i2s_config_t i2s_config = {.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
                                     .sample_rate = SAMPLING_FREQUENCY,
                                     .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
                                     .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
                                     .communication_format = I2S_COMM_FORMAT_STAND_I2S,
                                     .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
                                     .dma_buf_count = 4,
                                     .dma_buf_len = (int)MAX_SAMPLES,
                                     .use_apll = false,
                                     .tx_desc_auto_clear = false,
                                     .fixed_mclk = 0};

    const i2s_pin_config_t pin_config = {.bck_io_num = I2S_PIN_NO_CHANGE,
                                         .ws_io_num = PDM_CLK_PIN,
                                         .data_out_num = I2S_PIN_NO_CHANGE,
                                         .data_in_num = audioInputPin};

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));
    // Stereo, so both clock phases reach us. The PDM clock stays at 64 x the PCM
    // rate either way - the decimator produces one frame per clock period and
    // mono mode just discards a phase - so this does not disturb the 1.536MHz
    // line clock at the default DSR of 8.
    ESP_ERROR_CHECK(i2s_set_clk(I2S_NUM_0, SAMPLING_FREQUENCY, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
    ESP_ERROR_CHECK(i2s_start(I2S_NUM_0));
#endif
}

void SoundAnalyzerBase::InitADC_Modern()
{
#if !USE_M5 && !USE_I2S_AUDIO && !USE_PDM_AUDIO && IS_IDF5
    debugI("Audio: Initializing I2S ADC Analog Mic (Modern) on Channel 0");
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 1024,
        .conv_frame_size = MAX_SAMPLES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &_adc_handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = SAMPLING_FREQUENCY,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1, // Using ADC1
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };

    // Configure pattern: channel, attenuation, etc.
    adc_digi_pattern_config_t adc_pattern[1] = {0};
    adc_pattern[0].atten = ADC_ATTEN_DB_12; // 12dB (formerly 11dB) for full range ~3.3V
    adc_pattern[0].channel = ADC_CHANNEL_0; // FIXED for now, ideally map from AUDIO_INPUT_PIN
    adc_pattern[0].unit = ADC_UNIT_1;
    adc_pattern[0].bit_width = ADC_BITWIDTH_12;

    dig_cfg.adc_pattern = adc_pattern;
    dig_cfg.pattern_num = 1;

    ESP_ERROR_CHECK(adc_continuous_config(_adc_handle, &dig_cfg));
    ESP_ERROR_CHECK(adc_continuous_start(_adc_handle));
#endif
}

void SoundAnalyzerBase::InitADC_Legacy()
{
#if !USE_M5 && !USE_I2S_AUDIO && !USE_PDM_AUDIO && !IS_IDF5 && defined(SOC_I2S_SUPPORTS_ADC)
    debugI("Audio: Initializing I2S ADC Analog Mic (Legacy) on Channel 0");
    static_assert(SOC_I2S_SUPPORTS_ADC, "This ESP32 model does not support ADC built-in mode");

    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
        .sample_rate = SAMPLING_FREQUENCY,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 2,
        .dma_buf_len = MAX_SAMPLES,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_12));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_0));
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_adc_mode(ADC_UNIT_1, ADC1_CHANNEL_0));
#endif
}

// --- Private Sampling Helpers ---

#endif
