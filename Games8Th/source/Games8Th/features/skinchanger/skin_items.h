#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class CPaintKit;

class SkinItems
{
public:
	enum Type {
		None = 0,
		Weapon,
		Knife,
		Glove,
		Agent,
	};

	struct Skin {
		int id = 0;
		int rarity = 0;
		std::string token;
		std::string name;
		bool legacy = false;
	};

	struct Item {
		uint16_t def = 0;
		int rarity = 0;
		std::vector<Skin> skins;
		Type type = None;
		int8_t team = 0;
		std::string simple;
		std::string name;
		std::string icon;
		bool skinsReady = false;
	};

	void Scan();
	bool EnsureSkins(uint16_t def);
	bool Ready() const { return modelsReady; }
	std::vector<Item>& Items() { return items; }

	// Thread-safe readers for the GAME thread (FSN skin walk). The menu
	// thread mutates item.skins / kitCache concurrently - raw Items()
	// iteration from the game thread is a heap UAF. These copy out under
	// an SRWLOCK shared with the menu-side writers.
	bool IsLegacySkin(uint16_t def, int paintId);
	// Returns false if no agent model found for team (2=T, 3=CT).
	bool FirstAgentModel(int team, char* out, size_t outN);

	static const char* SimpleName(uint16_t def, const char* schemaWeapon);
	Item* Find(uint16_t def);

private:
	bool EnsurePaintList();
	std::vector<Skin> BuildKits(const std::string& simple);
	void LoadDisk();
	void SaveDisk();
	void ApplyCache();
	std::string CachePath();

	std::vector<Item> items;
	std::vector<CPaintKit*> paint;
	std::unordered_map<std::string, bool> fsCache;
	std::unordered_map<std::string, std::vector<Skin>> kitCache;
	bool modelsReady = false;
	bool paintReady = false;
};

SkinItems& GetSkinItems();

