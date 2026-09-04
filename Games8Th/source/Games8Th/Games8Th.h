#pragma once
#include "config/config.h"

#include "hooks/hooks.h"
#include "renderer/renderer.h"
#include "utils/schema/schema.h"
#include "interfaces/interfaces.h"
class Games8Th {
public:
	// Returns true only when schema+interfaces+hooks are fully ready. Partial failure rolls back to avoid half-wired features.
	bool init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView);
	void shutdown() noexcept;

	Schema schema;
	Renderer renderer;


	H::Hooks hooks;
	I::Interfaces interfaces;

};

