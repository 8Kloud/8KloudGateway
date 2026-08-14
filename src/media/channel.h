#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/config.h"

struct AVBufferRef;

namespace kg {

struct ChannelStatus {
    std::string state = "disabled";
    std::string detail = "disabled";
    std::string peer;
    std::string codec;
    std::string decoder;
    std::string omtAddress;
    bool connected = false;
    int width = 0;
    int height = 0;
    int fpsNum = 0;
    int fpsDen = 1;
    double fps = 0.0;
    double inputMbps = 0.0;
    double srtRttMs = 0.0;
    int64_t srtLost = 0;
    int64_t srtRetransmitted = 0;
    int omtConnections = 0;
    uint64_t framesDecoded = 0;
    uint64_t framesSent = 0;
    uint64_t decodeErrors = 0;
    uint64_t reconnects = 0;
    bool recordingActive = false;
    std::string recordingPath;
    std::string recordingError;
    uint64_t packetsRecorded = 0;
    uint64_t recordingErrors = 0;
};

class Channel {
public:
    Channel(size_t index, ChannelConfig config, AVBufferRef* cudaDevice);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void start();
    void stop();
    void apply(ChannelConfig config);
    void applyRecording(bool enabled, std::string directory);
    ChannelConfig config() const;
    ChannelStatus status() const;
    // Used by FFmpeg's C callbacks and the native libsrt AVIO bridge.
    bool interrupted() const;

private:
    struct RecordingControl {
        bool enabled;
        std::string directory;
        uint64_t generation;
    };

    void run();
    bool runConnection(const ChannelConfig& config, void* omtSender);
    void setStatus(const std::string& state, const std::string& detail);
    void clearSignal(const std::string& detail);
    RecordingControl recordingControl() const;

    size_t index_;
    AVBufferRef* cudaDevice_;
    mutable std::mutex mutex_;
    ChannelConfig config_;
    ChannelStatus status_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> generation_{1};
    uint64_t recordingGeneration_ = 1;
    bool recordingEnabled_ = false;
    std::string recordingDirectory_ = "recordings";
    uint64_t activeGeneration_ = 0;
    std::thread thread_;
};

class ChannelManager {
public:
    ChannelManager(const std::array<ChannelConfig, kChannelCount>& configs,
                   RecordingConfig recording);
    ~ChannelManager();

    bool start(std::string& error);
    void stop();
    bool update(size_t index, const ChannelConfig& config, std::string& error);
    bool setRecording(bool enabled, const std::string& directory,
                      std::string& error);
    std::array<ChannelConfig, kChannelCount> configs() const;
    nlohmann::json statusJson() const;
    bool cudaAvailable() const { return cudaDevice_ != nullptr; }

private:
    AVBufferRef* cudaDevice_ = nullptr;
    bool started_ = false;
    mutable std::mutex recordingMutex_;
    bool recordingEnabled_ = false;
    std::string recordingDirectory_ = "recordings";
    std::array<std::unique_ptr<Channel>, kChannelCount> channels_;
};

}  // namespace kg
