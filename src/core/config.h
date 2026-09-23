// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <array>
#include <string>

#include <nlohmann/json.hpp>

namespace kg {

constexpr size_t kChannelCount = 4;

struct ChannelConfig {
    bool enabled = false;
    int port = 9000;
    int latencyMs = 120;
    std::string streamId;
    std::string passphrase;
    int pbkeylen = 16;
    std::string omtName = "SRT 1";
    std::string omtQuality = "default";
    std::string decoder = "auto";  // auto, cuda, software
    bool omtEnabled = true;
    // Packet-for-packet SRT listener republishing the received MPEG-TS.
    bool srtOutputEnabled = false;
    int srtOutputPort = 9100;
    int srtOutputLatencyMs = 120;
    std::string srtOutputPassphrase;
    int srtOutputPbkeylen = 16;
};

struct RecordingConfig {
    bool active = false;
    std::string directory = "recordings";
};

struct WebConfig {
    std::string bind = "0.0.0.0";
    int port = 8080;
    std::string root;
};

struct Config {
    std::array<ChannelConfig, kChannelCount> channels;
    RecordingConfig recording;
    WebConfig web;
    std::string statePath = "gateway_state.json";

    static Config defaults();
    static Config load(const std::string& path);
    static bool validate(const ChannelConfig& channel, std::string& error);
    static bool validateRecording(const RecordingConfig& recording,
                                  std::string& error);
    static bool patchChannel(const nlohmann::json& patch, ChannelConfig& channel,
                             std::string& error);
    static nlohmann::json channelJson(const ChannelConfig& channel,
                                      bool includeSecret = false);
    bool saveState(std::string& error) const;
};

}  // namespace kg
