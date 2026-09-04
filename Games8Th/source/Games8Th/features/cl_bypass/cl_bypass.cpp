#define NOMINMAX
#include "cl_bypass.h"

#include "../../hooks/includeHooks.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/memsafe/memsafe.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// move_crc @ CBaseUserCmdPB+0x30 is ArenaStringPtr (not raw std::string*):
// empty -> points at client rdata sentinel (tag bits 0)
// set -> heap MSVC std::string* with low 2 tag bits ( |2 / |3 )
// Calling std::string::assign on the sentinel -> ACCESS_VIOLATION (log spam).
// Write path: mask tags + SSO rewrite when allocated; else game ArenaString
// assign helper (sub_181196690) with stack MSVC string. Only nCachedBits bit.

namespace CL_Bypass {
namespace {

	constexpr std::uint64_t kJump = IN_JUMP;
	constexpr std::uint64_t kAttack = IN_ATTACK;
	constexpr std::uintptr_t kArenaStrTagMask = 3ull;

	std::atomic<bool> g_inOriginalCreateMove{ false };
	bool g_crcHookActive = false;
	bool g_crcEnabled = true;

	// IDA client sub_181196690 - ArenaStringPtr assign from MSVC std::string
	using ArenaStrAssignFn = void* (__fastcall*)(void* destArenaStr, void* srcMsvcString, void* arena);
	ArenaStrAssignFn g_arenaStrAssign = nullptr;
	bool g_arenaStrAssignResolved = false;

	struct InternalSubTick {
		std::uint64_t button = 0;
		bool pressed = false;
		float when = 0.f;
	};
	std::vector<InternalSubTick> g_subticks;

	// MSVC x64 std::string layout (SSO cap 15) - matches game CRT string ops
	struct MsvcString {
		union {
			char sso[16];
			char* heap;
		};
		std::size_t size = 0;
		std::size_t cap = 15; // SSO
	};

	void ResolveArenaStrAssign()
	{
		if (g_arenaStrAssignResolved)
			return;
		g_arenaStrAssignResolved = true;
		// IDA 0x181196690
		constexpr const char* kPat = "48 89 5C 24 ? 55 56 57 48 83 EC 30 49 8B C0";
		const uintptr_t addr = M::patternScan("client", kPat);
		if (addr)
			g_arenaStrAssign = reinterpret_cast<ArenaStrAssignFn>(addr);
	}

	// IEEE CRC-32 (poly 0xEDB88320) - matches common CS2 move_crc digests
	std::uint32_t Crc32Update(std::uint32_t crc, const void* data, std::size_t len)
	{
		const auto* p = static_cast<const std::uint8_t*>(data);
		crc = ~crc;
		for (std::size_t i = 0; i < len; ++i) {
			crc ^= p[i];
			for (int b = 0; b < 8; ++b)
				crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
		return ~crc;
	}

	void CrcAdd(std::uint32_t& crc, const void* p, std::size_t n)
	{
		if (p && n)
			crc = Crc32Update(crc, p, n);
	}

	template <typename T>
	void CrcPod(std::uint32_t& crc, const T& v)
	{
		CrcAdd(crc, &v, sizeof(T));
	}

	bool LooksLikeBaseCmd(void* msg)
	{
		if (!msg || !Mem::ValidEntity(msg))
			return false;
		// CBaseUserCmdPB : CBasePB - vtable + has/cached bits + subtick field @ +0x18
		// strMoveCrc @ +0x30, pInButtonState @ +0x38
		const auto base = reinterpret_cast<std::uintptr_t>(msg);
		__try {
			const auto cached = *reinterpret_cast<std::uint64_t*>(base + 0x10);
			// Cached bits for a real base cmd usually has some of the known field bits
			(void)cached;
			void* buttons = *reinterpret_cast<void**>(base + 0x38);
			// buttons optional early; require user-range if set
			if (buttons && !Mem::IsUserPtr(buttons))
				return false;
			// nClientTick / moves look sane when present
			const int tick = *reinterpret_cast<int*>(base + 0x54);
			if (tick < -1 || tick > 0x7FFFFFFF)
				return false;
			const float fwd = *reinterpret_cast<float*>(base + 0x58);
			const float side = *reinterpret_cast<float*>(base + 0x5C);
			if (!std::isfinite(fwd) || !std::isfinite(side))
				return false;
			if (std::fabs(fwd) > 10.f || std::fabs(side) > 10.f)
				return false;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Rewrite bytes into already-allocated MSVC std::string (tag bits stripped).
	bool WriteIntoExistingMsvcString(void* strObj, const char* hex8, std::size_t len)
	{
		if (!strObj || !hex8 || len == 0 || len > 15)
			return false;
		if (!Mem::IsUserPtr(strObj) || !Mem::IsReadable(strObj, 32))
			return false;

		auto* raw = static_cast<std::uint8_t*>(strObj);
		const std::size_t cap = *reinterpret_cast<std::size_t*>(raw + 24);
		// SSO strings use cap 15; heap uses cap >= 16. Reject garbage.
		if (cap < len || cap > 0x1000)
			return false;

		char* buf = nullptr;
		if (cap < 16)
			buf = reinterpret_cast<char*>(raw); // SSO inline
		else {
			buf = *reinterpret_cast<char**>(raw);
			if (!buf || !Mem::IsUserPtr(buf) || !Mem::IsReadable(buf, len + 1))
				return false;
		}
		std::memcpy(buf, hex8, len);
		buf[len] = '\0';
		*reinterpret_cast<std::size_t*>(raw + 16) = len;
		return true;
	}

	// Write CRC hex into ArenaStringPtr strMoveCrc (+0x30).
	bool WriteCrcString(CBaseUserCmdPB* base, const char* hex8)
	{
		if (!base || !hex8)
			return false;

		const std::size_t len = std::strlen(hex8);
		if (len == 0 || len > 15)
			return false;

		__try {
			auto* taggedSlot = reinterpret_cast<std::uintptr_t*>(&base->strMoveCrc);
			const std::uintptr_t tagged = *taggedSlot;
			const std::uintptr_t tag = tagged & kArenaStrTagMask;
			void* strObj = reinterpret_cast<void*>(tagged & ~kArenaStrTagMask);

			// Already allocated (tag 2/3): rewrite in place - no CRT assign.
			if (tag != 0 && strObj && Mem::IsUserPtr(strObj)) {
				if (WriteIntoExistingMsvcString(strObj, hex8, len)) {
					base->SetBits(BASE_BITS_MOVE_CRC);
					return true;
				}
			}

			// Empty sentinel (tag 0) or bad existing: use game ArenaString assign.
			ResolveArenaStrAssign();
			if (g_arenaStrAssign) {
				MsvcString src{};
				std::memcpy(src.sso, hex8, len);
				src.sso[len] = '\0';
				src.size = len;
				src.cap = 15;

				void* arena = base->subtickMovesField.pArena;
				// nullptr arena -> heap path inside helper (copy ctor does this)
				g_arenaStrAssign(&base->strMoveCrc, &src, arena);
				base->SetBits(BASE_BITS_MOVE_CRC);
				return true;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			// Once per session - was spamming every CreateMove
			Con::SehOnce("CL_Bypass.WriteCrcString", GetExceptionCode());
		}

		// Cannot write - drop presence so serialize omits stale/wrong CRC.
		// Do NOT touch nHasBits @+8: that word packs arena flags, not field bits.
		__try {
			base->nCachedBits &= ~static_cast<std::uint64_t>(BASE_BITS_MOVE_CRC);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return false;
	}

	std::uint32_t HashBaseCmd(CBaseUserCmdPB* base)
	{
		std::uint32_t crc = 0;
		if (!base)
			return crc;

		__try {
			CrcPod(crc, base->nLegacyCommandNumber);
			CrcPod(crc, base->nClientTick);
			CrcPod(crc, base->flForwardMove);
			CrcPod(crc, base->flSideMove);
			CrcPod(crc, base->flUpMove);
			CrcPod(crc, base->nImpulse);
			CrcPod(crc, base->nWeaponSelect);
			CrcPod(crc, base->nRandomSeed);
			CrcPod(crc, base->nMousedX);
			CrcPod(crc, base->nMousedY);
			CrcPod(crc, base->m_uConsumedServerAngleChanges);
			CrcPod(crc, base->m_nCmdFlags);
			CrcPod(crc, base->m_uPredictionOffsetTicksx256);
			CrcPod(crc, base->m_uPawnEntityHandle);

			if (base->pInButtonState && Mem::IsUserPtr(base->pInButtonState)) {
				CrcPod(crc, base->pInButtonState->nValue);
				CrcPod(crc, base->pInButtonState->nValueChanged);
				CrcPod(crc, base->pInButtonState->nValueScroll);
			}
			if (base->pViewAngles && Mem::IsUserPtr(base->pViewAngles)) {
				CrcPod(crc, base->pViewAngles->angValue.x);
				CrcPod(crc, base->pViewAngles->angValue.y);
				CrcPod(crc, base->pViewAngles->angValue.z);
			}

			auto& field = base->subtickMovesField;
			if (field.pRep && field.nCurrentSize > 0 && field.nCurrentSize <= 64) {
				const int cap = field.pRep->nAllocatedSize;
				if (cap > 0 && cap <= 128) {
					const int n = (field.nCurrentSize < cap) ? field.nCurrentSize : cap;
					for (int i = 0; i < n; ++i) {
						CSubtickMoveStep* step = field.pRep->tElements[i];
						if (!step || !Mem::IsUserPtr(step))
							continue;
						CrcPod(crc, step->nButton);
						CrcPod(crc, step->bPressed);
						CrcPod(crc, step->flWhen);
						CrcPod(crc, step->flAnalogForwardDelta);
						CrcPod(crc, step->flAnalogLeftDelta);
					}
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("CL_Bypass.HashBaseCmd", GetExceptionCode());
		}
		return crc;
	}

	void SehSyncButtonsToPb(CUserCmd* cmd)
	{
		if (!cmd)
			return;
		__try {
			auto* base = cmd->csgoUserCmd.pBaseCmd;
			if (!base || !base->pInButtonState)
				return;
			base->pInButtonState->nValue = cmd->nButtons.nValue;
			base->pInButtonState->nValueChanged = cmd->nButtons.nValueChanged;
			base->pInButtonState->nValueScroll = cmd->nButtons.nValueScroll;
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			base->SetBits(BASE_BITS_BUTTONPB);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("CL_Bypass.SyncButtons", GetExceptionCode());
		}
	}

	void SehAddOneSubtick(CUserCmd* cmd, std::uint64_t button, bool pressed, float when)
	{
		if (!cmd)
			return;
		__try {
			CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
			if (!base)
				return;
			CSubtickMoveStep* step = base->add_subtick_move();
			if (!step)
				return;
			step->nHasBits = 0;
			step->nCachedBits = 0;
			step->nButton = button;
			step->bPressed = pressed;
			step->flWhen = when;
			step->flAnalogForwardDelta = 0.f;
			step->flAnalogLeftDelta = 0.f;
			step->SetBits(MOVESTEP_BITS_BUTTON | MOVESTEP_BITS_PRESSED | MOVESTEP_BITS_WHEN);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("CL_Bypass.AddSubtick", GetExceptionCode());
		}
	}

	void SyncButtonsToPb(CUserCmd* cmd)
	{
		SehSyncButtonsToPb(cmd);
	}

	// SafetyHook object for SerializePartialToArray
	CInlineHookObj<bool(__fastcall*)(void*, void*, int)> g_serializeHook{};

} // namespace

void SetInOriginalCreateMove(bool v)
{
	g_inOriginalCreateMove.store(v, std::memory_order_relaxed);
}

bool InOriginalCreateMove()
{
	return g_inOriginalCreateMove.load(std::memory_order_relaxed);
}

bool CrcHookActive()
{
	return g_crcHookActive;
}

void RecomputeMoveCrc(CBaseUserCmdPB* base)
{
	if (!g_crcEnabled || !base || !LooksLikeBaseCmd(base))
		return;

	const std::uint32_t h = HashBaseCmd(base);
	char hex[16]{};
	_snprintf_s(hex, sizeof(hex), _TRUNCATE, "%08x", h);
	if (WriteCrcString(base, hex))
		Con::Rate("cl_crc_ok", 5000, "move_crc=%s", hex);
}

void OnCBaseUserCmdPB(void* /*pMsg*/)
{
	// CRC recompute DISABLED.
	// Real move_crc = hash over protobuf-encoded bytes, not raw C fields.
	// Our IEEE CRC-32 over raw fields never matched server -> soft flag per
	// cmd -> untrust after ~1 game. Same rationale as CL_Bypass:
	// "CRC rewrite DISABLED (serialize fatals without real protobuf Message backup)."
	// If server enforces CRC, it never validated after our first mod anyway;
	// leaving the original engine-computed CRC in place lets the cmd land
	// with the engine's own hash of the fields it saw before our mutations.
}

void PreClientCreateMove(CUserCmd* /*cmd*/)
{
	g_subticks.clear();
}

void PostClientCreateMove(void* /*pCSGOInput*/, CUserCmd* cmd)
{
	if (!cmd)
		return;

	const size_t n = g_subticks.size();
	for (size_t i = 0; i < n; ++i)
		SehAddOneSubtick(cmd, g_subticks[i].button, g_subticks[i].pressed, g_subticks[i].when);
	g_subticks.clear();

	SyncButtonsToPb(cmd);

	// never recompute move_crc (see OnCBaseUserCmdPB comment).
}

void SetAttack(CUserCmd* cmd, bool addSubtick)
{
	if (!cmd)
		return;
	cmd->nButtons.nValue |= kAttack;
	cmd->nButtons.nValueChanged |= kAttack;
	SyncButtonsToPb(cmd);
	cmd->csgoUserCmd.SetAttack1StartHistoryIndex(0);
	if (addSubtick)
		AddProcessSubTick(kAttack, true);
}

void SetDontAttack(CUserCmd* cmd, bool addSubtick)
{
	if (!cmd)
		return;
	cmd->nButtons.nValue &= ~kAttack;
	cmd->nButtons.nValueScroll &= ~kAttack;
	SyncButtonsToPb(cmd);
	if (addSubtick)
		AddProcessSubTick(kAttack, false);
}

void SetJump(CUserCmd* cmd, bool addSubtick)
{
	if (!cmd)
		return;
	cmd->nButtons.nValue |= kJump;
	cmd->nButtons.nValueChanged |= kJump;
	cmd->nButtons.nValueScroll |= kJump;
	SyncButtonsToPb(cmd);
	if (addSubtick)
		AddProcessSubTick(kJump, true, 0.01f);
}

void SetDontJump(CUserCmd* cmd, bool addSubtick)
{
	if (!cmd)
		return;
	cmd->nButtons.nValue &= ~kJump;
	cmd->nButtons.nValueScroll &= ~kJump;
	SyncButtonsToPb(cmd);
	if (addSubtick)
		AddProcessSubTick(kJump, false, 0.01f);
}

void AddProcessSubTick(std::uint64_t button, bool pressed)
{
	if (g_subticks.capacity() < 16)
		g_subticks.reserve(16);
	g_subticks.push_back({ button, pressed, 0.99f });
}

void AddProcessSubTick(std::uint64_t button, bool pressed, float when)
{
	if (g_subticks.capacity() < 16)
		g_subticks.reserve(16);
	g_subticks.push_back({ button, pressed, when });
}

bool __fastcall hkSerializePartialToArray(void* msg, void* out, int size)
{
	// Kept for symbol stability; never installed anymore.
	auto original = g_serializeHook.GetOriginal();
	if (!original)
		return false;
	bool ok = false;
	__try {
		ok = original(msg, out, size);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("CL_Bypass.SerializePartial", GetExceptionCode());
		return false;
	}
	return ok;
}

bool Init()
{
	// surface: no SerializePartialToArray hook, no CRC recompute.
	// Rationale (from CL_Bypass source):
	// "CRC rewrite DISABLED (serialize fatals without real protobuf Message backup)."
	// Our old path re-hashed CBaseUserCmdPB via IEEE CRC-32 over raw C fields,
	// but the real move_crc is computed over protobuf-encoded bytes with a
	// different digest -> every post-mutation write shipped a wrong hash -> server
	// soft-flagged the client, accumulating to untrust after ~1 game.
	// Env kill-switch retained for symmetry (already-off default).
	char env[8]{};
	if (GetEnvironmentVariableA("GAMES8TH_CRC", env, sizeof(env)) > 0
		&& env[0] == '1') {
		Con::Warn("CL_Bypass: GAMES8TH_CRC=1 requested - IGNORED (unsafe; see notes)");
	}
	g_crcEnabled = false;
	g_crcHookActive = false;
	ResolveArenaStrAssign();
	Con::Ok("CL_Bypass: CRC recompute OFF");
	return true;
}

} // namespace CL_Bypass

