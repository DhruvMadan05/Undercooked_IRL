#include <Arduino.h>
#include <StandardStation.h>

// Pot, plate and delivery stations. They only read tags and show LEDs, so this
// is the same firmware three times; the PlatformIO environment picks the kind
// (see platformio.ini). What each one does is decided by the game server:
//   pot       the server times the cooking and drives the LEDs (bar, blinking
//             warning when about to burn, solid red once burnt)
//   plate     this reader IS the plate: any food put on it goes onto that plate, at any
//             time. The plate has its own tag, touched to the delivery station to serve it.
//   delivery  touch a plate's tag here to serve what is on its plate reader; loose food
//             put here is thrown away
// RC522 reader and LED strip: see shared/StationCore/src/StandardWiring.h.
// There is nothing else to wire.

#if defined(STATION_POT)
constexpr oc::StationKind kKind = oc::StationKind::Pot;
#elif defined(STATION_PLATE)
constexpr oc::StationKind kKind = oc::StationKind::Plate;
#elif defined(STATION_DELIVERY)
constexpr oc::StationKind kKind = oc::StationKind::Delivery;
#else
#error "Select an environment: pot, plate or delivery"
#endif

station::StandardStation readerStation(kKind);

void setup() {
  Serial.begin(115200);
  readerStation.begin();
}

void loop() {
  readerStation.update(millis());
}
