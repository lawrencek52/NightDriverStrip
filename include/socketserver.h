#pragma once

//+--------------------------------------------------------------------------
//
// File:        SocketServer.h
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

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>

#include "itaskservice.h"

#define STANDARD_DATA_HEADER_SIZE   24                                             // Size of the header for expanded data
#define COMPRESSED_HEADER_SIZE      16                                             // Size of the header for compressed data
#define LED_DATA_SIZE               sizeof(CRGB)                                   // Data size of an LED (24 bits or 3 bytes)

// We allocate whatever the max packet is, and use it to validate incoming packets, so right now it's set to the maximum
// LED data packet you could have (header plus 3 RGBs per NUM_LED)

#define MAXIMUM_PACKET_SIZE (STANDARD_DATA_HEADER_SIZE + LED_DATA_SIZE * NUM_LEDS) // Header plus 24 bits per actual LED
#define COMPRESSED_HEADER (0x44415645)                                             // ASCII "DAVE" as header

// Overflow-safe `STANDARD_DATA_HEADER_SIZE + itemCount * itemSize`. Returns
// false and clears packetSize if the multiply or add would wrap.

inline bool CheckedStandardPacketSize(uint32_t itemCount, size_t itemSize, size_t& packetSize)
{
    if (itemSize == 0 || itemCount > (std::numeric_limits<size_t>::max() - STANDARD_DATA_HEADER_SIZE) / itemSize)
    {
        packetSize = 0;
        return false;
    }

    packetSize = STANDARD_DATA_HEADER_SIZE + itemCount * itemSize;
    return true;
}

// NormalizeChannelMask / FirstChannelInMask
//
// The channel field of a pixel packet is a bitmask - bit 0 is channel 0 - so a
// sender can paint several strips with one frame. The original implementation
// predated multi-channel devices and sent a plain 0, which is why a 0 is read
// as "channel 0 only" rather than "no channels".
//
// Shared by the receive path, which fans a packet out to every channel in the
// mask, and by the status reply, which has room for one channel's statistics
// and reports the first one the packet targeted.

inline uint16_t NormalizeChannelMask(uint16_t channel16)
{
    return channel16 == 0 ? 1 : channel16;
}

inline size_t FirstChannelInMask(uint16_t channel16)
{
    return __builtin_ctz(NormalizeChannelMask(channel16));      // Never zero once normalized
}

bool ProcessIncomingData(allocated_unique_ptr<uint8_t []> & payloadData, size_t payloadLength);

#if INCOMING_WIFI_ENABLED

// SocketResponse
//
// Response data sent back to server every time we receive a packet
struct SocketResponse
{
    uint32_t    size;              // 4
    uint64_t    sequence;          // 8
    uint32_t    flashVersion;      // 4
    double      currentClock;      // 8
    double      oldestPacket;      // 8
    double      newestPacket;      // 8
    double      brightness;        // 8
    double      wifiSignal;        // 8
    uint32_t    bufferSize;        // 4
    uint32_t    bufferPos;         // 4
    uint32_t    fpsDrawing;        // 4
    uint32_t    watts;             // 4
} __attribute__((packed));

static_assert(sizeof(double) == 8);             // SocketResponse on wire uses 8 byte doubles
static_assert(sizeof(float)  == 4);             // PeakData on wire uses 4 byte floats

// Two things must be true for this to work and interop with the C# side:  floats must be 8 bytes, not the default
// of 4 for Arduino.  So that must be set in 'platformio.ini', and you must ensure that you align things such that
// floats land on byte multiples of 8, otherwise you'll get packing bytes inserted.  Welcome to my world! Once upon
// a time, I ported about a billion lines of x86 'pragma_pack(1)' code to the MIPS (davepl)!

static_assert( sizeof(SocketResponse) == 72, "SocketResponse struct size is not what is expected - check alignment and float size" );

// SocketServer
//
// Handles incoming connections from the server and passes the data that comes
// in. Inherits ITaskService so the accept/read loop, shutdown signaling, and
// listening-socket teardown all share the standard service lifecycle.
//
// Several senders are served at once, which is what a multi-channel device
// needs: the companion NDSCPP server opens one socket per LED feature, so a
// four-strip device sees four simultaneous connections, each streaming frames
// for its own channel. All of them are multiplexed through a single select()
// in one task rather than a task (or a turn) per connection.

class SocketServer : public ITaskService
{
private:

    // One sender per output channel is the expected case, plus headroom for a
    // client that is reconnecting before its previous socket has been reaped.
    // Capped well below CONFIG_LWIP_MAX_SOCKETS (16 on the S3) so the web
    // server, OTA and the color data server keep their own descriptors.

    static constexpr size_t MAX_CLIENTS = (NUM_CHANNELS + 2) > 8 ? 8 : (NUM_CHANNELS + 2);

    // A client that has gone quiet for this long is dropped, so a wedged sender
    // - or one that vanished without a FIN - can't hold a slot indefinitely.
    // Generous next to the 30fps a live sender runs at.

    static constexpr uint32_t CLIENT_IDLE_TIMEOUT_MS = 10000;

    // ClientConnection
    //
    // Everything the framing state machine needs for one connected sender.
    // The packet buffer has to be per connection because packets from
    // different senders interleave: each one accumulates in its own buffer
    // until it holds a whole packet. The buffer is only allocated while the
    // slot is in use.

    struct ClientConnection
    {
        int                              fd             = -1;
        allocated_unique_ptr<uint8_t []> buffer;
        size_t                           cbReceived     = 0;   // Bytes of the in-flight packet held so far
        uint32_t                         lastActivityMs = 0;   // millis() at the last successful read

        bool InUse() const { return fd >= 0; }
    };

    int                         _port;
    int                         _numLeds;

    // Listening socket fd. atomic<int> because release() can be invoked from
    // both the SocketServer task (Run/begin failure paths) and from the
    // service-stop path (OnBeforeWaitForStop, called on the caller's thread).
    // Using atomic exchange ensures only one of those callers actually
    // close()s the descriptor; the other observes -1 and is a no-op.

    std::atomic<int>            _server_fd{-1};
    struct sockaddr_in          _address;
    std::array<ClientConnection, MAX_CLIENTS> _clients;
    allocated_unique_ptr<uint8_t []> _abOutputBuffer;          // Shared decompression scratch

public:

    SocketServer(int port, int numLeds);
    ~SocketServer() override { Stop(); }

    // IService::Name
    const char* Name() const override { return "SocketServer"; }

    void release();
    bool begin();
    void SetLEDCount(size_t numLeds) { _numLeds = numLeds; }
    size_t GetLEDCount() const { return _numLeds; }

    // Status, for the debug CLI: how many senders are connected, and how many
    // bytes of half-arrived packets they are collectively holding.

    size_t CountActiveClients() const;
    size_t PendingPacketBytes() const;
    static constexpr size_t MaxClients() { return MAX_CLIENTS; }

  protected:
    // ITaskService hooks
    TaskConfig GetTaskConfig() const override;
    void Run() override;
    void OnBeforeWaitForStop() override;

  private:

    // ProcessIncomingConnectionsLoop
    //
    // Socket server main loop: one select() over the listening socket and every
    // connected client, accepting new senders and draining the ones with data
    // waiting. Returns when the listening socket goes bad, WiFi drops, or the
    // service is stopping - the caller then rebuilds the listener.

    bool ProcessIncomingConnectionsLoop();

    void AcceptNewConnection(int listen_fd);

    // Drains what is available from one client, framing and dispatching whole
    // packets as they complete. Returns false if the connection should be closed.
    bool ServiceClient(ClientConnection& client);

    bool ProcessCompletePacket(ClientConnection& client, size_t packetSize);
    void SendResponsePacket(int fd, size_t channel);

    void ReapIdleClients();
    void CloseClient(ClientConnection& client, const char* reason);
    void CloseAllClients();

    // PacketBytesNeeded
    //
    // Framing. Reports how many bytes the packet at the head of the buffer needs
    // in total, given the 'have' bytes of it that have arrived: four for the
    // magic word, then the size of whichever header the magic indicates, then
    // the whole packet. Reading no further than this is what keeps the reader
    // from swallowing the head of the next packet in a back-to-back stream.
    // Returns false on a malformed header, which costs the connection.

    static bool PacketBytesNeeded(const uint8_t* buffer, size_t have, size_t& needed);

  public:

    // DecompressBuffer
    //
    // Use unzlib to decompress a memory buffer

    static bool DecompressBuffer(const uint8_t * pBuffer, size_t cBuffer, uint8_t * pOutput, size_t expectedOutputSize);
};

#endif
