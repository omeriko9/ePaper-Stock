#pragma once

#include <stdbool.h>

// Starts the HTTP server on the configured port. Idempotent.
bool web_server_start();
void web_server_stop();
bool web_server_restart_if_needed(unsigned new_port);
bool web_server_is_running();
