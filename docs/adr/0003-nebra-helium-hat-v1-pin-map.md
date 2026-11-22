# ADR-0003: Pin map for the Nebra Helium Hotspot HAT V1

- Status: accepted
- Date: 2026-11-21

## Context

The design plan's pin map puts the concentrator on SPI1 CE2 (Pi pins
35/36/38/40), taken from hm-pyhelper's `nebra-light1` definition. The bench
HAT is the Nebra Helium Hotspot HAT V1 (RAK833 backplane). Its schematic puts
SPI on SPI0 CE0 (Pi pins 19/21/23/24). With the plan's map, the concentrator
did not answer. With the schematic's map, it answered at once.

## Decision

- New Kconfig choice `GW_HAT_NEBRA_HELIUM_V1`, the default. SPI: SCLK GPIO0,
  MOSI GPIO3, MISO GPIO2, CS GPIO36. Reset GPIO6, LED GPIO4, button GPIO5.
- The plan's SPI1 CE2 map is kept as `GW_HAT_NEBRA_HOTSPOT` ("Nebra Light
  Hotspot carrier") until a board of that type is tested.
- With this HAT, GPIO53 (speaker amplifier) is not on the SPI path, so R52 does
  not need to be removed.

## Consequences

- The design plan's "Pin map" and "Conflicts" sections need an update. Evidence
  is in docs/bringup-log.md.
- The band jumper (JP1) can be read at boot to check the build's region.
