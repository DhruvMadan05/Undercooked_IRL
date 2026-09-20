#pragma once

// RC522 tag reader that broadcasts scanned UIDs to other boards over ESP-NOW.
void rfidSetup();
void rfidLoop();
