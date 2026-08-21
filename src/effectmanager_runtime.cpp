//+--------------------------------------------------------------------------
//
// File:        effectmanager_runtime.cpp
//
// This file is part of effectmanager.cpp; see that file header for additional context.
//
// Split scope: EffectManager runtime lifecycle, updates, transitions, and effect list mutations.
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

#if ENABLE_AUDIO
#include "effects/matrix/spectrumeffects.h"
#endif

extern allocated_unique_ptr<EffectFactories> g_ptrEffectFactories;
void SaveEffectManagerConfig();

#if ENABLE_AUDIO

std::shared_ptr<LEDStripEffect> GetSpectrumAnalyzer(CRGB color)
{
    CHSV hueColor = rgb2hsv_approximate(color);
    CRGB color2 = CRGB(CHSV(hueColor.hue + 64, 255, 255));
    auto object = make_shared_psram<SpectrumAnalyzerEffect>("Spectrum Clr", 24, CRGBPalette16(color, color2), true);
    if (object->Init(g_ptrSystem->GetDevices()))
        return object;
    throw std::runtime_error("Could not initialize new spectrum analyzer, one color version!");
}

#endif

void EffectManager::StartEffect()
{
    // Acquire both mutexes atomically. Separate sequential lock_guards form
    // an AB-BA cycle with sites that use scoped_lock(render, effect_mgr).
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    // If there's a temporary effect override from the remote control active, we start that, else
    // we start the current regular effect

    if (!_tempEffect && _vEffects.empty())
    {
        _iCurrentEffect = 0;
        _effectStartTime = millis();
        debugV("No effect to start");
        return;
    }

    if (!_vEffects.empty() && _iCurrentEffect >= _vEffects.size())
        _iCurrentEffect = 0;

    auto effect = _tempEffect ? _tempEffect : _vEffects[_iCurrentEffect];

    #if USE_HUB75
        auto& matrix = static_cast<HUB75GFX&>(*_gfx[0]);
        matrix.SetCaption(effect->FriendlyName(), CAPTION_TIME);
    #endif

    // Zero the whites plane on every graphics device at effect-switch.
    //
    // CCT-aware effects (WarmGlowEffect and friends) write to whites[]
    // every frame; effects that don't know about whites write only to
    // leds[]. Without this reset, the previous effect's W-LED state
    // persists through the transition and gets re-rendered by the
    // PixelFormat under the new effect's colors - the strip ends up
    // showing the old warm-white tint behind whatever colors the new
    // effect draws. (leds[] doesn't need to be reset here for the same
    // reason: every well-behaved effect overwrites the pixels it draws
    // each frame, and effects that deliberately persist state across
    // frames - like FireEffect's heat decay - want that persistence.
    // The whites plane has no equivalent persisting-effect right now,
    // so blanket-zeroing it on switch is the right semantic.)

    for (auto& device : _gfx)
    {
        if (device && device->whites)
            memset(device->whites, 0,
                   device->GetLEDCount() * sizeof(CRGBW));
    }

    // With independent channels there is no single effect to start: each channel
    // owns its own instance, so start those instead. A temp effect still wins,
    // since a remote global color is meant to take over the whole device.

    if (_channelsIndependent && !_tempEffect)
    {
        const auto now = millis();

        for (auto& channel : _channels)
        {
            if (channel.effect)
            {
                channel.effect->Start();
                channel.nextDrawMs = now;
            }
        }
    }
    else
    {
        effect->Start();
    }

    _lastBeatSequence = g_Analyzer.LastBeat().sequence;
    _lastNearBeatSequence = g_Analyzer.LastNearBeat().sequence;
    _effectStartTime = millis();
}

void EffectManager::DispatchBeatIfNeeded()
{
#if ENABLE_AUDIO
    std::lock_guard effectGuard(g_effect_manager_mutex);

    // Beat callbacks are sequenced here so every active effect sees the same
    // detector output, including BeatEffectBase-derived effects via OnBeat().
    if (!_tempEffect && _vEffects.empty())
        return;

    // Dispatch to whatever is actually drawing: one shared effect, or every
    // channel's own instance. Missing this in per-channel mode would leave
    // audio-reactive effects on all but the first strip without beats.

    const auto forEachActiveEffect = [this](auto&& action)
    {
        if (_tempEffect || !_channelsIndependent)
        {
            action(GetCurrentEffect());
            return;
        }

        for (auto& channel : _channels)
            if (channel.effect)
                action(*channel.effect);
    };

    const auto nearBeat = g_Analyzer.LastNearBeat();
    if (nearBeat.sequence != 0 && nearBeat.sequence != _lastNearBeatSequence)
    {
        forEachActiveEffect([&](LEDStripEffect& effect) { effect.OnNearBeat(nearBeat); });
        _lastNearBeatSequence = nearBeat.sequence;
    }

    const auto beat = g_Analyzer.LastBeat();
    if (beat.sequence != 0 && beat.sequence != _lastBeatSequence)
    {
        forEachActiveEffect([&](LEDStripEffect& effect) { effect.OnBeat(beat); });
        _lastBeatSequence = beat.sequence;
    }
#endif
}

void EffectManager::SetTempEffect(std::shared_ptr<LEDStripEffect> effect)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);
    _tempEffect = effect;
}

bool EffectManager::Init()
{
    for (const auto & _vEffect : _vEffects)
    {
        debugV("About to init effect %s", _vEffect->FriendlyName().c_str());
        if (false == _vEffect->Init(_gfx))
        {
            debugW("Could not initialize effect: %s\n", _vEffect->FriendlyName().c_str());
            return false;
        }
        debugV("Loaded Effect: %s", _vEffect->FriendlyName().c_str());
    }
    if (_vEffects.empty())
        debugV("No local effects loaded");
    else
        debugV("First Effect: %s", GetCurrentEffectName().c_str());

    if (g_ptrSystem->GetDeviceConfig().ApplyGlobalColors())
        ApplyGlobalPaletteColors();

    return true;
}

bool EffectManager::ReinitializeEffects()
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (!Init())
        return false;

    if (_tempEffect)
    {
        debugV("About to re-init temp effect %s", _tempEffect->FriendlyName().c_str());
        if (!_tempEffect->Init(_gfx))
        {
            debugW("Could not re-initialize temporary effect: %s\n", _tempEffect->FriendlyName().c_str());
            return false;
        }
    }

    // A topology change can hand us a rebuilt device vector and new LED counts, so
    // rebind the channel views and re-init the clones against them. If any clone
    // fails to rebuild we fall back to shared mode rather than run a channel whose
    // effect is still sized for the old topology.

    BindChannelGraphics();

    for (auto& channel : _channels)
    {
        if (channel.effect && !channel.effect->Init(channel.gfx))
        {
            debugW("Could not re-initialize per-channel effect %s; falling back to shared mode",
                   channel.effect->FriendlyName().c_str());
            LeaveIndependentMode();
            break;
        }
    }

    StartEffect();
    return true;
}

bool EffectManager::ShowVU(bool bShow)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    auto& deviceConfig = g_ptrSystem->GetDeviceConfig();
    bool bResult = deviceConfig.ShowVUMeter();
    debugI("Setting ShowVU to %d\n", bShow);
    deviceConfig.SetShowVUMeter(bShow);

    // Erase any exising pixels since effects don't all clear each frame
    if (!bShow)
        _gfx[0]->setPixelsF(0, _gfx[0]->GetMatrixWidth(), CRGB::Black);

    return bResult;
}

bool EffectManager::IsVUVisible() const
{
    std::lock_guard effectGuard(g_effect_manager_mutex);
    return g_ptrSystem->GetDeviceConfig().ShowVUMeter() &&
           (_tempEffect || !_vEffects.empty()) &&
           GetCurrentEffect().CanDisplayVUMeter();
}


void EffectManager::ClearRemoteColor(bool retainRemoteEffect)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (!retainRemoteEffect)
        _tempEffect = nullptr;

    #if USE_HUB75
        g().PausePalette(false);
    #endif

    g_ptrSystem->GetDeviceConfig().ClearApplyGlobalColors();
}

// ApplyGlobalColor
//
// When a global color is set via the remote, we create a fill effect and assign it as the "remote effect"
// which takes drawing precedence

void EffectManager::ApplyGlobalColor(CRGB color)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    debugI("Setting Global Color: %08lX\n", (unsigned long)(uint32_t)color);

    auto& deviceConfig = g_ptrSystem->GetDeviceConfig();
    deviceConfig.SetColorSettings(color, deviceConfig.GlobalColor());

    ApplyGlobalPaletteColors();
}

void EffectManager::ApplyGlobalPaletteColors()
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    #if USE_HUB75
        auto& pMatrix = g();
        auto& deviceConfig = g_ptrSystem->GetDeviceConfig();
        auto& globalColor = deviceConfig.GlobalColor();
        auto& secondColor = deviceConfig.SecondColor();

        // If the two colors are the same, we just shift the palette by 64 degrees to create a palette
        // based from where those colors sit on the spectrum
        if (secondColor == globalColor)
        {
            CHSV hsv = rgb2hsv_approximate(globalColor);
            pMatrix.setPalette(CRGBPalette16(globalColor, CRGB(CHSV(hsv.hue + 64, 255, 255))));
        }
        else
        {
            // But if we have two different colors, we create a palette spread between them
            pMatrix.setPalette(CRGBPalette16(secondColor, globalColor));
        }

        pMatrix.PausePalette(true);
    #endif
}

void EffectManager::construct(bool clearTempEffect)
{
    _bPlayAll = false;

    BindChannelGraphics();

    // Channels start out following the shared current effect. A per-channel pin,
    // or a persisted selection restored by DeserializeFromJSON(), moves them off it.
    for (auto& channel : _channels)
        channel.index = _iCurrentEffect;

    if (clearTempEffect && _tempEffect)
    {
        _clearTempEffectWhenExpired = true;

        // This ensures that we start the correct effect after the temporary one.
        //   The switching to the next effect is taken care of by NextEffect(), which starts with
        //   increasing _iCurrentEffect. We therefore need to set it to the previous effect, to
        //   make sure that the first effect after the temporary one is the one we want (either the
        //   then current one when the chip was powered off, or the one at index 0).
        if (_iCurrentEffect == 0)
            _iCurrentEffect = EffectCount();

        _iCurrentEffect--;
    }
}

//
// Per-channel playback plumbing
//

void EffectManager::BindChannelGraphics()
{
    for (size_t i = 0; i < _channels.size(); i++)
    {
        auto& gfx = _channels[i].gfx;
        gfx.clear();

        // One device per channel. Builds where the device vector is shorter than
        // NUM_CHANNELS shouldn't happen (InitializeHardware allocates one device
        // per channel), but falling back to device 0 keeps us off a null deref.
        if (i < _gfx.size())
            gfx.push_back(_gfx[i]);
        else if (!_gfx.empty())
            gfx.push_back(_gfx[0]);
    }
}

void EffectManager::ClearChannelWhites(const ChannelPlayback& channel)
{
    if (channel.gfx.empty())
        return;

    const auto& device = channel.gfx[0];
    if (device && device->whites)
        memset(device->whites, 0, device->GetLEDCount() * sizeof(CRGBW));
}

uint EffectManager::EffectiveFrameRate(const ChannelPlayback& channel)
{
    if (channel.fpsOverride > 0)
        return channel.fpsOverride;

    return channel.effect ? channel.effect->DesiredFramesPerSecond() : 0;
}

std::shared_ptr<LEDStripEffect> EffectManager::MakeChannelEffect(size_t channel, size_t index)
{
    if (channel >= _channels.size() || index >= _vEffects.size() || _channels[channel].gfx.empty())
        return nullptr;

    auto& source = _vEffects[index];

    // Clone through JSON, the same way CopyEffect() does, so the channel's copy
    // inherits whatever settings the user has edited on the list entry. The JSON
    // factory map outlives startup - only the default factories are released -
    // so this is safe to call at any time.

    const auto& jsonEffectFactories = g_ptrEffectFactories->GetJSONFactories();
    auto factoryEntry = jsonEffectFactories.find(source->effectId());

    if (factoryEntry == jsonEffectFactories.end())
    {
        debugW("No JSON factory for effect %s, so it can't be bound to one channel", source->FriendlyName().c_str());
        return nullptr;
    }

    auto jsonDoc = CreateJsonDocument();
    auto jsonObject = jsonDoc.to<JsonObject>();

    if (!source->SerializeToJSON(jsonObject))
    {
        debugE("Could not serialize effect %s to JSON", source->FriendlyName().c_str());
        return nullptr;
    }

    auto effect = factoryEntry->second(jsonDoc.as<JsonObjectConst>());

    // Init() with the channel's one-element gfx vector is the whole trick: every
    // LEDStripEffect drawing helper works off _GFX, so a single-device _GFX means
    // "draw to this strip only" with no changes needed in the effect itself.

    if (!effect || !effect->Init(_channels[channel].gfx))
    {
        debugW("Could not initialize channel %zu instance of effect %s", channel, source->FriendlyName().c_str());
        return nullptr;
    }

    effect->SetEnabled(source->IsEnabled());
    effect->Start();

    return effect;
}

bool EffectManager::EnterIndependentMode()
{
    if (_vEffects.empty())
        return false;

    // Build every clone before committing any of them, so one un-clonable effect
    // leaves the device in shared mode rather than half-converted with dark strips.

    std::array<std::shared_ptr<LEDStripEffect>, NUM_CHANNELS> effects;

    for (size_t i = 0; i < _channels.size(); i++)
    {
        if (_channels[i].index >= _vEffects.size())
            _channels[i].index = _iCurrentEffect;

        effects[i] = MakeChannelEffect(i, _channels[i].index);
        if (!effects[i])
            return false;
    }

    const auto now = millis();

    for (size_t i = 0; i < _channels.size(); i++)
    {
        _channels[i].effect = std::move(effects[i]);
        _channels[i].nextDrawMs = now;
        ClearChannelWhites(_channels[i]);
    }

    _channelsIndependent = true;
    debugI("Switched to per-channel effects");

    return true;
}

void EffectManager::LeaveIndependentMode()
{
    if (!_channelsIndependent)
        return;

    for (auto& channel : _channels)
        channel.effect.reset();

    _channelsIndependent = false;
    debugI("Returned to a single effect on all channels");
}

void EffectManager::ResumeSharedPlayback()
{
    if (!_channelsIndependent)
        return;

    LeaveIndependentMode();

    // The persisted "cci" array is only written while the channels are
    // independent, so this write is what actually clears it.
    SaveEffectManagerConfig();
}

bool EffectManager::DrawChannel(ChannelPlayback& channel)
{
    if (!channel.effect)
        return false;

    const uint fps = EffectiveFrameRate(channel);
    const auto now = millis();

    // An fps of 0 means "as fast as the loop runs", so there's nothing to gate.
    if (fps > 0)
    {
        const uint32_t interval = std::max<uint32_t>(1, MILLIS_PER_SECOND / fps);

        // Signed difference so the comparison survives the millis() rollover.
        if (static_cast<int32_t>(now - channel.nextDrawMs) < 0)
            return false;

        // Advance the deadline by exactly one interval rather than resetting it to
        // now. The draw loop wakes on the *fastest* channel's period, so a reset
        // would let loop jitter push the channel whose interval equals that period
        // onto every other frame, halving its rate. Resync when we've fallen a full
        // interval behind (effect switch, OTA stall, a slow neighbouring channel).

        channel.nextDrawMs += interval;
        if (static_cast<int32_t>(now - channel.nextDrawMs) > static_cast<int32_t>(interval))
            channel.nextDrawMs = now + interval;
    }

    channel.effect->Draw();
    return true;
}

bool EffectManager::RemapIndexForMove(size_t& index, size_t from, size_t to)
{
    if (index == from)
        index = to;
    else if (from < index && to >= index)
        index--;
    else if (from > index && to <= index)
        index++;
    else
        return false;

    return true;
}

void EffectManager::RemapIndexForDelete(size_t& index, size_t deleted, size_t newCount)
{
    if (newCount == 0)
        index = 0;
    else if (index > deleted)
        index--;
    else if (index >= newCount)
        index = newCount - 1;
}

void EffectManager::EnableEffect(size_t i, bool skipSave)
{
    // Web, remote, and render tasks all touch the effect list/current effect.
    // Mutations take both locks so a UI request cannot reorder/delete/settings-
    // mutate an effect while the draw loop is inside that effect's Draw().
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (i >= _vEffects.size())
    {
        debugW("Invalid index for EnableEffect");
        return;
    }

    auto& effect = _vEffects[i];

    if (!effect->IsEnabled())
    {
        if (!AreEffectsEnabled())
            ClearRemoteColor(true);

        effect->SetEnabled(true);

        if (!skipSave)
            SaveEffectManagerConfig();

        {
            std::lock_guard listenerGuard(_listenerMutex);
            INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnEffectEnabledStateChanged, i, true);
        }
    }
}

void EffectManager::DisableEffect(size_t i, bool skipSave)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (i >= _vEffects.size())
    {
        debugW("Invalid index for DisableEffect");
        return;
    }

    auto effect = _vEffects[i];

    if (effect->IsEnabled())
    {
        effect->SetEnabled(false);

        if (!AreEffectsEnabled())
            ApplyGlobalColor(CRGB::Black);

        if (!skipSave)
            SaveEffectManagerConfig();

        {
            std::lock_guard listenerGuard(_listenerMutex);
            INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnEffectEnabledStateChanged, i, false);
        }
    }
}

void EffectManager::MoveEffect(size_t from, size_t to)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (from >= _vEffects.size() || to >= _vEffects.size())
    {
        debugW("Invalid index for MoveEffect");
        return;
    }

    if (from == to)
        return;
    else if (from < to)
        std::rotate(_vEffects.begin() + from, _vEffects.begin() + from + 1, _vEffects.begin() + to + 1);
    else // from > to
        std::rotate(_vEffects.rend() - from - 1, _vEffects.rend() - from, _vEffects.rend() - to);

    if (RemapIndexForMove(_iCurrentEffect, from, to))
        SaveCurrentEffectIndex();

    // The per-channel pins are indices into the same list, so they need the same
    // fix-up or a reorder would silently repoint a strip at a different effect.
    for (auto& channel : _channels)
        RemapIndexForMove(channel.index, from, to);

    SaveEffectManagerConfig();

    {
        std::lock_guard listenerGuard(_listenerMutex);
        INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnEffectListDirty);
    }
}

// Adds an effect to the effect list and enables it. If an effect is added that is already in the effect list then the result
//   is undefined but potentially messy.
bool EffectManager::AppendEffect(std::shared_ptr<LEDStripEffect>& effect)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (!effect->Init(_gfx))
        return false;

    _vEffects.push_back(effect);
    EnableEffect(_vEffects.size() - 1, true);

    SaveEffectManagerConfig();

    {
        std::lock_guard listenerGuard(_listenerMutex);
        INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnEffectListDirty);
    }

    return true;
}

// Creates a copy of an existing effect in the list. Note that the effect is created but not yet added to the effect list;
//   use the AppendEffect() function for that.
std::shared_ptr<LEDStripEffect> EffectManager::CopyEffect(size_t index)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (index >= _vEffects.size())
    {
        debugW("Invalid index for CopyEffect");
        return nullptr;
    }

    auto& sourceEffect = _vEffects[index];

    const auto& jsonEffectFactories = g_ptrEffectFactories->GetJSONFactories();
    auto factoryEntry = jsonEffectFactories.find(static_cast<int>(sourceEffect->effectId()));

    if (factoryEntry == jsonEffectFactories.end())
        return nullptr;

    auto jsonDoc = CreateJsonDocument();
    auto jsonObject = jsonDoc.to<JsonObject>();

    if (!sourceEffect->SerializeToJSON(jsonObject))
    {
        debugE("Could not serialize effect %s to JSON", sourceEffect->FriendlyName().c_str());
        return nullptr;
    }

    auto copiedEffect = factoryEntry->second(jsonDoc.as<JsonObjectConst>());

    if (!copiedEffect)
        return nullptr;

    copiedEffect->SetEnabled(false);

    return copiedEffect;
}

// Deletes an effect from the effect list. Note that core effects cannot be deleted.
bool EffectManager::DeleteEffect(size_t index)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (index >= _vEffects.size())
    {
        debugW("Invalid index for DeleteEffect");
        return false;
    }

    if (_vEffects[index]->IsCoreEffect())
        return false;

    DisableEffect(index, true);

    if (index == _iCurrentEffect && _vEffects.size() > 1)
        NextEffect(true);

    _vEffects.erase(_vEffects.begin() + index);

    if (_vEffects.empty())
    {
        _iCurrentEffect = 0;
    }
    else if (index <= _iCurrentEffect)
    {
        if (_iCurrentEffect > 0)
            _iCurrentEffect--;
        else if (_iCurrentEffect >= _vEffects.size())
            _iCurrentEffect = _vEffects.size() - 1;
    }

    // Repoint any channel pinned at or past the deleted entry. A channel that was
    // playing the deleted effect needs a rebuilt clone, since its instance is a
    // copy of a list entry that no longer exists.

    for (size_t i = 0; i < _channels.size(); i++)
    {
        auto& channel = _channels[i];
        const bool wasPlayingDeleted = channel.index == index;

        RemapIndexForDelete(channel.index, index, _vEffects.size());

        if (_channelsIndependent && wasPlayingDeleted)
        {
            // The deletion has landed this channel on a different effect, which is
            // no more entitled to the old effect's frame rate than one the user
            // picked by hand would be. See ResetChannelRateForNewEffect().
            channel.fpsOverride = 0;

            auto effect = MakeChannelEffect(i, channel.index);
            if (effect)
                channel.effect = std::move(effect);
        }
    }

    SaveCurrentEffectIndex();
    SaveEffectManagerConfig();

    {
        std::lock_guard listenerGuard(_listenerMutex);
        INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnEffectListDirty);
    }

    return true;
}

void EffectManager::CheckEffectTimerExpired()
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if (!_tempEffect && _vEffects.empty())
        return;

    // Pinning effects per channel turns auto-rotation off: the point of setting
    // strip 2 to Fire is that it stays on Fire. Selecting an effect for all
    // channels (POST /currentEffect with no channel) resumes rotation.

    if (_channelsIndependent && !_tempEffect)
        return;

    if (IsIntervalEternal() && !GetCurrentEffect().HasMaximumEffectTime())
        return;

    if (GetTimeUsedByCurrentEffect() >= GetEffectiveInterval())
    {
        if (_clearTempEffectWhenExpired)
        {
            _tempEffect.reset();
            _clearTempEffectWhenExpired = false;
        }

        debugV("%ldms elapsed: Next Effect", millis() - _effectStartTime);
        NextEffect();
        debugV("Current Effect: %s", GetCurrentEffectName().c_str());
    }
}

// Update to the next effect and abort the current effect.

void EffectManager::NextEffect(bool skipSave)
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    // "Advance the effect" is a device-wide request, and rotation is off while the
    // channels are pinned, so honoring it means dropping back to shared playback -
    // otherwise the button would appear to do nothing. skipSave marks the internal
    // call from DeleteEffect(), which must leave the per-channel pins alone.

    if (!skipSave)
        ResumeSharedPlayback();

    if (_vEffects.empty())
    {
        _iCurrentEffect = 0;
        _effectStartTime = millis();
        return;
    }

    auto enabled = AreEffectsEnabled();

    do
    {
        _iCurrentEffect++;
        _iCurrentEffect %= EffectCount();
        _effectStartTime = millis();
    } while (enabled && false == _bPlayAll && false == IsEffectEnabled(_iCurrentEffect));

    StartEffect();
    if (!skipSave)
        SaveCurrentEffectIndex();

    {
        std::lock_guard listenerGuard(_listenerMutex);
        INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnCurrentEffectChanged, _iCurrentEffect);
    }
}

// Go back to the previous effect and abort the current one.

void EffectManager::PreviousEffect()
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    ResumeSharedPlayback();     // See NextEffect() for why

    if (_vEffects.empty())
    {
        _iCurrentEffect = 0;
        _effectStartTime = millis();
        return;
    }

    auto enabled = AreEffectsEnabled();

    do
    {
        if (_iCurrentEffect == 0)
            _iCurrentEffect = EffectCount();

        _iCurrentEffect--;
        _effectStartTime = millis();
    } while (enabled && false == _bPlayAll && false == IsEffectEnabled(_iCurrentEffect));

    StartEffect();
    SaveCurrentEffectIndex();

    {
        std::lock_guard listenerGuard(_listenerMutex);
        INFORM_EVENT_LISTENERS(_effectEventListeners, IEffectEventListener::OnCurrentEffectChanged, _iCurrentEffect);
    }
}

// EffectManager::Update
//
// Draws the current effect.  If gUIDirty has been set by an interrupt handler, it is reset here

void EffectManager::Update()
{
    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

    if ((_gfx[0])->GetLEDCount() == 0)
        return;

    if (!_tempEffect && _vEffects.empty())
    {
        ApplyFadeLogic();
        return;
    }

    CheckEffectTimerExpired();
    DispatchBeatIfNeeded();

    // A temp effect (remote global color) deliberately owns every strip, so it
    // short-circuits per-channel playback.

    if (_tempEffect)
        _tempEffect->Draw();
    else if (_channelsIndependent)
        for (auto& channel : _channels)
            DrawChannel(channel);
    else
        _vEffects[_iCurrentEffect]->Draw();

    ApplyFadeLogic();
}

void EffectManager::ApplyFadeLogic()
{
    if (EffectCount() < 2)
    {
        g_Values.Fader = 255;
        return;
    }

    // The fader is one global value applied to every channel by the output
    // manager, and pinned channels don't rotate, so there's no transition to
    // fade through: hold it wide open.

    if (_channelsIndependent && !_tempEffect)
    {
        g_Values.Fader = 255;
        return;
    }

    if (IsIntervalEternal())
    {
        g_Values.Fader = 255;
        return;
    }

    const int msFadeTime = 2000;
    int r = GetTimeRemainingForCurrentEffect();
    int e = GetTimeUsedByCurrentEffect();

    if (e < msFadeTime)
    {
        g_Values.Fader = 255.0f * ((float)e / msFadeTime); // Fade in
    }
    else if (r < msFadeTime)
    {
        g_Values.Fader = 255.0f * ((float)r / msFadeTime); // Fade out
    }
    else
    {
        g_Values.Fader = 255; // No fade, not at start or end
    }
}
