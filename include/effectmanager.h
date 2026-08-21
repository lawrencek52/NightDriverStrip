#pragma once

//+--------------------------------------------------------------------------
//
// File:        EffectManager.h
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
//
// Description:
//
//    Based on my original ESP32LEDStick project this is the class that keeps
//    track of internal effects, which one is active, rotating among them,
//    and fading between them.
//
//
//
// History:     Apr-13-2019         Davepl      Created for NightDriverStrip
//
//---------------------------------------------------------------------------

#include "globals.h"
#include "jsonserializer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class GFXBase;
class LEDStripEffect;

#define JSON_FORMAT_VERSION         1
#define CURRENT_EFFECT_CONFIG_FILE  "/current.cfg"

#define INFORM_EVENT_LISTENERS(listeners, function, ...) \
    std::for_each(listeners.begin(), listeners.end(), [&](auto& listener) { std::invoke(&function, listener __VA_OPT__(,) __VA_ARGS__); })

// Forward references to functions in our accompanying CPP file

void InitSplashEffectManager();
void InitEffectsManager();
void SaveEffectManagerConfig();
void RemoveEffectManagerConfig();

// IFrameEventListener
//
// Abstract class that can be used to listen to frame-related events.

class IFrameEventListener
{
public:
    virtual void OnNewFrameAvailable() = 0;
};

// IEffectEventListener
//
// Abstract class that can be used to listen to effect-related events.

class IEffectEventListener
{
public:
    virtual void OnCurrentEffectChanged(size_t currentEffectIndex) = 0;
    virtual void OnEffectListDirty() = 0;
    virtual void OnEffectEnabledStateChanged(size_t effectIndex, bool newState) = 0;
    virtual void OnIntervalChanged(uint interval) = 0;
};

// BaseFrameEventListener
//
// Basic implementation of IFrameEventListener that remembers it's been called and allows
// that recollection to be read and cleared.

class BaseFrameEventListener : public IFrameEventListener
{
    std::atomic<bool> _newFrameAvailable = false;

public:
    void OnNewFrameAvailable() override
    {
        _newFrameAvailable = true;
    }

    bool CheckAndClearNewFrameAvailable()
    {
        return _newFrameAvailable.exchange(false);
    }
};

// EffectManager
//
// Handles keeping track of the effects, which one is active, asking it to draw, etc.

class  EffectManager : public IJSONSerializable
{
    std::vector<std::shared_ptr<LEDStripEffect>> _vEffects;

    size_t _iCurrentEffect = 0;
    uint _effectStartTime;
    uint _effectInterval = 0;
    bool _bPlayAll;
    bool _clearTempEffectWhenExpired = false;
    std::atomic_bool _newFrameAvailable = false;
    String _effectSetHashString = "";
    uint32_t _lastBeatSequence = 0;
    uint32_t _lastNearBeatSequence = 0;

    std::vector<std::shared_ptr<GFXBase>> _gfx;
    std::shared_ptr<LEDStripEffect> _tempEffect;
    std::vector<std::reference_wrapper<IFrameEventListener>> _frameEventListeners;
    std::vector<std::reference_wrapper<IEffectEventListener>> _effectEventListeners;
    mutable std::mutex _listenerMutex;

    // ChannelPlayback
    //
    // Per-output-channel playback state, used when the channels run independent
    // effects. _vEffects remains the single canonical effect list - the one the
    // UI edits and that gets persisted - and each channel plays a private clone
    // of the entry it is pinned to.
    //
    // The clone is Init()ed with a one-element gfx vector, and that is what
    // confines it to a single strip: every drawing helper in LEDStripEffect works
    // off _GFX, so a one-device _GFX means "draw to this strip only" without any
    // effect-side changes. Cloning (rather than sharing the list instance) is also
    // what keeps animation state - fire heat maps, particle systems, phase
    // accumulators - from colliding when two channels play the same effect.

    struct ChannelPlayback
    {
        std::shared_ptr<LEDStripEffect>       effect;                 // channel-private clone
        std::vector<std::shared_ptr<GFXBase>> gfx;                    // exactly one device
        size_t                                index       = 0;        // into _vEffects
        uint                                  fpsOverride = 0;        // 0 = follow the effect's DesiredFramesPerSecond()
        uint32_t                              nextDrawMs  = 0;        // millis() deadline for this channel's next frame
    };

    std::array<ChannelPlayback, NUM_CHANNELS> _channels;

    // False means every channel draws from one shared effect instance, which is
    // the original and default behavior: rotation and cross-fade both run. Pinning
    // any single channel flips this true, after which each channel is drawn from
    // its own clone at its own frame rate and the rotation timer stops - a pinned
    // strip is meant to stay put until it is changed again.

    bool _channelsIndependent = false;

    void construct(bool clearTempEffect);
    void DispatchBeatIfNeeded();

    // Gives every ChannelPlayback the one-element gfx vector it hands to Init().
    // Must be re-run whenever the device vector is rebuilt (live topology change).
    void BindChannelGraphics();

    // Builds a channel-private instance of _vEffects[index] bound to one strip.
    // Returns nullptr if the effect has no JSON factory or fails to initialize.
    std::shared_ptr<LEDStripEffect> MakeChannelEffect(size_t channel, size_t index);

    // Materializes clones for every channel and switches to independent mode.
    // Rolls back to shared mode and returns false if any clone can't be built.
    bool EnterIndependentMode();

    // Drops the per-channel clones and returns to one shared effect on all strips.
    void LeaveIndependentMode();

    // LeaveIndependentMode() plus a config write, so a reboot doesn't restore the
    // per-channel pins we just discarded. No-op when already in shared mode.
    void ResumeSharedPlayback();

    // Draws one channel if its own frame interval has elapsed. Returns true if it drew.
    bool DrawChannel(ChannelPlayback& channel);

    // Rate a channel should draw at: its override if set, else its effect's ask.
    static uint EffectiveFrameRate(const ChannelPlayback& channel);

    // Drops a channel's frame rate override when it is about to be pointed at a
    // different effect, so the new effect runs at its own DesiredFramesPerSecond()
    // instead of inheriting a rate that was chosen for its predecessor. Returns
    // true if an override was actually cleared. Must be called before the
    // channel's index is updated, since it compares against what is playing now.
    bool ResetChannelRateForNewEffect(ChannelPlayback& channel, size_t effectIndex);

    // Zeroes one channel's whites plane; see StartEffect() for why that's needed
    // at every effect switch.
    static void ClearChannelWhites(const ChannelPlayback& channel);

    // MoveEffect rotates the effect list, so any stored index into it needs the
    // same fix-up. Shared by the per-channel pinned indices so they can't drift
    // away from _iCurrentEffect. Returns true if the index moved.
    static bool RemapIndexForMove(size_t& index, size_t from, size_t to);

    // Adjusts a stored index after the entry at `deleted` was erased from the list.
    static void RemapIndexForDelete(size_t& index, size_t deleted, size_t newCount);

    // Implementation is in effects.cpp
    void LoadJSONEffects(const JsonArrayConst& effectsArray);

    static void SaveCurrentEffectIndex();
    static bool ReadCurrentEffectIndex(size_t& index);

    void ClearEffects()
    {
        _vEffects.clear();
    }

public:
    static const uint csFadeButtonSpeed = 15 * 1000;
    static const uint csSmoothButtonSpeed = 60 * 1000;

    EffectManager(const std::shared_ptr<LEDStripEffect>& effect, std::vector<std::shared_ptr<GFXBase>>& gfx);
    explicit EffectManager(std::vector<std::shared_ptr<GFXBase>>& gfx);
    EffectManager(const JsonObjectConst& jsonObject, std::vector<std::shared_ptr<GFXBase>>& gfx);
    ~EffectManager();

    // SetTempEffect - Sets a temporary effect to be played until remote changes it.
    //                 The effect must have already had its Init() function called.

    void SetTempEffect(std::shared_ptr<LEDStripEffect> effect);

    // GetBaseGraphics - Returns the vector of GFXBase objects that the effects use to draw

    std::vector<std::shared_ptr<GFXBase>> & GetBaseGraphics();

    void ReportNewFrameAvailable();
    void AddFrameEventListener(IFrameEventListener& listener);
    
    // RemoveFrameEventListener
    //
    // Removes a previously registered frame event listener by address.
    // Safe to call even if the listener was never registered (no-op).
    // Required for any listener whose lifetime is shorter than the
    // EffectManager's (e.g. a listener owned by a service that can be
    // Stop()-ed and have its task exit), since EffectManager stores
    // listeners by reference.

    void RemoveFrameEventListener(IFrameEventListener& listener);
    void AddEffectEventListener(IEffectEventListener& listener);

    void LoadDefaultEffects();
    bool ReinitializeEffects();

    // DeserializeFromJSON
    //
    // This function deserializes LED strip effects from a provided JSON object.
    //
    // It first clears any existing effects and then attempts to populate the effects vector from
    // the provided JSON object, which should contain an array of effects configurations ("efs").
    //
    // For each effect in the JSON array, it attempts to create an effect from its JSON configuration.
    // If an effect is successfully created, it's added to the effects vector.
    //
    // If no effects are successfully loaded from JSON, it loads the default effects.
    //
    // If the JSON object includes an "eef" array, the function attempts to load each effect's enabled
    // state from it.
    // If the index exceeds the "eef" array's size, the effect is enabled by default.
    //
    // The function also sets the effect interval from the "ivl" field in the JSON object, defaulting
    // to a pre-defined value if the field isn't present.
    //
    // If the JSON object includes a "cei" field, the function sets the current effect index to this
    // value. If the value is greater than or equal to the number of effects, it defaults to the last
    // effect in the vector.
    //
    // Lastly, the function calls the construct() method, indicating successful deserialization.

    bool DeserializeFromJSON(const JsonObjectConst& jsonObject) override;

    // SerializeToJSON - Serialize effects to a JSON object.
    //
    // This function serializes the current state of the LED strip effects into a JSON object.
    // It starts by setting the JSON format version ("PTY_VERSION") to a predefined value ("JSON_FORMAT_VERSION")
    // that helps in detecting and managing potential future incompatible structural updates.
    //
    // The function then sets the "ivl" and "cei" fields in the JSON object to the current effect interval
    // and the current effect index, respectively.
    //
    // Next, the function creates a nested array ("efs") in the JSON object to store the effects themselves.
    // It iterates through all effects, and for each effect, it creates a nested object in the effects array
    // and attempts to serialize the effect into this object. If serialization of any effect fails, the function
    // immediately returns false.
    //
    // If all effects are successfully serialized, the function returns true, indicating successful serialization.

    bool SerializeToJSON(JsonObject& jsonObject) override;

    // Must provide at least one drawing instance, like the first matrix or strip we are drawing on
    GFXBase& g(int iChannel = 0);
    const GFXBase& g(int iChannel = 0) const;

    // ShowVU - Control whether VU meter should be drawn.  Returns the previous state when set.
    virtual bool ShowVU(bool bShow);
    virtual bool IsVUVisible() const;

    // ApplyGlobalColor
    //
    // When a global color is set via the remote, we create a fill effect and assign it as the "remote effect"
    // which takes drawing precedence

    void ApplyGlobalColor(CRGB color);
    void ApplyGlobalPaletteColors();

    void ClearRemoteColor(bool retainRemoteEffect = false);

    void StartEffect();

    void EnableEffect(size_t i, bool skipSave = false);

    void DisableEffect(size_t i, bool skipSave = false);

    bool IsEffectEnabled(size_t i) const;

    void MoveEffect(size_t from, size_t to);

    // Creates a copy of an existing effect in the list. Note that the effect is created but not yet added to the effect list;
    //   use the AppendEffect() function for that.
    std::shared_ptr<LEDStripEffect> CopyEffect(size_t index);

    // Adds an effect to the effect list and enables it. If an effect is added that is already in the effect list then the result
    //   is undefined but potentially messy.
    bool AppendEffect(std::shared_ptr<LEDStripEffect>& effect);

    bool DeleteEffect(size_t index);

    void PlayAll(bool bPlayAll);
    void SetInterval(uint interval, bool skipSave = false);
    std::vector<std::shared_ptr<LEDStripEffect>> EffectsList() const;
    std::shared_ptr<LEDStripEffect> EffectAt(size_t index) const;
    bool IsCoreEffect(size_t index) const;
    size_t EffectCount() const;
    bool AreEffectsEnabled() const;
    bool HasCurrentEffect() const;
    size_t GetCurrentEffectIndex() const;
    LEDStripEffect& GetCurrentEffect() const;
    String GetCurrentEffectName() const;
    void SetCurrentEffectIndex(size_t i);

    // Per-channel effect selection
    //
    // kAllChannels targets every strip and returns the device to shared mode -
    // one effect instance on all channels, with rotation and cross-fade active.
    // That's what /currentEffect does when the request carries no channel, so the
    // pre-per-channel API contract is preserved exactly.

    static constexpr int kAllChannels = -1;
    static constexpr size_t ChannelCount() { return NUM_CHANNELS; }

    // Pins one channel (or all of them) to an effect. Returns false if the channel
    // or effect index is out of range, or if the per-channel clone couldn't be built.
    bool SetEffectIndex(int channel, size_t effectIndex);

    size_t GetChannelEffectIndex(size_t channel) const;
    bool AreChannelsIndependent() const;

    // Per-channel frame rate. Passing 0 hands the channel back to its effect's
    // DesiredFramesPerSecond(); GetChannelFrameRate() reports the effective rate
    // and GetChannelFrameRateOverride() the configured one (0 when unset).
    bool SetChannelFrameRate(int channel, uint fps);
    uint GetChannelFrameRate(size_t channel) const;
    uint GetChannelFrameRateOverride(size_t channel) const;

    // Rate the draw loop should pace itself at: the fastest of the active channels,
    // so a slow effect on one strip can't throttle a fast one on another. Each
    // channel is then gated to its own rate inside Update().
    size_t GetDesiredFramesPerSecond() const;
    uint GetTimeUsedByCurrentEffect() const;
    uint GetTimeRemainingForCurrentEffect() const;
    uint GetEffectiveInterval() const;
    uint GetInterval() const;
    bool IsIntervalEternal() const;

    void CheckEffectTimerExpired();

    void NextPalette();
    void PreviousPalette();
    // Update to the next effect and abort the current effect.

    void NextEffect(bool skipSave = false);

    // Go back to the previous effect and abort the current one.

    void PreviousEffect();

    bool Init();

    // EffectManager::Update
    //
    // Draws the current effect.  If gUIDirty has been set by an interrupt handler, it is reset here

    void Update();

    void ApplyFadeLogic();
};
