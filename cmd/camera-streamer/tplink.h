#pragma once

#include <stdio.h>

typedef struct tplink_options_s {
  char url[256];
  char user[64];
  char password[128];
} tplink_options_t;

// A TP-Link LTE router's mobile connection, read from its API, as a JSON
// object for /telemetry, or null without -tplink-url.
void tplink_json(FILE *stream);

