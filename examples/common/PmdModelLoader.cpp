#include "PmdModelLoader.h"

#include "ModelBinaryReader.h"
#include "ModelBuilder.h"
#include "ModelFormatsCommon.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

struct PmdVertex {
    glm::vec3 position{};
    glm::vec3 normal{};
    glm::vec2 uv{};
    uint16_t bone0 = 0;
    uint16_t bone1 = 0;
    float weight0 = 1.0f; // 0..1, bone1 gets 1 - weight0
};

struct PmdMaterial {
    glm::vec3 diffuse{};
    float alpha = 1.0f;
    float shininess = 0.0f;
    glm::vec3 specular{};
    glm::vec3 ambient{};
    uint32_t faceVertexCount = 0;
    std::string textureName;
};

struct PmdBone {
    std::string name;
    int32_t parent = -1;
    glm::vec3 position{};
};

// PMD texture fields can be "diffuse.png*sphere.spa"; only the first entry is
// a colour map, the rest are sphere/toon overlays.
std::string primaryTexture(const std::string &field)
{
    const size_t star = field.find('*');
    return star == std::string::npos ? field : field.substr(0, star);
}

} // namespace

int PmdModelLoader::probe(const uint8_t *data, size_t size) const
{
    return modelio::startsWith(data, size, "Pmd") ? 100 : 0;
}

PostProcessPlan PmdModelLoader::defaultPlan(const std::string &) const
{
    PostProcessPlan plan;
    plan.deduplicate = true;
    plan.flipTexCoordV = false; // MMD UVs are top-left, like Vulkan
    plan.generateNormals = true;
    plan.generateTangents = true;
    return plan;
}

void PmdModelLoader::postProcess(Model &model, const ModelLoadOptions &) const
{
    modelformats::finalizeMaterials(model);
}

Model PmdModelLoader::load(const std::string &path, const ModelLoadOptions &options) const
{
    const std::vector<uint8_t> bytes = modelio::readBinaryFile(path);
    modelio::BinaryReader r(bytes.data(), bytes.size());

    r.require(3);
    if (std::memcmp(r.data(), "Pmd", 3) != 0) throw std::runtime_error("PMD: bad magic in '" + path + "'");
    r.skip(3);
    const float version = r.f32();
    if (version != 1.0f) throw std::runtime_error("PMD: unsupported version " + std::to_string(version));
    const std::string modelName = r.fixedString(20);
    r.fixedString(256); // comment

    const uint32_t vertexCount = r.u32();
    if (vertexCount > 10000000) throw std::runtime_error("PMD: implausible vertex count");
    std::vector<PmdVertex> vertices(vertexCount);
    for (PmdVertex &vertex : vertices) {
        vertex.position = r.vec3();
        vertex.normal = r.vec3();
        vertex.uv = r.vec2();
        vertex.bone0 = r.u16();
        vertex.bone1 = r.u16();
        vertex.weight0 = static_cast<float>(r.u8()) / 100.0f;
        r.u8(); // edge flag
    }

    const uint32_t faceVertexCount = r.u32();
    if (faceVertexCount % 3 != 0) throw std::runtime_error("PMD: bad face vertex count");
    std::vector<uint32_t> faces(faceVertexCount);
    for (uint32_t &index : faces) index = r.u16();

    const uint32_t materialCount = r.u32();
    std::vector<PmdMaterial> materials(materialCount);
    for (PmdMaterial &material : materials) {
        material.diffuse = r.vec3();
        material.alpha = r.f32();
        material.shininess = r.f32();
        material.specular = r.vec3();
        material.ambient = r.vec3();
        r.u8(); // toon index
        r.u8(); // edge flag
        material.faceVertexCount = r.u32();
        material.textureName = primaryTexture(r.fixedString(20));
    }

    const uint16_t boneCount = r.u16();
    std::vector<PmdBone> bones(boneCount);
    for (PmdBone &bone : bones) {
        bone.name = r.fixedString(20);
        const uint16_t parent = r.u16();
        bone.parent = parent == 0xFFFF ? -1 : static_cast<int32_t>(parent);
        r.u16(); // tail bone
        r.u8();  // bone type
        r.u16(); // IK parent
        bone.position = r.vec3();
    }
    // IK / morph / display-list / physics extensions are deliberately not read:
    // geometry, materials and the bone tree are complete at this point.

    ModelBuilder builder;
    const size_t meshIndex = builder.addMesh(modelName.empty() ? "PMDMesh" : modelName);
    for (const PmdVertex &vertex : vertices) {
        MeshVertex out;
        out.position = vertex.position;
        out.normal = vertex.normal;
        out.texCoord = vertex.uv;
        builder.addVertex(out);
    }

    const std::string baseDir = modelformats::baseDirectoryFor(path, options);
    uint32_t cursor = 0;
    for (size_t i = 0; i < materials.size(); ++i) {
        const PmdMaterial &source = materials[i];
        Material material = modelformats::makePhongMaterial(
            "material" + std::to_string(i), glm::vec4(source.diffuse, source.alpha),
            source.specular, source.shininess, true);
        const int materialIndex = builder.addMaterial(material);
        const int textureIndex = modelformats::loadSideCarTexture(builder.model(), baseDir, source.textureName);
        if (textureIndex >= 0) builder.model().materials[static_cast<size_t>(materialIndex)].baseColorTexture = textureIndex;

        builder.addPrimitive(meshIndex, materialIndex,
                             VertexAttribute::ePosition | VertexAttribute::eNormal | VertexAttribute::eTexCoord);
        uint32_t count = source.faceVertexCount;
        if (i + 1 == materials.size()) count = static_cast<uint32_t>(faces.size()) - cursor;
        count = std::min<uint32_t>(count, static_cast<uint32_t>(faces.size()) - cursor);
        for (uint32_t k = 0; k < count; ++k) builder.addIndex(faces[cursor + k]);
        cursor += count;
    }
    if (materials.empty() && !faces.empty()) {
        builder.addPrimitive(meshIndex, -1,
                             VertexAttribute::ePosition | VertexAttribute::eNormal | VertexAttribute::eTexCoord);
        for (uint32_t index : faces) builder.addIndex(index);
    }

    builder.addNode("mesh", glm::mat4(1.0f), static_cast<int>(meshIndex));
    std::vector<int> boneNodes(bones.size(), -1);
    std::vector<Joint> joints;
    for (size_t i = 0; i < bones.size(); ++i) {
        const int parent = bones[i].parent >= 0 && static_cast<size_t>(bones[i].parent) < bones.size()
                               ? boneNodes[static_cast<size_t>(bones[i].parent)] : -1;
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), bones[i].position);
        boneNodes[i] = builder.addNode(bones[i].name, transform, -1, parent);
        Joint joint;
        joint.name = bones[i].name;
        joint.nodeIndex = boneNodes[i];
        joint.inverseBindMatrix = glm::translate(glm::mat4(1.0f), -bones[i].position);
        joints.push_back(std::move(joint));
    }
    if (!joints.empty()) {
        builder.setJoints(joints);
        for (const PmdVertex &vertex : vertices) {
            builder.addBoneData(glm::uvec4(vertex.bone0, vertex.bone1, 0u, 0u),
                                glm::vec4(vertex.weight0, 1.0f - vertex.weight0, 0.0f, 0.0f));
        }
    }

    return builder.build();
}
