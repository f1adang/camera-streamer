#pragma once

#include <stdio.h>

// The health of the host this runs on, as a JSON object, for /telemetry.
void server_stats_json(FILE *stream);
