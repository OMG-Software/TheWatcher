#pragma once

#include "store.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace thewatcher::server
{

// Build a fleet health summary JSON payload from current store state.
nlohmann::json build_report_json(Store& store, int64_t now_ms);

// Build and POST the report to the configured webhook URL.
// Returns true if the report was sent successfully, false otherwise.
bool generate_and_send_report(Store& store, int64_t now_ms);

} // namespace thewatcher::server
