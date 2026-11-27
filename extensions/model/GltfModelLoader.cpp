// Single translation unit that compiles the tinygltf and stb_image
// implementations for the whole frameworkExtensions library
// (extensions/texture/TextureLoader.cpp only includes the stb header for
// declarations).
// stb_image v2.21 puts its implementation OUTSIDE the include guard, so the
// same TU must include stb_image.h exactly once.  tiny_gltf.h includes it
// internally; we rely on that single include for both the declarations used
// below and the one compiled implementation.
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "GltfModelLoader.h"

#include <tinygltf/tiny_gltf.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace {

// `Model` and `Primitive` are now the loader-neutral names in ModelAsset.h, so
// the tinygltf counterparts must be spelled out (or aliased) rather than
// pulled in with a using-declaration.
using TinyModel = tinygltf::Model;
using tinygltf::Accessor;
using tinygltf::BufferView;

// Number of scalar components per accessor type.
size_t componentCount(int type)
{
    switch (type) {
    case TINYGLTF_TYPE_SCALAR: return 1;
    case TINYGLTF_TYPE_VEC2: return 2;
    case TINYGLTF_TYPE_VEC3: return 3;
    case TINYGLTF_TYPE_VEC4: return 4;
    default: return 0;
    }
}

// Reads the whole attribute array into `out`, filling each vertex with the
// fallback where the attribute is absent.
void readAttributeArray(const TinyModel &model, int accessorIndex, glm::vec4 fallback, size_t count,
                        std::vector<glm::vec4> &out)
{
    out.assign(count, fallback);
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
        return;
    }
    const Accessor &accessor = model.accessors[accessorIndex];
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return;
    }
    const BufferView &view = model.bufferViews[accessor.bufferView];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size())) {
        return;
    }
    const std::vector<unsigned char> &bytes = model.buffers[view.buffer].data;
    const size_t components = componentCount(accessor.type);
    if (components == 0) return;
    const size_t elementSize = components * sizeof(float);
    const size_t stride = view.byteStride > 0 ? static_cast<size_t>(view.byteStride) : elementSize;
    const size_t base = static_cast<size_t>(accessor.byteOffset) + static_cast<size_t>(view.byteOffset);
    if (base >= bytes.size()) return;
    const unsigned char *ptr = bytes.data() + base;

    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        for (size_t i = 0; i < count; ++i) {
            const unsigned char *p = ptr + i * stride;
            if (p + elementSize > bytes.data() + bytes.size()) break;
            glm::vec4 value = fallback;
            std::memcpy(&value.x, p, sizeof(float));
            if (components > 1) std::memcpy(&value.y, p + sizeof(float), sizeof(float));
            if (components > 2) std::memcpy(&value.z, p + 2 * sizeof(float), sizeof(float));
            if (components > 3) std::memcpy(&value.w, p + 3 * sizeof(float), sizeof(float));
            out[i] = value;
        }
    } else {
        // Integer attributes (e.g. normalized COLOR_0 as uint8) are handled
        // component-wise with the correct component width and stride.
        const bool signedType = accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE
                                || accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT;
        const size_t compWidth = (accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT
                                  || accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) ? 2 : 1;
        const size_t intElementSize = components * compWidth;
        const size_t intStride = view.byteStride > 0 ? static_cast<size_t>(view.byteStride) : intElementSize;
        auto readComponent = [&](const unsigned char *p, size_t comp) -> float {
            const unsigned char *q = p + comp * compWidth;
            if (q + compWidth > bytes.data() + bytes.size()) return fallback[comp];
            double raw = 0.0;
            if (compWidth == 1) {
                raw = signedType ? static_cast<double>(static_cast<int8_t>(*q)) : static_cast<double>(*q);
            } else {
                int16_t v = 0;
                std::memcpy(&v, q, 2);
                raw = signedType ? static_cast<double>(v) : static_cast<double>(static_cast<uint16_t>(v));
            }
            if (!accessor.normalized) return static_cast<float>(raw);
            const double divisor = signedType ? (compWidth == 1 ? 127.0 : 32767.0)
                                              : (compWidth == 1 ? 255.0 : 65535.0);
            return static_cast<float>(raw / divisor);
        };
        for (size_t i = 0; i < count; ++i) {
            const unsigned char *p = ptr + i * intStride;
            if (p + intElementSize > bytes.data() + bytes.size()) break;
            glm::vec4 value = fallback;
            value.x = readComponent(p, 0);
            if (components > 1) value.y = readComponent(p, 1);
            if (components > 2) value.z = readComponent(p, 2);
            if (components > 3) value.w = readComponent(p, 3);
            out[i] = value;
        }
    }
}

Material convertMaterial(const tinygltf::Material &src, const std::vector<int> &textureToImage)
{
    Material dst;
    dst.name = src.name;
    const tinygltf::PbrMetallicRoughness &pbr = src.pbrMetallicRoughness;
    dst.baseColorFactor = glm::vec4(pbr.baseColorFactor[0], pbr.baseColorFactor[1],
                                   pbr.baseColorFactor[2], pbr.baseColorFactor[3]);
    dst.metallicFactor = static_cast<float>(pbr.metallicFactor);
    dst.roughnessFactor = static_cast<float>(pbr.roughnessFactor);

    if (src.emissiveFactor.size() >= 3) {
        dst.emissiveFactor = glm::vec3(src.emissiveFactor[0], src.emissiveFactor[1], src.emissiveFactor[2]);
    }
    dst.alphaCutoff = static_cast<float>(src.alphaCutoff);
    dst.doubleSided = src.doubleSided;
    if (src.alphaMode == "MASK") dst.alphaMode = AlphaMode::eMask;
    else if (src.alphaMode == "BLEND") dst.alphaMode = AlphaMode::eBlend;

    // A texture points at a source image; materials index images directly so
    // examples can upload Model::images without a second indirection.
    auto lookup = [&](int textureIndex) -> int {
        if (textureIndex < 0 || textureIndex >= static_cast<int>(textureToImage.size())) return -1;
        return textureToImage[textureIndex];
    };
    dst.baseColorTexture = lookup(pbr.baseColorTexture.index);
    dst.normalTexture = lookup(src.normalTexture.index);
    dst.metallicRoughnessTexture = lookup(pbr.metallicRoughnessTexture.index);
    dst.occlusionTexture = lookup(src.occlusionTexture.index);
    dst.emissiveTexture = lookup(src.emissiveTexture.index);
    return dst;
}

glm::mat4 nodeTransform(const tinygltf::Node &node)
{
    if (node.matrix.size() == 16) {
        // glTF matrices are column-major, same convention as glm.
        glm::mat4 m{1.0f};
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                m[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
            }
        }
        return m;
    }
    glm::mat4 translation{1.0f};
    glm::mat4 rotation{1.0f};
    glm::mat4 scale{1.0f};
    if (node.translation.size() == 3) {
        translation = glm::translate(translation, glm::vec3(node.translation[0], node.translation[1],
                                                            node.translation[2]));
    }
    if (node.rotation.size() == 4) {
        glm::quat q(static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]),
                    static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]));
        rotation = glm::mat4_cast(q);
    }
    if (node.scale.size() == 3) {
        scale = glm::scale(scale, glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
    }
    return translation * rotation * scale;
}

// Resolves a side-car URI against the model's directory.  Previously the
// loader handed the raw URI to stb_image, which resolved it against the
// process working directory -- correct only when the sample happened to be run
// from data/models.
std::string resolveUri(const std::string &baseDirectory, const std::string &uri)
{
    if (uri.empty()) return {};
    if (uri.rfind("data:", 0) == 0) return {}; // tinygltf decodes these itself
    const std::filesystem::path candidate(uri);
    if (candidate.is_absolute() || baseDirectory.empty()) return uri;
    return (std::filesystem::path(baseDirectory) / candidate).string();
}

void decodeImage(const std::string &uri, TextureImage &out)
{
    if (uri.empty()) return;
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *pixels = stbi_load(uri.c_str(), &width, &height, &channels, 4);
    if (!pixels) return;
    out.width = width;
    out.height = height;
    out.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    out.mipLevels =
        static_cast<uint32_t>(std::log2(static_cast<float>(std::max(width, height)))) + 1;
}

} // namespace

Model GltfModelLoader::load(const std::string &path, const ModelLoadOptions &options) const
{
    tinygltf::Model input;
    tinygltf::TinyGLTF loader;
    std::string errors;
    std::string warnings;

    // This vendored tinygltf has separate ASCII/binary entry points.
    const bool isBinary = path.size() >= 4 && path.compare(path.size() - 4, 4, ".glb") == 0;
    const bool loaded = isBinary ? loader.LoadBinaryFromFile(&input, &errors, &warnings, path)
                                 : loader.LoadASCIIFromFile(&input, &errors, &warnings, path);
    if (!loaded) {
        throw std::runtime_error("gltf: failed to load '" + path + "': " + errors);
    }

    std::string baseDirectory = options.baseDirectory;
    if (baseDirectory.empty()) {
        baseDirectory = std::filesystem::path(path).parent_path().string();
    }

    Model model;

    // ----- Images: decode every image referenced by the model -------------
    model.images.resize(input.images.size());
    for (size_t i = 0; i < input.images.size(); ++i) {
        const tinygltf::Image &image = input.images[i];
        TextureImage &out = model.images[i];
        out.name = image.name;
        if (image.width > 0 && image.height > 0 && !image.image.empty()) {
            // tinygltf already decoded it (embedded data URI or .glb buffer).
            out.width = image.width;
            out.height = image.height;
            out.pixels = image.image;
            if (out.pixels.size() != static_cast<size_t>(out.width) * out.height * 4) {
                // Convert to RGBA if the source had a different component count.
                std::vector<uint8_t> rgba(static_cast<size_t>(out.width) * out.height * 4, 255);
                const size_t srcChannels = out.pixels.size() / static_cast<size_t>(out.width * out.height);
                for (size_t p = 0; p < static_cast<size_t>(out.width * out.height); ++p) {
                    for (size_t c = 0; c < 4 && c < srcChannels; ++c) {
                        rgba[p * 4 + c] = out.pixels[p * srcChannels + c];
                    }
                }
                out.pixels = std::move(rgba);
            }
        } else {
            decodeImage(resolveUri(baseDirectory, image.uri), out);
        }
        if (out.width > 0 && out.height > 0) {
            out.mipLevels = static_cast<uint32_t>(std::log2(static_cast<float>(std::max(out.width, out.height)))) + 1;
        }
    }

    // Texture -> image index map (one texture has one source image).
    std::vector<int> textureToImage(input.textures.size(), -1);
    for (size_t i = 0; i < input.textures.size(); ++i) {
        const int source = input.textures[i].source;
        if (source >= 0 && source < static_cast<int>(input.images.size())) {
            textureToImage[i] = source;
        }
    }

    // ----- Materials -------------------------------------------------------
    model.materials.resize(input.materials.size());
    for (size_t i = 0; i < input.materials.size(); ++i) {
        model.materials[i] = convertMaterial(input.materials[i], textureToImage);
    }

    // ----- Meshes, vertices and indices ------------------------------------
    model.meshes.resize(input.meshes.size());

    for (size_t meshIndex = 0; meshIndex < input.meshes.size(); ++meshIndex) {
        const tinygltf::Mesh &srcMesh = input.meshes[meshIndex];
        Mesh &dstMesh = model.meshes[meshIndex];
        dstMesh.name = srcMesh.name;

        for (const tinygltf::Primitive &primitive : srcMesh.primitives) {
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
                continue; // only triangle primitives are relevant here
            }
            auto posIt = primitive.attributes.find("POSITION");
            if (posIt == primitive.attributes.end()) continue;
            const Accessor &positionAccessor = input.accessors[posIt->second];
            const size_t vertexCount = positionAccessor.count;
            const uint32_t vertexStart = static_cast<uint32_t>(model.vertices.size());
            const uint32_t firstIndex = static_cast<uint32_t>(model.indices.size());

            auto normIt = primitive.attributes.find("NORMAL");
            auto uvIt = primitive.attributes.find("TEXCOORD_0");
            auto tangentIt = primitive.attributes.find("TANGENT");
            auto colorIt = primitive.attributes.find("COLOR_0");

            std::vector<glm::vec4> positions, normals, uvs, tangents, colors;
            readAttributeArray(input, posIt->second, glm::vec4(0.0f), vertexCount, positions);
            readAttributeArray(input, normIt == primitive.attributes.end() ? -1 : normIt->second,
                               glm::vec4(0.0f, 0.0f, 1.0f, 0.0f), vertexCount, normals);
            readAttributeArray(input, uvIt == primitive.attributes.end() ? -1 : uvIt->second,
                               glm::vec4(0.0f), vertexCount, uvs);
            readAttributeArray(input, tangentIt == primitive.attributes.end() ? -1 : tangentIt->second,
                               glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), vertexCount, tangents);
            readAttributeArray(input, colorIt == primitive.attributes.end() ? -1 : colorIt->second,
                               glm::vec4(1.0f), vertexCount, colors);

            for (size_t v = 0; v < vertexCount; ++v) {
                MeshVertex vertex;
                vertex.position = glm::vec3(positions[v]);
                // A zero-length normal would poison every lighting term and
                // the tangent basis; fall back to +Z instead.
                const glm::vec3 normal = glm::vec3(normals[v]);
                const float normalLength = glm::length(normal);
                vertex.normal = normalLength > 0.0f ? normal / normalLength : glm::vec3(0.0f, 0.0f, 1.0f);
                vertex.texCoord = glm::vec2(uvs[v]);
                vertex.tangent = glm::vec3(tangents[v]);
                vertex.color = glm::vec3(colors[v]);
                model.vertices.push_back(vertex);
            }

            // Indices (support uint8 / uint16 / uint32).
            if (primitive.indices >= 0 && primitive.indices < static_cast<int>(input.accessors.size())) {
                const Accessor &indexAccessor = input.accessors[primitive.indices];
                if (indexAccessor.bufferView >= 0
                    && indexAccessor.bufferView < static_cast<int>(input.bufferViews.size())) {
                    const BufferView &view = input.bufferViews[indexAccessor.bufferView];
                    if (view.buffer >= 0 && view.buffer < static_cast<int>(input.buffers.size())) {
                        const std::vector<unsigned char> &bytes = input.buffers[view.buffer].data;
                        const size_t base = static_cast<size_t>(indexAccessor.byteOffset)
                                            + static_cast<size_t>(view.byteOffset);
                        const size_t width = indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT ? 4
                                             : indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? 2
                                             : 1;
                        const size_t stride = view.byteStride > 0 ? static_cast<size_t>(view.byteStride) : width;
                        for (size_t i = 0; i < indexAccessor.count; ++i) {
                            const unsigned char *p = bytes.data() + base + i * stride;
                            uint32_t index = 0;
                            if (p + width <= bytes.data() + bytes.size()) {
                                if (width == 4) {
                                    std::memcpy(&index, p, 4);
                                } else if (width == 2) {
                                    uint16_t v = 0;
                                    std::memcpy(&v, p, 2);
                                    index = v;
                                } else {
                                    index = *p;
                                }
                            }
                            model.indices.push_back(index + vertexStart);
                        }
                    }
                }
            }

            Primitive dstPrimitive;
            dstPrimitive.firstIndex = firstIndex;
            dstPrimitive.indexCount = static_cast<uint32_t>(model.indices.size()) - firstIndex;
            dstPrimitive.materialIndex = primitive.material;
            // Honest attribute reporting: the post-process chain only fills in
            // what is missing, so a present-but-degenerate TANGENT must not be
            // recomputed behind the file's back.
            dstPrimitive.attributes = VertexAttribute::ePosition;
            if (normIt != primitive.attributes.end()) dstPrimitive.attributes |= VertexAttribute::eNormal;
            if (uvIt != primitive.attributes.end()) dstPrimitive.attributes |= VertexAttribute::eTexCoord;
            if (tangentIt != primitive.attributes.end()) dstPrimitive.attributes |= VertexAttribute::eTangent;
            if (colorIt != primitive.attributes.end()) dstPrimitive.attributes |= VertexAttribute::eColor;
            dstMesh.primitives.push_back(dstPrimitive);
        }
    }

    // ----- Nodes ------------------------------------------------------------
    model.nodes.resize(input.nodes.size());
    for (size_t i = 0; i < input.nodes.size(); ++i) {
        const tinygltf::Node &src = input.nodes[i];
        ModelNode &dst = model.nodes[i];
        dst.name = src.name;
        dst.transform = nodeTransform(src);
        dst.meshIndex = src.mesh;
        for (int child : src.children) {
            if (child >= 0 && child < static_cast<int>(input.nodes.size())) {
                dst.children.push_back(child);
            }
        }
    }
    if (!input.scenes.empty()) {
        for (int root : input.scenes[0].nodes) {
            if (root >= 0 && root < static_cast<int>(input.nodes.size())) {
                model.rootNodes.push_back(root);
            }
        }
    }
    if (model.rootNodes.empty()) {
        std::vector<bool> isChild(model.nodes.size(), false);
        for (const ModelNode &node : model.nodes) {
            for (int child : node.children) {
                if (child >= 0 && child < static_cast<int>(model.nodes.size())) isChild[child] = true;
            }
        }
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            if (!isChild[i]) model.rootNodes.push_back(static_cast<int>(i));
        }
    }

    model.recomputeBounds();
    return model;
}
