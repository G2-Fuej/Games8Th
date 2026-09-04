#include "..\..\..\Games8Th\hooks\hooks.h"
#include "..\..\..\Games8Th\interfaces\interfaces.h"

#include "cutlbuffer.h"

CUtlBuffer::
CUtlBuffer(int a1, int size, int a3)
{
	// Export miss leaves these null (interfaces.cpp only logs) - never call through
	if (!I::ConstructUtlBuffer)
		return;
	I::ConstructUtlBuffer(this, a1, size, a3);
}

void CUtlBuffer::
ensure(int size)
{
	if (!I::EnsureCapacityBuffer)
		return;
	I::EnsureCapacityBuffer(this, size);
}

void CUtlBuffer::
PutString(const char* szString)
{
	if (!I::PutUtlString || !szString)
		return;
	I::PutUtlString(this, szString);
}


