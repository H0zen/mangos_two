#include <vector>
#include "terrain/WmoModel.hpp"

#include <algorithm>

namespace world::terrain
{
    WmoModel::WmoModel(TriSoup soup, std::vector<uint16_t> triGroup,
                       std::vector<Group> groups, uint32_t rootWmoId, Bvh bvh)
        : m_triGroup(std::move(triGroup)), m_groups(std::move(groups)), m_rootId(rootWmoId)
    {
        m_soup = std::move(soup);
        m_bvh = std::move(bvh);
        if (m_bvh.Empty() && !m_soup.tris.empty())
        {
            m_bvh.Build(m_soup, &m_triGroup, 4);
        }
        DeriveWmoBounds();
    }

    void WmoModel::DeriveWmoBounds()
    {
        DeriveBounds();

        // Liquid is content too, so a liquid-only group is not "empty", and its footprint
        // has to be inside the bounds for the column cull to ever reach it.
        for (const Group& g : m_groups)
        {
            if (!g.hasLiquid || g.liquid.heights.empty())
            {
                continue;
            }
            m_empty = false;

            float zmin = g.liquid.heights[0], zmax = g.liquid.heights[0];
            for (float hz : g.liquid.heights)
            {
                zmin = std::min(zmin, hz);
                zmax = std::max(zmax, hz);
            }
            const Vec3 c = g.liquid.corner;
            m_bounds.expand({c.x, c.y, zmin});
            m_bounds.expand({c.x + g.liquid.tilesX * WMO_LIQUID_TILE_SIZE,
                             c.y + g.liquid.tilesY * WMO_LIQUID_TILE_SIZE, zmax});
        }
    }

    std::optional<WmoModel::AreaResult> WmoModel::AreaInfo(const Vec3& origin, const Vec3& dir,
                                                           float tMax) const
    {
        uint32_t tri = 0;
        const auto t = m_bvh.Raycast(m_soup, origin, dir, tMax, &tri);
        if (!t || tri >= m_triGroup.size())
        {
            return std::nullopt;
        }
        const uint16_t gi = m_triGroup[tri];
        if (gi >= m_groups.size())
        {
            return std::nullopt;
        }
        return AreaResult{m_groups[gi].groupWmoId, m_groups[gi].mogpFlags, *t};
    }

    std::optional<ICollisionModel::LocalLiquid> WmoModel::GroupLiquidAt(const Group& g,
                                                                        const Vec3& p) const
    {
        if (!g.hasLiquid)
        {
            return std::nullopt;
        }
        const Liquid& lq = g.liquid;
        if (!lq.tilesX || !lq.tilesY || lq.heights.empty())
        {
            return std::nullopt;
        }

        const float txf = (p.x - lq.corner.x) / WMO_LIQUID_TILE_SIZE;
        const float tyf = (p.y - lq.corner.y) / WMO_LIQUID_TILE_SIZE;
        const int tx = int(txf), ty = int(tyf);
        if (txf < 0.f || tyf < 0.f || tx >= int(lq.tilesX) || ty >= int(lq.tilesY))
        {
            return std::nullopt;
        }

        const size_t fi = size_t(tx) + size_t(ty) * lq.tilesX;
        if (fi < lq.flags.size() && (lq.flags[fi] & 0x0F) == 0x0F)
        {
            return std::nullopt;
        }

        const float dx = txf - tx, dy = tyf - ty;
        const uint32_t row = lq.tilesX + 1;
        auto H = [&](int a, int b) { return lq.heights[size_t(a) + size_t(b) * row]; };

        LocalLiquid out;
        if (dx > dy)
        {
            const float sx = H(tx + 1, ty) - H(tx, ty);
            const float sy = H(tx + 1, ty + 1) - H(tx + 1, ty);
            out.z = H(tx, ty) + dx * sx + dy * sy;
        }
        else
        {
            const float sx = H(tx + 1, ty + 1) - H(tx, ty + 1);
            const float sy = H(tx, ty + 1) - H(tx, ty);
            out.z = H(tx, ty) + dx * sx + dy * sy;
        }
        out.entry = lq.entry;
        out.kind = lq.kind;
        return out;
    }

    void WmoModel::LiquidsLocal(const Vec3& p, std::vector<LocalLiquid>& out) const
    {
        // Every group with water over this column, in serialized order. Choosing one here
        // is unanswerable from inside the model: p is a point on the sweep column, not the
        // queried position, so "which room" cannot be decided here.
        for (const Group& g : m_groups)
        {
            if (auto cur = GroupLiquidAt(g, p))
            {
                out.push_back(*cur);
            }
        }
    }
}
