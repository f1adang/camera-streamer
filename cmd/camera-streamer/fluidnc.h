#pragma once

#include <stdio.h>

#define FLUIDNC_MAX 8         // controllers watched at once

typedef struct http_worker_s http_worker_t;

typedef struct fluidnc_options_s {
  char url[2048];             // a list: comma-separated, or the option given again
  char name[2048];            // the names, in the same order
} fluidnc_options_t;

typedef struct fluidnc_controller_s {
  char url[256];
  char name[256];             // "" for none: the page names it after its host
} fluidnc_controller_t;

// The controllers -fluidnc-url names, each with the -fluidnc-name in the same
// place: how many, at most `max`.
int fluidnc_controllers(fluidnc_controller_t *out, int max);

// The controllers' status reports, relayed as server-sent events.
void http_fluidnc_events(http_worker_t *worker, FILE *stream);

// `s` as a JSON string, quoted and escaped.
void json_string(FILE *stream, const char *s);
