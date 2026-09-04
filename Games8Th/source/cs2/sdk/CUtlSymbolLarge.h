#pragma once
#include <cstdint>
#include "../../Games8Th/utils/memory/memsafe/memsafe.h"

#define UTL_INVAL_SYMBOL_LARGE 0

using UtlSymLargeId_t = std::intptr_t;

// Game-side CUtlSymbolLarge overlay. u.m_pAsString only valid for inline
// symbols; String() falls back to "" on invalid ids.
class CUtlSymbolLarge
{
public:
    CUtlSymbolLarge() { u.m_Id = UTL_INVAL_SYMBOL_LARGE; }

    CUtlSymbolLarge(UtlSymLargeId_t id) { u.m_Id = id; }

    inline const char* String() const
    {
        if (u.m_Id == UTL_INVAL_SYMBOL_LARGE || u.m_pAsString == nullptr)
            return "";
        if (!Mem::IsUserPtr(u.m_pAsString))
            return "";
        return u.m_pAsString;
    }

    inline bool IsValid() const { return u.m_Id != UTL_INVAL_SYMBOL_LARGE; }

    union
    {
        UtlSymLargeId_t m_Id;
        const char* m_pAsString;
    } u;
};

