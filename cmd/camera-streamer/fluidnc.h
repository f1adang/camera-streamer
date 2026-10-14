#pragma once

#include <stdio.h>

typedef struct http_worker_s http_worker_t;

typedef struct fluidnc_options_s {
  char url[512];
  char name[256];
} fluidnc_options_t;

// The controller's status reports, relayed as server-sent events.
void http_fluidnc_events(http_worker_t *worker, FILE *stream);
