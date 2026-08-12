#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include "core/config.h"
#include "core/log.h"
#include "media/channel.h"
#include "web/web_server.h"

namespace {
std::atomic<bool> g_stop{false};
void signalHandler(int) { g_stop = true; }

std::string executableDirectory() {
    char path[4096]{};
    const ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
    if (length <= 0) return ".";
    const std::filesystem::path executable(std::string(path, static_cast<size_t>(length)));
    return executable.parent_path().string();
}
}

int main(int argc, char** argv) {
    std::string configPath = "config/kloudgateway.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) configPath = argv[++i];
        else if (arg == "--debug") kg::setLogLevel(kg::LogLevel::Debug);
        else if (arg == "--help") {
            std::printf("Usage: kloudgateway [--config PATH] [--debug]\n");
            return 0;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }

    kg::Config config;
    try {
        config = kg::Config::load(configPath);
    } catch (const std::exception& e) {
        KG_ERROR("%s", e.what());
        return 1;
    }
    if (config.web.root.empty()) {
        const std::string adjacent = executableDirectory() + "/web";
        config.web.root = std::filesystem::is_directory(adjacent)
                              ? adjacent : KG_INSTALL_WEB_ROOT;
    }

    kg::ChannelManager manager(config.channels);
    std::string error;
    if (!manager.start(error)) {
        KG_ERROR("%s", error.c_str());
        return 1;
    }

    std::mutex configMutex;
    kg::WebServer web(config.web, {
        .status = [&] { return manager.statusJson(); },
        .apply = [&](size_t index, const nlohmann::json& patch, std::string& applyError) {
            std::lock_guard lock(configMutex);
            const kg::ChannelConfig previous = manager.configs()[index];
            kg::ChannelConfig candidate = previous;
            if (!kg::Config::patchChannel(patch, candidate, applyError)) return false;
            if (!manager.update(index, candidate, applyError)) return false;
            config.channels[index] = candidate;
            if (!config.saveState(applyError)) {
                const std::string saveError = applyError;
                config.channels[index] = previous;
                std::string rollbackError;
                if (!manager.update(index, previous, rollbackError))
                    KG_ERROR("channel %zu: rollback failed after state write error: %s",
                             index + 1, rollbackError.c_str());
                applyError = "state was not saved; change rolled back: " + saveError;
                return false;
            }
            KG_INFO("channel %zu: configuration applied", index + 1);
            return true;
        },
    });
    if (!web.start(error)) {
        KG_ERROR("%s", error.c_str());
        manager.stop();
        return 1;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    while (!g_stop.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    KG_INFO("shutdown requested");
    web.stop();
    manager.stop();
    return 0;
}
