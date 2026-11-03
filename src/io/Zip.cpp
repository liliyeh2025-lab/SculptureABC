// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/Zip.h"

#include "core/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace sf {

namespace {

void put16(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}
void put32(std::vector<uint8_t>& out, uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}
void put64(std::vector<uint8_t>& out, uint64_t v) {
    put32(out, uint32_t(v));
    put32(out, uint32_t(v >> 32));
}
uint32_t get16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }

constexpr uint32_t kMax32 = 0xFFFFFFFFu;

// CRC-32 tables for slicing by 8 (eight bytes per step).
const std::array<std::array<uint32_t, 256>, 8>& crcTables() {
    static const auto tables = [] {
        std::array<std::array<uint32_t, 256>, 8> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[size_t(s)][i] = (t[size_t(s) - 1][i] >> 8) ^ t[0][t[size_t(s) - 1][i] & 0xFF];
        return t;
    }();
    return tables;
}

std::filesystem::path fsPath(const std::string& p) { return std::filesystem::path(std::u8string(p.begin(), p.end())); }

} // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
    const auto& t = crcTables();
    uint32_t c = 0xFFFFFFFFu;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        const uint32_t lo = c ^ (uint32_t(data[i]) | uint32_t(data[i + 1]) << 8 | uint32_t(data[i + 2]) << 16 |
                                 uint32_t(data[i + 3]) << 24);
        c = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^ t[3][data[i + 4]] ^
            t[2][data[i + 5]] ^ t[1][data[i + 6]] ^ t[0][data[i + 7]];
    }
    for (; i < size; ++i) c = t[0][(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool writeStoredZip(const std::string& path, const std::vector<ZipEntry>& entries, size_t alignment, std::string& error,
                    bool forceZip64) {
    // CRCs first, in parallel (one entry per job: the big arrays of a project are separate entries).
    std::vector<uint32_t> crcs(entries.size());
    poolFor(entries.size(), [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) crcs[i] = crc32(entries[i].bytes(), entries[i].size());
    }, 1, 2);

    std::ofstream f(fsPath(path), std::ios::binary);
    if (!f) {
        error = "Cannot write " + path;
        return false;
    }
    // Entries go straight to the file (no copy of the archive in memory). Sizes and offsets that don't fit
    // 32 bits use ZIP64 extra fields; archives under 4 GB come out exactly as before.
    uint64_t offset = 0;
    std::vector<uint8_t> header, central;
    bool anyZip64 = forceZip64;
    for (size_t k = 0; k < entries.size(); ++k) {
        const ZipEntry& e = entries[k];
        const uint64_t size = e.size();
        const bool big = forceZip64 || size >= kMax32, farOff = forceZip64 || offset >= kMax32;
        anyZip64 = anyZip64 || big || farOff;
        header.clear();
        const size_t zip64Local = big ? 20 : 0; // id, length, uncompressed and compressed size
        // Pad with an extra field so the file data starts on an aligned offset.
        const uint64_t dataStart = offset + 30 + e.name.size() + zip64Local;
        size_t pad = alignment > 1 ? size_t((alignment - dataStart % alignment) % alignment) : 0;
        if (pad > 0 && pad < 4) pad += alignment; // an extra field needs at least its 4-byte header
        put32(header, 0x04034B50);
        put16(header, big ? 45 : 20); // version needed
        put16(header, 0);             // flags
        put16(header, 0);             // stored
        put16(header, 0);             // time
        put16(header, 0x21);          // date (1980-01-01)
        put32(header, crcs[k]);
        put32(header, big ? kMax32 : uint32_t(size));
        put32(header, big ? kMax32 : uint32_t(size));
        put16(header, uint32_t(e.name.size()));
        put16(header, uint32_t(zip64Local + pad));
        header.insert(header.end(), e.name.begin(), e.name.end());
        if (big) {
            put16(header, 0x0001);
            put16(header, 16);
            put64(header, size);
            put64(header, size);
        }
        if (pad) {
            put16(header, 0x1986); // padding extra field (same id as Pixar's usdzip)
            put16(header, uint32_t(pad - 4));
            header.insert(header.end(), pad - 4, 0);
        }
        if (!f.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size())) ||
            !f.write(reinterpret_cast<const char*>(e.bytes()), std::streamsize(size))) {
            error = "Cannot write " + path;
            return false;
        }

        std::vector<uint8_t> extra; // ZIP64: the fields saturated below, in this order
        if (big) {
            put64(extra, size);
            put64(extra, size);
        }
        if (farOff) put64(extra, offset);
        put32(central, 0x02014B50);
        put16(central, big || farOff ? 45 : 20); // version made by
        put16(central, big || farOff ? 45 : 20);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crcs[k]);
        put32(central, big ? kMax32 : uint32_t(size));
        put32(central, big ? kMax32 : uint32_t(size));
        put16(central, uint32_t(e.name.size()));
        put16(central, extra.empty() ? 0 : uint32_t(extra.size() + 4)); // extra
        put16(central, 0); // comment
        put16(central, 0); // disk
        put16(central, 0); // internal attributes
        put32(central, 0); // external attributes
        put32(central, farOff ? kMax32 : uint32_t(offset));
        central.insert(central.end(), e.name.begin(), e.name.end());
        if (!extra.empty()) {
            put16(central, 0x0001);
            put16(central, uint32_t(extra.size()));
            central.insert(central.end(), extra.begin(), extra.end());
        }
        offset += header.size() + size;
    }
    const uint64_t centralOffset = offset;
    std::vector<uint8_t> tail = std::move(central);
    anyZip64 = anyZip64 || centralOffset >= kMax32 || entries.size() >= 0xFFFF;
    if (anyZip64) {
        // ZIP64 end of central directory record and its locator.
        const uint64_t zip64Eocd = centralOffset + tail.size();
        put32(tail, 0x06064B50);
        put64(tail, 44);
        put16(tail, 45);
        put16(tail, 45);
        put32(tail, 0);
        put32(tail, 0);
        put64(tail, entries.size());
        put64(tail, entries.size());
        put64(tail, zip64Eocd - centralOffset);
        put64(tail, centralOffset);
        put32(tail, 0x07064B50);
        put32(tail, 0);
        put64(tail, zip64Eocd);
        put32(tail, 1);
    }
    const uint64_t centralSize = (anyZip64 ? tail.size() - 76 : tail.size());
    put32(tail, 0x06054B50);
    put16(tail, 0);
    put16(tail, 0);
    put16(tail, anyZip64 ? 0xFFFF : uint32_t(entries.size()));
    put16(tail, anyZip64 ? 0xFFFF : uint32_t(entries.size()));
    put32(tail, anyZip64 ? kMax32 : uint32_t(centralSize));
    put32(tail, anyZip64 ? kMax32 : uint32_t(centralOffset));
    put16(tail, 0);
    if (!f.write(reinterpret_cast<const char*>(tail.data()), std::streamsize(tail.size())) || !f.flush()) {
        error = "Cannot write " + path;
        return false;
    }
    return true;
}

bool readStoredZip(const std::string& path, std::vector<ZipEntry>& entries, std::string& error) {
    std::vector<ZipFileEntry> index;
    if (!readStoredZipIndex(path, index, error)) return false;
    std::ifstream f(fsPath(path), std::ios::binary);
    entries.clear();
    for (const ZipFileEntry& z : index) {
        ZipEntry e;
        e.name = z.name;
        e.data.resize(size_t(z.size));
        f.clear();
        f.seekg(std::streamoff(z.offset));
        if (z.size && !f.read(reinterpret_cast<char*>(e.data.data()), std::streamsize(z.size))) {
            error = "Truncated ZIP entry " + e.name;
            return false;
        }
        entries.push_back(std::move(e));
    }
    return true;
}

bool readStoredZipIndex(const std::string& path, std::vector<ZipFileEntry>& entries, std::string& error) {
    std::ifstream f(fsPath(path), std::ios::binary);
    if (!f) {
        error = "Cannot open " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    const uint64_t fileSize = uint64_t(f.tellg());
    // Reads `n` bytes at `at` (false past the end).
    auto readAt = [&](uint64_t at, size_t n, void* out) {
        if (at > fileSize || n > fileSize - at) return false;
        f.clear();
        f.seekg(std::streamoff(at));
        return bool(f.read(static_cast<char*>(out), std::streamsize(n)));
    };
    // Find the end-of-central-directory record (in the last 64 KB + 22 bytes).
    const size_t tailSize = size_t(std::min<uint64_t>(fileSize, 65557));
    std::vector<uint8_t> tail(tailSize);
    if (!readAt(fileSize - tailSize, tailSize, tail.data())) {
        error = path + " is not a ZIP/USDZ archive";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = tailSize >= 22 ? tailSize - 22 + 1 : 0; i-- > 0;)
        if (get32(&tail[i]) == 0x06054B50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) {
        error = path + " is not a ZIP/USDZ archive";
        return false;
    }
    uint64_t count = get16(&tail[eocd + 10]);
    uint64_t centralSize = get32(&tail[eocd + 12]);
    uint64_t centralOffset = get32(&tail[eocd + 16]);
    // ZIP64: the locator sits right before the record and points at the ZIP64 end of central directory.
    if (eocd >= 20 && get32(&tail[eocd - 20]) == 0x07064B50) {
        uint8_t rec[56];
        if (!readAt(get64(&tail[eocd - 12]), sizeof rec, rec) || get32(rec) != 0x06064B50) {
            error = "Corrupt ZIP64 end of central directory";
            return false;
        }
        count = get64(rec + 32);
        centralSize = get64(rec + 40);
        centralOffset = get64(rec + 48);
    }
    std::vector<uint8_t> z(size_t(std::min<uint64_t>(centralSize, fileSize)));
    if (!readAt(centralOffset, z.size(), z.data())) {
        error = "Corrupt ZIP central directory";
        return false;
    }
    size_t p = 0;
    entries.clear();
    for (uint64_t k = 0; k < count; ++k) {
        if (p + 46 > z.size() || get32(&z[p]) != 0x02014B50) {
            error = "Corrupt ZIP central directory";
            return false;
        }
        const uint32_t method = get16(&z[p + 10]);
        uint64_t size = get32(&z[p + 20]);
        const uint64_t compressed = get32(&z[p + 24]);
        const size_t nameLen = get16(&z[p + 28]), extraLen = get16(&z[p + 30]), commentLen = get16(&z[p + 32]);
        uint64_t local = get32(&z[p + 42]);
        if (p + 46 + nameLen + extraLen > z.size()) {
            error = "Corrupt ZIP central directory";
            return false;
        }
        ZipFileEntry e;
        e.name.assign(reinterpret_cast<const char*>(&z[p + 46]), nameLen);
        // ZIP64 extra field: 64-bit values for whichever of these were saturated, in this order.
        for (size_t x = p + 46 + nameLen; x + 4 <= p + 46 + nameLen + extraLen;) {
            const uint32_t id = get16(&z[x]), len = get16(&z[x + 2]);
            if (x + 4 + len > p + 46 + nameLen + extraLen) break;
            if (id == 0x0001) {
                size_t q = x + 4;
                auto take = [&](uint64_t& v) {
                    if (v == kMax32 && q + 8 <= x + 4 + len) {
                        v = get64(&z[q]);
                        q += 8;
                    }
                };
                uint64_t c = compressed;
                take(size);
                take(c);
                take(local);
            }
            x += 4 + len;
        }
        p += 46 + nameLen + extraLen + commentLen;
        if (method != 0) {
            error = "'" + e.name + "' is compressed; USDZ files must be uncompressed";
            return false;
        }
        uint8_t lh[30];
        if (!readAt(local, sizeof lh, lh) || get32(lh) != 0x04034B50) {
            error = "Corrupt ZIP entry " + e.name;
            return false;
        }
        const uint64_t data = local + 30 + get16(lh + 26) + get16(lh + 28);
        if (data > fileSize || size > fileSize - data) {
            error = "Truncated ZIP entry " + e.name;
            return false;
        }
        e.offset = data;
        e.size = size;
        entries.push_back(std::move(e));
    }
    return true;
}

} // namespace sf
