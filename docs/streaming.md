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
- `http://<ip>:8080/telemetry` - provide MJPEG stream with the telemetry of one or more FluidNC controllers laid over it

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

Several controllers, a Brushograph each, are watched at once by listing them, separated by
commas or by giving the option again, up to eight; the names go in the same order, and one
left empty is the host name:

- `--fluidnc-url=testikel.local,brushograph-2.local,192.168.1.30:81`
- `--fluidnc-name=Testikel,,Pinky`

Each has a box of its own, side by side over the picture's bottom left corner, lettered
smaller the more there are, and one under the other when the picture is narrow. A
controller that cannot be reached says why in its box, and the others carry on.

The page comes in the themes of the Brushograph WebUI, picked at its foot and remembered by the
browser: h4xx0r (the default, glitches and all, full screen included), Default, Dark mode,
Coconut mode, Pinkograph, UwU and Bruschologischer Kongress, which speaks German.

camera-streamer connects to the controller, not the browser, so the page works from
wherever `/stream` does, through a proxy included; only camera-streamer has to reach
the controller. It relays the status reports as server-sent events on
`http://<ip>:8080/fluidnc`:

    data: {"viewers":2,"machines":[{"connected":true,"age":120,"status":"<Run|MPos:89.032,122.430,44.991|FS:494,0|SD:65.13,/sd/a.gcode>","wco":"-3.000,-14.000,46.000","ov":"100,100,100","board":"MKS DLC32 V2.1","firmware":"FluidNC v3.7.8","error":""}],"server":{"model":"Raspberry Pi Zero 2 W Rev 1.0","soc":"BCM2837","cpu_cores":4,"cpu":"Cortex-A53","cpu_max_mhz":1000,"ram_mb":512,"camera":"Sony IMX219","throttled":0,"temp":52.1,…}}

`viewers` is the number of `/fluidnc` responses open, that is, of `/telemetry` pages being
looked at (a hidden tab lets go), and is sent with or without a controller. `machines` has
one entry a controller, in the order of `--fluidnc-url`, which is the order `/status` lists
them in under `fluidnc`, each with its `url` and `name`; it is empty without one. Of each,
`status` is the latest report as the controller sent it, and `wco` and `ov` the work offset and the overrides
from the last report that carried them, since FluidNC sends those only every few reports.
`board` and `firmware` are what the controller is, shown under its name: `$/board` from its
config and the firmware from `$I`, asked once a connection, and again every thirty seconds
the controller is idle until it has answered, since FluidNC refuses them during a job.
`battery` and `mobile` are the Victron battery monitor and the Teltonika router below, or
`null` without them. `server` is the health of the host camera-streamer runs on, shown in a
box over the picture's top left corner (under the picture, when it is narrow) as "Brušocam
server": the board's model from the device tree, its `soc`, `cpu_cores`, `cpu` (from the CPU
part in `/proc/cpuinfo`), `cpu_max_mhz` and `ram_mb` (from the Raspberry Pi's revision code),
the `camera`'s sensor ("Sony IMX219", from its V4L2 subdevice, or a USB camera's own name),
the Raspberry Pi firmware's `throttled` flags
(under-voltage, clock capped, throttled, soft temperature limit, now and since boot) or, on
kernels without them, the `undervoltage` alarm, the CPU temperature and clock, the load,
memory, the Wi-Fi signal in dBm, the bytes sent per second on every interface but the
loopback, the stream's frames per second, and the uptime. They are read from `/proc` and
`/sys` at most once a second; what the host does not have is `null`. An event comes
with every report, and at least once a second. Behind nginx, `X-Accel-Buffering: no` is sent
so the events are not held back; other proxies need response buffering off for `/fluidnc`.

One websocket to each controller is shared by every viewer, and held only while somebody
watches: FluidNC keeps four websockets and drops the oldest for a fifth. It is closed ten
seconds after the last viewer leaves. While the machine moves, FluidNC sends a report every
200 ms by itself; standing still, camera-streamer asks with `?` after four quiet seconds, and
reconnects after twelve with no answer, or after 2, 5, 10 and then every 30 seconds when the
controller cannot be reached. The WebUI 3 socket (`ws://<host>/`) is used, or WebUI 2's
(`ws://<host>:81/`) when the URL names port 81.

Each viewer of `/telemetry` holds two HTTP connections, `/stream` and `/fluidnc`, out of
`--http-maxcons` (10 by default), however many controllers there are.

### Victron battery monitor or solar charger

`/telemetry` can show a Victron battery monitor (a SmartShunt, a BMV) or solar charger (a
SmartSolar or BlueSolar MPPT), or either on a VE.Direct Bluetooth Smart dongle, in a box over
the picture's bottom right corner (under the picture, when it is narrow), titled after it,
"Rande Stats" for `--victron-name=Rande`. For a battery monitor: the state of charge,
voltage, current, power, Ah consumed, time to go, the aux input (battery temperature, starter
battery or midpoint) and any alarm. For a solar charger: its charge state (bulk, absorption,
float …), the battery's voltage and current, the solar power, today's yield, the load output
and any charger error ("Err 2"). For either, the Bluetooth signal. camera-streamer hears it
over Bluetooth through Instant Readout, which the device puts, encrypted, into every
advertisement: no connection or pairing, and the VictronConnect app can still connect to it.
Turn Instant readout via Bluetooth on in VictronConnect, and copy the MAC address and the
encryption key from Product info, Instant readout details:

- `--victron-mac=d4:12:34:56:78:9a` (the colons may be left out)
- `--victron-key=<32 hex digits>`; it changes when the Bluetooth PIN is changed or reset
- `--victron-name=Rande` to title the box; "Battery" without it

The key is on the command line, so anybody who can log in to the host can read it.

It is relayed with each `/fluidnc` event:

    "battery":{"name":"Rande","model":"SmartShunt 500A/50mV","error":"","age":958,"rssi":-72,"soc":66.9,"voltage":12.85,"current":-2.336,"consumed_ah":41.4,"ttg_min":4445,"alarm":0,"temperature":null,"starter_voltage":null,"midpoint_voltage":null}

`kind` is `battery_monitor` or `solar_charger`; a solar charger's are `charge_state` and
`charger_error`, as Victron numbers them, `yield_wh`, `pv_w` and `load_a`, beside `voltage`
and `current`, and what a battery monitor would give is `null`. `age` is how many
milliseconds ago the figures were heard, `null` until they are; the page
says so once they are ten seconds old. `current` is positive while charging. `alarm` is the
monitor's alarm reasons, a bit each. What the monitor does not know is `null`, and its row is
not shown. `error` says what is wrong: no Bluetooth adapter, a key that does not fit, or a
Victron device that is not a battery monitor.

The advertisements are heard through BlueZ over D-Bus, so `bluetoothd` must be running with
the adapter (`hci0`) powered, and camera-streamer must be allowed to talk to `org.bluez`,
which on Debian and Raspberry Pi OS any user of the system is. Not one that systemd's
`DynamicUser=` makes up, though: the system bus cannot look it up, and drops the connection
("The system bus would not talk to camera-streamer's user"). Run it as a system user instead,
made once with `useradd --system --no-create-home --groups video,i2c camera-streamer`, with
`User=camera-streamer` in place of `DynamicUser=yes`. An adapter that is off is turned on: on a
Raspberry Pi Zero 2 W, `bluetoothd` tries to at boot before the chip is ready ("Failed to set
mode") and leaves it off. It scans only while somebody watches,
and stops ten seconds after the last viewer leaves: a Raspberry Pi's Wi-Fi and Bluetooth
share a radio, and a scan takes time from the stream. With nothing heard for a minute it
starts the scan over, as it does after `bluetoothd` restarts. It is built in when `make`
finds `libsystemd` and `libcrypto` with `pkg-config` (`apt install pkg-config libsystemd-dev
libssl-dev`); without them, `make` says so, and the page says the build has no Bluetooth.
Installing them later and running `make` again is enough.

### Teltonika router

The same box can show the mobile connection of a Teltonika router on RutOS 7, such as a
TRB500: the operator (and whether it is roaming), the network (5G-NSA, LTE …, or not
connected), the signal as RSRP, the SINR, the 5G cell's RSRP in 5G NSA, the band, the data
received and sent since the modem connected, and the modem's temperature. Without a battery
monitor the box is "Mobile Stats". Over its figures it names the router and its modem,
"Teltonika TRB500 · Quectel RG501Q-EU", from `GET /api/system/device/status`, asked once,
and the modem status; over the battery's, "Victron Energy" and the monitor's model.

- `--teltonika-url=http://192.168.2.1` (plain HTTP; the router's WebUI must answer on it)
- `--teltonika-user=admin`, the default
- `--teltonika-password=<password>`; like the battery's key, readable on the host from the
  command line

camera-streamer logs in to the router's API (`POST /api/login`) and asks for
`GET /api/modems/status` every five seconds while somebody watches, with the token until it
runs out, logging in again when it has or when the router has forgotten it. After a failure,
a refused login included, it waits thirty seconds, so that a wrong password does not lock the
user out. It is relayed with each `/fluidnc` event:

    "mobile":{"age":3847,"error":"","modem":"Internal modem","modem_manufacturer":"Quectel","modem_model":"RG501Q-EU","router_model":"TRB500","operator":"Swisscom","conntype":"5G-NSA","ntype":"5G (NSA)","state":"Connected","netstate":"Registered, home","simstate":"Inserted","band":"LTE B3","rssi":-61,"rsrp":-95,"rsrq":-11,"sinr":11,"nr_rsrp":-101,"nr_sinr":6,"signal_quality":72,"temperature":47,"rxbytes":1839567890,"txbytes":414145678}

The figures are the first modem that is online, as the router names them, made numbers where
the router sends them as strings; `nr_rsrp` and `nr_sinr` are the 5G cell's, from its
`cell_info`. `age` and `error` are as for the battery.

### TP-Link Wi-Fi Access Point

The box can also show the Wi-Fi access point and connected client stats of a TP-Link router, such as the
Archer MR402-Outdoor (AC1200 Wireless Dual Band 4G LTE Outdoor Router) used as an AP: the Wi-Fi network (SSID),
the 2.4 GHz and 5 GHz channels, the number of connected clients (with breakdown by band), router uptime, and the list of
connected client devices (names, IPs, MAC addresses, bands and packet counters):

- `--tplink-url=http://192.168.1.1` (plain HTTP; the router's WebUI must answer on it)
- `--tplink-user=admin`, the default
- `--tplink-password=<password>`

camera-streamer authenticates using the router's encrypted API (secp224r1 ECC key exchange and
AES-128-CBC encryption) and queries AP status and connected clients every five seconds while somebody watches, logging
in again when the session expires. It is relayed as `ap` with each `/fluidnc` event:

    "ap":{"model":"TP-Link Archer MR402-Outdoor","description":"AC1200 Wireless Dual Band 4G LTE Outdoor Router","ssid":"RandeLab","channel_2g":1,"channel_5g":36,"clients_count":7,"clients_2g":3,"clients_5g":4,"uptime":432000,"clients":[{"mac":"AA:BB:CC:DD:EE:FF","ip":"192.168.1.100","name":"Pixel-9a","band":"5GHz","packets_sent":1234,"packets_recv":5678}],"age":1240,"error":""}

## RTSP server

The camera-streamer implements RTSP server via `live555`. Enable it with:

- adding `--rtsp-port`: will enable RTSP server on 8554
- adding `--rtsp-port=1111`: will enable RTSP server on custom port

The camera-streamer will expose single video stream:

- `rtsp://<ip>:8554/stream.h264` - the resolution is configured with `--camera-video.height`
