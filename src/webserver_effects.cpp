//+--------------------------------------------------------------------------
//
// File:        webserver_effects.cpp
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of webserver.cpp; see that file header for additional context.
//
// Split scope: web endpoints and handlers related to effects and effect list mutations.
//---------------------------------------------------------------------------


#include "globals.h"

#if ENABLE_WEBSERVER

#include "webserver.h"

#include <AsyncJson.h>
#include <memory>

#include "effectmanager.h"
#include "ledstripeffect.h"
#include "systemcontainer.h"

void CWebServer::GetEffectListText(AsyncWebServerRequest * pRequest)
{
    debugV("GetEffectListText");

    auto response = std::make_unique<AsyncJsonResponse>();
    auto& j = response->getRoot();
    auto& effectManager = g_ptrSystem->GetEffectManager();
    bool overflow = false;

    {
        std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);

        j["currentEffect"]         = effectManager.GetCurrentEffectIndex();
        j["millisecondsRemaining"] = effectManager.GetTimeRemainingForCurrentEffect();
        j["eternalInterval"]       = effectManager.IsIntervalEternal();
        j["effectInterval"]        = effectManager.GetInterval();

        // Per-channel playback. "currentEffect" above stays the device-wide answer
        // for clients that predate this, and every channel reports the same index
        // as long as the channels aren't independent.

        j["channelsIndependent"] = effectManager.AreChannelsIndependent();

        auto channelEffects = j["channelEffects"].to<JsonArray>();
        auto channelFrameRates = j["channelFrameRates"].to<JsonArray>();
        auto channelFrameRateOverrides = j["channelFrameRateOverrides"].to<JsonArray>();

        for (size_t channel = 0; channel < EffectManager::ChannelCount(); channel++)
        {
            channelEffects.add(effectManager.GetChannelEffectIndex(channel));
            channelFrameRates.add(effectManager.GetChannelFrameRate(channel));
            channelFrameRateOverrides.add(effectManager.GetChannelFrameRateOverride(channel));
        }

        for (const auto& effect : effectManager.EffectsList())
        {
            auto effectDoc = CreateJsonDocument();

            effectDoc["name"]    = effect->FriendlyName();
            effectDoc["enabled"] = effect->IsEnabled();
            effectDoc["core"]    = effect->IsCoreEffect();

            if (!j["Effects"].add(effectDoc))
            {
                debugV("JSON response buffer overflow!");
                overflow = true;
                break;
            }
        }
    }

    if (overflow)
    {
        response.reset();
        SendBufferOverflowResponse(pRequest);
        return;
    }

    AddCORSHeaderAndSendResponse(pRequest, response.release());
}

// GetChannelFromParam
//
// Reads the optional "channel" parameter. Absent means every channel, which is what
// these endpoints did before per-channel playback existed, so old clients are
// unaffected. Returns false if the parameter is present but isn't a valid channel.

bool CWebServer::GetChannelFromParam(AsyncWebServerRequest * pRequest, int& channel, bool post)
{
    channel = EffectManager::kAllChannels;

    if (!pRequest->hasParam("channel", post, false))
        return true;

    const long value = strtol(pRequest->getParam("channel", post, false)->value().c_str(), nullptr, 10);

    if (value < 0 || static_cast<size_t>(value) >= EffectManager::ChannelCount())
        return false;

    channel = static_cast<int>(value);
    return true;
}

void CWebServer::SetCurrentEffectIndex(AsyncWebServerRequest * pRequest)
{
    debugV("SetCurrentEffectIndex");

    int channel;

    if (!GetChannelFromParam(pRequest, channel, true))
    {
        AddCORSHeaderAndSendBadRequest(pRequest, "Invalid channel");
        return;
    }

    bool applied = true;
    PushPostParamIfPresent<size_t>(pRequest, "currentEffectIndex", [&](size_t value)
    {
        applied = g_ptrSystem->GetEffectManager().SetEffectIndex(channel, value);
        return applied;
    });

    if (!applied)
    {
        AddCORSHeaderAndSendBadRequest(pRequest, "Could not select that effect for the channel");
        return;
    }

    AddCORSHeaderAndSendOKResponse(pRequest);
}

// SetChannelFrameRate
//
// Overrides the rate a channel is drawn at, independent of what its effect asks
// for. An fps of 0 hands the channel back to its effect's DesiredFramesPerSecond().

void CWebServer::SetChannelFrameRate(AsyncWebServerRequest * pRequest)
{
    debugV("SetChannelFrameRate");

    int channel;

    if (!GetChannelFromParam(pRequest, channel, true))
    {
        AddCORSHeaderAndSendBadRequest(pRequest, "Invalid channel");
        return;
    }

    bool applied = true;
    PushPostParamIfPresent<size_t>(pRequest, "fps", [&](size_t value)
    {
        applied = g_ptrSystem->GetEffectManager().SetChannelFrameRate(channel, value);
        return applied;
    });

    if (!applied)
    {
        AddCORSHeaderAndSendBadRequest(pRequest, "Invalid frame rate");
        return;
    }

    AddCORSHeaderAndSendOKResponse(pRequest);
}

void CWebServer::EnableEffect(AsyncWebServerRequest * pRequest)
{
    debugV("EnableEffect");
    PushPostParamIfPresent<size_t>(pRequest, "effectIndex", SET_VALUE(g_ptrSystem->GetEffectManager().EnableEffect(value)));
    AddCORSHeaderAndSendOKResponse(pRequest);
}

void CWebServer::DisableEffect(AsyncWebServerRequest * pRequest)
{
    debugV("DisableEffect");
    PushPostParamIfPresent<size_t>(pRequest, "effectIndex", SET_VALUE(g_ptrSystem->GetEffectManager().DisableEffect(value)));
    AddCORSHeaderAndSendOKResponse(pRequest);
}

void CWebServer::MoveEffect(AsyncWebServerRequest * pRequest)
{
    debugV("MoveEffect");

    auto fromIndex = GetEffectIndexFromParam(pRequest, true);
    if (fromIndex == -1)
    {
        AddCORSHeaderAndSendOKResponse(pRequest);
        return;
    }

    PushPostParamIfPresent<size_t>(pRequest, "newIndex", SET_VALUE(g_ptrSystem->GetEffectManager().MoveEffect(fromIndex, value)));
    AddCORSHeaderAndSendOKResponse(pRequest);
}

void CWebServer::CopyEffect(AsyncWebServerRequest * pRequest)
{
    debugV("CopyEffect");

    auto index = GetEffectIndexFromParam(pRequest, true);
    if (index == -1)
    {
        AddCORSHeaderAndSendOKResponse(pRequest);
        return;
    }

    auto effect = g_ptrSystem->GetEffectManager().CopyEffect(index);
    if (!effect)
    {
        AddCORSHeaderAndSendOKResponse(pRequest);
        return;
    }

    ApplyEffectSettings(pRequest, effect);

    if (g_ptrSystem->GetEffectManager().AppendEffect(effect))
        SendEffectSettingsResponse(pRequest, effect);
    else
        AddCORSHeaderAndSendOKResponse(pRequest);
}

void CWebServer::DeleteEffect(AsyncWebServerRequest * pRequest)
{
    debugV("DeleteEffect");

    auto index = GetEffectIndexFromParam(pRequest, true);
    if (index == -1)
    {
        AddCORSHeaderAndSendOKResponse(pRequest);
        return;
    }

    if (g_ptrSystem->GetEffectManager().IsCoreEffect(index))
    {
        AddCORSHeaderAndSendBadRequest(pRequest, "Can't delete core effect");
        return;
    }

    g_ptrSystem->GetEffectManager().DeleteEffect(index);
    AddCORSHeaderAndSendOKResponse(pRequest);
}

void CWebServer::NextEffect(AsyncWebServerRequest * pRequest)
{
    debugV("NextEffect");
    g_ptrSystem->GetEffectManager().NextEffect();
    AddCORSHeaderAndSendOKResponse(pRequest);
}

void CWebServer::PreviousEffect(AsyncWebServerRequest * pRequest)
{
    debugV("PreviousEffect");
    g_ptrSystem->GetEffectManager().PreviousEffect();
    AddCORSHeaderAndSendOKResponse(pRequest);
}

bool CWebServer::CheckAndGetSettingsEffect(AsyncWebServerRequest * pRequest, std::shared_ptr<LEDStripEffect> & effect, bool post)
{
    auto effectIndex = GetEffectIndexFromParam(pRequest, post);

    if (effectIndex < 0)
    {
        AddCORSHeaderAndSendOKResponse(pRequest);

        return false;
    }

    effect = g_ptrSystem->GetEffectManager().EffectAt(effectIndex);
    if (!effect)
    {
        AddCORSHeaderAndSendOKResponse(pRequest);
        return false;
    }

    return true;
}

void CWebServer::GetEffectSettingSpecs(AsyncWebServerRequest * pRequest)
{
    std::shared_ptr<LEDStripEffect> effect;

    if (!CheckAndGetSettingsEffect(pRequest, effect))
        return;

    std::vector<std::reference_wrapper<SettingSpec>> settingSpecs;
    {
        std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);
        settingSpecs = effect->GetSettingSpecs();
    }

    SendSettingSpecsResponse(pRequest, settingSpecs);
}

void CWebServer::SendEffectSettingsResponse(AsyncWebServerRequest * pRequest, std::shared_ptr<LEDStripEffect> & effect)
{
    auto response = std::make_unique<AsyncJsonResponse>();
    auto jsonObject = response->getRoot().to<JsonObject>();

    bool serialized = false;
    {
        std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);
        serialized = effect->SerializeSettingsToJSON(jsonObject);
    }

    if (serialized)
    {
        AddCORSHeaderAndSendResponse(pRequest, response.release());
        return;
    }

    debugV("JSON response buffer overflow!");
    SendBufferOverflowResponse(pRequest);
}

void CWebServer::GetEffectSettings(AsyncWebServerRequest * pRequest)
{
    debugV("GetEffectSettings");

    std::shared_ptr<LEDStripEffect> effect;

    if (!CheckAndGetSettingsEffect(pRequest, effect))
        return;

    SendEffectSettingsResponse(pRequest, effect);
}

bool CWebServer::ApplyEffectSettings(AsyncWebServerRequest * pRequest, std::shared_ptr<LEDStripEffect> & effect)
{
    bool settingChanged = false;

    std::scoped_lock guard(g_render_mutex, g_effect_manager_mutex);
    for (auto& settingSpecWrapper : effect->GetSettingSpecs())
    {
        const auto& spec = settingSpecWrapper.get();
        if (spec.ApiPath)
            continue;
        const String& settingName = spec.Name;
        settingChanged = PushPostParamIfPresent<String>(pRequest, settingName, [&](auto value) { return effect->SetSetting(settingName, value); })
            || settingChanged;
    }

    return settingChanged;
}

void CWebServer::SetEffectSettings(AsyncWebServerRequest * pRequest)
{
    debugV("SetEffectSettings");

    std::shared_ptr<LEDStripEffect> effect;

    if (!CheckAndGetSettingsEffect(pRequest, effect, true))
        return;

    if (ApplyEffectSettings(pRequest, effect))
        SaveEffectManagerConfig();

    SendEffectSettingsResponse(pRequest, effect);
}

#endif  // ENABLE_WEBSERVER
