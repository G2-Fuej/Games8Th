#pragma once

#include <cstdint>
#include <optional>
#include <string>

class C_EconItemView {};
class C_CSWeaponBase;
class C_CSPlayerPawn;
class CGameSceneNode;
class C_BaseEntity;

template <typename K, typename V>
struct SkinUtlMap
{
	struct Node_t {
		int m_left;
		int m_right;
		int m_parent;
		int m_tag;
		K m_key;
		V m_value;
	};
	int m_size;
	int m_unknown;
	Node_t* m_data;
	int m_root;
};

class CEconItemDefinition
{
public:
	uint16_t DefIndex() const;
	uint8_t Rarity() const;
	const char* ItemBaseName() const;
	const char* ItemTypeName() const;
	const char* ModelName() const;
	int32_t StickerSupportCount() const;
	const char* IconName() const;
	const char* WeaponName() const;

	bool IsWeapon() const;
	bool IsKnife(bool excludeDefault) const;
	bool IsGlove(bool excludeDefault) const;
	bool IsAgent(bool excludeDefault) const;
};

class CPaintKit
{
public:
	int nID;
	const char* sName;
	const char* sDescriptionString;
	const char* sDescriptionTag;
	char pad0[0x8];
	char pad1[0x8];
	char pad2[0x8];
	char pad3[0x8];
	char pad4[0x4];
	int nRarity;
	uint8_t IsUseLegacyModel() const;
};

class CEconItemSchema
{
public:
	SkinUtlMap<int, CEconItemDefinition*>& SortedItemDefinitionMap();
	SkinUtlMap<int, CPaintKit*>& PaintKits();
};

class CEconItemSystem
{
public:
	CEconItemSchema* Schema();
};

struct SkinInventoryOwner
{
	uint64_t id = 0;
	uint32_t type = 0;
	uint32_t pad = 0;
	uint64_t ID() const { return id; }
};

namespace SkinSdk
{
	void Init();

	void* Source2Client();
	CEconItemSystem* EconItemSystem();
	CEconItemSchema* EconSchema();
	void* LocalInventory();
	uint64_t InventorySteamId();

	void* FileSystem();
	bool FileExistsGame(const char* path);
	const char* LocalizeSafe(const char* token, const char* fallback);

	CEconItemDefinition* FindDefByIndex(uint16_t defIdx);
	CEconItemDefinition* GetStaticData(C_EconItemView* view);
	CPaintKit* FindPaintKit(int paintId);

	void SetAttributeValueByName(C_EconItemView* view, const char* name, float value);
    bool SetModel(C_BaseEntity* ent, const char* model);
	void SetMeshGroupMask(CGameSceneNode* node, uint64_t mask);
    // Refresh the networkable entity after changing its model/item view.
    void PostDataUpdate(C_BaseEntity* entity, int updateType = 1);
	void UpdateSubclass(C_CSWeaponBase* weapon);
	// CS2 subclass ids are murmur2(def_index as decimal string, 0x31415926) -
	// NOT string-tokens of weapon names. Verified against the working changer
	// on the current build.
	std::uint32_t MakeSubclassToken(std::uint16_t defIndex);
	// Game-side model resolver: returns the canonical model path for whatever
	// def index is currently written in the item view (nullptr on miss).
	const char* WeaponModelPath(C_EconItemView* view);
	// Post-subclass-change viewmodel/attachment refresh (game fn, takes weapon).
	void UpdateWeaponViewModel(C_CSWeaponBase* weapon);
	void UpdateSkin(C_CSWeaponBase* weapon);
	void UpdateCompositeMaterial(void* compositeOwner);
	void UpdateCompositeMaterialSet(C_CSWeaponBase* weapon);
	void SetBodyGroup(C_CSPlayerPawn* pawn);
	void UpdateBodyGroupChoice(C_CSPlayerPawn* pawn);
	void RegenerateWeaponSkin(C_CSWeaponBase* weapon, bool force = true);

	void* FindHudElement(const char* name);
	int ClearHudWeaponIcon(void* hudWeapons, int slot, int64_t unk);
	void UpdateWeaponRows(void* hudWeapons);

	void* CompositeOwner(C_CSWeaponBase* weapon);

	C_BaseEntity* GetViewModel(C_CSPlayerPawn* pawn);
	C_BaseEntity* GetKnifeModel(C_CSPlayerPawn* pawn);

	const char* KnifeIconName(int defIdx);
	const char* KnifeWeaponName(int defIdx);
	bool IsSkinnableGloveDef(int defIdx);

	constexpr uint16_t kDefaultCtKnife = 42;
	constexpr uint16_t kDefaultTKnife = 59;
	constexpr int kGloveBloodhound = 5027;
	constexpr int kGloveBrokenFang = 4725;
	constexpr int kGloveSporty = 5030;
	constexpr int kGloveSlick = 5031;
	constexpr int kGloveHandwraps = 5032;
	constexpr int kGloveMotorcycle = 5033;
	constexpr int kGloveSpecialist = 5034;
	constexpr int kGloveHydra = 5035;
	constexpr int kGloveDefault = 5028;
}

