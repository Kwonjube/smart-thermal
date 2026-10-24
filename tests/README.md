# Tests

Nothing here is uploaded to the ESP32. Everything runs on a PC.

| What | How | Needs |
|---|---|---|
| Page tests (the printer's web page against a pretend printer) | `npm install`, then in one terminal `npm run mock`, in another `npm run page` | Node 18+, Chrome or Edge (or set `CHROME`) |
| Preview, demo and levels | `npm run pages` (builds `dist/` first) | as above |
| Scheduler and storage | `sh host/build.sh` | g++ (Linux, macOS or WSL), ArduinoJson 7 (`ARDUINOJSON`) |
| Schedule maths only | `g++ -std=c++17 sched_test.cpp -o sched_test && ./sched_test` | g++ |
| Relay session (firmware side) | `sh relayhost/build.sh`, then `sh start_stack.sh` and `node remote_test.js` | g++, ArduinoJson 7, mbedTLS 4 (`MBEDTLS`) |
| Against the real relay code | see `liverelay/README.md` | as above + wrangler |

`mock.js` serves the real page (assembled from the firmware's `WebUI*.h`) and answers
the API with `SmartThermal/tools/fakeunit.js`, the same pretend printer that is built into
`preview.html` and `demo.html`, so the tests and the demo cannot drift apart.

`photo.jpg` is a generated test picture. Screenshots, logs and test keys are written to
`tests/out/` and next to the harnesses; all are ignored by git.
