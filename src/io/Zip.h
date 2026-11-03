// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal uncompressed ("stored") ZIP archives, which is exactly what USDZ
// is: no compression, no encryption, every file's data 64-byte aligned.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sf {

struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
    // Instead of `data`: bytes owned by the caller, which must stay valid until writeStoredZip returns (saving a
    // project writes the meshes' arrays without copying them first).
    const uint8_t* external = nullptr;
    size_t externalSize = 0;

    const uint8_t* bytes() const { return external ? external : data.data(); }
    size_t size() const { return external ? externalSize : data.size(); }
};

// Where a stored entry's data sits in the archive file.
struct ZipFileEntry {
    std::string name;
    uint64_t offset = 0, size = 0;
};

uint32_t crc32(const uint8_t* data, size_t size);

// Writes entries in order with each file's data aligned to `alignment` bytes, streaming them to the file.
// Entries or archives past 4 GB use ZIP64 records (`forceZip64`: always, for tests).
bool writeStoredZip(const std::string& path, const std::vector<ZipEntry>& entries, size_t alignment, std::string& error,
                    bool forceZip64 = false);

// Reads a ZIP (or ZIP64) whose entries are stored (uncompressed), as USDZ requires.
bool readStoredZip(const std::string& path, std::vector<ZipEntry>& entries, std::string& error);
// Lists the entries of such a ZIP and where their data is (checked to lie within the file), without reading it.
bool readStoredZipIndex(const std::string& path, std::vector<ZipFileEntry>& entries, std::string& error);

} // namespace sf
