/*
 *     Copyright (C) 2026  Aaro (sybenx)
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef LIBRETRODROID_NETPLAY_H
#define LIBRETRODROID_NETPLAY_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace libretrodroid {

/**
 * Lockstep netplay between two devices running the same content. Each device sends the joypad state of
 * its own port for a frame a few frames ahead (the input delay), and a frame only runs once both
 * players' inputs for it are known. Both devices then run identical frames. The host sends its state
 * at the start, and again whenever the periodic state checksums of the two devices differ.
 *
 * The socket is a connected TCP socket owned by this object. All methods but the constructor and
 * destructor are called from the emulation thread.
 */
class Netplay {
public:
    enum class Status { CONNECTED = 1, ENDED = 2 };

    struct PendingState {
        uint32_t frame;
        std::vector<uint8_t> data;
    };

    Netplay(int socketFd, bool isHost, unsigned localPort, unsigned inputDelay);
    ~Netplay();

    bool isHost() const { return host; }
    unsigned getLocalPort() const { return localPort; }
    uint32_t getFrame() const { return frame; }
    Status getStatus() const { return connected ? Status::CONNECTED : Status::ENDED; }
    bool isConnected() const { return connected; }

    /** Host: sends the starting state. */
    void sendInitialState(const std::vector<uint8_t>& state);

    /** Guest: waits for the host's starting state. */
    std::optional<std::vector<uint8_t>> receiveInitialState(std::chrono::milliseconds timeout);

    /**
     * Sends this device's input for a later frame and makes the inputs of the next frame current.
     * Returns false if the other device's input has not arrived yet, so the frame must wait.
     */
    bool prepareFrame(uint16_t localInput, std::chrono::milliseconds timeout);

    /** Called after the frame prepared by prepareFrame() ran. */
    void frameCompleted();

    /** Joypad state for the frame being run. */
    int16_t getInputState(unsigned port, unsigned device, unsigned id) const;

    /** Whether a checksum of the state should be computed now. */
    bool isChecksumFrame() const;
    void submitChecksum(uint64_t checksum);

    /** Host: a checksum differed, so the current state should be sent. */
    bool consumeResyncRequest();
    void sendState(const std::vector<uint8_t>& state);

    /** Guest: a state sent by the host, once this device has run at least as many frames. */
    std::optional<PendingState> takePendingState();

    /** Makes the inputs of an already run frame current again, to run it a second time. */
    void restoreFrameInputs(uint32_t replayFrame);
    void setFrame(uint32_t newFrame) { frame = newFrame; }

    static uint64_t computeChecksum(const std::vector<uint8_t>& data);

private:
    enum PacketType : uint8_t {
        PACKET_INPUT = 1,
        PACKET_CHECKSUM = 2,
        PACKET_STATE = 3,
        PACKET_PING = 4,
    };

    static constexpr uint32_t CHECKSUM_INTERVAL = 120;
    static constexpr uint32_t HISTORY_FRAMES = 600;
    static constexpr std::chrono::seconds PING_INTERVAL = std::chrono::seconds(1);
    static constexpr std::chrono::seconds SILENCE_TIMEOUT = std::chrono::seconds(10);

    void readLoop();
    void pingLoop();
    bool readFully(void* buffer, size_t size);
    bool sendPacket(PacketType type, const void* payload, uint32_t size);
    void compareChecksums(uint32_t checkedFrame);
    void disconnect();

    int socketFd;
    bool host;
    unsigned localPort;
    unsigned remotePort;
    unsigned inputDelay;

    std::atomic<bool> connected { true };
    std::atomic<bool> stopping { false };
    std::atomic<int64_t> lastReceivedAt;

    std::mutex sendMutex;

    std::mutex mutex;
    std::condition_variable condition;
    std::map<uint32_t, uint16_t> localInputs;
    std::map<uint32_t, uint16_t> remoteInputs;
    std::map<uint32_t, uint64_t> localChecksums;
    std::map<uint32_t, uint64_t> remoteChecksums;
    std::optional<PendingState> pendingState;
    bool resyncRequested = false;

    uint32_t frame = 0;
    uint16_t currentLocalInput = 0;
    uint16_t currentRemoteInput = 0;

    std::thread readThread;
    std::thread pingThread;
};

}

#endif //LIBRETRODROID_NETPLAY_H
