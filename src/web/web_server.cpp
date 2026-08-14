#include "web/web_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "core/log.h"

namespace kg {
namespace {

std::string executableDirectory() {
    char path[PATH_MAX]{};
    const ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
    if (length <= 0) return ".";
    std::string value(path, static_cast<size_t>(length));
    const size_t slash = value.rfind('/');
    return slash == std::string::npos ? "." : value.substr(0, slash);
}

void writeAll(int socket, const std::string& value) {
    size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t count = send(socket, value.data() + offset,
                                   value.size() - offset, MSG_NOSIGNAL);
        if (count <= 0) return;
        offset += static_cast<size_t>(count);
    }
}

void reply(int socket, int code, const char* reason, const char* contentType,
           const std::string& body) {
    std::ostringstream header;
    header << "HTTP/1.1 " << code << ' ' << reason << "\r\n"
           << "Content-Type: " << contentType << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "Connection: close\r\n\r\n";
    writeAll(socket, header.str());
    writeAll(socket, body);
}

std::string contentType(const std::string& path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    if (path.ends_with(".js")) return "application/javascript; charset=utf-8";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".ico")) return "image/x-icon";
    return "application/octet-stream";
}

}  // namespace

WebServer::WebServer(WebConfig config, Hooks hooks)
    : config_(std::move(config)), hooks_(std::move(hooks)) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start(std::string& error) {
    root_ = config_.root.empty() ? executableDirectory() + "/web" : config_.root;
    if (!std::filesystem::is_directory(root_)) {
        error = "web root is not a directory: " + root_;
        return false;
    }
    std::error_code filesystemError;
    recordingBrowseRoot_ = std::filesystem::weakly_canonical(
        std::filesystem::current_path(filesystemError), filesystemError).string();
    if (filesystemError || recordingBrowseRoot_.empty()) {
        error = "cannot resolve recording folder browser root";
        return false;
    }
    listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_ < 0) {
        error = std::string("web socket: ") + std::strerror(errno);
        return false;
    }
    int yes = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(config_.port));
    if (config_.bind.empty() || config_.bind == "0.0.0.0") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, config_.bind.c_str(), &address.sin_addr) != 1) {
        error = "web.bind must be an IPv4 address";
        close(listener_);
        listener_ = -1;
        return false;
    }
    if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) < 0 ||
        listen(listener_, 32) < 0) {
        error = std::string("web bind/listen: ") + std::strerror(errno);
        close(listener_);
        listener_ = -1;
        return false;
    }
    stop_ = false;
    thread_ = std::thread(&WebServer::run, this);
    KG_INFO("web: panel on http://%s:%d (root %s)", config_.bind.c_str(),
            config_.port, root_.c_str());
    return true;
}

void WebServer::stop() {
    stop_ = true;
    if (listener_ >= 0) shutdown(listener_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (listener_ >= 0) close(listener_);
    listener_ = -1;
}

void WebServer::run() {
    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd event{listener_, POLLIN, 0};
        if (poll(&event, 1, 250) <= 0) continue;
        const int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        timeval timeout{2, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        handle(client);
        close(client);
    }
}

std::string WebServer::filePath(const std::string& uri) const {
    if (uri == "/" || uri == "/index.html") return root_ + "/index.html";
    if (uri == "/style.css") return root_ + "/style.css";
    if (uri == "/app.js") return root_ + "/app.js";
    if (uri == "/logo.svg") return root_ + "/logo.svg";
    if (uri == "/favicon.ico") return root_ + "/favicon.ico";
    return {};
}

void WebServer::handle(int client) {
    std::string request;
    request.reserve(8192);
    char chunk[4096];
    size_t headerEnd = std::string::npos;
    size_t contentLength = 0;
    while (request.size() < 64 * 1024) {
        const ssize_t count = recv(client, chunk, sizeof chunk, 0);
        if (count <= 0) break;
        request.append(chunk, static_cast<size_t>(count));
        headerEnd = request.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            const size_t position = request.find("Content-Length:");
            if (position != std::string::npos && position < headerEnd) {
                try {
                    contentLength = std::stoul(request.substr(position + 15));
                } catch (...) {
                    reply(client, 400, "Bad Request", "application/json",
                          R"({"error":"invalid Content-Length"})");
                    return;
                }
            }
            if (contentLength > 60 * 1024) {
                reply(client, 413, "Content Too Large", "application/json",
                      R"({"error":"request body too large"})");
                return;
            }
            if (request.size() >= headerEnd + 4 + contentLength) break;
        }
    }
    const size_t firstSpace = request.find(' ');
    const size_t secondSpace = firstSpace == std::string::npos
                                   ? std::string::npos : request.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        reply(client, 400, "Bad Request", "application/json", R"({"error":"bad request"})");
        return;
    }
    const std::string method = request.substr(0, firstSpace);
    const std::string uri = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);

    if (method == "GET" && uri == "/api/status") {
        try {
            reply(client, 200, "OK", "application/json",
                  hooks_.status ? hooks_.status().dump() : "{}");
        } catch (const std::exception& e) {
            reply(client, 500, "Internal Server Error", "application/json",
                  nlohmann::json{{"error", e.what()}}.dump());
        }
        return;
    }

    if (method == "POST" && uri == "/api/recording") {
        try {
            const std::string body = headerEnd == std::string::npos
                                         ? "" : request.substr(headerEnd + 4, contentLength);
            const nlohmann::json value = nlohmann::json::parse(body);
            const bool active = value.at("active").get<bool>();
            const std::string directory = value.at("directory").get<std::string>();
            std::string error;
            if (!hooks_.recording || !hooks_.recording(active, directory, error)) {
                reply(client, 422, "Unprocessable Content", "application/json",
                      nlohmann::json{{"error", error.empty()
                                                   ? "recording change rejected" : error}}.dump());
                return;
            }
            reply(client, 200, "OK", "application/json",
                  hooks_.status ? hooks_.status().dump() : "{}");
        } catch (const std::exception& e) {
            reply(client, 400, "Bad Request", "application/json",
                  nlohmann::json{{"error", e.what()}}.dump());
        }
        return;
    }

    if (method == "POST" && uri == "/api/recording/directories") {
        try {
            const std::string body = headerEnd == std::string::npos
                                         ? "" : request.substr(headerEnd + 4, contentLength);
            const nlohmann::json value = nlohmann::json::parse(body);
            std::filesystem::path path = value.value("path", "");
            std::error_code error;
            const std::filesystem::path browseRoot(recordingBrowseRoot_);
            if (path.empty()) path = browseRoot;
            if (path.is_relative()) path = browseRoot / path;
            path = std::filesystem::weakly_canonical(path, error);
            if (error) throw std::runtime_error("cannot resolve folder: " + error.message());
            const std::filesystem::path relative = path.lexically_relative(browseRoot);
            if (relative.empty() || (!relative.empty() && *relative.begin() == ".."))
                throw std::runtime_error("folder is outside the gateway working directory");
            if (!std::filesystem::is_directory(path, error)) {
                path = path.parent_path();
                error.clear();
            }
            if (path.empty() || !std::filesystem::is_directory(path, error))
                throw std::runtime_error("folder is not accessible");

            std::vector<std::filesystem::path> directories;
            for (std::filesystem::directory_iterator iterator(
                     path, std::filesystem::directory_options::skip_permission_denied,
                     error), end;
                 iterator != end; iterator.increment(error)) {
                if (error) {
                    error.clear();
                    continue;
                }
                if (iterator->is_directory(error)) directories.push_back(iterator->path());
                error.clear();
            }
            std::sort(directories.begin(), directories.end());
            nlohmann::json items = nlohmann::json::array();
            for (const auto& directory : directories)
                items.push_back({{"name", directory.filename().string()},
                                 {"path", directory.string()}});
            reply(client, 200, "OK", "application/json",
                  nlohmann::json{{"path", path.string()},
                                 {"parent", path == browseRoot
                                                ? path.string()
                                                : path.parent_path().string()},
                                 {"directories", std::move(items)}}.dump());
        } catch (const std::exception& e) {
            reply(client, 400, "Bad Request", "application/json",
                  nlohmann::json{{"error", e.what()}}.dump());
        }
        return;
    }

    constexpr std::string_view prefix = "/api/channels/";
    if ((method == "PUT" || method == "POST") && uri.starts_with(prefix)) {
        try {
            const std::string indexText = uri.substr(prefix.size());
            size_t used = 0;
            const unsigned long index = std::stoul(indexText, &used);
            if (used != indexText.size() || index >= kChannelCount)
                throw std::runtime_error("channel must be 0-3");
            const std::string body = headerEnd == std::string::npos
                                         ? "" : request.substr(headerEnd + 4, contentLength);
            const nlohmann::json patch = nlohmann::json::parse(body);
            std::string error;
            if (!hooks_.apply || !hooks_.apply(index, patch, error)) {
                reply(client, 422, "Unprocessable Content", "application/json",
                      nlohmann::json{{"error", error.empty() ? "change rejected" : error}}.dump());
                return;
            }
            reply(client, 200, "OK", "application/json",
                  hooks_.status ? hooks_.status().dump() : "{}");
        } catch (const std::exception& e) {
            reply(client, 400, "Bad Request", "application/json",
                  nlohmann::json{{"error", e.what()}}.dump());
        }
        return;
    }

    if (method == "GET") {
        const std::string path = filePath(uri);
        std::ifstream input(path, std::ios::binary);
        if (!path.empty() && input) {
            std::ostringstream body;
            body << input.rdbuf();
            reply(client, 200, "OK", contentType(path).c_str(), body.str());
            return;
        }
    }
    reply(client, 404, "Not Found", "application/json", R"({"error":"not found"})");
}

}  // namespace kg
