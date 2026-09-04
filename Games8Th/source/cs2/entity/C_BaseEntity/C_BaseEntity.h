#pragma once
#include <cstdint>
#include <cstring>
#include "../C_EntityInstance/C_EntityInstance.h"
#include "../../../Games8Th/utils/memory/memorycommon.h"
#include "../../../Games8Th/utils/math/vector/vector.h"
#include "../../../../source/Games8Th/utils/schema/schema.h"
#include "../../../../source/Games8Th/utils/memory/vfunc/vfunc.h"
#include "../../sdk/CUtlSymbolLarge.h"
#include "../handle.h"

// CModelState - model name lookup for HUD model classification
class CModelState
{
public:
    char pad0[0x80];
    schema(CUtlSymbolLarge, m_ModelName, "CModelState->m_ModelName");
};

class CGameSceneNode
{
public:
	schema(Vector_t, m_vecAbsOrigin, "CGameSceneNode->m_vecAbsOrigin");
	schema(bool, m_bDormant, "CGameSceneNode->m_bDormant");
	schema(CGameSceneNode*, m_pChild, "CGameSceneNode->m_pChild");
	schema(CGameSceneNode*, m_pNextSibling, "CGameSceneNode->m_pNextSibling");
	schema(CEntityInstance*, m_pOwner, "CGameSceneNode->m_pOwner");
	schema(CModelState, m_modelState, "CSkeletonInstance->m_modelState");
};

// Collision hull used for perspective-correct ESP boxes (mins/maxs + origin -> 8 corners)
class CCollisionProperty
{
public:
	schema(Vector_t, m_vecMins, "CCollisionProperty->m_vecMins");
	schema(Vector_t, m_vecMaxs, "CCollisionProperty->m_vecMaxs");
};

class C_BaseEntity : public CEntityInstance
{
public:
	schema(CGameSceneNode*, m_pGameSceneNode, "C_BaseEntity->m_pGameSceneNode");
	schema(CCollisionProperty*, m_pCollision, "C_BaseEntity->m_pCollision");
	schema(int, m_iMaxHealth, "C_BaseEntity->m_iMaxHealth");
	schema(std::
int32_t, m_iHealth, "C_BaseEntity->m_iHealth");
	schema(std::
uint8_t, m_lifeState, "C_BaseEntity->m_lifeState");
	schema(std::
uint8_t, m_iTeamNum, "C_BaseEntity->m_iTeamNum");
	schema(std::
uint32_t, m_fFlags, "C_BaseEntity->m_fFlags");
	schema(float, m_flSimulationTime, "C_BaseEntity->m_flSimulationTime");
	schema(float, m_flOldSimulationTime, "C_BasePlayerPawn->m_flOldSimulationTime");
	schema(CBaseHandle, m_hOwnerEntity, "C_BaseEntity->m_hOwnerEntity");
	schema(CBaseHandle, m_hController, "C_BasePlayerPawn->m_hController");
	bool IsBasePlayer()
	{
		char name[128]{};
		if (!Mem::SchemaClassName(this, name, sizeof(name)))
			return false;
		if (hash_32_fnv1a_const(name) == hash_32_fnv1a_const("C_CSPlayerPawn"))
			return true;
		return std::strstr(name, "PlayerPawn") != nullptr;
	}

	bool IsViewmodelAttachment()
	{
		char name[128]{};
		if (!Mem::SchemaClassName(this, name, sizeof(name)))
			return false;
		if (hash_32_fnv1a_const(name) == hash_32_fnv1a_const("C_CS2HudModelArms"))
			return true;
		return std::strstr(name, "HudModelArms") != nullptr;
	}

	bool IsViewmodel()
	{
		char name[128]{};
		if (!Mem::SchemaClassName(this, name, sizeof(name)))
			return false;
		const auto h = hash_32_fnv1a_const(name);
		if (h == hash_32_fnv1a_const("C_CS2HudModelWeapon")
			|| h == hash_32_fnv1a_const("C_CS2HudModelAddon")
			|| h == hash_32_fnv1a_const("C_CS2HudModelBase"))
			return true;
		if (std::strstr(name, "HudModelWeapon")
			|| std::strstr(name, "HudModelAddon"))
			return true;
		return false;
	}

	bool IsPlayerController()
	{
		char name[128]{};
		if (!Mem::SchemaClassName(this, name, sizeof(name)))
			return false;
		return hash_32_fnv1a_const(name) == hash_32_fnv1a_const("CCSPlayerController");
	}
};


