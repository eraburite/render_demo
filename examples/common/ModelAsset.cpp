#include "ModelAsset.h"

#include <algorithm>
#include <limits>

uint32_t Model::instanceCount() const
{
    uint32_t count = 0;
    for (const ModelNode &node : nodes) {
        if (node.meshIndex >= 0 && node.meshIndex < static_cast<int>(meshes.size())) {
            ++count;
        }
    }
    return count;
}

void Model::recomputeBounds()
{
    if (vertices.empty()) {
        minBounds = glm::vec3(0.0f);
        maxBounds = glm::vec3(0.0f);
        return;
    }
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (const MeshVertex &vertex : vertices) {
        lo = glm::min(lo, vertex.position);
        hi = glm::max(hi, vertex.position);
    }
    minBounds = lo;
    maxBounds = hi;
}

std::vector<glm::mat4> computeNodeWorldMatrices(const Model &model)
{
    std::vector<glm::mat4> locals;
    locals.reserve(model.nodes.size());
    for (const ModelNode &node : model.nodes) {
        locals.push_back(node.transform);
    }
    return computeNodeWorldMatrices(model, locals);
}

std::vector<glm::mat4> computeNodeWorldMatrices(const Model &model, const std::vector<glm::mat4> &localTransforms)
{
    std::vector<glm::mat4> world(model.nodes.size(), glm::mat4(1.0f));
    std::vector<bool> visited(model.nodes.size(), false);

    // Iterative DFS: a malformed file can nest deeply enough to threaten the
    // sample's 1 MB default stack, and recursion buys nothing here.
    struct Frame {
        int index;
        glm::mat4 parent;
    };
    std::vector<Frame> stack;
    for (auto it = model.rootNodes.rbegin(); it != model.rootNodes.rend(); ++it) {
        stack.push_back({*it, glm::mat4(1.0f)});
    }

    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (frame.index < 0 || frame.index >= static_cast<int>(model.nodes.size())) continue;
        if (visited[frame.index]) continue;
        visited[frame.index] = true;
        const ModelNode &node = model.nodes[frame.index];
        // A caller that animates a subset of nodes passes a list that may be
        // shorter than the node array; the rest keep their static transform.
        const glm::mat4 local =
            frame.index < static_cast<int>(localTransforms.size()) ? localTransforms[frame.index] : node.transform;
        world[frame.index] = frame.parent * local;
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            stack.push_back({*it, world[frame.index]});
        }
    }
    return world;
}

void computeWorldBounds(const Model &model, glm::vec3 &outMin, glm::vec3 &outMax)
{
    outMin = glm::vec3(0.0f);
    outMax = glm::vec3(0.0f);
    if (model.vertices.empty()) {
        return;
    }

    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    uint32_t counted = 0;
    forEachMeshInstance(model, [&](const glm::mat4 &world, int, const Mesh &, const Primitive &primitive) {
        const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                static_cast<uint32_t>(model.indices.size()));
        for (uint32_t i = primitive.firstIndex; i < end; ++i) {
            const uint32_t index = model.indices[i];
            if (index >= model.vertices.size()) continue;
            const glm::vec3 position = glm::vec3(world * glm::vec4(model.vertices[index].position, 1.0f));
            lo = glm::min(lo, position);
            hi = glm::max(hi, position);
            ++counted;
        }
    });

    if (counted == 0) {
        return;
    }
    outMin = lo;
    outMax = hi;
}


void forEachMeshInstance(const Model &model, const MeshInstanceCallback &fn)
{
    if (!fn) return;
    const std::vector<glm::mat4> world = computeNodeWorldMatrices(model);
    std::vector<bool> visited(model.nodes.size(), false);

    struct Frame {
        int index;
    };
    std::vector<Frame> stack;
    for (auto it = model.rootNodes.rbegin(); it != model.rootNodes.rend(); ++it) {
        stack.push_back({*it});
    }

    while (!stack.empty()) {
        const int index = stack.back().index;
        stack.pop_back();
        if (index < 0 || index >= static_cast<int>(model.nodes.size())) continue;
        if (visited[index]) continue;
        visited[index] = true;

        const ModelNode &node = model.nodes[index];
        if (node.meshIndex >= 0 && node.meshIndex < static_cast<int>(model.meshes.size())) {
            const Mesh &mesh = model.meshes[node.meshIndex];
            for (const Primitive &primitive : mesh.primitives) {
                fn(world[index], index, mesh, primitive);
            }
        }
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            stack.push_back({*it});
        }
    }
}
