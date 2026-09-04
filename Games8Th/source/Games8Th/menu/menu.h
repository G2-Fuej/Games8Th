#pragma once
#include <Windows.h>
#include <d3d11.h>
#include "../../../external/imgui/imgui.h"
#include "../../../external/imgui/imgui_impl_dx11.h"
#include "../../../external/imgui/imgui_impl_win32.h"
// CS2 weapon icon font (private-use glyphs)
extern ImFont* g_WeaponIconFont;
// Font Awesome solid - menu sidebar tab icons
extern ImFont* g_MenuIconFont;
void ApplyImGuiTheme();
// Call before ImGui::NewFrame. Rebuilds atlas if font size changed.
void MenuTryRebuildFonts();
class Menu {public:
Menu();
	void init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView);
	void render();
	void shutdown() noexcept;
	void toggleMenu();
	bool isOpen() const { return showMenu;
 }
private:
bool showMenu = false;
	int activeTab = 0;
}
;

