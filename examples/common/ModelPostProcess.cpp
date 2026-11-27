#include "ModelPostProcess.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace {

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
// unordered_map::operator[] would accumulate into garbage.
void accumulate(std::unordered_map<uint32_t, glm::vec3> &sums, uint32_t index, const glm::vec3 &value)
{
    auto it = sums.find(index);
    if (it == sums.end()) {
        sums.emplace(index, value);
    } else {
        it->second += value;
    }
}

uint32_t missingAttributeSets(const Model &model, uint32_t attribute)
{
    uint32_t count = 0;
    for (const Mesh &mesh : model.meshes) {
        for (const Primitive &primitive : mesh.primitives) {
            if (!primitive.has(attribute)) ++count;
        }
    }
    return count;
}

} // namespace

namespace model_post {

void flattenScene(Model &model)
{
    if (model.meshes.empty() || model.indices.empty()) return;

    // Baking node matrices into vertices is incompatible with skinning.
    if (model.isSkinned()) return;

    std::vector<MeshVertex> outVertices;
    std::vector<uint32_t> outIndices;
    std::vector<Mesh> outMeshes;
    std::vector<ModelNode> outNodes;
    outVertices.reserve(model.vertices.size());
    outIndices.reserve(model.indices.size());

    std::vector<bool> visited(model.meshes.size(), false);
    int openNodeIndex = -2;
    Mesh openMesh;

    forEachMeshInstance(model, [&](const glm::mat4 &world, int nodeIndex, const Mesh &mesh,
                                   const Primitive &primitive) {
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
        if (remapped.indexCount > 0) openMesh.primitives.push_back(remapped);
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
    for (size_t i = 0; i < model.nodes.size(); ++i) model.rootNodes.push_back(static_cast<int>(i));
    model.recomputeBounds();
}

void deduplicateVertices(Model &model)
{
    if (model.meshes.empty() || model.indices.empty()) return;
    // Merging skinned vertices is a bug, not an optimisation: bone sets differ.
    if (model.isSkinned()) return;

    std::vector<MeshVertex> outVertices;
    std::vector<uint32_t> outIndices;
    outVertices.reserve(model.vertices.size());
    outIndices.reserve(model.indices.size());

    for (Mesh &mesh : model.meshes) {
        for (Primitive &primitive : mesh.primitives) {
            const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                    static_cast<uint32_t>(model.indices.size()));
            const uint32_t newFirstIndex = static_cast<uint32_t>(outIndices.size());
            std::unordered_map<uint32_t, uint32_t> oldToNew;
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
            std::unordered_map<uint32_t, glm::vec3> sums;
            for (uint32_t i = primitive.firstIndex; i + 2 < end; i += 3) {
                const uint32_t i0 = model.indices[i];
                const uint32_t i1 = model.indices[i + 1];
                const uint32_t i2 = model.indices[i + 2];
                if (i0 >= model.vertices.size() || i1 >= model.vertices.size() || i2 >= model.vertices.size())
                    continue;
                const glm::vec3 &p0 = model.vertices[i0].position;
                const glm::vec3 &p1 = model.vertices[i1].position;
                const glm::vec3 &p2 = model.vertices[i2].position;
                const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
                if (glm::dot(faceNormal, faceNormal) <= 0.0f) continue;
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
            if (!primitive.has(VertexAttribute::eNormal) || !primitive.has(VertexAttribute::eTexCoord)) continue;
            const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                    static_cast<uint32_t>(model.indices.size()));
            std::unordered_map<uint32_t, glm::vec3> sums;
            for (uint32_t i = primitive.firstIndex; i + 2 < end; i += 3) {
                const uint32_t i0 = model.indices[i];
                const uint32_t i1 = model.indices[i + 1];
                const uint32_t i2 = model.indices[i + 2];
                if (i0 >= model.vertices.size() || i1 >= model.vertices.size() || i2 >= model.vertices.size())
                    continue;
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
    for (MeshVertex &vertex : model.vertices) vertex.texCoord.y = 1.0f - vertex.texCoord.y;
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

// --- Pipeline ---------------------------------------------------------------

std::vector<std::string> Pipeline::stepNames() const
{
    std::vector<std::string> names;
    names.reserve(m_steps.size());
    for (const Step &step : m_steps) names.push_back(step.name);
    return names;
}

std::vector<StepResult> Pipeline::run(Model &model, const ModelLoadOptions &options,
                                      const PostProcessPlan &plan) const
{
    std::vector<StepResult> results;
    for (const Step &step : m_steps) {
        if (step.enabled && !step.enabled(options, plan)) continue;
        StepResult result;
        result.name = step.name;
        if (step.apply) result.detail = step.apply(model, options, plan);
        results.push_back(std::move(result));
    }
    return results;
}

Pipeline makeDefaultPipeline(const IModelLoader &loader)
{
    Pipeline pipeline;

    // Order is now explicit data.  The invariants it protects:
    //   flatten  first, so bounds describe what the renderer draws.
    //   dedup    while attributes are still exactly what the file carried.
    //   flipV    before tangents (derived from UV derivatives).
    //   normals  before tangents (orthogonalised against them).
    //   scale    last among geometry moves; normalize/center use fresh bounds.
    pipeline.add({"flatten",
                  [](const ModelLoadOptions &o, const PostProcessPlan &) { return o.flattenScene; },
                  [](Model &m, const ModelLoadOptions &, const PostProcessPlan &) -> std::string {
                      if (m.isSkinned()) return "skipped (skinned)";
                      flattenScene(m);
                      return {};
                  }});
    pipeline.add({"deduplicate",
                  [](const ModelLoadOptions &, const PostProcessPlan &p) { return p.deduplicate; },
                  [](Model &m, const ModelLoadOptions &, const PostProcessPlan &) -> std::string {
                      deduplicateVertices(m);
                      return {};
                  }});
    pipeline.add({"flipV",
                  [](const ModelLoadOptions &, const PostProcessPlan &p) { return p.flipTexCoordV; },
                  [](Model &m, const ModelLoadOptions &, const PostProcessPlan &) -> std::string {
                      flipTexCoordV(m);
                      return {};
                  }});
    pipeline.add({"normals",
                  [](const ModelLoadOptions &, const PostProcessPlan &p) { return p.generateNormals; },
                  [](Model &m, const ModelLoadOptions &, const PostProcessPlan &) -> std::string {
                      const uint32_t sets = missingAttributeSets(m, VertexAttribute::eNormal);
                      generateMissingNormals(m);
                      return std::to_string(sets) + " set(s)";
                  }});
    pipeline.add({"tangents",
                  [](const ModelLoadOptions &, const PostProcessPlan &p) { return p.generateTangents; },
                  [](Model &m, const ModelLoadOptions &, const PostProcessPlan &) -> std::string {
                      const uint32_t sets = missingAttributeSets(m, VertexAttribute::eTangent);
                      generateMissingTangents(m);
                      return std::to_string(sets) + " set(s)";
                  }});
    pipeline.add({"scale",
                  [](const ModelLoadOptions &o, const PostProcessPlan &) { return o.scale != 1.0f; },
                  [](Model &m, const ModelLoadOptions &o, const PostProcessPlan &) -> std::string {
                      scalePositions(m, o.scale);
                      return {};
                  }});
    pipeline.add({"normalize",
                  [](const ModelLoadOptions &o, const PostProcessPlan &) { return o.normalizeSize; },
                  [](Model &m, const ModelLoadOptions &, const PostProcessPlan &) -> std::string {
                      normalizeSize(m, 2.0f);
                      return "longest axis -> 2.0";
                  }});
    pipeline.add({"center",
                  [](const ModelLoadOptions &o, const PostProcessPlan &) { return !o.normalizeSize && o.centerAtOrigin; },
                  [](Model &m, const ModelLoadOptions &, const PostProcessPlan &) -> std::string {
                      centerAtOrigin(m);
                      return {};
                  }});
    pipeline.add({"bounds", nullptr,
                  [](Model &m, const ModelLoadOptions &, const PostProcessPlan &) -> std::string {
                      m.recomputeBounds();
                      return {};
                  }});

    // Template Method hook: the format gets the last word, and the log shows it.
    pipeline.add({"loader-hook:" + std::string(loader.name()), nullptr,
                  [&loader](Model &m, const ModelLoadOptions &o, const PostProcessPlan &) -> std::string {
                      loader.postProcess(m, o);
                      return {};
                  }});

    return pipeline;
}

} // namespace model_post
