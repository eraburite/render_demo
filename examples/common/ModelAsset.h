#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// Loader-neutral model data.
//
// Every loader (glTF today, OBJ below, FBX/PLY/自建 later) produces exactly
// this structure, and every example consumes exactly this structure.  Neither
// side therefore knows which file format the geometry came from.
//
// The representation is a single interleaved vertex buffer plus one index
// buffer: a primitive is just (firstIndex, indexCount, materialIndex), so a
// whole scene renders with per-primitive draws over one buffer binding.  Each
// loader guarantees the stronger property that a primitive's index window is
// self-contained -- dedup and attribute fix-up never reach across primitives,
// which keeps firstIndex/indexCount meaningful.
// ---------------------------------------------------------------------------

// One vertex of the single interleaved layout every example uploads.
// Missing attributes keep their neutral default; check Primitive::attributes
// to know whether a value was actually present in the file.
struct MeshVertex {
    glm::vec3 position{};
    glm::vec3 normal{};
    glm::vec2 texCoord{};
    glm::vec3 tangent{};
    glm::vec3 color{1.0f, 1.0f, 1.0f};
};

enum class AlphaMode {
    eOpaque,
    eMask,
    eBlend,
};

struct Material {
    std::string name;
    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float alphaCutoff = 0.5f;
    AlphaMode alphaMode = AlphaMode::eOpaque;
    bool doubleSided = false;
    // Indices into Model::images, -1 when the slot is unused.
    int baseColorTexture = -1;
    int normalTexture = -1;
    int metallicRoughnessTexture = -1;
    int occlusionTexture = -1;
    int emissiveTexture = -1;
};

// Attribute presence bits.  Loaders fill the ones the file really carried;
// the post-process chain uses them to decide what still needs generating.
namespace VertexAttribute {
inline constexpr uint32_t ePosition = 1u << 0;
inline constexpr uint32_t eNormal = 1u << 1;
inline constexpr uint32_t eTangent = 1u << 2;
inline constexpr uint32_t eTexCoord = 1u << 3;
inline constexpr uint32_t eColor = 1u << 4;
} // namespace VertexAttribute

struct Primitive {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int materialIndex = -1;
    uint32_t attributes = VertexAttribute::ePosition;

    bool has(uint32_t attribute) const { return (attributes & attribute) != 0; }
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
};

// One node of the file's hierarchy.  `transform` is the local matrix with
// TRS already composed by the loader.
struct ModelNode {
    std::string name;
    glm::mat4 transform{1.0f};
    int meshIndex = -1;
    std::vector<int> children;
};

struct TextureImage {
    std::vector<uint8_t> pixels; // RGBA8
    int width = 0;
    int height = 0;
    uint32_t mipLevels = 1;
    std::string name;
};

struct Model {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Mesh> meshes;
    std::vector<ModelNode> nodes;
    std::vector<int> rootNodes;
    std::vector<Material> materials;
    std::vector<TextureImage> images;
    glm::vec3 minBounds{0.0f};
    glm::vec3 maxBounds{0.0f};

    bool valid() const { return !vertices.empty() && !indices.empty(); }

    uint64_t triangleCount() const { return indices.size() / 3; }

    // Number of (node, mesh) pairs that actually draw something.
    uint32_t instanceCount() const;

    // Recomputes min/maxBounds from the current vertex positions.
    void recomputeBounds();
};

// World matrix per node, parallel to model.nodes.  Cycles and duplicate
// parents are handled (each node is visited once); orphans that no root
// reaches keep the identity.
std::vector<glm::mat4> computeNodeWorldMatrices(const Model &model);

// Visits every drawing (node, mesh, primitive) triple depth-first in scene
// order, so callers do not re-implement hierarchy traversal.  `world` is the
// node's world matrix, `nodeIndex` lets a caller apply per-node visibility.
using MeshInstanceCallback =
    std::function<void(const glm::mat4 &world, int nodeIndex, const Mesh &mesh, const Primitive &primitive)>;
void forEachMeshInstance(const Model &model, const MeshInstanceCallback &fn);
