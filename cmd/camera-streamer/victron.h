#pragma once

#include <stdio.h>

typedef struct victron_options_s {
  char mac[32];
  char key[64];
  char name[256];
} victron_options_t;

// A Victron battery monitor's Instant Readout, heard over Bluetooth, as a
// JSON object for /telemetry, or null without -victron-mac.
void victron_json(FILE *stream);
