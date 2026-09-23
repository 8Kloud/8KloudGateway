// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "media/srt_output.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstring>

#include <srt/access_control.h>

#include "core/log.h"

namespace kg {
namespace {

// Seven TS packets: the conventional SRT live payload, and the most every
// downstream receiver is guaranteed to accept in one message.
constexpr int kPayload = 1316;

bool setOption(SRTSOCKET socket, SRT_SOCKOPT option, const void* value, int size,
               const char* name, std::string& error) {
    if (srt_setsockopt(socket, 0, option, value, size) != SRT_ERROR) return true;
    error = std::string("SRT output ") + name + ": " + srt_getlasterror_str();
    return false;
}

}  // namespace

std::string srtPeerName(const sockaddr_storage& storage) {
    char host[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;
    if (storage.ss_family == AF_INET) {
        const auto* value = reinterpret_cast<const sockaddr_in*>(&storage);
        inet_ntop(AF_INET, &value->sin_addr, host, sizeof host);
        port = ntohs(value->sin_port);
    } else if (storage.ss_family == AF_INET6) {
        const auto* value = reinterpret_cast<const sockaddr_in6*>(&storage);
        inet_ntop(AF_INET6, &value->sin6_addr, host, sizeof host);
        port = ntohs(value->sin6_port);
    }
    return host[0] ? std::string(host) + ":" + std::to_string(port) : "unknown";
}

bool SrtOutput::start(const ChannelConfig& config, std::string& error) {
    stop();
    stop_ = false;
    listener_ = srt_create_socket();
    if (listener_ == SRT_INVALID_SOCK) {
        error = std::string("SRT output socket: ") + srt_getlasterror_str();
        return false;
    }
    const SRT_TRANSTYPE transport = SRTT_LIVE;
    const int yes = 1, no = 0, payload = kPayload;
    bool ok = setOption(listener_, SRTO_TRANSTYPE, &transport, sizeof transport,
                        "SRTO_TRANSTYPE", error) &&
              setOption(listener_, SRTO_LATENCY, &config.srtOutputLatencyMs,
                        sizeof config.srtOutputLatencyMs, "SRTO_LATENCY", error) &&
              setOption(listener_, SRTO_PAYLOADSIZE, &payload, sizeof payload,
                        "SRTO_PAYLOADSIZE", error) &&
              setOption(listener_, SRTO_SNDSYN, &no, sizeof no, "SRTO_SNDSYN", error) &&
              setOption(listener_, SRTO_REUSEADDR, &yes, sizeof yes, "SRTO_REUSEADDR",
                        error);
    if (ok && !config.srtOutputPassphrase.empty()) {
        ok = setOption(listener_, SRTO_PBKEYLEN, &config.srtOutputPbkeylen,
                       sizeof config.srtOutputPbkeylen, "SRTO_PBKEYLEN", error) &&
             setOption(listener_, SRTO_PASSPHRASE, config.srtOutputPassphrase.data(),
                       static_cast<int>(config.srtOutputPassphrase.size()),
                       "SRTO_PASSPHRASE", error);
    }
    if (!ok) {
        stop();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(config.srtOutputPort));
    if (srt_bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) ==
        SRT_ERROR) {
        error = std::string("SRT output bind :") + std::to_string(config.srtOutputPort) +
                ": " + srt_getlasterror_str();
        stop();
        return false;
    }
    streamId_ = config.srtOutputStreamId;
    if (!streamId_.empty() &&
        srt_listen_callback(listener_, &SrtOutput::listenCallback, this) == SRT_ERROR) {
        error = std::string("SRT output listen callback: ") + srt_getlasterror_str();
        stop();
        return false;
    }
    if (srt_listen(listener_, kMaxClients) == SRT_ERROR) {
        error = std::string("SRT output listen: ") + srt_getlasterror_str();
        stop();
        return false;
    }
    epoll_ = srt_epoll_create();
    int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (epoll_ < 0 || srt_epoll_add_usock(epoll_, listener_, &events) == SRT_ERROR) {
        error = "SRT output epoll setup failed";
        stop();
        return false;
    }
    thread_ = std::thread(&SrtOutput::acceptLoop, this);
    KG_INFO("channel %zu: SRT output listening on :%d", index_ + 1,
            config.srtOutputPort);
    return true;
}

void SrtOutput::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    while (!clients_.empty()) closeClientLocked(clients_.size() - 1, "output stopped");
    if (epoll_ >= 0) srt_epoll_release(epoll_);
    epoll_ = -1;
    if (listener_ != SRT_INVALID_SOCK) srt_close(listener_);
    listener_ = SRT_INVALID_SOCK;
}

// Runs on libsrt's handshake thread before the caller is accepted, so a
// mismatched stream ID is refused during the handshake instead of being
// connected and then dropped.
int SrtOutput::listenCallback(void* opaque, SRTSOCKET socket, int, const sockaddr* peer,
                              const char* streamId) {
    auto* self = static_cast<SrtOutput*>(opaque);
    if (self->streamId_ == (streamId ? streamId : "")) return 0;
    sockaddr_storage storage{};
    if (peer) {
        std::memcpy(&storage, peer,
                    peer->sa_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));
    }
    KG_WARN("channel %zu: SRT output rejected %s: stream ID did not match",
            self->index_ + 1, srtPeerName(storage).c_str());
    srt_setrejectreason(socket, SRT_REJX_FORBIDDEN);
    return -1;
}

void SrtOutput::closeClientLocked(size_t position, const char* reason) {
    KG_INFO("channel %zu: SRT output caller %s disconnected (%s)", index_ + 1,
            clients_[position].peer.c_str(), reason);
    srt_close(clients_[position].socket);
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(position));
}

void SrtOutput::acceptLoop() {
    while (!stop_.load(std::memory_order_relaxed)) {
        SRTSOCKET ready[1]{};
        int count = 1;
        if (srt_epoll_wait(epoll_, ready, &count, nullptr, nullptr, 250, nullptr,
                           nullptr, nullptr, nullptr) > 0) {
            sockaddr_storage peer{};
            int peerLength = sizeof peer;
            const SRTSOCKET socket =
                srt_accept(listener_, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (socket != SRT_INVALID_SOCK) {
                const std::string peerText = srtPeerName(peer);
                std::lock_guard lock(mutex_);
                if (clients_.size() >= kMaxClients) {
                    KG_WARN("channel %zu: SRT output refused %s; %d callers already "
                            "connected", index_ + 1, peerText.c_str(), kMaxClients);
                    srt_close(socket);
                } else {
                    clients_.push_back({socket, peerText});
                    KG_INFO("channel %zu: SRT output caller %s connected", index_ + 1,
                            peerText.c_str());
                }
            }
        }
        // Reap receivers that went away while no contribution data was
        // flowing (send() only notices failures when it has data to send).
        std::lock_guard lock(mutex_);
        for (size_t i = clients_.size(); i-- > 0;) {
            const SRT_SOCKSTATUS state = srt_getsockstate(clients_[i].socket);
            if (state == SRTS_BROKEN || state == SRTS_CLOSED || state == SRTS_NONEXIST)
                closeClientLocked(i, "connection lost");
        }
    }
}

void SrtOutput::send(const char* data, int size) {
    std::lock_guard lock(mutex_);
    if (clients_.empty()) return;
    for (int offset = 0; offset < size; offset += kPayload) {
        const int length = std::min(kPayload, size - offset);
        for (size_t i = clients_.size(); i-- > 0;) {
            if (srt_sendmsg(clients_[i].socket, data + offset, length, -1, 1) != SRT_ERROR) {
                bytesSent_ += static_cast<uint64_t>(length);
                ++packetsSent_;
            } else if (srt_getlasterror(nullptr) == SRT_EASYNCSND) {
                ++packetsDropped_;
            } else {
                closeClientLocked(i, srt_getlasterror_str());
            }
        }
    }
}

SrtOutputStats SrtOutput::stats() {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - rateTime_).count();
    if (seconds >= 0.5) {
        mbps_ = static_cast<double>(bytesSent_ - rateBytes_) * 8.0 / seconds / 1e6;
        rateBytes_ = bytesSent_;
        rateTime_ = now;
    }
    SrtOutputStats result;
    result.clients = static_cast<int>(clients_.size());
    for (const auto& client : clients_) {
        if (!result.peers.empty()) result.peers += ", ";
        result.peers += client.peer;
    }
    result.mbps = mbps_;
    result.packetsSent = packetsSent_;
    result.packetsDropped = packetsDropped_;
    return result;
}

}  // namespace kg
