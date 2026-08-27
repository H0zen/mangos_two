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

/**
 * @file GridNotifiers.cpp
 * @brief Visitor pattern implementations for grid object notifications
 *
 * This file implements visitor classes that notify objects within
 * a grid of changes and events. Notifiers handle:
 *
 * - Visibility changes (objects entering/leaving view)
 * - Object creation/destruction
 * - Movement updates
 * - AI updates for creatures
 * - Player presence notifications
 *
 * The visitor pattern allows efficient iteration over grid objects
 * without coupling the grid to specific object types.
 *
 * @see GridNotifiers for notifier declarations
 * @see Map for grid management
 */

#include <set>
#include "GridNotifiers.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "UpdateData.h"
#include "Item.h"
#include "Map.h"
#include "Transports.h"
#include "TransportMap.h"
#include "PlayerRegistry.h"
#include "BattleGround/BattleGroundMgr.h"
#include "CreatureAI.h"
#include "Pet.h"

#include <algorithm>
#include <iterator>

using namespace MaNGOS;

/**
 * @brief Updates visibility for cameras affected by an object's visible changes.
 *
 * @param m The camera map to visit.
 */
void VisibleChangesNotifier::Visit(CameraMapType& m)
{
    for (CameraMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        iter->getSource()->UpdateVisibilityOf(&i_object);
    }
}

/**
 * @brief A transport gameobject is created and never remembered.
 *
 * The client keeps a transport for the life of the map, so the server holding a belief
 * about one would only give the diff something to destroy. It is rebuilt every pass, which
 * is what it has always been; everything else is remembered, and so everything else can
 * vanish.
 */
static bool VisibilityIsRemembered(WorldObject const* target)
{
    return target->GetTypeId() != TYPEID_GAMEOBJECT ||
           !static_cast<GameObject const*>(target)->IsTransport();
}

/**
 * @brief The one side effect a vanishing object ever had beyond the bytes: a pet that
 *        leaves its owner's belief is unsummoned.
 *
 * Asked on the GUID, not on a resolved object, and that is the point. The old test lived
 * on the object-level destroy path only, so it could not run for a leftover at all, and
 * resolving a leftover means a lookup on the observer's map -- which is the wrong map the
 * moment owner and pet are on opposite sides of a vessel's boundary. The owner already
 * knows his pet's guid; no map is consulted.
 */
static void UnsummonPetLeavingBelief(Player& player, ObjectGuid const& guid)
{
    if (player.GetPetGuid() != guid)
    {
        return;
    }

    if (Pet* pet = player.GetPet())
    {
        pet->Unsummon(PET_SAVE_REAGENTS);
    }
}

/**
 * @brief Records one candidate as something the client should hold. Sends nothing.
 */
void VisibleNotifier::Consider(WorldObject* target, WorldObject const* viewPoint)
{
    Player& player = *i_camera.GetOwner();

    // The belief the client already holds is an INPUT to the question, not merely the thing
    // its answer is compared against: what is already on screen is kept there by a laxer
    // test than the one that put it there. That is this tree's hysteresis, and it survives
    // the rewrite unchanged -- it is why the predicate takes `inVisibleList` at all.
    if (!target->IsVisibleForInState(&player, viewPoint, player.HaveAtClient(target)))
    {
        return;
    }

    if (!VisibilityIsRemembered(target))
    {
        i_visibleNow.insert(target);
        return;
    }

    i_should.insert(target->GetObjectGuid());

    if (!player.HaveAtClient(target))
    {
        i_visibleNow.insert(target);
    }
}

/**
 * @brief Differences belief against the sweep, then sends what changed.
 */
void VisibleNotifier::Notify()
{
    Player& player = *i_camera.GetOwner();

    // A deck has no cells, so nobody aboard ever arrives from a cell visit. The vessel's
    // own passengers are collected here instead, from her map's list, and then take part in
    // exactly the same diff as everything the sweep found ashore.
    if (player.GetMap()->AsTransport())
    {
        Map::PlayerList const& aboard = player.GetMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = aboard.begin(); itr != aboard.end(); ++itr)
        {
            Player* mate = itr->getSource();
            if (mate && i_believed.find(mate->GetObjectGuid()) != i_believed.end())
            {
                // ignore far sight case
                mate->UpdateVisibilityOf(mate, &player);
                Consider(mate, &player);
            }
        }
    }

    // ===== THE DIFF =====
    GuidSet vanished;
    std::set_difference(i_believed.begin(), i_believed.end(),
                        i_should.begin(), i_should.end(),
                        std::inserter(vanished, vanished.begin()));

    for (GuidSet::const_iterator itr = vanished.begin(); itr != vanished.end(); ++itr)
    {
        UnsummonPetLeavingBelief(player, *itr);

        // THE ONE DOOR OUT OF BELIEF, so the one place this refusal is written. A unit the
        // client draws on the zone map is dropped from the belief set -- the bookkeeping
        // stays honest, and coming back sends a fresh create -- but is never sent an
        // out-of-range block, because that list empties on DESTROY and on nothing else.
        // It matters for the siege vehicles rather than the gunships: a demolisher is an
        // ordinary grid creature, so every time its driver rode out of a watcher's cells
        // this is where its icon used to go.
        if (!player.GetMap()->IsZoneMapTrackedGuid(*itr))
        {
            i_data.AddOutOfRangeGUID(*itr);
        }

        player.ForgetSeen(*itr);

        DEBUG_FILTER_LOG(LOG_FILTER_VISIBILITY_CHANGES, "%s vanished for %s",
                         itr->GetString().c_str(), player.GetGuidStr().c_str());
    }

    for (std::set<WorldObject*>::const_iterator itr = i_visibleNow.begin(); itr != i_visibleNow.end(); ++itr)
    {
        WorldObject* target = *itr;

        target->BuildCreateUpdateBlockForPlayer(&i_data, &player);

        if (VisibilityIsRemembered(target))
        {
            player.RememberSeen(target);
        }

        DEBUG_FILTER_LOG(LOG_FILTER_VISIBILITY_CHANGES, "%s appeared for %s",
                         target->GetGuidStr().c_str(), player.GetGuidStr().c_str());
    }

    if (i_data.HasData())
    {
        // send create/outofrange packet to player (except player create updates that already sent using SendUpdateToPlayer)
        WorldPacket packet;
        i_data.BuildPacket(&packet);
        player.GetSession()->SendPacket(&packet);

        // send out of range to other players if need
        GuidSet const& oor = i_data.GetOutOfRangeGUIDs();
        for (GuidSet::const_iterator iter = oor.begin(); iter != oor.end(); ++iter)
        {
            if (!iter->IsPlayer())
            {
                continue;
            }

            if (Player* plr = sPlayerRegistry.Find(*iter))
            {
                plr->UpdateVisibilityOf(plr->GetCamera().GetBody(), &player);
            }
        }
    }

    // Now do operations that required done at object visibility change to visible

    // send data at target visibility change (adding to client)
    for (std::set<WorldObject*>::const_iterator vItr = i_visibleNow.begin(); vItr != i_visibleNow.end(); ++vItr)
    {
        // target aura duration for caster show only if target exist at caster client
        if ((*vItr) != &player && (*vItr)->isType(TYPEMASK_UNIT))
        {
            player.SendAurasForTarget((Unit*)(*vItr));
        }
    }
}

/**
 * @brief Delivers a packet to cameras in range of the source player.
 *
 * @param m The camera map to visit.
 */
void MessageDeliverer::Visit(CameraMapType& m)
{
    for (CameraMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        Player* owner = iter->getSource()->GetOwner();

        if (i_toSelf || owner != &i_player)
        {
            if (!i_player.InSamePhase(iter->getSource()->GetBody()))
            {
                continue;
            }

            if (WorldSession* session = owner->GetSession())
            {
                session->SendPacket(i_message);
            }
        }
    }
}

/**
 * @brief Delivers a packet to nearby cameras except one skipped receiver.
 *
 * @param m The camera map to visit.
 */
void MessageDelivererExcept::Visit(CameraMapType& m)
{
    for (CameraMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        Player* owner = iter->getSource()->GetOwner();

        if (!owner->InSamePhase(i_phaseMask) || owner == i_skipped_receiver)
        {
            continue;
        }

        if (WorldSession* session = owner->GetSession())
        {
            session->SendPacket(i_message);
        }
    }
}

/**
 * @brief Delivers an object-scoped packet to all camera owners in the visited set.
 *
 * @param m The camera map to visit.
 */
void ObjectMessageDeliverer::Visit(CameraMapType& m)
{
    for (CameraMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        if (!iter->getSource()->GetBody()->InSamePhase(i_phaseMask))
        {
            continue;
        }

        if (WorldSession* session = iter->getSource()->GetOwner()->GetSession())
        {
            session->SendPacket(i_message);
        }
    }
}

/**
 * @brief Delivers a packet to nearby cameras within an optional distance filter.
 *
 * @param m The camera map to visit.
 */
void MessageDistDeliverer::Visit(CameraMapType& m)
{
    for (CameraMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        Player* owner = iter->getSource()->GetOwner();

        if ((i_toSelf || owner != &i_player) &&
            (!i_ownTeamOnly || owner->GetTeam() == i_player.GetTeam()) &&
            (!i_dist || iter->getSource()->GetBody()->Where().WithinDist((i_player).Where(), i_dist)))
        {
            if (!i_player.InSamePhase(iter->getSource()->GetBody()))
            {
                continue;
            }

            if (WorldSession* session = owner->GetSession())
            {
                session->SendPacket(i_message);
            }
        }
    }
}

/**
 * @brief Delivers an object-scoped packet to cameras within an optional distance filter.
 *
 * @param m The camera map to visit.
 */
void ObjectMessageDistDeliverer::Visit(CameraMapType& m)
{
    for (CameraMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        if (!i_dist || iter->getSource()->GetBody()->Where().WithinDist((i_object).Where(), i_dist))
        {
            if (!i_object.InSamePhase(iter->getSource()->GetBody()))
            {
                continue;
            }

            if (WorldSession* session = iter->getSource()->GetOwner()->GetSession())
            {
                session->SendPacket(i_message);
            }
        }
    }
}

template<class T>
/**
 * @brief Updates all world objects referenced by a grid manager.
 *
 * @tparam T The grid object type.
 * @param m The grid reference manager.
 */
void ObjectUpdater::Visit(GridRefManager<T>& m)
{
    for (typename GridRefManager<T>::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        WorldObject::UpdateHelper helper(iter->getSource());
        helper.Update(i_timeDiff);
    }
}

/**
 * @brief Checks whether a corpse is a valid cannibalize target.
 *
 * @param u The corpse candidate.
 * @return true if the corpse can be cannibalized.
 */
bool CannibalizeObjectCheck::operator()(Corpse* u)
{
    // ignore bones
    if (u->GetType() == CORPSE_BONES)
    {
        return false;
    }

    Player* owner = sPlayerRegistry.Find(u->GetOwnerGuid());

    if (!owner || i_fobj->IsFriendlyTo(owner))
    {
        return false;
    }

    if (InReach(*i_fobj, *u, i_range))
    {
        return true;
    }

    return false;
}

/**
 * @brief Respawns a creature if battleground event rules allow it.
 *
 * @param u The creature to respawn.
 */
void MaNGOS::RespawnDo::operator()(Creature* u) const
{
    // prevent respawn creatures for not active BG event
    Map* map = u->GetMap();
    if (map->IsBattleGroundOrArena())
    {
        BattleGroundEventIdx eventId = sBattleGroundMgr.GetCreatureEventIndex(u->GetGUIDLow());
        if (!((BattleGroundMap*)map)->GetBG()->IsActiveEvent(eventId.event1, eventId.event2))
        {
            return;
        }
    }

    u->Respawn();
}

/**
 * @brief Respawns a game object if battleground event rules allow it.
 *
 * @param u The game object to respawn.
 */
void MaNGOS::RespawnDo::operator()(GameObject* u) const
{
    // prevent respawn gameobject for not active BG event
    Map* map = u->GetMap();
    if (map->IsBattleGroundOrArena())
    {
        BattleGroundEventIdx eventId = sBattleGroundMgr.GetGameObjectEventIndex(u->GetGUIDLow());
        if (!((BattleGroundMap*)map)->GetBG()->IsActiveEvent(eventId.event1, eventId.event2))
        {
            return;
        }
    }

    u->Respawn();
}

/**
 * @brief Calls nearby assist-capable creatures to attack an enemy.
 *
 * @param u The nearby creature to test.
 */
void MaNGOS::CallOfHelpCreatureInRangeDo::operator()(Creature* u)
{
    if (u == i_funit)
    {
        return;
    }

    if (!u->CanAssistTo(i_funit, i_enemy, false))
    {
        return;
    }

    // too far
    if (!InReach(*i_funit, *u, i_range))
    {
        return;
    }

    // only if see assisted creature
    if (!HasLineOfSight(*i_funit, *u))
    {
        return;
    }

    if (u->AI())
    {
        u->AI()->AttackStart(i_enemy);
    }
}

/**
 * @brief Checks whether a nearby creature can assist against an enemy.
 *
 * @param u The nearby creature to test.
 * @return true if the creature can assist.
 */
bool MaNGOS::AnyAssistCreatureInRangeCheck::operator()(Creature* u)
{
    if (u == i_funit)
    {
        return false;
    }

    if (!u->CanAssistTo(i_funit, i_enemy))
    {
        return false;
    }

    // too far
    if (!InReach(*i_funit, *u, i_range))
    {
        return false;
    }

    // only if see assisted creature
    if (!HasLineOfSight(*i_funit, *u))
    {
        return false;
    }

    return true;
}

template void ObjectUpdater::Visit<GameObject>(GameObjectMapType&);
template void ObjectUpdater::Visit<DynamicObject>(DynamicObjectMapType&);
