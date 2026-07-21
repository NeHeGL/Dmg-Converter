/*
 * dmg-converter.h — DMG (UDIF) parsing & ISO conversion
 * Part of dmgconverter  —  2026 Jeff Molofee (NeHe)
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// ── Block types ───────────────────────────────────────────────────────────────
static constexpr uint32_t UDIF_SIGNATURE  = 0x6B6F6C79u; // 'koly'
static constexpr uint32_t SECTOR_SIZE     = 512;

static constexpr uint32_t BLOCK_ZLIB      = 0x80000005u;
static constexpr uint32_t BLOCK_RAW       = 0x00000001u;
static constexpr uint32_t BLOCK_IGNORE    = 0x00000002u;
static constexpr uint32_t BLOCK_COMMENT   = 0x7FFFFFFEu;
static constexpr uint32_t BLOCK_TERM      = 0xFFFFFFFFu;
static constexpr uint32_t BLOCK_BZIP2     = 0x80000006u;
static constexpr uint32_t BLOCK_LZFSE    = 0x80000007u;
static constexpr uint32_t BLOCK_ZERO      = 0x00000000u;
static constexpr uint32_t BLOCK_ADC       = 0x80000004u;

// ── Structures ────────────────────────────────────────────────────────────────
struct BlkxRun {
    uint32_t type;
    uint32_t comment;
    uint64_t sectorStart;
    uint64_t sectorCount;
    uint64_t compOffset;
    uint64_t compLength;
};

struct BlkxTable {
    uint64_t             sectorNumber;
    uint64_t             sectorCount;
    uint64_t             dataOffset;
    std::vector<BlkxRun> runs;
};

struct Partition {
    int         index;
    std::string name;
    uint64_t    sectors;
    uint64_t    startSector;
    double      sizeMB;
    int         runs;
};

// Progress callback: (percent 0-100, message)
using ProgressCb = std::function<void(int, const std::string&)>;

// ── Public API ────────────────────────────────────────────────────────────────

// Convert dmgPath → isoPath.  Throws std::runtime_error on failure.
void convertDmgToIso(const std::string& dmgPath,
                     const std::string& isoPath,
                     ProgressCb         cb = nullptr);

// List partitions in a DMG.  Throws std::runtime_error on failure.
std::vector<Partition> listPartitions(const std::string& dmgPath);
