#include "KtxLoader.h"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace {

constexpr std::array<uint8_t, 12> kKtxIdentifier = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31,
                                                    0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

// glInternalFormat values that appear in the asset pack.
constexpr uint32_t kGlRgba8 = 0x8058;
constexpr uint32_t kGlSrgb8Alpha8 = 0x8C43;
constexpr uint32_t kGlR16 = 0x8234;

uint32_t readU32(const std::vector<uint8_t> &bytes, size_t offset)
{
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("KTX: truncated header");
    }
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

uint32_t bytesPerTexelOf(vk::Format format)
{
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        return 4;
    case vk::Format::eR16Unorm:
        return 2;
    default:
        return 0;
    }
}

} // namespace

TextureImageSpec KtxTexture::imageSpec() const
{
    TextureImageSpec spec;
    spec.width = width;
    spec.height = height;
    spec.depth = depth;
    spec.layers = layers;
    spec.faces = faces;
    spec.miplevels = mipLevels;
    spec.channels = bytesPerTexelOf(format);
    spec.format = format;
    spec.mipsInData = true;
    // Tiling textures (terrain, floors) set eRepeat explicitly; clamp is the safe
    // default because this project's meshes map UVs onto [0, 1] per face.
    spec.addressMode = vk::SamplerAddressMode::eClampToEdge;
    spec.anisotropy = true;
    return spec;
}

KtxTexture KtxTexture::withSrgb(bool srgb) const
{
    KtxTexture copy = *this;
    if (srgb) {
        if (copy.format == vk::Format::eR8G8B8A8Unorm) {
            copy.format = vk::Format::eR8G8B8A8Srgb;
        }
    } else if (copy.format == vk::Format::eR8G8B8A8Srgb) {
        copy.format = vk::Format::eR8G8B8A8Unorm;
    }
    return copy;
}

KtxTexture loadKtx(const std::string &path, bool preferSrgb)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("KTX: cannot open '" + path + "'");
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 64) {
        throw std::runtime_error("KTX: '" + path + "' is shorter than the 64 byte header");
    }
    if (!std::equal(kKtxIdentifier.begin(), kKtxIdentifier.end(), bytes.begin())) {
        throw std::runtime_error("KTX: '" + path + "' does not carry the KTX 1.0 identifier");
    }

    const uint32_t endianness = readU32(bytes, 12);
    if (endianness != 0x04030201u) {
        throw std::runtime_error("KTX: '" + path + "' is big endian, which this loader does not handle");
    }
    const uint32_t glType = readU32(bytes, 16);
    const uint32_t glTypeSize = readU32(bytes, 20);
    const uint32_t glFormat = readU32(bytes, 24);
    const uint32_t glInternalFormat = readU32(bytes, 28);
    const uint32_t pixelWidth = readU32(bytes, 36);
    const uint32_t pixelHeight = readU32(bytes, 40);
    const uint32_t pixelDepth = readU32(bytes, 44);
    const uint32_t arrayElements = readU32(bytes, 48);
    const uint32_t numberOfFaces = readU32(bytes, 52);
    const uint32_t numberOfMipmaps = readU32(bytes, 56);
    const uint32_t keyValueBytes = readU32(bytes, 60);

    KtxTexture texture;
    texture.width = pixelWidth;
    texture.height = pixelHeight == 0 ? 1u : pixelHeight;
    texture.depth = pixelDepth == 0 ? 1u : pixelDepth;
    texture.layers = arrayElements == 0 ? 1u : arrayElements;
    texture.faces = numberOfFaces == 0 ? 1u : numberOfFaces;
    texture.mipLevels = numberOfMipmaps == 0 ? 1u : numberOfMipmaps;

    switch (glInternalFormat) {
    case kGlRgba8:
        if (glType != 0x1401 /* GL_UNSIGNED_BYTE */ || glTypeSize != 1 || glFormat != 0x1908 /* GL_RGBA */) {
            throw std::runtime_error("KTX: '" + path + "' declares RGBA8 but with an unexpected type/format");
        }
        texture.format = preferSrgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
        break;
    case kGlSrgb8Alpha8:
        if (glType != 0x1401 || glTypeSize != 1 || glFormat != 0x1908) {
            throw std::runtime_error("KTX: '" + path + "' declares SRGB8_ALPHA8 but with an unexpected type/format");
        }
        texture.format = vk::Format::eR8G8B8A8Srgb;
        break;
    case kGlR16:
        if (glType != 0x1403 /* GL_UNSIGNED_SHORT */ || glTypeSize != 2 || glFormat != 0x8D94 /* GL_RED */) {
            throw std::runtime_error("KTX: '" + path + "' declares R16 but with an unexpected type/format");
        }
        texture.format = vk::Format::eR16Unorm;
        break;
    default:
        throw std::runtime_error("KTX: '" + path + "' uses glInternalFormat 0x" +
                                 std::to_string(glInternalFormat) +
                                 ", only uncompressed RGBA8 / SRGB8_ALPHA8 / R16 are supported");
    }

    const uint32_t bytesPerTexel = bytesPerTexelOf(texture.format);
    size_t offset = 64 + keyValueBytes;

    for (uint32_t mip = 0; mip < texture.mipLevels; ++mip) {
        const uint32_t imageSize = readU32(bytes, offset);
        offset += 4;
        if (offset + imageSize > bytes.size()) {
            throw std::runtime_error("KTX: '" + path + "' is truncated at mip " + std::to_string(mip));
        }

        // KTX 1.0 stores one imageSize per mip; for a plain cubemap that is the
        // size of a single face, in every other case it already covers all array
        // elements and faces.
        if (texture.faces == 6 && texture.layers == 1) {
            for (uint32_t face = 0; face < 6; ++face) {
                texture.data.insert(texture.data.end(), bytes.begin() + offset, bytes.begin() + offset + imageSize);
                offset += imageSize;
                // Faces are padded to a 4 byte boundary.
                const uint32_t padding = (4 - (imageSize % 4)) % 4;
                offset += padding;
            }
        } else {
            texture.data.insert(texture.data.end(), bytes.begin() + offset, bytes.begin() + offset + imageSize);
            offset += imageSize;
        }

        // Rows must be tightly packed for the single-copy upload path; KTX pads
        // each row to 4 bytes, which only differs for odd widths with 16-bit
        // texels (none of the shipped assets).
        const uint32_t mipWidth = texture.width >> mip ? texture.width >> mip : 1u;
        const uint32_t rowBytes = mipWidth * bytesPerTexel;
        if (rowBytes % 4 != 0 && texture.depth == 1) {
            throw std::runtime_error("KTX: '" + path + "' has row padding at mip " + std::to_string(mip) +
                                     " (width " + std::to_string(mipWidth) + "), which would need per-row unpadding");
        }
    }

    if (offset > bytes.size()) {
        throw std::runtime_error("KTX: '" + path + "' overran its mip chain");
    }
    return texture;
}
