#include "WdtParser.hpp"

#include <algorithm>

namespace world::terrain
{
    using namespace world::terrain::internal;

    bool ParseWdt(const uint8_t* data, size_t size, WdtData& out)
    {
        if (!data || size < 8)
        {
            return false;
        }

        out = WdtData{};

        const uint8_t* mwmo = nullptr;
        uint32_t mwmoSize = 0;
        const uint8_t* modf = nullptr;
        uint32_t modfSize = 0;

        bool truncated = false;
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

            if (TagIs(tag, "MPHD"))
            {
                if (csize >= 4)
                {
                    out.mphdFlags = RdU32(body);
                    out.hasGlobalWmo = (out.mphdFlags & 0x0001) != 0;
                }
            }
            else if (TagIs(tag, "MAIN"))
            {
                constexpr size_t ENTRY = 8;
                const size_t entries = std::min<size_t>(csize / ENTRY, 64ULL * 64);
                for (size_t i = 0; i < entries; ++i)
                {
                    // Row-major in tx (the tile index from world X). See WdtData::HasAdt.
                    out.adtGrid[i / 64][i % 64] = (RdU32(body + i * ENTRY) & 0x1) != 0;
                }
                out.hasMainChunk = true;
            }
            else if (TagIs(tag, "MWMO"))
            {
                mwmo = body;
                mwmoSize = csize;
            }
            else if (TagIs(tag, "MODF"))
            {
                modf = body;
                modfSize = csize;
            }

            pos += 8 + csize;
        }

        if (out.hasGlobalWmo && mwmo && mwmoSize > 0)
        {
            const char* s = reinterpret_cast<const char*>(mwmo);
            uint32_t len = 0;
            while (len < mwmoSize && s[len] != '\0')
            {
                ++len;
            }
            out.globalWmoName.assign(s, len);
        }

        if (out.hasGlobalWmo && modf && modfSize >= 64)
        {
            out.globalWmoPlacement = ReadModf(modf);
        }

        // A WDT CUT INSIDE MAIN IS NOT A MAP WITHOUT TERRAIN. Breaking out and answering
        // true recorded no grid entries, so Wdt() cached it as a valid no-terrain map and
        // BakeMap passed over it -- a full extraction omitting that map's whole tile and
        // nav cache, at exit 0. `pos != size` catches the other half: a walk that stops on
        // 1-7 bytes of a partial header never enters the loop body to notice.
        return !truncated && pos == size;
    }
}
