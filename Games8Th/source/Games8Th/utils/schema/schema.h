#pragma once
#include <memory>
#include <type_traits>

#include "..\fnv1a\fnv1a.h"
#include "..\memory\vfunc\vfunc.h"
#include "..\memory\memsafe\memsafe.h"
#include "..\math\utlmemory\utlmemory.h"
#include "..\math\utlvector\utlvector.h"
#include "..\..\..\cs2\datatypes\schema\ISchemaClass\ISchemaClass.h"

// Schema access: cheap user-range check only (no VirtualQuery - FPS critical)
// Lazy offset: retry until Schema::init fills the map (static const 0 was sticky forever).
#define SCHEMA_ADD_OFFSET(TYPE, NAME, OFFSET)                                                                   \
    [[nodiscard]] inline std::add_lvalue_reference_t<TYPE> NAME()                                            \
    {                                                                                                           \
        static TYPE s_fallback{};                                                                              \
        static std::uint32_t uOffset = 0;                                                                      \
        if (!uOffset) {                                                                                        \
            uOffset = (OFFSET);                                                                                \
            if (!uOffset)                                                                                      \
                return s_fallback;                                                                             \
        }                                                                                                       \
        auto* base = reinterpret_cast<std::uint8_t*>(this);                                                    \
        if (!Mem::IsUserPtr(base))                                                                             \
            return s_fallback;                                                                                 \
        auto* p = reinterpret_cast<std::add_pointer_t<TYPE>>(base + uOffset);                                  \
        if (!Mem::Probe(p, sizeof(TYPE)))                                                                      \
            return s_fallback;                                                                                 \
        return *p;                                                                                             \
    }

#define SCHEMA_ADD_POFFSET(TYPE, NAME, OFFSET)                                                                 \
    [[nodiscard]] inline std::add_pointer_t<TYPE> NAME()                                                    \
    {                                                                                                          \
        static std::uint32_t uOffset = 0;                                                                      \
        if (!uOffset) {                                                                                        \
            uOffset = (OFFSET);                                                                                \
            if (!uOffset)                                                                                      \
                return nullptr;                                                                                \
        }                                                                                                       \
        auto* base = reinterpret_cast<std::uint8_t*>(this);                                                    \
        if (!Mem::IsUserPtr(base))                                                                             \
            return nullptr;                                                                                    \
        auto* p = reinterpret_cast<std::add_pointer_t<TYPE>>(base + uOffset);                                  \
        if (!Mem::Probe(p, 1))                                                                                 \
            return nullptr;                                                                                    \
        return p;                                                                                              \
    }

#define SCHEMA_ARRAY(TYPE, NAME, FIELD) \
    [[nodiscard]] inline TYPE* NAME() { \
        static uint32_t uOffset = 0; \
        if (!uOffset) \
            uOffset = SchemaFinder::Get(hash_32_fnv1a_const(FIELD)); \
        auto* base = reinterpret_cast<std::uint8_t*>(this); \
        if (!uOffset || !Mem::IsUserPtr(base)) \
            return nullptr; \
        TYPE* p = nullptr; \
        __try { p = reinterpret_cast<TYPE*>(base + uOffset); } \
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; } \
        return p; \
    }

#define schema(TYPE, NAME, FIELD)  SCHEMA_ADD_OFFSET(TYPE, NAME, SchemaFinder::Get(hash_32_fnv1a_const(FIELD)) + 0u)

// Schema array element: base field (e.g. CFiringModeFloat float[2]) + IDX*sizeof(TYPE)
#define schema_arr(TYPE, NAME, FIELD, IDX) SCHEMA_ADD_OFFSET(TYPE, NAME, SchemaFinder::Get(hash_32_fnv1a_const(FIELD)) + ((ptrdiff_t)(IDX) * (ptrdiff_t)sizeof(TYPE)))

#define schema_pfield(TYPE, NAME, FIELD, ADDITIONAL) SCHEMA_ADD_OFFSET(TYPE, NAME, SchemaFinder::Get(hash_32_fnv1a_const(FIELD)) + ADDITIONAL)

// Two-hop schema chain (outer field + fixed container delta + inner field)
#define schema_pfield2(TYPE, NAME, FIELD1, ADD1, FIELD2) SCHEMA_ADD_OFFSET(TYPE, NAME, SchemaFinder::Get(hash_32_fnv1a_const(FIELD1)) + (ptrdiff_t)(ADD1) + SchemaFinder::Get(hash_32_fnv1a_const(FIELD2)))

#define SCHEMA_ADD_RAW_OFFSET(TYPE, NAME, OFFSET) \
    [[nodiscard]] inline TYPE NAME() noexcept \
    { \
        TYPE fallback{}; \
        if (!Mem::IsUserPtr(this)) \
            return fallback; \
        TYPE v{}; \
        if (!Mem::ReadField(this, OFFSET, v)) \
            return fallback; \
        return v; \
    }

#define add_offset_near(_class, _name, _type, _field_name, _offset)              \
[[nodiscard]] inline std::add_lvalue_reference_t<_type> _name()                  \
{                                                                                \
    static _type s_fallback{};                                                   \
    static const uint32_t baseOffset = SchemaFinder::Get(                        \
        hash_32_fnv1a_const(_field_name)                                         \
    );                                                                           \
    static const uint32_t totalOffset = baseOffset + (_offset);                  \
    auto* base = reinterpret_cast<uint8_t*>(this);                               \
    if (!Mem::IsUserPtr(base))                                                   \
        return s_fallback;                                                       \
    auto* p = reinterpret_cast<std::add_pointer_t<_type>>(base + totalOffset);   \
    if (!Mem::Probe(p, sizeof(_type)))                                           \
        return s_fallback;                                                       \
    return *p;                                                                   \
}

class Schema {
public:
    bool init(const char* module_name, int module_type);

    ISchemaSystem* schema_system = nullptr;

};

namespace SchemaFinder {

    [[nodiscard]] std::
uint32_t Get(const uint32_t hashed);
    [[nodiscard]] std::
uint32_t GetExternal(const char* moduleName, const uint32_t HashedClass, const uint32_t HashedFieldName);
 // True after Schema::init filled the map (so a 0 from Get is a real miss).
    [[nodiscard]] bool Ready();
}

