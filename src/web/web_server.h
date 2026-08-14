// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/config.h"

namespace kg {

class WebServer {
public:
    struct Hooks {
        std::function<nlohmann::json()> status;
        std::function<bool(size_t, const nlohmann::json&, std::string&)> apply;
        std::function<bool(bool, const std::string&, std::string&)> recording;
    };

    WebServer(WebConfig config, Hooks hooks);
    ~WebServer();

    bool start(std::string& error);
    void stop();

private:
    void run();
    void handle(int client);
    std::string filePath(const std::string& uri) const;

    WebConfig config_;
    Hooks hooks_;
    std::string root_;
    std::string recordingBrowseRoot_;
    int listener_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace kg
