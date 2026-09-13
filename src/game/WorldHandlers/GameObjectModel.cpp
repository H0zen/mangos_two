/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include <memory>
#include <vector>
#include "GameObjectModel.h"


#include <cfloat>
#include <cmath>

using Geometry::Aabb;
using Geometry::Vector3;

GameObjectModel* GameObjectModel::CreateStandalone(
    std::shared_ptr<const world::terrain::ICollisionModel> model,
    const Geometry::Transform& xf, uint32 phaseMask)
{
    if (!model || model->Empty())
    {
        return nullptr;
    }

    GameObjectModel* body = new GameObjectModel();
    body->m_model = std::move(model);
    body->m_phaseMask = phaseMask;
    body->m_xf = xf;
    body->DeriveBounds();
    return body;
}

void GameObjectModel::SetPose(const Geometry::Transform& xf)
{
    m_xf = xf;
    DeriveBounds();
}

// The world box is the PLACED model box, not the model box translated: a rotated door
// sweeps a larger footprint than its own extents, and a box that does not cover the body
// is a body the broadphase never offers to the narrowphase.
void GameObjectModel::DeriveBounds()
{
    m_bounds = Aabb{};
    if (!m_model)
    {
        return;
    }
    const Aabb& local = m_model->Bounds();
    if (!local.valid())
    {
        return;
    }
    for (int i = 0; i < 8; ++i)
    {
        const Vector3 corner{(i & 1) ? local.hi.x : local.lo.x,
                             (i & 2) ? local.hi.y : local.lo.y,
                             (i & 4) ? local.hi.z : local.lo.z};
        m_bounds.expand(m_xf.localToWorld(corner));
    }
}

float GameObjectModel::SegmentHitFraction(const Vector3& a, const Vector3& b) const
{
    if (!m_collidable || !m_model)
    {
        return 2.0f;
    }

    const Vector3 originLocal = m_xf.worldToLocal(a);
    const Vector3 dirLocal = m_xf.worldToLocal(b) - originLocal;

    if (auto t = m_model->RaycastNearest(originLocal, dirLocal, 1.0f))
    {
        if (*t >= 0.f)
        {
            return *t;
        }
    }
    return 2.0f;
}

void GameObjectModel::AddSurfaces(float x, float y, float zTop, float zBottom,
                                  world::terrain::Column& out) const
{
    if (!m_collidable || !m_model || !m_bounds.coversColumn(x, y))
    {
        return;
    }

    // Swept over this object's OWN extent, not the caller's window, for the same reason
    // the baked instances are: a window that reaches into the gather makes the column a
    // function of who asked. A lift standing above a player's 50-yard box is not absent
    // from the world, and a selection that walks upward -- the ceiling over a point, the
    // blocker between a point and a water surface -- needs it to be in the column.
    const float sweepTop = std::max(zTop, m_bounds.hi.z + 1.f);
    const float sweepBottom = std::min(zBottom, m_bounds.lo.z - 1.f);

    const Vector3 originWorld{x, y, sweepTop};
    const Vector3 downWorld{0.f, 0.f, -1.f};
    const Vector3 originLocal = m_xf.worldToLocal(originWorld);
    const Vector3 dirLocal = m_xf.worldToLocalDirection(downWorld);

    // localToWorld(o + t*d) == originWorld + t*downWorld, so t is a world distance
    // whatever this object's scale.
    thread_local std::vector<float> hits;
    hits.clear();
    m_model->RaycastAll(originLocal, dirLocal, sweepTop - sweepBottom, hits);
    for (const float t : hits)
    {
        out.AddSolid(sweepTop - t, world::terrain::SurfaceKind::Live);
    }
}
