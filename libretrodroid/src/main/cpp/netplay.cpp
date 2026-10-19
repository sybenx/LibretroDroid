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

#include "netplay.h"

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "core.h"
#include "log.h"

namespace libretrodroid {

namespace {

int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void putUint32(std::vector<uint8_t>& buffer, uint32_t value) {
    for (int i = 0; i < 4; i++) buffer.push_back((value >> (8 * i)) & 0xFF);
}

uint32_t readUint32(const uint8_t* data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t) data[3] << 24);
}

}

Netplay::Netplay(int socketFd, bool isHost, unsigned localPort, unsigned inputDelay):
    socketFd(socketFd),
    host(isHost),
    localPort(localPort),
    remotePort(localPort == 0 ? 1 : 0),
    inputDelay(inputDelay),
    lastReceivedAt(nowMillis()) {

    // Nobody can press anything before the first input arrives.
    for (uint32_t i = 0; i < inputDelay; i++) {
        localInputs[i] = 0;
        remoteInputs[i] = 0;
    }

    readThread = std::thread(&Netplay::readLoop, this);
    pingThread = std::thread(&Netplay::pingLoop, this);
}

Netplay::~Netplay() {
    stopping = true;
    disconnect();
    if (readThread.joinable()) readThread.join();
    if (pingThread.joinable()) pingThread.join();
    close(socketFd);
}

void Netplay::disconnect() {
    if (connected.exchange(false)) {
        LOGI("Netplay disconnected");
    }
    shutdown(socketFd, SHUT_RDWR);
    condition.notify_all();
}

bool Netplay::readFully(void* buffer, size_t size) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < size) {
        ssize_t count = recv(socketFd, bytes + done, size - done, 0);
        if (count <= 0) return false;
        done += count;
    }
    return true;
}

bool Netplay::sendPacket(PacketType type, const void* payload, uint32_t size) {
    std::vector<uint8_t> packet;
    packet.reserve(5 + size);
    packet.push_back(type);
    putUint32(packet, size);
    auto* bytes = static_cast<const uint8_t*>(payload);
    packet.insert(packet.end(), bytes, bytes + size);

    std::lock_guard<std::mutex> lock(sendMutex);
    size_t done = 0;
    while (done < packet.size()) {
        ssize_t count = send(socketFd, packet.data() + done, packet.size() - done, MSG_NOSIGNAL);
        if (count <= 0) {
            disconnect();
            return false;
        }
        done += count;
    }
    return true;
}

void Netplay::readLoop() {
    std::vector<uint8_t> payload;
    while (!stopping) {
        uint8_t header[5];
        if (!readFully(header, sizeof(header))) break;
        uint32_t size = readUint32(header + 1);
        payload.resize(size);
        if (size > 0 && !readFully(payload.data(), size)) break;
        lastReceivedAt = nowMillis();

        std::lock_guard<std::mutex> lock(mutex);
        switch (header[0]) {
            case PACKET_INPUT: {
                if (size < 6) break;
                uint32_t inputFrame = readUint32(payload.data());
                remoteInputs[inputFrame] = payload[4] | (payload[5] << 8);
                break;
            }
            case PACKET_CHECKSUM: {
                if (size < 12) break;
                uint32_t checkedFrame = readUint32(payload.data());
                uint64_t checksum;
                memcpy(&checksum, payload.data() + 4, sizeof(checksum));
                remoteChecksums[checkedFrame] = checksum;
                compareChecksums(checkedFrame);
                break;
            }
            case PACKET_STATE: {
                if (size < 4) break;
                PendingState state { readUint32(payload.data()), {} };
                state.data.assign(payload.begin() + 4, payload.end());
                pendingState = std::move(state);
                break;
            }
            default:
                break;
        }
        condition.notify_all();
    }
    disconnect();
}

void Netplay::pingLoop() {
    auto lastPing = std::chrono::steady_clock::now();
    while (!stopping && connected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (std::chrono::steady_clock::now() - lastPing >= PING_INTERVAL) {
            lastPing = std::chrono::steady_clock::now();
            sendPacket(PACKET_PING, nullptr, 0);
        }
        auto silence = std::chrono::milliseconds(nowMillis() - lastReceivedAt);
        if (silence > SILENCE_TIMEOUT) {
            LOGE("Netplay: nothing received for %lld ms", (long long) silence.count());
            disconnect();
        }
    }
}

void Netplay::sendInitialState(const std::vector<uint8_t>& state) {
    sendState(state);
}

std::optional<std::vector<uint8_t>> Netplay::receiveInitialState(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait_for(lock, timeout, [&] { return pendingState.has_value() || !connected; });
    if (!pendingState.has_value()) return std::nullopt;
    auto result = std::move(pendingState->data);
    pendingState.reset();
    return result;
}

bool Netplay::prepareFrame(uint16_t localInput, std::chrono::milliseconds timeout) {
    uint32_t targetFrame = frame + inputDelay;
    bool isNewInput = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (localInputs.find(targetFrame) == localInputs.end()) {
            localInputs[targetFrame] = localInput;
            isNewInput = true;
        }
    }

    if (isNewInput) {
        std::vector<uint8_t> payload;
        putUint32(payload, targetFrame);
        payload.push_back(localInput & 0xFF);
        payload.push_back(localInput >> 8);
        sendPacket(PACKET_INPUT, payload.data(), payload.size());
    }

    std::unique_lock<std::mutex> lock(mutex);
    condition.wait_for(lock, timeout, [&] { return remoteInputs.count(frame) > 0 || !connected; });
    auto remote = remoteInputs.find(frame);
    if (remote == remoteInputs.end()) return false;

    currentLocalInput = localInputs[frame];
    currentRemoteInput = remote->second;
    return true;
}

void Netplay::frameCompleted() {
    frame++;
    if (frame <= HISTORY_FRAMES) return;

    std::lock_guard<std::mutex> lock(mutex);
    uint32_t oldest = frame - HISTORY_FRAMES;
    localInputs.erase(localInputs.begin(), localInputs.lower_bound(oldest));
    remoteInputs.erase(remoteInputs.begin(), remoteInputs.lower_bound(oldest));
}

int16_t Netplay::getInputState(unsigned port, unsigned device, unsigned id) const {
    if (device != RETRO_DEVICE_JOYPAD) return 0;

    uint16_t mask = 0;
    if (port == localPort) mask = currentLocalInput;
    else if (port == remotePort) mask = currentRemoteInput;

    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t) mask;
    if (id < 16) return (mask >> id) & 1;
    return 0;
}

bool Netplay::isChecksumFrame() const {
    return frame > 0 && frame % CHECKSUM_INTERVAL == 0;
}

void Netplay::submitChecksum(uint64_t checksum) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        localChecksums[frame] = checksum;
        compareChecksums(frame);
    }

    std::vector<uint8_t> payload;
    putUint32(payload, frame);
    auto* bytes = reinterpret_cast<const uint8_t*>(&checksum);
    payload.insert(payload.end(), bytes, bytes + sizeof(checksum));
    sendPacket(PACKET_CHECKSUM, payload.data(), payload.size());
}

// Called with the mutex held.
void Netplay::compareChecksums(uint32_t checkedFrame) {
    auto local = localChecksums.find(checkedFrame);
    auto remote = remoteChecksums.find(checkedFrame);
    if (local == localChecksums.end() || remote == remoteChecksums.end()) return;

    if (local->second != remote->second) {
        LOGE("Netplay: states differ at frame %u", checkedFrame);
        if (host) resyncRequested = true;
    }

    localChecksums.erase(localChecksums.begin(), ++local);
    remoteChecksums.erase(remoteChecksums.begin(), ++remote);
}

bool Netplay::consumeResyncRequest() {
    std::lock_guard<std::mutex> lock(mutex);
    bool result = resyncRequested;
    resyncRequested = false;
    return result;
}

void Netplay::sendState(const std::vector<uint8_t>& state) {
    std::vector<uint8_t> payload;
    payload.reserve(4 + state.size());
    putUint32(payload, frame);
    payload.insert(payload.end(), state.begin(), state.end());
    sendPacket(PACKET_STATE, payload.data(), payload.size());
}

std::optional<Netplay::PendingState> Netplay::takePendingState() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!pendingState.has_value() || pendingState->frame > frame) return std::nullopt;
    auto result = std::move(pendingState);
    pendingState.reset();
    return result;
}

void Netplay::restoreFrameInputs(uint32_t replayFrame) {
    std::lock_guard<std::mutex> lock(mutex);
    auto local = localInputs.find(replayFrame);
    auto remote = remoteInputs.find(replayFrame);
    currentLocalInput = local != localInputs.end() ? local->second : 0;
    currentRemoteInput = remote != remoteInputs.end() ? remote->second : 0;
}

uint64_t Netplay::computeChecksum(const std::vector<uint8_t>& data) {
    // FNV-1a
    uint64_t hash = 14695981039346656037ULL;
    for (uint8_t byte : data) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}
