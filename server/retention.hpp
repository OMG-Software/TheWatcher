#pragma once

#include "store.hpp"

#include <cstdint>

namespace thewatcher::server
{

// Deletes rows past their retention period from every table that grows
// without bound (#76). Runs from the server's periodic maintenance tick, so it
// does not depend on agents sending metrics. Settings (server_settings, whole
// days; invalid or non-positive values fall back to the default):
//   metrics_retention_days         default 30  metrics
//   status_history_retention_days  default 90  status history (the newest row per
//                                              indicator is always kept)
//   alerts_retention_days          default 90  acknowledged or deleted alerts, plus
//                                              expired silences and finished
//                                              maintenance windows
//   log_matches_retention_days     default 30  log matches
// Store errors propagate and stop the run; the next run starts again.
void run_retention(Store& store, int64_t now_ms);

} // namespace thewatcher::server
