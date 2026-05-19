#pragma once

// Intentionally a hard compile error: the ESP32-S3 pinout is a placeholder.
// See spec section 1.2.2 — it will be redesigned and validated against
// real hardware before any code is allowed to depend on it. Anyone hitting
// this should either design the S3 pinout, or guard the consumer with
// `#if CONFIG_IDF_TARGET_ESP32` so the S3 build can keep going without
// touching this header.

#error "ESP32-S3 pinout is a TBD placeholder. See spec 1.2.2."
