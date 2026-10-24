#!/bin/sh
# Scheduler + item storage tests on a PC (Linux/macOS/WSL, g++ with C++17).
# The firmware files are copied from ../../SmartThermal each time, so the
# tests always run against the real code; the .h files here are PC shims.
# Needs ArduinoJson 7: set ARDUINOJSON to its src folder (the one holding
# ArduinoJson.h), e.g. ~/Arduino/libraries/ArduinoJson/src.
set -e
cd "$(dirname "$0")"
AJ="${ARDUINOJSON:-$HOME/Arduino/libraries/ArduinoJson/src}"
rm -rf build && mkdir build
for f in Items.cpp Items.h Scheduler.cpp Scheduler.h SchedRule.h Config.h; do cp "../../SmartThermal/$f" build/; done
g++ -std=gnu++17 -O1 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -I. -Ibuild -I"$AJ" -o build/host_test host_test.cpp build/Items.cpp build/Scheduler.cpp
./build/host_test
