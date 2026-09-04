#pragma once

#include <string>
#include <d3d11.h>
#include "../../../../external/imgui/imgui.h"

class SkinPreview
{
public:
	void Shutdown();
	ImTextureID GetTexture(const std::string& iconPath);
	ImTextureID GetModelTexture(const char* simpleName);
	ImTextureID GetPaintTexture(const char* simpleName, const char* kitToken);
	static std::string AgentPath(const char* modelOrIcon);
};

SkinPreview& GetSkinPreview();

