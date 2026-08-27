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
 * @file PlayerVisibility.cpp
 * @brief Cohesion split of Player.cpp -- visibility / phasing / who-can-see updates.
 *        Same `Player` class; no behaviour change.
 */

#include <set>
#include "Common/ServerDefines.h"
#include "Player.h"
#include "Language.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "UpdateMask.h"
#include "SkillDiscovery.h"
#include "QuestDef.h"
#include "GossipDef.h"
#include "UpdateData.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "MapManager.h"
#include "MapPersistentStateMgr.h"
#include "InstanceData.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "ObjectMgr.h"
#include "CreatureAI.h"
#include "Formulas.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Pet.h"
#include "Util.h"
#include "Transports.h"
#include "Weather.h"
#include "BattleGround/BattleGround.h"
#include "BattleGround/BattleGroundMgr.h"
#include "BattleGround/BattleGroundAV.h"
#include "OutdoorPvP/OutdoorPvP.h"
#include "ArenaTeam.h"
#include "Chat.h"
#include "Spell.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "AchievementMgr.h"
#include "Mail.h"
#include "SpellAuras.h"
#include "DBCStores.h"
#include "SQLStorages.h"
#include "Vehicle.h"
#include "Calendar.h"
#include "DisableMgr.h"
#ifdef ENABLE_ELUNA
#include "LuaEngine.h"
#endif /* ENABLE_ELUNA */
#include <cmath>

/**
 * @brief Checks whether this player should be visible to another player in grid range.
 *
 * @param pl The observing player.
 * @return True if this player should be visible; otherwise, false.
 */
bool Player::IsVisibleInGridForPlayer(Player* pl) const
{
    // gamemaster in GM mode see all, including ghosts
    if (pl->isGameMaster() && GetSession()->GetSecurity() <= pl->GetSession()->GetSecurity())
    {
        return true;
    }

    // player see dead player/ghost from own group/raid
    if (IsInSameRaidWith(pl))
    {
        return true;
    }

    // Live player see live player or dead player with not realized corpse
    if (pl->IsAlive() || pl->m_deathTimer > 0)
    {
        return IsAlive() || m_deathTimer > 0;
    }

    // Ghost see other friendly ghosts, that's for sure
    if (!(IsAlive() || m_deathTimer > 0) && IsFriendlyTo(pl))
    {
        return true;
    }

    // Dead player see live players near own corpse
    if (IsAlive())
    {
        if (Corpse* corpse = pl->GetCorpse())
        {
            // 20 - aggro distance for same level, 25 - max additional distance if player level less that creature level
            if (InReach(*corpse, *this, (20 + 25) * sWorld.getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO)))
            {
                return true;
            }
        }
    }

    // and not see any other
    return false;
}

/**
 * @brief Checks whether this player should appear in global player visibility contexts.
 *
 * @param u The player attempting to see this player.
 * @return True if this player is globally visible; otherwise, false.
 */
bool Player::IsVisibleGloballyFor(Player* u) const
{
    if (!u)
    {
        return false;
    }

    // Always can see self
    if (u == this)
    {
        return true;
    }

    // Visible units, always are visible for all players
    if (GetVisibility() == VISIBILITY_ON)
    {
        return true;
    }

    // GMs are visible for higher gms (or players are visible for gms)
    if (u->GetSession()->GetSecurity() > SEC_PLAYER)
    {
        return GetSession()->GetSecurity() <= u->GetSession()->GetSecurity();
    }

    // non faction visibility non-breakable for non-GMs
    if (GetVisibility() == VISIBILITY_OFF)
    {
        return false;
    }

    // non-gm stealth/invisibility not hide from global player lists
    return true;
}

template<class T>
inline void BeforeVisibilityDestroy(T* /*t*/, Player* /*p*/)
{
}

template<>
inline void BeforeVisibilityDestroy<Creature>(Creature* t, Player* p)
{
    if (p->GetPetGuid() == t->GetObjectGuid() && (t->IsPet()))
    {
        (reinterpret_cast<Pet*>(t))->Unsummon(PET_SAVE_REAGENTS);
    }
}

/**
 * @brief Updates visibility of a single world object for the player.
 *
 * @param viewPoint The viewpoint used for visibility checks.
 * @param target The target object whose visibility is being updated.
 */
void Player::UpdateVisibilityOf(WorldObject const* viewPoint, WorldObject* target)
{
    if (HaveAtClient(target))
    {
        if (!target->IsVisibleForInState(this, viewPoint, true))
        {
            ObjectGuid t_guid = target->GetObjectGuid();

            if (target->GetTypeId() == TYPEID_UNIT)
            {
                BeforeVisibilityDestroy<Creature>(reinterpret_cast<Creature*>(target), this);

                // at remove from map (destroy) show kill animation (in different out of range/stealth case)
                target->DestroyForPlayer(this, !target->IsInWorld() && (reinterpret_cast<Creature*>(target))->IsDead());
            }
            else
            {
                target->DestroyForPlayer(this);
            }

            ForgetSeen(target);

            DEBUG_FILTER_LOG(LOG_FILTER_VISIBILITY_CHANGES, "UpdateVisibilityOf: %s out of range for player %u. Distance = %f", t_guid.GetString().c_str(), GetGUIDLow(), Where().DistanceTo(target->Where()));
        }
    }
    else
    {
        if (target->IsVisibleForInState(this, viewPoint, false))
        {
            target->SendCreateUpdateToPlayer(this);
            if (target->GetTypeId() != TYPEID_GAMEOBJECT || !(reinterpret_cast<GameObject*>(target))->IsTransport())
            {
                RememberSeen(target);
            }

            DEBUG_FILTER_LOG(LOG_FILTER_VISIBILITY_CHANGES, "UpdateVisibilityOf: %s is visible now for player %u. Distance = %f", target->GetGuidStr().c_str(), GetGUIDLow(), Where().DistanceTo(target->Where()));

            // target aura duration for caster show only if target exist at caster client
            // send data at target visibility change (adding to client)
            if (target != this && target->isType(TYPEMASK_UNIT))
            {
                SendAurasForTarget(reinterpret_cast<Unit*>(target));
            }
        }
    }
}


void WorldObject::DetachAllObservers()
{
    // Only this side. The observers' forward sets are NOT corrected from here, and that is
    // deliberate: they live on maps this object cannot reach, and the leftovers sweep
    // already drops a guid whose object is gone. Correcting them would mean a lookup per
    // observer per despawn to fix something that fixes itself one tick later.
    m_observers.clear();
}

void Player::RememberSeen(WorldObject* target)
{
    if (!target || target == this)
    {
        return;
    }

    m_clientGUIDs.insert(target->GetObjectGuid());
    target->AddObserver(GetObjectGuid());
}

void Player::ForgetSeen(WorldObject* target)
{
    if (!target)
    {
        return;
    }

    m_clientGUIDs.erase(target->GetObjectGuid());
    target->RemoveObserver(GetObjectGuid());
}

void Player::ForgetSeen(ObjectGuid target)
{
    m_clientGUIDs.erase(target);

    // Best effort on the reverse side. The guid may name something already destroyed, in
    // which case there is no set left to correct and nothing has leaked.
    if (Map* on = FindMap())
    {
        if (WorldObject* obj = on->GetWorldObject(target))
        {
            obj->RemoveObserver(GetObjectGuid());
        }
    }
}

void Player::ForgetEverythingSeen()
{
    GuidSet const held = m_clientGUIDs;
    for (ObjectGuid const& guid : held)
    {
        ForgetSeen(guid);
    }

    // Whatever could not be resolved above is dropped here anyway: the client threw its
    // world away, so believing otherwise is the one error that cannot be recovered from --
    // anything still listed is skipped by UpdateVisibilityOf and never sent again.
    m_clientGUIDs.clear();
}
