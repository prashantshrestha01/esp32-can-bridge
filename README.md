# ESP32 WiFi <-> CAN bridge — 2011 Ford Fusion

The ESP32 joins the car's CAN bus through the SN65HVD230 transceiver and
serves it over its own WiFi network. Connect your phone to the `FusionCAN`
access point, open **http://192.168.4.1**, and you get a live CAN monitor in
the browser plus a form to transmit frames (the replay half of
sniff-diff-replay).

- **Board:** Elecrow CrowPanel 5.0" (ESP32-S3-WROOM-1-N4R8)
- **Toolchain:** ESP-IDF v5.4.4, target `esp32s3`
- **CAN:** S3 built-in TWAI + SN65HVD230 transceiver
- **WiFi:** access-point mode (`FusionCAN` / `fusion1234`), HTTP + WebSocket

## Wiring

Default bus: **MS-CAN body bus, 125 kbps, OBD2 pins 3/11** (climate, locks,
windows). OBD2 pigtail, male plug front view, wide side up: top row = pins
1–8 left to right, bottom row = 9–16.

| OBD2 pin | Goes to |
|---|---|
| 3 (MS-CAN-H, top row 3rd from left) | SN65HVD230 `CANH` |
| 11 (MS-CAN-L, bottom row 3rd from left) | SN65HVD230 `CANL` |
| 4 / 5 (GND) | ESP32 `GND`, transceiver `GND` |

| Transceiver pin | Goes to |
|---|---|
| `VCC` | ESP32 `3V3` (3.3 V module — not 5 V) |
| `GND` | ESP32 `GND` |
| `TXD` | ESP32 `IO19` (J4 pad 7) |
| `RXD` | ESP32 `IO20` (J4 pad 8) |

**Remove the 120 Ω termination jumper** on the transceiver. Sanity check with
the car OFF: ~60 Ω between OBD2 pins 3 and 11.

For the **HS-CAN powertrain bus** (500 kbps, pins 6/14): set
`CAN_BITRATE_KBPS` to 500 in `main/bridge.c` and move CANH/CANL to pins 6/14.
For safe sniffing with no transmit at all: set `CAN_LISTEN_ONLY` to 1.

Power: USB-C from a laptop/power bank is fine for testing. Permanent in-car
install wants a buck converter on an ACC-switched fuse (cigarette lighter) —
never OBD2 pin 16 (always-on, drains the battery).

## Flash

Prebuilt: `can-bridge.bin` (flash at 0x0), or:

```bash
. ~/esp/esp-idf/export.sh
cd esp32-can-bridge
idf.py set-target esp32s3
idf.py build
```

Flash with `idf.py -p /dev/ttyACM0 flash` or
[Adafruit's web flasher](https://adafruit.github.io/Adafruit_WebSerial_ESPTool/)
(ESP32-S3, file at offset 0x0).

## Use

1. Wire it up, plug the pigtail into the car, power the ESP32.
2. On your phone, join the `FusionCAN` WiFi (password `fusion1234`).
3. Open **http://192.168.4.1** — live frames stream into the table, new IDs
   highlight yellow.
4. Ignition ON to wake the body modules.
5. To test a captured message: type the ID (hex, e.g. `2F0`) and data bytes
   (hex, e.g. `01 02 A3`) into the send form and hit Send.

Serial monitor at 115200 mirrors the same log over USB.

## Notes

- HVAC messages on Ford body buses are usually simple: same ID, one or two
  bytes change per button. Some modules want the message repeated every
  100–500 ms or they time out — if a single send does nothing, that's the
  next thing to try.
- Stay off safety buses (ABS, airbag, steering). Climate, locks, windows and
  lights are the safe playground.
- The S3 has one TWAI controller. For HS-CAN + MS-CAN simultaneously, add an
  MCP2515 SPI module (~$5) as the second bus.
