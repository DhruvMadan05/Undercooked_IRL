#include <Arduino.h>
#include <StandardStation.h>

#include "FryTask.h"

// Deep fryer station. Which foods can be fried and how big the progress goal
// is are set on the server (level.toml); this file is only the wiring.
// RC522 reader and LED strip: see shared/StationCore/src/StandardWiring.h.
//
// HC-SR04 ultrasonic, reads the height of the player's hand/basket handle
// above the fryer.
//   HC-SR04 VCC  -> 5V              (the sensor itself needs 5V, not 3.3V)
//   HC-SR04 GND  -> GND
//   HC-SR04 TRIG -> GPIO4           (direct -- ESP32's 3.3V output is a valid trigger level)
//   HC-SR04 ECHO -> GPIO35 through a voltage divider: ECHO -> 1k -> node,
//                   node -> 2k -> GND, node -> GPIO35. ECHO pulses at 5V,
//                   which will damage the ESP32's 3.3V-only GPIO inputs
//                   without the divider. GPIO35 is input-only, which is fine
//                   since it is only ever read (same idea as the pan's joystick pins).
#define TRIG_PIN 4
#define ECHO_PIN 35

// 0.91" SSD1306 OLED (128x32), I2C, "Ver 1.6" 4-pin module. Shows the target
// zone's position and which direction the hand needs to move to reach it.
//   OLED GND -> GND
//   OLED VCC -> 3V3     (these modules are 3.3V logic; check the silkscreen
//                        before trying 5V even if it has an onboard regulator)
//   OLED SCL -> GPIO22  (I2C clock, ESP32's default)
//   OLED SDA -> GPIO21  (I2C data, ESP32's default)
// Default I2C address for these boards is 0x3C. If the screen stays blank,
// run an I2C scanner sketch first -- a few clones ship as 0x3D instead.
#define OLED_SDA_PIN  21
#define OLED_SCL_PIN  22
#define OLED_ADDRESS  0x3C

// Hand-height range the ultrasonic cares about, in cm. Closer than NEAR_CM
// clamps to "top" (1.0); farther than FAR_CM clamps to "bottom" (0.0). Tune
// to the physical mounting height. CATCH_ZONE_FRAC is how much of that range
// counts as a hit, centered on the target -- wider is easier.
#define NEAR_CM 3.0f
#define FAR_CM  30.0f
#define CATCH_ZONE_FRAC 0.30f

FryTask fryTask(TRIG_PIN, ECHO_PIN, OLED_SDA_PIN, OLED_SCL_PIN, OLED_ADDRESS, NEAR_CM, FAR_CM, CATCH_ZONE_FRAC);
station::StandardStation deepFryer(oc::StationKind::Fryer, &fryTask);

void setup() {
  Serial.begin(115200);
  deepFryer.begin();
}

void loop() {
  deepFryer.update(millis());
}
