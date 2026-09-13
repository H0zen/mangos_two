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

#include "TestHarness.h"

#include "terrain/Column.hpp"

using namespace world::terrain;

namespace
{
    Column Build(std::initializer_list<Surface> surfaces)
    {
        Column c;
        for (const Surface& s : surfaces)
        {
            if (s.Solid())
            {
                c.AddSolid(s.z, s.kind);
            }
            else
            {
                c.AddLiquid(s.AsLiquid());
            }
        }
        return c;
    }

    Surface Solid(float z, SurfaceKind kind = SurfaceKind::Static)
    {
        Surface s;
        s.z = z;
        s.kind = kind;
        return s;
    }

    Surface Liquid(float z, LiquidKind kind = LiquidKind::Water, uint16_t entry = 1)
    {
        Surface s;
        s.z = z;
        s.kind = SurfaceKind::Liquid;
        s.liquid = kind;
        s.liquidEntry = entry;
        return s;
    }
}

// The case this selection exists for. A player on a dry floor inside Undercity, with the
// building's ceiling over him and the Tirisfal lake above that: the old selection handed
// back the lake, the floor guard downstream compared that surface against the floor under
// his feet, and the breath timer started while he stood on stone.
TEST(LiquidOverIgnoresWaterAboveTheCeiling)
{
    const Column c = Build({Solid(-55.8f), Solid(-40.0f), Liquid(25.0f)});

    CHECK(!c.LiquidOver(-55.781f).has_value());

    // The map-wide question still has its old answer; only the point of view is new.
    const auto anywhere = c.HighestLiquid();
    REQUIRE(anywhere.has_value());
    CHECK_EQ(anywhere->z, 25.0f);
}

// The same case with the surfaces a real tile actually carries. The blocker overhead is
// not a ceiling but the lake BED: at any XY that holds water, the heightmap sits just
// under the water it holds, and both are far above a player standing in the city below.
// This is the shape that decided the heightmap must enter the column unclipped.
TEST(LiquidOverIgnoresALakeWhoseBedIsOverhead)
{
    const Column c = Build({Solid(-55.8f, SurfaceKind::Static),
                            Solid(30.0f, SurfaceKind::Terrain), Liquid(35.0f)});

    CHECK(!c.LiquidOver(-55.781f).has_value());
}

// The other direction, and the reason the fix is a blocker test rather than a height
// window: a swimmer can be far deeper below a surface than any fixed sweep would reach,
// and nothing lies between, so the water is still his.
TEST(LiquidOverKeepsDeepWaterWithTheBedFarBelow)
{
    const Column c = Build({Solid(-120.0f, SurfaceKind::Terrain), Liquid(0.0f)});

    const auto deep = c.LiquidOver(-100.0f);
    REQUIRE(deep.has_value());
    CHECK_EQ(deep->z, 0.0f);
}

TEST(LiquidOverFindsTheWaterThePointIsIn)
{
    const Column c = Build({Solid(-60.0f), Liquid(-50.0f)});

    const auto in = c.LiquidOver(-55.0f);
    REQUIRE(in.has_value());
    CHECK_EQ(in->z, -50.0f);
}

// A canal one storey down is not the water you are in, even though it is the only liquid
// in the column and sits well within the sweep.
TEST(LiquidOverIgnoresACanalBelowTheFloorYouStandOn)
{
    const Column c = Build({Solid(-50.0f), Liquid(-60.0f), Solid(-65.0f)});

    CHECK(!c.LiquidOver(-49.0f).has_value());
}

// Standing above open water -- no solid in between -- still reports it, because that is
// what tells a caller how far the drop is and whether the point can be walked on.
TEST(LiquidOverKeepsWaterBelowWhenNothingIsInTheWay)
{
    const Column c = Build({Solid(-60.0f), Liquid(-50.0f)});

    const auto below = c.LiquidOver(-45.0f);
    REQUIRE(below.has_value());
    CHECK_EQ(below->z, -50.0f);
}

TEST(LiquidOverTakesTheHighestReachableSurface)
{
    const Column c = Build({Solid(-60.0f), Liquid(-50.0f), Liquid(-45.0f)});

    const auto pick = c.LiquidOver(-55.0f);
    REQUIRE(pick.has_value());
    CHECK_EQ(pick->z, -45.0f);
}

// A solid exactly at the liquid's own height is the ledge the water laps against, not a
// lid over it. The interval tested is open at both ends for precisely this.
TEST(LiquidOverIsNotBlockedByALedgeFlushWithTheSurface)
{
    const Column c = Build({Solid(-50.0f), Liquid(-50.0f), Solid(-60.0f)});

    const auto flush = c.LiquidOver(-55.0f);
    REQUIRE(flush.has_value());
    CHECK_EQ(flush->z, -50.0f);
}

// The floor a point rests on never hides the water that point is standing in.
TEST(LiquidOverTieAtThePointItselfGoesToTheLiquid)
{
    const Column c = Build({Solid(-55.0f), Liquid(-55.0f)});

    const auto tie = c.LiquidOver(-55.0f);
    REQUIRE(tie.has_value());
    CHECK_EQ(tie->z, -55.0f);
}

TEST(LiquidOverOnAColumnWithoutLiquidIsEmpty)
{
    const Column c = Build({Solid(-55.0f), Solid(-40.0f)});

    CHECK(!c.LiquidOver(-50.0f).has_value());
    CHECK(!c.HighestLiquid().has_value());
}

// The selection carries the row id through untouched: identity is the DBC's business,
// and picking a different surface must not quietly become picking a different liquid.
TEST(LiquidOverPreservesTheLiquidIdentity)
{
    const Column c = Build({Solid(-60.0f), Liquid(-50.0f, LiquidKind::Slime, 20)});

    const auto slime = c.LiquidOver(-55.0f);
    REQUIRE(slime.has_value());
    CHECK(slime->liquid == LiquidKind::Slime);
    CHECK_EQ(int(slime->liquidEntry), 20);
}
