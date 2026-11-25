// Single translation unit that compiles tiny_obj_loader.  stb_image is only
// declared here; its one implementation lives in GltfModelLoader.cpp.
#define TINYOBJLOADER_IMPLEMENTATION
#include "ObjModelLoader.h"

#include <tiny_obj_loader.h>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

// OBJ indexes position, texcoord and normal independently, so a vertex is
// identified by the triple rather than by any single index.  Deduplicating on
// the triple is exact -- unlike hashing the resolved floats -- and it is what
// collapses the fully exploded face stream back into an indexed mesh.
struct IndexTriple {
    int position = -1;
    int texcoord = -1;
    int normal = -1;

    bool operator==(const IndexTriple &other) const
    {
        return position == other.position && texcoord == other.texcoord && normal == other.normal;
    }
};

struct IndexTripleHash {
    size_t operator()(const IndexTriple &triple) const
    {
        size_t hash = static_cast<size_t>(triple.position) * 0x9e3779b97f4a7c15ull;
        hash ^= static_cast<size_t>(triple.texcoord) + 0x517cc1b727220a95ull + (hash << 6) + (hash >> 2);
        hash ^= static_cast<size_t>(triple.normal) + 0x517cc1b727220a95ull + (hash << 6) + (hash >> 2);
        return hash;
    }
};

std::string resolvePath(const std::string &baseDirectory, const std::string &name)
{
    if (name.empty()) return {};
    const std::filesystem::path candidate(name);
    if (candidate.is_absolute() || baseDirectory.empty()) return name;
    return (std::filesystem::path(baseDirectory) / candidate).string();
}

// Decodes a side-car texture once per file and returns its index in
// model.images, or -1 when the map is missing (a normal situation: plenty of
// OBJ files reference .mtl textures that were never shipped).
int resolveTexture(const std::string &name, const std::string &baseDirectory, std::vector<TextureImage> &images,
                   std::unordered_map<std::string, int> &cache)
{
    if (name.empty()) return -1;
    auto cached = cache.find(name);
    if (cached != cache.end()) return cached->second;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *pixels = stbi_load(resolvePath(baseDirectory, name).c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        cache.emplace(name, -1);
        return -1;
    }

    TextureImage image;
    image.name = name;
    image.width = width;
    image.height = height;
    image.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    image.mipLevels = static_cast<uint32_t>(std::log2(static_cast<float>(std::max(width, height)))) + 1;

    const int index = static_cast<int>(images.size());
    images.push_back(std::move(image));
    cache.emplace(name, index);
    return index;
}

Material convertMaterial(const tinyobj::material_t &src, const std::string &baseDirectory,
                         std::vector<TextureImage> &images, std::unordered_map<std::string, int> &cache)
{
    Material dst;
    dst.name = src.name;
    dst.baseColorFactor = glm::vec4(static_cast<float>(src.diffuse[0]), static_cast<float>(src.diffuse[1]),
                                    static_cast<float>(src.diffuse[2]), static_cast<float>(src.dissolve));
    dst.emissiveFactor = glm::vec3(static_cast<float>(src.emission[0]), static_cast<float>(src.emission[1]),
                                   static_cast<float>(src.emission[2]));

    // Core MTL has no metallic/roughness.  The PBR extension (map_Pm/map_Pr
    // plus the scalar forms) writes them; otherwise stay dielectric and derive
    // roughness from the Phong exponent, which is the only hint available.
    dst.metallicFactor = src.metallic > 0.0f ? static_cast<float>(src.metallic) : 0.0f;
    dst.roughnessFactor = src.roughness > 0.0f
                              ? static_cast<float>(src.roughness)
                              : glm::clamp(1.0f - static_cast<float>(src.shininess) / 1000.0f, 0.08f, 1.0f);

    // dissolve == 1 is fully opaque; anything less means the file wants
    // blending.  Alpha testing is a glTF concept and has no MTL equivalent.
    dst.alphaMode = src.dissolve < 0.999f ? AlphaMode::eBlend : AlphaMode::eOpaque;
    // OBJ does not record winding guarantees, and exporters disagree, so
    // treating every surface as two-sided avoids inverted, invisible faces.
    dst.doubleSided = true;

    dst.baseColorTexture = resolveTexture(src.diffuse_texname, baseDirectory, images, cache);
    dst.normalTexture =
        resolveTexture(src.normal_texname.empty() ? src.bump_texname : src.normal_texname, baseDirectory, images,
                       cache);
    dst.occlusionTexture = resolveTexture(src.ambient_texname, baseDirectory, images, cache);
    dst.emissiveTexture = resolveTexture(src.emissive_texname, baseDirectory, images, cache);
    return dst;
}

float attributeAt(const std::vector<tinyobj::real_t> &values, int index, int component, int width)
{
    if (index < 0) return 0.0f;
    const size_t offset = static_cast<size_t>(index) * width + component;
    return offset < values.size() ? static_cast<float>(values[offset]) : 0.0f;
}

} // namespace

Model ObjModelLoader::load(const std::string &path, const ModelLoadOptions &options) const
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string error;

    std::string baseDirectory = options.baseDirectory;
    if (baseDirectory.empty()) {
        baseDirectory = std::filesystem::path(path).parent_path().string();
    }
    // tiny_obj_loader concatenates mtl_basedir with the .mtl name verbatim, so
    // the separator has to be part of the base.
    const std::string mtlBaseDirectory = baseDirectory.empty() ? std::string() : baseDirectory + "/";

    // triangulate = true: the engine only draws triangle lists, and the
    // loader would otherwise emit one index per polygon corner.  Polygons are
    // fanned, so quads and n-gons keep their winding.
    const bool loaded = tinyobj::LoadObj(&attrib, &shapes, &materials, &error, path.c_str(),
                                         mtlBaseDirectory.empty() ? nullptr : mtlBaseDirectory.c_str(),
                                         /*triangulate=*/true);
    if (!loaded) {
        throw std::runtime_error("obj: failed to load '" + path + "': " + error);
    }

    Model model;

    std::unordered_map<std::string, int> textureCache;
    model.materials.reserve(materials.size());
    for (const tinyobj::material_t &source : materials) {
        model.materials.push_back(convertMaterial(source, baseDirectory, model.images, textureCache));
    }

    for (const tinyobj::shape_t &shape : shapes) {
        const tinyobj::mesh_t &source = shape.mesh;
        if (source.indices.empty()) continue;

        Mesh mesh;
        mesh.name = shape.name;

        // A primitive carries exactly one material, so the face stream is cut
        // into runs of equal material id.  Runs (rather than one primitive per
        // distinct material) keep the output ordered like the file.
        Primitive primitive;
        std::unordered_map<IndexTriple, uint32_t, IndexTripleHash> uniqueVertices;
        bool hasNormal = true;
        bool hasTexCoord = true;
        size_t cursor = 0;
        int currentMaterial = -2; // -2 = no primitive open yet

        auto closePrimitive = [&]() {
            if (currentMaterial == -2) return;
            currentMaterial = -2;
            primitive.indexCount = static_cast<uint32_t>(model.indices.size()) - primitive.firstIndex;
            if (primitive.indexCount == 0) return;
            primitive.attributes = VertexAttribute::ePosition;
            // Attributes are all-or-nothing per primitive: mixing generated
            // and file-provided normals inside one draw would show a seam.
            if (hasNormal) primitive.attributes |= VertexAttribute::eNormal;
            if (hasTexCoord) primitive.attributes |= VertexAttribute::eTexCoord;
            mesh.primitives.push_back(primitive);
        };

        for (size_t face = 0; face < source.num_face_vertices.size(); ++face) {
            const int faceMaterial =
                face < source.material_ids.size() ? source.material_ids[face] : -1;
            if (faceMaterial != currentMaterial) {
                closePrimitive();
                currentMaterial = faceMaterial;
                primitive = Primitive{};
                primitive.firstIndex = static_cast<uint32_t>(model.indices.size());
                primitive.materialIndex = faceMaterial;
                uniqueVertices.clear();
                hasNormal = true;
                hasTexCoord = true;
            }

            const int corners = source.num_face_vertices[face];
            // LoadObj was asked to triangulate, so this is always 3; skipping
            // anything else keeps the cursor aligned if a future caller turns
            // triangulation off.
            if (corners != 3) {
                cursor += static_cast<size_t>(corners);
                continue;
            }
            for (int corner = 0; corner < corners; ++corner) {
                if (cursor >= source.indices.size()) break;
                const tinyobj::index_t &index = source.indices[cursor++];

                const IndexTriple key{index.vertex_index, index.texcoord_index, index.normal_index};
                auto existing = uniqueVertices.find(key);
                if (existing != uniqueVertices.end()) {
                    model.indices.push_back(existing->second);
                    continue;
                }

                MeshVertex vertex;
                vertex.position = glm::vec3(attributeAt(attrib.vertices, index.vertex_index, 0, 3),
                                            attributeAt(attrib.vertices, index.vertex_index, 1, 3),
                                            attributeAt(attrib.vertices, index.vertex_index, 2, 3));
                if (index.normal_index >= 0) {
                    const glm::vec3 normal(attributeAt(attrib.normals, index.normal_index, 0, 3),
                                           attributeAt(attrib.normals, index.normal_index, 1, 3),
                                           attributeAt(attrib.normals, index.normal_index, 2, 3));
                    const float length = glm::length(normal);
                    vertex.normal = length > 0.0f ? normal / length : glm::vec3(0.0f, 0.0f, 1.0f);
                } else {
                    hasNormal = false;
                }
                if (index.texcoord_index >= 0) {
                    vertex.texCoord = glm::vec2(attributeAt(attrib.texcoords, index.texcoord_index, 0, 2),
                                                attributeAt(attrib.texcoords, index.texcoord_index, 1, 2));
                } else {
                    hasTexCoord = false;
                }
                // OBJ carries no per-vertex colour; the neutral white default
                // of MeshVertex applies.

                const uint32_t newIndex = static_cast<uint32_t>(model.vertices.size());
                model.vertices.push_back(vertex);
                uniqueVertices.emplace(key, newIndex);
                model.indices.push_back(newIndex);
            }
        }
        closePrimitive();

        if (!mesh.primitives.empty()) {
            model.meshes.push_back(std::move(mesh));
        }
    }

    if (model.meshes.empty()) {
        throw std::runtime_error("obj: '" + path + "' contains no triangle geometry" + (error.empty() ? "" : (": " + error)));
    }

    // OBJ has no scene graph.  One root node per mesh keeps the traversal code
    // in the examples, and forEachMeshInstance, working unchanged.
    model.nodes.reserve(model.meshes.size());
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        ModelNode node;
        node.name = model.meshes[i].name;
        node.meshIndex = static_cast<int>(i);
        model.nodes.push_back(std::move(node));
        model.rootNodes.push_back(static_cast<int>(i));
    }

    model.recomputeBounds();
    return model;
}
