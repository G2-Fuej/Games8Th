#pragma once
#include <cstdint>
#include "..\C_EntityInstance\C_EntityInstance.h"
#include "../../../Games8Th/utils/memory/memorycommon.h"
#include "../../../Games8Th/utils/math/vector/vector.h"
#include "..\..\..\..\source\Games8Th\utils\schema\schema.h"
#include "..\..\..\..\source\Games8Th\utils\memory\vfunc\vfunc.h"
#include "..\handle.h"
class CCSPlayer_WeaponServices{public:
schema(CBaseHandle, m_hActiveWeapon, "CPlayer_WeaponServices->m_hActiveWeapon");
}
;
class CPlayer_ObserverServices{public:
schema(std::
uint8_t, m_iObserverMode, "CPlayer_ObserverServices->m_iObserverMode");
	schema(CBaseHandle, m_hObserverTarget, "CPlayer_ObserverServices->m_hObserverTarget");
	// Dump +0x54 ??" client forces mode locally (bypasses some team filters)
schema(bool, m_bForcedObserverMode, "CPlayer_ObserverServices->m_bForcedObserverMode");
}
;
class CCSWeaponBaseVData{public:
schema(const char*, m_szName, "CCSWeaponBaseVData->m_szName");
	// Weapon type from VData (CSWeaponType_t) - used by 's exact IsNonGunWeapon
schema(int, m_WeaponType, "CCSWeaponBaseVData->m_WeaponType");
// Full-auto flag (semi pistols false) - schema dump 0x734
	schema(bool, m_bIsFullAuto, "CCSWeaponBaseVData->m_bIsFullAuto");
schema(int, m_nNumBullets, "CCSWeaponBaseVData->m_nNumBullets");
	// Damage / pen (autowall + mindamage)
	schema(int, m_nDamage, "CCSWeaponBaseVData->m_nDamage");
	schema(int, m_iMaxClip1, "C_BasePlayerWeaponVData->m_iMaxClip1");
	schema(float, m_flArmorRatio, "CCSWeaponBaseVData->m_flArmorRatio");
	schema(float, m_flRange, "CCSWeaponBaseVData->m_flRange");
	schema(float, m_flRangeModifier, "CCSWeaponBaseVData->m_flRangeModifier");
	schema(float, m_flPenetration, "CCSWeaponBaseVData->m_flPenetration");
	schema(float, m_flHeadshotMultiplier, "CCSWeaponBaseVData->m_flHeadshotMultiplier");
// Grenade base throw speed (HE/flash ~750, molly/inc ~700)
schema(float, m_flThrowVelocity, "CCSWeaponBaseVData->m_flThrowVelocity");
	// Hide viewmodel while zoomed (schema dump 0x7F9) ??" AWP/SSG true by default
schema(bool, m_bHideViewModelWhenZoomed, "CCSWeaponBaseVData->m_bHideViewModelWhenZoomed");
	// Bullet speed (u/s) ??" schema resolved at runtime
schema(float, m_flBulletSpeed, "CCSWeaponBaseVData->m_flBulletSpeed");
// float[2]
	// Primary mode index = C_CSWeaponBase::m_weaponMode (0 hip / 1 alt).
schema_arr(float, m_flCycleTimePrimary, "CCSWeaponBaseVData->m_flCycleTime", 0);
	schema_arr(float, m_flCycleTimeSecondary, "CCSWeaponBaseVData->m_flCycleTime", 1);
// float[2].
	// GetInaccuracy composes these; seed gates should use live mode, not hardcoded defs.
	schema_arr(float, m_flSpread0, "CCSWeaponBaseVData->m_flSpread", 0);
	schema_arr(float, m_flSpread1, "CCSWeaponBaseVData->m_flSpread", 1);
	schema_arr(float, m_flInaccuracyStand0, "CCSWeaponBaseVData->m_flInaccuracyStand", 0);
	schema_arr(float, m_flInaccuracyStand1, "CCSWeaponBaseVData->m_flInaccuracyStand", 1);
	schema_arr(float, m_flInaccuracyJump0, "CCSWeaponBaseVData->m_flInaccuracyJump", 0);
	schema_arr(float, m_flInaccuracyJump1, "CCSWeaponBaseVData->m_flInaccuracyJump", 1);
	schema_arr(float, m_flInaccuracyFire0, "CCSWeaponBaseVData->m_flInaccuracyFire", 0);
	schema_arr(float, m_flInaccuracyFire1, "CCSWeaponBaseVData->m_flInaccuracyFire", 1);
	schema_arr(float, m_flInaccuracyMove0, "CCSWeaponBaseVData->m_flInaccuracyMove", 0);
	schema_arr(float, m_flInaccuracyMove1, "CCSWeaponBaseVData->m_flInaccuracyMove", 1);
	schema(float, m_flRecoveryTimeStand, "CCSWeaponBaseVData->m_flRecoveryTimeStand");
	schema(float, m_flRecoveryTimeCrouch, "CCSWeaponBaseVData->m_flRecoveryTimeCrouch");
}
;
class C_CSWeaponBase {public:
schema(bool, m_bInReload, "C_CSWeaponBase->m_bInReload");
	// Inspect anim (replaces removed CCSPlayer_WeaponServices look-at bools)
schema(bool, m_bInspectPending, "C_CSWeaponBase->m_bInspectPending");
	schema(bool, m_bInspectShouldLoop, "C_CSWeaponBase->m_bInspectShouldLoop");
	schema(std::
int32_t, m_iClip1, "C_BasePlayerWeapon->m_iClip1");
	// Fire-rate gate: compare to controller m_nTickBase (+1 slack)
schema(std::
int32_t, m_nNextPrimaryAttackTick, "C_BasePlayerWeapon->m_nNextPrimaryAttackTick");
// class is C4)
schema(bool, m_bStartedArming, "C_C4->m_bStartedArming");
	schema_pfield2(std::
uint16_t, m_iItemDefinitionIndex, "C_EconEntity->m_AttributeManager", 0x50, "C_EconItemView->m_iItemDefinitionIndex");
	// Accuracy (hitchance / CalcSpread)
schema(int, m_weaponMode, "C_CSWeaponBase->m_weaponMode");
	schema(float, m_flTurningInaccuracy, "C_CSWeaponBase->m_flTurningInaccuracy");
	schema(float, m_fAccuracyPenalty, "C_CSWeaponBase->m_fAccuracyPenalty");
	schema(float, m_flRecoilIndex, "C_CSWeaponBase->m_flRecoilIndex");
	// Scope zoom lives on C_CSWeaponBaseGun (not base) ??" dump 0x1CE0
schema(std::
int32_t, m_zoomLevel, "C_CSWeaponBaseGun->m_zoomLevel");
	// Grenade throw state (C_BaseCSGrenade) - dump 0x1CF0 / 0x1CE3
schema(float, m_flThrowStrength, "C_BaseCSGrenade->m_flThrowStrength");
	schema(bool, m_bPinPulled, "C_BaseCSGrenade->m_bPinPulled");
	CCSWeaponBaseVData* Data();
	bool IsNonGunWeapon() const;
}
;
// Planted bomb (C_PlantedC4) - cast entity when class name matches
class C_PlantedC4 {
public:
schema(bool, m_bBombTicking, "C_PlantedC4->m_bBombTicking");
	schema(std::
int32_t, m_nBombSite, "C_PlantedC4->m_nBombSite");
	schema(float, m_flC4Blow, "C_PlantedC4->m_flC4Blow");
	schema(bool, m_bCannotBeDefused, "C_PlantedC4->m_bCannotBeDefused");
	schema(bool, m_bHasExploded, "C_PlantedC4->m_bHasExploded");
	schema(float, m_flTimerLength, "C_PlantedC4->m_flTimerLength");
	schema(bool, m_bBeingDefused, "C_PlantedC4->m_bBeingDefused");
	schema(float, m_flDefuseLength, "C_PlantedC4->m_flDefuseLength");
	schema(float, m_flDefuseCountDown, "C_PlantedC4->m_flDefuseCountDown");
	schema(bool, m_bBombDefused, "C_PlantedC4->m_bBombDefused");
}
;


