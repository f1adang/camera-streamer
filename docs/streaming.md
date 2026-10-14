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

- `--fluidnc-url=fluidnc.local` (or an address and port, `http://192.168.1.20`, `192.168.1.20:81`)
- `--fluidnc-name=Testikel` to name it in the box over the picture; without it, the box shows the host name, less `.local`

The page comes in the themes of the Brushograph WebUI, picked at its foot and remembered by the
browser: h4xx0r (the default, glitches and all, full screen included), Default, Dark mode,
Coconut mode, Pinkograph, UwU and Bruschologischer Kongress, which speaks German.

camera-streamer connects to the controller, not the browser, so the page works from
wherever `/stream` does, through a proxy included; only camera-streamer has to reach
the controller. It relays the status reports as server-sent events on
`http://<ip>:8080/fluidnc`:

    data: {"connected":true,"age":120,"status":"<Run|MPos:89.032,122.430,44.991|FS:494,0|SD:65.13,/sd/a.gcode>","wco":"-3.000,-14.000,46.000","error":""}

`status` is the latest report as the controller sent it, and `wco` the work offset from the
last report that carried one, since FluidNC sends it only every few reports. An event comes
with every report, and at least once a second. Behind nginx, `X-Accel-Buffering: no` is sent
so the events are not held back; other proxies need response buffering off for `/fluidnc`.

One websocket to the controller is shared by every viewer, and held only while somebody
watches: FluidNC keeps four websockets and drops the oldest for a fifth. It is closed ten
seconds after the last viewer leaves. While the machine moves, FluidNC sends a report every
200 ms by itself; standing still, camera-streamer asks with `?` after four quiet seconds, and
reconnects after twelve with no answer, or after 2, 5, 10 and then every 30 seconds when the
controller cannot be reached. The WebUI 3 socket (`ws://<host>/`) is used, or WebUI 2's
(`ws://<host>:81/`) when the URL names port 81.

Each viewer of `/telemetry` holds two HTTP connections, `/stream` and `/fluidnc`, out of
`--http-maxcons` (10 by default).

## RTSP server

The camera-streamer implements RTSP server via `live555`. Enable it with:

- adding `--rtsp-port`: will enable RTSP server on 8554
- adding `--rtsp-port=1111`: will enable RTSP server on custom port

The camera-streamer will expose single video stream:

- `rtsp://<ip>:8554/stream.h264` - the resolution is configured with `--camera-video.height`
