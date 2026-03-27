# DES_PDC — Parking Distance Control System

A distributed automotive embedded system built on Yocto Linux, implementing a full Parking Distance Control (PDC) feature across two ECUs communicating over Ethernet via SOME/IP (vsomeip / CommonAPI).

## System Architecture

```
┌──────────────────────┐    ┌─────────────────────────────┐         ┌──────────────────────────────────────────┐
│  Arduino             │    │        ECU1 — RPi 4         │         │        ECU2 — Jetson Orin Nano           │
│                      │    │     (meta-vehiclecontrol)   │         │        (meta-seame-headunit)             │
│  Ultrasonic sensor   │    │                             │  Eth    │                                          │
│  → distance (float)  │    │  VehicleControl Service ────┼─────────┼──► unified-compositor (Wayland)          │
│                      │CAN │  (SOME/IP server 0x1234)    │  SOME/IP│        ├── GearApp          (HU)         │
│  Wheel speed sensor  ├───►│                             │         │        ├── HomeScreenApp     (HU)        │
│  → speed (cm/s)      │    │  Gamepad → gear/speed/      │         │        ├── MediaApp          (HU)        │
│                      │    │           battery data      │         │        ├── AmbientApp        (HU)        │
│  MCP2515 CAN         │    │                             │  Eth    │        ├── PDCApp (gear=R)   (HU)        │
│  ID: 0x0F6           │    │  OV5647 Camera ─────────────┼─────────┼──►     ├── Speedometer       (IC)        │
│  1000 KBPS           │    │  GStreamer RTP sender       │  UDP    │        ├── BatteryMeter      (IC)        │
│  100ms cycle         │    │  (192.168.1.100:5000)       │         │        └── GearState         (IC)        │
└──────────────────────┘    │                             │         │                                          │
                            │  MCP2518FD CAN bus          │         │  GStreamer RTP/UDP → PDCApp camera feed  │
                            └─────────────────────────────┘         └──────────────────────────────────────────┘
                                     192.168.1.100                               192.168.1.101
```

## Repository Structure

```
DES_PDC/
├── ECU2_YOCTO/                  # Yocto build system for Jetson Orin Nano (ECU2)
│   ├── layers/
│   │   ├── meta-seame-headunit/ # Custom layer — all SEAME apps and configs
│   │   │   ├── recipes-apps/
│   │   │   │   ├── unified-compositor/   # Wayland HU compositor (vsomeip client)
│   │   │   │   ├── ic-compositor/        # Wayland IC compositor
│   │   │   │   ├── gearapp/              # Gear indicator (HU)
│   │   │   │   ├── pdcapp/               # Rear camera + PDC overlay (HU)
│   │   │   │   ├── homescreenapp/        # Home screen (HU)
│   │   │   │   ├── mediaapp/             # Media player (HU)
│   │   │   │   ├── ambientapp/           # Ambient display (HU)
│   │   │   │   ├── speedometer-app/      # Speed display (IC)
│   │   │   │   ├── batterymeter-app/     # Battery display (IC)
│   │   │   │   └── gearstate-app/        # Gear state display (IC)
│   │   │   ├── recipes-containers/       # Per-app Docker container recipes (OTA)
│   │   │   │   ├── gearstate-container/
│   │   │   │   ├── speedometer-container/
│   │   │   │   ├── batterymeter-container/
│   │   │   │   ├── pdcapp-container/
│   │   │   │   ├── gearapp-container/
│   │   │   │   ├── homescreen-container/
│   │   │   │   ├── mediaapp-container/
│   │   │   │   └── ambientapp-container/
│   │   │   └── recipes-core/images/
│   │   │       └── seame-headunit-image.bb  # Top-level image recipe
│   │   └── meta-middleware/     # vsomeip / CommonAPI middleware layer
│   └── build/                   # Yocto build directory (not version-controlled)
└── pdcMeta-ECU1/                # Yocto meta layer for Raspberry Pi 4 (ECU1)
    └── meta-vehiclecontrol/
```

## Arduino

- **Role**: Sensor node — measures distance and wheel speed, sends over CAN
- **Sensors**:
  - Ultrasonic sensor (TRIG pin 8, ECHO pin 7) — measures distance up to ~8.5m
  - Wheel speed sensor (pin 3, interrupt-driven) — 20 pulses/rev, 20.083cm circumference
- **CAN**: MCP2515 (SPI, CS pin 9), 1000 KBPS, CAN ID `0x0F6`
- **Message format** (8 bytes, every 100ms):
  - Bytes 0-2: speed in cm/s (integer high byte, integer low byte, 2 decimal places)
  - Bytes 3-6: distance in cm (4-byte float)
  - Byte 7: reserved
- **Connected to**: RPi 4 via MCP2518FD CAN bus

## ECU1 — Raspberry Pi 4

- **OS**: Yocto Linux (custom `meta-vehiclecontrol` layer)
- **Role**: Vehicle sensor hub, control hub and camera source
- **Key functions**:
  - Receives speed and distance data from Arduino over CAN (MCP2518FD, 1000 kbps)
  - Reads gamepad input → publishes gear, speed, battery over SOME/IP
  - Streams Pi Camera v1.3 via GStreamer RTP/UDP to ECU2
- **SOME/IP**: VehicleControl service (ID `0x1234`, instance `0x5678`)
- **Camera pipeline**: `libcamerasrc → x264enc → rtph264pay → udpsink`
- **Network**: `eth0` at `192.168.1.100`

## ECU2 — Jetson Orin Nano

- **OS**: Yocto Linux (NVIDIA Tegra BSP + `meta-seame-headunit` layer)
- **Role**: Display unit — instrument cluster (IC) + head unit (HU)
- **Key functions**:
  - Subscribes to VehicleControl SOME/IP service for gear/speed/battery
  - `unified-compositor` renders all apps — both IC (speedometer, battery, gear state) and HU (gear app, home, media, ambient)
  - When gear = R: PDCApp appears on HU with live rear camera feed from ECU1
- **Network**: `eth0` at `192.168.1.101`

## Per-App Docker Containers (OTA)

Each application runs in its own Docker container for independent OTA updates. Resource allocation is enforced via Linux cgroups:

| Container | CPU Cores | CPU Shares | Memory | OOM Priority |
|---|---|---|---|---|
| gearstate | 0,1 (exclusive) | 1024 | 256m | -1000 (never kill) |
| speedometer | 0,1 (exclusive) | 1024 | 256m | -1000 (never kill) |
| batterymeter | 0,1 (exclusive) | 1024 | 256m | -1000 (never kill) |
| pdcapp | 2,3,4 (cores 2,3 exclusive) | 1024 | 512m | -300 (protected) |
| gearapp | 4,5 | 800 | 256m | -300 (protected) |
| homescreen | 4,5 | 512 | 512m | 0 (default) |
| mediaapp | 4,5 | 512 | 512m | 0 (default) |
| ambientapp | 4,5 | 256 | 256m | +300 (sacrificed first) |

IC apps (gearstate, speedometer, batterymeter) are pinned to cores 0,1 — physically unreachable by HU apps.

## Building ECU2 Image

### Prerequisites

```bash
# Install Yocto host dependencies (Ubuntu 22.04)
sudo apt install gawk wget git diffstat unzip texinfo gcc build-essential \
    chrpath socat cpio python3 python3-pip python3-pexpect xz-utils \
    debianutils iputils-ping python3-git python3-jinja2 libegl1-mesa \
    libsdl1.2-dev pylint xterm python3-subunit mesa-common-dev zstd liblz4-tool
```

### Clone and Build

```bash
git clone https://github.com/Changseok-Oh29/DES_PDC.git
cd DES_PDC
git submodule update --init --recursive

cd ECU2_YOCTO
. setup-env --machine jetson-orin-nano-devkit build
bitbake seame-headunit-image
```

### Flash to Jetson

Follow the NVIDIA Jetson flashing guide using the generated image in `build/tmp/deploy/images/jetson-orin-nano-devkit/`.

## Network Setup

After first boot, configure static IPs:

```bash
# On Jetson (ECU2)
sudo ip addr add 192.168.1.101/24 dev eth0

# On RPi (ECU1) — already configured via Yocto
# 192.168.1.100/24 on eth0
```

## Hardware

| Component | ECU1 (RPi 4) | ECU2 (Jetson Orin Nano) |
|---|---|---|
| Board | Raspberry Pi 4 | NVIDIA Jetson Orin Nano |
| Camera | OV5647 (RPi Cam v1.3) | — |
| CAN | MCP2518FD via SPI | — |
| Display | — | HDMI (IC + HU via Weston) |
| Network | eth0 192.168.1.100 | eth0 192.168.1.101 |

## License

MIT License — SEA:ME
