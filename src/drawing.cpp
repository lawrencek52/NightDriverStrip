//+--------------------------------------------------------------------------
//
// File:        drawing.cpp
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
// Description:
//
//    Main draw loop and rendering code
//
// History:     May-11-2021         Davepl      Commented
//              Nov-02-2022         Davepl      Broke up into multiple functions
//
//---------------------------------------------------------------------------

#include "globals.h"

#include <algorithm>
#include <ArduinoOTA.h>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>

#include "colordata.h"
#include "ledbuffer.h"
#include "nd_network.h"
#include "ntptimeclient.h"
#include "renderservice.h"
#include "systemcontainer.h"
#include "taskmgr.h"   // DRAWING_STACK_SIZE / DRAWING_PRIORITY / DRAWING_CORE

#if ENABLE_AUDIO
#include "audioservice.h"
#endif

#include "effects/matrix/spectrumeffects.h"

static DRAM_ATTR CRGB l_SinglePixel = CRGB::Blue;

// When each channel last drew a remote frame. Per channel rather than per
// device, because a multi-channel device can be fed by several senders at once
// (the companion NDSCPP server opens a connection per strip): one strip going
// quiet should return that strip to its local effect without disturbing the
// others, and one strip receiving frames should no longer freeze the rest.

static DRAM_ATTR std::array<uint64_t, NUM_CHANNELS> l_usLastWifiDraw = {};
static DRAM_ATTR bool l_WiFiActivityActive = false;
static uint32_t l_FrameCountThisSecond = 0;
static uint32_t l_LastSecondBoundaryMs = 0;

static uint32_t MicrosSinceLastWifiDraw(size_t channel)
{
    return micros() - static_cast<uint32_t>(l_usLastWifiDraw[channel]);
}

// A channel counts as remotely fed until its stream has been quiet for
// TIME_BEFORE_LOCAL; a channel that has never seen a frame never counts.

static bool IsChannelWifiFed(size_t channel)
{
    return l_usLastWifiDraw[channel] != 0 &&
           MicrosSinceLastWifiDraw(channel) <= (TIME_BEFORE_LOCAL * MICROS_PER_SECOND);
}

// ChannelsNeedingLocalDraw
//
// One bit per channel that has no remote frames arriving and should therefore
// be showing its own effect. Zero means every strip is being fed remotely, in
// which case the local effects are skipped entirely - which is what keeps a
// fully remote-driven device from paying for renders nobody sees.

static uint32_t ChannelsNeedingLocalDraw()
{
    uint32_t channelMask = 0;

    for (size_t channel = 0; channel < l_usLastWifiDraw.size(); channel++)
        if (!IsChannelWifiFed(channel))
            channelMask |= 1u << channel;

    return channelMask;
}

#if ENABLE_AUDIO

// AudioNeededForActiveEffects
//
// Only a handful of effects are actually built around the analyzer (see
// LEDStripEffect::RequiresAudio()), and channels being fed remotely never draw
// a local effect at all. Both are checked so the PDM mic - and the GDMA channel
// it shares with the RMT strip output - only runs when something needs it.

static bool AudioNeededForActiveEffects(uint32_t channelsNeedingLocalDraw)
{
    if (channelsNeedingLocalDraw == 0)
        return false;

    auto& effectManager = g_ptrSystem->GetEffectManager();

    if (!effectManager.AreChannelsIndependent())
        return effectManager.HasCurrentEffect() && effectManager.GetCurrentEffect().RequiresAudio();

    for (size_t channel = 0; channel < EffectManager::ChannelCount(); channel++)
    {
        if (!(channelsNeedingLocalDraw & (1u << channel)))
            continue;

        auto effect = effectManager.EffectAt(effectManager.GetChannelEffectIndex(channel));
        if (effect && effect->RequiresAudio())
            return true;
    }

    return false;
}

// UpdateAudioServiceForActiveEffects
//
// Starts/stops the audio sampler to match AudioNeededForActiveEffects(), reusing
// AudioService::Reconfigure()'s existing stop/start and no-op-if-unchanged logic.
// FromCurrentSettings() re-reads the persisted pin/mode, same as the normal
// boot-time and settings-change enable paths.

static void UpdateAudioServiceForActiveEffects(uint32_t channelsNeedingLocalDraw)
{
    auto& audioService = g_ptrSystem->GetAudioService();
    const bool needed = AudioNeededForActiveEffects(channelsNeedingLocalDraw);

    if (needed == audioService.IsRunning())
        return;

    audioService.Reconfigure(needed ? AudioConfig::FromCurrentSettings() : AudioConfig{});
}

#endif // ENABLE_AUDIO

#if WIFI_ACTIVITY_PIN >= 0
static bool IsWiFiDrawWindowActive()
{
    for (size_t channel = 0; channel < l_usLastWifiDraw.size(); channel++)
        if (IsChannelWifiFed(channel))
            return true;

    return false;
}

static void SetWiFiActivityPin(bool active)
{
    if (l_WiFiActivityActive == active)
        return;

    digitalWrite(WIFI_ACTIVITY_PIN, active ? HIGH : LOW);
    l_WiFiActivityActive = active;
}

static void PrepareWiFiActivityPin()
{
    pinMode(WIFI_ACTIVITY_PIN, OUTPUT);
    digitalWrite(WIFI_ACTIVITY_PIN, LOW);
    l_WiFiActivityActive = false;
}

static void UpdateWiFiActivityPin(uint16_t wifiPixelsDrawn, uint16_t localPixelsDrawn)
{
    if (wifiPixelsDrawn > 0)
    {
        SetWiFiActivityPin(true);
        return;
    }

    if (localPixelsDrawn > 0 || !IsWiFiDrawWindowActive())
        SetWiFiActivityPin(false);
}
#else
static void PrepareWiFiActivityPin()
{
}

static void UpdateWiFiActivityPin(uint16_t, uint16_t)
{
}

static void SetWiFiActivityPin(bool)
{
}
#endif

// The g_buffer_mutex is a global mutex used to protect access while adding or removing frames
// from the led buffer.

std::shared_ptr<LEDStripEffect> GetSpectrumAnalyzer(CRGB color);    // Defined in effectmanager.cpp

// WiFiDraw
//
// Draws from WiFi color data if available, returns pixels drawn this frame

uint16_t WiFiDraw()
{
    // Builds with INCOMING_WIFI_ENABLED=0 never create the buffer managers,
    // but this path is still reachable whenever WiFi itself is connected.
    if (!g_ptrSystem->HasBufferManagers())
        return 0;

    std::lock_guard guard(g_buffer_mutex);

    uint16_t pixelsDrawn = 0;
    size_t channel = 0;

    for (auto& bufferManager : g_ptrSystem->GetBufferManagers())
    {
        const size_t thisChannel = channel++;

        timeval tv;
        gettimeofday(&tv, nullptr);

        // Pull buffers out of the queue.

        if (false == bufferManager.IsEmpty())
        {
            std::shared_ptr<LEDBuffer> pBuffer;
            #if ENABLE_NTP
            if (NTPTimeClient::HasClockBeenSet() == false)
            {
                pBuffer = bufferManager.GetOldestBuffer();
            }
            else
            {
                // Using a 'while' rather than an 'if' causes it to pull frames until it's caught up
                // written as 'while' it will pull frames until it gets one that is current.
                // Chew through ALL frames older than now, ignoring all but the last of them

                while (!bufferManager.IsEmpty() && bufferManager.PeekOldestBuffer()->IsBufferOlderThan(tv))
                    pBuffer = bufferManager.GetOldestBuffer();
            }
            #else
            pBuffer = bufferManager.GetOldestBuffer();
            #endif

            if (pBuffer)
            {
                // Stamped per channel: this strip is remotely fed, whatever the
                // others are doing.
                if (thisChannel < l_usLastWifiDraw.size())
                    l_usLastWifiDraw[thisChannel] = micros();

                debugV("Calling LEDBuffer::Draw from wire with %d/%zu pixels.", pixelsDrawn, pBuffer->_pStrand->GetLEDCount());
                pBuffer->DrawBuffer();
                // In case we drew some pixels and then drew 0 due a failure, we want to return a positive
                // number of pixels drawn so the caller knows we did in fact render.
                pixelsDrawn += pBuffer->Length();
            }
        }
    }
    debugV("WifIDraw claims to have drawn %d pixels", pixelsDrawn);
    return pixelsDrawn;
}

// LocalDraw
//
// Draws from effects table rather than from WiFi data.  Returns the number of LEDs rendered.
//
// channelMask says which strips still want their local effect; the strips being
// fed remote frames are left out of it. Per-channel playback honors the mask
// exactly, while one shared effect instance paints every strip it owns - the
// draw loop handles that case by drawing remote frames afterwards, on top.

uint16_t LocalDraw(uint32_t channelMask)
{
    // Every strip is being fed remotely, so there is nothing local to draw.
    // Returning 0 also tells the caller no pixels were rendered, which is what
    // keeps it from needlessly pushing the strip - that costs real time.

    if (channelMask == 0)
    {
        debugV("Not drawing local effects because every channel is being fed remotely");
        return 0;
    }

    if (!g_ptrSystem->HasEffectManager())
    {
        debugW("Drawing before EffectManager is ready, so delaying...");
        delay(100);
        return 0;
    }
    else
    {
        auto& effectManager = g_ptrSystem->GetEffectManager();

        if (effectManager.HasCurrentEffect())
        {
            effectManager.Update(channelMask);      // Draw the current built in effect

            #if SHOW_VU_METER
                #if ENABLE_AUDIO
                    static auto spectrum = std::static_pointer_cast<SpectrumAnalyzerEffect>(GetSpectrumAnalyzer(0));
                    if (effectManager.IsVUVisible())
                        spectrum->DrawVUMeter(g_ptrSystem->GetEffectManager().GetBaseGraphics(), 0, g_Analyzer.IsRemoteAudioActive() ? & vuPaletteBlue : &vuPaletteGreen);
                #endif
            #endif

            const auto activeLEDCount = g_ptrSystem->GetEffectManager().g().GetLEDCount();
            debugV("LocalDraw claims to have drawn %zu pixels", activeLEDCount);
            return activeLEDCount;
        }
    }

    debugV("Local draw not drawing");
    return 0;
}

// CalcDelayUntilNextFrame
//
// Returns the amount of time to wait patiently until it's time to draw the next frame, up to one second max

int CalcDelayUntilNextFrame(double frameStartTime, uint16_t localPixelsDrawn, uint16_t wifiPixelsDrawn)
{
    // Delay enough to slow down to the desired framerate

#if MILLIS_PER_FRAME == 0

    constexpr auto kMinDelay = 0.001;

    if (localPixelsDrawn > 0)
    {
        double fpsRaw = 0.0;
        {
            std::lock_guard effectGuard(g_effect_manager_mutex);
            auto& effectManager = g_ptrSystem->GetEffectManager();
            // Paces on the fastest channel when the channels run different effects;
            // each channel is then gated to its own rate inside EffectManager::Update().
            if (effectManager.HasCurrentEffect())
                fpsRaw = static_cast<double>(effectManager.GetDesiredFramesPerSecond());
        }
        // If FPS is invalid (<= 0 or non-finite), treat as unlimited (0s minimum frame time).
        const double minimumFrameTime = (!std::isfinite(fpsRaw) || fpsRaw <= 0.0) ? 0.0 : (1.0 / fpsRaw);
        // Use a monotonic-like elapsed (never negative) in case wall clock adjustments go backward.
        const double elapsed = std::max(0.0, g_Values.AppTime.CurrentTime() - frameStartTime);
        // Always recompute FreeDrawTime so it cannot "stick" to a prior large value.
        g_Values.FreeDrawTime = std::clamp(minimumFrameTime - elapsed, 0.0, 1.0);
    }
    else if (wifiPixelsDrawn > 0)
    {
        // Look through all the channels to see how far away the next wifi frame is times for.  We can then delay
        // up to the minimum value found across all buffer managers.

        double t = std::numeric_limits<double>::max();
        bool bFoundFrame = false;

        {
            // The socket task can add frames while the render task is
            // calculating its next sleep. Protect this read-only peek with the
            // same mutex used by enqueue/dequeue so the ring indices and
            // timestamps are sampled consistently.

            std::lock_guard guard(g_buffer_mutex);
            for (auto& bufferManager : g_ptrSystem->GetBufferManagers())
            {
                auto pOldest = bufferManager.PeekOldestBuffer();
                if (pOldest)
                {
                    // TimeTillDue() should be non-negative for future-due frames; if negative (stale), treat as now.
                    // Note I'm not using clamp since clamp can return nan if TimeTillDue does, whereas this guards against that.
                    t = std::min(t, std::max(0.0, pOldest->TimeTillDue()));
                    bFoundFrame = true;
                }
            }
        }
        // Bound the delay to at most 1 second to avoid pathological multi-second sleeps.
        g_Values.FreeDrawTime = bFoundFrame ? std::clamp(t, 0.0, 1.0) : kMinDelay;
    }
    else
    {
        debugV("Nothing drawn this pass because neither wifi nor local rendered a frame");
        // Nothing drawn this pass - check back soon
        g_Values.FreeDrawTime = kMinDelay;
    }

    return g_Values.FreeDrawTime * MILLIS_PER_SECOND;

#else

    // Fixed cadence: a build that pins MILLIS_PER_FRAME wants every frame to take
    // that long regardless of what was drawn, so we just sleep off whatever is left
    // of the period. Bounded at one second like the adaptive path above.

    (void) localPixelsDrawn;
    (void) wifiPixelsDrawn;

    const double elapsed = std::max(0.0, g_Values.AppTime.CurrentTime() - frameStartTime);
    g_Values.FreeDrawTime = std::clamp(MILLIS_PER_FRAME / (double) MILLIS_PER_SECOND - elapsed, 0.0, 1.0);

    return g_Values.FreeDrawTime * MILLIS_PER_SECOND;

#endif
}

// ShowOnboardLED
//
// If the board has an onboard LED, this will update it to show some activity from the draw

void ShowOnboardRGBLED()
{
    // Some boards have onboard PWM RGB LEDs, so if defined, we color them here.  If we're doing audio,
    // the color maps to the sound level.  If no audio, it shows the middle LED color from the strip.

    #if ONBOARD_LED_R
        #if ENABLE_AUDIO
            CRGB c = ColorFromPalette(HeatColors_p, g_Analyzer.VURatioFade() / 2.0 * 255);
            ledcWrite(1, 255 - c.r); // write red component to channel 1, etc.
            ledcWrite(2, 255 - c.g);
            ledcWrite(3, 255 - c.b);
        #else
            const auto& graphics = *g_ptrSystem->GetDevices()[0];
            int iLed = graphics.GetLEDCount() / 2;
            ledcWrite(1, 255 - graphics.leds[iLed].r); // write red component to channel 1, etc.
            ledcWrite(2, 255 - graphics.leds[iLed].g);
            ledcWrite(3, 255 - graphics.leds[iLed].b);
        #endif
    #endif
}

// PrepareOnboardPixel
//
// Do any setup required for the onboard pixel, if we have one

void PrepareOnboardPixel()
{
    #ifdef ONBOARD_PIXEL_POWER
        FastLED.addLeds<WS2812B, ONBOARD_PIXEL_DATA, ONBOARD_PIXEL_ORDER>(&l_SinglePixel, 1);
        pinMode(ONBOARD_PIXEL_POWER, OUTPUT);
        digitalWrite(ONBOARD_PIXEL_POWER, HIGH);
    #endif
}

void ShowOnboardPixel()
{
    // Some boards have onboard PWM RGB LEDs, so if defined, we color them here.  If we're doing audio,
    // the color maps to the sound level.  If no audio, it shows the middle LED color from the strip.

    #ifdef ONBOARD_PIXEL_POWER
        l_SinglePixel = FastLED[0].leds()[0];
    #endif
}

// RenderService ITaskService hooks
//
// Start/Stop/IsRunning are inherited final from ITaskService; this class
// only supplies the task config and the per-frame render loop body. The
// The render task is pinned to DRAWING_CORE to isolate the display workload
// from audio sampling and other timing-sensitive services.

ITaskService::TaskConfig RenderService::GetTaskConfig() const
{
    return TaskConfig {
        "Draw Loop",
        DRAWING_STACK_SIZE,
        DRAWING_PRIORITY,
        DRAWING_CORE,
        2000   // Stop timeout: loop yields up to 1s in CalcDelayUntilNextFrame.
    };
}

// RenderService::Run
//
// Main draw loop. Calls WiFiDraw / LocalDraw, runs PostProcessFrame, and
// updates the FPS window. Holds the global render mutex for the duration
// of each frame so runtime topology/output changes can't reconfigure the
// active buffers mid-frame. Polls ShouldShutdown() between frames so a
// Stop() in OTA / shutdown can break the loop cleanly.

void IRAM_ATTR RenderService::Run()
{
    debugW(">> RenderService::Run\n");

    // If this board has an onboard RGB pixel, set it up now

    PrepareOnboardPixel();
    PrepareWiFiActivityPin();

    // Start the effect

    g_ptrSystem->GetEffectManager().StartEffect();

    // Run the draw loop

    debugW("Entering main draw loop!");

    while (!ShouldShutdown())
    {
        g_Values.AppTime.NewFrame();

        uint16_t localPixelsDrawn   = 0;
        uint16_t wifiPixelsDrawn    = 0;
        double frameStartTime       = g_Values.AppTime.FrameStartTime();

        {
            // Hold the render and effect-manager mutexes together for the whole
            // frame so that runtime topology/output changes cannot reconfigure the
            // active buffers mid-frame. We MUST acquire them atomically with
            // scoped_lock here because every other site that needs both (web
            // handlers, remote control, runtime reconfig) acquires them via
            // std::scoped_lock too. Taking them individually here (R first, then
            // E later inside EffectManager getters) would form an AB-BA cycle
            // with std::lock's adaptive ordering and wedge the AsyncTCP task on
            // the first API call -- which manifests as total loss of network
            // connectivity even though WiFi association is still up.

            std::scoped_lock renderGuard(g_render_mutex, g_effect_manager_mutex);
            auto& graphics = *g_ptrSystem->GetDevices()[0];

            graphics.PrepareFrame();

            // Strips whose sender has gone quiet (or never had one) fall back to
            // their own effect, while the strips still being fed keep showing
            // what arrives. Local goes first and remote paints over the top of
            // it, which is what makes the mixed case work at all: in shared mode
            // the single effect instance necessarily covers every strip it owns,
            // so ordering - not the mask - is what protects the remote-fed ones.
            //
            // Powered off (IR remote) forces the mask to 0 regardless of what
            // ChannelsNeedingLocalDraw() says, so local effects stay suspended -
            // WiFiDraw() below still runs unconditionally, so a channel being fed
            // by LED Central lights right back up even while "off".
            //
            // The first frame after PowerOff() is special-cased: PostProcessFrame()
            // treats "0 pixels drawn" as nothing to transmit, so the buffer
            // PowerOff() already zeroed would otherwise never reach the physical
            // strip - it would just keep showing its last real frame. Reporting a
            // real (non-zero) count for that one frame forces the (already black)
            // buffer to actually transmit, then subsequent frames go back to 0/idle.
            // The count has to be the same per-channel figure LocalDraw() reports,
            // because that is how PostProcessFrame() interprets it - not the
            // all-channels total from DeviceConfig::GetActiveLEDCount(), which both
            // over-claims and can wrap the uint16_t on a large multi-channel rig.

            auto& effectManager = g_ptrSystem->GetEffectManager();
            if (effectManager.IsPoweredOn())
                localPixelsDrawn = LocalDraw(ChannelsNeedingLocalDraw());
            else if (effectManager.ConsumePendingBlankFrame())
                localPixelsDrawn = static_cast<uint16_t>(effectManager.g().GetLEDCount());
            else
                localPixelsDrawn = 0;

            if (nd_network::IsWiFiConnected())
                wifiPixelsDrawn = WiFiDraw();

            // If we drew any pixels by any method, we'll call that a frame and track it for FPS purposes.  We also notify the
            // color data thread that a new frame is available and can be transmitted to clients

            if (wifiPixelsDrawn + localPixelsDrawn > 0)
            {
                // If the module has onboard LEDs, we support a couple of different types, and we set it to be the same as whatever
                // is on LED #0 of Channel #0.

                ShowOnboardPixel();
                ShowOnboardRGBLED();

                ++l_FrameCountThisSecond;
                g_ptrSystem->GetEffectManager().ReportNewFrameAvailable();
            }

            // Count actual frames emitted by the draw loop over completed
            // clock-second windows. This avoids FastLED's internal estimate
            // and lets an idle device report 0 after a full second passes.
            const uint32_t nowMs = millis();
            if (l_LastSecondBoundaryMs == 0)
                l_LastSecondBoundaryMs = nowMs;
            while (nowMs - l_LastSecondBoundaryMs >= MILLIS_PER_SECOND)
            {
                g_Values.FPS = l_FrameCountThisSecond;
                l_FrameCountThisSecond = 0;
                l_LastSecondBoundaryMs += MILLIS_PER_SECOND;
            }

            graphics.PostProcessFrame(localPixelsDrawn, wifiPixelsDrawn);
            UpdateWiFiActivityPin(wifiPixelsDrawn, localPixelsDrawn);
        }

        // Delay at least 2ms and not more than 1s until next frame is due

        constexpr auto minimumDelay = 5;
        delay( std::max(minimumDelay, CalcDelayUntilNextFrame(frameStartTime, localPixelsDrawn, wifiPixelsDrawn) ));

        // Once an OTA flash update has started, we don't want to hog the CPU or it goes quite slowly,
        // so we'll slow down to share the CPU a bit once the update has begun

        if (g_Values.UpdateStarted)
            delay(500);
    }

    SetWiFiActivityPin(false);
}
