#include "ModelLoader.h"

#include "AssimpModelLoader.h"
#include "GltfModelLoader.h"
#include "ObjModelLoader.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

std::string lowerExtension(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// Vertices are compared on quantised attribute values rather than raw bytes:
// bit comparison would treat -0.0f and +0.0f as different and would miss
// values that differ only in the last mantissa bit after a float round-trip
// through an intermediate matrix.
struct VertexKey {
    std::array<int64_t, 14> components{};

    bool operator==(const VertexKey &other) const { return components == other.components; }
};

struct VertexKeyHash {
    size_t operator()(const VertexKey &key) const
    {
        size_t hash = 0xcbf29ce484222325ull;
        for (int64_t value : key.components) {
            hash ^= static_cast<size_t>(value);
            hash *= 0x100000001b3ull;
            hash ^= hash >> 29;
        }
        return hash;
    }
};

constexpr double kQuantiseScale = 1.0e6;

VertexKey makeVertexKey(const MeshVertex &vertex)
{
    const double values[14] = {
        vertex.position.x, vertex.position.y, vertex.position.z,
        vertex.normal.x,   vertex.normal.y,   vertex.normal.z,
        vertex.texCoord.x, vertex.texCoord.y,
        vertex.tangent.x,  vertex.tangent.y,  vertex.tangent.z,
        vertex.color.x,    vertex.color.y,    vertex.color.z,
    };
    VertexKey key;
    for (size_t i = 0; i < 14; ++i) {
        key.components[i] = static_cast<int64_t>(std::llround(values[i] * kQuantiseScale));
    }
    return key;
}

// glm's default constructor leaves members uninitialised, so
// unordered_map::operator[] would accumulate into garbage.  Always seed with
// an explicit zero vector.
void accumulate(std::unordered_map<uint32_t, glm::vec3> &sums, uint32_t index, const glm::vec3 &value)
{
    auto it = sums.find(index);
    if (it == sums.end()) {
        sums.emplace(index, value);
    } else {
        it->second += value;
    }
}

PostProcessPlan mergePlan(const IModelLoader &loader, const ModelLoadOptions &options, const std::string &path)
{
    PostProcessPlan plan = loader.defaultPlan(path);
    if (options.deduplicate != Toggle::eAuto) plan.deduplicate = options.deduplicate == Toggle::eOn;
    if (options.flipTexCoordV != Toggle::eAuto) plan.flipTexCoordV = options.flipTexCoordV == Toggle::eOn;
    if (options.generateNormals != Toggle::eAuto) plan.generateNormals = options.generateNormals == Toggle::eOn;
    if (options.generateTangents != Toggle::eAuto) plan.generateTangents = options.generateTangents == Toggle::eOn;
    return plan;
}

} // namespace

namespace model_post {

void flattenScene(Model &model)
{
    if (model.meshes.empty() || model.indices.empty()) return;

    std::vector<MeshVertex> outVertices;
    std::vector<uint32_t> outIndices;
    std::vector<Mesh> outMeshes;
    std::vector<ModelNode> outNodes;
    outVertices.reserve(model.vertices.size());
    outIndices.reserve(model.indices.size());

    std::vector<bool> visited(model.meshes.size(), false);

    // Primitives arrive grouped per node (forEachMeshInstance is depth first), so
    // a change of nodeIndex starts a new output mesh.  Each output primitive keeps
    // its own index window, which is the invariant dedup and the attribute passes
    // rely on.
    int openNodeIndex = -2;
    Mesh openMesh;

    forEachMeshInstance(model, [&](const glm::mat4 &world, int nodeIndex, const Mesh &mesh,
                                   const Primitive &primitive) {
        // forEachMeshInstance hands out references into model.meshes, so the index
        // of the mesh being visited is its address offset.
        const size_t meshIndex = static_cast<size_t>(&mesh - model.meshes.data());
        if (meshIndex < visited.size()) visited[meshIndex] = true;

        if (nodeIndex != openNodeIndex) {
            if (openNodeIndex != -2 && !openMesh.primitives.empty()) {
                ModelNode node;
                node.name = openMesh.name;
                node.meshIndex = static_cast<int>(outMeshes.size());
                outMeshes.push_back(std::move(openMesh));
                outNodes.push_back(std::move(node));
            }
            openNodeIndex = nodeIndex;
            openMesh = Mesh{};
            openMesh.name = mesh.name;
        }

        const uint32_t firstIndex = static_cast<uint32_t>(outIndices.size());
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
        const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                static_cast<uint32_t>(model.indices.size()));

        std::unordered_map<uint32_t, uint32_t> remap;
        for (uint32_t i = primitive.firstIndex; i < end; ++i) {
            const uint32_t source = model.indices[i];
            if (source >= model.vertices.size()) continue;
            const auto existing = remap.find(source);
            if (existing != remap.end()) {
                outIndices.push_back(existing->second);
                continue;
            }
            const MeshVertex &src = model.vertices[source];
            MeshVertex vertex = src;
            vertex.position = glm::vec3(world * glm::vec4(src.position, 1.0f));
            const glm::vec3 normal = normalMatrix * src.normal;
            const float length = glm::length(normal);
            vertex.normal = length > 0.0f ? normal / length : src.normal;
            const uint32_t index = static_cast<uint32_t>(outVertices.size());
            outVertices.push_back(vertex);
            remap.emplace(source, index);
            outIndices.push_back(index);
        }

        Primitive remapped = primitive;
        remapped.firstIndex = firstIndex;
        remapped.indexCount = static_cast<uint32_t>(outIndices.size()) - firstIndex;
        if (remapped.indexCount > 0) {
            openMesh.primitives.push_back(remapped);
        }
    });

    if (openNodeIndex != -2 && !openMesh.primitives.empty()) {
        ModelNode node;
        node.name = openMesh.name;
        node.meshIndex = static_cast<int>(outMeshes.size());
        outMeshes.push_back(std::move(openMesh));
        outNodes.push_back(std::move(node));
    }

    // Geometry no node reaches would otherwise disappear silently.
    for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
        if (visited[meshIndex] || model.meshes[meshIndex].primitives.empty()) continue;
        const Mesh &mesh = model.meshes[meshIndex];
        Mesh copy;
        copy.name = mesh.name;
        for (const Primitive &primitive : mesh.primitives) {
            const uint32_t firstIndex = static_cast<uint32_t>(outIndices.size());
            const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                    static_cast<uint32_t>(model.indices.size()));
            for (uint32_t i = primitive.firstIndex; i < end; ++i) {
                const uint32_t source = model.indices[i];
                if (source >= model.vertices.size()) continue;
                outIndices.push_back(static_cast<uint32_t>(outVertices.size()));
                outVertices.push_back(model.vertices[source]);
            }
            Primitive remapped = primitive;
            remapped.firstIndex = firstIndex;
            remapped.indexCount = static_cast<uint32_t>(outIndices.size()) - firstIndex;
            if (remapped.indexCount > 0) copy.primitives.push_back(remapped);
        }
        if (copy.primitives.empty()) continue;
        ModelNode node;
        node.name = copy.name;
        node.meshIndex = static_cast<int>(outMeshes.size());
        outMeshes.push_back(std::move(copy));
        outNodes.push_back(std::move(node));
    }

    if (outMeshes.empty()) return;

    model.vertices = std::move(outVertices);
    model.indices = std::move(outIndices);
    model.meshes = std::move(outMeshes);
    model.nodes = std::move(outNodes);
    model.rootNodes.clear();
    model.rootNodes.reserve(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        model.rootNodes.push_back(static_cast<int>(i));
    }
    model.recomputeBounds();
}

void deduplicateVertices(Model &model)
{
    if (model.meshes.empty() || model.indices.empty()) return;

    std::vector<MeshVertex> outVertices;
    std::vector<uint32_t> outIndices;
    outVertices.reserve(model.vertices.size());
    outIndices.reserve(model.indices.size());

    // Each primitive is compacted into its own private window.  Sharing
    // vertices across primitives would still render correctly, but it would
    // break the invariant that a primitive occupies a contiguous index range,
    // which every example relies on.
    for (Mesh &mesh : model.meshes) {
        for (Primitive &primitive : mesh.primitives) {
            const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                    static_cast<uint32_t>(model.indices.size()));
            const uint32_t newFirstIndex = static_cast<uint32_t>(outIndices.size());

            std::unordered_map<uint32_t, uint32_t> oldToNew; // source vertex -> output vertex
            std::unordered_map<VertexKey, uint32_t, VertexKeyHash> uniqueVertices;

            for (uint32_t i = primitive.firstIndex; i < end; ++i) {
                const uint32_t sourceIndex = model.indices[i];
                if (sourceIndex >= model.vertices.size()) continue;

                auto remapped = oldToNew.find(sourceIndex);
                uint32_t targetIndex = 0;
                if (remapped != oldToNew.end()) {
                    targetIndex = remapped->second;
                } else {
                    const MeshVertex &source = model.vertices[sourceIndex];
                    const VertexKey key = makeVertexKey(source);
                    auto existing = uniqueVertices.find(key);
                    if (existing != uniqueVertices.end()) {
                        targetIndex = existing->second;
                    } else {
                        targetIndex = static_cast<uint32_t>(outVertices.size());
                        outVertices.push_back(source);
                        uniqueVertices.emplace(key, targetIndex);
                    }
                    oldToNew.emplace(sourceIndex, targetIndex);
                }
                outIndices.push_back(targetIndex);
            }

            primitive.firstIndex = newFirstIndex;
            primitive.indexCount = static_cast<uint32_t>(outIndices.size()) - newFirstIndex;
        }
    }

    model.vertices = std::move(outVertices);
    model.indices = std::move(outIndices);
    model.recomputeBounds();
}

void generateMissingNormals(Model &model)
{
    for (Mesh &mesh : model.meshes) {
        for (Primitive &primitive : mesh.primitives) {
            if (primitive.has(VertexAttribute::eNormal)) continue;
            const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                    static_cast<uint32_t>(model.indices.size()));
            if (end <= primitive.firstIndex) continue;

            // Unnormalised cross products accumulate with an area weight, so
            // large triangles dominate -- the standard cheap smooth normal.
            std::unordered_map<uint32_t, glm::vec3> sums;
            for (uint32_t i = primitive.firstIndex; i + 2 < end; i += 3) {
                const uint32_t i0 = model.indices[i];
                const uint32_t i1 = model.indices[i + 1];
                const uint32_t i2 = model.indices[i + 2];
                if (i0 >= model.vertices.size() || i1 >= model.vertices.size() || i2 >= model.vertices.size()) {
                    continue;
                }
                const glm::vec3 &p0 = model.vertices[i0].position;
                const glm::vec3 &p1 = model.vertices[i1].position;
                const glm::vec3 &p2 = model.vertices[i2].position;
                const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
                if (glm::dot(faceNormal, faceNormal) <= 0.0f) continue; // degenerate
                accumulate(sums, i0, faceNormal);
                accumulate(sums, i1, faceNormal);
                accumulate(sums, i2, faceNormal);
            }

            for (const auto &[index, sum] : sums) {
                const float length = glm::length(sum);
                model.vertices[index].normal = length > 0.0f ? sum / length : glm::vec3(0.0f, 0.0f, 1.0f);
            }
            primitive.attributes |= VertexAttribute::eNormal;
        }
    }
}

void generateMissingTangents(Model &model)
{
    for (Mesh &mesh : model.meshes) {
        for (Primitive &primitive : mesh.primitives) {
            if (primitive.has(VertexAttribute::eTangent)) continue;
            const bool usable = primitive.has(VertexAttribute::eNormal)
                                && primitive.has(VertexAttribute::eTexCoord);
            if (!usable) continue;

            const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                    static_cast<uint32_t>(model.indices.size()));
            std::unordered_map<uint32_t, glm::vec3> sums;
            for (uint32_t i = primitive.firstIndex; i + 2 < end; i += 3) {
                const uint32_t i0 = model.indices[i];
                const uint32_t i1 = model.indices[i + 1];
                const uint32_t i2 = model.indices[i + 2];
                if (i0 >= model.vertices.size() || i1 >= model.vertices.size() || i2 >= model.vertices.size()) {
                    continue;
                }
                const MeshVertex &v0 = model.vertices[i0];
                const MeshVertex &v1 = model.vertices[i1];
                const MeshVertex &v2 = model.vertices[i2];
                const glm::vec3 edge1 = v1.position - v0.position;
                const glm::vec3 edge2 = v2.position - v0.position;
                const glm::vec2 delta1 = v1.texCoord - v0.texCoord;
                const glm::vec2 delta2 = v2.texCoord - v0.texCoord;
                const float determinant = delta1.x * delta2.y - delta2.x * delta1.y;
                if (std::fabs(determinant) < 1.0e-12f) continue;
                const float r = 1.0f / determinant;
                const glm::vec3 tangent = (edge1 * delta2.y - edge2 * delta1.y) * r;
                if (glm::dot(tangent, tangent) <= 0.0f) continue;
                accumulate(sums, i0, tangent);
                accumulate(sums, i1, tangent);
                accumulate(sums, i2, tangent);
            }

            for (const auto &[index, sum] : sums) {
                MeshVertex &vertex = model.vertices[index];
                // Remove the normal component, then renormalise: an
                // orthogonal basis is what the shader's TBN assumes.
                glm::vec3 tangent = sum - vertex.normal * glm::dot(vertex.normal, sum);
                const float length = glm::length(tangent);
                if (length > 0.0f) {
                    vertex.tangent = tangent / length;
                } else {
                    const glm::vec3 fallback = std::fabs(vertex.normal.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                                                 : glm::vec3(1.0f, 0.0f, 0.0f);
                    vertex.tangent = glm::normalize(glm::cross(fallback, vertex.normal));
                }
            }
            primitive.attributes |= VertexAttribute::eTangent;
        }
    }
}

void flipTexCoordV(Model &model)
{
    for (MeshVertex &vertex : model.vertices) {
        vertex.texCoord.y = 1.0f - vertex.texCoord.y;
    }
}

void transformPositions(Model &model, const glm::mat4 &transform)
{
    for (MeshVertex &vertex : model.vertices) {
        vertex.position = glm::vec3(transform * glm::vec4(vertex.position, 1.0f));
    }
    model.recomputeBounds();
}

void scalePositions(Model &model, float scale)
{
    if (scale == 1.0f) return;
    transformPositions(model, glm::scale(glm::mat4(1.0f), glm::vec3(scale)));
}

void centerAtOrigin(Model &model)
{
    model.recomputeBounds();
    const glm::vec3 center = (model.minBounds + model.maxBounds) * 0.5f;
    if (center == glm::vec3(0.0f)) return;
    transformPositions(model, glm::translate(glm::mat4(1.0f), -center));
}

void normalizeSize(Model &model, float targetSize)
{
    model.recomputeBounds();
    const glm::vec3 extent = model.maxBounds - model.minBounds;
    const float longest = std::max({extent.x, extent.y, extent.z, 0.0001f});
    centerAtOrigin(model);
    scalePositions(model, targetSize / longest);
}

} // namespace model_post

ModelLoaderRegistry &ModelLoaderRegistry::instance()
{
    static ModelLoaderRegistry registry;
    return registry;
}

ModelLoaderRegistry::ModelLoaderRegistry()
{
    // Registration order is priority order for `extensions()` resolution, which
    // walks the list in reverse: the hand-written loaders therefore stay
    // authoritative for .gltf/.obj, and assimp only takes the formats nobody
    // else covers (.dae, .fbx, .3ds, ...).  That is deliberate -- swapping the
    // OBJ path onto a third-party importer would change existing examples'
    // output under a library upgrade, with nothing in this repo to compare
    // against.
    registerLoader(std::make_unique<AssimpModelLoader>());
    registerLoader(std::make_unique<GltfModelLoader>());
    registerLoader(std::make_unique<ObjModelLoader>());
}

void ModelLoaderRegistry::registerLoader(std::unique_ptr<IModelLoader> loader)
{
    if (!loader) return;
    // Resolution walks the list in reverse so the most recently registered
    // loader wins; that is what "override a built-in format in a test" needs.
    m_loaders.push_back(std::move(loader));
}

const IModelLoader *ModelLoaderRegistry::find(const std::string &path) const
{
    const std::string extension = lowerExtension(path);
    if (extension.empty()) return nullptr;
    for (auto it = m_loaders.rbegin(); it != m_loaders.rend(); ++it) {
        for (const std::string &candidate : (*it)->extensions()) {
            if (candidate == extension) return it->get();
        }
    }
    return nullptr;
}

const IModelLoader *ModelLoaderRegistry::findByName(const std::string &name) const
{
    for (const auto &loader : m_loaders) {
        if (name == loader->name()) return loader.get();
    }
    return nullptr;
}

bool ModelLoaderRegistry::supports(const std::string &path) const
{
    return find(path) != nullptr;
}

std::vector<std::string> ModelLoaderRegistry::supportedExtensions() const
{
    std::vector<std::string> result;
    for (const auto &loader : m_loaders) {
        for (const std::string &extension : loader->extensions()) {
            result.push_back(extension);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

Model ModelLoaderRegistry::load(const std::string &path, const ModelLoadOptions &options) const
{
    const IModelLoader *loader = nullptr;
    if (!options.preferredLoader.empty()) {
        loader = findByName(options.preferredLoader);
        if (!loader) {
            throw std::runtime_error("ModelLoader: unknown loader '" + options.preferredLoader + "'");
        }
    } else {
        loader = find(path);
    }
    if (!loader) {
        std::string supported;
        for (const std::string &extension : supportedExtensions()) {
            if (!supported.empty()) supported += ", ";
            supported += "." + extension;
        }
        std::string message = "ModelLoader: no loader for '" + path + "' (supported: " + supported + ")";
        const std::string assimpProblem = AssimpModelLoader::runtimeDiagnostic();
        if (!assimpProblem.empty()) {
            message += "; assimp formats need its runtime, " + assimpProblem;
        }
        throw std::runtime_error(message);
    }

    Model model = loader->load(path, options);
    const PostProcessPlan plan = mergePlan(*loader, options, path);

    const size_t verticesBefore = model.vertices.size();
    const size_t indicesBefore = model.indices.size();
    uint32_t generatedNormals = 0;
    uint32_t generatedTangents = 0;

    // Order matters:
    //   flatten    first, so every later step sees world-space geometry and the
    //              bounds normalizeSize reads are the ones a renderer draws.
    //   dedup      while the attribute set is still exactly what the file
    //              carried -- generated attributes would make vertices
    //              that are the same in the file look different.
    //   flip V     before tangents, which are derived from UV derivatives.
    //   normals    before tangents, which are orthogonalised against them.
    //   transforms last, they move positions but keep topology.
    if (options.flattenScene) model_post::flattenScene(model);
    if (plan.deduplicate) model_post::deduplicateVertices(model);
    if (plan.flipTexCoordV) model_post::flipTexCoordV(model);
    if (plan.generateNormals) {
        for (const Mesh &mesh : model.meshes) {
            for (const Primitive &primitive : mesh.primitives) {
                if (!primitive.has(VertexAttribute::eNormal)) ++generatedNormals;
            }
        }
        model_post::generateMissingNormals(model);
    }
    if (plan.generateTangents) {
        for (const Mesh &mesh : model.meshes) {
            for (const Primitive &primitive : mesh.primitives) {
                if (!primitive.has(VertexAttribute::eTangent)) ++generatedTangents;
            }
        }
        model_post::generateMissingTangents(model);
    }
    if (options.scale != 1.0f) model_post::scalePositions(model, options.scale);
    if (options.normalizeSize) {
        model_post::normalizeSize(model, 2.0f);
    } else if (options.centerAtOrigin) {
        model_post::centerAtOrigin(model);
    }
    model.recomputeBounds();

    if (options.log) {
        std::cout << "[model] " << loader->name() << ": " << path << " -> " << model.vertices.size() << " vertices ("
                  << verticesBefore << " before dedup), " << model.indices.size() << " indices (" << indicesBefore
                  << "), " << model.triangleCount() << " triangles, " << model.meshes.size() << " mesh(es), "
                  << model.materials.size() << " material(s), " << model.images.size() << " image(s), "
                  << model.instanceCount() << " node instance(s), " << generatedNormals << " normal set(s) and "
                  << generatedTangents << " tangent set(s) generated" << std::endl;
    }
    return model;
}

Model loadModel(const std::string &path, const ModelLoadOptions &options)
{
    return ModelLoaderRegistry::instance().load(path, options);
}
