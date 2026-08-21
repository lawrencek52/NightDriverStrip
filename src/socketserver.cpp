//+--------------------------------------------------------------------------
//
// File:        socketserver.cpp
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
//    Hosts a socket server on port 49152 to receive LED data from the master
//
// History:     Oct-26-2018     Davepl      Created
//---------------------------------------------------------------------------

#include "globals.h"
#include "byte_utils.h"
#include "ledbuffer.h"
#include "nd_network.h"
#include "socketserver.h"
#include "soundanalyzer.h"
#include "systemcontainer.h"
#include "taskmgr.h"   // SOCKET_STACK_SIZE / SOCKET_PRIORITY / SOCKET_CORE
#include "values.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <new>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern "C"
{
    #include "uzlib/src/uzlib.h"
}

#if INCOMING_WIFI_ENABLED

namespace
{
    bool CheckedAdd(size_t left, size_t right, size_t& result)
    {
        if (left > std::numeric_limits<size_t>::max() - right)
            return false;

        result = left + right;
        return true;
    }
}

// SocketResponse
//
// Response data sent back to server every time we receive a packet

SocketServer::SocketServer(int port, int numLeds) :
    _port(port),
    _numLeds(numLeds)
{
    _abOutputBuffer = make_unique_psram<uint8_t[]>(MAXIMUM_PACKET_SIZE + 1);                    // +1 for uzlib one byte overreach bug
    memset(&_address, 0, sizeof(_address));
}

// ITaskService hooks
//
// Start/Stop/IsRunning are inherited final from ITaskService; this class only
// supplies the task config, the accept loop body, and the listening-socket
// shutdown nudge that breaks accept() out of its blocking call.

ITaskService::TaskConfig SocketServer::GetTaskConfig() const
{
    return TaskConfig {
        "Socket Server Loop",
        SOCKET_STACK_SIZE,
        SOCKET_PRIORITY,
        SOCKET_CORE,
        1500   // Stop timeout: accept() can block for ~1s before returning EBADF.
    };
}

void SocketServer::OnBeforeWaitForStop()
{
    // Closing the listening socket from this thread breaks the task out of
    // any blocking accept/read call so it can see ShouldShutdown() promptly.
    release();
}

// SocketServer::Run
//
// Opens the listening socket and services connections until told to stop. Note
// that the listener now outlives any one connection: it used to be torn down
// and rebuilt between clients, which discarded the accept backlog and reset
// every sender that was queued behind the one being served.
void SocketServer::Run()
{
    while (!ShouldShutdown())
    {
        if (!nd_network::IsWiFiConnected())
        {
            delay(500);
            continue;
        }

        if (!begin())
        {
            debugE("Failed to start socket server, retrying in 5 seconds...");
            delay(5000);
            continue;
        }

        ProcessIncomingConnectionsLoop();
        debugV("Socket server loop exited.  Rebuilding listener...");

        CloseAllClients();
        release();

        // Paced, so a listener that fails the moment it's built (no route, no
        // memory) can't spin this loop at full speed rebuilding it.
        delay(500);
    }

    // Drop everything if Stop() didn't already, so we leave the service in a
    // clean state regardless of which path we exited through.
    CloseAllClients();
    release();
}

void SocketServer::release()
{
    // Atomic exchange: only the caller that observed a non-negative fd does
    // the close(). The other (Run loop vs. OnBeforeWaitForStop) sees -1 and
    // becomes a no-op, eliminating a double-close race.
    int fd = _server_fd.exchange(-1);
    if (fd >= 0)
        close(fd);
}

bool SocketServer::begin()
{
    release();                                          // Drop any previous listener before rebuilding

    // Build the socket on a local fd and only publish it into the atomic
    // _server_fd member after listen() succeeds. That keeps release() (which
    // can run on a different thread via OnBeforeWaitForStop) from closing a
    // half-configured descriptor or racing with bind/listen.

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        debugE("socket error\n");
        return false;
    }

    nd_network::SetSocketBlockingEnabled(fd, false);

    // When an error occurs, and we close and reopen the port, we need to specify reuse flags
    // or it might be too soon to use the port again, since close doesn't actually close it
    // until the socket is no longer in use.

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        debugE("setsockopt SO_REUSEADDR failed on socket %d: %s (%d)", fd, strerror(errno), errno);
        close(fd);
        return false;
    }

    memset(&_address, 0, sizeof(_address));
    _address.sin_family = AF_INET;
    _address.sin_addr.s_addr = INADDR_ANY;
    _address.sin_port = htons( _port );

    if (bind(fd, (struct sockaddr *)&_address, sizeof(_address)) < 0)       // Bind socket to port
    {
        debugE("bind failed on port %d, socket %d: %s (%d)", _port, fd, strerror(errno), errno);
        close(fd);
        return false;
    }
    if (listen(fd, 6) < 0)                                                  // Start listening for connections
    {
        debugE("listen failed on port %d, socket %d: %s (%d)", _port, fd, strerror(errno), errno);
        close(fd);
        return false;
    }

    _server_fd.store(fd);
    debugI("Socket server %d listening on port %d", fd, _port);
    return true;
}

// DecompressBuffer
//
// Use unzlib to decompress a memory buffer

bool SocketServer::DecompressBuffer(const uint8_t * pBuffer, size_t cBuffer, uint8_t * pOutput, size_t expectedOutputSize)
{
    if (pBuffer == nullptr || pOutput == nullptr || cBuffer < 4)
    {
        debugE("Compressed packet too short to decompress: %zu bytes", cBuffer);
        return false;
    }

    debugV("Compressed Data: %02X %02X %02X %02X...", pBuffer[0], pBuffer[1], pBuffer[2], pBuffer[3]);

    struct uzlib_uncomp d = { 0 };
    uzlib_uncompress_init(&d, nullptr, 0);

    d.source         = pBuffer;
    d.source_limit   = pBuffer + cBuffer;
    d.source_read_cb = nullptr;
    d.dest_start     = pOutput;
    d.dest           = pOutput;

    // There's an "off by one" bug/feature in uzlib that reaches one byte past the end.  Took forever
    // to find it...

    d.dest_limit     = pOutput + expectedOutputSize + 1;

    int res = uzlib_zlib_parse_header(&d);
    if (res < 0)
    {
        debugE("ERROR: Cannot parse zlib data header\n");
        return false;
    }

    res = uzlib_uncompress_chksum(&d);                                          // Expand the data

    if (res != TINF_DONE) {
        debugE("Error during decompression after producing %d bytes: %d\n", d.dest - pOutput, res);
        return false;
    }

    if (d.dest - pOutput != expectedOutputSize)
    {
        debugE("Expected it to to decompress to %d but got %d instead\n", expectedOutputSize, d.dest - pOutput);
        return false;
    }

    return true;
}

// PacketBytesNeeded
//
// See the header for what this is for. Note that the answer grows as bytes
// arrive: 4 bytes tells us which header we're looking at, the header tells us
// how long the packet is. Callers re-ask after every read.

bool SocketServer::PacketBytesNeeded(const uint8_t* buffer, size_t have, size_t& needed)
{
    needed = sizeof(uint32_t);                          // The magic word, which picks the header format
    if (have < needed)
        return true;

    if (DWORDFromMemory(buffer) == COMPRESSED_HEADER)
    {
        needed = COMPRESSED_HEADER_SIZE;
        if (have < needed)
            return true;

        const uint32_t compressedSize = DWORDFromMemory(&buffer[4]);
        const uint32_t expandedSize   = DWORDFromMemory(&buffer[8]);

        if (!CheckedAdd(COMPRESSED_HEADER_SIZE, compressedSize, needed) ||
            needed > MAXIMUM_PACKET_SIZE ||
            expandedSize > MAXIMUM_PACKET_SIZE)
        {
            debugE("Compressed packet sizes are invalid: compressed=%lu expanded=%lu max=%lu",
                   (unsigned long)compressedSize, (unsigned long)expandedSize, (unsigned long)MAXIMUM_PACKET_SIZE);
            return false;
        }

        return true;
    }

    needed = STANDARD_DATA_HEADER_SIZE;
    if (have < needed)
        return true;

    const uint16_t command16 = WORDFromMemory(buffer);
    const uint32_t length32  = DWORDFromMemory(&buffer[4]);

    // Peak data counts bytes of audio bands, pixel data counts LEDs of three
    // bytes each. Anything else and we don't know where this packet ends, which
    // means we can't find the start of the next one either.

    size_t itemSize = 0;
    switch (command16)
    {
        case WIFI_COMMAND_PEAKDATA:     itemSize = sizeof(uint8_t); break;
        case WIFI_COMMAND_PIXELDATA64:  itemSize = LED_DATA_SIZE;   break;

        default:
            debugE("Unknown command in packet received: %u", command16);
            return false;
    }

    if (!CheckedStandardPacketSize(length32, itemSize, needed) || needed > MAXIMUM_PACKET_SIZE)
    {
        debugE("Too many bytes promised (%zu) - more than we can use for our LEDs at max packet (%lu)",
               needed, (unsigned long)MAXIMUM_PACKET_SIZE);
        return false;
    }

    return true;
}

// AcceptNewConnection
//
// Takes one pending connection off the listening socket and parks it in a free
// client slot.

void SocketServer::AcceptNewConnection(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    const int fd = accept(listen_fd, (struct sockaddr *)&addr, &addrlen);
    if (fd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            debugE("Socket server %d failed to accept connection: %s (%d)", listen_fd, strerror(errno), errno);
        return;
    }

    auto slot = std::find_if(_clients.begin(), _clients.end(), [](const ClientConnection& client) { return !client.InUse(); });

    if (slot == _clients.end())
    {
        // Accepted and immediately dropped rather than left queued: an
        // unaccepted connection keeps select() reporting the listener readable,
        // which would spin this loop. The sender will retry, and a slot frees
        // up as soon as one of the current ones closes or times out.

        debugW("All %zu client slots in use, refusing connection from %s", MAX_CLIENTS, inet_ntoa(addr.sin_addr));
        close(fd);
        return;
    }

    // The allocators throw rather than returning null, and an escaping bad_alloc
    // would take the socket task (and with it the whole server) down over one
    // connection we could simply have refused.

    try
    {
        slot->buffer = make_unique_psram<uint8_t[]>(MAXIMUM_PACKET_SIZE);
    }
    catch (const std::bad_alloc&)
    {
        debugE("Could not allocate a %lu byte packet buffer for the new client", (unsigned long)MAXIMUM_PACKET_SIZE);
        close(fd);
        return;
    }

    // Non-blocking, because this one task owns every client: blocking in read()
    // on a sender that stalled mid-packet would hold up all the others, and
    // that starvation is exactly what this server is meant to avoid.

    nd_network::SetSocketBlockingEnabled(fd, false);

    slot->fd             = fd;
    slot->cbReceived     = 0;
    slot->lastActivityMs = millis();

    debugI("Incoming connection from: %s (%zu of %zu slots in use)", inet_ntoa(addr.sin_addr), CountActiveClients(), MAX_CLIENTS);
}

// ServiceClient
//
// Reads what this client has waiting, dispatching each packet as it completes.
// Only ever reads up to the end of the packet in flight, so back-to-back frames
// stay framed. Returns false when the connection should be closed.

bool SocketServer::ServiceClient(ClientConnection& client)
{
    // Bounded so one busy sender can't monopolize the task while the other
    // channels wait; select() hands us straight back here if more is pending.

    constexpr int kMaxPacketsPerPass = 4;

    for (int packets = 0; packets < kMaxPacketsPerPass; )
    {
        size_t needed = 0;
        if (!PacketBytesNeeded(client.buffer.get(), client.cbReceived, needed))
            return false;

        if (client.cbReceived < needed)
        {
            const ssize_t cbRead = read(client.fd, client.buffer.get() + client.cbReceived, needed - client.cbReceived);

            if (cbRead > 0)
            {
                client.cbReceived += cbRead;
                client.lastActivityMs = millis();
                continue;                       // Re-frame: the header may now say more is coming
            }

            if (cbRead == 0)                    // Orderly close by the peer
                return false;

            if (errno == EINTR)
                continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;                    // Drained for now; the rest of the packet arrives on a later pass

            debugE("Read error on client socket %d: %s (%d)", client.fd, strerror(errno), errno);
            return false;
        }

        if (!ProcessCompletePacket(client, needed))
            return false;

        client.cbReceived = 0;
        packets++;
    }

    return true;
}

// ResponseChannelFor
//
// Which channel's statistics belong in the reply to this packet: the first one
// the packet targeted, since the reply has room for exactly one. Anything that
// isn't pixel data - or is too short to say - reports on channel 0.

static size_t ResponseChannelFor(const uint8_t* packet, size_t packetLength)
{
    if (packetLength < STANDARD_DATA_HEADER_SIZE || WORDFromMemory(packet) != WIFI_COMMAND_PIXELDATA64)
        return 0;

    return FirstChannelInMask(WORDFromMemory(&packet[2]));
}

// ProcessCompletePacket
//
// Hands one whole packet to ProcessIncomingData(), decompressing it first if it
// arrived that way, and answers the sender for the packet types that expect it.

bool SocketServer::ProcessCompletePacket(ClientConnection& client, size_t packetSize)
{
    if (DWORDFromMemory(client.buffer.get()) == COMPRESSED_HEADER)
    {
        const uint32_t compressedSize = DWORDFromMemory(&client.buffer[4]);
        const uint32_t expandedSize   = DWORDFromMemory(&client.buffer[8]);

        debugV("Compressed Header: compressedSize: %lu, expandedSize: %lu",
               (unsigned long)compressedSize, (unsigned long)expandedSize);

        // If our buffer is in PSRAM it would be expensive to decompress in place, as the SPIRAM doesn't like
        // non-linear access from what I can tell.  I bet it must send addr+len to request each unique read, so
        // one big read one time would work best, and we use that to copy it to a regular RAM buffer.

        #if USE_PSRAM
            allocated_unique_ptr<uint8_t[]> tempBuffer;
            try
            {
                tempBuffer = make_unique_internal<uint8_t[]>(packetSize + 1);   // Plus one for uzlib buffer overreach bug
            }
            catch (const std::bad_alloc&)
            {
                debugE("Could not allocate %zu bytes of internal RAM to decompress from", packetSize + 1);
                return false;
            }
            memcpy(tempBuffer.get(), client.buffer.get(), packetSize);
            auto pSourceBuffer = &tempBuffer[COMPRESSED_HEADER_SIZE];
        #else
            auto pSourceBuffer = &client.buffer[COMPRESSED_HEADER_SIZE];
        #endif

        if (!DecompressBuffer(pSourceBuffer, compressedSize, _abOutputBuffer.get(), expandedSize))
        {
            debugE("Error decompressing data\n");
            return false;
        }

        if (!ProcessIncomingData(_abOutputBuffer, expandedSize))
        {
            debugE("Error processing data\n");
            return false;
        }

        // The channel lives in the packet that was compressed, not the wrapper.
        SendResponsePacket(client.fd, ResponseChannelFor(_abOutputBuffer.get(), expandedSize));
        return true;
    }

    const uint16_t command16 = WORDFromMemory(client.buffer.get());

    // The semantic checks - band counts, LED counts against the channel - live
    // in ProcessIncomingData(); framing above has already established that the
    // packet is entire and within our size limits.

    if (!ProcessIncomingData(client.buffer, packetSize))
    {
        debugE("Error processing packet with command %u from wifi\n", command16);
        return false;
    }

    // Peak data is fire and forget; only pixel frames are acknowledged.
    if (command16 == WIFI_COMMAND_PIXELDATA64)
        SendResponsePacket(client.fd, ResponseChannelFor(client.buffer.get(), packetSize));

    return true;
}

// SendResponsePacket
//
// Reports our clock, buffer depth and power draw back to the sender that just
// fed us a frame, which is how it paces itself. The statistics are those of the
// channel the frame was for: with a sender per strip, telling all of them about
// channel 0 would have every sender but one pacing against a queue that isn't
// theirs.

void SocketServer::SendResponsePacket(int fd, size_t channel)
{
    static uint64_t sequence = 0;

    debugV("Sending Response Packet from Socket Server for channel %zu", channel);

    auto& bufferManagers = g_ptrSystem->GetBufferManagers();

    // A sender is free to name a channel this device doesn't have; report on
    // channel 0 rather than reading off the end of the array.
    if (channel >= bufferManagers.size())
        channel = 0;

    auto& bufferManager = bufferManagers[channel];

    std::lock_guard guard(g_buffer_mutex);
    SocketResponse response = {
                                .size = sizeof(SocketResponse),
                                .sequence     = sequence++,
                                .flashVersion = FLASH_VERSION,
                                .currentClock = g_Values.AppTime.CurrentTime(),
                                .oldestPacket = bufferManager.AgeOfOldestBuffer(),
                                .newestPacket = bufferManager.AgeOfNewestBuffer(),
                                .brightness   = g_Values.Brite,
                                .wifiSignal   = (float) nd_network::GetWiFiRSSI(),
                                .bufferSize   = bufferManager.BufferCount(),
                                .bufferPos    = bufferManager.Depth(),
                                .fpsDrawing   = g_Values.FPS,
                                .watts        = g_Values.Watts
                              };

    // Not fatal, and it doesn't affect the frame we just accepted: a client
    // that isn't draining its responses simply misses this one.
    if (sizeof(response) != write(fd, &response, sizeof(response)))
        debugV("Unable to send response back to server.");
}

void SocketServer::ReapIdleClients()
{
    const uint32_t now = millis();

    for (auto& client : _clients)
        if (client.InUse() && (now - client.lastActivityMs) > CLIENT_IDLE_TIMEOUT_MS)   // Unsigned math survives rollover
            CloseClient(client, "idle timeout");
}

void SocketServer::CloseClient(ClientConnection& client, const char* reason)
{
    if (!client.InUse())
        return;

    debugI("Closing client socket %d: %s", client.fd, reason);

    close(client.fd);
    client.fd         = -1;
    client.cbReceived = 0;
    client.buffer.reset();                              // Give the packet buffer back until the slot is reused
}

void SocketServer::CloseAllClients()
{
    for (auto& client : _clients)
        CloseClient(client, "server shutting down");
}

size_t SocketServer::CountActiveClients() const
{
    return std::count_if(_clients.begin(), _clients.end(), [](const ClientConnection& client) { return client.InUse(); });
}

size_t SocketServer::PendingPacketBytes() const
{
    size_t pending = 0;
    for (const auto& client : _clients)
        pending += client.cbReceived;

    return pending;
}

// ProcessIncomingConnectionsLoop
//
// Socket server main loop - one select() covering the listening socket and
// every connected client, so all of the senders feeding a multi-channel device
// are served from this single task. Returns when the listener goes bad, WiFi
// drops, or we're stopping; Run() then rebuilds and calls us again.

bool SocketServer::ProcessIncomingConnectionsLoop()
{
    while (!ShouldShutdown())
    {
        const int listen_fd = _server_fd.load();
        if (listen_fd < 0)
        {
            debugE("No _server_fd, returning.");
            return false;
        }

        if (!nd_network::IsWiFiConnected())
        {
            debugW("WiFi connection lost, dropping socket clients");
            return false;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listen_fd, &readSet);
        int maxFd = listen_fd;

        for (const auto& client : _clients)
        {
            if (client.InUse())
            {
                FD_SET(client.fd, &readSet);
                maxFd = std::max(maxFd, client.fd);
            }
        }

        // A short tick rather than an indefinite wait, so shutdown stays
        // responsive and the idle sweep still runs on a quiet network.

        timeval timeout = { .tv_sec = 0, .tv_usec = 100 * 1000 };

        const int ready = select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            debugE("select() failed on the socket server: %s (%d)", strerror(errno), errno);
            return false;
        }

        if (ready > 0)
        {
            if (FD_ISSET(listen_fd, &readSet))
                AcceptNewConnection(listen_fd);

            for (auto& client : _clients)
                if (client.InUse() && FD_ISSET(client.fd, &readSet) && !ServiceClient(client))
                    CloseClient(client, "connection closed or protocol error");
        }

        ReapIdleClients();
    }

    return true;
}

#endif
