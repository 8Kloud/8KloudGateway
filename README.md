# 8Kloud Gateway

8Kloud Gateway accepts as many as four independent MPEG-TS contribution feeds
over SRT and republishes their video as four simultaneous OMT sources. Its web
panel uses the same control language as the other 8Kloud server projects.

```text
 SRT :9000 ─► native libsrt ─► MPEG-TS ─► H.264/HEVC/AV1 decoder ─► UYVY ─► OMT "SRT 1"
 SRT :9001 ─► native libsrt ─► MPEG-TS ─► H.264/HEVC/AV1 decoder ─► UYVY ─► OMT "SRT 2"
 SRT :9002 ─► native libsrt ─► MPEG-TS ─► H.264/HEVC/AV1 decoder ─► UYVY ─► OMT "SRT 3"
 SRT :9003 ─► native libsrt ─► MPEG-TS ─► H.264/HEVC/AV1 decoder ─► UYVY ─► OMT "SRT 4"
```

There is deliberately no NDI code, SDK lookup, configuration, or fallback.
The receiver follows the proven `srt2ndi` shape but calls libsrt itself:
`srt_create_socket` / `srt_accept` / `srt_recvmsg` feed a custom FFmpeg
`AVIOContext`. FFmpeg is used only to demux MPEG-TS and decode H.264, HEVC, or AV1; its
`srt://` protocol wrapper is never used.

## Build

Target dependencies on Ubuntu Server are:

```sh
sudo apt install cmake ninja-build build-essential pkg-config \
  nlohmann-json3-dev libsrt-gnutls-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

Populate `third_party/omt` as described in [docs/omt.md](docs/omt.md), then:

```sh
cmake --preset release -DKLOUDGATEWAY_REQUIRE_APP=ON
cmake --build --preset release -j
ctest --preset release
./build/release/bin/kloudgateway --config config/kloudgateway.json
```

For AV1-in-MPEG-TS, build the pinned FFmpeg receiver first (the distro FFmpeg
currently exposes AV1 TS packets as anonymous data):

```sh
packaging/build-ffmpeg-receiver.sh
PKG_CONFIG_PATH="$PWD/build/ffmpeg-lgpl/lib/pkgconfig" \
  cmake --preset release -DKLOUDGATEWAY_REQUIRE_APP=ON
```

The script applies
[`0001-av1-mpegts-draft.patch`](packaging/patches/ffmpeg/0001-av1-mpegts-draft.patch),
which recognizes the `AV01` registration descriptor and reverses the draft TS
start-code/emulation-prevention representation before the AV1 parser runs.
CMake detects this non-system FFmpeg and includes its four shared libraries in
the private installed runtime directory alongside OMT.

Open `http://server:8080`. Channel 1 listens on UDP 9000 by default; the other
three channels are configured but disabled. A typical caller URL is:

```text
srt://gateway.example:9000?mode=caller&latency=120000&transtype=live
```

The panel can independently enable channels, change ports, require an SRT
stream ID, set AES passphrases, choose CUDA/software decode, and name/quality
each OMT output. An Apply restarts only that channel. Secrets are written to
`gateway_state.json` with mode 0600 and are never returned to the browser.

To install under `/usr` and run it as a locked-down service:

```sh
cmake -S . -B build/install -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DKLOUDGATEWAY_REQUIRE_APP=ON
cmake --build build/install -j
sudo cmake --install build/install
sudo systemd-sysusers
sudo systemd-tmpfiles --create
sudo systemctl enable --now kloudgateway
```

## Runtime behavior

- Each channel has its own listener, decoder, scaler, OMT sender, reconnect
  loop, status, and fault counters. A dead or malformed feed does not disturb
  the other three.
- `auto` decode uses a single shared CUDA device context, as in `srt2ndi`, and
  falls back to FFmpeg's software decoder if CUDA is unavailable. `cuda`
  refuses to stream rather than silently falling back.
- Only H.264, HEVC, and AV1 video streams are admitted. Other MPEG-TS programs are
  rejected after probing. Audio is intentionally ignored in this video-only
  gateway.
- Decoded frames are packed as UYVY 4:2:2 and synchronously handed to libomt,
  which performs the VMX encode. Input presentation times are mapped onto an
  OMT 100 ns monotonic timeline.
- Native `srt_bstats` telemetry drives the panel's receive rate, RTT, loss,
  and retransmission counters.

For AV1, the sender and installed FFmpeg must use the same AOM draft carriage.
An unpatched build reports that AV1-in-TS needs the patched FFmpeg rather than
silently treating it as a supported video stream.
