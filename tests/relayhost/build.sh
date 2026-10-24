#!/bin/sh
# PC harness: the unit's real RelaySession + OnlineKey talking to a relay
# (tests/relay/relay.js, or the real relay under wrangler dev), replaying
# requests to the mock unit. Needs:
#   ARDUINOJSON  ArduinoJson 7 src folder (holding ArduinoJson.h)
#   MBEDTLS      an mbedTLS 4 source tree built with `make lib`
set -e
cd "$(dirname "$0")"
AJ="${ARDUINOJSON:-$HOME/Arduino/libraries/ArduinoJson/src}"
MB="${MBEDTLS:-$HOME/mbedtls}"
g++ -std=gnu++17 -O1 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -I. -I"$AJ" \
  -I"$MB/include" -I"$MB/tf-psa-crypto/include" -I"$MB/tf-psa-crypto/drivers/builtin/include" \
  host_relay.cpp ../../SmartThermal/RelaySession.cpp ../../SmartThermal/OnlineKey.cpp \
  -L"$MB/library" -lmbedcrypto -o host_relay
echo "built tests/relayhost/host_relay"
