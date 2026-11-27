#pragma once
#include "ModelAsset.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// Builder for the loader-neutral Model.
//
// The first three loaders each hand-assembled vertices/indices/meshes/nodes and
// had to re-derive the same invariant: every Primitive owns a contiguous,
// self-contained window of the index buffer.  Four more formats would have
// meant four more copies of that bookkeeping (and four chances to get the
// skinning parallel arrays out of step).  ModelBuilder owns it once.
//
// It is deliberately a *builder*, not a factory hierarchy: the product (Model)
// is one concrete value type, and the varying part is the sequence of append
// operations each file format performs.
// ---------------------------------------------------------------------------
class ModelBuilder {
public:
    size_t addMesh(const std::string &name)
    {
        Mesh mesh;
        mesh.name = name;
        m_model.meshes.push_back(std::move(mesh));
        return m_model.meshes.size() - 1;
    }

    // Starts a new primitive whose index window begins at the current end of
    // the index buffer.  Returns (meshIndex, primitiveIndex); pass them to
    // select() before appending indices.
    size_t addPrimitive(size_t meshIndex, int materialIndex, uint32_t attributes)
    {
        Primitive primitive;
        primitive.firstIndex = static_cast<uint32_t>(m_model.indices.size());
        primitive.indexCount = 0;
        primitive.materialIndex = materialIndex;
        primitive.attributes = attributes;
        Mesh &mesh = m_model.meshes.at(meshIndex);
        mesh.primitives.push_back(primitive);
        select(meshIndex, mesh.primitives.size() - 1);
        return mesh.primitives.size() - 1;
    }

    void select(size_t meshIndex, size_t primitiveIndex)
    {
        m_meshIndex = meshIndex;
        m_primitiveIndex = primitiveIndex;
    }

    uint32_t addVertex(const MeshVertex &vertex)
    {
        const uint32_t index = static_cast<uint32_t>(m_model.vertices.size());
        m_model.vertices.push_back(vertex);
        return index;
    }

    void addIndex(uint32_t vertexIndex)
    {
        m_model.indices.push_back(vertexIndex);
        current().indexCount++;
    }

    void addTriangle(uint32_t a, uint32_t b, uint32_t c)
    {
        addIndex(a);
        addIndex(b);
        addIndex(c);
    }

    int addMaterial(const Material &material)
    {
        m_model.materials.push_back(material);
        return static_cast<int>(m_model.materials.size()) - 1;
    }

    // Skinning: joints are declared once, then one 4-tuple per vertex in the
    // exact order addVertex() returned them.
    void setJoints(const std::vector<Joint> &joints)
    {
        Skin skin;
        skin.name = "skin0";
        skin.joints = joints;
        m_model.skins.clear();
        m_model.skins.push_back(std::move(skin));
    }

    void addBoneData(const glm::uvec4 &indices, const glm::vec4 &weights)
    {
        m_model.boneIndices.push_back(indices);
        m_model.boneWeights.push_back(weights);
    }

    int addNode(const std::string &name, const glm::mat4 &transform, int meshIndex, int parent = -1)
    {
        ModelNode node;
        node.name = name;
        node.transform = transform;
        node.meshIndex = meshIndex;
        const int index = static_cast<int>(m_model.nodes.size());
        m_model.nodes.push_back(std::move(node));
        if (parent >= 0 && static_cast<size_t>(parent) < m_model.nodes.size()) {
            m_model.nodes[static_cast<size_t>(parent)].children.push_back(index);
            m_hasExplicitRoots = true;
        } else {
            m_model.rootNodes.push_back(index);
        }
        return index;
    }

    // Direct access for a loader that needs a field the builder does not model.
    Model &model() { return m_model; }

    Model build()
    {
        if (m_hasExplicitRoots) {
            // rootNodes was also appended to for parentless nodes; that is
            // exactly what we want (every parentless node is a root).
        }
        // A skinned model must keep the parallel arrays aligned; a mismatch is
        // a loader bug, not something to silently render wrong.
        if (!m_model.boneIndices.empty() || !m_model.boneWeights.empty()) {
            if (m_model.boneIndices.size() != m_model.vertices.size() ||
                m_model.boneWeights.size() != m_model.vertices.size()) {
                throw std::runtime_error("ModelBuilder: skinning arrays are not parallel to vertices");
            }
        }
        m_model.recomputeBounds();
        return std::move(m_model);
    }

private:
    Primitive &current()
    {
        return m_model.meshes.at(m_meshIndex).primitives.at(m_primitiveIndex);
    }

    Model m_model;
    size_t m_meshIndex = 0;
    size_t m_primitiveIndex = 0;
    bool m_hasExplicitRoots = false;
};
