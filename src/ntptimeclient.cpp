//+--------------------------------------------------------------------------
//
// File:        ntptimeclient.cpp
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
//    Sets the system clock from the specified NTP Server
//
// History:     Jul-12-2018         Davepl      Created for BigBlueLCD
//              Oct-09-2018         Davepl      Copied to LEDWifi project
//---------------------------------------------------------------------------

#include "globals.h"
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/time.h>
#include <WiFiUdp.h>
#include "nd_network.h"

#include "deviceconfig.h"
#include "ledbuffer.h"
#include "ntptimeclient.h"
#include "socketserver.h"
#include "systemcontainer.h"
#include "values.h"

#if ENABLE_NTP

// NTPTimeClient
//
// Basically, I took some really ancient NTP code that I had on hand that I knew
// worked and wrapped it in a class.  As expected, it works, but it could likely
// benefit from cleanup or even wholesale replacement.

// File-static variables to replace class static members
static DRAM_ATTR bool l_bClockSet = false;
static DRAM_ATTR std::mutex l_clockMutex;

bool NTPTimeClient::HasClockBeenSet()
{
    return l_bClockSet;
}

// NTP wire timestamps are seconds since 1900 as a 32.32 fixed-point number.

static constexpr double kNtpToUnixSeconds = 2208988800.0;

static double NowSeconds()
{
    timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec / (double) MICROS_PER_SECOND;
}

static double NtpTimestampToUnix(const uint8_t* p)
{
    const uint32_t seconds  = (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
    const uint32_t fraction = (uint32_t) p[4] << 24 | (uint32_t) p[5] << 16 | (uint32_t) p[6] << 8 | p[7];
    return seconds - kNtpToUnixSeconds + fraction / 4294967296.0;
}

static void UnixToNtpTimestamp(double unixSeconds, uint8_t* p)
{
    const double ntp = unixSeconds + kNtpToUnixSeconds;
    const uint32_t seconds  = (uint32_t) ntp;
    const uint32_t fraction = (uint32_t) ((ntp - seconds) * 4294967296.0);
    for (int i = 0; i < 4; i++)
    {
        p[i]     = seconds  >> (24 - 8 * i);
        p[4 + i] = fraction >> (24 - 8 * i);
    }
}

// QueryNtpServer
//
// One standard NTP exchange. With t1 = our send time, t2/t3 = the server's receive/transmit
// times, and t4 = our receive time, the clock offset is ((t2 - t1) + (t3 - t4)) / 2, which
// cancels the network delay as long as it's about the same both ways. The reply is polled for
// every millisecond so t4 is accurate, and must echo our transmit timestamp so a stale reply
// to an earlier request can't be mistaken for this one.

static bool QueryNtpServer(WiFiUDP* pUDP, const IPAddress& server, double& offset, double& roundTrip)
{
    constexpr uint32_t kReplyTimeoutMs = 1500;

    while (pUDP->parsePacket() != 0)
        #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
            pUDP->clear();
        #else
            pUDP->flush();
        #endif

    uint8_t packet[NTP_PACKET_LENGTH] = {};
    packet[0] = 0b00100011;                     // leap 0, version 4, mode 3 (client)

    const double t1 = NowSeconds();
    UnixToNtpTimestamp(t1, &packet[40]);

    if (!pUDP->beginPacket(server, 123) || pUDP->write(packet, NTP_PACKET_LENGTH) != NTP_PACKET_LENGTH || !pUDP->endPacket())
    {
        debugW("NTP: could not send request to %s", server.toString().c_str());
        return false;
    }

    const uint32_t sentMs = millis();
    while (!pUDP->parsePacket())
    {
        if (millis() - sentMs > kReplyTimeoutMs)
        {
            debugW("NTP: no reply from %s", server.toString().c_str());
            return false;
        }
        delay(1);
    }
    const double t4 = NowSeconds();

    uint8_t reply[NTP_PACKET_LENGTH];
    if (pUDP->read(reply, NTP_PACKET_LENGTH) != NTP_PACKET_LENGTH)
        return false;

    const uint8_t mode    = reply[0] & 0x07;
    const uint8_t stratum = reply[1];
    if (mode != 4 || stratum == 0 || stratum > 15 || memcmp(&reply[24], &packet[40], 8) != 0)
    {
        debugW("NTP: ignoring bad reply from %s (mode %u, stratum %u)", server.toString().c_str(), mode, stratum);
        return false;
    }

    const double t2 = NtpTimestampToUnix(&reply[32]);
    const double t3 = NtpTimestampToUnix(&reply[40]);
    offset    = ((t2 - t1) + (t3 - t4)) / 2;
    roundTrip = (t4 - t1) - (t3 - t2);
    return roundTrip >= 0;
}

// Best of a few exchanges: the one with the shortest round trip had the least queuing delay
// in it, so its offset is the most trustworthy.

static bool MeasureOffset(WiFiUDP* pUDP, const IPAddress& server, double& offset, double& roundTrip)
{
    bool any = false;
    for (int i = 0; i < 3; i++)
    {
        double sampleOffset, sampleRoundTrip;
        if (!QueryNtpServer(pUDP, server, sampleOffset, sampleRoundTrip))
            break;      // Unreachable or misbehaving: don't spend two more timeouts on it
        if (!any || sampleRoundTrip < roundTrip)
        {
            offset    = sampleOffset;
            roundTrip = sampleRoundTrip;
            any       = true;
        }
    }
    return any;
}

static DRAM_ATTR bool   l_bLastSyncLocal = false;
static DRAM_ATTR double l_lastOffsetMs   = 0;
static DRAM_ATTR double l_lastRoundTripMs = 0;
static String           l_lastServer;

bool NTPTimeClient::LastSyncWasLocal()  { return l_bLastSyncLocal; }
double NTPTimeClient::LastOffsetMs()    { return l_lastOffsetMs; }
double NTPTimeClient::LastRoundTripMs() { return l_lastRoundTripMs; }
String NTPTimeClient::LastServer()      { return l_lastServer; }

// UpdateClockFromWeb
//
// Measures the clock against an NTP server and steers it. While LED-Central is sending frames
// (any in the last two minutes) its host is the server, because frame timestamps come from its
// clock and it's one hop away; if it doesn't answer, or isn't sending, the configured NTP server
// is used instead.
//
// The first sync after boot, or an error over a second, steps the clock straight to the measured
// time. Otherwise it moves a quarter of the offset per sync: a small step each time instead of
// one big jump, which settles within a few milliseconds while absorbing the crystal's drift and
// the odd noisy measurement.

bool NTPTimeClient::UpdateClockFromWeb(WiFiUDP * pUDP)
{
    if (g_Values.UpdateStarted)
    {
        debugW("Update already in progress, skipping time check, as it seems to disturb the update process.");
        return false;
    }

    std::lock_guard guard(l_clockMutex);

    constexpr uint32_t kFrameSenderMaxAgeMs = 2 * 60 * 1000;
    constexpr double   kStepThreshold       = 1.0;
    constexpr double   kSlewFraction        = 0.25;

    double offset = 0, roundTrip = 0;
    bool measured = false;
    bool local = false;
    IPAddress server;

    // A host that doesn't answer NTP (LED-Central's machine isn't serving it) is left alone for
    // a while rather than costing a timeout on every sync.
    constexpr uint32_t kLocalRetryMs = 10 * 60 * 1000;
    static uint32_t s_localFailedMs = 0;
    static bool s_localFailed = false;

    uint32_t frameSender = 0;
    if (GetRecentFrameSender(frameSender, kFrameSenderMaxAgeMs) && (!s_localFailed || millis() - s_localFailedMs > kLocalRetryMs))
    {
        server = IPAddress(frameSender);
        measured = local = MeasureOffset(pUDP, server, offset, roundTrip);
        s_localFailed = !measured;
        if (!measured)
        {
            s_localFailedMs = millis();
            debugW("NTP: LED-Central's host %s doesn't answer NTP; using the configured server for now", server.toString().c_str());
        }
    }

    if (!measured)
    {
        if (!nd_network::GetWiFiHostByName(g_ptrSystem->GetDeviceConfig().GetNTPServer().c_str(), server))
            // Use Google Time (time.google.com) as default. The pool.ntp.org
            // servers (IPs) don't necessarily last very long.
            server.fromString("216.239.35.12");
        measured = MeasureOffset(pUDP, server, offset, roundTrip);
    }

    if (!measured)
        return false;

    const bool step = !l_bClockSet || std::abs(offset) > kStepThreshold;
    const double correction = step ? offset : offset * kSlewFraction;

    const double target = NowSeconds() + correction;
    timeval tvNew;
    tvNew.tv_sec  = (time_t) target;
    tvNew.tv_usec = (suseconds_t) ((target - tvNew.tv_sec) * MICROS_PER_SECOND);
    settimeofday(&tvNew, nullptr);

    l_bLastSyncLocal  = local;
    l_lastOffsetMs    = offset * 1000;
    l_lastRoundTripMs = roundTrip * 1000;
    l_lastServer      = server.toString();

    debugI("NTP: %s %s offset %.1f ms (round trip %.1f ms), %s %.1f ms",
           local ? "LED-Central" : "server", l_lastServer.c_str(), offset * 1000, roundTrip * 1000,
           step ? "stepped" : "slewed", correction * 1000);

    l_bClockSet = true;  // Clock has been set at least once

    return true;
}

#endif
