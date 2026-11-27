#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct LoadedTexture {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int channels = 4;
    uint32_t mipLevels = 1;
};

// Decodes with STB_rgb_alpha and computes the mip chain length.  The
// STB_IMAGE_IMPLEMENTATION is compiled exactly once in
// extensions/model/GltfModelLoader.cpp (the single tinygltf/stb TU of the
// frameworkExtensions library).
LoadedTexture loadTexture(const std::string &path);
