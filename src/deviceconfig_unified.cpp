//+--------------------------------------------------------------------------
//
// File:        deviceconfig_unified.cpp
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of deviceconfig.cpp; see that file header for additional context.
//
// Split scope: unified settings API mapping and DeviceConfig integration helpers.
//---------------------------------------------------------------------------


#include "globals.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

#include "audioservice.h"
#include "deviceconfig.h"
#include "deviceconfig_internal.h"
#include "jsonserializer.h"
#include "systemcontainer.h"

const char* DeviceConfig::OutputDriverName(OutputDriver driver)
{
    return DriverName(driver);
}

const char* DeviceConfig::WS281xColorOrderName(WS281xColorOrder colorOrder)
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

std::optional<DeviceConfig::OutputDriver> DeviceConfig::ParseOutputDriverName(const String& name)
{
    if (name == "hub75")
        return OutputDriver::HUB75;
    if (name == "apa102")
        return OutputDriver::APA102;
    if (name == "ws281x")
        return OutputDriver::WS281x;

    return std::nullopt;
}

std::optional<DeviceConfig::WS281xColorOrder> DeviceConfig::ParseWS281xColorOrderName(const String& name)
{
    if (name == "RGB") return WS281xColorOrder::RGB;
    if (name == "RBG") return WS281xColorOrder::RBG;
    if (name == "GRB") return WS281xColorOrder::GRB;
    if (name == "GBR") return WS281xColorOrder::GBR;
    if (name == "BRG") return WS281xColorOrder::BRG;
    if (name == "BGR") return WS281xColorOrder::BGR;

    return std::nullopt;
}

const char* DeviceConfig::MatrixOriginName(MatrixOrigin origin)
{
    switch (origin)
    {
        case MatrixOrigin::TopLeft:     return "topLeft";
        case MatrixOrigin::TopRight:    return "topRight";
        case MatrixOrigin::BottomLeft:  return "bottomLeft";
        case MatrixOrigin::BottomRight: return "bottomRight";
        default:                        return "topLeft";
    }
}

const char* DeviceConfig::SerpentineAxisName(SerpentineAxis axis)
{
    return axis == SerpentineAxis::Horizontal ? "horizontal" : "vertical";
}

std::optional<MatrixOrigin> DeviceConfig::ParseMatrixOriginName(const String& name)
{
    if (name == "topLeft") return MatrixOrigin::TopLeft;
    if (name == "topRight") return MatrixOrigin::TopRight;
    if (name == "bottomLeft") return MatrixOrigin::BottomLeft;
    if (name == "bottomRight") return MatrixOrigin::BottomRight;

    return std::nullopt;
}

std::optional<SerpentineAxis> DeviceConfig::ParseSerpentineAxisName(const String& name)
{
    if (name == "horizontal") return SerpentineAxis::Horizontal;
    if (name == "vertical") return SerpentineAxis::Vertical;

    return std::nullopt;
}

SuccessResultWithMessage DeviceConfig::SetRuntimeConfig(const RuntimeConfig& config, bool skipWrite)
{
    auto [isValid, validationMessage] = ValidateRuntimeConfig(config);
    if (!isValid)
        return { false, validationMessage };

    const bool changed =
        runtimeTopology.channels != config.topology.channels
        || runtimeOutputs.driver != config.outputs.driver
        || runtimeOutputs.channelCount != config.outputs.channelCount
        || runtimeOutputs.outputPins != config.outputs.outputPins
        || runtimeOutputs.clockPins != config.outputs.clockPins
        || runtimeOutputs.colorOrder != config.outputs.colorOrder;

    runtimeTopology = config.topology;
    runtimeOutputs = config.outputs;

    if (!skipWrite)
        SaveToJSON();

    if (changed && !skipWrite)
        LogRuntimeConfig("runtime config changed");

    return { true, "" };
}

void DeviceConfig::AppendPins(JsonArray target, const std::array<int8_t, NUM_CHANNELS>& pins)
{
    for (auto pin : pins)
        target.add(pin);
}

ResultWithMessage<std::optional<int>> DeviceConfig::ResolveUnifiedAudioInputPin(JsonObjectConst device)
{
    std::optional<int> requestedAudioInputPin;

    if (device[DeviceConfig::AudioInputPinTag].is<int>())
        requestedAudioInputPin = device[DeviceConfig::AudioInputPinTag].as<int>();

    if (device["audio"].is<JsonObjectConst>())
    {
        auto audio = device["audio"].as<JsonObjectConst>();
        if (audio["audioInputPin"].is<int>())
        {
            const int nestedAudioInputPin = audio["audioInputPin"].as<int>();
            if (requestedAudioInputPin.has_value() && requestedAudioInputPin.value() != nestedAudioInputPin)
                return { std::nullopt, "Malformed request" };

            requestedAudioInputPin = nestedAudioInputPin;
        }
    }

    return { requestedAudioInputPin, "" };
}

namespace
{
    ResultWithMessage<std::optional<uint16_t>> ParseTopologyDimension(JsonObjectConst topology, const char* key)
    {
        auto value = topology[key];
        if (value.isNull())
            return { std::nullopt, "" };

        if (!value.is<size_t>())
            return { std::nullopt, String(key) + " must be a positive integer" };

        const size_t dimension = value.as<size_t>();
        if (dimension == 0)
            return { std::nullopt, String(key) + " must be greater than zero" };

        if (dimension > std::numeric_limits<uint16_t>::max())
            return { std::nullopt, String(key) + " is too large" };

        return { static_cast<uint16_t>(dimension), "" };
    }
}

void DeviceConfig::SerializeUnifiedSettings(JsonObject root) const
{
    auto device = root["device"].to<JsonObject>();
    device["hostname"] = GetHostname();
    device["location"] = GetLocation();
    device["locationIsZip"] = IsLocationZip();
    device["countryCode"] = GetCountryCode();
    device["timeZone"] = GetTimeZone();
    device["use24HourClock"] = Use24HourClock();
    device["useCelsius"] = UseCelsius();
    device["ntpServer"] = GetNTPServer();
    // The key itself is write-only and never serialized; this just lets the UI show a
    // masked placeholder instead of an always-blank field regardless of whether one is set.
    device["openWeatherApiKeySet"] = !GetOpenWeatherAPIKey().isEmpty();
    device["rememberCurrentEffect"] = RememberCurrentEffect();
    device["powerLimit"] = GetPowerLimit();
    device["brightness"] = GetBrightness();
    device["globalColor"] = GlobalColor();
    device["secondColor"] = SecondColor();
    device["applyGlobalColors"] = ApplyGlobalColors();

    auto schedule = device["schedule"].to<JsonObject>();
    schedule["enabled"] = ScheduleEnabled();
    schedule["dimPercent"] = GetScheduleDimPercent();
    schedule["dimTime"] = GetScheduleDimTime();
    schedule["offTime"] = GetScheduleOffTime();
    schedule["onTime"] = GetScheduleOnTime();
    schedule["latLongAuto"] = ScheduleLatLongAuto();
    schedule["latitude"] = GetScheduleLatitude();
    schedule["longitude"] = GetScheduleLongitude();
    schedule["latLongStatus"] = GetScheduleLatLongStatus();

    auto remote = device["remote"].to<JsonObject>();
    #if ENABLE_REMOTE
    remote["enabled"] = true;
    remote["pin"] = IR_REMOTE_PIN;
    #else
    remote["enabled"] = false;
    remote["pin"] = -1;
    #endif
    remote["resetEffectInterval"] = RemoteEffectButtonsResetInterval();

    auto audio = device["audio"].to<JsonObject>();
    audio["enabled"] =
    #if ENABLE_AUDIO
        true;
    #else
        false;
    #endif
    audio["audioInputPin"] = GetAudioInputPin();
    audio["compiledDefaultPin"] = GetCompiledAudioInputPin();
    audio["mode"] = GetAudioInputModeName();
    audio["liveApply"] = SupportsLiveAudioInputReconfigure();
    audio["requiresReboot"] = !SupportsLiveAudioInputReconfigure();
    audio["supportsPinOverride"] = SupportsConfigurableAudioInputPin();

    auto topology = root["topology"].to<JsonObject>();
    // Read-only summary for display/back-compat (e.g. formatTopologySummary() client-side) -
    // "matrix" | "individualStrips" | "mixed". Never parsed as input; per-channel shape is the
    // only way to actually change the topology now.
    topology["layout"] = GetLayoutSummary();

    auto channels = topology["channels"].to<JsonArray>();
    for (size_t i = 0; i < runtimeTopology.channels.size(); ++i)
    {
        const auto& ch = runtimeTopology.channels[i];
        auto channelObj = channels.add<JsonObject>();
        channelObj["shape"] = ch.shape == ChannelShape::Matrix ? "matrix" : "strip";
        channelObj["stripLength"] = ch.stripLength;
        channelObj["matrixWidth"] = ch.matrixWidth;
        channelObj["matrixHeight"] = ch.matrixHeight;
        channelObj["matrixSerpentine"] = ch.matrixSerpentine;
        channelObj["matrixOrigin"] = MatrixOriginName(ch.origin);
        channelObj["matrixAxis"] = SerpentineAxisName(ch.axis);
    }

    topology["ledCount"] = GetActiveLEDCount();
    topology["liveApply"] = SupportsLiveTopology();

    auto outputs = root["outputs"].to<JsonObject>();
    outputs["driver"] = GetRuntimeDriverName();
    outputs["compiledDriver"] = GetCompiledDriverName();
    outputs["liveApply"] = SupportsLiveOutputReconfigure();

    auto ws281x = outputs["ws281x"].to<JsonObject>();
    ws281x["channelCount"] = GetChannelCount();
    ws281x["compiledMaxChannels"] = GetCompiledChannelCount();
    ws281x["colorOrder"] = GetColorOrderName(GetWS281xColorOrder());
    ws281x["compiledColorOrder"] = GetColorOrderName(GetCompiledWS281xColorOrder());
    AppendPins(ws281x["pins"].to<JsonArray>(), GetWS281xPins());

    auto apa102 = outputs["apa102"].to<JsonObject>();
    apa102["channelCount"] = GetChannelCount();
    apa102["compiledMaxChannels"] = GetCompiledChannelCount();
    apa102["colorOrder"] = GetColorOrderName(GetWS281xColorOrder());
    apa102["compiledColorOrder"] = GetColorOrderName(GetCompiledWS281xColorOrder());
    AppendPins(apa102["dataPins"].to<JsonArray>(), GetAPA102DataPins());
    AppendPins(apa102["clockPins"].to<JsonArray>(), GetAPA102ClockPins());
}

void DeviceConfig::SerializeUnifiedSettingsSchema(JsonObject root) const
{
    auto topology = root["topology"].to<JsonObject>();
    topology["compiledMaxWidth"] =
        GetCompiledOutputDriver() == OutputDriver::HUB75
            ? GetCompiledMatrixWidth()
            : GetCompiledLEDCount();
    topology["compiledMaxHeight"] =
        GetCompiledOutputDriver() == OutputDriver::HUB75
            ? GetCompiledMatrixHeight()
            : GetCompiledLEDCount();
    topology["compiledNominalWidth"] = GetCompiledMatrixWidth();
    topology["compiledNominalHeight"] = GetCompiledMatrixHeight();
    topology["compiledMaxLEDs"] = GetCompiledLEDCount();
    topology["liveApply"] = SupportsLiveTopology();
    topology["rejectMessage"] = DeviceConfigInternal::RecompileNeededMessage();
    // HUB75 panels have a fixed matrix layout baked into the firmware and never expose
    // per-channel topology settings at all (see the IsHub75Build() gate in
    // deviceconfig_settings_specs.cpp); non-HUB75 builds get one channel{i}Shape/dims/origin/
    // axis set of settings per compiled channel, with options inline on those specs.
    topology["compiledMaxChannels"] = GetCompiledChannelCount();
    topology["compiledMaxStripLength"] = GetCompiledLEDCount();

    auto outputs = root["outputs"].to<JsonObject>();
    outputs["compiledDriver"] = GetCompiledDriverName();
    outputs["liveApply"] = SupportsLiveOutputReconfigure();
    outputs["rejectMessage"] = DeviceConfigInternal::RecompileNeededMessage();

    auto drivers = outputs["allowedDrivers"].to<JsonArray>();
    drivers.add(GetCompiledDriverName());

    auto ws281x = outputs["ws281x"].to<JsonObject>();
    const auto compiledChannels = GetCompiledChannelCount();
    ws281x["compiledMaxChannels"] = compiledChannels;
    ws281x["compiledMaxLEDs"] = GetCompiledLEDCount();
    ws281x["compiledColorOrder"] = GetColorOrderName(GetCompiledWS281xColorOrder());
    auto allowedChannelCounts = ws281x["allowedChannelCounts"].to<JsonArray>();
    for (size_t channel = 1; channel <= compiledChannels; ++channel)
        allowedChannelCounts.add(channel);

    auto allowedColorOrders = ws281x["allowedColorOrders"].to<JsonArray>();
    allowedColorOrders.add("RGB");
    allowedColorOrders.add("RBG");
    allowedColorOrders.add("GRB");
    allowedColorOrders.add("GBR");
    allowedColorOrders.add("BRG");
    allowedColorOrders.add("BGR");
    AppendPins(ws281x["compiledPins"].to<JsonArray>(), GetCompiledWS281xPins());

    auto apa102 = outputs["apa102"].to<JsonObject>();
    apa102["compiledMaxChannels"] = compiledChannels;
    apa102["compiledMaxLEDs"] = GetCompiledLEDCount();
    apa102["compiledColorOrder"] = GetColorOrderName(GetCompiledWS281xColorOrder());
    auto apa102AllowedChannelCounts = apa102["allowedChannelCounts"].to<JsonArray>();
    for (size_t channel = 1; channel <= compiledChannels; ++channel)
        apa102AllowedChannelCounts.add(channel);

    auto apa102AllowedColorOrders = apa102["allowedColorOrders"].to<JsonArray>();
    apa102AllowedColorOrders.add("RGB");
    apa102AllowedColorOrders.add("RBG");
    apa102AllowedColorOrders.add("GRB");
    apa102AllowedColorOrders.add("GBR");
    apa102AllowedColorOrders.add("BRG");
    apa102AllowedColorOrders.add("BGR");
    AppendPins(apa102["compiledDataPins"].to<JsonArray>(), GetCompiledWS281xPins());
    AppendPins(apa102["compiledClockPins"].to<JsonArray>(), GetCompiledAPA102ClockPins());

    auto device = root["device"].to<JsonObject>();
    auto remote = device["remote"].to<JsonObject>();
    remote["enabled"] =
    #if ENABLE_REMOTE
        true;
    #else
        false;
    #endif
    remote["pin"] = IR_REMOTE_PIN;
    remote["defaultResetEffectInterval"] = true;

    auto audio = device["audio"].to<JsonObject>();
    audio["enabled"] =
    #if ENABLE_AUDIO
        true;
    #else
        false;
    #endif
    audio["compiledDefaultPin"] = GetCompiledAudioInputPin();
    audio["mode"] = GetAudioInputModeName();
    audio["liveApply"] = SupportsLiveAudioInputReconfigure();
    audio["requiresReboot"] = !SupportsLiveAudioInputReconfigure();
    audio["supportsPinOverride"] = SupportsConfigurableAudioInputPin();
    audio["rejectMessage"] = DeviceConfigInternal::RecompileNeededMessage();

    struct SectionInfo
    {
        const char* id;
        const char* title;
        const char* description;
    };

    static constexpr SectionInfo kSections[] =
    {
        { "topology",   "Topology",          "Active matrix dimensions and layout." },
        { "output",     "Output",            "LED driver, channel count, color order, and per-channel pin assignments." },
        { "appearance", "Appearance",        "Brightness, colors, effect rotation, and visual preferences." },
        { "schedule",   "Nightly Schedule",  "Automatic dimming and on/off times, including sunrise/sunset." },
        { "audio",      "Audio",             "Microphone input pin and audio capture configuration." },
        { "location",   "Location",          "Where the device is for weather and timezone defaults." },
        { "clock",      "Clock & Weather",   "Time display, NTP, and weather API options." },
        { "system",     "System",            "Identification, power limits, and other system-wide options." },
    };

    auto sections = root["sections"].to<JsonArray>();
    for (const auto& info : kSections)
    {
        auto entry = sections.add<JsonObject>();
        entry["id"] = info.id;
        entry["title"] = info.title;
        entry["description"] = info.description;
    }
}

SuccessResultWithMessage DeviceConfig::ParseAndValidateUnifiedSettings(JsonObjectConst root, UnifiedSettingsRequest& out) const
{
    out = UnifiedSettingsRequest{};
    out.requestedRuntimeConfig = GetRuntimeConfig();

    if (root["topology"].is<JsonObjectConst>())
    {
        auto topology = root["topology"].as<JsonObjectConst>();
        // HUB75 never accepts per-channel topology (it's always the compiled panel) - a stray
        // request carrying topology.channels is defensively rejected rather than silently ignored.
        if (topology["channels"].is<JsonArrayConst>())
        {
            if (IsHub75Build())
                return { false, DeviceConfigInternal::RecompileNeededMessage() };

            auto channels = topology["channels"].as<JsonArrayConst>();
            auto& targetChannels = out.requestedRuntimeConfig.topology.channels;
            for (size_t i = 0; i < targetChannels.size() && i < channels.size(); ++i)
            {
                if (!channels[i].is<JsonObjectConst>())
                    continue;

                auto channelObj = channels[i].as<JsonObjectConst>();
                auto& ch = targetChannels[i];

                if (channelObj["shape"].is<String>())
                {
                    const auto shapeName = channelObj["shape"].as<String>();
                    if (shapeName != "strip" && shapeName != "matrix")
                        return { false, String("topology.channels[") + i + "].shape must be \"strip\" or \"matrix\"" };
                    ch.shape = shapeName == "matrix" ? ChannelShape::Matrix : ChannelShape::Strip;
                }

                auto [requestedStripLength, stripLengthMessage] = ParseTopologyDimension(channelObj, "stripLength");
                if (!requestedStripLength.has_value() && !stripLengthMessage.isEmpty())
                    return { false, String("topology.channels[") + i + "]." + stripLengthMessage };
                if (requestedStripLength.has_value())
                    ch.stripLength = requestedStripLength.value();

                auto [requestedWidth, widthMessage] = ParseTopologyDimension(channelObj, "matrixWidth");
                if (!requestedWidth.has_value() && !widthMessage.isEmpty())
                    return { false, String("topology.channels[") + i + "]." + widthMessage };
                if (requestedWidth.has_value())
                    ch.matrixWidth = requestedWidth.value();

                auto [requestedHeight, heightMessage] = ParseTopologyDimension(channelObj, "matrixHeight");
                if (!requestedHeight.has_value() && !heightMessage.isEmpty())
                    return { false, String("topology.channels[") + i + "]." + heightMessage };
                if (requestedHeight.has_value())
                    ch.matrixHeight = requestedHeight.value();

                if (channelObj["matrixSerpentine"].is<bool>())
                    ch.matrixSerpentine = channelObj["matrixSerpentine"].as<bool>();

                if (channelObj["matrixOrigin"].is<String>())
                {
                    auto parsedOrigin = ParseMatrixOriginName(channelObj["matrixOrigin"].as<String>());
                    if (!parsedOrigin.has_value())
                        return { false, String("topology.channels[") + i + "].matrixOrigin is invalid" };
                    ch.origin = parsedOrigin.value();
                }

                if (channelObj["matrixAxis"].is<String>())
                {
                    auto parsedAxis = ParseSerpentineAxisName(channelObj["matrixAxis"].as<String>());
                    if (!parsedAxis.has_value())
                        return { false, String("topology.channels[") + i + "].matrixAxis is invalid" };
                    ch.axis = parsedAxis.value();
                }
            }
            out.runtimeConfigTouched = true;
        }
    }

    if (root["outputs"].is<JsonObjectConst>())
    {
        auto outputs = root["outputs"].as<JsonObjectConst>();
        if (outputs["driver"].is<String>())
        {
            const auto driver = outputs["driver"].as<String>();
            auto parsedDriver = ParseOutputDriverName(driver);
            if (!parsedDriver.has_value())
                return { false, "invalid output driver" };

            out.requestedRuntimeConfig.outputs.driver = parsedDriver.value();
            out.runtimeConfigTouched = true;
        }

        if (outputs["ws281x"].is<JsonObjectConst>())
        {
            auto ws281x = outputs["ws281x"].as<JsonObjectConst>();
            if (ws281x["channelCount"].is<size_t>())
            {
                out.requestedRuntimeConfig.outputs.channelCount = ws281x["channelCount"].as<size_t>();
                out.runtimeConfigTouched = true;
            }
            if (ws281x["colorOrder"].is<String>())
            {
                auto colorOrder = ParseWS281xColorOrderName(ws281x["colorOrder"].as<String>());
                if (!colorOrder.has_value())
                    return { false, "invalid WS281x color order" };

                out.requestedRuntimeConfig.outputs.colorOrder = colorOrder.value();
                out.runtimeConfigTouched = true;
            }
            if (ws281x["pins"].is<JsonArrayConst>())
            {
                auto pins = ws281x["pins"].as<JsonArrayConst>();
                for (size_t i = 0; i < out.requestedRuntimeConfig.outputs.outputPins.size() && i < pins.size(); ++i)
                {
                    if (pins[i].is<int>())
                    {
                        out.requestedRuntimeConfig.outputs.outputPins[i] = pins[i].as<int>();
                        out.runtimeConfigTouched = true;
                    }
                }
            }
        }

        if (outputs["apa102"].is<JsonObjectConst>())
        {
            auto apa102 = outputs["apa102"].as<JsonObjectConst>();
            if (apa102["channelCount"].is<size_t>())
            {
                out.requestedRuntimeConfig.outputs.channelCount = apa102["channelCount"].as<size_t>();
                out.runtimeConfigTouched = true;
            }
            if (apa102["colorOrder"].is<String>())
            {
                auto colorOrder = ParseWS281xColorOrderName(apa102["colorOrder"].as<String>());
                if (!colorOrder.has_value())
                    return { false, "invalid APA102 color order" };

                out.requestedRuntimeConfig.outputs.colorOrder = colorOrder.value();
                out.runtimeConfigTouched = true;
            }
            if (apa102["dataPins"].is<JsonArrayConst>())
            {
                auto pins = apa102["dataPins"].as<JsonArrayConst>();
                for (size_t i = 0; i < out.requestedRuntimeConfig.outputs.outputPins.size() && i < pins.size(); ++i)
                {
                    if (pins[i].is<int>())
                    {
                        out.requestedRuntimeConfig.outputs.outputPins[i] = pins[i].as<int>();
                        out.runtimeConfigTouched = true;
                    }
                }
            }
            if (apa102["clockPins"].is<JsonArrayConst>())
            {
                auto pins = apa102["clockPins"].as<JsonArrayConst>();
                for (size_t i = 0; i < out.requestedRuntimeConfig.outputs.clockPins.size() && i < pins.size(); ++i)
                {
                    if (pins[i].is<int>())
                    {
                        out.requestedRuntimeConfig.outputs.clockPins[i] = pins[i].as<int>();
                        out.runtimeConfigTouched = true;
                    }
                }
            }
        }
    }

    auto [runtimeConfigValid, runtimeConfigMessage] = ValidateRuntimeConfig(out.requestedRuntimeConfig);
    if (!runtimeConfigValid)
        return { false, runtimeConfigMessage };

    if (root["device"].is<JsonObjectConst>())
    {
        auto device = root["device"].as<JsonObjectConst>();

        FieldAccess::AssignIfPresent(device, HostnameTag, out.hostname);
        FieldAccess::AssignIfPresent(device, LocationTag, out.location);
        FieldAccess::AssignIfPresent(device, LocationIsZipTag, out.locationIsZip);
        FieldAccess::AssignIfPresent(device, CountryCodeTag, out.countryCode);
        FieldAccess::AssignIfPresent(device, TimeZoneTag, out.timeZone);
        FieldAccess::AssignIfPresent(device, Use24HourClockTag, out.use24HourClock);
        FieldAccess::AssignIfPresent(device, UseCelsiusTag, out.useCelsius);
        FieldAccess::AssignIfPresent(device, NTPServerTag, out.ntpServer);
        FieldAccess::AssignIfPresent(device, RememberCurrentEffectTag, out.rememberCurrentEffect);

        if (device["remote"].is<JsonObjectConst>())
        {
            auto remote = device["remote"].as<JsonObjectConst>();
            FieldAccess::AssignIfPresent(remote, "resetEffectInterval", out.remoteEffectButtonsResetInterval);
        }

        if (device[OpenWeatherApiKeyTag].is<String>())
        {
            const auto requestedKey = device[OpenWeatherApiKeyTag].as<String>();
            auto [isValid, validationMessage] = ValidateOpenWeatherAPIKey(requestedKey);
            if (!isValid)
                return { false, validationMessage };

            out.openWeatherApiKey = requestedKey;
        }

        if (device[PowerLimitTag].is<int>())
        {
            const int requestedPowerLimit = device[PowerLimitTag].as<int>();
            auto [isValid, validationMessage] = ValidatePowerLimit(requestedPowerLimit);
            if (!isValid)
                return { false, validationMessage };

            out.powerLimit = requestedPowerLimit;
        }

        if (device[BrightnessTag].is<int>())
        {
            const int requestedBrightness = device[BrightnessTag].as<int>();
            auto [isValid, validationMessage] = ValidateBrightness(requestedBrightness);
            if (!isValid)
                return { false, validationMessage };

            out.brightness = requestedBrightness;
        }

        const bool hasTopLevelAudioInputPin = device[DeviceConfig::AudioInputPinTag].is<int>();
        const bool hasNestedAudioInputPin =
            device["audio"].is<JsonObjectConst>()
            && device["audio"].as<JsonObjectConst>()["audioInputPin"].is<int>();

        if (hasTopLevelAudioInputPin || hasNestedAudioInputPin)
        {
            auto [requestedAudioInputPin, audioPinResolveMessage] = ResolveUnifiedAudioInputPin(device);
            if (!requestedAudioInputPin.has_value())
                return { false, audioPinResolveMessage };

            auto [isValid, validationMessage] = ValidateAudioInputPin(requestedAudioInputPin.value());
            if (!isValid)
                return { false, validationMessage };

            out.audioInputPin = requestedAudioInputPin.value();
        }

        FieldAccess::AssignIfPresent(device, GlobalColorTag, out.globalColor);
        FieldAccess::AssignIfPresent(device, SecondColorTag, out.secondColor);
        out.clearGlobalColor = device[ClearGlobalColorTag].is<bool>() && device[ClearGlobalColorTag].as<bool>();
        out.applyGlobalColors = device[ApplyGlobalColorsTag].is<bool>() && device[ApplyGlobalColorsTag].as<bool>();

        if (device["schedule"].is<JsonObjectConst>())
        {
            auto schedule = device["schedule"].as<JsonObjectConst>();

            FieldAccess::AssignIfPresent(schedule, "enabled", out.scheduleEnabled);
            FieldAccess::AssignIfPresent(schedule, "latLongAuto", out.scheduleLatLongAuto);

            if (schedule["dimPercent"].is<int>())
            {
                const int requestedDimPercent = schedule["dimPercent"].as<int>();
                auto [isValid, validationMessage] = ValidateScheduleDimPercent(requestedDimPercent);
                if (!isValid)
                    return { false, validationMessage };

                out.scheduleDimPercent = requestedDimPercent;
            }

            struct { const char* key; std::optional<String>& out; } scheduleTimeFields[] = {
                { "dimTime", out.scheduleDimTime },
                { "offTime", out.scheduleOffTime },
                { "onTime",  out.scheduleOnTime },
            };
            for (auto& field : scheduleTimeFields)
            {
                if (!schedule[field.key].is<String>())
                    continue;

                const String requestedTime = schedule[field.key].as<String>();
                auto [isValid, validationMessage] = ValidateScheduleTime(requestedTime);
                if (!isValid)
                    return { false, validationMessage };

                field.out = requestedTime;
            }

            if (schedule["latitude"].is<float>())
            {
                const float requestedLatitude = schedule["latitude"].as<float>();
                auto [isValid, validationMessage] = ValidateScheduleLatitude(requestedLatitude);
                if (!isValid)
                    return { false, validationMessage };

                out.scheduleLatitude = requestedLatitude;
            }

            if (schedule["longitude"].is<float>())
            {
                const float requestedLongitude = schedule["longitude"].as<float>();
                auto [isValid, validationMessage] = ValidateScheduleLongitude(requestedLongitude);
                if (!isValid)
                    return { false, validationMessage };

                out.scheduleLongitude = requestedLongitude;
            }
        }
    }

    return { true, "" };
}

SuccessResultWithMessage DeviceConfig::ApplyUnifiedDeviceSettings(const UnifiedSettingsRequest& request)
{
    FieldAccess::ApplyIfPresent(request.hostname, *this, &DeviceConfig::SetHostname);
    FieldAccess::ApplyIfPresent(request.location, *this, &DeviceConfig::SetLocation);
    FieldAccess::ApplyIfPresent(request.locationIsZip, *this, &DeviceConfig::SetLocationIsZip);
    FieldAccess::ApplyIfPresent(request.countryCode, *this, &DeviceConfig::SetCountryCode);
    FieldAccess::ApplyIfPresent(request.openWeatherApiKey, *this, &DeviceConfig::SetOpenWeatherAPIKey);
    FieldAccess::ApplyIfPresent(request.timeZone, [this](const String& value) { SetTimeZone(value); });
    FieldAccess::ApplyIfPresent(request.use24HourClock, *this, &DeviceConfig::Set24HourClock);
    FieldAccess::ApplyIfPresent(request.useCelsius, *this, &DeviceConfig::SetUseCelsius);
    FieldAccess::ApplyIfPresent(request.ntpServer, *this, &DeviceConfig::SetNTPServer);
    FieldAccess::ApplyIfPresent(request.rememberCurrentEffect, *this, &DeviceConfig::SetRememberCurrentEffect);
    FieldAccess::ApplyIfPresent(request.remoteEffectButtonsResetInterval, *this, &DeviceConfig::SetRemoteEffectButtonsResetInterval);
    FieldAccess::ApplyIfPresent(request.powerLimit, *this, &DeviceConfig::SetPowerLimit);
    FieldAccess::ApplyIfPresent(request.brightness, *this, &DeviceConfig::SetBrightness);
    FieldAccess::ApplyIfPresent(request.scheduleEnabled, *this, &DeviceConfig::SetScheduleEnabled);
    FieldAccess::ApplyIfPresent(request.scheduleDimPercent, *this, &DeviceConfig::SetScheduleDimPercent);
    FieldAccess::ApplyIfPresent(request.scheduleDimTime, *this, &DeviceConfig::SetScheduleDimTime);
    FieldAccess::ApplyIfPresent(request.scheduleOffTime, *this, &DeviceConfig::SetScheduleOffTime);
    FieldAccess::ApplyIfPresent(request.scheduleOnTime, *this, &DeviceConfig::SetScheduleOnTime);
    FieldAccess::ApplyIfPresent(request.scheduleLatitude, *this, &DeviceConfig::SetScheduleLatitude);
    FieldAccess::ApplyIfPresent(request.scheduleLongitude, *this, &DeviceConfig::SetScheduleLongitude);
    FieldAccess::ApplyIfPresent(request.scheduleLatLongAuto, *this, &DeviceConfig::SetScheduleLatLongAuto);

    // Re-resolve (overwriting any manual lat/long this same request also carried) whenever
    // auto mode is on and something that could change the result was just touched - not on
    // every settings save, since each attempt is a blocking HTTP call.
    const bool scheduleLocationInputsChanged = request.location.has_value() || request.locationIsZip.has_value() ||
                                                request.countryCode.has_value() || request.scheduleLatLongAuto.has_value();
    if (scheduleLatLongAuto && scheduleLocationInputsChanged)
        ResolveScheduleLatLongFromLocation();

    if (request.audioInputPin.has_value())
    {
        const int oldPin = GetAudioInputPin();
        SetAudioInputPin(request.audioInputPin.value());

        const int newPin = GetAudioInputPin();
        if (newPin != oldPin && SupportsLiveAudioInputReconfigure() && g_ptrSystem && g_ptrSystem->HasAudioService())
        {
            auto& audioService = g_ptrSystem->GetAudioService();
            if (!audioService.Reconfigure(AudioConfig::FromCurrentSettings()))
            {
                SetAudioInputPin(oldPin);
                return { false, "Audio input pin live apply failed" };
            }
        }
    }

    ApplyColorSettings(request.globalColor,
                       request.secondColor,
                       request.clearGlobalColor,
                       request.applyGlobalColors);

    return { true, "" };
}
