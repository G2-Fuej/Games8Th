#include "C_CSWeaponBase.h"
#include "..\..\..\Games8Th\hooks\hooks.h"
#include "..\..\..\Games8Th\features\sdk_prio_a\sdk_prio_a.h"
#include <cstring>
#include "..\C_EntityInstance\C_EntityInstance.h"
#include "..\..\..\Games8Th\utils\console\console.h"
CCSWeaponBaseVData* C_CSWeaponBase::
Data()
{
	if (!this || !Mem::ValidEntity(this))
		return nullptr;
	// Primary: field offset (H::oGetWeaponData). Fallback: dump GetEconWpnData.
	if (H::
oGetWeaponData > 0) {
		CCSWeaponBaseVData* viaOff = nullptr;
		__try {
			viaOff = *reinterpret_cast<CCSWeaponBaseVData**>(
				(uintptr_t)
this + H::
oGetWeaponData);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			TW_SEH_CATCH("weapon.getData");
			viaOff = nullptr;
		}
		if (viaOff && Mem::IsUserPtr(viaOff))
			return viaOff;
	}
	if (void* viaFn = SdkPrioA::
GetEconWpnData(this))
		return Mem::IsUserPtr(viaFn) ? reinterpret_cast<CCSWeaponBaseVData*>(viaFn) : nullptr;
	return nullptr;
}

bool C_CSWeaponBase::
IsNonGunWeapon() const
{
	if (!this || !Mem::ValidEntity(this))
		return false;
	char name[128]{};
	if (Mem::SchemaClassName(this, name, sizeof(name))) {
		const char* n = name;
		if (strstr(n, "Knife") || strstr(n, "Taser") || strstr(n, "Grenade") || strstr(n, "C4") ||
			strstr(n, "Flash") || strstr(n, "Smoke") || strstr(n, "Molotov") || strstr(n, "Decoy") ||
			strstr(n, "Shield") || strstr(n, "BaseItem") || strstr(n, "Tablet") || strstr(n, "Fists") ||
			strstr(n, "Melee") || strstr(n, "Healthshot") || strstr(n, "BumpMine"))
			return true;
		// Named gun classes: C_Weapon*, C_DEagle, C_AK47, etc.
		if (strstr(n, "Weapon") || strstr(n, "DEagle") || strstr(n, "AK47") || strstr(n, "M4A") ||
			strstr(n, "SSG") || strstr(n, "AWP") || strstr(n, "Glock") || strstr(n, "USP") ||
			strstr(n, "P250") || strstr(n, "FiveSeven") || strstr(n, "Tec9") || strstr(n, "Elite") ||
			strstr(n, "Revolver") || strstr(n, "Negev") || strstr(n, "M249") || strstr(n, "Nova") ||
			strstr(n, "XM1014") || strstr(n, "MAG7") || strstr(n, "Sawed") || strstr(n, "MAC10") ||
			strstr(n, "MP") || strstr(n, "P90") || strstr(n, "Bizon") || strstr(n, "UMP") ||
			strstr(n, "Galil") || strstr(n, "Famas") || strstr(n, "SG556") || strstr(n, "AUG") ||
			strstr(n, "SCAR") || strstr(n, "G3SG") || strstr(n, "Scout"))
			return false;
	}

	auto* vdata = const_cast<C_CSWeaponBase*>(this)->Data();
	if (vdata) {
		const int wtype = vdata->m_WeaponType();
		// CS2 CCSWeaponType: knife=0, pistol=1, submachinegun=2, rifle=3, shotgun=4, sniper=5, machinegun=6, c4=7, taser=8, grenade=9, equipment=10...
		if (wtype >= 1 && wtype <= 6)
			return false;
		return true;
	}

	// Unknown: allow aimbot attempt rather than hard-block
	return false;
}


