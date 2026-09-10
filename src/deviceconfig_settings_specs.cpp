//+--------------------------------------------------------------------------
//
// File:        deviceconfig_settings_specs.cpp
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of deviceconfig.cpp; see that file header for additional context.
//
// Split scope: DeviceConfig setting specification/schema definitions.
//---------------------------------------------------------------------------


#include "globals.h"

#include "deviceconfig.h"

const std::vector<std::reference_wrapper<SettingSpec>>& DeviceConfig::GetSettingSpecs()
{
    if (settingSpecs.empty())
    {
        // Build the table in one allocation. On no-PSRAM boards this metadata
        // is created after the HUB75 DMA buffers, so geometric vector growth
        // can temporarily require both old and new contiguous blocks.
        constexpr size_t kFixedSettingSpecCapacity = 37;
        const auto compiledChannelCount = GetCompiledChannelCount();
        // Each compiled channel contributes a per-channel pin spec (two on APA102) plus 7
        // per-channel topology specs (shape, stripLength, matrixWidth, matrixHeight,
        // matrixSerpentine, matrixOrigin, matrixAxis) on non-HUB75 builds.
        const size_t outputSettingSpecCount = compiledChannelCount
        #if USE_APA102
            * 9
        #else
            * 8
        #endif
        ;
        settingSpecs.reserve(kFixedSettingSpecCapacity + outputSettingSpecCount);
        settingSpecReferences.reserve(kFixedSettingSpecCapacity + outputSettingSpecCount);

        // Section keys used below correspond to entries in the section catalog
        // emitted by FillUnifiedSettingsSchemaJson(). Keep them in sync.
        constexpr const char* kSectionSystem     = "system";
        constexpr const char* kSectionLocation   = "location";
        constexpr const char* kSectionClock      = "clock";
        constexpr const char* kSectionAudio      = "audio";
        constexpr const char* kSectionTopology   = "topology";
        constexpr const char* kSectionOutput     = "output";
        constexpr const char* kSectionSchedule   = "schedule";

        // ---- system section ------------------------------------------------
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name           = HostnameTag,
            .FriendlyName   = "Hostname",
            .Description    = "The hostname of the device. A reboot is required after changing this.",
            .Type           = SettingSpec::SettingType::String,
            .EmptyAllowed   = true,
            .Section        = kSectionSystem,
            .RequiresReboot = true,
            .ApiPath        = "device.hostname"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name          = PowerLimitTag,
            .FriendlyName  = "Power limit",
            .Description   = "The maximum power in mW that the LEDs attached to the board are allowed to use.",
            .Type          = SettingSpec::SettingType::Integer,
            .HasValidation = true,
            .MinimumValue  = (double)POWER_LIMIT_MIN,
            .Section       = kSectionSystem,
            .ApiPath       = "device.powerLimit"
        }));

        // ---- location section ----------------------------------------------
        // Explicit Priority on every entry here: specs without one sort alphabetically by
        // friendly name, which would scatter the geography-related fields (location, country,
        // lat/long/auto-detect) out of their natural reading order.
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = LocationTag,
            .FriendlyName = "Location",
            .Description  = "The location (city or postal code) where the device is located.",
            .Type         = SettingSpec::SettingType::String,
            .Section      = kSectionLocation,
            .Priority     = 0,
            .ApiPath      = "device.location"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = LocationIsZipTag,
            .FriendlyName = "Location is postal code",
            .Description  = "Indicates if the value for the \"Location\" setting is a postal code (yes if checked) or not.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Section      = kSectionLocation,
            .Priority     = 1,
            .ApiPath      = "device.locationIsZip"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = CountryCodeTag,
            .FriendlyName = "Country code",
            .Description  = "The <a href=\"https://en.wikipedia.org/wiki/ISO_3166-1_alpha-2\">ISO 3166-1 alpha-2</a> country "
                            "code for the country that the device is located in.",
            .Type         = SettingSpec::SettingType::String,
            .Section      = kSectionLocation,
            .Priority     = 2,
            .ApiPath      = "device.countryCode",
            .Widget       = SettingSpec::WidgetKind::Select,
            .Options      = SettingSpec::OptionsSource::IntlCountryCodes
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = ScheduleLatLongAutoTag,
            .FriendlyName = "Auto-detect from location",
            .Description  = "Automatically resolve latitude/longitude from the Location/Country code settings above (needs an Open Weather API key). "
                            "Turn this off to enter latitude/longitude manually below.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Section      = kSectionLocation,
            .Priority     = 3,
            .ApiPath      = "device.schedule.latLongAuto"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name          = ScheduleLatitudeTag,
            .FriendlyName  = "Latitude",
            .Description   = "Device location latitude in degrees (-90 to 90), used only to compute sunrise/sunset for the nightly schedule. "
                            "Ignored (and overwritten) while \"Auto-detect from location\" is on.",
            .Type          = SettingSpec::SettingType::Float,
            .HasValidation = true,
            .MinimumValue  = -90.0,
            .MaximumValue  = 90.0,
            .Section       = kSectionLocation,
            .Priority      = 4,
            .ApiPath       = "device.schedule.latitude"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name          = ScheduleLongitudeTag,
            .FriendlyName  = "Longitude",
            .Description   = "Device location longitude in degrees (-180 to 180), used only to compute sunrise/sunset for the nightly schedule. "
                            "Ignored (and overwritten) while \"Auto-detect from location\" is on.",
            .Type          = SettingSpec::SettingType::Float,
            .HasValidation = true,
            .MinimumValue  = -180.0,
            .MaximumValue  = 180.0,
            .Section       = kSectionLocation,
            .Priority      = 5,
            .ApiPath       = "device.schedule.longitude"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name               = TimeZoneTag,
            .FriendlyName       = "Time zone",
            .Description        = "The timezone the device resides in, in <a href=\"https://en.wikipedia.org/wiki/Tz_database\">tz database</a> format. "
                                  "The list of available timezone identifiers can be found in the <a href=\"/timezones.json\">timezones.json</a> file.",
            .Type               = SettingSpec::SettingType::String,
            .Section            = kSectionLocation,
            .Priority           = 6,
            .ApiPath            = "device.timeZone",
            .Widget             = SettingSpec::WidgetKind::Select,
            .Options            = SettingSpec::OptionsSource::ExternalTimeZones,
            .OptionsExternalUrl = "/timezones.json"
        }));

        // ---- clock section -------------------------------------------------
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = Use24HourClockTag,
            .FriendlyName = "Use 24 hour clock",
            .Description  = "Indicates if time should be shown in 24-hour format (yes if checked) or 12-hour AM/PM format.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Section      = kSectionClock,
            .ApiPath      = "device.use24HourClock"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = UseCelsiusTag,
            .FriendlyName = "Use degrees Celsius",
            .Description  = "Indicates if temperatures should be shown in degrees Celsius (yes if checked) or degrees Fahrenheit.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Section      = kSectionClock,
            .ApiPath      = "device.useCelsius"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = NTPServerTag,
            .FriendlyName = "NTP server address",
            .Description  = "The hostname or IP address of the NTP server to be used for time synchronization.",
            .Type         = SettingSpec::SettingType::String,
            .Section      = kSectionClock,
            .ApiPath      = "device.ntpServer"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name          = OpenWeatherApiKeyTag,
            .FriendlyName  = "Open Weather API key",
            .Description   = "The API key for the <a href=\"https://openweathermap.org/api\">Weather API provided by Open Weather Map</a>. "
                            "The key itself is never sent back to the browser once saved; the field shows a masked placeholder "
                            "instead of going blank when one is already configured. Leave it blank to keep the current key.",
            .Type          = SettingSpec::SettingType::String,
            .HasValidation = true,
            .Access        = SettingSpec::SettingAccess::WriteOnly,
            .Section       = kSectionClock,
            .ApiPath       = "device.openWeatherApiKey",
            .Widget        = SettingSpec::WidgetKind::Secret
        }));

        // ---- audio section -------------------------------------------------
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name           = AudioInputPinTag,
            .FriendlyName   = "Audio input pin",
            .Description    = "External microphone input pin. This is boot-applied today because the audio task still owns the active DMA/I2S handles once sampling starts.",
            .Type           = SettingSpec::SettingType::Integer,
            .HasValidation  = true,
            .MinimumValue   = -1.0,
            .MaximumValue   = 48.0,
            .Section        = kSectionAudio,
            .RequiresReboot = !SupportsLiveAudioInputReconfigure(),
            .ApiPath        = "device.audioInputPin"
        }));

        // ---- appearance section --------------------------------------------
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name          = BrightnessTag,
            .FriendlyName  = "Brightness",
            .Description   = "Overall brightness the connected LEDs or matrix should be run at.",
            .Type          = SettingSpec::SettingType::Integer,
            .HasValidation = true,
            .Section       = kSectionAppearance,
            .ApiPath       = "device.brightness",
            .Widget        = SettingSpec::WidgetKind::Slider,
            .DisplayRawMin = (double)BRIGHTNESS_MIN,
            .DisplayRawMax = (double)BRIGHTNESS_MAX,
            .DisplayMin    = 5.0,
            .DisplayMax    = 100.0,
            .DisplaySuffix = "%"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = GlobalColorTag,
            .FriendlyName = "Global color",
            .Description  = "Main color that is applied to all those effects that support using it.",
            .Type         = SettingSpec::SettingType::Color,
            .Section      = kSectionAppearance,
            .ApiPath      = "device.globalColor"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = SecondColorTag,
            .FriendlyName = "Second color",
            .Description  = "Second color that is used to create a global palette in combination with the current global color. That palette is used "
                            "by some effects. Defaults to the <em>previous</em> global color if not explicitly set.",
            .Type         = SettingSpec::SettingType::Color,
            .Section      = kSectionAppearance,
            .ApiPath      = "device.secondColor"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = ApplyGlobalColorsTag,
            .FriendlyName = "(Re)apply global color",
            .Description  = "You can use this to \"reselect\" and apply the current global color, to force the composition of the derived "
                            "global palette. This checkbox is ignored if the \"Clear global color\" checkbox is selected.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Access       = SettingSpec::SettingAccess::WriteOnly,
            .Section      = kSectionAppearance,
            .ApiPath      = "device.applyGlobalColors"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = ClearGlobalColorTag,
            .FriendlyName = "Clear global color",
            .Description  = "Stop applying the global color/derived palette. This takes precedence over the \"(Re)apply global color\" checkbox.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Access       = SettingSpec::SettingAccess::WriteOnly,
            .Section      = kSectionAppearance,
            .ApiPath      = "device.clearGlobalColor"
        }));

        #if SHOW_VU_METER
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = ShowVUMeterTag,
            .FriendlyName = "Show VU meter",
            .Description  = "Used to show (checked) or hide the VU meter at the top of the matrix.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Section      = kSectionAppearance
        }));
        #endif

        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = RememberCurrentEffectTag,
            .FriendlyName = "Remember current effect",
            .Description  = "Indicates if the current effect index should be saved after an effect transition, so the device resumes "
                            "from the same effect when restarted. Enabling this will lead to more wear on the flash chip of your device.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Section      = kSectionAppearance,
            .ApiPath      = "device.rememberCurrentEffect"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = RemoteEffectButtonsResetIntervalTag,
            .FriendlyName = "Remote effect buttons reset interval",
            .Description  = "When enabled, remote B+/B- effect changes force the effect interval back to the default rotation speed (60 seconds). "
                            "Disable this to keep your configured interval, including 0 (no timeout), when changing effects with the remote.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Section      = kSectionAppearance,
            .ApiPath      = "device.remote.resetEffectInterval"
        }));

        // ---- schedule section ------------------------------------------------
        // Explicit Priority throughout, in the order these are expected to be changed most
        // to least often: enable, the three times, the dim percentage, then the read-only
        // result of the (now Location-panel-owned) lat/long auto-detect, kept here since
        // it's the sunrise/sunset times above that actually depend on it being accurate.
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = ScheduleEnabledTag,
            .FriendlyName = "Enable nightly schedule",
            .Description  = "Automatically dim and turn the display off overnight, and back on in the morning.",
            .Type         = SettingSpec::SettingType::Boolean,
            .Section      = kSectionSchedule,
            .Priority     = 0,
            .ApiPath      = "device.schedule.enabled"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name          = ScheduleDimTimeTag,
            .FriendlyName  = "Dim time",
            .Description   = "When the display starts dimming. Pick a clock time (15-minute steps) or a sun-relative event.",
            .Type          = SettingSpec::SettingType::String,
            .HasValidation = true,
            .Section       = kSectionSchedule,
            .Priority      = 1,
            .ApiPath       = "device.schedule.dimTime",
            .Widget        = SettingSpec::WidgetKind::TimeSchedule,
            .OptionValues  = {"sunrise", "sunset", "noon", "midnight"},
            .OptionLabels  = {"Sunrise", "Sunset", "Noon", "Midnight"}
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name          = ScheduleOffTimeTag,
            .FriendlyName  = "Off time",
            .Description   = "When the display turns fully off. Pick a clock time (15-minute steps) or a sun-relative event.",
            .Type          = SettingSpec::SettingType::String,
            .HasValidation = true,
            .Section       = kSectionSchedule,
            .Priority      = 2,
            .ApiPath       = "device.schedule.offTime",
            .Widget        = SettingSpec::WidgetKind::TimeSchedule,
            .OptionValues  = {"sunrise", "sunset", "noon", "midnight"},
            .OptionLabels  = {"Sunrise", "Sunset", "Noon", "Midnight"}
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name          = ScheduleOnTimeTag,
            .FriendlyName  = "On time",
            .Description   = "When the display returns to normal brightness. Pick a clock time (15-minute steps) or a sun-relative event.",
            .Type          = SettingSpec::SettingType::String,
            .HasValidation = true,
            .Section       = kSectionSchedule,
            .Priority      = 3,
            .ApiPath       = "device.schedule.onTime",
            .Widget        = SettingSpec::WidgetKind::TimeSchedule,
            .OptionValues  = {"sunrise", "sunset", "noon", "midnight"},
            .OptionLabels  = {"Sunrise", "Sunset", "Noon", "Midnight"}
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name          = ScheduleDimPercentTag,
            .FriendlyName  = "Dim to",
            .Description   = "Brightness percentage to dim to during the dim window, before going fully off.",
            .Type          = SettingSpec::SettingType::Integer,
            .HasValidation = true,
            .MinimumValue  = 0.0,
            .MaximumValue  = 100.0,
            .Section       = kSectionSchedule,
            .Priority      = 4,
            .ApiPath       = "device.schedule.dimPercent",
            .Widget        = SettingSpec::WidgetKind::Slider,
            .DisplayRawMin = 0.0,
            .DisplayRawMax = 100.0,
            .DisplayMin    = 0.0,
            .DisplayMax    = 100.0,
            .DisplaySuffix = "%"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name         = "scheduleLatLongStatus",
            .FriendlyName = "Auto-detect result",
            .Description  = "Result of the most recent latitude/longitude auto-detect attempt (blank until one has run; see the "
                            "Location panel). A failure here doesn't block saving other settings, but it does mean sunrise/sunset "
                            "times above may be inaccurate.",
            .Type         = SettingSpec::SettingType::String,
            .Access       = SettingSpec::SettingAccess::ReadOnly,
            .Section      = kSectionSchedule,
            .Priority     = 5,
            .ApiPath      = "device.schedule.latLongStatus"
        }));

        // ---- topology section ----------------------------------------------
        // Each output channel independently is a plain strip or its own matrix - one spec set
        // per compiled channel, always generated (even for inactive channels) so the UI can
        // render every channel's row; the web form flags shape-specific fields as conditional
        // on that channel's own shape select and on the channel being below the active count.
        // HUB75 is always a single compile-time-fixed matrix and never exposes any of this.
        if (!IsHub75Build())
        {
            static constexpr const char* kShapeValues[] = { "strip", "matrix" };
            static constexpr const char* kShapeLabels[] = { "Strip", "Matrix" };
            static constexpr const char* kOriginValues[] = { "topLeft", "topRight", "bottomLeft", "bottomRight" };
            static constexpr const char* kOriginLabels[] = { "Top-left", "Top-right", "Bottom-left", "Bottom-right" };
            static constexpr const char* kAxisValues[] = { "horizontal", "vertical" };
            static constexpr const char* kAxisLabels[] = { "Horizontal (each strip is a row)", "Vertical (each strip is a column)" };

            // 7 fields per channel (shape, stripLength, matrixWidth, matrixHeight,
            // matrixSerpentine, matrixOrigin, matrixAxis), 4 strings per field (name,
            // friendlyName, description, apiPath). See the class-level comment on
            // _channelTopologyStrings (deviceconfig.h) for why these live in a pre-sized
            // member vector rather than locals - the SettingSpec objects keep raw c_str()
            // pointers into it for the life of the device, so it must never reallocate
            // mid-loop.
            constexpr size_t kFieldsPerChannel = 7;
            constexpr size_t kStringsPerField = 4;
            constexpr size_t kStringsPerChannel = kFieldsPerChannel * kStringsPerField;

            if (_channelTopologyStrings.size() < static_cast<size_t>(compiledChannelCount) * kStringsPerChannel)
                _channelTopologyStrings.resize(compiledChannelCount * kStringsPerChannel);

            auto fieldSlot = [this, kStringsPerChannel, kStringsPerField](size_t channel, size_t field, size_t str) -> std::string&
            {
                return _channelTopologyStrings[channel * kStringsPerChannel + field * kStringsPerField + str];
            };

            for (size_t i = 0; i < compiledChannelCount; ++i)
            {
                char buf[220];

                snprintf(buf, sizeof(buf), "channel%zuShape", i); fieldSlot(i, 0, 0) = buf;
                snprintf(buf, sizeof(buf), "Channel %zu shape", i + 1); fieldSlot(i, 0, 1) = buf;
                snprintf(buf, sizeof(buf), "Whether channel %zu is a plain strip or its own matrix.", i + 1); fieldSlot(i, 0, 2) = buf;
                snprintf(buf, sizeof(buf), "topology.channels[%zu].shape", i); fieldSlot(i, 0, 3) = buf;
                settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                    .Name         = fieldSlot(i, 0, 0).c_str(),
                    .FriendlyName = fieldSlot(i, 0, 1).c_str(),
                    .Description  = fieldSlot(i, 0, 2).c_str(),
                    .Type         = SettingSpec::SettingType::String,
                    .Section      = kSectionTopology,
                    .Priority     = static_cast<int>(i * kFieldsPerChannel + 0),
                    .ApiPath      = fieldSlot(i, 0, 3).c_str(),
                    .Widget       = SettingSpec::WidgetKind::Select,
                    .OptionValues = { kShapeValues[0], kShapeValues[1] },
                    .OptionLabels = { kShapeLabels[0], kShapeLabels[1] }
                }));

                snprintf(buf, sizeof(buf), "channel%zuStripLength", i); fieldSlot(i, 1, 0) = buf;
                snprintf(buf, sizeof(buf), "Channel %zu strip length", i + 1); fieldSlot(i, 1, 1) = buf;
                snprintf(buf, sizeof(buf), "Number of LEDs on channel %zu. Used when this channel's shape is Strip.", i + 1); fieldSlot(i, 1, 2) = buf;
                snprintf(buf, sizeof(buf), "topology.channels[%zu].stripLength", i); fieldSlot(i, 1, 3) = buf;
                settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                    .Name         = fieldSlot(i, 1, 0).c_str(),
                    .FriendlyName = fieldSlot(i, 1, 1).c_str(),
                    .Description  = fieldSlot(i, 1, 2).c_str(),
                    .Type         = SettingSpec::SettingType::PositiveBigInteger,
                    .MinimumValue = 1.0,
                    .MaximumValue = (double)GetCompiledLEDCount(),
                    .Section      = kSectionTopology,
                    .Priority     = static_cast<int>(i * kFieldsPerChannel + 1),
                    .ApiPath      = fieldSlot(i, 1, 3).c_str()
                }));

                snprintf(buf, sizeof(buf), "channel%zuMatrixWidth", i); fieldSlot(i, 2, 0) = buf;
                snprintf(buf, sizeof(buf), "Channel %zu matrix width", i + 1); fieldSlot(i, 2, 1) = buf;
                snprintf(buf, sizeof(buf), "Matrix width for channel %zu. Used when this channel's shape is Matrix; "
                                            "width * height must stay within the compiled per-channel LED budget.", i + 1); fieldSlot(i, 2, 2) = buf;
                snprintf(buf, sizeof(buf), "topology.channels[%zu].matrixWidth", i); fieldSlot(i, 2, 3) = buf;
                settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                    .Name         = fieldSlot(i, 2, 0).c_str(),
                    .FriendlyName = fieldSlot(i, 2, 1).c_str(),
                    .Description  = fieldSlot(i, 2, 2).c_str(),
                    .Type         = SettingSpec::SettingType::PositiveBigInteger,
                    .MinimumValue = 1.0,
                    .MaximumValue = (double)GetCompiledLEDCount(),
                    .Section      = kSectionTopology,
                    .Priority     = static_cast<int>(i * kFieldsPerChannel + 2),
                    .ApiPath      = fieldSlot(i, 2, 3).c_str()
                }));

                snprintf(buf, sizeof(buf), "channel%zuMatrixHeight", i); fieldSlot(i, 3, 0) = buf;
                snprintf(buf, sizeof(buf), "Channel %zu matrix height", i + 1); fieldSlot(i, 3, 1) = buf;
                snprintf(buf, sizeof(buf), "Matrix height for channel %zu. Used when this channel's shape is Matrix; "
                                            "width * height must stay within the compiled per-channel LED budget.", i + 1); fieldSlot(i, 3, 2) = buf;
                snprintf(buf, sizeof(buf), "topology.channels[%zu].matrixHeight", i); fieldSlot(i, 3, 3) = buf;
                settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                    .Name         = fieldSlot(i, 3, 0).c_str(),
                    .FriendlyName = fieldSlot(i, 3, 1).c_str(),
                    .Description  = fieldSlot(i, 3, 2).c_str(),
                    .Type         = SettingSpec::SettingType::PositiveBigInteger,
                    .MinimumValue = 1.0,
                    .MaximumValue = (double)GetCompiledLEDCount(),
                    .Section      = kSectionTopology,
                    .Priority     = static_cast<int>(i * kFieldsPerChannel + 3),
                    .ApiPath      = fieldSlot(i, 3, 3).c_str()
                }));

                snprintf(buf, sizeof(buf), "channel%zuMatrixSerpentine", i); fieldSlot(i, 4, 0) = buf;
                snprintf(buf, sizeof(buf), "Channel %zu serpentine", i + 1); fieldSlot(i, 4, 1) = buf;
                snprintf(buf, sizeof(buf), "Whether channel %zu's matrix wiring zigzags. Used when this channel's shape is Matrix.", i + 1); fieldSlot(i, 4, 2) = buf;
                snprintf(buf, sizeof(buf), "topology.channels[%zu].matrixSerpentine", i); fieldSlot(i, 4, 3) = buf;
                settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                    .Name         = fieldSlot(i, 4, 0).c_str(),
                    .FriendlyName = fieldSlot(i, 4, 1).c_str(),
                    .Description  = fieldSlot(i, 4, 2).c_str(),
                    .Type         = SettingSpec::SettingType::Boolean,
                    .Section      = kSectionTopology,
                    .Priority     = static_cast<int>(i * kFieldsPerChannel + 4),
                    .ApiPath      = fieldSlot(i, 4, 3).c_str()
                }));

                snprintf(buf, sizeof(buf), "channel%zuMatrixOrigin", i); fieldSlot(i, 5, 0) = buf;
                snprintf(buf, sizeof(buf), "Channel %zu matrix origin", i + 1); fieldSlot(i, 5, 1) = buf;
                snprintf(buf, sizeof(buf), "Which corner of channel %zu's matrix LED 0 is wired to. Used when this channel's shape is Matrix.", i + 1); fieldSlot(i, 5, 2) = buf;
                snprintf(buf, sizeof(buf), "topology.channels[%zu].matrixOrigin", i); fieldSlot(i, 5, 3) = buf;
                settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                    .Name         = fieldSlot(i, 5, 0).c_str(),
                    .FriendlyName = fieldSlot(i, 5, 1).c_str(),
                    .Description  = fieldSlot(i, 5, 2).c_str(),
                    .Type         = SettingSpec::SettingType::String,
                    .Section      = kSectionTopology,
                    .Priority     = static_cast<int>(i * kFieldsPerChannel + 5),
                    .ApiPath      = fieldSlot(i, 5, 3).c_str(),
                    .Widget       = SettingSpec::WidgetKind::Select,
                    .OptionValues = { kOriginValues[0], kOriginValues[1], kOriginValues[2], kOriginValues[3] },
                    .OptionLabels = { kOriginLabels[0], kOriginLabels[1], kOriginLabels[2], kOriginLabels[3] }
                }));

                snprintf(buf, sizeof(buf), "channel%zuMatrixAxis", i); fieldSlot(i, 6, 0) = buf;
                snprintf(buf, sizeof(buf), "Channel %zu serpentine axis", i + 1); fieldSlot(i, 6, 1) = buf;
                snprintf(buf, sizeof(buf), "Which axis channel %zu's serpentine zigzag runs along. Used when this channel's shape is Matrix.", i + 1); fieldSlot(i, 6, 2) = buf;
                snprintf(buf, sizeof(buf), "topology.channels[%zu].matrixAxis", i); fieldSlot(i, 6, 3) = buf;
                settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                    .Name         = fieldSlot(i, 6, 0).c_str(),
                    .FriendlyName = fieldSlot(i, 6, 1).c_str(),
                    .Description  = fieldSlot(i, 6, 2).c_str(),
                    .Type         = SettingSpec::SettingType::String,
                    .Section      = kSectionTopology,
                    .Priority     = static_cast<int>(i * kFieldsPerChannel + 6),
                    .ApiPath      = fieldSlot(i, 6, 3).c_str(),
                    .Widget       = SettingSpec::WidgetKind::Select,
                    .OptionValues = { kAxisValues[0], kAxisValues[1] },
                    .OptionLabels = { kAxisLabels[0], kAxisLabels[1] }
                }));
            }
        }

        // ---- output section -------------------------------------------------
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name              = OutputDriverTag,
            .FriendlyName      = "Output driver",
            .Description       = "Runtime-selected driver. If this differs from the firmware's compiled driver, the API reports recompile required.",
            .Type              = SettingSpec::SettingType::String,
            .Section           = kSectionOutput,
            .Priority          = 0,
            .ApiPath           = "outputs.driver",
            .Widget            = SettingSpec::WidgetKind::Select,
            .Options           = SettingSpec::OptionsSource::SchemaPath,
            .OptionValues      = {"ws281x", "apa102", "hub75"},
            .OptionLabels      = {"WS281x", "APA102", "HUB75"},
            .OptionsSchemaPath = "outputs.allowedDrivers"
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name              = WS281xChannelCountTag,
            .FriendlyName      = "Strip channel count",
            .Description       = "Number of active strip channels within the compiled maximum.",
            .Type              = SettingSpec::SettingType::PositiveBigInteger,
            .MinimumValue      = 1.0,
            .MaximumValue      = (double)GetCompiledChannelCount(),
            .Section           = kSectionOutput,
            .Priority          = 1,
            .ApiPath           =
            #if USE_APA102
                "outputs.apa102.channelCount",
            #else
                "outputs.ws281x.channelCount",
            #endif
            .Widget            = SettingSpec::WidgetKind::Select,
            .Options           = SettingSpec::OptionsSource::SchemaPath,
            .OptionsSchemaPath =
            #if USE_APA102
                "outputs.apa102.allowedChannelCounts"
            #else
                "outputs.ws281x.allowedChannelCounts"
            #endif
        }));
        settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
            .Name              = WS281xColorOrderTag,
            .FriendlyName      = "Strip color order",
            .Description       = "Byte order used when streaming RGB values to the strip. This applies live on strip builds and is ignored on HUB75 builds.",
            .Type              = SettingSpec::SettingType::String,
            .Section           = kSectionOutput,
            .Priority          = 2,
            .ApiPath           =
            #if USE_APA102
                "outputs.apa102.colorOrder",
            #else
                "outputs.ws281x.colorOrder",
            #endif
            .Widget            = SettingSpec::WidgetKind::Select,
            .Options           = SettingSpec::OptionsSource::SchemaPath,
            .OptionsSchemaPath =
            #if USE_APA102
                "outputs.apa102.allowedColorOrders"
            #else
                "outputs.ws281x.allowedColorOrders"
            #endif
        }));

        // WS281x/APA102 pin settings are meaningless on HUB75 builds - the matrix uses a
        // fixed set of parallel data lines, not per-channel GPIO pins - so skip generating
        // them entirely rather than relying on the UI to hide them. Matches how the topology
        // section above skips itself on HUB75 builds.
        if (!IsHub75Build())
        {
            // pinSpecStrings backs the const char* pointers stored in each SettingSpec below.
            // Both the per-channel pin loop and the per-channel strip-length loop below push
            // strings onto this vector and hold onto c_str() pointers from those strings. If the
            // vector reallocates between those emplace_back calls the earlier c_str() pointers
            // dangle and the spec JSON ends up serializing whatever the freed memory contains
            // (in practice, control characters that JSON.parse then rejects). Budget room for
            // both loops upfront so reallocation never happens mid-iteration.
            constexpr size_t kPinStringsPerChannel =
            #if USE_APA102
                12; // 8 for data+clock pin spec, 4 for strip-length spec
            #else
                8;  // 4 for pin spec, 4 for strip-length spec
            #endif
            pinSpecStrings.reserve(compiledChannelCount * kPinStringsPerChannel);
            const auto stableCStr = [](const String& s) { return s.c_str(); };

            for (size_t i = 0; i < compiledChannelCount; ++i)
            {
                const auto& nameStr        = pinSpecStrings.emplace_back(str_sprintf("stripDataPin%zu", i));
                const auto& friendlyStr    = pinSpecStrings.emplace_back(
                    #if USE_APA102
                        str_sprintf("APA102 data pin %zu", i + 1)
                    #else
                        str_sprintf("WS281x pin %zu", i + 1)
                    #endif
                );
                const auto& descriptionStr = pinSpecStrings.emplace_back(
                    #if USE_APA102
                        str_sprintf("GPIO assigned to APA102 data for channel %zu.", i + 1)
                    #else
                        str_sprintf("GPIO assigned to WS281x channel %zu.", i + 1)
                    #endif
                );
                const auto& apiPathStr     = pinSpecStrings.emplace_back(
                    #if USE_APA102
                        str_sprintf("outputs.apa102.dataPins[%zu]", i)
                    #else
                        str_sprintf("outputs.ws281x.pins[%zu]", i)
                    #endif
                );

                settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                    .Name         = stableCStr(nameStr),
                    .FriendlyName = stableCStr(friendlyStr),
                    .Description  = stableCStr(descriptionStr),
                    .Type         = SettingSpec::SettingType::Integer,
                    .MinimumValue = -1.0,
                    .MaximumValue = 48.0,
                    .Section      = kSectionOutput,
                    .Priority     = 3 + static_cast<int>(i),
                    .ApiPath      = stableCStr(apiPathStr)
                }));

                #if USE_APA102
                const auto& clockNameStr        = pinSpecStrings.emplace_back(str_sprintf("apa102ClockPin%zu", i));
                const auto& clockFriendlyStr    = pinSpecStrings.emplace_back(str_sprintf("APA102 clock pin %zu", i + 1));
                const auto& clockDescriptionStr = pinSpecStrings.emplace_back(str_sprintf("GPIO assigned to APA102 clock for channel %zu.", i + 1));
                const auto& clockApiPathStr     = pinSpecStrings.emplace_back(str_sprintf("outputs.apa102.clockPins[%zu]", i));

                settingSpecs.push_back(SettingSpec::Validate(SettingSpec{
                    .Name         = stableCStr(clockNameStr),
                    .FriendlyName = stableCStr(clockFriendlyStr),
                    .Description  = stableCStr(clockDescriptionStr),
                    .Type         = SettingSpec::SettingType::Integer,
                    .MinimumValue = -1.0,
                    .MaximumValue = 48.0,
                    .Section      = kSectionOutput,
                    .Priority     = 3 + static_cast<int>(compiledChannelCount) + static_cast<int>(i),
                    .ApiPath      = stableCStr(clockApiPathStr)
                }));
                #endif
            }
        }

        settingSpecReferences.insert(settingSpecReferences.end(), settingSpecs.begin(), settingSpecs.end());
    }

    return settingSpecReferences;
}
