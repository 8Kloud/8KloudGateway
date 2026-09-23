// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>

#include <srt/srt.h>

#include "core/config.h"

namespace kg {

std::string srtPeerName(const sockaddr_storage& storage);

struct SrtOutputStats {
    int clients = 0;
    std::string peers;
    double mbps = 0.0;
    uint64_t packetsSent = 0;
    uint64_t packetsDropped = 0;
};

// A per-channel SRT listener that republishes the received MPEG-TS byte
// stream, untouched, to every connected downstream caller. Sends are
// non-blocking so a slow or stalled receiver never back-pressures the
// contribution feed; its packets are dropped instead.
class SrtOutput {
public:
    static constexpr int kMaxClients = 8;

    explicit SrtOutput(size_t channelIndex) : index_(channelIndex) {}
    ~SrtOutput() { stop(); }

    SrtOutput(const SrtOutput&) = delete;
    SrtOutput& operator=(const SrtOutput&) = delete;

    bool start(const ChannelConfig& config, std::string& error);
    void stop();
    void send(const char* data, int size);
    SrtOutputStats stats();

private:
    struct Client {
        SRTSOCKET socket;
        std::string peer;
    };

    void acceptLoop();
    void closeClientLocked(size_t position, const char* reason);

    size_t index_;
    SRTSOCKET listener_ = SRT_INVALID_SOCK;
    int epoll_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Client> clients_;
    uint64_t bytesSent_ = 0;
    uint64_t packetsSent_ = 0;
    uint64_t packetsDropped_ = 0;
    uint64_t rateBytes_ = 0;
    std::chrono::steady_clock::time_point rateTime_ = std::chrono::steady_clock::now();
    double mbps_ = 0.0;
};

}  // namespace kg
