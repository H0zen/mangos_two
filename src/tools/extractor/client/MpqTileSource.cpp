#include <memory>
#include <string>
#include <vector>
#include "MpqTileSource.hpp"

#include "AdtParser.hpp"

#include <cmath>

namespace world::terrain
{
    namespace
    {
        constexpr float DEG2RAD = 3.14159265358979323846f / 180.0f;
        constexpr float MID = MAP_CENTER * TILE_SIZE;

        Aabb WorldBoundsOf(const Aabb& local, const Transform& xf)
        {
            Aabb out;
            if (!local.valid())
            {
                return out;
            }
            for (int i = 0; i < 8; ++i)
            {
                const Vec3 c{(i & 1) ? local.hi.x : local.lo.x,
                             (i & 2) ? local.hi.y : local.lo.y,
                             (i & 4) ? local.hi.z : local.lo.z};
                out.expand(xf.localToWorld(c));
            }
            return out;
        }

        // Placement into the same world frame the terrain uses. The 180 degrees added to
        // the Z euler is the diag(-1,-1,1) axis flip, which is exactly a half-turn.
        // Built through the three-argument constructor, never by assigning to .scale:
        // that constructor exists to clamp a non-positive or non-finite scale, and MDDF
        // stores scale as uint16/1024, which is legitimately 0 for a malformed record.
        // worldToLocal divides by it, so the whole model quietly stops being hit by any
        // ray instead of failing.
        Transform PlacementTransform(const Placement& p)
        {
            return Transform({MID - p.pos.z, MID - p.pos.x, p.pos.y},
                             Mat3::fromEuler(p.rotDeg.z * DEG2RAD, p.rotDeg.x * DEG2RAD,
                                             (p.rotDeg.y + 180.0f) * DEG2RAD),
                             p.scale);
        }

        // A WDT global WMO's MODF is already in world coordinates, so no re-centring.
        Transform GlobalWmoTransform(const Placement& p)
        {
            return Transform({p.pos.z, p.pos.x, p.pos.y},
                             Mat3::fromEuler(p.rotDeg.z * DEG2RAD, p.rotDeg.x * DEG2RAD,
                                             (p.rotDeg.y + 180.0f) * DEG2RAD),
                             p.scale);
        }

        // MODD's quaternion is authored against the M2's RAW model space, but M2Parser
        // stores hull vertices Y-negated. The rotation acting on the STORED vertices is
        // therefore R(quat) * diag(1,-1,1). Skip that and every doodad comes out mirrored
        // about its own Y axis -- which still overlaps its bounding box, so it looks
        // plausible and quietly puts the collision in the wrong place.
        Transform WmoDoodadTransform(const Transform& wmoXf, const WmoDoodad& d)
        {
            Mat3 r = Mat3::fromQuat(d.quat[0], d.quat[1], d.quat[2], d.quat[3]);
            r.m[1] = -r.m[1];
            r.m[4] = -r.m[4];
            r.m[7] = -r.m[7];

            // The PRODUCT is what gets clamped: either factor alone can be fine while
            // the product underflows to zero.
            return Transform(wmoXf.localToWorld(d.pos), Mat3::mulm(wmoXf.rot, r),
                             wmoXf.scale * d.scale);
        }
    }

    std::string MpqTileSource::MapDirectory(uint32_t mapId) const
    {
        if (m_maps)
        {
            if (const std::string* dir = m_maps->Find(mapId))
            {
                return *dir;
            }
        }
        return std::string();
    }

    std::string MpqTileSource::AdtPath(uint32_t mapId, int tx, int ty) const
    {
        const std::string name = MapDirectory(mapId);
        if (name.empty())
        {
            return std::string();
        }
        return "World\\Maps\\" + name + "\\" + name + "_" + std::to_string(ty) + "_" +
               std::to_string(tx) + ".adt";
    }

    std::string MpqTileSource::WdtPath(uint32_t mapId) const
    {
        const std::string name = MapDirectory(mapId);
        if (name.empty())
        {
            return std::string();
        }
        return "World\\Maps\\" + name + "\\" + name + ".wdt";
    }

    const WdtData* MpqTileSource::Wdt(uint32_t mapId)
    {
        auto it = m_wdtCache.find(mapId);
        if (it != m_wdtCache.end())
        {
            return &it->second;
        }

        const std::string path = WdtPath(mapId);
        if (path.empty())
        {
            return nullptr;                             // no directory: never had terrain
        }

        // ABSENT AND BROKEN ARE DIFFERENT ANSWERS, and this function can only give one of
        // them. Map.dbc lists identities that never had a WDT -- every `Transport<entry>`
        // vessel row has a directory and no WDT at all -- and BakeMap is right to pass
        // over those; a WDT the archive HAS and cannot serve or parse is a truncated or
        // stale-format client, and collapsing the two let a full extraction drop that
        // map's entire tile and nav cache and still exit 0. Hence Contains(), not Read():
        // a read miss on its own does not distinguish them.
        std::vector<uint8_t> bytes;
        WdtData wdt;
        if (!m_archive.Read(path, bytes))
        {
            if (m_archive.Contains(path))
            {
                m_wdtBroken.insert(mapId);
            }
            return nullptr;
        }

        if (!ParseWdt(bytes, wdt))
        {
            m_wdtBroken.insert(mapId);
            return nullptr;
        }
        return &m_wdtCache.emplace(mapId, std::move(wdt)).first->second;
    }

    bool MpqTileSource::WdtUnreadable(uint32_t mapId) const
    {
        return m_wdtBroken.find(mapId) != m_wdtBroken.end();
    }

    /// False when a doodad this set names could not be loaded -- the same failure the
    /// placement loop reports, since a WMO's furniture is collision like any other.
    bool MpqTileSource::AttachWmoDoodads(const Placement& p, const std::string& wmoPath,
                                         const Transform& wmoXf, TerrainTile& tile)
    {
        const WmoRootData* root = m_wmo.Root(wmoPath);
        if (!root || root->sets.empty())
        {
            return true;
        }

        // The placement names the one furnishing set that exists in the world; baking
        // every set would stack alternative furniture in the same room.
        const uint32_t setIdx = (p.doodadSet < root->sets.size()) ? p.doodadSet : 0u;
        const WmoDoodadSet& set = root->sets[setIdx];

        bool ok = true;
        const uint64_t end = uint64_t(set.start) + set.count;
        for (uint64_t i = set.start; i < end && i < root->doodads.size(); ++i)
        {
            const WmoDoodad& d = root->doodads[size_t(i)];
            if (d.name.empty())
            {
                continue;
            }
            auto model = m_m2.Load(d.name);
            if (!model)
            {
                ok = false;
                continue;
            }
            if (model->Empty())
            {
                continue;
            }

            StaticInstance inst;
            inst.xf = WmoDoodadTransform(wmoXf, d);
            inst.model = model;
            inst.worldBounds = WorldBoundsOf(model->Bounds(), inst.xf);
            inst.adtId = p.nameSet;
            tile.instances.push_back(std::move(inst));
        }
        return ok;
    }

    std::shared_ptr<TerrainTile> MpqTileSource::LoadAdt(uint32_t mapId, int tx, int ty)
    {
        const std::string path = AdtPath(mapId, tx, ty);
        if (path.empty())
        {
            return nullptr;
        }

        std::vector<uint8_t> bytes;
        if (!m_archive.Read(path, bytes))
        {
            return nullptr;
        }

        AdtData adt;
        if (!ParseAdt(bytes, adt) || !adt.hasTerrain)
        {
            return nullptr;
        }

        auto tile = std::make_shared<TerrainTile>();
        tile->tx = tx;
        tile->ty = ty;
        tile->hasTerrain = true;
        tile->v9 = std::move(adt.v9);
        tile->v8 = std::move(adt.v8);
        tile->holes = adt.holes;
        tile->areaIds = adt.areaIds;
        tile->hasLiquid = adt.hasLiquid;
        tile->liquidHeight = std::move(adt.liquidHeight);
        tile->liquidShow = std::move(adt.liquidShow);
        tile->liquidEntry = std::move(adt.liquidEntry);

        if (tile->hasLiquid)
        {
            const size_t cells = tile->liquidShow.size();
            tile->liquidKind.assign(cells, uint8_t(LiquidKind::None));
            tile->liquidDeep.assign(cells, 0);
            for (size_t i = 0; i < cells; ++i)
            {
                if (!tile->liquidShow[i])
                {
                    continue;
                }
                const LiquidKind kind =
                    world::ClassifyLiquid(tile->liquidEntry[i], m_liquidTypes);
                tile->liquidKind[i] = uint8_t(kind);
                // Dark water is the MCLQ per-cell bit (pre-WotLK tiles), or an ocean cell
                // whose MH2O chunk carries the DEEP ATTRIBUTE. It is a real 8x8 mask in
                // mh2o_chunk_attributes, not a property of the vertex format: the former
                // ocean-without-light-map guess read "this layer stores texture
                // coordinates" as "this water is fatiguing".
                tile->liquidDeep[i] =
                    (adt.liquidDark[i] ||
                     (kind == LiquidKind::Ocean && adt.liquidDeepAttr[i]))
                        ? 1
                        : 0;
            }
        }

        if (!m_loadStatics)
        {
            return tile;
        }

        bool loadFailed = false;
        auto attach = [&](const Placement& p,
                          const std::shared_ptr<const ICollisionModel>& model)
        {
            // NULL IS A FAILED LOAD, Empty is a model with no collision. Since the loaders
            // started answering null for a missing root, a missing declared group or a
            // corrupt M2, treating the two alike baked the tile without that building and
            // reported success -- BakeMap only ever looks at hasTerrain.
            if (!model)
            {
                loadFailed = true;
                return;
            }
            if (model->Empty())
            {
                return;
            }
            StaticInstance inst;
            inst.xf = PlacementTransform(p);
            inst.model = model;
            inst.worldBounds = WorldBoundsOf(model->Bounds(), inst.xf);
            inst.adtId = p.nameSet;
            tile->instances.push_back(std::move(inst));
        };

        for (const Placement& p : adt.wmoPlacements)
        {
            if (p.nameIndex >= adt.wmoNames.size())
            {
                continue;
            }
            const std::string& wmoPath = adt.wmoNames[p.nameIndex];
            attach(p, m_wmo.Load(wmoPath));
            if (!AttachWmoDoodads(p, wmoPath, PlacementTransform(p), *tile))
            {
                loadFailed = true;
            }
        }

        for (const Placement& p : adt.m2Placements)
        {
            if (p.nameIndex < adt.m2Names.size())
            {
                attach(p, m_m2.Load(adt.m2Names[p.nameIndex]));
            }
        }

        if (loadFailed)
        {
            return nullptr;
        }

        return tile;
    }

    std::shared_ptr<TerrainTile> MpqTileSource::LoadGlobalWmo(uint32_t mapId)
    {
        auto cached = m_globalWmoCache.find(mapId);
        if (cached != m_globalWmoCache.end())
        {
            return cached->second;
        }

        const WdtData* wdt = Wdt(mapId);
        if (!wdt || !wdt->hasGlobalWmo || wdt->globalWmoName.empty() ||
            !wdt->globalWmoPlacement)
        {
            return nullptr;
        }

        auto model = m_wmo.Load(wdt->globalWmoName);
        if (!model || model->Empty())
        {
            return nullptr;
        }

        auto tile = std::make_shared<TerrainTile>();
        tile->isGlobalWmo = true;

        const Transform xf = GlobalWmoTransform(*wdt->globalWmoPlacement);
        StaticInstance inst;
        inst.xf = xf;
        inst.model = model;
        inst.worldBounds = WorldBoundsOf(model->Bounds(), inst.xf);
        inst.adtId = wdt->globalWmoPlacement->nameSet;
        tile->instances.push_back(std::move(inst));

        // A dungeon IS one big WMO, so all of its furniture is doodads -- and a doodad that
        // will not load is the same failure the ADT placement loop reports. Discarding this
        // answer wrote the map's one tile without that collision and called it ok; the
        // result is not cached either, so the next call retries rather than serving it.
        if (!AttachWmoDoodads(*wdt->globalWmoPlacement, wdt->globalWmoName, xf, *tile))
        {
            return nullptr;
        }

        m_globalWmoCache[mapId] = tile;
        return tile;
    }

    std::shared_ptr<TerrainTile> MpqTileSource::Load(uint32_t mapId, int tx, int ty)
    {
        if (auto adtTile = LoadAdt(mapId, tx, ty))
        {
            return adtTile;
        }
        return LoadGlobalWmo(mapId);
    }
}
