//+--------------------------------------------------------------------------
//
// File:        effectmanager_playback.cpp
//
// This file is part of effectmanager.cpp; see that file header for additional context.
//
// Split scope: EffectManager playback timing, index/palette navigation, and default-load control.
//---------------------------------------------------------------------------


#include "globals.h"

#include <algorithm>
#include <FS.h>
#include <limits>
#include <set>
#include <SPIFFS.h>

#include "deviceconfig.h"
#include "effectfactories.h"
#include "effectmanager.h"
#include "gfxbase.h"
#include "jsonserializer.h"
#include "ledstripeffect.h"
#include "systemcontainer.h"
#include "websocketserver.h"

#include "effects/strip/misceffects.h"
#include "effects/strip/musiceffect.h"
#if USE_HUB75
#include "hub75gfx.h"
#endif

extern allocated_unique_ptr<EffectFactories> g_ptrEffectFactories;
void SaveEffectManagerConfig();

void EffectManager::SetCurrentEffectIndex(size_t i)
{
    SetEffectIndex(kAllChannels, i);
}

// ResetChannelRateForNewEffect
//
// A frame rate override is a statement about one effect - "run Fire on strip 2 at
// 5fps" - so it doesn't carry over to whatever replaces that effect. Selecting a
// different effect for a strip hands the rate back to the new effect's
// DesiredFramesPerSecond(); re-selecting the effect a strip is already showing
// leaves the override alone, since nothing new was picked.

bool EffectManager::ResetChannelRateForNewEffect(ChannelPlayback& channel, size_t effectIndex)
{
    // In shared mode the strip is showing _iCurrentEffect, not channel.index:
    // rotation advances the shared index without touching the per-channel ones,
    // so channel.index is stale until the channel is actually pinned.
    const size_t playing = _channelsIndependent ? channel.index : _iCurrentEffect;

    if (playing == effectIndex || channel.fpsOverride == 0)
        return false;

    channel.fpsOverride = 0;
    return true;
}

// SetEffectIndex
//
// Points one channel, or every channel, at an effect. Selecting for all channels
// collapses back to a single shared effect instance with rotation and cross-fade
// running, which is the behavior the device has always had. Selecting for one
// channel switches to independent mode, where each strip draws its own clone.
// Either way, a strip that changes effect gives up its frame rate override.

bool EffectManager::SetEffectIndex(int channel, size_t effectIndex)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (effectIndex >= _vEffects.size())
    {
        debugW("Invalid effect index %zu for SetEffectIndex", effectIndex);
        return false;
    }

    if (channel == kAllChannels)
    {
        // Before ResumeSharedPlayback(), which drops independent mode and with it
        // the per-channel indices this compares against.
        bool clearedRates = false;
        for (auto& ch : _channels)
            clearedRates |= ResetChannelRateForNewEffect(ch, effectIndex);

        ResumeSharedPlayback();

        for (auto& ch : _channels)
            ch.index = effectIndex;

        _iCurrentEffect = effectIndex;
        _effectStartTime = millis();

        StartEffect();
        SaveCurrentEffectIndex();

        // SaveCurrentEffectIndex() only writes the index; the cleared rates live in
        // the effect manager config, which is worth rewriting only if one changed.
        if (clearedRates)
            SaveEffectManagerConfig();

        {
            std::lock_guard listenerGuard(_listenerMutex);
            INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnCurrentEffectChanged, effectIndex);
        }

        return true;
    }

    if (channel < 0 || static_cast<size_t>(channel) >= _channels.size())
    {
        debugW("Invalid channel %d for SetEffectIndex", channel);
        return false;
    }

    const auto channelIndex = static_cast<size_t>(channel);
    const size_t previousIndex = _channels[channelIndex].index;
    const uint previousRate = _channels[channelIndex].fpsOverride;

    ResetChannelRateForNewEffect(_channels[channelIndex], effectIndex);
    _channels[channelIndex].index = effectIndex;

    if (!_channelsIndependent)
    {
        // First per-channel pin. Every other channel keeps showing what it was
        // already showing, which is the shared effect.
        for (size_t i = 0; i < _channels.size(); i++)
            if (i != channelIndex)
                _channels[i].index = _iCurrentEffect;

        if (!EnterIndependentMode())
        {
            _channels[channelIndex].index = previousIndex;
            _channels[channelIndex].fpsOverride = previousRate;
            debugW("Could not build per-channel effects, so staying in shared mode");
            return false;
        }
    }
    else
    {
        auto effect = MakeChannelEffect(channelIndex, effectIndex);

        if (!effect)
        {
            _channels[channelIndex].index = previousIndex;
            _channels[channelIndex].fpsOverride = previousRate;
            return false;
        }

        _channels[channelIndex].effect = std::move(effect);
        _channels[channelIndex].nextDrawMs = millis();
        ClearChannelWhites(_channels[channelIndex]);
    }

    SaveEffectManagerConfig();

    {
        std::lock_guard listenerGuard(_listenerMutex);
        INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnCurrentEffectChanged, effectIndex);
    }

    return true;
}

// SetChannelFrameRate
//
// Overrides how often a channel is drawn, independent of what its effect asks for
// via DesiredFramesPerSecond(). 0 hands the channel back to the effect's own rate.
// Only meaningful in independent mode; in shared mode the one effect's rate governs.

bool EffectManager::SetChannelFrameRate(int channel, uint fps)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    // A cap well above any LED strip's practical refresh rate, just to keep a
    // fat-fingered API call from turning into a divide-by-tiny-interval spin.
    constexpr uint maxFrameRate = 1000;

    if (fps > maxFrameRate)
    {
        debugW("Frame rate %u out of range for channel %d", fps, channel);
        return false;
    }

    if (channel == kAllChannels)
    {
        for (auto& ch : _channels)
            ch.fpsOverride = fps;
    }
    else if (channel < 0 || static_cast<size_t>(channel) >= _channels.size())
    {
        debugW("Invalid channel %d for SetChannelFrameRate", channel);
        return false;
    }
    else
    {
        _channels[static_cast<size_t>(channel)].fpsOverride = fps;
    }

    SaveEffectManagerConfig();
    return true;
}

void EffectManager::PlayAll(bool bPlayAll)
{
    std::lock_guard effectGuard(g_effect_manager_mutex);
    _bPlayAll = bPlayAll;
}

void EffectManager::SetInterval(uint interval, bool skipSave)
{
    std::lock_guard effectGuard(g_effect_manager_mutex);

    // Reject/ignore intervals smaller than a second, but allow 0 (infinity)
    if (interval > 0 && interval < 1000)
        return;

    _effectInterval = interval;

    if (!skipSave)
        SaveEffectManagerConfig();

    {
        std::lock_guard listenerGuard(_listenerMutex);
        INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnIntervalChanged, interval);
    }
}

uint EffectManager::GetTimeUsedByCurrentEffect() const
{
    std::lock_guard effectGuard(g_effect_manager_mutex);
    return millis() - _effectStartTime;
}

uint EffectManager::GetTimeRemainingForCurrentEffect() const
{
    std::lock_guard effectGuard(g_effect_manager_mutex);
    if (!_tempEffect && _vEffects.empty())
        return 0;

    // If the Interval is set to zero, we treat that as an infinite interval and don't even look at the time used so far
    uint timeUsedByCurrentEffect = GetTimeUsedByCurrentEffect();
    uint interval = GetEffectiveInterval();

    return timeUsedByCurrentEffect > interval ? 0 : (interval - timeUsedByCurrentEffect);
}

uint EffectManager::GetEffectiveInterval() const
{
    std::lock_guard effectGuard(g_effect_manager_mutex);
    if (!_tempEffect && _vEffects.empty())
        return IsIntervalEternal() ? std::numeric_limits<uint>::max() : _effectInterval;

    const size_t index = _vEffects.empty() ? 0 : std::min(_iCurrentEffect, _vEffects.size() - 1);
    auto& currentEffect = *(_tempEffect ? _tempEffect : _vEffects[index]);
    // This allows you to return a MaximumEffectTime and your effect won't be shown longer than that
    return min((IsIntervalEternal() ? std::numeric_limits<uint>::max() : _effectInterval),
               (currentEffect.HasMaximumEffectTime() ? currentEffect.MaximumEffectTime() : std::numeric_limits<uint>::max()));
}

uint EffectManager::GetInterval() const
{
    std::lock_guard effectGuard(g_effect_manager_mutex);
    return _effectInterval;
}

bool EffectManager::IsIntervalEternal() const
{
    std::lock_guard effectGuard(g_effect_manager_mutex);
    return _effectInterval == 0;
}

void EffectManager::NextPalette()
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);
    debugV("EffectManager::NextPalette");
    g().CyclePalette(1);
}

void EffectManager::PreviousPalette()
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);
    debugV("EffectManager::PreviousPalette");
    g().CyclePalette(-1);
}

void EffectManager::LoadDefaultEffects()
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    _effectSetHashString = g_ptrEffectFactories->HashString();

    for (const auto &numberedFactory : g_ptrEffectFactories->GetDefaultFactories())
    {
        auto pEffect = numberedFactory.CreateEffect();
        if (pEffect)
        {
            // Effects in the default list are core effects. These can be disabled but not deleted.
            pEffect->MarkAsCoreEffect();
            _vEffects.push_back(pEffect);
        }
    }

    SetInterval(DEFAULT_EFFECT_INTERVAL, true);

    construct(true);
}
