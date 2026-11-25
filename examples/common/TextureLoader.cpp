#include "TextureLoader.h"
#include "root_directory.h"
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <iostream>

// STB_IMAGE_IMPLEMENTATION is compiled once in GltfModelLoader.cpp (which also
// vendors tinygltf); this TU only needs the declarations.
#include "stb_image.h"

LoadedTexture loadTexture(const std::string &path)
{
    std::string resolved = path;
    if (!std::filesystem::exists(resolved)) {
        std::string candidate = std::string(PROJECT_ROOT_PATH) + "/" + path;
        if (std::filesystem::exists(candidate)) {
            resolved = candidate;
        }
    }
    if (!std::filesystem::exists(resolved)) {
        throw std::runtime_error("texture not found: " + path);
    }

    int width = 0, height = 0, channels = 0;
    stbi_uc *pixels = stbi_load(resolved.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        throw std::runtime_error("failed to load texture: " + path);
    }

    LoadedTexture result;
    result.width = width;
    result.height = height;
    result.channels = 4;
    result.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    result.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return result;
}
