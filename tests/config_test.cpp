// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "core/config.h"

namespace {
int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; \
    std::exit(1); } } while (0)

kg::Config load(const std::string& text) {
    std::ofstream output("config_test.json");
    output << text;
    output.close();
    kg::Config config = kg::Config::load("config_test.json");
    std::filesystem::remove("config_test.json");
    return config;
}

void defaults() {
    const auto config = load("{}");
    CHECK(config.channels.size() == 4);
    CHECK(config.channels[0].enabled);
    CHECK(!config.channels[1].enabled);
    CHECK(config.channels[3].port == 9003);
    CHECK(config.channels[0].omtName == "SRT 1");
    CHECK(config.channels[0].omtEnabled);
    CHECK(!config.channels[0].srtOutputEnabled);
    CHECK(config.channels[2].srtOutputPort == 9102);
    CHECK(!config.recording.active);
    CHECK(config.recording.directory == "recordings");
    CHECK(config.web.port == 8080);
}

void validation() {
    kg::ChannelConfig channel;
    std::string error;
    CHECK(kg::Config::validate(channel, error));
    channel.port = 0;
    CHECK(!kg::Config::validate(channel, error));
    channel.port = 9000;
    channel.passphrase = "short";
    CHECK(!kg::Config::validate(channel, error));
    channel.passphrase = "long-enough";
    channel.pbkeylen = 20;
    CHECK(!kg::Config::validate(channel, error));
    channel.pbkeylen = 32;
    channel.decoder = "nvdec";
    CHECK(!kg::Config::validate(channel, error));
    channel.decoder = "cuda";
    CHECK(kg::Config::validate(channel, error));
    channel.omtEnabled = false;
    CHECK(!kg::Config::validate(channel, error));
    channel.srtOutputEnabled = true;
    CHECK(kg::Config::validate(channel, error));
    channel.srtOutputPort = channel.port;
    CHECK(!kg::Config::validate(channel, error));
    channel.srtOutputPort = channel.port + 100;
    channel.srtOutputStreamId.assign(513, 'x');
    CHECK(!kg::Config::validate(channel, error));
    channel.srtOutputStreamId = "relay";
    CHECK(kg::Config::validate(channel, error));
    channel.srtOutputPassphrase = "short";
    CHECK(!kg::Config::validate(channel, error));
    channel.srtOutputPassphrase.clear();
    channel.srtOutputPbkeylen = 20;
    CHECK(!kg::Config::validate(channel, error));
    CHECK(!kg::Config::validateRecording({true, ""}, error));
    CHECK(kg::Config::validateRecording(
        {true, "/var/lib/kloudgateway/recordings"}, error));
}

void sparsePatchAndSecrets() {
    kg::ChannelConfig channel;
    channel.port = 9000;
    channel.omtName = "Program";
    channel.passphrase = "existing-secret";
    std::string error;
    CHECK(kg::Config::patchChannel({{"port", 9100}}, channel, error));
    CHECK(channel.port == 9100);
    CHECK(channel.omtName == "Program");
    CHECK(channel.passphrase == "existing-secret");
    CHECK(kg::Config::patchChannel({{"passphrase", ""}}, channel, error));
    CHECK(channel.passphrase == "existing-secret");
    CHECK(kg::Config::patchChannel({{"clear_passphrase", true}}, channel, error));
    CHECK(channel.passphrase.empty());
    const auto browser = kg::Config::channelJson(channel);
    CHECK(!browser.contains("passphrase"));
    CHECK(!browser.at("encrypted").get<bool>());
    CHECK(!browser.contains("record_mkv"));
    CHECK(!browser.contains("recording_directory"));

    CHECK(kg::Config::patchChannel({{"srt_output_enabled", true},
                                    {"srt_output_port", 9200},
                                    {"omt_enabled", false},
                                    {"srt_output_passphrase", "relay-secret"}},
                                   channel, error));
    CHECK(channel.srtOutputEnabled && !channel.omtEnabled);
    CHECK(kg::Config::patchChannel({{"srt_output_passphrase", ""}}, channel, error));
    CHECK(channel.srtOutputPassphrase == "relay-secret");
    CHECK(!kg::Config::channelJson(channel).contains("srt_output_passphrase"));
    CHECK(kg::Config::channelJson(channel).at("srt_output_encrypted").get<bool>());
    CHECK(kg::Config::channelJson(channel, true).at("srt_output_passphrase") ==
          "relay-secret");
    CHECK(kg::Config::patchChannel({{"clear_srt_output_passphrase", true}}, channel,
                                   error));
    CHECK(channel.srtOutputPassphrase.empty());
    CHECK(!kg::Config::patchChannel({{"srt_output_enabled", false}}, channel, error));
}

void portAndNameConflicts() {
    // Channel 2's SRT output port collides with channel 1's input: disabled.
    auto config = load(R"({"channels":[{"port":9000},
        {"enabled":true,"port":9001,"srt_output_enabled":true,"srt_output_port":9000}]})");
    CHECK(config.channels[0].enabled);
    CHECK(!config.channels[1].enabled);
    // OMT names only need to be unique among channels actually publishing OMT.
    config = load(R"({"channels":[{"omt_name":"Same"},
        {"enabled":true,"omt_name":"Same","omt_enabled":false,"srt_output_enabled":true}]})");
    CHECK(config.channels[1].enabled);
}

void overlayAndPersistence() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("kloudgateway-config-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto old = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    {
        std::ofstream output("boot.json");
        output << R"({"channels":[{"port":9100}],"state_path":"state.json"})";
    }
    {
        std::ofstream output("state.json");
        output << R"({"channels":[{"port":9200}]})";
    }
    kg::Config config = kg::Config::load("boot.json");
    CHECK(config.channels[0].port == 9200);
    config.channels[0].port = 9300;
    config.channels[0].passphrase = "persisted-secret";
    config.channels[0].srtOutputEnabled = true;
    config.channels[0].srtOutputPassphrase = "relay-secret";
    config.channels[0].srtOutputStreamId = "live/relay";
    config.recording = {true, "captures"};
    std::string error;
    CHECK(config.saveState(error));
    CHECK((std::filesystem::status("state.json").permissions() &
           std::filesystem::perms::group_read) == std::filesystem::perms::none);
    config = kg::Config::load("boot.json");
    CHECK(config.channels[0].port == 9300);
    CHECK(config.channels[0].passphrase == "persisted-secret");
    CHECK(config.channels[0].srtOutputEnabled);
    CHECK(config.channels[0].srtOutputPassphrase == "relay-secret");
    CHECK(config.channels[0].srtOutputStreamId == "live/relay");
    CHECK(config.recording.active);
    CHECK(config.recording.directory == "captures");
    std::filesystem::current_path(old);
    std::filesystem::remove_all(directory);
}
}

int main() {
    defaults();
    validation();
    sparsePatchAndSecrets();
    portAndNameConflicts();
    overlayAndPersistence();
    std::cout << checks << " config checks passed\n";
}
