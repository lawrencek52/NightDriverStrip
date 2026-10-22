//+--------------------------------------------------------------------------
//
// File:        webserver_crash.cpp
//
// NightDriverStrip - (c) 2026 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of webserver.cpp; see that file header for additional context.
//
// Split scope: crash report handlers, so a panic on a controller that isn't
// plugged into USB can still be diagnosed over WiFi.
//
//   GET  /api/v1/crash         last reset reason, plus a summary of the core dump
//                              saved to the coredump partition by the last panic:
//                              task, PC, backtrace (Xtensa) or registers and a raw
//                              stack dump (RISC-V, which can't unwind on device)
//   GET  /api/v1/crash/raw     the whole core dump image, for
//                              `esp-coredump info_corefile -c <file> -t raw <elf>`
//   POST /api/v1/crash/clear   erase the stored core dump
//   POST /api/v1/crash/test    confirm=yes: deliberately panic, to test all of the above
//
// Decode a summary's addresses with the matching firmware.elf (check elfSha256
// against `sha256sum firmware.elf`), e.g. xtensa-esp32s3-elf-addr2line -pfiaC -e
// firmware.elf <pc> <backtrace...>.
//---------------------------------------------------------------------------

#include "globals.h"

#if ENABLE_WEBSERVER

#include "webserver.h"

#include <AsyncJson.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <optional>

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    #include <esp_core_dump.h>
    #define ND_HAS_CORE_DUMP 1
#else
    #define ND_HAS_CORE_DUMP 0
#endif

namespace
{
    const char* ResetReasonName(esp_reset_reason_t reason)
    {
        switch (reason)
        {
            case ESP_RST_POWERON:   return "power-on";
            case ESP_RST_EXT:       return "external pin";
            case ESP_RST_SW:        return "software (esp_restart)";
            case ESP_RST_PANIC:     return "panic";
            case ESP_RST_INT_WDT:   return "interrupt watchdog";
            case ESP_RST_TASK_WDT:  return "task watchdog";
            case ESP_RST_WDT:       return "other watchdog";
            case ESP_RST_DEEPSLEEP: return "deep sleep wake";
            case ESP_RST_BROWNOUT:  return "brownout";
            case ESP_RST_SDIO:      return "SDIO";
            default:                return "unknown";
        }
    }

    String Hex32(uint32_t value)
    {
        char text[11];
        snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(value));
        return text;
    }

    bool HasCoreDumpPartition()
    {
        return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr) != nullptr;
    }

    // Whether a valid core dump is stored. Checking it reads and CRCs the whole image, so
    // it's done once per boot (a dump is only ever written by a panic, which reboots) and
    // again only after a clear - not on every /statistics poll.
    std::optional<bool> s_crashPresent;
}

bool CWebServer::IsCrashDumpPresent()
{
    #if ND_HAS_CORE_DUMP
        if (!s_crashPresent.has_value())
            s_crashPresent = HasCoreDumpPartition() && esp_core_dump_image_check() == ESP_OK;
        return *s_crashPresent;
    #else
        return false;
    #endif
}

void CWebServer::GetCrashInfo(AsyncWebServerRequest * pRequest)
{
    auto response = new AsyncJsonResponse();
    auto root = response->getRoot().to<JsonObject>();

    const auto reason = esp_reset_reason();
    root["resetReason"] = static_cast<int>(reason);
    root["resetReasonName"] = ResetReasonName(reason);
    root["coreDumpPartition"] = HasCoreDumpPartition();
    root["present"] = IsCrashDumpPresent();

    #if ND_HAS_CORE_DUMP
        size_t address = 0, size = 0;
        if (IsCrashDumpPresent() && esp_core_dump_image_get(&address, &size) == ESP_OK)
            root["size"] = size;

        auto summary = make_unique_psram<esp_core_dump_summary_t>();
        if (IsCrashDumpPresent() && esp_core_dump_get_summary(summary.get()) == ESP_OK)
        {
            auto s = root["summary"].to<JsonObject>();
            s["task"] = String(summary->exc_task).substring(0, sizeof(summary->exc_task));
            s["pc"] = Hex32(summary->exc_pc);
            s["coreDumpVersion"] = summary->core_dump_version;
            char sha[sizeof(summary->app_elf_sha256) + 1] = {};
            memcpy(sha, summary->app_elf_sha256, sizeof(summary->app_elf_sha256));
            s["elfSha256"] = sha;

            #if __XTENSA__
                s["arch"] = "xtensa";
                s["excCause"] = summary->ex_info.exc_cause;
                s["excVaddr"] = Hex32(summary->ex_info.exc_vaddr);
                auto backtrace = s["backtrace"].to<JsonArray>();
                for (uint32_t i = 0; i < std::min<uint32_t>(summary->exc_bt_info.depth, std::size(summary->exc_bt_info.bt)); ++i)
                    backtrace.add(Hex32(summary->exc_bt_info.bt[i]));
                s["backtraceCorrupted"] = summary->exc_bt_info.corrupted;
            #elif __riscv
                s["arch"] = "riscv";
                s["mcause"] = Hex32(summary->ex_info.mcause);
                s["mtval"] = Hex32(summary->ex_info.mtval);
                s["ra"] = Hex32(summary->ex_info.ra);
                s["sp"] = Hex32(summary->ex_info.sp);
                // No on-device unwinding on RISC-V: hand back the raw stack, for decoding on
                // the host (or fetch /api/v1/crash/raw for the full image).
                String stack;
                const auto stackBytes = std::min<uint32_t>(summary->exc_bt_info.dump_size, sizeof(summary->exc_bt_info.stackdump));
                stack.reserve(stackBytes * 2);
                for (uint32_t i = 0; i < stackBytes; ++i)
                {
                    char pair[3];
                    snprintf(pair, sizeof(pair), "%02x", summary->exc_bt_info.stackdump[i]);
                    stack += pair;
                }
                s["stackDump"] = stack;
            #endif
        }
    #endif

    AddCORSHeaderAndSendResponse(pRequest, response);
}

void CWebServer::GetCrashDump(AsyncWebServerRequest * pRequest)
{
    #if ND_HAS_CORE_DUMP
        size_t address = 0, size = 0;
        if (!IsCrashDumpPresent() || esp_core_dump_image_get(&address, &size) != ESP_OK)
        {
            AddCORSHeaderAndSendResponse(pRequest, pRequest->beginResponse(404, "text/plain", "No core dump stored"));
            return;
        }

        // Streamed straight from flash in whatever chunk size the server asks for, so the
        // image (up to the partition's size) is never held in RAM.
        auto response = pRequest->beginResponse("application/octet-stream", size,
            [address, size](uint8_t* buffer, size_t maxLen, size_t index) -> size_t
            {
                const size_t count = std::min(maxLen, size - index);
                return esp_flash_read(nullptr, buffer, address + index, count) == ESP_OK ? count : 0;
            });
        response->addHeader("Content-Disposition", "attachment; filename=\"coredump.bin\"");
        AddCORSHeaderAndSendResponse(pRequest, response);
    #else
        AddCORSHeaderAndSendResponse(pRequest, pRequest->beginResponse(404, "text/plain", "Core dumps not supported on this build"));
    #endif
}

void CWebServer::ClearCrashDump(AsyncWebServerRequest * pRequest)
{
    #if ND_HAS_CORE_DUMP
        if (HasCoreDumpPartition() && esp_core_dump_image_erase() != ESP_OK)
        {
            AddCORSHeaderAndSendResponse(pRequest, pRequest->beginResponse(500, "text/plain", "Erase failed"));
            return;
        }
        s_crashPresent.reset();
    #endif
    AddCORSHeaderAndSendOKResponse(pRequest);
}

void CWebServer::TriggerTestCrash(AsyncWebServerRequest * pRequest)
{
    if (!pRequest->hasParam("confirm", true) || pRequest->getParam("confirm", true)->value() != "yes")
    {
        AddCORSHeaderAndSendResponse(pRequest, pRequest->beginResponse(400, "text/plain", "POST confirm=yes to crash and reboot this controller"));
        return;
    }

    AddCORSHeaderAndSendResponse(pRequest, pRequest->beginResponse(202, "text/plain", "Crashing in 1 second"));

    // Panic from a short-lived task of its own, after the reply has gone out, so the
    // resulting core dump names an obvious culprit ("CrashTest") in its backtrace.
    xTaskCreate([](void*)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        debugW("Deliberate test crash requested over /api/v1/crash/test");
        abort();
    }, "CrashTest", 4096, nullptr, 1, nullptr);
}

#endif // ENABLE_WEBSERVER
