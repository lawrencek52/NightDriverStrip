//+--------------------------------------------------------------------------
//
// File:        ws281xoutputmanager.cpp
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// ESP32 runtime WS281x output manager. This is the small indirection layer that
// lets us change pins/channel count/live LED count without changing effect code.
//
// The runtime transport intentionally uses the ESP-IDF RMT driver (or PARLIO on
// chips whose RMT can't DMA, such as the ESP32-C5) directly instead of FastLED's
// ESP32 transport. FastLED remains responsible for CRGB color handling in the
// effect layer, while NightDriver owns the mutable channel/pin configuration
// required for live topology/output changes.
//
//---------------------------------------------------------------------------

#include "globals.h"

#if USE_WS281X

#include "ws281xoutputmanager.h"

#include <algorithm>
#include <cstdint>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include <soc/soc_caps.h>

// Two RMT backends, picked at compile time. On IDF 4.x (Arduino-ESP32 2.x)
// only the legacy driver/rmt.h API exists; on IDF 5.x driver_ng is the
// supported path. The legacy and driver_ng headers cannot be included in
// the same translation unit on IDF 5 because they use the name
// rmt_channel_t for two different types - the legacy as an enum, the new
// as `struct rmt_channel_t *` - so we include only the one we'll use.
#if ESP_IDF_VERSION_MAJOR >= 5
#include <driver/rmt_tx.h>
#else
#if defined(CONFIG_RMT_SUPPRESS_DEPRECATE_WARN)
#undef CONFIG_RMT_SUPPRESS_DEPRECATE_WARN
#endif
#define CONFIG_RMT_SUPPRESS_DEPRECATE_WARN 1
#include <driver/rmt.h>
#ifndef RMT_DEFAULT_CONFIG_TX
    #error "NightDriverStrip WS281x runtime transport requires the ESP-IDF legacy RMT API on IDF 4.x."
#endif
#endif

// Chips whose RMT can't DMA (C5/C6/H2) refill RMT memory from an ISR mid-frame;
// on a single core shared with WiFi that refill gets starved and long strips
// glitch. They also have only two RMT TX channels. Their PARLIO peripheral
// streams every strip in parallel straight from a DMA buffer instead.
#if ESP_IDF_VERSION_MAJOR >= 5 && SOC_PARLIO_SUPPORTED && !SOC_RMT_SUPPORT_DMA
#define WS281X_USE_PARLIO 1
#include <driver/parlio_tx.h>
#include <esp_cache.h>
#include <esp_memory_utils.h>
#include <hal/parlio_ll.h>
#else
#define WS281X_USE_PARLIO 0
#endif

#include "gfxbase.h"
#include "pixelformat.h"
#include "ws281xgfx.h"

// One channel's packed wire bytes for the frame being shown.
struct FrameChannel
{
    size_t index;
    const uint8_t* bytes;
    size_t byteCount;
    int8_t pin;
    size_t ledCount;
};

// Transport base class. Concrete subclasses (in the anonymous namespace below)
// wrap a single ESP-IDF output peripheral so the manager itself stays
// driver-agnostic. The base class lives at file scope so that the matching
// `class Transport;` forward declaration in the header (used by the
// `std::unique_ptr<Transport>` member) refers to the same type.
class Transport
{
public:
    virtual ~Transport() = default;
    // Configure (or reconfigure) a single TX channel for the given GPIO. Caller
    // guarantees the channel is not currently installed when this is invoked.
    // `byteCount` is the size of the outputBytes buffer for that channel
    // (ledCount * PixelFormat::BytesPerPixel()).
    virtual SuccessResultWithMessage ConfigureChannel(size_t channelIndex, gpio_num_t pin, size_t byteCount) = 0;

    // Tear down a previously installed channel. Idempotent: safe to call on
    // an already-released channel.
    virtual void ReleaseChannel(size_t channelIndex) = 0;

    // Transmit one frame on every listed channel and block until it has gone
    // out (or timed out). Implementation does its own error logging.
    virtual void ShowFrame(const FrameChannel* channels, size_t count) = 0;
};

namespace
{
    static_assert(NUM_CHANNELS <= 8, "ESP32 RMT path supports up to 8 WS281x channels");

    // RMT drives each channel independently, so a frame is queued channel by
    // channel and then waited on channel by channel.
    class RmtTransport : public ::Transport
    {
    protected:
        // Queue a frame for transmission on this channel.
        virtual void TransmitChannel(size_t channelIndex, const uint8_t* bytes, size_t byteCount, int8_t pin, size_t activeLEDCount) = 0;

        // Block until the most recent frame on this channel has finished
        // transmitting (or the timeout elapses).
        virtual void WaitForChannel(size_t channelIndex, int8_t pin, size_t activeLEDCount) = 0;

    public:
        void ShowFrame(const FrameChannel* channels, size_t count) override
        {
            // Queue every active channel first, then wait for completion in a second
            // pass. This keeps all strips in the same frame as closely aligned as the
            // RMT API allows.
            //
            // Starting all channels in the same instant means their initial DMA
            // descriptor-fill bursts land on the shared memory bus simultaneously;
            // with NUM_CHANNELS > 2 that contention can stall one channel's burst
            // long enough to glitch a few dozen bits into its stream (observed
            // around pixel 15 on channels 2/3, the ones started last). A short,
            // fixed stagger between each channel's transmit call spreads those
            // bursts out. This is deliberately much smaller than a full frame -
            // waiting for each channel to finish before starting the next doesn't
            // scale to the 1200-LED/20fps target (4 channels x ~36ms each would blow
            // the ~50ms frame budget), whereas a few hundred microseconds of stagger
            // is negligible at any supported frame rate/LED count.
            constexpr uint32_t kInterChannelStaggerUs = 500;

            for (size_t i = 0; i < count; ++i)
            {
                const auto& channel = channels[i];
                if (channel.index > 0)
                    delayMicroseconds(kInterChannelStaggerUs);

                TransmitChannel(channel.index, channel.bytes, channel.byteCount, channel.pin, channel.ledCount);
            }

            // The transmit wait is also where live reconfiguration pressure tends to
            // show up first, so failures here are logged separately from the queue step.

            for (size_t i = 0; i < count; ++i)
                WaitForChannel(channels[i].index, channels[i].pin, channels[i].ledCount);
        }
    };

    // Common timing
    //
    // FastLED already carries the WS2812 timing constants we want. We reuse
    // those values here, but the actual transport and GPIO ownership remain in
    // NightDriver's runtime manager rather than in FastLED's controller layer.

    constexpr uint32_t kWs2812T0HighNs = FASTLED_WS2812_T1;
    constexpr uint32_t kWs2812T0LowNs  = FASTLED_WS2812_T2 + FASTLED_WS2812_T3;
    constexpr uint32_t kWs2812T1HighNs = FASTLED_WS2812_T1 + FASTLED_WS2812_T2;
    constexpr uint32_t kWs2812T1LowNs  = FASTLED_WS2812_T3;

    // 25 ns ticks (40 MHz APB / 2 on legacy via kRmtClockDivider, or
    // resolution_hz=40MHz on driver_ng). The wait timeout is shared.
    constexpr TickType_t kRmtWaitTimeout = pdMS_TO_TICKS(100);

    constexpr uint16_t NsToRmtTicks(uint32_t nanoseconds)
    {
        constexpr uint32_t kTickNs = 25;
        return static_cast<uint16_t>((nanoseconds + (kTickNs - 1)) / kTickNs);
    }

#if ESP_IDF_VERSION_MAJOR < 5
    // Legacy-only: clock divider model and per-bit rmt_item32_t entries
    // populated from ISR by the translator. driver_ng's bytes-encoder
    // has its own bit-symbol descriptors built inside DriverNgTransport
    // and doesn't need any of these.
    constexpr uint8_t kRmtClockDivider = 2;
    constexpr uint8_t kRmtMemoryBlocksPerChannel = 1;

    constexpr rmt_item32_t MakeRmtItem(uint16_t highTicks, uint16_t lowTicks)
    {
        rmt_item32_t item{};
        item.level0 = 1;
        item.duration0 = highTicks;
        item.level1 = 0;
        item.duration1 = lowTicks;
        return item;
    }

    const DRAM_ATTR rmt_item32_t kBitZero = MakeRmtItem(NsToRmtTicks(kWs2812T0HighNs), NsToRmtTicks(kWs2812T0LowNs));
    const DRAM_ATTR rmt_item32_t kBitOne  = MakeRmtItem(NsToRmtTicks(kWs2812T1HighNs), NsToRmtTicks(kWs2812T1LowNs));

    // The translator is called by the legacy RMT driver as it needs more items.
    // It consumes raw GRB/RGB/etc. bytes and expands each bit into one timing
    // item, so the higher layers only need to provide packed color bytes.
    void IRAM_ATTR WS2812ByteTranslator(const void* src, rmt_item32_t* dest, size_t srcSize, size_t wantedNum, size_t* translatedSize, size_t* itemNum)
    {
        const auto* bytes = static_cast<const uint8_t*>(src);
        const size_t maxBytesByItems = wantedNum / 8;
        const size_t maxBytes = srcSize < maxBytesByItems ? srcSize : maxBytesByItems;

        size_t outputItems = 0;
        size_t consumedBytes = 0;
        while (consumedBytes < maxBytes)
        {
            const uint8_t value = bytes[consumedBytes];
            for (int bit = 7; bit >= 0; --bit)
                dest[outputItems++] = (value & (1U << bit)) ? kBitOne : kBitZero;
            ++consumedBytes;
        }

        *translatedSize = consumedBytes;
        *itemNum = outputItems;
    }
#endif // ESP_IDF_VERSION_MAJOR < 5

    // Per-pixel packing moved into PixelFormat (see include/pixelformat.h).
    // Ws2812Format reproduces the previous PackChannelPixels behavior; new
    // chips (SK6812, SM16825, WS2805) add sibling format classes there.

    void LogRuntimeWS281xConfiguration(const DeviceConfig& config, const std::vector<std::shared_ptr<GFXBase>>& devices, const char* reason)
    {
        debugI("WS281x config (%s): path=runtime driver=%s channels=%zu matrix=%ux%u serpentine=%d colorOrder=%s leds=%zu",
               reason ? reason : "update",
               config.GetRuntimeDriverName().c_str(),
               config.GetChannelCount(),
               static_cast<unsigned>(config.GetMatrixWidth()),
               static_cast<unsigned>(config.GetMatrixHeight()),
               config.IsMatrixSerpentine(),
               DeviceConfig::GetColorOrderName(config.GetWS281xColorOrder()).c_str(),
               config.GetActiveLEDCount());

        const auto& pins = config.GetWS281xPins();
        for (size_t channel = 0; channel < config.GetChannelCount() && channel < devices.size(); ++channel)
        {
            const auto& graphics = *devices[channel];
            debugI("WS281x channel %zu (runtime): pin=%d leds=%zu matrix=%ux%u serpentine=%d buffer=%p",
                   channel,
                   pins[channel],
                   graphics.GetLEDCount(),
                   static_cast<unsigned>(graphics.GetMatrixWidth()),
                   static_cast<unsigned>(graphics.GetMatrixHeight()),
                   graphics.IsSerpentine(),
                   graphics.leds);
        }
    }

    String FormatRmtError(const char* action, esp_err_t error)
    {
        return str_sprintf("%s failed (%s)", action, esp_err_to_name(error));
    }

#if ESP_IDF_VERSION_MAJOR < 5
    // Legacy IDF RMT API (driver/rmt.h). State-free: the channel index *is*
    // the rmt_channel_t value passed to every API call. Only present on
    // IDF 4 because legacy and driver_ng headers can't coexist in the same
    // translation unit on IDF 5 (rmt_channel_t name collision).
    class LegacyTransport : public RmtTransport
    {
    public:
        SuccessResultWithMessage ConfigureChannel(size_t channelIndex, gpio_num_t pin, size_t /*byteCount*/) override
        {
            // The IDF4-compatible helper seeds a TX config for the requested
            // channel/GPIO. We then override the pieces that matter for WS2812
            // timing and idle behavior.
            rmt_config_t config = RMT_DEFAULT_CONFIG_TX(pin, static_cast<rmt_channel_t>(channelIndex));
            config.clk_div = kRmtClockDivider;
            config.mem_block_num = kRmtMemoryBlocksPerChannel;
            config.tx_config.idle_output_en = true;
            config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

            if (const auto error = rmt_config(&config); error != ESP_OK)
                return { false, FormatRmtError("rmt_config", error) };

            if (const auto error = rmt_driver_install(static_cast<rmt_channel_t>(channelIndex), 0, ESP_INTR_FLAG_IRAM); error != ESP_OK)
                return { false, FormatRmtError("rmt_driver_install", error) };

            if (const auto error = rmt_translator_init(static_cast<rmt_channel_t>(channelIndex), WS2812ByteTranslator); error != ESP_OK)
            {
                rmt_driver_uninstall(static_cast<rmt_channel_t>(channelIndex));
                return { false, FormatRmtError("rmt_translator_init", error) };
            }

            return { true, "" };
        }

        void ReleaseChannel(size_t channelIndex) override
        {
            // Wait for any in-flight frame to finish before tearing down the
            // transport so a live pin/channel change does not pull the driver out
            // from under Show().
            const auto channel = static_cast<rmt_channel_t>(channelIndex);
            const auto waitError = rmt_wait_tx_done(channel, kRmtWaitTimeout);
            if (waitError == ESP_ERR_TIMEOUT)
            {
                debugW("rmt_wait_tx_done timed out during ReleaseChannel for channel=%zu; forcing TX stop",
                       channelIndex);

                const auto stopError = rmt_tx_stop(channel);
                if (stopError != ESP_OK)
                {
                    debugE("rmt_tx_stop failed during ReleaseChannel for channel=%zu error=%s",
                           channelIndex,
                           esp_err_to_name(stopError));
                }
            }
            else if (waitError != ESP_OK)
            {
                debugE("rmt_wait_tx_done failed during ReleaseChannel for channel=%zu error=%s",
                       channelIndex,
                       esp_err_to_name(waitError));
            }

            const auto uninstallError = rmt_driver_uninstall(channel);
            if (uninstallError != ESP_OK)
            {
                debugE("rmt_driver_uninstall failed during ReleaseChannel for channel=%zu error=%s",
                       channelIndex,
                       esp_err_to_name(uninstallError));
            }
        }

        void TransmitChannel(size_t channelIndex, const uint8_t* bytes, size_t byteCount, int8_t pin, size_t activeLEDCount) override
        {
            const auto error = rmt_write_sample(static_cast<rmt_channel_t>(channelIndex), bytes, byteCount, false);
            if (error != ESP_OK)
            {
                debugE("rmt_write_sample failed for channel=%zu pin=%d leds=%zu error=%s",
                       channelIndex,
                       pin,
                       activeLEDCount,
                       esp_err_to_name(error));
            }
        }

        void WaitForChannel(size_t channelIndex, int8_t pin, size_t activeLEDCount) override
        {
            const auto error = rmt_wait_tx_done(static_cast<rmt_channel_t>(channelIndex), kRmtWaitTimeout);
            if (error != ESP_OK)
            {
                debugE("rmt_wait_tx_done failed for channel=%zu pin=%d leds=%zu error=%s",
                       channelIndex,
                       pin,
                       activeLEDCount,
                       esp_err_to_name(error));
            }
        }
    };
#endif // ESP_IDF_VERSION_MAJOR < 5

#if ESP_IDF_VERSION_MAJOR >= 5
    // driver_ng IDF RMT API (driver/rmt_tx.h). Holds parallel arrays of
    // channel and encoder handles, since that API issues opaque handles
    // rather than identifying channels by index.
    constexpr rmt_symbol_word_t MakeRmtSymbol(uint16_t highTicks, uint16_t lowTicks)
    {
        rmt_symbol_word_t symbol{};
        symbol.level0 = 1;
        symbol.duration0 = highTicks;
        symbol.level1 = 0;
        symbol.duration1 = lowTicks;
        return symbol;
    }

    class DriverNgTransport : public RmtTransport
    {
        rmt_channel_handle_t _channels[NUM_CHANNELS] = {};
        rmt_encoder_handle_t _encoders[NUM_CHANNELS] = {};

    public:
        SuccessResultWithMessage ConfigureChannel(size_t channelIndex, gpio_num_t pin, size_t byteCount) override
        {
            rmt_tx_channel_config_t channelConfig = {};
            channelConfig.gpio_num = pin;
            channelConfig.clk_src = RMT_CLK_SRC_DEFAULT;
            // 40 MHz / 25 ns ticks - matches legacy clock divider 2 from APB 80 MHz.
            channelConfig.resolution_hz = 40 * 1000 * 1000;
            channelConfig.trans_queue_depth = 4;
            // Default (0) lets the driver pick any of the low/medium levels (1-3); pin to the
            // top of that range so other same-tier ISRs (e.g. I2S/PDM audio) can't win arbitration
            // and delay the RMT kickoff.
            channelConfig.intr_priority = 3;

#if SOC_RMT_SUPPORT_DMA
            // Each physical RMT channel only owns SOC_RMT_MEM_WORDS_PER_CHANNEL (48) words of
            // on-chip memory; a channel asking for more borrows blocks from its neighbors, so
            // with NUM_CHANNELS run back-to-back the last channels configured (higher indices,
            // i.e. later strips) can end up with the least buffer headroom. That starves their
            // ISR-driven refill during long transmissions and drops bits - worse on later
            // channels, worse toward the end of a strip. DMA moves the buffer into ordinary SRAM,
            // refilled by hardware instead of a per-bit ISR, so channels no longer fight over the
            // tiny shared memory pool. See the RMT DMA note in the ESP-IDF docs.
            //
            // Size the DMA buffer to hold the *entire* frame (1 symbol per bit). Anything
            // smaller forces the driver to refill the buffer from the ISR mid-transmission
            // once the preloaded portion is exhausted; that refill is subject to the same
            // interrupt-latency starvation described above and reliably corrupts the tail of
            // long single-channel runs (e.g. a 512-pixel matrix glitching past pixel ~340,
            // where a previous, smaller cap here ran out). Sizing to the whole frame means the
            // hardware streams straight out of SRAM with no live refill, so no starvation
            // window exists regardless of strip length. Per-channel SRAM cost is byteCount*32
            // (e.g. ~48KB for a 512-pixel matrix at 3 bytes/pixel) - trivial next to the ESP32's
            // internal SRAM budget for the channel counts/strip lengths this project targets.
            channelConfig.flags.with_dma = 1;
            channelConfig.mem_block_symbols = std::max<size_t>(byteCount * 8, 64);
#else
            channelConfig.mem_block_symbols = 96;  // grow in steps of SOC_RMT_MEM_WORDS_PER_CHANNEL (48)
#endif

            if (const auto error = rmt_new_tx_channel(&channelConfig, &_channels[channelIndex]); error != ESP_OK)
            {
                _channels[channelIndex] = nullptr;
                return { false, FormatRmtError("rmt_new_tx_channel", error) };
            }

            rmt_bytes_encoder_config_t encoderConfig = {};
            encoderConfig.bit0 = MakeRmtSymbol(NsToRmtTicks(kWs2812T0HighNs), NsToRmtTicks(kWs2812T0LowNs));
            encoderConfig.bit1 = MakeRmtSymbol(NsToRmtTicks(kWs2812T1HighNs), NsToRmtTicks(kWs2812T1LowNs));
            encoderConfig.flags.msb_first = 1;

            if (const auto error = rmt_new_bytes_encoder(&encoderConfig, &_encoders[channelIndex]); error != ESP_OK)
            {
                rmt_del_channel(_channels[channelIndex]);
                _channels[channelIndex] = nullptr;
                _encoders[channelIndex] = nullptr;
                return { false, FormatRmtError("rmt_new_bytes_encoder", error) };
            }

            if (const auto error = rmt_enable(_channels[channelIndex]); error != ESP_OK)
            {
                rmt_del_encoder(_encoders[channelIndex]);
                rmt_del_channel(_channels[channelIndex]);
                _channels[channelIndex] = nullptr;
                _encoders[channelIndex] = nullptr;
                return { false, FormatRmtError("rmt_enable", error) };
            }

            return { true, "" };
        }

        void ReleaseChannel(size_t channelIndex) override
        {
            if (_channels[channelIndex])
            {
                const auto waitError = rmt_tx_wait_all_done(_channels[channelIndex], 100);
                if (waitError == ESP_ERR_TIMEOUT)
                {
                    debugW("rmt_tx_wait_all_done timed out during ReleaseChannel for channel=%zu",
                           channelIndex);
                }
                else if (waitError != ESP_OK)
                {
                    debugE("rmt_tx_wait_all_done failed during ReleaseChannel for channel=%zu error=%s",
                           channelIndex,
                           esp_err_to_name(waitError));
                }

                if (const auto error = rmt_disable(_channels[channelIndex]); error != ESP_OK)
                {
                    debugE("rmt_disable failed during ReleaseChannel for channel=%zu error=%s",
                           channelIndex,
                           esp_err_to_name(error));
                }
            }

            if (_encoders[channelIndex])
            {
                if (const auto error = rmt_del_encoder(_encoders[channelIndex]); error != ESP_OK)
                {
                    debugE("rmt_del_encoder failed during ReleaseChannel for channel=%zu error=%s",
                           channelIndex,
                           esp_err_to_name(error));
                }
                _encoders[channelIndex] = nullptr;
            }

            if (_channels[channelIndex])
            {
                if (const auto error = rmt_del_channel(_channels[channelIndex]); error != ESP_OK)
                {
                    debugE("rmt_del_channel failed during ReleaseChannel for channel=%zu error=%s",
                           channelIndex,
                           esp_err_to_name(error));
                }
                _channels[channelIndex] = nullptr;
            }
        }

        void TransmitChannel(size_t channelIndex, const uint8_t* bytes, size_t byteCount, int8_t pin, size_t activeLEDCount) override
        {
            rmt_transmit_config_t txConfig = {};
            txConfig.loop_count = 0;
            const auto error = rmt_transmit(_channels[channelIndex], _encoders[channelIndex], bytes, byteCount, &txConfig);
            if (error != ESP_OK)
            {
                debugE("rmt_transmit failed for channel=%zu pin=%d leds=%zu error=%s",
                       channelIndex,
                       pin,
                       activeLEDCount,
                       esp_err_to_name(error));
            }
        }

        void WaitForChannel(size_t channelIndex, int8_t pin, size_t activeLEDCount) override
        {
            const auto error = rmt_tx_wait_all_done(_channels[channelIndex], 100); // 100ms timeout
            if (error != ESP_OK)
            {
                debugE("rmt_tx_wait_all_done failed for channel=%zu pin=%d leds=%zu error=%s",
                       channelIndex,
                       pin,
                       activeLEDCount,
                       esp_err_to_name(error));
            }
        }
    };
#endif // ESP_IDF_VERSION_MAJOR >= 5

#if WS281X_USE_PARLIO
    // PARLIO (parallel IO) TX transport. One TX unit clocks every strip out at
    // once - channel N is data lane N - from a single DMA buffer, so the CPU
    // isn't involved after the transfer starts.
    //
    // Each WS2812 bit is encoded as three 400 ns slots at 2.5 MHz: high, data,
    // low. That gives T0H = 400 ns, T1H = 800 ns and 1.2 us per bit, inside the
    // WS2812B datasheet window (+/-150 ns). 2.5 MHz is an exact integer division
    // of the 240 MHz PARLIO source clock, which matters because the divider has
    // no fractional part.
    //
    // The unit is rebuilt lazily on the first frame after any channel change, so
    // ApplyConfig's per-channel release/configure sequence costs one rebuild.
    class ParlioTransport : public ::Transport
    {
        static constexpr uint32_t kSlotHz      = 2'500'000;
        static constexpr size_t   kSlotsPerBit = 3;
        // ~300 us of trailing low slots latches WS2812B V5 parts (>280 us) even
        // when frames are sent back to back. A multiple of 8 keeps the frame
        // byte-aligned at every lane width.
        static constexpr size_t   kLatchSlots  = 752;
        static constexpr size_t   kCacheLine   = 64;      // >= every chip's L1/L2 line size

        static_assert(NUM_CHANNELS <= SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH, "PARLIO TX unit has too few data lanes for NUM_CHANNELS");

        std::array<gpio_num_t, NUM_CHANNELS> _pins;
        std::array<size_t, NUM_CHANNELS>     _byteCounts{};
        parlio_tx_unit_handle_t _unit = nullptr;
        uint8_t* _frame = nullptr;     // DMA buffer: encoded slots + zeroed latch tail
        size_t   _frameBytes = 0;
        size_t   _maxBytes = 0;        // longest channel, in wire bytes
        size_t   _width = 0;           // lane count, a power of two as PARLIO requires
        bool     _dirty = true;

        struct Layout { size_t width; size_t maxBytes; size_t frameBytes; };

        Layout ComputeLayout() const
        {
            size_t lanes = 0, maxBytes = 0;
            for (size_t i = 0; i < NUM_CHANNELS; ++i)
            {
                if (_pins[i] == GPIO_NUM_NC)
                    continue;
                lanes = i + 1;
                maxBytes = std::max(maxBytes, _byteCounts[i]);
            }
            if (!lanes)
                return {};

            size_t width = 1;
            while (width < lanes)
                width <<= 1;
            return { width, maxBytes, (maxBytes * 8 * kSlotsPerBit + kLatchSlots) * width / 8 };
        }

        void Teardown()
        {
            if (_unit)
            {
                parlio_tx_unit_wait_all_done(_unit, 100);
                parlio_tx_unit_disable(_unit);
                parlio_del_tx_unit(_unit);
                _unit = nullptr;
            }
            free(_frame);
            _frame = nullptr;
            _frameBytes = _maxBytes = _width = 0;
        }

        bool Rebuild()
        {
            Teardown();
            _dirty = false;

            const auto layout = ComputeLayout();
            if (!layout.width)
                return false;

            // Prefer PSRAM: at full length the frame is ~44 KB, and on the C5 that much
            // internal RAM is the difference between WiFi/lwIP surviving a burst of browser
            // connections or deadlocking for lack of RX buffers. GDMA reads PSRAM directly;
            // the buffer is cache-line aligned so ShowFrame can write the cache back. The
            // cost is that a flash write (config save, OTA) can stall the shared MSPI bus
            // and glitch the frame in flight.
#if SOC_PSRAM_DMA_CAPABLE
            _frame = static_cast<uint8_t*>(heap_caps_aligned_calloc(kCacheLine, 1, layout.frameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
#endif
            if (!_frame)
                _frame = static_cast<uint8_t*>(heap_caps_calloc(1, layout.frameBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (!_frame)
            {
                debugE("PARLIO: failed to allocate %zu-byte DMA frame buffer", layout.frameBytes);
                return false;
            }

            parlio_tx_unit_config_t config = {};
            config.clk_src            = PARLIO_CLK_SRC_DEFAULT;
            config.clk_in_gpio_num    = GPIO_NUM_NC;
            config.output_clk_freq_hz = kSlotHz;
            config.data_width         = layout.width;
            std::fill(std::begin(config.data_gpio_nums), std::end(config.data_gpio_nums), GPIO_NUM_NC);
            std::copy(_pins.begin(), _pins.end(), config.data_gpio_nums);
            config.clk_out_gpio_num   = GPIO_NUM_NC;
            config.valid_gpio_num     = GPIO_NUM_NC;
            config.trans_queue_depth  = 1;
            config.max_transfer_size  = layout.frameBytes;
            config.sample_edge        = PARLIO_SAMPLE_EDGE_POS;
            config.bit_pack_order     = PARLIO_BIT_PACK_ORDER_LSB;

            if (const auto error = parlio_new_tx_unit(&config, &_unit); error != ESP_OK)
            {
                debugE("parlio_new_tx_unit failed: %s", esp_err_to_name(error));
                _unit = nullptr;
                Teardown();
                return false;
            }
            if (const auto error = parlio_tx_unit_enable(_unit); error != ESP_OK)
            {
                debugE("parlio_tx_unit_enable failed: %s", esp_err_to_name(error));
                Teardown();
                return false;
            }

            _width = layout.width;
            _maxBytes = layout.maxBytes;
            _frameBytes = layout.frameBytes;
            return true;
        }

        // Transpose the channels' wire bytes into PARLIO samples. Every slot is
        // one sample of _width bits (bit N = lane N), packed LSB-first into the
        // buffer to match PARLIO_BIT_PACK_ORDER_LSB. A lane past the end of its
        // strip gets no high slot at all, so shorter strips just see idle low.
        //
        // This runs over every wire bit of every frame, so it avoids per-lane
        // loops: each byte position's lane bytes are packed into one 64-bit word
        // (lane N in byte N), and the multiply below gathers bit `bit` of all
        // eight bytes into the top byte in one step - the multiplier's shifts
        // (56 - 7N) land byte N's bit at result bit 56 + N with no carries.
        // A WS2812 bit's three slots (high, data, low) are then appended to the
        // output as one 3 * _width-bit group.
        void Encode(const FrameChannel* channels, size_t count)
        {
            constexpr uint64_t kLaneBits = 0x0101010101010101ULL;
            constexpr uint64_t kGather   = 0x0102040810204080ULL;

            uint8_t* out = _frame;
            uint64_t acc = 0;
            size_t accBits = 0;
            const size_t groupBits = kSlotsPerBit * _width;

            for (size_t byteIndex = 0; byteIndex < _maxBytes; ++byteIndex)
            {
                uint32_t live = 0;
                uint64_t lanes = 0;
                for (size_t i = 0; i < count; ++i)
                {
                    const auto& channel = channels[i];
                    if (byteIndex < channel.byteCount)
                    {
                        live |= 1u << channel.index;
                        lanes |= uint64_t(channel.bytes[byteIndex]) << (8 * channel.index);
                    }
                }

                for (int bit = 7; bit >= 0; --bit)     // WS2812 is MSB-first
                {
                    const uint32_t data = (((lanes >> bit) & kLaneBits) * kGather) >> 56;
                    acc |= uint64_t(live | data << _width) << accBits;     // third slot is 0
                    for (accBits += groupBits; accBits >= 8; accBits -= 8, acc >>= 8)
                        *out++ = static_cast<uint8_t>(acc);
                }
            }
        }

    public:
        ParlioTransport() { _pins.fill(GPIO_NUM_NC); }
        ~ParlioTransport() override { Teardown(); }

        SuccessResultWithMessage ConfigureChannel(size_t channelIndex, gpio_num_t pin, size_t byteCount) override
        {
            _pins[channelIndex] = pin;
            _byteCounts[channelIndex] = byteCount;
            _dirty = true;

            // A whole frame must fit one PARLIO transaction; splitting it would
            // leave an ISR-timed gap mid-frame, which is what this path avoids.
            if (const auto layout = ComputeLayout(); layout.frameBytes * 8 > PARLIO_LL_TX_MAX_BITS_PER_FRAME)
            {
                ReleaseChannel(channelIndex);
                return { false, str_sprintf("PARLIO frame of %zu bits exceeds the %u-bit hardware limit; shorten the longest strip",
                                            layout.frameBytes * 8, static_cast<unsigned>(PARLIO_LL_TX_MAX_BITS_PER_FRAME)) };
            }
            return { true, "" };
        }

        void ReleaseChannel(size_t channelIndex) override
        {
            _pins[channelIndex] = GPIO_NUM_NC;
            _byteCounts[channelIndex] = 0;
            _dirty = true;
        }

        void ShowFrame(const FrameChannel* channels, size_t count) override
        {
            if (_dirty)
                Rebuild();
            if (!_unit || !count)
                return;

            Encode(channels, count);
            if (esp_ptr_external_ram(_frame))
                esp_cache_msync(_frame, _frameBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

            parlio_transmit_config_t txConfig = {};
            txConfig.idle_value = 0;
            if (const auto error = parlio_tx_unit_transmit(_unit, _frame, _frameBytes * 8, &txConfig); error != ESP_OK)
            {
                debugE("parlio_tx_unit_transmit failed: %s", esp_err_to_name(error));
                return;
            }
            if (const auto error = parlio_tx_unit_wait_all_done(_unit, 100); error != ESP_OK)
                debugE("parlio_tx_unit_wait_all_done failed: %s", esp_err_to_name(error));
        }
    };
#endif // WS281X_USE_PARLIO

    std::unique_ptr<::Transport> CreateTransport()
    {
#if WS281X_USE_PARLIO
        return std::make_unique<ParlioTransport>();
#elif ESP_IDF_VERSION_MAJOR >= 5
        return std::make_unique<DriverNgTransport>();
#else
        return std::make_unique<LegacyTransport>();
#endif
    }
}

namespace
{
    // Pick the PixelFormat that matches the LED chip on the wire. Currently
    // selected by compile-time flag - eventually this could become a
    // DeviceConfig runtime knob. SK6812 (4-channel RGBW) is the second chip
    // we support; the WS2812 path is the default.
    std::unique_ptr<PixelFormat> CreatePixelFormat()
    {
#if defined(USE_SK6812) && USE_SK6812
        return std::make_unique<Sk6812Format>();
#else
        return std::make_unique<Ws2812Format>();
#endif
    }
}

// Out-of-line so the unique_ptr<Transport> default-deleter sees the full
// Transport definition above, and the unique_ptr<PixelFormat> deleter sees
// the full PixelFormat hierarchy from pixelformat.h.
WS281xOutputManager::WS281xOutputManager()
    : _transport(CreateTransport()), _format(CreatePixelFormat())
{
}

WS281xOutputManager::~WS281xOutputManager()
{
    Reset();
}

void WS281xOutputManager::Reset()
{
    // Reset can race with the draw loop during live reconfiguration or teardown,
    // so it shares the same transport mutex as Show()/ApplyConfig().
    std::lock_guard guard(WS281xGFX::TransportMutex());

    for (size_t i = 0; i < _channels.size(); ++i)
        ReleaseChannel(i);

    _activeChannelCount = 0;
    _activeLEDCount = 0;
    _colorOrder = DeviceConfig::GetCompiledWS281xColorOrder();
}

SuccessResultWithMessage WS281xOutputManager::RecreateChannel(size_t channelIndex, int8_t pin, size_t ledCount)
{
    auto& state = _channels[channelIndex];
    // BytesPerPixel comes from the chip-specific PixelFormat (3 for WS2812,
    // 4 for SK6812, 5 for SM16825/WS2805).
    const auto byteCount = ledCount * _format->BytesPerPixel();

    // A channel recreate is the "hard" reconfigure path: tear down any existing
    // RMT binding, resize the packed byte buffer if LED count changed, then
    // install a fresh RMT TX channel on the new GPIO.

    if (state.installed)
        ReleaseChannel(channelIndex);

    if (!state.outputBytes || state.byteCount != byteCount)
    {
        // The legacy driver DMAs from this buffer (so it MUST live in
        // DMA-capable internal RAM) and driver_ng's non-DMA mode does fine
        // with the same allocation. With the PSRAM-by-default policy in
        // main.cpp, plain std::make_unique<uint8_t[]>(byteCount) lands
        // buffers above the threshold in PSRAM, which fails on the legacy
        // path with:
        //   "rmt: Using buffer allocated from psram"  -> ESP_ERR_INVALID_ARG
        // heap_caps_malloc with DMA+INTERNAL pins it correctly for both
        // drivers, so we use the same allocator either way.
#if WS281X_USE_PARLIO
        // PARLIO only reads these with the CPU while encoding its own DMA frame,
        // so keep them out of scarce internal RAM.
        auto* mem = static_cast<uint8_t*>(heap_caps_malloc_prefer(byteCount, 2,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
        auto* mem = static_cast<uint8_t*>(heap_caps_malloc(byteCount,
                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
        if (!mem)
            return { false, "failed to allocate DMA-capable WS281x byte buffer" };

        std::fill_n(mem, byteCount, 0);
        // unique_ptr<uint8_t[]> default deleter calls free(), which is the
        // correct deallocator for heap_caps_malloc'd memory on ESP-IDF.
        state.outputBytes.reset(mem);
        state.byteCount = byteCount;
    }

    auto [channelConfigured, channelConfigureError] = _transport->ConfigureChannel(channelIndex, static_cast<gpio_num_t>(pin), byteCount);
    if (!channelConfigured)
        return { false, channelConfigureError };

    state.pin = pin;
    state.ledCount = ledCount;
    state.installed = true;
    state.active = true;
    return { true, "" };
}

void WS281xOutputManager::ReleaseChannel(size_t channelIndex)
{
    auto& state = _channels[channelIndex];

    // Wait for any in-flight frame to finish before tearing down the
    // transport so a live pin/channel change does not pull the driver out
    // from under Show().

    if (state.installed)
    {
        _transport->ReleaseChannel(channelIndex);
        state.installed = false;
    }

    if (state.pin >= 0)
    {
        pinMode(state.pin, OUTPUT);
        digitalWrite(state.pin, LOW);
    }

    state.pin = -1;
    state.ledCount = 0;
    state.active = false;
}

SuccessResultWithMessage WS281xOutputManager::ApplyConfig(const DeviceConfig& config, const std::vector<std::shared_ptr<GFXBase>>& devices)
{
    // ApplyConfig and Show share the transport mutex so GPIO/channel changes are
    // atomic with respect to the render thread's transmit path.
    std::lock_guard guard(WS281xGFX::TransportMutex());

    if (config.GetOutputDriver() != DeviceConfig::OutputDriver::WS281x)
        return { false, "recompile needed" };

    const size_t channelCount = std::min(config.GetChannelCount(), devices.size());
    const auto& pins = config.GetWS281xPins();

    // Walk the full compile-time channel array every apply:
    // - active entries are recreated only if pin/length/install state changed
    // - inactive entries are explicitly released so old GPIO bindings disappear
    // Per-channel LED counts come from DeviceConfig::GetChannelLEDCount(channel): matrix layouts
    // hand every channel width*height, individual-strip layouts hand each its own length.
    size_t totalActiveLEDCount = 0;

    for (size_t i = 0; i < _channels.size(); ++i)
    {
        const bool shouldBeActive = i < channelCount;
        if (!shouldBeActive)
        {
            ReleaseChannel(i);
            continue;
        }

        const size_t channelLEDCount = config.GetChannelLEDCount(i);
        if (channelLEDCount == 0)
        {
            ReleaseChannel(i);
            continue;
        }

        auto& state = _channels[i];
        if (!state.active || !state.installed || state.pin != pins[i] || state.ledCount != channelLEDCount)
        {
            auto [channelRecreated, recreateError] = RecreateChannel(i, pins[i], channelLEDCount);
            if (!channelRecreated)
                return { false, recreateError };
        }

        totalActiveLEDCount += channelLEDCount;
    }

    _activeChannelCount = channelCount;
    _activeLEDCount = totalActiveLEDCount;
    _colorOrder = config.GetWS281xColorOrder();

    LogRuntimeWS281xConfiguration(config, devices, "apply");

    return { true, "" };
}

void WS281xOutputManager::Show(const std::vector<std::shared_ptr<GFXBase>>& devices, uint16_t pixelsDrawn, uint8_t brightness, uint8_t fader)
{
    // The same mutex used by ApplyConfig() keeps live transport mutations from
    // colliding with the draw loop while it is filling buffers or transmitting.

    std::lock_guard guard(WS281xGFX::TransportMutex());

    if (_activeChannelCount == 0 || _activeLEDCount == 0)
        return;

    // First build packed output bytes for every active channel.  The GFX layer
    // owns CRGB frame buffers; the runtime transport owns these temporary-once-
    // per-channel packed bytes that match the selected color order.

    std::array<FrameChannel, NUM_CHANNELS> frame;
    size_t frameCount = 0;

    for (size_t channelIndex = 0; channelIndex < _activeChannelCount && channelIndex < devices.size(); ++channelIndex)
    {
        auto& state = _channels[channelIndex];
        if (!state.active || !state.installed || !state.outputBytes)
            continue;

        const auto& device = devices[channelIndex];
        auto* output = state.outputBytes.get();

        // Each channel uses its own LED count, not the cross-channel total. For matrix layouts
        // this matches width*height, so behaviour is identical to the pre-individual-strips path.
        const size_t channelLEDCount = state.ledCount;
        const size_t channelPixelsToShow = std::min(static_cast<size_t>(pixelsDrawn), channelLEDCount);

        // Delegate to the chip-specific format. Passes the optional whites
        // plane (nullptr for plain WS2812 builds; populated by setPixelCCT /
        // setPixelWhite calls on SK6812+ builds). cctKelvin, ambient white,
        // and white-extract ratio defaults are baked in here pending the
        // DeviceConfig knobs landing in a follow-up commit. Each can be
        // overridden at build time via a -D in the env's build_src_flags.

        #ifndef NIGHTDRIVER_DEFAULT_CCT_KELVIN
            #define NIGHTDRIVER_DEFAULT_CCT_KELVIN 4000
        #endif
        #ifndef NIGHTDRIVER_DEFAULT_AMBIENT_CW
            #define NIGHTDRIVER_DEFAULT_AMBIENT_CW 0
        #endif
        #ifndef NIGHTDRIVER_DEFAULT_AMBIENT_WW
            #define NIGHTDRIVER_DEFAULT_AMBIENT_WW 0
        #endif

        // SK6812_WHITE_EXTRACT_RATIO: 0..255, fraction of shared-portion
        // white pulled into the dedicated W LED

        #ifndef SK6812_WHITE_EXTRACT_RATIO
            #define SK6812_WHITE_EXTRACT_RATIO 128
        #endif

        constexpr uint16_t kDefaultCctKelvin   = NIGHTDRIVER_DEFAULT_CCT_KELVIN;
        constexpr uint8_t  kDefaultAmbientCw   = NIGHTDRIVER_DEFAULT_AMBIENT_CW;
        constexpr uint8_t  kDefaultAmbientWw   = NIGHTDRIVER_DEFAULT_AMBIENT_WW;
        constexpr uint8_t  kDefaultExtractRatio = SK6812_WHITE_EXTRACT_RATIO;

        _format->Pack(output,
                      device->leds,
                      device->whites,                 // may be nullptr
                      channelLEDCount,
                      channelPixelsToShow,
                      brightness, fader,
                      _colorOrder,
                      kDefaultCctKelvin,
                      kDefaultAmbientCw,
                      kDefaultAmbientWw,
                      kDefaultExtractRatio);

        frame[frameCount++] = { channelIndex, output, state.byteCount, state.pin, state.ledCount };
    }

    const auto showStartMicros = micros();
    _transport->ShowFrame(frame.data(), frameCount);

    const auto showElapsedMicros = micros() - showStartMicros;
    if (showElapsedMicros > 50000UL)
    {
        debugW("WS281x runtime show slow: channels=%zu leds=%zu elapsed=%lu us",
               _activeChannelCount,
               _activeLEDCount,
               static_cast<unsigned long>(showElapsedMicros));
    }
}

#endif
