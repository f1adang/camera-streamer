# Streaming

Camera Streamer exposes a number of streaming capabilities.

## HTTP web server

All streams are exposed over very simple HTTP server, providing different streams for different purposes:

- `http://<ip>:8080/` - index page
- `http://<ip>:8080/snapshot` - provide JPEG snapshot (works well everywhere)
- `http://<ip>:8080/stream` - provide MJPEG stream (works well everywhere)
- `http://<ip>:8080/video` - provide automated video.mp4 or video.hls stream depending on browser used
- `http://<ip>:8080/video.mp4` or `http://<ip>:8080/video.mkv` - provide remuxed `mkv` or `mp4` stream (uses `ffmpeg` to remux, works as of now only in Desktop Chrome and Safari)
- `http://<ip>:8080/webrtc` - provide WebRTC feed
- `http://<ip>:8080/telemetry` - provide MJPEG stream with FluidNC telemetry laid over it

## WebRTC support

The WebRTC is accessible via `http://<ip>:8080/webrtc` by default and is available when there's H264 output generated.

WebRTC support is implemented using awesome [libdatachannel](https://github.com/paullouisageneau/libdatachannel/) library.

The support will be compiled by default when doing `make`.

## FluidNC telemetry

`http://<ip>:8080/telemetry` shows the `/stream` with a [FluidNC](https://github.com/bdring/FluidNC)
controller's state laid over it: Idle, Running, Paused, Alarm and the rest, the feed,
X, Y and Z in work coordinates, and the file being run with how far through it the
controller has read. Give the controller with:

- adding `--fluidnc-url=fluidnc.local` (or an address, `http://192.168.1.20`): every viewer of `/telemetry` watches it; it is also reported by `/status` as `fluidnc.url`
- opening `/telemetry?fluidnc=192.168.1.20`: watches that controller from this page, whatever was given at start-up

The browser talks to the controller, not camera-streamer, so both have to be reachable from it.
It reads the status reports FluidNC sends every websocket while the machine moves, and asks
with `?` only when the machine has gone quiet. The WebUI 3 socket (`ws://<host>/`) is used,
or WebUI 2's (`ws://<host>:81/`) when the URL names port 81. FluidNC keeps four websockets
and drops the oldest for a fifth, so the page reconnects after 2, 5, 10 and then every 30
seconds, and lets go of the socket and the stream while its tab is hidden.

## RTSP server

The camera-streamer implements RTSP server via `live555`. Enable it with:

- adding `--rtsp-port`: will enable RTSP server on 8554
- adding `--rtsp-port=1111`: will enable RTSP server on custom port

The camera-streamer will expose single video stream:

- `rtsp://<ip>:8554/stream.h264` - the resolution is configured with `--camera-video.height`
