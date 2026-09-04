#pragma once
#include "..\..\..\..\source\Games8Th\utils\schema\schema.h"
#include "..\..\..\..\source\Games8Th\utils\memory\vfunc\vfunc.h"
#include "..\handle.h"

class CEntityInstance;
class CEntityIdentity
{
public:
	SCHEMA_ADD_OFFSET(std::
uint32_t, index, 0x10);
	schema(const char*, m_designerName, "CEntityIdentity->m_designerName");
	schema( std::
uint32_t, flags, "CEntityIdentity->m_flags");

	[[nodiscard]] bool valid()
	{
		return index() != INVALID_EHANDLE_INDEX;
	}

	[[nodiscard]] int get_index()
	{
		if (!valid())
			return ENT_ENTRY_MASK;

		return index() & ENT_ENTRY_MASK;
	}

	[[nodiscard]] int get_serial_number()
	{
		return index() >> NUM_SERIAL_NUM_SHIFT_BITS;
	}

	CEntityInstance* pInstance;
};

class CEntityInstance
{
public:

	void dump_class_info(SchemaClassInfoData_t** pReturn)
	{
		if (!pReturn || !this || !Mem::IsUserPtr(this))
			return;
		M::
vfunc<void, 46U>(this, pReturn);
	}


	[[nodiscard]] CBaseHandle handle()
	{
		if (!this || !Mem::IsUserPtr(this))
			return CBaseHandle();
		CEntityIdentity* identity = m_pEntityIdentity();
		if (!identity || !Mem::IsUserPtr(identity))
			return CBaseHandle();
		const auto ida = reinterpret_cast<std::
uintptr_t>(identity);
		if (ida < 0x10000ull || ida > 0x00007FFFFFFFFFFFull)
			return CBaseHandle();
		if (!identity->valid())
			return CBaseHandle();
		return CBaseHandle(identity->get_index(), identity->get_serial_number() - (identity->flags() & 1));
	}

	schema(CEntityIdentity*, m_pEntityIdentity, "CEntityInstance->m_pEntity");
};


