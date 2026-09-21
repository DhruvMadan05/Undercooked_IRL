## Inspiration

Overcooked is one of the best co-op games ever made, and it's still just a screen. Everyone crowds around a TV, mashes buttons, and yells at pixels. We wanted the yelling to be about a real frying pan.

It also made us think about team building. The best team-building activities force people to communicate, split up work, and cover for each other under pressure, but the good ones tend to be expensive, need a venue, or need special equipment. Overcooked already teaches all of that, so we asked: what if it were physical, and cheap enough for any club, class, or team event to run?

## What it does

**Undercooked** is Overcooked rebuilt with real, physical cooking stations. Players chop on a real cutting board (a limit switch counts the presses), waggle a joystick to fry in a pan, scrub a sink, and slide food onto plates. Food is a cheap RFID tag, and a laptop runs the game, spawns orders, and keeps score on a live browser scoreboard that any phone or laptop on the Wi-Fi can open.

Nobody can finish an order alone. Players have to call out orders, hand off ingredients, and cover for each other before the clock runs out.

**It costs about $130 to build a full 8-station kitchen.** Each station is a roughly $6 ESP32 board, a $2 RFID reader, a status LED strip, and a few dollars of wiring. Ingredients are 15-cent RFID tags. Add a laptop you already own and there's no console, big screen, or venue to book.

**It's easy to set up.** Pair each station with a single touch of a tag, plug in the bridge, and open the scoreboard. Levels, recipes, and scoring live in one config file, so new levels don't need code changes.

## How we built it

- **Stations:** an ESP32 with an MFRC522 RFID reader, WS2812B LEDs, and one input each (limit switch or joystick), written in C++ with PlatformIO. Stations report events and draw whatever they're told.
- **Bridge:** a single ESP32 that relays between the stations (over ESP-NOW) and the laptop (over USB serial), and doubles as the calibration reader.
- **Game server:** Python. A pure game engine owns all game state, so a reset is one function call and progress survives picking food up and putting it down on a different station.
- **Scoreboard:** a FastAPI websocket pushes the full game state ten times a second to a plain JavaScript page with no build step.

## Challenges we ran into

- **Tiny packets, unreliable radios.** ESP-NOW packets max out at 32 bytes, so we built our own typed protocol with acks, retries, and de-duplication so a lost ack never runs a handler twice.
- **Knowing when a tag has left.** The RFID reader can't say "the tag is gone", only "no answer", so we tuned polling and miss-counting to detect removal reliably.
- **Real hardware quirks.** Joysticks had to run off 3.3V on ADC1 pins, because ADC2 stops working when Wi-Fi is on, and opening the serial port reboots the bridge.
- **Keeping two languages in sync.** The firmware is C++ and the server is Python, so a test parses the C++ header and fails the build if the two protocols drift.

## Accomplishments that we're proud of

- We tested everything on real hardware, and every station works.
- The whole game can run with zero boards plugged in, thanks to a simulated bridge that speaks the real protocol, backed by 100+ Python tests and native C++ unit tests.
- A full kitchen for around $130 that anyone can build.

## What we learned

- **Keep the hardware dumb.** Putting all game state on the laptop and letting stations only report events and draw what they're told made the system far easier to debug, test, and reset.
- **Physical games are unforgiving.** Wiring, timing, and radio range all show up in gameplay in a way they never do in software, so testing on real hardware early matters.
- **Test the seams.** Most of our bugs lived where two pieces met (C++ and Python, radio and serial), so tests that guard those boundaries paid off the most.
- **Cheap parts go a long way.** A few dollars of ESP32s, RFID readers, and tags is enough to build something people actually want to play together.

## What's next for Undercooked

- **More levels:** new kitchens, bigger orders, and tougher recipes.
- **Better physical stations:** 3D-printed housings for the cutting board, pan, pot, and sink instead of loose breadboards, while keeping costs low.
- **More station types:** a real recipe for the sink, new appliances, and plate contents shown on LEDs.
- **Bigger teams:** more players and a full table-sized kitchen for team events.

## Built with

C++, Arduino, PlatformIO, ESP32, ESP-NOW, MFRC522 RFID, WS2812B LEDs, Python, FastAPI, WebSockets, JavaScript
