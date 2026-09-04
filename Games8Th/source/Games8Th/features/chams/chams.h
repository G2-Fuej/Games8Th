#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_Material/C_Material.h"
#include "../../../../external/imgui/imgui.h"
#include "../../utils/math/utlstronghandle/utlstronghandle.h"

// mercey chams system - ported 1:1 (backtrack + onshot chams skipped).
// Draw happens through GeneratePrimitives: original fills the primitive
// buffer, then the appended primitives get their material/colour swapped.

namespace chams
{
	// mercey settings::esp::cham_ids
	enum class ChamIds : std::uint8_t
	{
		liquid, metallic, matte, flat, bloom, outlines, glow, electric, distortion, hologram, pearl,
		liquid_ignorez, matte_ignorez, flat_ignorez, bloom_ignorez, outlines_ignorez, glow_ignorez, distortion_ignorez, hologram_ignorez,
		count
	};

	namespace Materials
	{
		bool init();
		[[nodiscard]] bool ready() noexcept;
		[[nodiscard]] CMaterial2* find(ChamIds id);
		[[nodiscard]] CMaterial2* load(const char* vmatData, const char* name);
		void set_material_vec3(CMaterial2* mat, const char* paramName, float x, float y, float z);
	}

	// mercey features::esp::detail::primitive_buffer (0x70 stride)
	namespace prim
	{
		inline constexpr std::size_t kStride = 0x70;
		inline constexpr std::size_t kSceneObjOff = 0x18;
		inline constexpr std::size_t kMatOff = 0x20;
		inline constexpr std::size_t kMatCopyOff = 0x28;
		inline constexpr std::size_t kColorOff = 0x50;

		struct mesh_primitive
		{
			std::byte pad_00[0x18]{};
			std::uintptr_t scene_object{};
			std::uintptr_t material{};
			std::uintptr_t material_copy{};
			std::byte pad_30[0x20]{};
			std::uint32_t color{};
			float opacity{};
			std::int32_t draw_order{};
			std::byte pad_5c[0x6]{};
			std::uint16_t flags{};
			std::uint16_t flags_2{};
			std::byte pad_66[0xA]{};
		};
		static_assert(sizeof(mesh_primitive) == kStride);
		static_assert(offsetof(mesh_primitive, material) == kMatOff);
		static_assert(offsetof(mesh_primitive, material_copy) == kMatCopyOff);
		static_assert(offsetof(mesh_primitive, color) == kColorOff);

		// Fixed array + separate overflow array - never treat as one allocation.
		struct output_buffer
		{
			std::uintptr_t fixed_data{};
			std::int32_t fixed_capacity{};
			std::int32_t fixed_count{};
			std::int32_t overflow_count{};
			std::uint32_t reserved_14{};
			std::uintptr_t overflow_data{};
			std::int32_t overflow_capacity{};
			std::uint32_t overflow_allocation_flags{};

			[[nodiscard]] int count() const noexcept;
			[[nodiscard]] std::uintptr_t at(int index) const noexcept;
		};
		static_assert(sizeof(output_buffer) == 0x28);
		static_assert(offsetof(output_buffer, fixed_count) == 0xC);
		static_assert(offsetof(output_buffer, overflow_count) == 0x10);
		static_assert(offsetof(output_buffer, overflow_data) == 0x18);

		[[nodiscard]] bool read_buffer(void* addr, output_buffer& out);
		void replace_primitive(void* primitive, CMaterial2* mat, std::uint32_t packedColor);
	}

	struct ChamsLayer
	{
		bool enabled = false;
		ImVec4 color = ImVec4(1.f, 1.f, 1.f, 1.f);
		int material = 0;
	};

	struct ChamsConfig
	{
		bool enabled = false;
		ChamsLayer primary{};
		ChamsLayer secondary{};
		ChamsLayer overlay{};
	};

	class PlayerChams
	{
	public:
		using OriginalFn = void(__fastcall*)(void*, void*, void*, void*);

		bool on_generate_primitives(C_BaseEntity* ownerEntity, std::uint32_t ownerHash,
			void* sceneObject, void* primitiveBuffer, OriginalFn original, void* a1, void* sceneView);
		void on_sort_primitives(void* entries, std::uint32_t count);

	private:
		void apply_layer(void* primitiveBuffer, OriginalFn original, void* a1,
			void* sceneObject, void* sceneView, const ImVec4& color, int materialId);
		void apply_overlay(void* primitiveBuffer, OriginalFn original, void* a1,
			void* sceneObject, void* sceneView, const ImVec4& color, int materialId);

		[[nodiscard]] bool is_overlay_material(CMaterial2* mat) const;
		void add_overlay_material(CMaterial2* mat);

		static constexpr auto kMaxOverlayMaterials{ 16 };
		std::array<std::atomic<CMaterial2*>, kMaxOverlayMaterials> m_overlayMaterials{};
		std::atomic<int> m_overlayMaterialCount{ 0 };
		mutable std::mutex m_overlayMaterialsMutex{};
	};

	class ItemChams
	{
	public:
		using OriginalFn = void(__fastcall*)(void*, void*, void*, void*);

		bool on_generate_primitives(C_BaseEntity* ownerEntity, std::uint32_t ownerHash,
			void* sceneObject, void* primitiveBuffer, OriginalFn original, void* a1, void* sceneView);

	private:
		void apply_layer(void* primitiveBuffer, OriginalFn original, void* a1,
			void* sceneObject, void* sceneView, const ImVec4& color, int materialId);
		[[nodiscard]] std::uint32_t get_item_group(std::uint32_t schemaHash);
	};

	// mercey cheat::generate_primitives hook body: player chams, then item chams,
	// then fall through to the caller's original call.
	bool OnGeneratePrimitives(void* a1, void* sceneObj, void* sceneView, void* drawList,
		std::int64_t(__fastcall* original)(void*, void*, void*, void*),
		std::int64_t* outRet);

	// mercey cheat::sort_primitives hook body.
	void OnSortPrimitives(void* entries, std::uint32_t count);
}

