//+--------------------------------------------------------------------------
//
// File:        deviceconfig.cpp
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
//    Implementation of DeviceConfig class methods
//
// History:     Apr-18-2023         Rbergen      Created
//
//---------------------------------------------------------------------------

#include "globals.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <driver/gpio.h>
#include <HTTPClient.h>
#include <memory>
#include <optional>
#include <UrlEncode.h>

#include "audioservice.h"
#include "deviceconfig.h"
#include "deviceconfig_internal.h"
#include "effectmanager.h"
#include "jsonserializer.h"
#include "ntptimeclient.h"
#include "systemcontainer.h"

extern const char timezones_start[] asm("_binary_config_timezones_json_start");

// DeviceConfig holds, persists and loads device-wide configuration settings. Effect-specific settings should
// be managed using overrides of the respective methods in LEDStripEffect (mainly FillSettingSpecs(),
// SerializeSettingsToJSON() and SetSetting()).
//
// Adding a setting to the list of known/saved settings requires the following:
// 1. Adding the setting variable to the list at the top of the class definition
// 2. Adding a corresponding Tag to the list of static constexpr const char * strings further below
// 3. Adding a corresponding SettingSpec in the GetSettingSpecs() function
// 4. Adding logic to set a default in case the JSON load isn't possible in the DeviceConfig() constructor
//    (in deviceconfig.cpp)
// 5. Adding (de)serialization logic for the setting to the SerializeToJSON()/DeserializeFromJSON() methods
// 6. Adding a Get/Set method for the setting (and, where applicable, their implementation in deviceconfig.cpp)
// 7. If you've added an entry to secrets.example.h to define a default value for your setting then add a
//    test at the top of this file to confirm that the new #define is found. This prevents drift when users
//    have an existing tree and don't know to refresh their modified version of secrets.h.
//
// For the first 5 points, a comment has been added to the respective place in the existing code.
// Generally speaking, one will also want to add logic to the webserver to retrieve and set the setting.

void DeviceConfig::SaveToJSON() const
{
    g_ptrSystem->GetJSONWriter().FlagWriter(writerIndex);
}

std::array<int8_t, NUM_CHANNELS> DeviceConfig::GetCompiledWS281xPins()
{
    return DeviceConfigInternal::GetCompiledWS281xPins();
}

std::array<int8_t, NUM_CHANNELS> DeviceConfig::GetCompiledAPA102ClockPins()
{
    return DeviceConfigInternal::GetCompiledAPA102ClockPins();
}

const char* DeviceConfig::DriverName(OutputDriver driver)
{
    switch (driver)
    {
        case OutputDriver::HUB75:
            return "hub75";

        case OutputDriver::APA102:
            return "apa102";

        case OutputDriver::WS281x:
        default:
            return "ws281x";
    }
}

DeviceConfig::WS281xColorOrder DeviceConfig::GetCompiledWS281xColorOrder()
{
    return DeviceConfigInternal::GetCompiledWS281xColorOrder();
}

String DeviceConfig::GetColorOrderName(WS281xColorOrder colorOrder)
{
    switch (colorOrder)
    {
        case WS281xColorOrder::RGB: return "RGB";
        case WS281xColorOrder::RBG: return "RBG";
        case WS281xColorOrder::GRB: return "GRB";
        case WS281xColorOrder::GBR: return "GBR";
        case WS281xColorOrder::BRG: return "BRG";
        case WS281xColorOrder::BGR: return "BGR";
        default:                    return "GRB";
    }
}

bool DeviceConfig::IsHub75Build()
{
    return GetCompiledOutputDriver() == OutputDriver::HUB75;
}

void DeviceConfig::LogRuntimeConfig(const char* reason) const
{
    String activePins;
    for (size_t i = 0; i < runtimeOutputs.channelCount && i < runtimeOutputs.outputPins.size(); ++i)
    {
        if (!activePins.isEmpty())
            activePins += ',';
        activePins += String(runtimeOutputs.outputPins[i]);
    }

    String activeClockPins;
    for (size_t i = 0; i < runtimeOutputs.channelCount && i < runtimeOutputs.clockPins.size(); ++i)
    {
        if (!activeClockPins.isEmpty())
            activeClockPins += ',';
        activeClockPins += String(runtimeOutputs.clockPins[i]);
    }

    String channelSummary;
    for (size_t i = 0; i < runtimeOutputs.channelCount && i < runtimeTopology.channels.size(); ++i)
    {
        const auto& ch = runtimeTopology.channels[i];
        if (!channelSummary.isEmpty())
            channelSummary += ' ';
        if (ch.shape == ChannelShape::Matrix)
            channelSummary += String("ch") + i + "=matrix:" + ch.matrixWidth + "x" + ch.matrixHeight
                + ":serp=" + ch.matrixSerpentine + ":origin=" + static_cast<int>(ch.origin) + ":axis=" + static_cast<int>(ch.axis);
        else
            channelSummary += String("ch") + i + "=strip:" + ch.stripLength;
    }

    debugI("Runtime config (%s): driver=%s leds=%u channels=%u colorOrder=%s audioPin=%d",
           reason,
           DriverName(runtimeOutputs.driver),
           static_cast<unsigned>(GetActiveLEDCount()),
           static_cast<unsigned>(runtimeOutputs.channelCount),
           GetColorOrderName(runtimeOutputs.colorOrder).c_str(),
           audioInputPin);
    debugI("Runtime config channels (%s): %s", reason, channelSummary.c_str());

    debugI("Runtime config pins (%s): data=%s clock=%s", reason, activePins.c_str(), activeClockPins.c_str());
}

uint16_t DeviceConfig::GetChannelLEDCount(size_t channel) const
{
    if (channel >= runtimeOutputs.channelCount || channel >= runtimeTopology.channels.size())
        return 0;

    const auto& ch = runtimeTopology.channels[channel];
    return ch.shape == ChannelShape::Strip
        ? ch.stripLength
        : static_cast<uint16_t>(static_cast<size_t>(ch.matrixWidth) * ch.matrixHeight);
}

size_t DeviceConfig::GetActiveLEDCount() const
{
    size_t total = 0;
    const size_t count = std::min(runtimeOutputs.channelCount, runtimeTopology.channels.size());
    for (size_t i = 0; i < count; ++i)
        total += GetChannelLEDCount(i);
    return total;
}

bool DeviceConfig::IsUniformMatrix() const
{
    const size_t count = std::min(runtimeOutputs.channelCount, runtimeTopology.channels.size());
    if (count == 0)
        return false;

    const auto& first = runtimeTopology.channels[0];
    if (first.shape != ChannelShape::Matrix)
        return false;

    for (size_t i = 1; i < count; ++i)
    {
        const auto& ch = runtimeTopology.channels[i];
        if (ch.shape != ChannelShape::Matrix
            || ch.matrixWidth != first.matrixWidth
            || ch.matrixHeight != first.matrixHeight
            || ch.matrixSerpentine != first.matrixSerpentine
            || ch.origin != first.origin
            || ch.axis != first.axis)
            return false;
    }

    return true;
}

bool DeviceConfig::AreAllChannelsStrip() const
{
    const size_t count = std::min(runtimeOutputs.channelCount, runtimeTopology.channels.size());
    if (count == 0)
        return false;

    for (size_t i = 0; i < count; ++i)
    {
        if (runtimeTopology.channels[i].shape != ChannelShape::Strip)
            return false;
    }

    return true;
}

String DeviceConfig::GetLayoutSummary() const
{
    if (IsHub75Build() || IsUniformMatrix())
        return "matrix";
    if (AreAllChannelsStrip())
        return "individualStrips";
    return "mixed";
}

DeviceConfig::DeviceConfig()
{
    runtimeOutputs.driver = GetCompiledOutputDriver();
    runtimeOutputs.channelCount = NUM_CHANNELS;
    runtimeOutputs.outputPins = GetCompiledWS281xPins();
    runtimeOutputs.clockPins = GetCompiledAPA102ClockPins();
    runtimeOutputs.colorOrder = GetCompiledWS281xColorOrder();

    // Default every channel to a matrix matching the compiled panel dims (mirroring the old
    // global-Matrix default), with a sensible strip length carried along too in case the user
    // later flips a channel to Strip without first editing it.
    const uint16_t defaultStripLength = static_cast<uint16_t>(GetCompiledLEDCount());
    for (auto& ch : runtimeTopology.channels)
    {
        ch.shape = ChannelShape::Matrix;
        ch.matrixWidth = MATRIX_WIDTH;
        ch.matrixHeight = MATRIX_HEIGHT;
        ch.matrixSerpentine = !IsHub75Build();
        ch.origin = MatrixOrigin::TopLeft;
        ch.axis = SerpentineAxis::Vertical;
        ch.stripLength = defaultStripLength;
    }

    writerIndex = g_ptrSystem->GetJSONWriter().RegisterWriter(
        [this] { assert(SaveToJSONFile(DEVICE_CONFIG_FILE, *this)); }
    );

    auto jsonDoc = CreateJsonDocument();

    if (LoadJSONFile(DEVICE_CONFIG_FILE, jsonDoc))
    {
        debugI("Loading DeviceConfig from JSON");

        DeserializeFromJSON(jsonDoc.as<JsonObjectConst>(), true);
    }
    else
    {
        debugW("DeviceConfig could not be loaded from JSON, using defaults");

        SetTimeZone(timeZone, true);

        SaveToJSON();
    }

    LogRuntimeConfig("init");
}

bool DeviceConfig::SerializeToJSON(JsonObject& jsonObject)
{
    return SerializeToJSON(jsonObject, true);
}

bool DeviceConfig::SerializeToJSON(JsonObject& jsonObject, bool includeSensitive)
{
    auto jsonDoc = CreateJsonDocument();

    // Add serialization logic for additional settings to this code
    jsonDoc[HostnameTag] = hostname;
    jsonDoc[LocationTag] = location;
    jsonDoc[LocationIsZipTag] = locationIsZip;
    jsonDoc[CountryCodeTag] = countryCode;
    jsonDoc[TimeZoneTag] = timeZone;
    jsonDoc[Use24HourClockTag] = use24HourClock;
    jsonDoc[UseCelsiusTag] = useCelsius;
    jsonDoc[NTPServerTag] = ntpServer;
    jsonDoc[RememberCurrentEffectTag] = rememberCurrentEffect;
    jsonDoc[RemoteEffectButtonsResetIntervalTag] = remoteEffectButtonsResetInterval;
    jsonDoc[PowerLimitTag] = powerLimit;
    jsonDoc[PowerLimitDefaultTag] = POWER_LIMIT_DEFAULT;
    // Only serialize showVUMeter if the VU meter is enabled in the build
    #if SHOW_VU_METER
    jsonDoc[ShowVUMeterTag] = showVUMeter;
    #endif
    jsonDoc[BrightnessTag] = brightness;
    jsonDoc[ScheduleEnabledTag] = scheduleEnabled;
    jsonDoc[ScheduleDimPercentTag] = scheduleDimPercent;
    jsonDoc[ScheduleDimTimeTag] = scheduleDimTime;
    jsonDoc[ScheduleOffTimeTag] = scheduleOffTime;
    jsonDoc[ScheduleOnTimeTag] = scheduleOnTime;
    jsonDoc[ScheduleLatLongAutoTag] = scheduleLatLongAuto;
    jsonDoc[ScheduleLatitudeTag] = scheduleLatitude;
    jsonDoc[ScheduleLongitudeTag] = scheduleLongitude;
    jsonDoc[ScheduleLatLongStatusTag] = scheduleLatLongStatus;
    jsonDoc[GlobalColorTag] = globalColor;
    jsonDoc[ApplyGlobalColorsTag] = applyGlobalColors;
    jsonDoc[SecondColorTag] = secondColor;
    jsonDoc[AudioInputPinTag] = audioInputPin;

    auto channelShapes = jsonDoc[ChannelShapesTag].to<JsonArray>();
    auto channelStripLengths = jsonDoc[ChannelStripLengthsTag].to<JsonArray>();
    auto channelMatrixWidths = jsonDoc[ChannelMatrixWidthsTag].to<JsonArray>();
    auto channelMatrixHeights = jsonDoc[ChannelMatrixHeightsTag].to<JsonArray>();
    auto channelMatrixSerpentines = jsonDoc[ChannelMatrixSerpentinesTag].to<JsonArray>();
    auto channelMatrixOrigins = jsonDoc[ChannelMatrixOriginsTag].to<JsonArray>();
    auto channelMatrixAxes = jsonDoc[ChannelMatrixAxesTag].to<JsonArray>();
    for (const auto& ch : runtimeTopology.channels)
    {
        channelShapes.add(static_cast<uint8_t>(ch.shape));
        channelStripLengths.add(ch.stripLength);
        channelMatrixWidths.add(ch.matrixWidth);
        channelMatrixHeights.add(ch.matrixHeight);
        channelMatrixSerpentines.add(ch.matrixSerpentine);
        channelMatrixOrigins.add(static_cast<uint8_t>(ch.origin));
        channelMatrixAxes.add(static_cast<uint8_t>(ch.axis));
    }

    jsonDoc[OutputDriverTag] = DriverName(runtimeOutputs.driver);
    jsonDoc[WS281xChannelCountTag] = runtimeOutputs.channelCount;
    jsonDoc[WS281xColorOrderTag] = GetColorOrderName(runtimeOutputs.colorOrder);

    auto ws281xPins = jsonDoc[WS281xPinsTag].to<JsonArray>();
    for (auto pin : runtimeOutputs.outputPins)
        ws281xPins.add(pin);

    auto apa102ClockPins = jsonDoc[APA102ClockPinsTag].to<JsonArray>();
    for (auto pin : runtimeOutputs.clockPins)
        apa102ClockPins.add(pin);

    if (includeSensitive)
        jsonDoc[OpenWeatherApiKeyTag] = openWeatherApiKey;

    return SetIfNotOverflowed(jsonDoc, jsonObject, __PRETTY_FUNCTION__);
}

bool DeviceConfig::DeserializeFromJSON(const JsonObjectConst& jsonObject)
{
    return DeserializeFromJSON(jsonObject, false);
}

bool DeviceConfig::DeserializeFromJSON(const JsonObjectConst& jsonObject, bool skipWrite)
{
    // If we're told to ignore saved config, we shouldn't touch anything
    if (IGNORE_SAVED_DEVICE_CONFIG)
        return true;

    // Add deserialization logic for additional settings to this code
    SetIfPresentIn(jsonObject, hostname, HostnameTag);
    SetIfPresentIn(jsonObject, location, LocationTag);
    SetIfPresentIn(jsonObject, locationIsZip, LocationIsZipTag);
    SetIfPresentIn(jsonObject, countryCode, CountryCodeTag);
    SetIfPresentIn(jsonObject, openWeatherApiKey, OpenWeatherApiKeyTag);
    SetIfPresentIn(jsonObject, use24HourClock, Use24HourClockTag);
    SetIfPresentIn(jsonObject, useCelsius, UseCelsiusTag);
    SetIfPresentIn(jsonObject, ntpServer, NTPServerTag);
    SetIfPresentIn(jsonObject, rememberCurrentEffect, RememberCurrentEffectTag);
    SetIfPresentIn(jsonObject, remoteEffectButtonsResetInterval, RemoteEffectButtonsResetIntervalTag);
    if (jsonObject[PowerLimitTag].is<int>())
    {
        const int savedPowerLimit = jsonObject[PowerLimitTag].as<int>();
        #ifdef POWER_LIMIT_MW
        if (!jsonObject[PowerLimitDefaultTag].is<int>() && savedPowerLimit == POWER_LIMIT_LEGACY_DEFAULT)
        {
            debugW("Ignoring saved legacy powerLimit default %d; using compiled default %d", savedPowerLimit, POWER_LIMIT_DEFAULT);
            powerLimit = POWER_LIMIT_DEFAULT;
        }
        else
        #endif
        {
            auto [isValid, validationMessage] = ValidatePowerLimit(savedPowerLimit);
            if (isValid)
                powerLimit = savedPowerLimit;
            else
            {
                debugW("Ignoring saved powerLimit %d: %s", savedPowerLimit, validationMessage.c_str());
                powerLimit = POWER_LIMIT_DEFAULT;
            }
        }
    }
    SetIfPresentIn(jsonObject, brightness, BrightnessTag);
    // Persisted config predates the newer brightness guardrails in some installs, so treat an invalid
    // saved brightness as "unset" and fall back to the normal 100% default instead of booting dark.
    if (brightness < BRIGHTNESS_MIN || brightness > BRIGHTNESS_MAX)
        brightness = BRIGHTNESS_MAX;
    // Only deserialize showVUMeter if the VU meter is enabled in the build
    #if SHOW_VU_METER
    SetIfPresentIn(jsonObject, showVUMeter, ShowVUMeterTag);
    #endif
    SetIfPresentIn(jsonObject, scheduleEnabled, ScheduleEnabledTag);
    SetIfPresentIn(jsonObject, scheduleDimPercent, ScheduleDimPercentTag);
    if (scheduleDimPercent > 100)
        scheduleDimPercent = 30;
    SetIfPresentIn(jsonObject, scheduleDimTime, ScheduleDimTimeTag);
    SetIfPresentIn(jsonObject, scheduleOffTime, ScheduleOffTimeTag);
    SetIfPresentIn(jsonObject, scheduleOnTime, ScheduleOnTimeTag);
    SetIfPresentIn(jsonObject, scheduleLatLongAuto, ScheduleLatLongAutoTag);
    SetIfPresentIn(jsonObject, scheduleLatitude, ScheduleLatitudeTag);
    SetIfPresentIn(jsonObject, scheduleLongitude, ScheduleLongitudeTag);
    SetIfPresentIn(jsonObject, scheduleLatLongStatus, ScheduleLatLongStatusTag);
    SetIfPresentIn(jsonObject, globalColor, GlobalColorTag);
    SetIfPresentIn(jsonObject, applyGlobalColors, ApplyGlobalColorsTag);
    SetIfPresentIn(jsonObject, secondColor, SecondColorTag);
    if (jsonObject[AudioInputPinTag].is<int>())
    {
        const int persistedAudioInputPin = jsonObject[AudioInputPinTag].as<int>();
        auto [pinValid, _] = ValidateAudioInputPin(persistedAudioInputPin);
        audioInputPin = pinValid ? persistedAudioInputPin : GetCompiledAudioInputPin();
    }

    RuntimeConfig updated = GetRuntimeConfig();

    if (jsonObject[ChannelShapesTag].is<JsonArrayConst>())
    {
        // Current format: parallel per-channel arrays.
        auto shapes = jsonObject[ChannelShapesTag].as<JsonArrayConst>();
        auto stripLengths = jsonObject[ChannelStripLengthsTag].as<JsonArrayConst>();
        auto matrixWidths = jsonObject[ChannelMatrixWidthsTag].as<JsonArrayConst>();
        auto matrixHeights = jsonObject[ChannelMatrixHeightsTag].as<JsonArrayConst>();
        auto matrixSerpentines = jsonObject[ChannelMatrixSerpentinesTag].as<JsonArrayConst>();
        auto matrixOrigins = jsonObject[ChannelMatrixOriginsTag].as<JsonArrayConst>();
        auto matrixAxes = jsonObject[ChannelMatrixAxesTag].as<JsonArrayConst>();

        for (size_t i = 0; i < updated.topology.channels.size() && i < shapes.size(); ++i)
        {
            auto& ch = updated.topology.channels[i];
            if (shapes[i].is<int>())
                ch.shape = static_cast<uint8_t>(shapes[i].as<int>()) == static_cast<uint8_t>(ChannelShape::Matrix)
                    ? ChannelShape::Matrix : ChannelShape::Strip;
            if (i < stripLengths.size() && stripLengths[i].is<int>())
                ch.stripLength = static_cast<uint16_t>(stripLengths[i].as<int>());
            if (i < matrixWidths.size() && matrixWidths[i].is<int>())
                ch.matrixWidth = static_cast<uint16_t>(matrixWidths[i].as<int>());
            if (i < matrixHeights.size() && matrixHeights[i].is<int>())
                ch.matrixHeight = static_cast<uint16_t>(matrixHeights[i].as<int>());
            if (i < matrixSerpentines.size() && matrixSerpentines[i].is<bool>())
                ch.matrixSerpentine = matrixSerpentines[i].as<bool>();
            if (i < matrixOrigins.size() && matrixOrigins[i].is<int>())
                ch.origin = static_cast<MatrixOrigin>(std::clamp(matrixOrigins[i].as<int>(), 0, 3));
            if (i < matrixAxes.size() && matrixAxes[i].is<int>())
                ch.axis = static_cast<SerpentineAxis>(std::clamp(matrixAxes[i].as<int>(), 0, 1));
        }
    }
    else if (jsonObject[LegacyMatrixWidthTag].is<int>() || jsonObject[LegacyMatrixLayoutTag].is<String>()
             || jsonObject[LegacyMatrixStripLengthsTag].is<JsonArrayConst>())
    {
        // One-way migration from a pre-per-channel-topology config: old layout==Matrix becomes
        // every channel Matrix-shaped with the old global width/height/serpentine (origin/axis
        // default to TopLeft/Vertical, which is exactly what the old hardcoded xy() formula did
        // - zero behavior change); old layout==IndividualStrips becomes every channel Strip-shaped
        // with its own old per-channel length. The next SaveToJSON() writes the new format only.
        bool legacyIsIndividualStrips = false;
        if (jsonObject[LegacyMatrixLayoutTag].is<String>())
        {
            const auto layoutName = jsonObject[LegacyMatrixLayoutTag].as<String>();
            legacyIsIndividualStrips = (layoutName == "individualStrips" || layoutName == "individual");
        }

        uint16_t legacyWidth = MATRIX_WIDTH, legacyHeight = MATRIX_HEIGHT;
        bool legacySerpentine = !IsHub75Build();
        SetIfPresentIn(jsonObject, legacyWidth, LegacyMatrixWidthTag);
        SetIfPresentIn(jsonObject, legacyHeight, LegacyMatrixHeightTag);
        SetIfPresentIn(jsonObject, legacySerpentine, LegacyMatrixSerpentineTag);

        std::array<uint16_t, NUM_CHANNELS> legacyStripLengths{};
        legacyStripLengths.fill(static_cast<uint16_t>(GetCompiledLEDCount()));
        if (jsonObject[LegacyMatrixStripLengthsTag].is<JsonArrayConst>())
        {
            auto lengths = jsonObject[LegacyMatrixStripLengthsTag].as<JsonArrayConst>();
            for (size_t i = 0; i < legacyStripLengths.size() && i < lengths.size(); ++i)
            {
                if (lengths[i].is<int>())
                    legacyStripLengths[i] = static_cast<uint16_t>(lengths[i].as<int>());
            }
        }
        else
        {
            // Even older configs persisted per-strip lengths as matrixStripLength0,
            // matrixStripLength1, ... (one tag per index).
            for (size_t i = 0; i < legacyStripLengths.size(); ++i)
            {
                const String tag = String(LegacyMatrixStripLength0Tag) + String(static_cast<unsigned>(i));
                if (jsonObject[tag].is<int>())
                    legacyStripLengths[i] = static_cast<uint16_t>(jsonObject[tag].as<int>());
            }
        }

        for (size_t i = 0; i < updated.topology.channels.size(); ++i)
        {
            auto& ch = updated.topology.channels[i];
            if (legacyIsIndividualStrips)
            {
                ch.shape = ChannelShape::Strip;
                ch.stripLength = legacyStripLengths[i];
            }
            else
            {
                ch.shape = ChannelShape::Matrix;
                ch.matrixWidth = legacyWidth;
                ch.matrixHeight = legacyHeight;
                ch.matrixSerpentine = legacySerpentine;
            }
            ch.origin = MatrixOrigin::TopLeft;
            ch.axis = SerpentineAxis::Vertical;
        }
    }
    // else: no persisted topology at all (brand new device) - keep the constructor defaults.

    // Sanitize any persisted values that look out of range (e.g. from a build with different
    // compiled limits) before they get re-serialised back to SPIFFS or shipped out as part of
    // the unified /settings or /api/v1/settings response.
    const uint16_t compiledMaxLEDs = static_cast<uint16_t>(GetCompiledLEDCount());
    for (auto& ch : updated.topology.channels)
    {
        if (ch.stripLength == 0 || ch.stripLength > compiledMaxLEDs)
        {
            debugW("Persisted channel stripLength %u out of range, resetting to %u", ch.stripLength, compiledMaxLEDs);
            ch.stripLength = compiledMaxLEDs;
        }
        if (ch.matrixWidth == 0 || ch.matrixWidth > compiledMaxLEDs)
        {
            debugW("Persisted channel matrixWidth %u out of range, resetting to %u", ch.matrixWidth, MATRIX_WIDTH);
            ch.matrixWidth = MATRIX_WIDTH;
        }
        if (ch.matrixHeight == 0 || ch.matrixHeight > compiledMaxLEDs)
        {
            debugW("Persisted channel matrixHeight %u out of range, resetting to %u", ch.matrixHeight, MATRIX_HEIGHT);
            ch.matrixHeight = MATRIX_HEIGHT;
        }
    }

    if (jsonObject[OutputDriverTag].is<String>())
    {
        const auto driverName = jsonObject[OutputDriverTag].as<String>();
        if (driverName == DriverName(OutputDriver::HUB75))
            updated.outputs.driver = OutputDriver::HUB75;
        else if (driverName == DriverName(OutputDriver::APA102))
            updated.outputs.driver = OutputDriver::APA102;
        else if (driverName == DriverName(OutputDriver::WS281x))
            updated.outputs.driver = OutputDriver::WS281x;
    }

    if (jsonObject[WS281xChannelCountTag].is<size_t>())
        updated.outputs.channelCount = jsonObject[WS281xChannelCountTag].as<size_t>();

    if (jsonObject[WS281xColorOrderTag].is<String>())
    {
        const auto colorOrderName = jsonObject[WS281xColorOrderTag].as<String>();
        if (colorOrderName == "RGB") updated.outputs.colorOrder = WS281xColorOrder::RGB;
        else if (colorOrderName == "RBG") updated.outputs.colorOrder = WS281xColorOrder::RBG;
        else if (colorOrderName == "GRB") updated.outputs.colorOrder = WS281xColorOrder::GRB;
        else if (colorOrderName == "GBR") updated.outputs.colorOrder = WS281xColorOrder::GBR;
        else if (colorOrderName == "BRG") updated.outputs.colorOrder = WS281xColorOrder::BRG;
        else if (colorOrderName == "BGR") updated.outputs.colorOrder = WS281xColorOrder::BGR;
    }

    if (jsonObject[WS281xPinsTag].is<JsonArrayConst>())
    {
        auto pinArray = jsonObject[WS281xPinsTag].as<JsonArrayConst>();
        for (size_t i = 0; i < updated.outputs.outputPins.size() && i < pinArray.size(); ++i)
        {
            if (pinArray[i].is<int>())
                updated.outputs.outputPins[i] = pinArray[i].as<int>();
        }
    }

    if (jsonObject[APA102ClockPinsTag].is<JsonArrayConst>())
    {
        auto pinArray = jsonObject[APA102ClockPinsTag].as<JsonArrayConst>();
        for (size_t i = 0; i < updated.outputs.clockPins.size() && i < pinArray.size(); ++i)
        {
            if (pinArray[i].is<int>())
                updated.outputs.clockPins[i] = pinArray[i].as<int>();
        }
    }

    auto [runtimeConfigValid, runtimeConfigError] = SetRuntimeConfig(updated, true);
    if (!runtimeConfigValid)
        debugW("Ignoring invalid persisted runtime config: %s", runtimeConfigError.c_str());

    if (ntpServer.isEmpty())
        ntpServer = NTP_SERVER_DEFAULT;

    if (jsonObject[TimeZoneTag].is<String>())
        return SetTimeZone(jsonObject[TimeZoneTag], true);

    if (!skipWrite)
        SaveToJSON();

    return true;
}

void DeviceConfig::RemovePersisted()
{
    RemoveJSONFile(DEVICE_CONFIG_FILE);
}

const String& DeviceConfig::GetTimeZone() const
{
    return timeZone;
}

void DeviceConfig::Set24HourClock(bool new24HourClock)
{
    SetAndSave(use24HourClock, new24HourClock);
}

void DeviceConfig::SetHostname(const String &newHostname)
{
    SetAndSave(hostname, newHostname);
}

void DeviceConfig::SetLocation(const String &newLocation)
{
    SetAndSave(location, newLocation);
}

void DeviceConfig::SetCountryCode(const String &newCountryCode)
{
    SetAndSave(countryCode, newCountryCode);
}

void DeviceConfig::SetLocationIsZip(bool newLocationIsZip)
{
    SetAndSave(locationIsZip, newLocationIsZip);
}

void DeviceConfig::SetOpenWeatherAPIKey(const String &newOpenWeatherAPIKey)
{
    SetAndSave(openWeatherApiKey, newOpenWeatherAPIKey);
}

void DeviceConfig::SetUseCelsius(bool newUseCelsius)
{
    SetAndSave(useCelsius, newUseCelsius);
}

void DeviceConfig::SetNTPServer(const String &newNTPServer)
{
    SetAndSave(ntpServer, newNTPServer);
}

void DeviceConfig::SetRememberCurrentEffect(bool newRememberCurrentEffect)
{
    SetAndSave(rememberCurrentEffect, newRememberCurrentEffect);
}

void DeviceConfig::SetRemoteEffectButtonsResetInterval(bool newRemoteEffectButtonsResetInterval)
{
    SetAndSave(remoteEffectButtonsResetInterval, newRemoteEffectButtonsResetInterval);
}

SuccessResultWithMessage DeviceConfig::ValidateBrightness(int newBrightness)
{
    if (newBrightness < BRIGHTNESS_MIN)
        return { false, String("brightness is below minimum value of ") + BRIGHTNESS_MIN };

    if (newBrightness > BRIGHTNESS_MAX)
        return { false, String("brightness is above maximum value of ") + BRIGHTNESS_MAX };

    return { true, "" };
}

SuccessResultWithMessage DeviceConfig::ValidateBrightness(const String& newBrightness)
{
    return ValidateBrightness(newBrightness.toInt());
}

void DeviceConfig::SetBrightness(int newBrightness)
{
    SetAndSave(brightness, uint8_t(std::clamp<int>(newBrightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX)));
}

void DeviceConfig::SetShowVUMeter(bool newShowVUMeter)
{
    // We only actually persist if the VU meter is enabled in the build
    #if SHOW_VU_METER
    SetAndSave(showVUMeter, newShowVUMeter);
    #else
    showVUMeter = newShowVUMeter;
    #endif
}

void DeviceConfig::SetScheduleEnabled(bool newScheduleEnabled)
{
    SetAndSave(scheduleEnabled, newScheduleEnabled);
}

SuccessResultWithMessage DeviceConfig::ValidateScheduleDimPercent(int newScheduleDimPercent)
{
    if (newScheduleDimPercent < 0 || newScheduleDimPercent > 100)
        return { false, "scheduleDimPercent must be between 0 and 100" };

    return { true, "" };
}

void DeviceConfig::SetScheduleDimPercent(int newScheduleDimPercent)
{
    SetAndSave(scheduleDimPercent, uint8_t(std::clamp(newScheduleDimPercent, 0, 100)));
}

SuccessResultWithMessage DeviceConfig::ValidateScheduleTime(const String& newScheduleTime)
{
    if (newScheduleTime == "sunrise" || newScheduleTime == "sunset" ||
        newScheduleTime == "noon" || newScheduleTime == "midnight")
        return { true, "" };

    // Otherwise must be "HH:MM", 24-hour, minute on a 15-minute step.
    const int colon = newScheduleTime.indexOf(':');
    if (colon < 1 || colon != newScheduleTime.length() - 3)
        return { false, "schedule time must be \"HH:MM\" or one of sunrise/sunset/noon/midnight" };

    const String hourPart = newScheduleTime.substring(0, colon);
    const String minutePart = newScheduleTime.substring(colon + 1);
    if (hourPart.length() == 0 || minutePart.length() != 2)
        return { false, "schedule time must be \"HH:MM\" or one of sunrise/sunset/noon/midnight" };

    for (size_t i = 0; i < hourPart.length(); i++)
        if (!isDigit(hourPart[i]))
            return { false, "schedule time hour must be numeric" };
    for (size_t i = 0; i < minutePart.length(); i++)
        if (!isDigit(minutePart[i]))
            return { false, "schedule time minute must be numeric" };

    const int hour = hourPart.toInt();
    const int minute = minutePart.toInt();
    if (hour < 0 || hour > 23)
        return { false, "schedule time hour must be between 00 and 23" };
    if (minute % 15 != 0 || minute < 0 || minute > 45)
        return { false, "schedule time minute must be a 15-minute step (00/15/30/45)" };

    return { true, "" };
}

void DeviceConfig::SetScheduleDimTime(const String& newScheduleDimTime)
{
    SetAndSave(scheduleDimTime, newScheduleDimTime);
}

void DeviceConfig::SetScheduleOffTime(const String& newScheduleOffTime)
{
    SetAndSave(scheduleOffTime, newScheduleOffTime);
}

void DeviceConfig::SetScheduleOnTime(const String& newScheduleOnTime)
{
    SetAndSave(scheduleOnTime, newScheduleOnTime);
}

void DeviceConfig::SetScheduleLatLongAuto(bool newScheduleLatLongAuto)
{
    SetAndSave(scheduleLatLongAuto, newScheduleLatLongAuto);
}

SuccessResultWithMessage DeviceConfig::ValidateScheduleLatitude(float newScheduleLatitude)
{
    if (newScheduleLatitude < -90.0f || newScheduleLatitude > 90.0f)
        return { false, "latitude must be between -90 and 90" };

    return { true, "" };
}

SuccessResultWithMessage DeviceConfig::ValidateScheduleLongitude(float newScheduleLongitude)
{
    if (newScheduleLongitude < -180.0f || newScheduleLongitude > 180.0f)
        return { false, "longitude must be between -180 and 180" };

    return { true, "" };
}

void DeviceConfig::SetScheduleLatitude(float newScheduleLatitude)
{
    SetAndSave(scheduleLatitude, std::clamp(newScheduleLatitude, -90.0f, 90.0f));
    _cachedSunEventEpochDay = -1;
}

void DeviceConfig::SetScheduleLongitude(float newScheduleLongitude)
{
    SetAndSave(scheduleLongitude, std::clamp(newScheduleLongitude, -180.0f, 180.0f));
    _cachedSunEventEpochDay = -1;
}

namespace
{
    constexpr double kDegToRad = M_PI / 180.0;
    constexpr double kRadToDeg = 180.0 / M_PI;

    // Sunrise equation (https://en.wikipedia.org/wiki/Sunrise_equation), accurate to within a
    // few minutes - plenty for a brightness schedule, and small enough to run once a day on an
    // ESP32 without pulling in a dedicated astronomy library. longitude is positive East.
    // Falls back to 06:00/18:00 UTC if the sun doesn't rise/set that day (polar latitudes).
    void ComputeSunTimesUtcMinutes(float latitudeDeg, float longitudeDeg, time_t nowUtc,
                                    double& sunriseUtcMinutes, double& sunsetUtcMinutes)
    {
        const double julianDate = (double)nowUtc / 86400.0 + 2440587.5;
        const double meanSolarNoon = std::floor(julianDate - 2451545.0 + 0.0008) - (double)longitudeDeg / 360.0;

        const double solarMeanAnomalyDeg = std::fmod(357.5291 + 0.98560028 * meanSolarNoon, 360.0);
        const double M = solarMeanAnomalyDeg * kDegToRad;

        const double centerDeg = 1.9148 * std::sin(M) + 0.0200 * std::sin(2 * M) + 0.0003 * std::sin(3 * M);
        double eclipticLongitudeDeg = std::fmod(solarMeanAnomalyDeg + centerDeg + 180.0 + 102.9372, 360.0);
        if (eclipticLongitudeDeg < 0)
            eclipticLongitudeDeg += 360.0;
        const double lambda = eclipticLongitudeDeg * kDegToRad;

        const double solarTransit = 2451545.0 + meanSolarNoon + 0.0053 * std::sin(M) - 0.0069 * std::sin(2 * lambda);
        const double declination = std::asin(std::sin(lambda) * std::sin(23.4397 * kDegToRad));
        const double phi = (double)latitudeDeg * kDegToRad;

        const double cosHourAngle = (std::sin(-0.833 * kDegToRad) - std::sin(phi) * std::sin(declination))
                                   / (std::cos(phi) * std::cos(declination));

        if (cosHourAngle < -1.0 || cosHourAngle > 1.0)
        {
            // Polar day/night: the sun doesn't cross the horizon today at this latitude.
            sunriseUtcMinutes = 6.0 * 60.0;
            sunsetUtcMinutes = 18.0 * 60.0;
            return;
        }

        const double hourAngleDeg = std::acos(cosHourAngle) * kRadToDeg;
        const double julianRise = solarTransit - hourAngleDeg / 360.0;
        const double julianSet = solarTransit + hourAngleDeg / 360.0;

        // A Julian Date's fractional part is 0 at noon UTC, so +0.5 shifts to a
        // fraction-of-day-since-midnight before scaling to minutes.
        auto fractionalDayToMinutes = [](double jd)
        {
            const double fractionalDay = jd + 0.5 - std::floor(jd + 0.5);
            return fractionalDay * 1440.0;
        };

        sunriseUtcMinutes = fractionalDayToMinutes(julianRise);
        sunsetUtcMinutes = fractionalDayToMinutes(julianSet);
    }
}

void DeviceConfig::ComputeSunEvents(float latitude, float longitude, time_t nowUtc,
                                     uint16_t& sunriseMinutesLocal, uint16_t& sunsetMinutesLocal)
{
    double sunriseUtc, sunsetUtc;
    ComputeSunTimesUtcMinutes(latitude, longitude, nowUtc, sunriseUtc, sunsetUtc);

    // Derive the local wall-clock offset from the currently configured TZ (DST-aware, since
    // it's read at "now") rather than assuming a fixed UTC offset.
    struct tm utcTm{};
    struct tm localTm{};
    gmtime_r(&nowUtc, &utcTm);
    localtime_r(&nowUtc, &localTm);
    const int utcMinutesOfDay = utcTm.tm_hour * 60 + utcTm.tm_min;
    const int localMinutesOfDay = localTm.tm_hour * 60 + localTm.tm_min;
    int offsetMinutes = localMinutesOfDay - utcMinutesOfDay;
    // The two reads are the same instant, so an offset outside +-12h only means the TZ shift
    // pushed the local date to the next/previous day - normalize back into range.
    if (offsetMinutes > 720) offsetMinutes -= 1440;
    if (offsetMinutes < -720) offsetMinutes += 1440;

    auto toLocalMinutes = [&](double utcMinutes)
    {
        int m = ((int)std::lround(utcMinutes) + offsetMinutes) % 1440;
        if (m < 0)
            m += 1440;
        return (uint16_t)m;
    };

    sunriseMinutesLocal = toLocalMinutes(sunriseUtc);
    sunsetMinutesLocal = toLocalMinutes(sunsetUtc);
}

void DeviceConfig::EnsureSunEventsCached() const
{
    const time_t now = time(nullptr);
    const int32_t today = (int32_t)(now / 86400);
    if (today == _cachedSunEventEpochDay)
        return;

    ComputeSunEvents(scheduleLatitude, scheduleLongitude, now, _cachedSunriseMinutes, _cachedSunsetMinutes);
    _cachedSunEventEpochDay = today;
}

uint16_t DeviceConfig::ResolveScheduleMinutes(const String& token) const
{
    if (token == "noon")
        return 12 * 60;
    if (token == "midnight")
        return 0;
    if (token == "sunrise" || token == "sunset")
    {
        EnsureSunEventsCached();
        return token == "sunrise" ? _cachedSunriseMinutes : _cachedSunsetMinutes;
    }

    // "HH:MM"
    const int colon = token.indexOf(':');
    if (colon < 1)
        return 0;

    const int hour = std::clamp((int)token.substring(0, colon).toInt(), 0, 23);
    const int minute = std::clamp((int)token.substring(colon + 1).toInt(), 0, 59);
    return (uint16_t)(hour * 60 + minute);
}

uint8_t DeviceConfig::GetScheduleDimFactor255() const
{
    if (!scheduleEnabled)
        return 255;

#if ENABLE_NTP
    // Without a valid wall-clock time there's no way to know whether "now" falls inside
    // the dim/off window - time(nullptr) reads back whatever the RTC defaults to (often
    // close to epoch 0) until NTP has synced at least once after boot. Defaulting to
    // "normal" here would risk exactly what this feature exists to prevent (lights left
    // on overnight) for however long that takes; defaulting to off is the safe failure
    // mode, and it self-corrects within seconds of NTP completing.
    if (!NTPTimeClient::HasClockBeenSet())
        return 0;
#endif

    const uint16_t dimMinutes = ResolveScheduleMinutes(scheduleDimTime);
    const uint16_t offMinutes = ResolveScheduleMinutes(scheduleOffTime);
    const uint16_t onMinutes  = ResolveScheduleMinutes(scheduleOnTime);

    struct tm localTm{};
    const time_t now = time(nullptr);
    localtime_r(&now, &localTm);
    const uint16_t nowMinutes = (uint16_t)(localTm.tm_hour * 60 + localTm.tm_min);

    // A typical schedule dims in the evening, goes off after midnight, and comes back on the
    // next morning - i.e. it straddles the 0/1440 wraparound. Shifting everything by half a day
    // moves that wraparound to midday instead, where a plain nightly schedule never lands, so
    // the three thresholds can be compared with simple linear less-than checks.
    auto shift = [](uint16_t m) { return (uint16_t)(((int)m - 720 + 1440) % 1440); };

    const uint16_t shiftedNow = shift(nowMinutes);
    const uint16_t shiftedDim = shift(dimMinutes);
    const uint16_t shiftedOff = shift(offMinutes);
    const uint16_t shiftedOn  = shift(onMinutes);

    if (shiftedNow < shiftedDim || shiftedNow >= shiftedOn)
        return 255;
    if (shiftedNow < shiftedOff)
        return (uint8_t)std::lround(255.0 * scheduleDimPercent / 100.0);

    return 0;
}

SuccessResultWithMessage DeviceConfig::ValidatePowerLimit(int newPowerLimit)
{
    if (newPowerLimit < POWER_LIMIT_MIN)
        return { false, String("powerLimit is below minimum value of ") + POWER_LIMIT_MIN };

    return { true, "" };
}

SuccessResultWithMessage DeviceConfig::ValidatePowerLimit(const String& newPowerLimit)
{
    return ValidatePowerLimit(newPowerLimit.toInt());
}

void DeviceConfig::SetPowerLimit(int newPowerLimit)
{
    auto [isValid, _] = ValidatePowerLimit(newPowerLimit);
    if (isValid)
        SetAndSave(powerLimit, newPowerLimit);
}

void DeviceConfig::SetApplyGlobalColors()
{
    SetAndSave(applyGlobalColors, true);
}

void DeviceConfig::ClearApplyGlobalColors()
{
    SetAndSave(applyGlobalColors, false);
}

void DeviceConfig::SetGlobalColor(const CRGB& newGlobalColor)
{
    SetAndSave(globalColor, newGlobalColor);
}

void DeviceConfig::SetSecondColor(const CRGB& newSecondColor)
{
    SetAndSave(secondColor, newSecondColor);
}

SuccessResultWithMessage DeviceConfig::ValidateAudioInputPin(int pin) const
{
    if (pin < -1)
        return { false, "audio input pin must be -1 or a valid GPIO" };

    if (pin == GetCompiledAudioInputPin())
        return { true, "" };

    // The settings API now separates "compiled default" from "active value". External I2S mics can
    // move their DIN pin at boot, but the M5 onboard mic path and the current ADC path are still fixed.
    if (!SupportsConfigurableAudioInputPin())
        return { false, DeviceConfigInternal::RecompileNeededMessage() };

    if (pin == -1)
        return { true, "" };

    if (!GPIO_IS_VALID_GPIO(static_cast<gpio_num_t>(pin)))
        return { false, "audio input pin must be a valid GPIO" };

    return { true, "" };
}

void DeviceConfig::SetAudioInputPin(int newAudioInputPin)
{
    auto [isValid, _] = ValidateAudioInputPin(newAudioInputPin);
    if (!isValid)
        return;

    if (audioInputPin == newAudioInputPin)
        return;

    SetAndSave(audioInputPin, static_cast<int8_t>(newAudioInputPin));
    LogRuntimeConfig("audio input pin changed");
}

// This setter separates "apply the timezone to the running process" from "persist a user edit".
// Startup/config-load needs to set TZ immediately so localtime() is correct, but it must not
// immediately rewrite device.cfg just because we re-applied the already-persisted value.
// The timezone JSON file used by this logic is generated using tools/gen-tz-json.py
bool DeviceConfig::SetTimeZone(const String& newTimeZone, bool skipWrite)
{
    String quotedTZ = "\n\"" + newTimeZone + '"';

    const char *start = strstr(timezones_start, quotedTZ.c_str());

    // If we can't find the new timezone as a timezone name, assume it's a literal value
    if (start == nullptr)
        setenv("TZ", newTimeZone.c_str(), 1);
    // We received a timezone name, so we extract and use its timezone value
    else
    {
        start += quotedTZ.length();
        start = strchr(start, '"');
        if (start == nullptr)      // Can't actually happen unless timezone file is malformed
            return false;

        start++;
        const char *end = strchr(start, '"');
        if (end == nullptr)        // Can't actually happen unless timezone file is malformed
            return false;

        size_t length = end - start;

        std::unique_ptr<char[]> value = std::make_unique<char[]>(length + 1);
        strncpy(value.get(), start, length);
        value[length] = 0;

        setenv("TZ", value.get(), 1);
    }

    tzset();

    timeZone = newTimeZone;
    if (!skipWrite)
        SaveToJSON();

    return true;
}

#if ENABLE_WIFI
SuccessResultWithMessage DeviceConfig::ValidateOpenWeatherAPIKey(const String &newOpenWeatherAPIKey)
{
    HTTPClient http;

    String url = "http://api.openweathermap.org/data/2.5/weather?lat=0&lon=0&appid=" + urlEncode(newOpenWeatherAPIKey);

    http.begin(url);

    switch (http.GET())
    {
        case HTTP_CODE_OK:
        {
            http.end();
            return { true, "" };
        }

        case HTTP_CODE_UNAUTHORIZED:
        {
            auto jsonDoc = CreateJsonDocument();
            deserializeJson(jsonDoc, http.getString());

            String message = "";
            if (jsonDoc["message"].is<String>())
                message = jsonDoc["message"].as<String>();

            http.end();
            return { false, message };
        }

        // Anything else
        default:
        {
            http.end();
            return { false, "Unable to validate" };
        }
    }
}

// ResolveScheduleLatLongFromLocation
//
// Mirrors PatternWeather::updateCoordinates() - same OpenWeatherMap geocoding endpoints,
// same request shape - but writes the result into the schedule's lat/long instead of an
// effect-local cache. Best-effort: any failure (no key, no network, bad location) just
// leaves the existing scheduleLatitude/scheduleLongitude in place.
bool DeviceConfig::ResolveScheduleLatLongFromLocation()
{
    if (location.isEmpty() || openWeatherApiKey.isEmpty())
    {
        scheduleLatLongStatus = "Not attempted: set a location and an Open Weather API key first.";
        return false;
    }

    HTTPClient http;
    String url;
    if (locationIsZip)
        url = "http://api.openweathermap.org/geo/1.0/zip"
            "?zip=" + urlEncode(location) + "," + urlEncode(countryCode) + "&appid=" + urlEncode(openWeatherApiKey);
    else
        url = "http://api.openweathermap.org/geo/1.0/direct"
            "?q=" + urlEncode(location) + "," + urlEncode(countryCode) + "&limit=1&appid=" + urlEncode(openWeatherApiKey);

    http.begin(url);
    const int httpResponseCode = http.GET();
    if (httpResponseCode != HTTP_CODE_OK)
    {
        debugW("ResolveScheduleLatLongFromLocation: geocoding request for '%s' failed (HTTP %d)", location.c_str(), httpResponseCode);
        http.end();
        scheduleLatLongStatus = String("Failed: geocoding request returned HTTP ") + httpResponseCode
            + ". Check the API key and, for a postal code, that it's formatted the way Open Weather expects.";
        return false;
    }

    auto doc = CreateJsonDocument();
    deserializeJson(doc, http.getString());
    JsonObject coordinates = locationIsZip ? doc.as<JsonObject>() : doc[0].as<JsonObject>();
    http.end();

    if (!coordinates["lat"].is<float>() || !coordinates["lon"].is<float>())
    {
        debugW("ResolveScheduleLatLongFromLocation: no coordinates found for '%s'", location.c_str());
        // Open Weather's zip geocoding is only reliable with the outward/prefix portion of
        // alphanumeric postal codes (e.g. UK, Canada) - a full 6-character Canadian postal
        // code commonly returns no match where the 3-character FSA (e.g. "K1A") does.
        if (locationIsZip && countryCode == "CA" && location.length() > 3)
            scheduleLatLongStatus = "Failed: no coordinates found for '" + location
                + "'. Open Weather's postal code lookup for Canada usually only works with the "
                + "3-character forward sortation area (e.g. \"K1A\" instead of \"K1A 0A6\") - try that.";
        else
            scheduleLatLongStatus = "Failed: no coordinates found for '" + location + "'.";
        return false;
    }

    SetScheduleLatitude(coordinates["lat"].as<float>());
    SetScheduleLongitude(coordinates["lon"].as<float>());
    scheduleLatLongStatus = "OK: resolved '" + location + "' to " + String(GetScheduleLatitude(), 4)
        + ", " + String(GetScheduleLongitude(), 4) + ".";
    return true;
}
#else
bool DeviceConfig::ResolveScheduleLatLongFromLocation()
{
    scheduleLatLongStatus = "Not attempted: this build has no WiFi/network support.";
    return false;
}
#endif  // ENABLE_WIFI
