// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHA-256 (FIPS 180-4), for checking downloads such as community gallery
// brushes against the hash their index publishes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sf {

// Lowercase hex digest of `size` bytes.
std::string sha256Hex(const void* data, size_t size);
inline std::string sha256Hex(const std::string& data) { return sha256Hex(data.data(), data.size()); }

} // namespace sf
