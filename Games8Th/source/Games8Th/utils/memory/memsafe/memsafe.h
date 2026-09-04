#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <Windows.h>

#include "../../fnv1a/fnv1a.h"

// Defined in utils/schema/schema.cpp (schema.h includes this header - forward
// declare, never include, to keep the include graph acyclic).
namespace SchemaFinder {
	[[nodiscard]] std::uint32_t Get(const std::uint32_t hashed);
}

// Schema-cached offsets for the identity walks (fallback = last dump value).
// Retry-until-ready: pre-init miss stays uncached so init can upgrade. Only
// non-zero schema hits are cached - a 0 never poisons the static.
[[nodiscard]] inline std::uint32_t OffEntityIdentity() noexcept {
	static std::uint32_t c = 0;
	if (c)
		return c;
	const std::uint32_t o = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_pEntity"));
	if (o) {
		c = o;
		return o;
	}
	return 0x10;
}
[[nodiscard]] inline std::uint32_t OffDesignerName() noexcept {
	static std::uint32_t c = 0;
	if (c)
		return c;
	const std::uint32_t o = SchemaFinder::Get(hash_32_fnv1a_const("CEntityIdentity->m_designerName"));
	if (o) {
		c = o;
		return o;
	}
	return 0x20;
}

// Lightweight pointer guards for hot paths (ESP/aim/schema).
// NO VirtualQuery on the hot path - that destroyed FPS (~10).
// Use IsReadableHeavy() only for rare/untrusted pointers.
namespace Mem {

inline constexpr std::uintptr_t kMinUser = 0x10000ull;
inline constexpr std::uintptr_t kMaxUser = 0x00007FFFFFFFFFFFull;
// Full CBaseHandle entry range (0x7FFF mask). HUD/viewmodel/hands live well above 2048.
	// Iteration caps (ESP loops) should still clamp GetHighestEntityIndex if needed.
	constexpr int kMaxEntityIndex = 0x7FFE;
constexpr int kMaxPlayers = 64;
constexpr int kMinHealth = 0;
constexpr int kMaxHealth = 200;
constexpr int kMaxArmor = 100;
constexpr int kMaxTeam = 3;
constexpr int kMaxBoneIndex = 128;

[[nodiscard]] inline bool IsUserPtr(const void* p) noexcept {
	const auto a = reinterpret_cast<std::
uintptr_t>(p);
	return a >= kMinUser && a <= kMaxUser;
}

[[nodiscard]] inline bool IsReadable(const void* p, std::size_t size) noexcept;

// Touch first/last byte under SEH. Range checks miss decommitted pages.
[[nodiscard]] inline bool Probe(const void* p, std::size_t size = 1) noexcept {
	if (!IsReadable(p, size))
		return false;
	volatile char sink = 0;
	__try {
		const char* b = static_cast<const char*>(p);
		sink = b[0];
		if (size > 1)
			sink = b[size - 1];
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	(void)sink;
	return true;
}

// Hot path: range only (no syscall)
[[nodiscard]] inline bool IsReadable(const void* p, std::
size_t size) noexcept {
	if (!p || size == 0)
		return false;
	const auto start = reinterpret_cast<std::
uintptr_t>(p);
	if (start < kMinUser || start > kMaxUser)
		return false;
	if (size > kMaxUser || start + size < start || start + size - 1 > kMaxUser)
		return false;
	return true;
}

// Rare path only - VirtualQuery (expensive)
[[nodiscard]] bool IsReadableHeavy(const void* p, std::
size_t size) noexcept;

[[nodiscard]] inline bool Valid(const void* p, std::
size_t minBytes = sizeof(void*)) noexcept {
	return IsReadable(p, minBytes);
}

template <typename T>
[[nodiscard]] inline bool Read(const void* p, T& out) noexcept {
	if (!IsReadable(p, sizeof(T)))
		return false;
	// SEH: user-range check doesn't detect decommitted pages (entity teardown
	// race). Memcpy AVs otherwise; wrap so hot paths cannot bubble it up.
	__try {
		std::
memcpy(&out, p, sizeof(T));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

template <typename T>
[[nodiscard]] inline T ReadOr(const void* p, T fallback = T{}) noexcept {
	T v{};
	if (!Read(p, v))
		return fallback;
	return v;
}

[[nodiscard]] inline void* ReadPtr(const void* p) noexcept {
	return ReadOr<void*>(p, nullptr);
}

template <typename T>
[[nodiscard]] inline bool ReadField(const void* base, std::
size_t offset, T& out) noexcept {
	if (!IsUserPtr(base))
		return false;
	return Read(reinterpret_cast<const std::
uint8_t*>(base) + offset, out);
}

template <typename T>
[[nodiscard]] inline T ReadFieldOr(const void* base, std::
size_t offset, T fallback = T{}) noexcept {
	T v{};
	if (!ReadField(base, offset, v))
		return fallback;
	return v;
}

[[nodiscard]] inline int ClampHealth(int h) noexcept {
	if (h < kMinHealth) return kMinHealth;
	if (h > kMaxHealth) return kMaxHealth;
	return h;
}
[[nodiscard]] inline int ClampArmor(int a) noexcept {
	if (a < 0) return 0;
	if (a > kMaxArmor) return kMaxArmor;
	return a;
}
[[nodiscard]] inline int ClampEntityIndex(int i) noexcept {
	if (i < 0) return 0;
	if (i > kMaxEntityIndex) return kMaxEntityIndex;
	return i;
}
[[nodiscard]] inline bool ValidEntityIndex(int i) noexcept {
	return i >= 0 && i <= kMaxEntityIndex;
}
[[nodiscard]] inline bool ValidBoneIndex(int i) noexcept {
	return i >= 0 && i <= kMaxBoneIndex;
}
[[nodiscard]] inline bool ValidTeam(int t) noexcept {
	return t >= 0 && t <= kMaxTeam;
}
[[nodiscard]] inline bool ValidHealth(int h) noexcept {
	return h >= kMinHealth && h <= kMaxHealth;
}

// Entity: user-range + first qword (vtable) looks like a pointer.
// SEH-wrapped: engine can decommit an entity pool page while we hold a stale
// pointer (round teardown / TDM respawn) - bare memcpy AVs otherwise.
[[nodiscard]] inline bool ValidEntity(const void* ent) noexcept {
	if (!IsUserPtr(ent))
		return false;
	void* vt = nullptr;
	__try {
		std::
memcpy(&vt, ent, sizeof(vt));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return IsUserPtr(vt);
}

[[nodiscard]] inline bool Finite(float f) noexcept {
	return f == f && f > -1.0e12f && f < 1.0e12f;
}

// Copy a C string under SEH. Range-only IsReadable cannot see a freed
// string page - identity designer / schema names die on OnAdd/OnRemove.
[[nodiscard]] inline bool PeekCString(const char* p, char* out, std::size_t cap) noexcept {
	if (!out || cap < 2)
		return false;
	out[0] = '\0';
	if (!IsUserPtr(p))
		return false;
	__try {
		std::size_t n = 0;
		for (; n + 1 < cap; ++n) {
			const char c = p[n];
			out[n] = c;
			if (c == '\0')
				return true;
		}
		out[cap - 1] = '\0';
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out[0] = '\0';
		return false;
	}
}

// Schema name: identity -> CEntityClass+0x58 -> SchemaClassInfo+0x8.
[[nodiscard]] inline bool SchemaClassName(const void* entity, char* out, std::size_t cap) noexcept {
	if (out && cap)
		out[0] = '\0';
	void* identity = nullptr;
	if (!ReadField(entity, OffEntityIdentity(), identity) || !identity)
		return false;
	void* entityClass = nullptr;
	if (!ReadField(identity, 0x8, entityClass) || !entityClass)
		return false;
	void* classInfo = nullptr;
	if (!ReadField(entityClass, 0x58, classInfo) || !classInfo)
		return false;
	const char* name = nullptr;
	if (!ReadField(classInfo, 0x8, name) || !name)
		return false;
	return PeekCString(name, out, cap);
}

[[nodiscard]] inline bool DesignerName(const void* entity, char* out, std::size_t cap) noexcept {
	if (out && cap)
		out[0] = '\0';
	void* identity = nullptr;
	if (!ReadField(entity, OffEntityIdentity(), identity) || !identity)
		return false;
	const char* name = nullptr;
	if (!ReadField(identity, OffDesignerName(), name) || !name)
		return false;
	return PeekCString(name, out, cap);
}

} // namespace Mem

