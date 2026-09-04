#pragma once
#include <cstdint>
#include <utility>
#include <Windows.h>
#include "../../../external/safetyhook/safetyhook.hpp"

#define CS_CONCATENATE_DETAIL(x, y) x ## y
#define CS_CONCATENATE(x, y) CS_CONCATENATE_DETAIL(x, y)

#define CS_CLASS_NO_CONSTRUCTOR(CLASS) \
    CLASS() = delete; \
    CLASS(CLASS&&) = delete; \
    CLASS(const CLASS&) = delete;

#define CS_CLASS_NO_ASSIGNMENT(CLASS) \
    CLASS& operator=(CLASS&&) = delete; \
    CLASS& operator=(const CLASS&) = delete;

#define CS_CLASS_NO_INITIALIZER(CLASS) \
    CS_CLASS_NO_CONSTRUCTOR(CLASS) \
    CS_CLASS_NO_ASSIGNMENT(CLASS)

#define MEM_PAD(SIZE) \
    private: \
    char CS_CONCATENATE(pad_, __COUNTER__)[SIZE]; \
    public:

// SafetyHook-backed inline hook. Call sites keep Add/Remove/Restore/Replace/GetOriginal.
template <typename T>
class CInlineHookObj
{
public:
    CInlineHookObj() = default;
    CInlineHookObj(const CInlineHookObj&) = delete;
    CInlineHookObj& operator=(const CInlineHookObj&) = delete;
    CInlineHookObj(CInlineHookObj&&) = default;
    CInlineHookObj& operator=(CInlineHookObj&&) = default;

    bool Add(void* pFunction, void* pDetour)
    {
        if (pFunction == nullptr || pDetour == nullptr)
            return false;

        m_hook.reset();

        auto result = safetyhook::InlineHook::create(pFunction, pDetour);
        if (!result)
            return false;

        m_hook = std::move(*result);
        return static_cast<bool>(m_hook) && m_hook.enabled();
    }

    bool Remove()
    {
        m_hook.reset();
        return true;
    }

    bool Restore()
    {
        if (!m_hook || !m_hook.enabled())
            return false;

        return m_hook.disable().has_value();
    }

    inline T GetOriginal()
    {
        return m_hook ? m_hook.original<T>() : T{};
    }

    inline void* GetTarget() const
    {
        return m_hook ? static_cast<void*>(m_hook.target()) : nullptr;
    }

    inline bool IsHooked() const
    {
        return static_cast<bool>(m_hook) && m_hook.enabled();
    }

private:
    SafetyHookInline m_hook{};
};

