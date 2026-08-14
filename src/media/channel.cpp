#include "media/channel.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include <srt/srt.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include <libomt.h>

#include "core/log.h"

namespace kg {
namespace {

using namespace std::chrono_literals;

std::string avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof text);
    return text;
}

std::string peerName(const sockaddr_storage& storage, int length) {
    (void)length;
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

OMTQuality omtQuality(const std::string& quality) {
    if (quality == "low") return OMTQuality_Low;
    if (quality == "medium") return OMTQuality_Medium;
    if (quality == "high") return OMTQuality_High;
    return OMTQuality_Default;
}

struct Socket {
    SRTSOCKET value = SRT_INVALID_SOCK;
    ~Socket() { if (value != SRT_INVALID_SOCK) srt_close(value); }
    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};

struct Epoll {
    int value = srt_epoll_create();
    ~Epoll() { if (value >= 0) srt_epoll_release(value); }
};

struct FormatCloser {
    void operator()(AVFormatContext* value) const {
        if (value) avformat_close_input(&value);
    }
};
struct CodecCloser {
    void operator()(AVCodecContext* value) const { avcodec_free_context(&value); }
};
struct FrameCloser {
    void operator()(AVFrame* value) const { av_frame_free(&value); }
};
struct PacketCloser {
    void operator()(AVPacket* value) const { av_packet_free(&value); }
};
struct SwsCloser {
    void operator()(SwsContext* value) const { sws_freeContext(value); }
};
struct AvioCloser {
    void operator()(AVIOContext* value) const { avio_context_free(&value); }
};

struct DecodeChoice { AVPixelFormat cudaFormat = AV_PIX_FMT_NONE; };

AVPixelFormat choosePixelFormat(AVCodecContext* context,
                                const AVPixelFormat* formats) {
    const auto* choice = static_cast<const DecodeChoice*>(context->opaque);
    if (choice && choice->cudaFormat != AV_PIX_FMT_NONE) {
        for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
            if (*format == choice->cudaFormat) return *format;
    }
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
        if (*format != AV_PIX_FMT_CUDA) return *format;
    return formats[0];
}

struct IoState {
    SRTSOCKET socket = SRT_INVALID_SOCK;
    Channel* channel = nullptr;
};

int readSrt(void* opaque, uint8_t* buffer, int size) {
    auto* state = static_cast<IoState*>(opaque);
    while (!state->channel->interrupted()) {
        const int count = srt_recvmsg(state->socket, reinterpret_cast<char*>(buffer), size);
        if (count > 0) return count;
        if (count == 0) return AVERROR_EOF;
        if (srt_getlasterror(nullptr) == SRT_EASYNCRCV) continue;
        return AVERROR_EOF;
    }
    return AVERROR_EXIT;
}

int interruptFfmpeg(void* opaque) {
    return static_cast<Channel*>(opaque)->interrupted() ? 1 : 0;
}

bool setSrtOption(SRTSOCKET socket, SRT_SOCKOPT option, const void* value,
                  int size, const char* name, std::string& error) {
    if (srt_setsockopt(socket, 0, option, value, size) != SRT_ERROR) return true;
    error = std::string(name) + ": " + srt_getlasterror_str();
    return false;
}

std::filesystem::path recordingPath(const std::string& directory, size_t index) {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&time, &local);

    std::ostringstream name;
    name << "channel-" << index + 1 << '-' << std::put_time(&local, "%Y%m%d-%H%M%S")
         << '-' << std::setfill('0') << std::setw(3) << milliseconds.count();
    const std::filesystem::path base = std::filesystem::path(directory) / name.str();
    std::filesystem::path candidate = base;
    candidate += ".mkv";
    std::error_code existsError;
    for (unsigned suffix = 1; std::filesystem::exists(candidate, existsError) &&
                              !existsError; ++suffix) {
        candidate = base;
        candidate += "-" + std::to_string(suffix) + ".mkv";
        existsError.clear();
    }
    return candidate;
}

class MkvRecorder {
public:
    ~MkvRecorder() { close(); }

    bool open(AVFormatContext* input, const std::string& directory, size_t index,
              std::string& error) {
        std::error_code filesystemError;
        std::filesystem::create_directories(directory, filesystemError);
        if (filesystemError) {
            error = "create directory '" + directory + "': " +
                    filesystemError.message();
            return false;
        }
        path_ = recordingPath(directory, index).string();

        int rc = avformat_alloc_output_context2(&output_, nullptr, "matroska",
                                                path_.c_str());
        if (rc < 0 || !output_) {
            error = "Matroska context: " + avError(rc < 0 ? rc : AVERROR_UNKNOWN);
            cleanup(false);
            return false;
        }

        streamMap_.assign(input->nb_streams, -1);
        for (unsigned i = 0; i < input->nb_streams; ++i) {
            AVStream* source = input->streams[i];
            if (source->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                source->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                continue;
            AVStream* destination = avformat_new_stream(output_, nullptr);
            if (!destination) {
                error = "Matroska stream allocation failed";
                cleanup(false);
                return false;
            }
            rc = avcodec_parameters_copy(destination->codecpar, source->codecpar);
            if (rc < 0) {
                error = "Matroska codec parameters: " + avError(rc);
                cleanup(false);
                return false;
            }
            destination->codecpar->codec_tag = 0;
            destination->time_base = source->time_base;
            destination->avg_frame_rate = source->avg_frame_rate;
            destination->r_frame_rate = source->r_frame_rate;
            destination->sample_aspect_ratio = source->sample_aspect_ratio;
            destination->disposition = source->disposition;
            av_dict_copy(&destination->metadata, source->metadata, 0);
            streamMap_[i] = destination->index;
        }
        if (output_->nb_streams == 0) {
            error = "MPEG-TS contains no recordable audio or video streams";
            cleanup(false);
            return false;
        }
        av_dict_copy(&output_->metadata, input->metadata, 0);
        output_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
        if (!(output_->oformat->flags & AVFMT_NOFILE)) {
            rc = avio_open(&output_->pb, path_.c_str(), AVIO_FLAG_WRITE);
            if (rc < 0) {
                error = "open '" + path_ + "': " + avError(rc);
                cleanup(false);
                return false;
            }
        }
        rc = avformat_write_header(output_, nullptr);
        if (rc < 0) {
            error = "Matroska header: " + avError(rc);
            cleanup(false);
            return false;
        }
        headerWritten_ = true;
        inputStartTime_ = input->start_time;
        return true;
    }

    int write(AVFormatContext* input, const AVPacket* source) {
        if (!output_ || source->stream_index < 0 ||
            static_cast<size_t>(source->stream_index) >= streamMap_.size())
            return 0;
        const int outputIndex = streamMap_[source->stream_index];
        if (outputIndex < 0) return 0;

        AVPacket* packet = av_packet_clone(source);
        if (!packet) return AVERROR(ENOMEM);
        AVStream* inputStream = input->streams[source->stream_index];
        AVStream* outputStream = output_->streams[outputIndex];
        if (inputStartTime_ != AV_NOPTS_VALUE) {
            const int64_t offset = av_rescale_q(inputStartTime_, AV_TIME_BASE_Q,
                                                inputStream->time_base);
            if (packet->pts != AV_NOPTS_VALUE) packet->pts -= offset;
            if (packet->dts != AV_NOPTS_VALUE) packet->dts -= offset;
        }
        av_packet_rescale_ts(packet, inputStream->time_base, outputStream->time_base);
        packet->stream_index = outputIndex;
        packet->pos = -1;
        const int rc = av_interleaved_write_frame(output_, packet);
        av_packet_free(&packet);
        return rc;
    }

    int close() {
        if (!output_) return 0;
        const int rc = headerWritten_ ? av_write_trailer(output_) : 0;
        cleanup(true);
        return rc;
    }

    const std::string& path() const { return path_; }
    bool active() const { return output_ && headerWritten_; }
    bool accepts(int inputIndex) const {
        return inputIndex >= 0 && static_cast<size_t>(inputIndex) < streamMap_.size() &&
               streamMap_[inputIndex] >= 0;
    }

private:
    void cleanup(bool keepFile) {
        if (!output_) return;
        if (!(output_->oformat->flags & AVFMT_NOFILE) && output_->pb)
            avio_closep(&output_->pb);
        avformat_free_context(output_);
        output_ = nullptr;
        headerWritten_ = false;
        if (!keepFile && !path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    AVFormatContext* output_ = nullptr;
    std::vector<int> streamMap_;
    int64_t inputStartTime_ = AV_NOPTS_VALUE;
    std::string path_;
    bool headerWritten_ = false;
};

}  // namespace

Channel::Channel(size_t index, ChannelConfig config, AVBufferRef* cudaDevice)
    : index_(index), cudaDevice_(cudaDevice), config_(std::move(config)) {}

Channel::~Channel() { stop(); }

void Channel::start() {
    stop_ = false;
    thread_ = std::thread(&Channel::run, this);
}

void Channel::stop() {
    stop_ = true;
    generation_.fetch_add(1, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

void Channel::apply(ChannelConfig config) {
    {
        std::lock_guard lock(mutex_);
        config_ = std::move(config);
    }
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void Channel::applyRecording(bool enabled, std::string directory) {
    std::lock_guard lock(mutex_);
    recordingEnabled_ = enabled;
    recordingDirectory_ = std::move(directory);
    ++recordingGeneration_;
}

ChannelConfig Channel::config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

Channel::RecordingControl Channel::recordingControl() const {
    std::lock_guard lock(mutex_);
    return {recordingEnabled_, recordingDirectory_, recordingGeneration_};
}

ChannelStatus Channel::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

bool Channel::interrupted() const {
    return stop_.load(std::memory_order_relaxed) ||
           generation_.load(std::memory_order_relaxed) != activeGeneration_;
}

void Channel::setStatus(const std::string& state, const std::string& detail) {
    std::lock_guard lock(mutex_);
    status_.state = state;
    status_.detail = detail;
}

void Channel::clearSignal(const std::string& detail) {
    std::lock_guard lock(mutex_);
    status_.connected = false;
    status_.peer.clear();
    status_.codec.clear();
    status_.decoder.clear();
    status_.width = status_.height = status_.fpsNum = 0;
    status_.fpsDen = 1;
    status_.fps = status_.inputMbps = status_.srtRttMs = 0.0;
    status_.omtConnections = 0;
    status_.recordingActive = false;
    status_.state = "waiting";
    status_.detail = detail;
}

void Channel::run() {
    while (!stop_.load(std::memory_order_relaxed)) {
        const ChannelConfig snapshot = config();
        activeGeneration_ = generation_.load(std::memory_order_relaxed);
        if (!snapshot.enabled) {
            clearSignal("disabled");
            setStatus("disabled", "disabled");
            while (!interrupted()) std::this_thread::sleep_for(100ms);
            continue;
        }

        omt_send_t* sender = omt_send_create(snapshot.omtName.c_str(),
                                             omtQuality(snapshot.omtQuality));
        if (!sender) {
            setStatus("error", "OMT sender creation failed");
            KG_ERROR("channel %zu: omt_send_create('%s') failed", index_ + 1,
                     snapshot.omtName.c_str());
            for (int i = 0; i < 10 && !interrupted(); ++i) std::this_thread::sleep_for(100ms);
            continue;
        }
        char address[OMT_MAX_STRING_LENGTH]{};
        omt_send_getaddress(sender, address, sizeof address);
        {
            std::lock_guard lock(mutex_);
            status_.omtAddress = address;
        }
        KG_INFO("channel %zu: OMT sender '%s' ready (%s)", index_ + 1,
                snapshot.omtName.c_str(), address);

        while (!interrupted()) {
            const bool connected = runConnection(snapshot, sender);
            if (interrupted()) break;
            {
                std::lock_guard lock(mutex_);
                if (connected) ++status_.reconnects;
            }
            for (int i = 0; i < 10 && !interrupted(); ++i) std::this_thread::sleep_for(100ms);
        }
        omt_send_destroy(sender);
        {
            std::lock_guard lock(mutex_);
            status_.omtAddress.clear();
        }
    }
}

bool Channel::runConnection(const ChannelConfig& config, void* opaqueSender) {
    auto* sender = static_cast<omt_send_t*>(opaqueSender);
    Socket listener;
    listener.value = srt_create_socket();
    if (listener.value == SRT_INVALID_SOCK) {
        setStatus("error", std::string("SRT socket: ") + srt_getlasterror_str());
        return false;
    }

    std::string error;
    const SRT_TRANSTYPE transport = SRTT_LIVE;
    const int yes = 1, receiveBuffer = 12'058'624, flightWindow = 8192;
    const int payload = 1316;
    if (!setSrtOption(listener.value, SRTO_TRANSTYPE, &transport, sizeof transport,
                      "SRTO_TRANSTYPE", error) ||
        !setSrtOption(listener.value, SRTO_RCVLATENCY, &config.latencyMs,
                      sizeof config.latencyMs, "SRTO_RCVLATENCY", error) ||
        !setSrtOption(listener.value, SRTO_RCVBUF, &receiveBuffer,
                      sizeof receiveBuffer, "SRTO_RCVBUF", error) ||
        !setSrtOption(listener.value, SRTO_FC, &flightWindow, sizeof flightWindow,
                      "SRTO_FC", error) ||
        !setSrtOption(listener.value, SRTO_PAYLOADSIZE, &payload, sizeof payload,
                      "SRTO_PAYLOADSIZE", error) ||
        !setSrtOption(listener.value, SRTO_REUSEADDR, &yes, sizeof yes,
                      "SRTO_REUSEADDR", error)) {
        setStatus("error", error);
        return false;
    }
    if (!config.passphrase.empty()) {
        if (!setSrtOption(listener.value, SRTO_PBKEYLEN, &config.pbkeylen,
                          sizeof config.pbkeylen, "SRTO_PBKEYLEN", error) ||
            !setSrtOption(listener.value, SRTO_PASSPHRASE, config.passphrase.data(),
                          static_cast<int>(config.passphrase.size()),
                          "SRTO_PASSPHRASE", error)) {
            setStatus("error", error);
            return false;
        }
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(config.port));
    if (srt_bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof address) ==
        SRT_ERROR) {
        setStatus("error", std::string("SRT bind: ") + srt_getlasterror_str());
        return false;
    }
    if (srt_listen(listener.value, 1) == SRT_ERROR) {
        setStatus("error", std::string("SRT listen: ") + srt_getlasterror_str());
        return false;
    }
    clearSignal("waiting for SRT caller on :" + std::to_string(config.port));

    Epoll epoll;
    int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (epoll.value < 0 ||
        srt_epoll_add_usock(epoll.value, listener.value, &events) == SRT_ERROR) {
        setStatus("error", "SRT epoll setup failed");
        return false;
    }

    Socket data;
    sockaddr_storage peer{};
    int peerLength = sizeof peer;
    while (!interrupted()) {
        SRTSOCKET ready[1]{};
        int count = 1;
        if (srt_epoll_wait(epoll.value, ready, &count, nullptr, nullptr, 250,
                           nullptr, nullptr, nullptr, nullptr) <= 0)
            continue;
        data.value = srt_accept(listener.value, reinterpret_cast<sockaddr*>(&peer),
                                &peerLength);
        if (data.value != SRT_INVALID_SOCK) break;
    }
    if (data.value == SRT_INVALID_SOCK) return false;

    if (!config.streamId.empty()) {
        char received[513]{};
        int length = 512;
        if (srt_getsockflag(data.value, SRTO_STREAMID, received, &length) == SRT_ERROR ||
            config.streamId != std::string(received, strnlen(received, 512))) {
            setStatus("rejected", "SRT stream ID did not match");
            KG_WARN("channel %zu: rejected caller with mismatched stream ID", index_ + 1);
            return false;
        }
    }
    const int timeout = 200;
    if (!setSrtOption(data.value, SRTO_RCVTIMEO, &timeout, sizeof timeout,
                      "SRTO_RCVTIMEO", error)) {
        setStatus("error", error);
        return false;
    }
    const std::string peerText = peerName(peer, peerLength);
    {
        std::lock_guard lock(mutex_);
        status_.connected = true;
        status_.peer = peerText;
        status_.state = "probing";
        status_.detail = "probing MPEG-TS";
    }
    KG_INFO("channel %zu: SRT caller %s connected", index_ + 1, peerText.c_str());

    IoState io{data.value, this};
    unsigned char* avioBuffer = static_cast<unsigned char*>(av_malloc(65'536));
    if (!avioBuffer) {
        setStatus("error", "AVIO buffer allocation failed");
        return true;
    }
    std::unique_ptr<AVIOContext, AvioCloser> avio(
        avio_alloc_context(avioBuffer, 65'536, 0, &io, readSrt, nullptr, nullptr));
    if (!avio) {
        av_free(avioBuffer);
        setStatus("error", "AVIO context allocation failed");
        return true;
    }

    AVFormatContext* rawFormat = avformat_alloc_context();
    if (!rawFormat) {
        setStatus("error", "demux context allocation failed");
        return true;
    }
    rawFormat->pb = avio.get();
    rawFormat->flags |= AVFMT_FLAG_CUSTOM_IO;
    rawFormat->interrupt_callback = {interruptFfmpeg, this};
    const AVInputFormat* transportStream = av_find_input_format("mpegts");
    int rc = avformat_open_input(&rawFormat, nullptr, transportStream, nullptr);
    if (rc < 0) {
        if (rawFormat) avformat_free_context(rawFormat);
        setStatus("error", "MPEG-TS open: " + avError(rc));
        return true;
    }
    std::unique_ptr<AVFormatContext, FormatCloser> format(rawFormat);
    rc = avformat_find_stream_info(format.get(), nullptr);
    if (rc < 0) {
        setStatus("error", "MPEG-TS probe: " + avError(rc));
        return true;
    }
    const int videoIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO,
                                               -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        bool privateData = false;
        for (unsigned i = 0; i < format->nb_streams; ++i)
            privateData |= format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_DATA;
        setStatus("error", privateData
            ? "no recognized video; AV1-in-TS needs the patched FFmpeg build"
            : "MPEG-TS contains no video stream");
        return true;
    }
    AVStream* stream = format->streams[videoIndex];
    const AVCodecID codecId = stream->codecpar->codec_id;
    if (codecId != AV_CODEC_ID_H264 && codecId != AV_CODEC_ID_HEVC &&
        codecId != AV_CODEC_ID_AV1) {
        setStatus("rejected", "video codec must be H.264, HEVC, or AV1");
        KG_WARN("channel %zu: rejected codec %s", index_ + 1,
                avcodec_get_name(codecId));
        return true;
    }
    const AVCodec* decoder = avcodec_find_decoder(codecId);
    if (!decoder) {
        setStatus("error", "FFmpeg has no decoder for " +
                           std::string(avcodec_get_name(codecId)));
        return true;
    }

    DecodeChoice choice;
    if (config.decoder != "software" && cudaDevice_) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* hardware = avcodec_get_hw_config(decoder, i);
            if (!hardware) break;
            if ((hardware->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                hardware->device_type == AV_HWDEVICE_TYPE_CUDA) {
                choice.cudaFormat = hardware->pix_fmt;
                break;
            }
        }
    }
    if (config.decoder == "cuda" &&
        (!cudaDevice_ || choice.cudaFormat == AV_PIX_FMT_NONE)) {
        setStatus("error", "CUDA decoder requested but unavailable for this codec");
        return true;
    }

    std::unique_ptr<AVCodecContext, CodecCloser> codec(avcodec_alloc_context3(decoder));
    if (!codec) {
        setStatus("error", "decoder context allocation failed");
        return true;
    }
    avcodec_parameters_to_context(codec.get(), stream->codecpar);
    codec->opaque = &choice;
    codec->get_format = choosePixelFormat;
    if (choice.cudaFormat != AV_PIX_FMT_NONE)
        codec->hw_device_ctx = av_buffer_ref(cudaDevice_);
    rc = avcodec_open2(codec.get(), decoder, nullptr);
    if (rc < 0 && config.decoder == "auto" && choice.cudaFormat != AV_PIX_FMT_NONE) {
        KG_WARN("channel %zu: CUDA decoder open failed (%s), falling back to software",
                index_ + 1, avError(rc).c_str());
        codec.reset(avcodec_alloc_context3(decoder));
        if (!codec) {
            setStatus("error", "software decoder context allocation failed");
            return true;
        }
        choice.cudaFormat = AV_PIX_FMT_NONE;
        avcodec_parameters_to_context(codec.get(), stream->codecpar);
        codec->opaque = &choice;
        codec->get_format = choosePixelFormat;
        rc = avcodec_open2(codec.get(), decoder, nullptr);
    }
    if (rc < 0) {
        setStatus("error", "decoder open: " + avError(rc));
        return true;
    }

    std::unique_ptr<AVPacket, PacketCloser> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameCloser> decoded(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameCloser> host(av_frame_alloc());
    if (!packet || !decoded || !host) {
        setStatus("error", "frame allocation failed");
        return true;
    }

    MkvRecorder recorder;
    uint64_t appliedRecordingGeneration = 0;
    auto closeRecorder = [&] {
        if (!recorder.active()) return;
        const int trailerRc = recorder.close();
        std::string trailerError;
        if (trailerRc < 0)
            trailerError = "Matroska trailer: " + avError(trailerRc);
        {
            std::lock_guard lock(mutex_);
            status_.recordingActive = false;
            if (!trailerError.empty()) {
                status_.recordingError = trailerError;
                ++status_.recordingErrors;
            }
        }
        if (!trailerError.empty())
            KG_WARN("channel %zu: %s", index_ + 1, trailerError.c_str());
        else
            KG_INFO("channel %zu: finalized recording %s", index_ + 1,
                    recorder.path().c_str());
    };
    auto syncRecorder = [&] {
        const RecordingControl control = recordingControl();
        if (control.generation == appliedRecordingGeneration) return;
        appliedRecordingGeneration = control.generation;
        closeRecorder();
        if (!control.enabled) {
            std::lock_guard lock(mutex_);
            status_.recordingActive = false;
            status_.recordingError.clear();
            return;
        }

        std::string recordingError;
        if (recorder.open(format.get(), control.directory, index_, recordingError)) {
            {
                std::lock_guard lock(mutex_);
                status_.recordingActive = true;
                status_.recordingPath = recorder.path();
                status_.recordingError.clear();
            }
            KG_INFO("channel %zu: recording original packets to %s", index_ + 1,
                    recorder.path().c_str());
        } else {
            {
                std::lock_guard lock(mutex_);
                status_.recordingActive = false;
                status_.recordingError = recordingError;
                ++status_.recordingErrors;
            }
            KG_WARN("channel %zu: recording unavailable: %s", index_ + 1,
                    recordingError.c_str());
        }
    };
    syncRecorder();
    SwsContext* rawScaler = nullptr;
    std::vector<uint8_t> uyvy;
    int outputWidth = 0, outputHeight = 0;
    AVPixelFormat inputFormat = AV_PIX_FMT_NONE;
    AVRational rate = av_guess_frame_rate(format.get(), stream, nullptr);
    if (rate.num <= 0 || rate.den <= 0) rate = AVRational{30000, 1001};
    const int64_t startTime = av_gettime_relative();
    int64_t firstPts = AV_NOPTS_VALUE;
    int64_t firstOmt = startTime / 100;
    int64_t lastOmt = firstOmt - 1;
    uint64_t windowFrames = 0;
    auto windowStart = std::chrono::steady_clock::now();
    auto lastStats = windowStart;
    {
        std::lock_guard lock(mutex_);
        status_.codec = avcodec_get_name(codecId);
        status_.decoder = choice.cudaFormat == AV_PIX_FMT_NONE ? "software" : "cuda";
        status_.state = "streaming";
        status_.detail = "streaming to OMT";
    }

    while (!interrupted()) {
        syncRecorder();
        rc = av_read_frame(format.get(), packet.get());
        if (rc == AVERROR(EAGAIN)) continue;
        if (rc < 0) break;
        if (recorder.active() && recorder.accepts(packet->stream_index)) {
            const int recordRc = recorder.write(format.get(), packet.get());
            if (recordRc < 0) {
                const std::string recordingError =
                    "Matroska packet write: " + avError(recordRc);
                recorder.close();
                {
                    std::lock_guard lock(mutex_);
                    status_.recordingActive = false;
                    status_.recordingError = recordingError;
                    ++status_.recordingErrors;
                }
                KG_WARN("channel %zu: %s", index_ + 1, recordingError.c_str());
            } else {
                std::lock_guard lock(mutex_);
                ++status_.packetsRecorded;
            }
        }
        if (packet->stream_index != videoIndex) {
            av_packet_unref(packet.get());
            continue;
        }
        rc = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            std::lock_guard lock(mutex_);
            ++status_.decodeErrors;
            continue;
        }

        while (!interrupted()) {
            rc = avcodec_receive_frame(codec.get(), decoded.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) {
                std::lock_guard lock(mutex_);
                ++status_.decodeErrors;
                break;
            }
            AVFrame* frame = decoded.get();
            if (decoded->format == choice.cudaFormat && choice.cudaFormat != AV_PIX_FMT_NONE) {
                av_frame_unref(host.get());
                rc = av_hwframe_transfer_data(host.get(), decoded.get(), 0);
                if (rc < 0) {
                    std::lock_guard lock(mutex_);
                    ++status_.decodeErrors;
                    av_frame_unref(decoded.get());
                    continue;
                }
                av_frame_copy_props(host.get(), decoded.get());
                frame = host.get();
            }
            const auto pixelFormat = static_cast<AVPixelFormat>(frame->format);
            if (frame->width != outputWidth || frame->height != outputHeight ||
                pixelFormat != inputFormat) {
                outputWidth = frame->width;
                outputHeight = frame->height;
                inputFormat = pixelFormat;
                rawScaler = sws_getCachedContext(rawScaler, outputWidth, outputHeight,
                                                 inputFormat, outputWidth, outputHeight,
                                                 AV_PIX_FMT_UYVY422, SWS_FAST_BILINEAR,
                                                 nullptr, nullptr, nullptr);
                if (!rawScaler) {
                    setStatus("error", "pixel conversion setup failed");
                    av_frame_unref(decoded.get());
                    break;
                }
                uyvy.resize(static_cast<size_t>(outputWidth) * outputHeight * 2);
                std::lock_guard lock(mutex_);
                status_.width = outputWidth;
                status_.height = outputHeight;
                status_.fpsNum = rate.num;
                status_.fpsDen = rate.den;
            }
            uint8_t* destination[] = {uyvy.data()};
            int destinationStride[] = {outputWidth * 2};
            sws_scale(rawScaler, frame->data, frame->linesize, 0, outputHeight,
                      destination, destinationStride);

            int64_t pts = frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = frame->pts;
            if (firstPts == AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE) firstPts = pts;
            int64_t omtTimestamp = firstOmt;
            if (pts != AV_NOPTS_VALUE && firstPts != AV_NOPTS_VALUE) {
                omtTimestamp += av_rescale_q(pts - firstPts, stream->time_base,
                                             AVRational{1, 10'000'000});
            } else {
                omtTimestamp = av_gettime_relative() / 100;
            }
            // A discontinuity or TS timestamp wrap must not send OMT time
            // backwards. One 100 ns tick is enough; the following good PTS
            // resumes the source timeline without inventing a frame delay.
            omtTimestamp = std::max(omtTimestamp, lastOmt + 1);
            lastOmt = omtTimestamp;

            OMTMediaFrame output{};
            output.Type = OMTFrameType_Video;
            output.Codec = OMTCodec_UYVY;
            output.Timestamp = omtTimestamp;
            output.Width = outputWidth;
            output.Height = outputHeight;
            output.Stride = outputWidth * 2;
            output.Flags = (frame->flags & AV_FRAME_FLAG_INTERLACED)
                               ? OMTVideoFlags_Interlaced : OMTVideoFlags_None;
            output.FrameRateN = rate.num;
            output.FrameRateD = rate.den;
            output.AspectRatio = static_cast<float>(outputWidth) / outputHeight;
            output.ColorSpace = outputHeight < 720 ? OMTColorSpace_BT601
                                                   : OMTColorSpace_BT709;
            output.Data = uyvy.data();
            output.DataLength = static_cast<int>(uyvy.size());
            const int sent = omt_send(sender, &output);
            ++windowFrames;
            {
                std::lock_guard lock(mutex_);
                ++status_.framesDecoded;
                if (sent) ++status_.framesSent;
                status_.omtConnections = omt_send_connections(sender);
            }
            av_frame_unref(host.get());
            av_frame_unref(decoded.get());

            const auto now = std::chrono::steady_clock::now();
            if (now - windowStart >= 1s) {
                const double seconds = std::chrono::duration<double>(now - windowStart).count();
                std::lock_guard lock(mutex_);
                status_.fps = windowFrames / seconds;
                windowFrames = 0;
                windowStart = now;
            }
            if (now - lastStats >= 1s) {
                SRT_TRACEBSTATS transportStats{};
                if (srt_bstats(data.value, &transportStats, 1) == 0) {
                    std::lock_guard lock(mutex_);
                    status_.inputMbps = transportStats.mbpsRecvRate;
                    status_.srtRttMs = transportStats.msRTT;
                    status_.srtLost += transportStats.pktRcvLoss;
                    status_.srtRetransmitted += transportStats.pktRcvRetrans;
                }
                lastStats = now;
            }
        }
    }
    closeRecorder();
    if (rawScaler) sws_freeContext(rawScaler);
    if (!interrupted()) clearSignal("SRT disconnected; waiting to rebind");
    KG_INFO("channel %zu: SRT caller %s disconnected", index_ + 1,
            peerText.c_str());
    return true;
}

ChannelManager::ChannelManager(
    const std::array<ChannelConfig, kChannelCount>& configs,
    RecordingConfig recording) {
    recordingEnabled_ = recording.active;
    recordingDirectory_ = std::move(recording.directory);
    const int rc = av_hwdevice_ctx_create(&cudaDevice_, AV_HWDEVICE_TYPE_CUDA,
                                          nullptr, nullptr, 0);
    if (rc < 0) {
        cudaDevice_ = nullptr;
        KG_WARN("decoder: shared CUDA device unavailable (%s); auto uses software",
                avError(rc).c_str());
    } else {
        KG_INFO("decoder: shared CUDA device ready for all channels");
    }
    for (size_t i = 0; i < channels_.size(); ++i) {
        channels_[i] = std::make_unique<Channel>(i, configs[i], cudaDevice_);
        channels_[i]->applyRecording(recordingEnabled_, recordingDirectory_);
    }
}

ChannelManager::~ChannelManager() {
    stop();
    av_buffer_unref(&cudaDevice_);
}

bool ChannelManager::start(std::string& error) {
    if (started_) return true;
    if (srt_startup() != 0) {
        error = std::string("srt_startup: ") + srt_getlasterror_str();
        return false;
    }
    for (auto& channel : channels_) channel->start();
    started_ = true;
    return true;
}

void ChannelManager::stop() {
    if (!started_) return;
    for (auto& channel : channels_) if (channel) channel->stop();
    srt_cleanup();
    started_ = false;
}

bool ChannelManager::update(size_t index, const ChannelConfig& config,
                            std::string& error) {
    if (index >= channels_.size()) {
        error = "channel index is out of range";
        return false;
    }
    if (!Config::validate(config, error)) return false;
    for (size_t i = 0; i < channels_.size(); ++i) {
        if (i != index && config.enabled && channels_[i]->config().enabled &&
            channels_[i]->config().port == config.port) {
            error = "another enabled channel already uses this SRT port";
            return false;
        }
        if (i != index && config.enabled && channels_[i]->config().enabled &&
            channels_[i]->config().omtName == config.omtName) {
            error = "another enabled channel already uses this OMT name";
            return false;
        }
    }
    channels_[index]->apply(config);
    return true;
}

bool ChannelManager::setRecording(bool enabled, const std::string& directory,
                                  std::string& error) {
    if (!Config::validateRecording({enabled, directory}, error)) return false;
    bool directoryChanged;
    {
        std::lock_guard lock(recordingMutex_);
        directoryChanged = directory != recordingDirectory_;
    }
    if (enabled || directoryChanged) {
        std::error_code filesystemError;
        std::filesystem::create_directories(directory, filesystemError);
        if (filesystemError || !std::filesystem::is_directory(directory)) {
            error = "recording directory '" + directory + "' is unavailable";
            if (filesystemError) error += ": " + filesystemError.message();
            return false;
        }
    }

    for (auto& channel : channels_)
        channel->applyRecording(enabled, directory);
    {
        std::lock_guard lock(recordingMutex_);
        recordingEnabled_ = enabled;
        recordingDirectory_ = directory;
    }
    KG_INFO("recording: %s for all channels (directory %s)",
            enabled ? "started" : "stopped", directory.c_str());
    return true;
}

std::array<ChannelConfig, kChannelCount> ChannelManager::configs() const {
    std::array<ChannelConfig, kChannelCount> result;
    for (size_t i = 0; i < result.size(); ++i) result[i] = channels_[i]->config();
    return result;
}

nlohmann::json ChannelManager::statusJson() const {
    nlohmann::json channels = nlohmann::json::array();
    for (size_t i = 0; i < channels_.size(); ++i) {
        const ChannelConfig config = channels_[i]->config();
        const ChannelStatus status = channels_[i]->status();
        nlohmann::json value = Config::channelJson(config);
        value.update({{"index", i},
                      {"state", status.state},
                      {"detail", status.detail},
                      {"connected", status.connected},
                      {"peer", status.peer},
                      {"codec", status.codec},
                      {"active_decoder", status.decoder},
                      {"omt_address", status.omtAddress},
                      {"width", status.width},
                      {"height", status.height},
                      {"fps_num", status.fpsNum},
                      {"fps_den", status.fpsDen},
                      {"fps", status.fps},
                      {"input_mbps", status.inputMbps},
                      {"srt_rtt_ms", status.srtRttMs},
                      {"srt_lost", status.srtLost},
                      {"srt_retransmitted", status.srtRetransmitted},
                      {"omt_connections", status.omtConnections},
                      {"frames_decoded", status.framesDecoded},
                      {"frames_sent", status.framesSent},
                      {"decode_errors", status.decodeErrors},
                      {"reconnects", status.reconnects},
                      {"recording_active", status.recordingActive},
                      {"recording_path", status.recordingPath},
                      {"recording_error", status.recordingError},
                      {"packets_recorded", status.packetsRecorded},
                      {"recording_errors", status.recordingErrors}});
        channels.push_back(std::move(value));
    }
    bool recordingEnabled;
    std::string recordingDirectory;
    {
        std::lock_guard lock(recordingMutex_);
        recordingEnabled = recordingEnabled_;
        recordingDirectory = recordingDirectory_;
    }
    return {{"channels", std::move(channels)},
            {"recording", {{"active", recordingEnabled},
                           {"directory", recordingDirectory}}},
            {"capabilities", {{"srt", "native libsrt"},
                              {"codecs", {"h264", "hevc", "av1"}},
                              {"output", "omt"},
                              {"recording", "mkv-remux"},
                              {"cuda", cudaDevice_ != nullptr}}}};
}

}  // namespace kg
