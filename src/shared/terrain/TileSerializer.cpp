#include <memory>
#include <string>
#include <vector>
#include "terrain/TileSerializer.hpp"
#include "terrain/CollisionModel.hpp"
#include "terrain/WmoModel.hpp"

#include <array>
#include <cstdio>
#include <unordered_map>

namespace world::terrain
{
    namespace
    {
        constexpr uint32_t MAGIC = 0x32474E4D;  // "MNG2" in file order
        constexpr uint32_t VERSION = 1;

        constexpr uint32_t MAX_MODELS = 1u << 20;
        constexpr uint32_t MAX_INSTANCES = 1u << 22;

        /// Every triangle indexes the soup's own vertices. TriSoup::At does no checking,
        /// by design -- it is the innermost thing in a raycast -- so the file has to be
        /// proved right here instead.
        bool SoupIndicesInRange(const TriSoup& soup)
        {
            const uint32_t n = uint32_t(soup.verts.size());
            for (const auto& t : soup.tris)
            {
                if (t[0] >= n || t[1] >= n || t[2] >= n)
                {
                    return false;
                }
            }
            return true;
        }

        bool Finite(const Vec3& v) { return v.isFinite(); }

        bool Finite(const Mat3& m)
        {
            for (const float f : m.m)
            {
                if (!::Geometry::isFinite(f))
                {
                    return false;
                }
            }
            return true;
        }

        // A count is only believable if the file still holds that many elements. A fixed
        // ceiling is not enough: it still lets a corrupt header reserve hundreds of
        // megabytes before the short read is noticed, and on an overcommitting kernel
        // that reservation succeeds, so the guard never fires and the test that was
        // supposed to prove it stays green either way. Measuring against the actual file
        // makes the allocation impossible rather than merely unlikely.
        long RemainingBytes(std::FILE* f)
        {
            const long here = std::ftell(f);
            if (here < 0 || std::fseek(f, 0, SEEK_END) != 0)
            {
                return -1;
            }
            const long end = std::ftell(f);
            if (end < 0 || std::fseek(f, here, SEEK_SET) != 0)
            {
                return -1;
            }
            return end - here;
        }

        template <class T>
        bool WPod(std::FILE* f, const T& v)
        {
            return std::fwrite(&v, sizeof(T), 1, f) == 1;
        }

        template <class T>
        bool RPod(std::FILE* f, T& v)
        {
            return std::fread(&v, sizeof(T), 1, f) == 1;
        }

        template <class T>
        bool WVec(std::FILE* f, const std::vector<T>& v)
        {
            const uint32_t n = uint32_t(v.size());
            if (std::fwrite(&n, 4, 1, f) != 1)
            {
                return false;
            }
            return n == 0 || std::fwrite(v.data(), sizeof(T), n, f) == n;
        }

        template <class T>
        bool RVec(std::FILE* f, std::vector<T>& v)
        {
            uint32_t n = 0;
            if (std::fread(&n, 4, 1, f) != 1)
            {
                return false;
            }
            const long left = RemainingBytes(f);
            if (left < 0 || uint64_t(n) * sizeof(T) > uint64_t(left))
            {
                return false;
            }
            v.resize(n);
            return n == 0 || std::fread(v.data(), sizeof(T), n, f) == n;
        }

        template <class T, size_t N>
        bool WArray(std::FILE* f, const std::array<T, N>& a)
        {
            return std::fwrite(a.data(), sizeof(T), N, f) == N;
        }

        template <class T, size_t N>
        bool RArray(std::FILE* f, std::array<T, N>& a)
        {
            return std::fread(a.data(), sizeof(T), N, f) == N;
        }

        bool WriteGroup(std::FILE* f, const WmoModel::Group& g)
        {
            bool ok = WPod(f, g.mogpFlags) && WPod(f, g.groupWmoId);
            const uint8_t hasLiquid = g.hasLiquid ? 1 : 0;
            ok = ok && WPod(f, hasLiquid);
            if (g.hasLiquid)
            {
                ok = ok && WPod(f, g.liquid.tilesX) && WPod(f, g.liquid.tilesY) &&
                     WPod(f, g.liquid.corner) && WPod(f, g.liquid.entry) &&
                     WPod(f, g.liquid.kind) && WVec(f, g.liquid.heights) &&
                     WVec(f, g.liquid.flags);
            }
            return ok;
        }

        bool ReadGroup(std::FILE* f, WmoModel::Group& g)
        {
            bool ok = RPod(f, g.mogpFlags) && RPod(f, g.groupWmoId);
            uint8_t hasLiquid = 0;
            ok = ok && RPod(f, hasLiquid);
            g.hasLiquid = hasLiquid != 0;
            if (ok && g.hasLiquid)
            {
                ok = RPod(f, g.liquid.tilesX) && RPod(f, g.liquid.tilesY) &&
                     RPod(f, g.liquid.corner) && RPod(f, g.liquid.entry) &&
                     RPod(f, g.liquid.kind) && RVec(f, g.liquid.heights) &&
                     RVec(f, g.liquid.flags);

                // The corner grid is indexed at a stride of tilesX+1 by every reader, so
                // a count that disagrees with the dimensions is not a smaller surface --
                // it is a read off the end of the vector on the first query over it.
                const size_t corners =
                    (size_t(g.liquid.tilesX) + 1) * (size_t(g.liquid.tilesY) + 1);
                if (ok && g.liquid.heights.size() != corners)
                {
                    return false;
                }

                // The flags grid decides which tiles are DRY. GroupLiquidAt reads it as
                // `fi < flags.size() && ...`, so a short or empty array does not fail --
                // it silently answers "not dry" for every cell past the end and floods
                // rooms that the client marks as having no water in them.
                const size_t cells = size_t(g.liquid.tilesX) * g.liquid.tilesY;
                if (ok && g.liquid.flags.size() != cells)
                {
                    return false;
                }
            }
            return ok;
        }
    }

    std::string TileFileName(uint32_t mapId, int tx, int ty)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "t_%u_%d_%d.tile", mapId, tx, ty);
        return name;
    }

    std::string GlobalWmoFileName(uint32_t mapId)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "w_%u.tile", mapId);
        return name;
    }

    std::string GoModelFileName(uint32_t displayId)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "go_%u.tile", displayId);
        return name;
    }

    bool WriteTile(const TerrainTile& tile, const std::string& path)
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            return false;
        }

        const uint8_t hasTerrain = tile.hasTerrain ? 1 : 0;
        const uint8_t globalWmo = tile.isGlobalWmo ? 1 : 0;
        const uint8_t hasLiquid = tile.hasLiquid ? 1 : 0;

        bool ok = WPod(f, MAGIC) && WPod(f, VERSION) && WPod(f, tile.tx) &&
                  WPod(f, tile.ty) && WPod(f, hasTerrain) && WPod(f, globalWmo) &&
                  WVec(f, tile.v9) && WVec(f, tile.v8) && WArray(f, tile.holes) &&
                  WArray(f, tile.areaIds) && WPod(f, hasLiquid) &&
                  WVec(f, tile.liquidHeight) && WVec(f, tile.liquidShow) &&
                  WVec(f, tile.liquidKind) && WVec(f, tile.liquidEntry) &&
                  WVec(f, tile.liquidDeep);

        // Deduped model table: a WMO instanced fifty times is written once and the
        // instances index it.
        std::unordered_map<const ICollisionModel*, uint32_t> modelIndex;
        std::vector<const ICollisionModel*> models;
        for (const StaticInstance& inst : tile.instances)
        {
            const ICollisionModel* m = inst.model.get();
            if (m && !modelIndex.count(m))
            {
                modelIndex[m] = uint32_t(models.size());
                models.push_back(m);
            }
        }

        const uint32_t nModels = uint32_t(models.size());
        ok = ok && WPod(f, nModels);
        for (const ICollisionModel* m : models)
        {
            const uint8_t kind = uint8_t(m->Kind());
            ok = ok && WPod(f, kind);
            if (m->Kind() == ModelKind::Wmo)
            {
                const auto* w = static_cast<const WmoModel*>(m);
                const uint32_t nGroups = uint32_t(w->Groups().size());
                ok = ok && WPod(f, w->RootId()) && WPod(f, nGroups);
                for (const WmoModel::Group& g : w->Groups())
                {
                    ok = ok && WriteGroup(f, g);
                }
                // soup.tris is already in the BVH's leaf order, so nothing is rebuilt.
                ok = ok && WVec(f, w->Soup().verts) && WVec(f, w->Soup().tris) &&
                     WVec(f, w->TriGroups()) && WVec(f, w->GetBvh().Nodes());
            }
            else
            {
                const auto* c = static_cast<const CollisionModel*>(m);
                ok = ok && WVec(f, c->Soup().verts) && WVec(f, c->Soup().tris) &&
                     WVec(f, c->GetBvh().Nodes());
            }
        }

        const uint32_t nInstances = uint32_t(tile.instances.size());
        ok = ok && WPod(f, nInstances);
        for (const StaticInstance& inst : tile.instances)
        {
            auto found = modelIndex.find(inst.model.get());
            const uint32_t idx = found != modelIndex.end() ? found->second : 0xFFFFFFFFu;
            ok = ok && WPod(f, inst.xf.pos) && WArray(f, inst.xf.rot.m) &&
                 WPod(f, inst.xf.scale) && WPod(f, inst.worldBounds.lo) &&
                 WPod(f, inst.worldBounds.hi) && WPod(f, idx) && WPod(f, inst.adtId);
        }

        std::fclose(f);
        if (!ok)
        {
            std::remove(path.c_str());
        }
        return ok;
    }

    std::shared_ptr<TerrainTile> ReadTile(const std::string& path)
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            return nullptr;
        }

        uint32_t magic = 0, version = 0;
        if (!RPod(f, magic) || !RPod(f, version) || magic != MAGIC || version != VERSION)
        {
            std::fclose(f);
            return nullptr;
        }

        auto tile = std::make_shared<TerrainTile>();
        uint8_t hasTerrain = 0, globalWmo = 0, hasLiquid = 0;

        bool ok = RPod(f, tile->tx) && RPod(f, tile->ty) && RPod(f, hasTerrain) &&
                  RPod(f, globalWmo) && RVec(f, tile->v9) && RVec(f, tile->v8) &&
                  RArray(f, tile->holes) && RArray(f, tile->areaIds) &&
                  RPod(f, hasLiquid) && RVec(f, tile->liquidHeight) &&
                  RVec(f, tile->liquidShow) && RVec(f, tile->liquidKind) &&
                  RVec(f, tile->liquidEntry) && RVec(f, tile->liquidDeep);

        tile->hasTerrain = hasTerrain != 0;
        tile->isGlobalWmo = globalWmo != 0;
        tile->hasLiquid = hasLiquid != 0;

        // A grid is present EXACTLY when its flag says so, and when present it is exactly
        // the size every reader indexes it at -- TerrainHeight walks v9 at a stride of
        // V9_SIDE and guards only against empty. "Fits in the remaining file" was a
        // truncation check, never a shape one; and accepting empty unconditionally lets a
        // tile DECLARE terrain and carry none, pass the startup probe, then answer every
        // height query with nothing.
        constexpr size_t CORNERS = size_t(V9_SIDE) * V9_SIDE;
        constexpr size_t CELLS = size_t(GRID_PER_TILE) * GRID_PER_TILE;
        auto grid = [](const auto& v, bool present, size_t n)
        {
            return v.size() == (present ? n : size_t(0));
        };
        ok = ok && grid(tile->v9, tile->hasTerrain, CORNERS) &&
             grid(tile->v8, tile->hasTerrain, CELLS) &&
             grid(tile->liquidHeight, tile->hasLiquid, CORNERS) &&
             grid(tile->liquidShow, tile->hasLiquid, CELLS) &&
             grid(tile->liquidKind, tile->hasLiquid, CELLS) &&
             grid(tile->liquidEntry, tile->hasLiquid, CELLS) &&
             grid(tile->liquidDeep, tile->hasLiquid, CELLS);

        uint32_t nModels = 0;
        ok = ok && RPod(f, nModels) && nModels <= MAX_MODELS;

        std::vector<std::shared_ptr<const ICollisionModel>> models;
        if (ok)
        {
            models.resize(nModels);
        }

        for (uint32_t i = 0; ok && i < nModels; ++i)
        {
            uint8_t kind = 0;
            if (!RPod(f, kind))
            {
                ok = false;
                break;
            }

            TriSoup soup;
            std::vector<Bvh::Node> nodes;

            if (kind == uint8_t(ModelKind::Wmo))
            {
                uint32_t rootId = 0, nGroups = 0;
                ok = RPod(f, rootId) && RPod(f, nGroups) && nGroups <= MAX_MODELS;
                std::vector<WmoModel::Group> groups(ok ? nGroups : 0);
                for (uint32_t g = 0; ok && g < nGroups; ++g)
                {
                    ok = ReadGroup(f, groups[g]);
                }

                std::vector<uint16_t> triGroup;
                Bvh bvh;
                ok = ok && RVec(f, soup.verts) && RVec(f, soup.tris) &&
                     RVec(f, triGroup) && RVec(f, nodes) &&
                     triGroup.size() == soup.tris.size() && SoupIndicesInRange(soup) &&
                     bvh.Adopt(std::move(nodes), soup.tris.size());
                if (ok)
                {
                    models[i] = std::make_shared<WmoModel>(std::move(soup),
                                                           std::move(triGroup),
                                                           std::move(groups), rootId,
                                                           std::move(bvh));
                }
            }
            else if (kind == uint8_t(ModelKind::Mesh))
            {
                Bvh bvh;
                ok = RVec(f, soup.verts) && RVec(f, soup.tris) && RVec(f, nodes) &&
                     SoupIndicesInRange(soup) &&
                     bvh.Adopt(std::move(nodes), soup.tris.size());
                if (ok)
                {
                    models[i] = std::make_shared<CollisionModel>(std::move(soup),
                                                                 std::move(bvh));
                }
            }
            else
            {
                ok = false;
            }
        }

        uint32_t nInstances = 0;
        ok = ok && RPod(f, nInstances) && nInstances <= MAX_INSTANCES;
        for (uint32_t i = 0; ok && i < nInstances; ++i)
        {
            StaticInstance inst;
            uint32_t idx = 0;
            ok = RPod(f, inst.xf.pos) && RArray(f, inst.xf.rot.m) &&
                 RPod(f, inst.xf.scale) && RPod(f, inst.worldBounds.lo) &&
                 RPod(f, inst.worldBounds.hi) && RPod(f, idx) && RPod(f, inst.adtId);
            if (ok)
            {
                // THE POSE ARRIVES AS RAW PODS, which walks straight past the Transform
                // constructor whose whole purpose is this clamp -- so the baker clamping
                // a zero or non-finite scale fixes only half of it. worldToLocal divides
                // by scale, and every comparison against the resulting NaN is false, so
                // the model is silently never hit rather than failing.
                if (!(inst.xf.scale > 0.f) || !::Geometry::isFinite(inst.xf.scale))
                {
                    inst.xf.scale = 1.f;
                }
                if (!Finite(inst.xf.pos) || !Finite(inst.xf.rot) ||
                    !Finite(inst.worldBounds.lo) || !Finite(inst.worldBounds.hi))
                {
                    ok = false;
                    break;
                }

                if (idx < models.size())
                {
                    inst.model = models[idx];
                }
                tile->instances.push_back(std::move(inst));
            }
        }

        std::fclose(f);
        return ok ? tile : nullptr;
    }
}
