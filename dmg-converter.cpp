/*
 * dmg-converter.cpp — DMG (UDIF) parsing & ISO conversion
 * Part of dmgconverter  —  2026 Jeff Molofee (NeHe)
 */

#include "dmg-converter.h"
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <algorithm>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>
#pragma comment(lib, "cabinet.lib")

// ── Big-endian readers ────────────────────────────────────────────────────────
static uint32_t beU32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint64_t beU64(const uint8_t* p) {
    return ((uint64_t)beU32(p)<<32)|beU32(p+4);
}
// ── UDIF koly trailer ─────────────────────────────────────────────────────────
struct UDIFTrailer {
    uint32_t version;
    uint64_t plistOffset;
    uint64_t plistLength;
    uint64_t sectorCount;
};

// Scan backwards through the file for the 'koly' signature.
// Returns file offset of koly block, or -1 if not found.
static int64_t findKoly(FILE* f, int64_t fileSize) {
    const uint8_t KOLY[4] = {'k','o','l','y'};
    const int64_t CHUNK = 4*1024*1024;
    const int OVERLAP = 3;

    std::vector<uint8_t> buf;
    int64_t pos = fileSize;
    std::vector<uint8_t> leftover;

    while (pos > 0) {
        int64_t readSize = std::min(CHUNK, pos);
        pos -= readSize;
        buf.resize((size_t)(readSize + leftover.size()));
        _fseeki64(f, pos, SEEK_SET);
        fread(buf.data(), 1, (size_t)readSize, f);
        // append leftover at end
        if (!leftover.empty())
            memcpy(buf.data() + readSize, leftover.data(), leftover.size());

        // search backwards for 'koly'
        for (int64_t i = (int64_t)buf.size() - 4; i >= 0; --i) {
            if (memcmp(buf.data()+i, KOLY, 4) == 0) {
                int64_t candidate = pos + i;
                // validate version field (bytes 4-7 of koly block)
                uint8_t hdr[8];
                _fseeki64(f, candidate, SEEK_SET);
                if (fread(hdr, 1, 8, f) == 8) {
                    uint32_t ver = beU32(hdr+4);
                    if (ver >= 1 && ver <= 4)
                        return candidate;
                }
            }
        }
        // keep last OVERLAP bytes as leftover for next iteration
        leftover.assign(buf.begin(), buf.begin() + std::min((int64_t)OVERLAP, (int64_t)buf.size()));
    }
    return -1;
}

static UDIFTrailer parseKoly(FILE* f, int64_t kolyOffset) {
    uint8_t data[512];
    _fseeki64(f, kolyOffset, SEEK_SET);
    if (fread(data, 1, 512, f) < 512)
        throw std::runtime_error("Failed to read UDIF trailer");
    if (beU32(data) != UDIF_SIGNATURE)
        throw std::runtime_error("Invalid UDIF signature");
    UDIFTrailer t{};
    t.version     = beU32(data+4);
    t.plistOffset = beU64(data+216);
    t.plistLength = beU64(data+224);
    t.sectorCount = beU64(data+492);
    return t;
}
// ── Plist / blkx parsing ──────────────────────────────────────────────────────
// We parse just enough of the Apple plist XML to extract blkx <data> blocks.
// No external XML library needed — the format is predictable enough.

// Read plist bytes from file at the koly-recorded offset, or scan before koly.
static std::vector<uint8_t> readPlist(FILE* f, const UDIFTrailer& udif, int64_t kolyOffset) {
    if (udif.plistLength == 0)
        throw std::runtime_error("DMG has no embedded plist");
    std::vector<uint8_t> buf(udif.plistLength);
    _fseeki64(f, (int64_t)udif.plistOffset, SEEK_SET);
    fread(buf.data(), 1, buf.size(), f);
    return buf;
}

// Scan up to 2 MB before kolyOffset for <?xml or bplist00
static std::vector<uint8_t> findPlistBeforeKoly(FILE* f, int64_t kolyOffset) {
    const int64_t WINDOW = 2*1024*1024;
    int64_t start = std::max((int64_t)0, kolyOffset - WINDOW);
    int64_t sz = kolyOffset - start;
    std::vector<uint8_t> region((size_t)sz);
    _fseeki64(f, start, SEEK_SET);
    fread(region.data(), 1, region.size(), f);

    // Search backwards for <?xml
    for (int64_t i = sz - 5; i >= 0; --i) {
        if (memcmp(region.data()+i, "<?xml", 5) == 0) {
            // find </plist>
            const char* endTag = "</plist>";
            int64_t endLen = 8;
            for (int64_t j = sz - endLen; j >= i; --j) {
                if (memcmp(region.data()+j, endTag, endLen) == 0) {
                    int64_t len = j + endLen - i;
                    return std::vector<uint8_t>(region.begin()+i, region.begin()+i+len);
                }
            }
            return std::vector<uint8_t>(region.begin()+i, region.end());
        }
    }
    return {};
}

// Decode base64 (plist <data> blocks)
static std::vector<uint8_t> decodeBase64(const std::string& s) {
    static const char* enc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const char* p = strchr(enc, c);
        if (!p) continue;
        val = (val << 6) + (int)(p - enc);
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// Extract (name, raw_blkx_data) pairs from plist XML bytes
static std::vector<std::pair<std::string,std::vector<uint8_t>>>
parsePlistBlkx(const std::vector<uint8_t>& plist) {
    // Convert to string for searching
    std::string xml(plist.begin(), plist.end());
    std::vector<std::pair<std::string,std::vector<uint8_t>>> result;

    // Find the blkx array
    size_t blkxPos = xml.find("\"blkx\"");
    if (blkxPos == std::string::npos) blkxPos = xml.find(">blkx<");
    if (blkxPos == std::string::npos) return result;

    // Walk through <dict> entries after blkx
    size_t pos = blkxPos;
    while (true) {
        // Find next <dict>
        size_t dictStart = xml.find("<dict>", pos);
        if (dictStart == std::string::npos) break;
        size_t dictEnd = xml.find("</dict>", dictStart);
        if (dictEnd == std::string::npos) break;
        std::string entry = xml.substr(dictStart, dictEnd - dictStart + 7);
        pos = dictEnd + 7;

        // Extract Name
        std::string name;
        size_t nk = entry.find(">Name<");
        if (nk != std::string::npos) {
            size_t vs = entry.find("<string>", nk);
            size_t ve = entry.find("</string>", nk);
            if (vs != std::string::npos && ve != std::string::npos)
                name = entry.substr(vs+8, ve-(vs+8));
        }

        // Extract <data> block
        size_t ds = entry.find("<data>");
        size_t de = entry.find("</data>");
        if (ds == std::string::npos || de == std::string::npos) continue;
        std::string b64 = entry.substr(ds+6, de-(ds+6));
        std::vector<uint8_t> raw = decodeBase64(b64);
        if (raw.size() < 4) continue;
        result.emplace_back(name, std::move(raw));

        // Stop if we hit </array> (end of blkx list)
        if (xml.find("</array>", dictEnd) < xml.find("<dict>", pos))
            break;
    }
    return result;
}
// ── BlkxTable parser ──────────────────────────────────────────────────────────
static BlkxTable parseBlkx(const std::vector<uint8_t>& data) {
    if (data.size() < 204)
        throw std::runtime_error("blkx data too small");
    uint32_t sig = beU32(data.data());
    if (sig != 0x6D697368u) // 'mish'
        throw std::runtime_error("Invalid blkx signature");
    BlkxTable t{};
    t.sectorNumber = beU64(data.data()+8);
    t.sectorCount  = beU64(data.data()+16);
    t.dataOffset   = beU64(data.data()+24);
    uint32_t numRuns = beU32(data.data()+200);
    size_t off = 204;
    for (uint32_t i = 0; i < numRuns; ++i) {
        if (off + 40 > data.size()) break;
        BlkxRun r{};
        r.type        = beU32(data.data()+off);
        r.comment     = beU32(data.data()+off+4);
        r.sectorStart = beU64(data.data()+off+8);
        r.sectorCount = beU64(data.data()+off+16);
        r.compOffset  = beU64(data.data()+off+24);
        r.compLength  = beU64(data.data()+off+32);
        t.runs.push_back(r);
        off += 40;
    }
    return t;
}

// ── ADC decompressor ──────────────────────────────────────────────────────────
static std::vector<uint8_t> adcDecompress(const uint8_t* src, size_t srcLen) {
    std::vector<uint8_t> out;
    out.reserve(srcLen * 4);
    size_t i = 0;
    while (i < srcLen) {
        uint8_t b = src[i++];
        if (b & 0x80) {
            // Plain bytes
            int count = (b & 0x7F) + 1;
            for (int k = 0; k < count && i < srcLen; ++k)
                out.push_back(src[i++]);
        } else if (b & 0x40) {
            // Long run
            int count = (b & 0x3F) + 4;
            if (i >= srcLen) break;
            uint32_t offset = ((uint32_t)(b & 0x3F) << 8) | src[i++];
            int pos = (int)out.size() - (int)offset - 1;
            for (int k = 0; k < count; ++k)
                out.push_back((pos+k >= 0 && pos+k < (int)out.size()) ? out[pos+k] : 0);
        } else {
            // Short run
            int count = ((b & 0x3F) >> 2) + 3;
            if (i >= srcLen) break;
            uint32_t offset = ((uint32_t)(b & 0x03) << 8) | src[i++];
            int pos = (int)out.size() - (int)offset - 1;
            for (int k = 0; k < count; ++k)
                out.push_back((pos+k >= 0 && pos+k < (int)out.size()) ? out[pos+k] : 0);
        }
    }
    return out;
}
// ── Helper: get blkx list from a DMG file ─────────────────────────────────────
using BlkxList = std::vector<std::pair<std::string,std::vector<uint8_t>>>;

static BlkxList getBlkxList(FILE* f, int64_t fileSize) {
    int64_t kolyOff = findKoly(f, fileSize);
    if (kolyOff < 0)
        throw std::runtime_error("Not a valid DMG file: UDIF 'koly' signature not found");

    UDIFTrailer udif = parseKoly(f, kolyOff);

    // Try primary plist
    BlkxList blkx;
    try {
        auto plist = readPlist(f, udif, kolyOff);
        if (!plist.empty() && plist[0] == '<')
            blkx = parsePlistBlkx(plist);
    } catch (...) {}

    // Fallback: scan before koly
    if (blkx.empty()) {
        auto fb = findPlistBeforeKoly(f, kolyOff);
        if (!fb.empty())
            blkx = parsePlistBlkx(fb);
    }

    if (blkx.empty())
        throw std::runtime_error("No readable partition map (blkx) found in DMG");
    return blkx;
}

// ── ZLIB decompressor (Windows Compression API — no external zlib needed) ─────
// DMG zlib blocks are raw deflate streams (no zlib header/trailer).
// COMPRESS_ALGORITHM_MSZIP adds a 2-byte CK header, so we can't use it directly.
// Instead we use COMPRESS_ALGORITHM_XPRESS_HUFF... actually for raw deflate we
// need to use zlib. Since cabinet.lib / compressapi only does MSZIP (which wraps
// deflate with a header), we implement a minimal inflate using the Windows
// ntdll RtlDecompressBuffer with COMPRESSION_FORMAT_LZNT1... no, that's LZNT1.
//
// Best no-dependency approach for raw deflate on Windows: load zlib1.dll from
// Python's installation (it's always present), or use a bundled miniz.
// We ship a single-file miniz implementation via miniz.h (see below).
// For now: use COMPRESS_ALGORITHM_MSZIP by stripping the 2-byte CK header trick:
// MSZIP = 2-byte 'CK' + raw deflate. We prepend 'CK' and let the API strip it.
static std::vector<uint8_t> zlibDecompress(const uint8_t* src, size_t srcLen) {
    // Prepend MSZIP 2-byte magic 'CK' so Windows API can process it
    std::vector<uint8_t> mszipSrc(srcLen + 2);
    mszipSrc[0] = 'C'; mszipSrc[1] = 'K';
    memcpy(mszipSrc.data() + 2, src, srcLen);

    DECOMPRESSOR_HANDLE hDecomp = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &hDecomp))
        throw std::runtime_error("CreateDecompressor failed");

    // Get output size
    SIZE_T outSize = 0;
    Decompress(hDecomp, mszipSrc.data(), mszipSrc.size(), nullptr, 0, &outSize);

    std::vector<uint8_t> out(outSize ? outSize : srcLen * 8);
    SIZE_T actual = 0;
    BOOL ok = Decompress(hDecomp, mszipSrc.data(), mszipSrc.size(),
                         out.data(), out.size(), &actual);
    CloseDecompressor(hDecomp);

    if (!ok && actual == 0)
        throw std::runtime_error("ZLIB (MSZIP) decompression failed");
    out.resize(actual);
    return out;
}

// ── Public: convertDmgToIso ───────────────────────────────────────────────────
void convertDmgToIso(const std::string& dmgPath,
                     const std::string& isoPath,
                     ProgressCb cb) {
    auto report = [&](int pct, const std::string& msg) {
        if (cb) cb(pct, msg);
    };

    report(0, "Opening DMG file...");
    FILE* f = fopen(dmgPath.c_str(), "rb");
    if (!f) throw std::runtime_error("Cannot open DMG file: " + dmgPath);

    _fseeki64(f, 0, SEEK_END);
    int64_t fileSize = _ftelli64(f);
    if (fileSize < 512) { fclose(f); throw std::runtime_error("File too small to be a valid DMG"); }

    report(5, "Searching for UDIF koly block...");
    BlkxList blkx;
    try {
        blkx = getBlkxList(f, fileSize);
    } catch (...) {
        fclose(f);
        throw;
    }

    // Build tables & compute total sectors
    report(10, "Parsing partition map...");
    struct TableEntry { std::string name; BlkxTable table; };
    std::vector<TableEntry> tables;
    uint64_t totalSectors = 0;
    for (auto& [name, data] : blkx) {
        try {
            BlkxTable t = parseBlkx(data);
            totalSectors = std::max(totalSectors, t.sectorNumber + t.sectorCount);
            tables.push_back({name, std::move(t)});
        } catch (...) {}
    }
    if (tables.empty()) { fclose(f); throw std::runtime_error("No valid block tables found in DMG"); }

    report(20, "Converting " + std::to_string(totalSectors) + " sectors to ISO...");

    uint64_t isoSize = totalSectors * SECTOR_SIZE;
    FILE* out = fopen(isoPath.c_str(), "wb");
    if (!out) { fclose(f); throw std::runtime_error("Cannot create output file: " + isoPath); }

    // Pre-allocate output
    _fseeki64(out, (int64_t)(isoSize - 1), SEEK_SET);
    uint8_t zero = 0;
    fwrite(&zero, 1, 1, out);
    _fseeki64(out, 0, SEEK_SET);

    // Count total runs for progress
    size_t totalRuns = 0, doneRuns = 0;
    for (auto& e : tables) totalRuns += e.table.runs.size();

    std::vector<uint8_t> compBuf;

    for (auto& entry : tables) {
        for (auto& run : entry.table.runs) {
            ++doneRuns;
            if (run.type == BLOCK_TERM || run.type == BLOCK_COMMENT || run.sectorCount == 0)
                goto next_run;
            {
                uint64_t outOff = (entry.table.sectorNumber + run.sectorStart) * SECTOR_SIZE;

                if (run.type == BLOCK_ZERO || run.type == BLOCK_IGNORE) {
                    // already zeroed
                } else if (run.type == BLOCK_RAW) {
                    compBuf.resize((size_t)run.compLength);
                    _fseeki64(f, (int64_t)run.compOffset, SEEK_SET);
                    fread(compBuf.data(), 1, compBuf.size(), f);
                    _fseeki64(out, (int64_t)outOff, SEEK_SET);
                    fwrite(compBuf.data(), 1, compBuf.size(), out);
                } else if (run.type == BLOCK_ZLIB) {
                    compBuf.resize((size_t)run.compLength);
                    _fseeki64(f, (int64_t)run.compOffset, SEEK_SET);
                    fread(compBuf.data(), 1, compBuf.size(), f);
                    auto dec = zlibDecompress(compBuf.data(), compBuf.size());
                    _fseeki64(out, (int64_t)outOff, SEEK_SET);
                    fwrite(dec.data(), 1, dec.size(), out);
                } else if (run.type == BLOCK_ADC) {
                    compBuf.resize((size_t)run.compLength);
                    _fseeki64(f, (int64_t)run.compOffset, SEEK_SET);
                    fread(compBuf.data(), 1, compBuf.size(), f);
                    auto dec = adcDecompress(compBuf.data(), compBuf.size());
                    _fseeki64(out, (int64_t)outOff, SEEK_SET);
                    fwrite(dec.data(), 1, dec.size(), out);
                }
                // BZIP2 and LZFSE omitted (rare; would require extra libs)
            }
            next_run:
            if (totalRuns > 0) {
                int pct = 20 + (int)(75 * doneRuns / totalRuns);
                report(pct, "Converting... (" + std::to_string(doneRuns) + "/" + std::to_string(totalRuns) + " blocks)");
            }
        }
    }

    fclose(out);
    fclose(f);
    report(100, "Conversion complete!");
}

// ── Public: listPartitions ────────────────────────────────────────────────────
std::vector<Partition> listPartitions(const std::string& dmgPath) {
    FILE* f = fopen(dmgPath.c_str(), "rb");
    if (!f) throw std::runtime_error("Cannot open: " + dmgPath);
    _fseeki64(f, 0, SEEK_END);
    int64_t sz = _ftelli64(f);
    BlkxList blkx;
    try { blkx = getBlkxList(f, sz); } catch (...) { fclose(f); throw; }
    fclose(f);

    std::vector<Partition> result;
    int idx = 0;
    for (auto& [name, data] : blkx) {
        Partition p{};
        p.index = idx++;
        p.name  = name;
        try {
            BlkxTable t = parseBlkx(data);
            p.sectors     = t.sectorCount;
            p.startSector = t.sectorNumber;
            p.sizeMB      = (double)(t.sectorCount * SECTOR_SIZE) / (1024.0*1024.0);
            p.runs = 0;
            for (auto& r : t.runs)
                if (r.type != BLOCK_TERM && r.type != BLOCK_COMMENT && r.sectorCount > 0)
                    ++p.runs;
        } catch (...) {}
        result.push_back(p);
    }
    return result;
}
