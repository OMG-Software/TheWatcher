#pragma once

#include <filesystem>
#include <string>

namespace thewatcher::common
{

// Atomically replaces `target` with `content`. The data is written to an
// owner-only temporary file in the same directory (0600 on POSIX; a protected
// DACL for SYSTEM, Administrators and the owner on Windows), flushed to stable
// storage and then renamed over `target`, so a crash or full disk can never
// leave a truncated file and the secret is never readable by other users.
// Throws std::runtime_error on any failure; the temporary file is removed and
// the previous `target` is left in place.
void write_private_file_atomic(const std::filesystem::path& target, const std::string& content);

} // namespace thewatcher::common
