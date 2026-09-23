// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "core/config.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.h"

namespace kg {
namespace {
using nlohmann::json;

template <typename T>
void take(const json& value, const char* key, T& out) {
    if (value.contains(key) && !value.at(key).is_null()) out = value.at(key).get<T>();
}

void overlay(const json& root, Config& cfg) {
    if (root.contains("channels") && root.at("channels").is_array()) {
        const auto& channels = root.at("channels");
        for (size_t i = 0; i < std::min(channels.size(), kChannelCount); ++i) {
            const auto& value = channels.at(i);
            auto& channel = cfg.channels[i];
            take(value, "enabled", channel.enabled);
            take(value, "port", channel.port);
            take(value, "latency_ms", channel.latencyMs);
            take(value, "stream_id", channel.streamId);
            take(value, "passphrase", channel.passphrase);
            take(value, "pbkeylen", channel.pbkeylen);
            take(value, "omt_name", channel.omtName);
            take(value, "omt_quality", channel.omtQuality);
            take(value, "decoder", channel.decoder);
            take(value, "omt_enabled", channel.omtEnabled);
            take(value, "srt_output_enabled", channel.srtOutputEnabled);
            take(value, "srt_output_port", channel.srtOutputPort);
            take(value, "srt_output_latency_ms", channel.srtOutputLatencyMs);
            take(value, "srt_output_passphrase", channel.srtOutputPassphrase);
            take(value, "srt_output_pbkeylen", channel.srtOutputPbkeylen);
        }
    }
    if (root.contains("recording")) {
        const auto& value = root.at("recording");
        take(value, "active", cfg.recording.active);
        take(value, "directory", cfg.recording.directory);
    }
    if (root.contains("web")) {
        const auto& value = root.at("web");
        take(value, "bind", cfg.web.bind);
        take(value, "port", cfg.web.port);
        take(value, "root", cfg.web.root);
    }
    take(root, "state_path", cfg.statePath);
}

json parseFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open config: " + path);
    try {
        return json::parse(input, nullptr, true, true);
    } catch (const std::exception& e) {
        throw std::runtime_error("config parse error in " + path + ": " + e.what());
    }
}

bool oneOf(const std::string& value, std::initializer_list<const char*> allowed) {
    return std::any_of(allowed.begin(), allowed.end(), [&](const char* item) {
        return value == item;
    });
}
}  // namespace

Config Config::defaults() {
    Config cfg;
    for (size_t i = 0; i < cfg.channels.size(); ++i) {
        cfg.channels[i].enabled = i == 0;
        cfg.channels[i].port = 9000 + static_cast<int>(i);
        cfg.channels[i].srtOutputPort = 9100 + static_cast<int>(i);
        cfg.channels[i].omtName = "SRT " + std::to_string(i + 1);
    }
    return cfg;
}

Config Config::load(const std::string& path) {
    Config cfg = defaults();
    overlay(parseFile(path), cfg);
    if (!cfg.statePath.empty()) {
        std::ifstream state(cfg.statePath);
        if (state) {
            try {
                overlay(json::parse(state, nullptr, true, true), cfg);
                KG_INFO("config: applied panel state from %s", cfg.statePath.c_str());
            } catch (const std::exception& e) {
                KG_WARN("config: state overlay ignored: %s", e.what());
            }
        }
    }

    if (cfg.web.port < 1 || cfg.web.port > 65535) {
        KG_WARN("config: web port %d invalid; using 8080", cfg.web.port);
        cfg.web.port = 8080;
    }
    std::string recordingError;
    if (!validateRecording(cfg.recording, recordingError)) {
        KG_WARN("config: recording invalid (%s); using stopped/default",
                recordingError.c_str());
        cfg.recording = {};
    }
    std::set<int> ports;
    std::set<std::string> omtNames;
    for (size_t i = 0; i < cfg.channels.size(); ++i) {
        std::string error;
        if (!validate(cfg.channels[i], error)) {
            KG_WARN("config: channel %zu invalid (%s); disabling it", i + 1,
                    error.c_str());
            cfg.channels[i].enabled = false;
        } else if (cfg.channels[i].enabled &&
                   (ports.count(cfg.channels[i].port) ||
                    (cfg.channels[i].srtOutputEnabled &&
                     ports.count(cfg.channels[i].srtOutputPort)))) {
            KG_WARN("config: channel %zu duplicates an enabled SRT port; disabling it",
                    i + 1);
            cfg.channels[i].enabled = false;
        } else if (cfg.channels[i].enabled && cfg.channels[i].omtEnabled &&
                   omtNames.count(cfg.channels[i].omtName)) {
            KG_WARN("config: channel %zu duplicates an enabled OMT name; disabling it",
                    i + 1);
            cfg.channels[i].enabled = false;
        } else if (cfg.channels[i].enabled) {
            ports.insert(cfg.channels[i].port);
            if (cfg.channels[i].srtOutputEnabled) ports.insert(cfg.channels[i].srtOutputPort);
            if (cfg.channels[i].omtEnabled) omtNames.insert(cfg.channels[i].omtName);
        }
    }
    return cfg;
}

bool Config::validate(const ChannelConfig& channel, std::string& error) {
    if (channel.port < 1 || channel.port > 65535) error = "port must be 1-65535";
    else if (channel.latencyMs < 20 || channel.latencyMs > 8000)
        error = "latency_ms must be 20-8000";
    else if (channel.omtName.empty() || channel.omtName.size() > 128)
        error = "omt_name must be 1-128 characters";
    else if (!oneOf(channel.omtQuality, {"default", "low", "medium", "high"}))
        error = "omt_quality must be default, low, medium, or high";
    else if (!oneOf(channel.decoder, {"auto", "cuda", "software"}))
        error = "decoder must be auto, cuda, or software";
    else if (!channel.passphrase.empty() &&
             (channel.passphrase.size() < 10 || channel.passphrase.size() > 79))
        error = "passphrase must be blank or 10-79 characters";
    else if (channel.pbkeylen != 16 && channel.pbkeylen != 24 && channel.pbkeylen != 32)
        error = "pbkeylen must be 16, 24, or 32";
    else if (channel.streamId.size() > 512)
        error = "stream_id is too long";
    else if (!channel.omtEnabled && !channel.srtOutputEnabled)
        error = "enable OMT output, SRT output, or both";
    else if (channel.srtOutputPort < 1 || channel.srtOutputPort > 65535)
        error = "srt_output_port must be 1-65535";
    else if (channel.srtOutputEnabled && channel.srtOutputPort == channel.port)
        error = "srt_output_port must differ from the SRT listen port";
    else if (channel.srtOutputLatencyMs < 20 || channel.srtOutputLatencyMs > 8000)
        error = "srt_output_latency_ms must be 20-8000";
    else if (!channel.srtOutputPassphrase.empty() &&
             (channel.srtOutputPassphrase.size() < 10 ||
              channel.srtOutputPassphrase.size() > 79))
        error = "srt_output_passphrase must be blank or 10-79 characters";
    else if (channel.srtOutputPbkeylen != 16 && channel.srtOutputPbkeylen != 24 &&
             channel.srtOutputPbkeylen != 32)
        error = "srt_output_pbkeylen must be 16, 24, or 32";
    else {
        error.clear();
        return true;
    }
    return false;
}

bool Config::validateRecording(const RecordingConfig& recording,
                               std::string& error) {
    if (recording.directory.empty()) error = "recording directory must not be empty";
    else if (recording.directory.size() > 4096)
        error = "recording directory is too long";
    else if (recording.directory.find('\0') != std::string::npos)
        error = "recording directory contains a null character";
    else {
        error.clear();
        return true;
    }
    return false;
}

bool Config::patchChannel(const nlohmann::json& patch, ChannelConfig& channel,
                          std::string& error) {
    try {
        take(patch, "enabled", channel.enabled);
        take(patch, "port", channel.port);
        take(patch, "latency_ms", channel.latencyMs);
        take(patch, "stream_id", channel.streamId);
        if (patch.contains("passphrase") && !patch.at("passphrase").is_null()) {
            const std::string secret = patch.at("passphrase").get<std::string>();
            if (!secret.empty()) channel.passphrase = secret;
        }
        if (patch.value("clear_passphrase", false)) channel.passphrase.clear();
        take(patch, "pbkeylen", channel.pbkeylen);
        take(patch, "omt_name", channel.omtName);
        take(patch, "omt_quality", channel.omtQuality);
        take(patch, "decoder", channel.decoder);
        take(patch, "omt_enabled", channel.omtEnabled);
        take(patch, "srt_output_enabled", channel.srtOutputEnabled);
        take(patch, "srt_output_port", channel.srtOutputPort);
        take(patch, "srt_output_latency_ms", channel.srtOutputLatencyMs);
        if (patch.contains("srt_output_passphrase") &&
            !patch.at("srt_output_passphrase").is_null()) {
            const std::string secret = patch.at("srt_output_passphrase").get<std::string>();
            if (!secret.empty()) channel.srtOutputPassphrase = secret;
        }
        if (patch.value("clear_srt_output_passphrase", false))
            channel.srtOutputPassphrase.clear();
        take(patch, "srt_output_pbkeylen", channel.srtOutputPbkeylen);
    } catch (const std::exception& e) {
        error = std::string("invalid field type: ") + e.what();
        return false;
    }
    return validate(channel, error);
}

nlohmann::json Config::channelJson(const ChannelConfig& channel, bool includeSecret) {
    json value{{"enabled", channel.enabled},
               {"port", channel.port},
               {"latency_ms", channel.latencyMs},
               {"stream_id", channel.streamId},
               {"encrypted", !channel.passphrase.empty()},
               {"pbkeylen", channel.pbkeylen},
               {"omt_name", channel.omtName},
               {"omt_quality", channel.omtQuality},
               {"decoder", channel.decoder},
               {"omt_enabled", channel.omtEnabled},
               {"srt_output_enabled", channel.srtOutputEnabled},
               {"srt_output_port", channel.srtOutputPort},
               {"srt_output_latency_ms", channel.srtOutputLatencyMs},
               {"srt_output_encrypted", !channel.srtOutputPassphrase.empty()},
               {"srt_output_pbkeylen", channel.srtOutputPbkeylen}};
    if (includeSecret) {
        value["passphrase"] = channel.passphrase;
        value["srt_output_passphrase"] = channel.srtOutputPassphrase;
    }
    return value;
}

bool Config::saveState(std::string& error) const {
    if (!validateRecording(recording, error)) return false;
    json channels = json::array();
    for (const auto& channel : this->channels) {
        if (!validate(channel, error)) return false;
        channels.push_back(channelJson(channel, true));
    }
    json root{{"channels", std::move(channels)},
              {"recording", {{"active", recording.active},
                             {"directory", recording.directory}}}};
    const std::string temporary = statePath + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            error = "cannot write " + temporary;
            return false;
        }
        if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0)
            KG_WARN("config: chmod(%s): %s", temporary.c_str(), std::strerror(errno));
        output << root.dump(2) << '\n';
        if (!output) {
            error = "write failed: " + temporary;
            return false;
        }
    }
    if (std::rename(temporary.c_str(), statePath.c_str()) != 0) {
        error = std::string("rename state: ") + std::strerror(errno);
        ::unlink(temporary.c_str());
        return false;
    }
    return true;
}

}  // namespace kg
