#pragma once

#include <stdio.h>

typedef struct teltonika_options_s {
  char url[256];
  char user[64];
  char password[128];
} teltonika_options_t;

// A Teltonika router's mobile connection, read from its API, as a JSON
// object for /telemetry, or null without -teltonika-url.
void teltonika_json(FILE *stream);
