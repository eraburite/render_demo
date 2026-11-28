#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

// Minimal KTX 1.0 reader for the asset pack that ships with the reference
// examples.  Every texture in assets/textures is uncompressed (RGBA8 or R16), so
// the loader only accepts those and fails loudly on anything else instead of
// silently producing garbage.
//
// Layout of `data` matches TextureImageSpec::mipsInData: packed as
// (mip level -> array layer -> cube face), tightly packed rows, so the result
// can be handed straight to TextureImage::updateRawData().
struct KtxTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t layers = 1;    // array elements (a cube counts as 1)
    uint32_t faces = 1;     // 6 for cube / cube array
    uint32_t mipLevels = 1;
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    std::vector<uint8_t> data;

    bool valid() const { return width > 0 && !data.empty(); }
    size_t byteSize() const { return data.size(); }

    // Description for the descriptor layout / auto-created texture resource.
    TextureImageSpec imageSpec() const;
    // Copy with the base-colour formats switched to their sRGB counterpart.
    KtxTexture withSrgb(bool srgb) const;
};

// `preferSrgb` promotes RGBA8 to R8G8B8A8_SRGB (what colour maps want).
KtxTexture loadKtx(const std::string &path, bool preferSrgb = false);
