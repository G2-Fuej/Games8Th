#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

// IDA FlashOverlay @ 0x18113C960 - builds "FlashbangOverlay" material.
// 100% = skip entirely. Partial = scale pawn flash alphas then call original.
void __fastcall H::
hkRenderFlashbangOverlay(void* a1, int split, void** matSys, void* a4, void* a5) {
	float amount = Config::
antiflash_amount;
	if (amount < 0.f) amount = 0.f;
	if (amount > 100.f) amount = 100.f;

	if (amount >= 99.5f)
		return;

	if (amount > 0.01f) {
		if (C_CSPlayerPawn* local = H::
SafeLocalPlayer()) {
			const float keep = 1.f - (amount * 0.01f);
			__try {
				float& overlay = local->m_flFlashOverlayAlpha();
				float& maxA = local->m_flFlashMaxAlpha();
				float& dur = local->m_flFlashDuration();
				// Scale ONCE per flash event, not per frame: this hook runs every
				// Present while the overlay renders, and m_flFlash* are only
				// re-networked on a new flash - multiplying per frame compounds
				// exponentially (50% setting hit ~99% removal in ~10 frames).
				// A fresh flash event = duration jumped back up above the last
				// seen value (the engine decays it monotonically otherwise).
				static float s_lastDur = 0.f;
				if (dur > s_lastDur + 0.001f) {
					overlay *= keep;
					maxA *= keep;
					dur *= keep;
				}
				s_lastDur = dur;
			} __except (EXCEPTION_EXECUTE_HANDLER) { TW_SEH_CATCH("antiflash.adjustOverlay"); }
		}
	}

	auto original = RenderFlashBangOverlay.GetOriginal();
	if (original) {
		__try { original(a1, split, matSys, a4, a5); }
		__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("FlashOverlay original", GetExceptionCode()); }
	}
}

