#include "AdtParser.hpp"

namespace world::terrain
{
    namespace
    {
        using namespace world::terrain::internal;

        constexpr int CELL = ADT_CELLS_PER_CHUNK;

        // MCNK header field offsets, relative to the byte after the chunk's tag+size.
        // Identical in 2.4.3 and 3.3.5a.
        constexpr size_t MCNK_FLAGS = 0x00;
        constexpr size_t MCNK_INDEX_X = 0x04;
        constexpr size_t MCNK_INDEX_Y = 0x08;
        constexpr size_t MCNK_OFS_MCVT = 0x14;
        constexpr size_t MCNK_AREA_ID = 0x34;
        constexpr size_t MCNK_HOLES = 0x3C;
        constexpr size_t MCNK_OFS_MCLQ = 0x60;
        constexpr size_t MCNK_SIZE_MCLQ = 0x64;
        constexpr size_t MCNK_POS_Z = 0x70;

        constexpr uint32_t MCNK_FLAG_RIVER = 1u << 2;
        constexpr uint32_t MCNK_FLAG_OCEAN = 1u << 3;
        constexpr uint32_t MCNK_FLAG_MAGMA = 1u << 4;
        constexpr uint32_t MCNK_FLAG_SLIME = 1u << 5;

        constexpr uint8_t MCLQ_NO_LIQUID = 0x0F;
        constexpr uint8_t MCLQ_DARK = 0x80;

        /// The fixed part of an MCNK record, after the 8-byte tag+size.
        constexpr uint32_t MCNK_HEADER = 128;

        void EnsureLiquid(AdtData& out)
        {
            if (!out.liquidHeight.empty())
            {
                return;
            }
            out.liquidHeight.assign(size_t(ADT_V9) * ADT_V9, 0.0f);
            out.liquidShow.assign(size_t(ADT_GRID) * ADT_GRID, 0);
            out.liquidEntry.assign(size_t(ADT_GRID) * ADT_GRID, 0);
            out.liquidDark.assign(size_t(ADT_GRID) * ADT_GRID, 0);
            out.liquidDeepAttr.assign(size_t(ADT_GRID) * ADT_GRID, 0);
        }

        using ChunkSeen = std::array<bool, size_t(ADT_CHUNKS) * ADT_CHUNKS>;

        /// False when the record is unreadable, which FAILS THE WHOLE PARSE rather than
        /// skipping the chunk: the height arrays are zero-filled before the walk, so a
        /// chunk quietly left out is not a hole in the tile -- it is 8x8 cells of flat
        /// ground at 0.0, baked and counted as a successful map square. Marks its own
        /// index in @p filled, which is how the caller knows the grid is whole.
        bool ReadMcnk(const uint8_t* mcnk, uint32_t mcnkSize, AdtData& out,
                      ChunkSeen& filled)
        {
            // Every field below is read from the 128-byte header, and the caller has only
            // proved that the record's DECLARED size fits the file. A record shorter than
            // its own header reads past the end of the last chunk in the archive.
            if (mcnkSize < MCNK_HEADER)
            {
                return false;
            }

            const uint8_t* h = mcnk + 8;
            const uint32_t flags = RdU32(h + MCNK_FLAGS);
            const uint32_t ix = RdU32(h + MCNK_INDEX_X);
            const uint32_t iy = RdU32(h + MCNK_INDEX_Y);
            if (ix > 15 || iy > 15)
            {
                return false;
            }

            const uint32_t offsMcvt = RdU32(h + MCNK_OFS_MCVT);
            const uint32_t offsMclq = RdU32(h + MCNK_OFS_MCLQ);
            const uint32_t sizeMclq = RdU32(h + MCNK_SIZE_MCLQ);
            const float baseZ = RdF32(h + MCNK_POS_Z);
            const uint32_t span = mcnkSize + 8;

            out.holes[iy * ADT_CHUNKS + ix] = static_cast<uint16_t>(RdU32(h + MCNK_HOLES) & 0xFFFF);
            out.areaIds[iy * ADT_CHUNKS + ix] = static_cast<uint16_t>(RdU32(h + MCNK_AREA_ID) & 0xFFFF);

            // MCVT is the chunk's ENTIRE contribution to the heightmap, and the grid was
            // zero-filled before the walk, so a chunk without one is not a gap in the
            // tile -- it is 9x9 corners of flat ground at 0.0 that nothing downstream can
            // tell from real terrain. Skipping it here is what let a corrupt ADT bake.
            if (!offsMcvt || uint64_t(offsMcvt) + 8 + 145 * 4 > uint64_t(span))
            {
                return false;
            }

            // MCVT is 145 floats: 9 V9 corners then 8 V8 centres, per row.
            const uint8_t* hm = mcnk + offsMcvt + 8;
            for (int y = 0; y <= CELL; ++y)
            {
                const int cy = int(iy) * CELL + y;
                for (int x = 0; x <= CELL; ++x)
                {
                    const int cx = int(ix) * CELL + x;
                    out.v9[cy * ADT_V9 + cx] = baseZ + RdF32(hm + (y * 17 + x) * 4);
                }
            }
            for (int y = 0; y < CELL; ++y)
            {
                const int cy = int(iy) * CELL + y;
                for (int x = 0; x < CELL; ++x)
                {
                    const int cx = int(ix) * CELL + x;
                    out.v8[cy * ADT_GRID + cx] = baseZ + RdF32(hm + (y * 17 + 9 + x) * 4);
                }
            }
            filled[iy * ADT_CHUNKS + ix] = true;

            // MCLQ is the pre-WotLK liquid chunk and is absent from 3.3.5a client data;
            // it is read only so an older or hand-made tile is not silently dry. MH2O,
            // parsed after every MCNK, overrides whatever this writes.
            // Summed in 64 bits: offsMclq comes straight off the file, and a value near
            // UINT32_MAX makes the 32-bit sum WRAP BELOW span, so the guard passes and
            // mcnk + offsMclq reads outside the buffer.
            constexpr uint64_t MCLQ_BYTES = 8 + 81 * 8 + 64;
            if (!offsMclq || sizeMclq <= 8 ||
                uint64_t(offsMclq) + 8 + MCLQ_BYTES > uint64_t(span))
            {
                return true;      // no MCLQ is the 3.3.5a norm, and the heights are read
            }

            const uint8_t* lq = mcnk + offsMclq + 8;
            const uint8_t* heights = lq + 8;
            const uint8_t* lqFlags = lq + 8 + 81 * 8;

            EnsureLiquid(out);
            out.hasLiquid = true;

            // In the MCLQ era the MCNK flags *are* the type; the DBC is not involved.
            // Dropping the slime case makes Undercity sludge report as swimmable water.
            uint16_t entry = 1;
            if (flags & MCNK_FLAG_SLIME)
            {
                entry = 4;
            }
            else if (flags & MCNK_FLAG_MAGMA)
            {
                entry = 3;
            }
            else if (flags & MCNK_FLAG_OCEAN)
            {
                entry = 2;
            }
            else if (!(flags & MCNK_FLAG_RIVER))
            {
                entry = 1;
            }

            for (int y = 0; y <= CELL; ++y)
            {
                const int cy = int(iy) * CELL + y;
                for (int x = 0; x <= CELL; ++x)
                {
                    const int cx = int(ix) * CELL + x;
                    out.liquidHeight[cy * ADT_V9 + cx] = RdF32(heights + (y * 9 + x) * 8 + 4);
                }
            }
            for (int y = 0; y < CELL; ++y)
            {
                const int cy = int(iy) * CELL + y;
                for (int x = 0; x < CELL; ++x)
                {
                    const int cx = int(ix) * CELL + x;
                    const uint8_t cellFlags = lqFlags[y * CELL + x];
                    if (cellFlags == MCLQ_NO_LIQUID)
                    {
                        continue;
                    }
                    const size_t idx = size_t(cy) * ADT_GRID + cx;
                    out.liquidShow[idx] = 1;
                    out.liquidEntry[idx] = entry;
                    out.liquidDark[idx] = (cellFlags & MCLQ_DARK) ? 1 : 0;
                }
            }
            return true;
        }

        // MH2O: 256 SMLiquidChunk headers, one per MCNK in IndexY-major order, followed
        // by the instances they point at. Every offset inside is relative to the byte
        // after the chunk tag+size, never to the file.
        void ReadMh2o(const uint8_t* body, uint32_t bodySize, AdtData& out)
        {
            constexpr uint32_t CHUNK_HDR = 12;
            constexpr uint32_t INSTANCE = 24;
            if (bodySize < CHUNK_HDR * ADT_CHUNKS * ADT_CHUNKS)
            {
                return;
            }

            for (int iy = 0; iy < ADT_CHUNKS; ++iy)
            {
                for (int ix = 0; ix < ADT_CHUNKS; ++ix)
                {
                    const uint8_t* ch = body + size_t(iy * ADT_CHUNKS + ix) * CHUNK_HDR;
                    const uint32_t offsInstances = RdU32(ch + 0);
                    const uint32_t layers = RdU32(ch + 4);
                    if (!offsInstances || !layers)
                    {
                        continue;
                    }
                    if (uint64_t(offsInstances) + uint64_t(layers) * INSTANCE > bodySize)
                    {
                        continue;
                    }

                    // SMLiquidChunk's third field points at
                    // mh2o_chunk_attributes { uint64 fishable; uint64 deep; }, two 8x8
                    // bit masks. wowdev ADT/v18: "can be ommitted for all-0", so absent
                    // means NOT deep. This is the real source for dark water; the
                    // vertex-format guess it replaced is not an attribute at all.
                    const uint32_t offsAttributes = RdU32(ch + 8);
                    uint64_t deepBits = 0;
                    if (offsAttributes && uint64_t(offsAttributes) + 16 <= bodySize)
                    {
                        deepBits = RdU64(body + offsAttributes + 8);
                    }

                    for (uint32_t l = 0; l < layers; ++l)
                    {
                        const uint8_t* inst = body + offsInstances + l * INSTANCE;
                        const uint16_t entry = RdU16(inst + 0);
                        const uint16_t lvf = RdU16(inst + 2);
                        const float minHeight = RdF32(inst + 4);
                        const uint8_t xOfs = inst[12];
                        const uint8_t yOfs = inst[13];
                        const uint8_t w = inst[14];
                        const uint8_t hgt = inst[15];
                        const uint32_t offsExists = RdU32(inst + 16);
                        const uint32_t offsVerts = RdU32(inst + 20);

                        if (!w || !hgt || xOfs + w > CELL || yOfs + hgt > CELL)
                        {
                            continue;
                        }

                        // Vertex format 2 is depth-only and carries no heights.
                        const bool hasHeights = (lvf != 2);
                        const uint32_t corners = uint32_t(w + 1) * uint32_t(hgt + 1);
                        const uint8_t* heights = nullptr;
                        if (hasHeights && offsVerts &&
                            uint64_t(offsVerts) + uint64_t(corners) * 4 <= bodySize)
                        {
                            heights = body + offsVerts;
                        }

                        const uint8_t* exists = nullptr;
                        const uint32_t existsBytes = (uint32_t(w) * hgt + 7) / 8;
                        if (offsExists && uint64_t(offsExists) + existsBytes <= bodySize)
                        {
                            exists = body + offsExists;
                        }

                        EnsureLiquid(out);
                        out.hasLiquid = true;
                        out.hasMh2o = true;

                        for (int y = 0; y < hgt; ++y)
                        {
                            const int cy = iy * CELL + yOfs + y;
                            for (int x = 0; x < w; ++x)
                            {
                                const int bit = y * w + x;
                                if (exists && !((exists[bit >> 3] >> (bit & 7)) & 1u))
                                {
                                    continue;
                                }
                                const int cx = ix * CELL + xOfs + x;
                                const size_t idx = size_t(cy) * ADT_GRID + cx;

                                // Indexed over the CHUNK's own 8x8 cells, not over this
                                // instance's rectangle. A shore where only the outer cells
                                // fatigue carries some bits set and some clear, so testing
                                // the mask as a boolean drowns the shallows with it.
                                const int deepBit = (yOfs + y) * CELL + (xOfs + x);

                                out.liquidShow[idx] = 1;
                                out.liquidEntry[idx] = entry;
                                out.liquidDark[idx] = 0;
                                out.liquidDeepAttr[idx] =
                                    uint8_t((deepBits >> deepBit) & 1ull);
                            }
                        }

                        for (int y = 0; y <= hgt; ++y)
                        {
                            const int cy = iy * CELL + yOfs + y;
                            for (int x = 0; x <= w; ++x)
                            {
                                const int cx = ix * CELL + xOfs + x;
                                const float z = heights
                                                    ? RdF32(heights + (y * (w + 1) + x) * 4)
                                                    : minHeight;
                                out.liquidHeight[size_t(cy) * ADT_V9 + cx] = z;
                            }
                        }
                    }
                }
            }
        }
    }

    bool ParseAdt(const uint8_t* data, size_t size, AdtData& out)
    {
        if (!data || size < 8)
        {
            return false;
        }

        const uint8_t* mwmo = nullptr;
        uint32_t mwmoSize = 0;
        const uint8_t* mwid = nullptr;
        uint32_t mwidSize = 0;
        const uint8_t* mmdx = nullptr;
        uint32_t mmdxSize = 0;
        const uint8_t* mmid = nullptr;
        uint32_t mmidSize = 0;
        const uint8_t* mh2o = nullptr;
        uint32_t mh2oSize = 0;

        bool sawMcnk = false;
        bool truncated = false;
        ChunkSeen filled{};
        size_t pos = 0;
        while (pos + 8 <= size)
        {
            const uint8_t* tag = data + pos;
            const uint32_t csize = RdU32(data + pos + 4);
            const uint8_t* body = data + pos + 8;
            if (static_cast<uint64_t>(pos) + 8 + csize > size)
            {
                truncated = true;
                break;
            }

            if (TagIs(tag, "MCNK"))
            {
                if (!sawMcnk)
                {
                    out.v9.assign(size_t(ADT_V9) * ADT_V9, 0.0f);
                    out.v8.assign(size_t(ADT_GRID) * ADT_GRID, 0.0f);
                    out.holes.fill(0);
                    out.areaIds.fill(0);
                    sawMcnk = true;
                }
                if (!ReadMcnk(tag, csize, out, filled))
                {
                    return false;
                }
            }
            else if (TagIs(tag, "MH2O"))
            {
                mh2o = body;
                mh2oSize = csize;
            }
            else if (TagIs(tag, "MWMO"))
            {
                mwmo = body;
                mwmoSize = csize;
            }
            else if (TagIs(tag, "MWID"))
            {
                mwid = body;
                mwidSize = csize;
            }
            else if (TagIs(tag, "MMDX"))
            {
                mmdx = body;
                mmdxSize = csize;
            }
            else if (TagIs(tag, "MMID"))
            {
                mmid = body;
                mmidSize = csize;
            }
            else if (TagIs(tag, "MODF"))
            {
                for (uint32_t o = 0; o + 64 <= csize; o += 64)
                {
                    out.wmoPlacements.push_back(ReadModf(body + o));
                }
            }
            else if (TagIs(tag, "MDDF"))
            {
                for (uint32_t o = 0; o + 36 <= csize; o += 36)
                {
                    out.m2Placements.push_back(ReadMddf(body + o));
                }
            }

            pos += 8 + csize;
        }

        // A chunk declaring more bytes than the file holds is a truncated file, and the
        // half that noticed was only ever the terrain one: `filled` catches a cut-off
        // MCNK grid below. A tile cut inside MODF or MDDF returned true with the
        // remaining placements silently absent -- and the caller then hands back a tile
        // that counts as written, with the WMO and M2 collision simply not in it.
        if (truncated)
        {
            return false;
        }

        if (mh2o)
        {
            ReadMh2o(mh2o, mh2oSize, out);
        }
        if (mwmo && mwid)
        {
            out.wmoNames = ResolveNames(mwmo, mwmoSize, mwid, mwidSize);
        }
        if (mmdx && mmid)
        {
            out.m2Names = ResolveNames(mmdx, mmdxSize, mmid, mmidSize);
        }

        // hasTerrain HAS to mean the heightmap was read, and sawMcnk only meant a chunk
        // tag went past. The grid is 16x16 disjoint 9x9 blocks, so one chunk missing --
        // absent, indexed twice, or cut off by a truncated file -- leaves a block of flat
        // 0.0 in an otherwise real tile, and every check downstream sees a tile that
        // parsed. Partial is not terrain.
        if (sawMcnk)
        {
            for (const bool ok : filled)
            {
                if (!ok)
                {
                    return false;
                }
            }
            out.hasTerrain = true;
        }
        return true;
    }
}
