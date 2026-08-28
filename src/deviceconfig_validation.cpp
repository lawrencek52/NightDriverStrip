//+--------------------------------------------------------------------------
//
// File:        deviceconfig_validation.cpp
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of deviceconfig.cpp; see that file header for additional context.
//
// Split scope: DeviceConfig value validation routines.
//---------------------------------------------------------------------------


#include "globals.h"

#include "deviceconfig.h"
#include "deviceconfig_internal.h"

SuccessResultWithMessage DeviceConfig::ValidateTopology(uint16_t width, uint16_t height, bool serpentine) const
{
    if (width == 0 || height == 0)
        return { false, "matrix dimensions must be greater than zero" };

    const size_t requestedLEDCount = static_cast<size_t>(width) * height;
    const size_t compiledLEDCount = GetCompiledLEDCount();
    if (requestedLEDCount > compiledLEDCount)
    {
        return {
            false,
            String("Matrix dimensions ") + width + " x " + height
                + " require " + static_cast<unsigned long>(requestedLEDCount)
                + " LEDs, but this firmware was compiled for "
                + static_cast<unsigned long>(compiledLEDCount)
                + ". Lower width/height or flash a build compiled for more LEDs."
        };
    }

    if (IsHub75Build())
    {
        if (width != GetCompiledMatrixWidth() || height != GetCompiledMatrixHeight())
            return { false, DeviceConfigInternal::RecompileNeededMessage() };

        if (serpentine != GetCompiledMatrixSerpentine())
            return { false, DeviceConfigInternal::RecompileNeededMessage() };
    }

    return { true, "" };
}

SuccessResultWithMessage DeviceConfig::ValidateStripLengths(const std::array<uint16_t, NUM_CHANNELS>& lengths,
                                                            size_t channelCount) const
{
    // HUB75 doesn't run per-strip layouts at all; the caller should not reach this branch, but
    // defend against it so an accidentally-misconfigured build doesn't try to allocate a layout
    // the panel can't display.
    if (IsHub75Build())
        return { false, DeviceConfigInternal::RecompileNeededMessage() };

    if (channelCount == 0)
        return { false, "channel count must be greater than zero" };

    if (channelCount > GetCompiledChannelCount())
        return { false, DeviceConfigInternal::RecompileNeededMessage() };

    // Each channel gets its own PSRAM GFX buffer and its own DMA byte buffer, so the limit is
    // PER STRIP rather than a shared total. The compile-time NUM_LEDS is the per-strip default
    // and a reasonable upper bound (the uint16_t field can technically hold 65535, but anything
    // much beyond NUM_LEDS would chew through memory without a recompile to bump the buffer).
    const uint16_t perStripMax = GetCompiledLEDCount();
    for (size_t i = 0; i < channelCount; ++i)
    {
        if (lengths[i] == 0)
            return { false, String("Strip ") + (i + 1) + " must have at least one LED" };

        if (lengths[i] > perStripMax)
        {
            return {
                false,
                String("Strip ") + (i + 1) + " has " + lengths[i]
                    + " LEDs, but this firmware was compiled for a maximum of "
                    + static_cast<unsigned long>(perStripMax)
                    + " LEDs per strip. Lower this strip or flash a build compiled for more LEDs per strip."
            };
        }
    }

    return { true, "" };
}

SuccessResultWithMessage DeviceConfig::ValidateOutputDriver(OutputDriver driver) const
{
    if (driver != GetCompiledOutputDriver())
        return { false, DeviceConfigInternal::RecompileNeededMessage() };

    return { true, "" };
}

SuccessResultWithMessage DeviceConfig::ValidateStripSettings(size_t channelCount,
                                                             const std::array<int8_t, NUM_CHANNELS>& dataPins,
                                                             const std::array<int8_t, NUM_CHANNELS>& clockPins,
                                                             WS281xColorOrder colorOrder) const
{
    if (channelCount == 0)
        return { false, "channel count must be greater than zero" };

    if (channelCount > GetCompiledChannelCount())
        return { false, DeviceConfigInternal::RecompileNeededMessage() };

    if (IsHub75Build())
    {
        if (channelCount != GetCompiledChannelCount())
            return { false, DeviceConfigInternal::RecompileNeededMessage() };

        if (dataPins != GetCompiledWS281xPins())
            return { false, DeviceConfigInternal::RecompileNeededMessage() };

        if (clockPins != GetCompiledAPA102ClockPins())
            return { false, DeviceConfigInternal::RecompileNeededMessage() };

        if (colorOrder != GetCompiledWS281xColorOrder())
            return { false, DeviceConfigInternal::RecompileNeededMessage() };

        return { true, "" };
    }

    for (size_t i = 0; i < channelCount; ++i)
    {
        if (dataPins[i] < 0)
            return { false, "active channels require valid GPIO pins" };

        if (!GPIO_IS_VALID_OUTPUT_GPIO(static_cast<gpio_num_t>(dataPins[i])))
            return { false, "strip data pins must be valid output GPIOs" };

        for (size_t j = i + 1; j < channelCount; ++j)
        {
            if (dataPins[i] == dataPins[j])
                return { false, "strip data pins must be unique" };
        }
    }

    if (GetCompiledOutputDriver() == OutputDriver::APA102)
    {
        for (size_t i = 0; i < channelCount; ++i)
        {
            if (clockPins[i] < 0)
                return { false, "APA102 channels require valid clock GPIO pins" };

            if (!GPIO_IS_VALID_OUTPUT_GPIO(static_cast<gpio_num_t>(clockPins[i])))
                return { false, "APA102 clock pins must be valid output GPIOs" };

            if (clockPins[i] == dataPins[i])
                return { false, "APA102 data and clock pins must be different" };

            for (size_t j = i + 1; j < channelCount; ++j)
            {
                if (clockPins[i] == clockPins[j])
                    return { false, "APA102 clock pins must be unique" };

                if (clockPins[i] == dataPins[j] || dataPins[i] == clockPins[j])
                    return { false, "APA102 data and clock pins must be unique" };
            }
        }
    }

    return { true, "" };
}

SuccessResultWithMessage DeviceConfig::ValidateRuntimeConfig(const RuntimeConfig& config) const
{
    auto [driverValid, driverMessage] = ValidateOutputDriver(config.outputs.driver);
    if (!driverValid)
        return { false, driverMessage };

    // Strip-lengths only matter for the individual-strip layout. The matrix path keeps validating
    // the existing width/height/serpentine triple so HUB75 builds (which are always Matrix) are
    // unaffected.
    if (config.topology.layout == LayoutType::IndividualStrips && !IsHub75Build())
    {
        auto [lengthsValid, lengthsMessage] = ValidateStripLengths(config.topology.stripLengths, config.outputs.channelCount);
        if (!lengthsValid)
            return { false, lengthsMessage };
    }
    else
    {
        auto [topologyValid, topologyMessage] = ValidateTopology(config.topology.width, config.topology.height, config.topology.serpentine);
        if (!topologyValid)
            return { false, topologyMessage };
    }

    auto [stripValid, stripMessage] = ValidateStripSettings(config.outputs.channelCount,
                                                            config.outputs.outputPins,
                                                            config.outputs.clockPins,
                                                            config.outputs.colorOrder);
    if (!stripValid)
        return { false, stripMessage };

    return { true, "" };
}
