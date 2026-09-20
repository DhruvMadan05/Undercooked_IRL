#include <Arduino.h>
#include <StandardStation.h>

// Plate and delivery stations. They only read tags and show LEDs, so this
// is the same firmware twice; the PlatformIO environment picks the kind
// (see platformio.ini). What each one does is decided by the game server:
//   plate     this reader IS the plate: any food put on it goes onto that plate, at any
//             time. The plate has its own tag, touched to the delivery station to serve it.
//   delivery  touch a plate's tag here to serve what is on its plate reader; loose food
//             put here is thrown away
// RC522 reader and LED strip: see shared/StationCore/src/StandardWiring.h.
// There is nothing else to wire.

#if defined(STATION_PLATE)
constexpr oc::StationKind kKind = oc::StationKind::Plate;
#elif defined(STATION_DELIVERY)
constexpr oc::StationKind kKind = oc::StationKind::Delivery;
#else
#error "Select an environment: plate or delivery"
#endif

station::StandardStation readerStation(kKind);

void setup() {
  Serial.begin(115200);
  readerStation.begin();
}

void loop() {
  readerStation.update(millis());
}
