# Terra PetPlant

An ESP32S3 plant-companion prototype combining soil, light, temperature and humidity sensing with a display, speaker and voice clips. The repository contains Arduino firmware, audio assets, a Cloudflare relay and hardware notes.

## Repository map

- [Firmware](src/) and [PlatformIO configuration](platformio.ini).
- [Wiring reference](WIRING.md): documented sensor, display and audio connections. The wiring document records individual bench tests; it is not an end-to-end acceptance report.
- [Audio assets](data/) and [LittleFS partition table](partitions_petplant.csv).
- [Cloudflare relay](cloud/terra-relay/): a separate network component; review its configuration before deployment.

## Build configurations

The configured target is a Seeed Studio XIAO ESP32S3 using the Arduino framework. Two PlatformIO environments are provided:

```sh
pio run -e seeed_xiao_esp32s3  # round GC9A01 display configuration
pio run -e xiao_v2_mono       # SSD1306 mono OLED configuration
```

These are build commands from the checked-in configuration, not a firmware upload or hardware test. Dependencies and display flags are listed in [platformio.ini](platformio.ini). Match the selected display configuration to the actual hardware. The [wiring guide](WIRING.md) describes the round-display arrangement; the mono environment uses a different display configuration.

## Status and setup boundaries

This is a hardware/software prototype. Build, filesystem contents, sensor calibration, network provisioning and relay configuration depend on the intended device. Review the firmware and wiring before flashing. Runtime network access depends on the enabled features and relay setup; the presence of source does not establish that a hosted relay is running. No hosted service or paid plan is required merely to read this repository.
