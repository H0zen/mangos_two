#include <unordered_map>
#include <memory>
#include <string>
#include <vector>
#include "ModelLoaders.hpp"

#include "M2Parser.hpp"
#include "terrain/CollisionModel.hpp"
#include "terrain/WmoModel.hpp"

namespace world::terrain
{
    const WmoRootData* WmoLoader::Root(const std::string& rootPath)
    {
        auto it = m_roots.find(rootPath);
        if (it != m_roots.end())
        {
            return it->second.doodads.empty() ? nullptr : &it->second;
        }

        std::vector<uint8_t> bytes;
        WmoRootData root;
        if (m_archive.Read(rootPath, bytes))
        {
            ParseWmoRoot(bytes, root);
        }
        auto& slot = m_roots.emplace(rootPath, std::move(root)).first->second;
        return slot.doodads.empty() ? nullptr : &slot;
    }

    std::shared_ptr<const ICollisionModel> WmoLoader::Load(const std::string& rootPath)
    {
        auto cached = m_cache.find(rootPath);
        if (cached != m_cache.end())
        {
            return cached->second;
        }

        std::vector<uint8_t> rootBytes;
        if (!m_archive.Read(rootPath, rootBytes))
        {
            m_cache.emplace(rootPath, nullptr);
            m_roots.emplace(rootPath, WmoRootData{});
            return nullptr;
        }

        // No MOHD is not a WMO root. Ignoring that left nGroups at 0, so the loop below
        // ran zero times and an EMPTY model was cached as loaded -- indistinguishable
        // downstream from a building that genuinely has no collision.
        WmoRootData root;
        if (!ParseWmoRoot(rootBytes, root))
        {
            m_cache.emplace(rootPath, nullptr);
            m_roots.emplace(rootPath, WmoRootData{});
            return nullptr;
        }
        if (m_roots.find(rootPath) == m_roots.end())
        {
            m_roots.emplace(rootPath, root);
        }

        // One soup for the whole WMO. Each group's vertices are re-indexed as they are
        // appended, so the ~60% that only ever fed render-only faces are never carried.
        TriSoup soup;
        std::vector<uint16_t> triGroup;
        std::vector<WmoModel::Group> groups;

        for (uint32_t g = 0; g < root.nGroups; ++g)
        {
            // The root DECLARES its group count, so a group file the archive does not
            // have is an incomplete client, not a group with nothing in it. Skipping it
            // returns a model that is non-empty and therefore looks loaded, missing that
            // wing's collision.
            std::vector<uint8_t> groupBytes;
            if (!m_archive.Read(WmoGroupPath(rootPath, g), groupBytes))
            {
                m_cache.emplace(rootPath, nullptr);
                return nullptr;
            }

            // EMPTY IS NOT MALFORMED. A group with neither collidable geometry nor
            // liquid is the ordinary case -- render-only, portal and ambient groups are
            // most of any WMO -- and failing the model on those drops real buildings. A
            // group that is BROKEN is a missing wing, and the building is not loaded.
            WmoGroupData parsed;
            const WmoGroupParse result = ParseWmoGroup(groupBytes, root.flags, parsed);
            if (result == WmoGroupParse::Malformed)
            {
                m_cache.emplace(rootPath, nullptr);
                return nullptr;
            }
            if (result == WmoGroupParse::Empty)
            {
                continue;
            }

            WmoModel::Group meta;
            meta.mogpFlags = parsed.mogpFlags;
            meta.groupWmoId = parsed.groupWmoId;
            meta.hasLiquid = parsed.hasLiquid;
            if (parsed.hasLiquid)
            {
                meta.liquid.tilesX = parsed.liquid.tilesX;
                meta.liquid.tilesY = parsed.liquid.tilesY;
                meta.liquid.corner = parsed.liquid.corner;
                meta.liquid.heights = std::move(parsed.liquid.heights);
                meta.liquid.flags = std::move(parsed.liquid.flags);
                meta.liquid.entry = parsed.liquid.entry;
                meta.liquid.kind = static_cast<uint8_t>(
                    world::ClassifyLiquid(parsed.liquid.entry, m_liquidTypes));
            }

            const uint16_t gi = uint16_t(groups.size());
            groups.push_back(std::move(meta));

            std::unordered_map<uint16_t, uint32_t> remap;
            remap.reserve(parsed.tris.size() * 3);
            auto vertexOf = [&](uint16_t local)
            {
                auto found = remap.find(local);
                if (found != remap.end())
                {
                    return found->second;
                }
                const uint32_t idx = uint32_t(soup.verts.size());
                soup.verts.push_back(parsed.verts[local]);
                remap.emplace(local, idx);
                return idx;
            };

            for (const auto& t : parsed.tris)
            {
                soup.tris.push_back({vertexOf(t[0]), vertexOf(t[1]), vertexOf(t[2])});
                triGroup.push_back(gi);
            }
        }

        Bvh bvh;
        bvh.Build(soup, &triGroup, 4);

        auto model = std::make_shared<WmoModel>(std::move(soup), std::move(triGroup),
                                                std::move(groups), root.wmoId,
                                                std::move(bvh));
        m_cache.emplace(rootPath, model);
        return model;
    }

    std::shared_ptr<const ICollisionModel> M2Loader::Load(const std::string& path)
    {
        const std::string key = M2PathOf(path);
        auto cached = m_cache.find(key);
        if (cached != m_cache.end())
        {
            return cached->second;
        }

        std::vector<uint8_t> bytes;
        if (!m_archive.Read(key, bytes))
        {
            m_cache.emplace(key, nullptr);
            return nullptr;
        }

        M2Data parsed;
        ParseM2(bytes, parsed);

        TriSoup soup;
        soup.verts = std::move(parsed.verts);
        soup.tris = std::move(parsed.tris);

        auto model = std::make_shared<CollisionModel>(std::move(soup));
        m_cache.emplace(key, model);
        return model;
    }
}
