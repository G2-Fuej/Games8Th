#include "CCSPlayerController.h"
#include "../../../Games8Th/utils/schema/schema.h"
#include <Windows.h>
#include "../../../Games8Th/utils/console/console.h"

CCSPlayerController::
CCSPlayerController(uintptr_t address) : address(address) {}

uintptr_t CCSPlayerController::
getAddress() const {
	return reinterpret_cast<uintptr_t>(this);
}

bool CCSPlayerController::
ReadSanitizedName(char* buf, size_t bufSize) const {
	if (!buf || bufSize == 0)
		return false;
	buf[0] = '\0';
	if (!this || !Mem::IsUserPtr(this))
		return false;

	bool ok = false;
	__try {
		const uint32_t off = SchemaFinder::
Get(
			hash_32_fnv1a_const("CCSPlayerController->m_sSanitizedPlayerName"));
		if (!off)
			return false;

		const char* p = nullptr;
		if (!Mem::ReadField(this, off, p) || !p)
			return false;
		if (!Mem::PeekCString(p, buf, bufSize))
			return false;
		ok = buf[0] != '\0';
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		TW_SEH_CATCH("controller.readName");
		buf[0] = '\0';
		return false;
	}
	return ok;
}

