// Config.h — all site-specific values live here and nowhere else.
// Network settings live in NVS and are set on the setup page.

#pragma once
#include <Arduino.h>

// ---- Firmware ---------------------------------------------------------
#define FIRMWARE_VERSION "0.26.0"
// Set by the compiler at build time. Never edit this by hand.
#define FIRMWARE_DATE    __DATE__

// ---- Wi-Fi ------------------------------------------------------------
// WROOM-32 is 2.4 GHz only. A 5 GHz-only SSID will never associate.
//
// No network is compiled in. A unit with no saved network starts in SETUP:
// its own hotspot, a setup slip with a QR code to join it, and a setup
// page that opens by itself. Holding BOOT for 5 s while running forgets
// the saved network and returns to setup.
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// Setup hotspot: this prefix plus the last four hex digits of the unit's
// MAC, so every unit's name differs. Its password is generated once per
// unit and kept in a namespace a factory reset does not touch, so the
// label on the case stays true for the life of the unit.
#define SETUP_AP_PREFIX "SmartThermal-"

// The BOOT button (GPIO 0). Read only while the firmware is running:
// held during power-up it puts the ESP32 into its flashing mode instead.
#define BOOT_PIN      0
#define BOOT_HOLD_MS  5000

// ---- Printer ----------------------------------------------------------
// From the POS-8370 self-test slip: static IP, DHCP disabled, port 9100.
// Confirmed on hardware: printer 192.168.11.100, gateway 192.168.11.1.
#define PRINTER_IP_A 192
#define PRINTER_IP_B 168
#define PRINTER_IP_C 11
#define PRINTER_IP_D 100

#define PRINTER_PORT 9100

// How long to wait for the printer to accept a TCP connection.
#define PRINTER_CONNECT_TIMEOUT_MS 4000

// ---- Build ----------------------------------------------------------------
// 1 = TRAINER build: includes the unattended trainer (menu T / X / Z).
// 0 = LEND-OUT build: no trainer code at all. It reads the thermal profile
//     the trainer left in NVS, which survives flashing one over the other
//     as long as "Erase All Flash Before Sketch Upload" stays disabled.
#define BUILD_TRAINER 1

// Bench tests (serial H, J, K, V). 0 for a unit that leaves the house:
// production carries none of the test code.
#define BUILD_BENCH 1

// ---- Printer link -------------------------------------------------------
// 1 = W5500 Ethernet, cabled straight to the printer.
// 0 = Wi-Fi TCP through the router, as in every build before v0.7.1.
//
// A compile-time switch rather than a setting, because the two are
// different hardware and a unit is built for one of them. Flip to 0 to
// fall back if the W5500 is not wired yet.
//
// WHY THE W5500 NEEDS NO ROUTING WORK
// The Arduino Ethernet library drives the W5500's OWN hardware TCP/IP
// stack. It never touches the ESP32's lwIP stack or its routing table, so
// the Wi-Fi LAN and the printer cable are two networks that cannot see
// each other - even if their addresses overlap. The printer can keep
// whatever address it already has; nothing needs moving onto the Wi-Fi
// subnet. The W5500 simply takes a spare address on the printer's subnet.
#define PRINTER_LINK_W5500 1

// W5500 wiring. These are the ESP32's default VSPI pins.
#define W5500_CS   5
#define W5500_RST  4
#define W5500_SCK  18
#define W5500_MISO 19
#define W5500_MOSI 23

// ---- Time -------------------------------------------------------------
// POSIX TZ string. The sign is inverted by convention: UTC+8 is "-8".
// Perth (AWST) has no daylight saving, so a single offset is enough.
// Melbourne/Sydney would be "AEST-10AEDT,M10.1.0,M4.1.0/3".
#define TIMEZONE  "AWST-8"
#define NTP_SERVER_1 "au.pool.ntp.org"
#define NTP_SERVER_2 "pool.ntp.org"

// Online printing (v0.21): the website's printer relay. The unit connects
// out to wss://RELAY_HOST RELAY_PATH. Changeable in Settings (Online).
#define RELAY_HOST "print.justifiedsystems.com.au"
#define RELAY_PATH "/relay/connect"

// Keep-awake (v0.22): after this many hours without a print, send the
// printer a job that prints nothing, to see whether long quiet spells are
// what makes it stop answering. 0 = off.
#define KEEPAWAKE_HOURS 8

