#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "../memsafe/memsafe.h"

namespace M {
	template <typename T, std::size_t nIndex, class CBaseClass, typename... Args_t>
	static inline T vfunc(CBaseClass* thisptr, Args_t... argList) {
		using VirtualFn_t = T(__thiscall*)(const void*, decltype(argList)...);
		if (!thisptr || !Mem::IsUserPtr(thisptr)) {
			if constexpr (std::is_void_v<T>)
				return;
			else
				return T{};
		}
		void* vt = nullptr;
		if (!Mem::Read(thisptr, vt) || !Mem::IsUserPtr(vt)) {
			if constexpr (std::is_void_v<T>)
				return;
			else
				return T{};
		}
		void* fn = nullptr;
		if (!Mem::Read(reinterpret_cast<void**>(vt) + nIndex, fn) || !Mem::IsUserPtr(fn)) {
			if constexpr (std::is_void_v<T>)
				return;
			else
				return T{};
		}
		if constexpr (std::is_void_v<T>) {
			__try {
				reinterpret_cast<VirtualFn_t>(fn)(thisptr, argList...);
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
			return;
		} else {
			T result{};
			__try {
				result = reinterpret_cast<VirtualFn_t>(fn)(thisptr, argList...);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return T{};
			}
			return result;
		}
	}

	template <typename T, std::size_t nIndex, class CBaseClass, typename... Args_t>
	static inline T CallVFunc(CBaseClass* thisptr, Args_t... argList)
	{
		return vfunc<T, nIndex, CBaseClass, Args_t...>(thisptr, argList...);
	}
}

