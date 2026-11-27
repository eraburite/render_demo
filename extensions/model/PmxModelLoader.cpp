#include "PmxModelLoader.h"

#include "ModelBinaryReader.h"
#include "ModelBuilder.h"
#include "ModelFormatsCommon.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

struct PmxVertex {
    glm::vec3 position{};
    glm::vec3 normal{};
    glm::vec2 uv{};
    glm::uvec4 boneIndices{0u};
    glm::vec4 boneWeights{0.0f};
};

struct PmxMaterial {
    std::string name;
    glm::vec4 diffuse{};
    glm::vec3 specular{};
    float shininess = 0.0f;
    glm::vec3 ambient{};
    uint8_t drawMode = 0;
    int32_t textureIndex = -1;
    int32_t sphereTextureIndex = -1;
    uint8_t sphereMode = 0;
    uint8_t toonMode = 0;
    int32_t toonTextureIndex = -1;
    int32_t faceVertexCount = 0;
    std::string textureName;
};

struct PmxBone {
    std::string name;
    glm::vec3 position{};
    int32_t parent = -1;
    uint16_t flags = 0;
};

enum : uint16_t {
    kBoneTargetShowMode = 0x0001,
    kBoneIK = 0x0020,
    kBoneAppendRotate = 0x0100,
    kBoneAppendTranslate = 0x0200,
    kBoneFixedAxis = 0x0400,
    kBoneLocalAxis = 0x0800,
    kBoneExternalParent = 0x2000,
};

// PMX 2.1 QDEF is identical in layout to BDEF4 but is kept distinct so the
// log can report which deformation types a real file actually used.
struct WeightStats {
    uint32_t bdef1 = 0, bdef2 = 0, bdef4 = 0, sdef = 0, qdef = 0;
};

WeightStats readVertexWeights(modelio::BinaryReader &r, uint8_t indexSize, PmxVertex &vertex)
{
    WeightStats stats;
    const uint8_t type = r.u8();
    auto readIndex = [&](uint8_t size) { return r.signedIndex(size); };
    switch (type) {
    case 0: { // BDEF1
        const int32_t b0 = readIndex(indexSize);
        vertex.boneIndices = glm::uvec4(static_cast<uint32_t>(std::max(b0, 0)), 0u, 0u, 0u);
        vertex.boneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        stats.bdef1 = 1;
        break;
    }
    case 1: { // BDEF2
        const int32_t b0 = readIndex(indexSize);
        const int32_t b1 = readIndex(indexSize);
        const float w0 = r.f32();
        vertex.boneIndices = glm::uvec4(static_cast<uint32_t>(std::max(b0, 0)),
                                        static_cast<uint32_t>(std::max(b1, 0)), 0u, 0u);
        vertex.boneWeights = glm::vec4(w0, 1.0f - w0, 0.0f, 0.0f);
        stats.bdef2 = 1;
        break;
    }
    case 2: { // BDEF4
        std::array<uint32_t, 4> indices{};
        glm::vec4 weights{};
        for (int i = 0; i < 4; ++i) indices[i] = static_cast<uint32_t>(std::max(readIndex(indexSize), 0));
        for (int i = 0; i < 4; ++i) weights[i] = r.f32();
        vertex.boneIndices = glm::uvec4(indices[0], indices[1], indices[2], indices[3]);
        vertex.boneWeights = weights;
        stats.bdef4 = 1;
        break;
    }
    case 3: { // SDEF: use the BDEF2 part, skip C/R0/R1
        const int32_t b0 = readIndex(indexSize);
        const int32_t b1 = readIndex(indexSize);
        const float w0 = r.f32();
        r.vec3(); r.vec3(); r.vec3();
        vertex.boneIndices = glm::uvec4(static_cast<uint32_t>(std::max(b0, 0)),
                                        static_cast<uint32_t>(std::max(b1, 0)), 0u, 0u);
        vertex.boneWeights = glm::vec4(w0, 1.0f - w0, 0.0f, 0.0f);
        stats.sdef = 1;
        break;
    }
    case 4: { // QDEF (PMX 2.1)
        std::array<uint32_t, 4> indices{};
        glm::vec4 weights{};
        for (int i = 0; i < 4; ++i) indices[i] = static_cast<uint32_t>(std::max(readIndex(indexSize), 0));
        for (int i = 0; i < 4; ++i) weights[i] = r.f32();
        vertex.boneIndices = glm::uvec4(indices[0], indices[1], indices[2], indices[3]);
        vertex.boneWeights = weights;
        stats.qdef = 1;
        break;
    }
    default:
        throw std::runtime_error("PMX: unknown vertex weight type " + std::to_string(type));
    }
    return stats;
}

} // namespace

int PmxModelLoader::probe(const uint8_t *data, size_t size) const
{
    return modelio::startsWith(data, size, "PMX ") ? 100 : 0;
}

PostProcessPlan PmxModelLoader::defaultPlan(const std::string &) const
{
    PostProcessPlan plan;
    plan.deduplicate = true;      // no-op for skinned models (see model_post)
    plan.flipTexCoordV = false;   // MMD UVs are already top-left, like Vulkan
    plan.generateNormals = true;  // no-op: PMX vertices carry normals
    plan.generateTangents = true;
    return plan;
}

void PmxModelLoader::postProcess(Model &model, const ModelLoadOptions &) const
{
    modelformats::finalizeMaterials(model);
}

Model PmxModelLoader::load(const std::string &path, const ModelLoadOptions &options) const
{
    const std::vector<uint8_t> bytes = modelio::readBinaryFile(path);
    modelio::BinaryReader r(bytes.data(), bytes.size());

    // --- header -------------------------------------------------------------
    r.require(4);
    if (std::memcmp(r.data(), "PMX ", 4) != 0) throw std::runtime_error("PMX: bad magic in '" + path + "'");
    r.skip(4);
    const float version = r.f32();
    if (version < 2.0f || version > 2.1f + 0.001f) {
        throw std::runtime_error("PMX: unsupported version " + std::to_string(version));
    }
    const uint8_t dataSize = r.u8();
    if (dataSize < 8) throw std::runtime_error("PMX: header data size < 8");
    const uint8_t encode = r.u8();
    const uint8_t addUVNum = r.u8();
    const uint8_t vertexIndexSize = r.u8();
    const uint8_t textureIndexSize = r.u8();
    /*materialIndexSize*/ r.u8();
    const uint8_t boneIndexSize = r.u8();
    /*morphIndexSize*/ r.u8();
    /*rigidBodyIndexSize*/ r.u8();
    if (dataSize > 8) r.skip(dataSize - 8); // forward-compatible header extension
    const bool utf16 = (encode == 0);
    auto string = [&]() { return r.lengthString(utf16); };

    // --- info ---------------------------------------------------------------
    const std::string modelName = string();
    string(); string(); string(); // English name / comments, unused

    // --- vertices -----------------------------------------------------------
    const int32_t vertexCount = r.i32();
    if (vertexCount < 0 || vertexCount > 10000000) throw std::runtime_error("PMX: implausible vertex count");
    std::vector<PmxVertex> vertices(static_cast<size_t>(vertexCount));
    WeightStats stats;
    for (PmxVertex &vertex : vertices) {
        vertex.position = r.vec3();
        vertex.normal = r.vec3();
        vertex.uv = r.vec2();
        for (uint8_t i = 0; i < addUVNum; ++i) r.vec4();
        const WeightStats one = readVertexWeights(r, boneIndexSize, vertex);
        stats.bdef1 += one.bdef1; stats.bdef2 += one.bdef2; stats.bdef4 += one.bdef4;
        stats.sdef += one.sdef; stats.qdef += one.qdef;
        r.f32(); // edge magnification
    }

    // --- faces --------------------------------------------------------------
    const int32_t faceVertexCount = r.i32();
    if (faceVertexCount < 0 || faceVertexCount % 3 != 0) throw std::runtime_error("PMX: bad face vertex count");
    std::vector<uint32_t> faces(static_cast<size_t>(faceVertexCount));
    for (uint32_t &index : faces) index = r.unsignedIndex(vertexIndexSize);

    // --- textures -----------------------------------------------------------
    const int32_t textureCount = r.i32();
    std::vector<std::string> textureNames;
    textureNames.reserve(static_cast<size_t>(std::max(textureCount, 0)));
    for (int32_t i = 0; i < textureCount; ++i) textureNames.push_back(string());

    // --- materials ----------------------------------------------------------
    const int32_t materialCount = r.i32();
    std::vector<PmxMaterial> materials(static_cast<size_t>(std::max(materialCount, 0)));
    for (PmxMaterial &material : materials) {
        material.name = string();
        string(); // English name
        material.diffuse = r.vec4();
        material.specular = r.vec3();
        material.shininess = r.f32();
        material.ambient = r.vec3();
        material.drawMode = r.u8();
        r.vec4(); // edge colour
        r.f32();  // edge size
        material.textureIndex = r.signedIndex(textureIndexSize);
        material.sphereTextureIndex = r.signedIndex(textureIndexSize);
        material.sphereMode = r.u8();
        material.toonMode = r.u8();
        if (material.toonMode == 0) material.toonTextureIndex = r.signedIndex(textureIndexSize);
        else material.toonTextureIndex = r.u8();
        string(); // memo
        material.faceVertexCount = r.i32();
        if (material.textureIndex >= 0 && material.textureIndex < static_cast<int32_t>(textureNames.size())) {
            material.textureName = textureNames[static_cast<size_t>(material.textureIndex)];
        }
    }

    // --- bones --------------------------------------------------------------
    const int32_t boneCount = r.i32();
    std::vector<PmxBone> bones(static_cast<size_t>(std::max(boneCount, 0)));
    for (PmxBone &bone : bones) {
        bone.name = string();
        string(); // English name
        bone.position = r.vec3();
        bone.parent = r.signedIndex(boneIndexSize);
        r.i32();  // deform depth
        bone.flags = r.u16();
        if ((bone.flags & kBoneTargetShowMode) == 0) r.vec3();
        else r.signedIndex(boneIndexSize);
        if (bone.flags & (kBoneAppendRotate | kBoneAppendTranslate)) {
            r.signedIndex(boneIndexSize);
            r.f32();
        }
        if (bone.flags & kBoneFixedAxis) r.vec3();
        if (bone.flags & kBoneLocalAxis) { r.vec3(); r.vec3(); }
        if (bone.flags & kBoneExternalParent) r.i32();
        if (bone.flags & kBoneIK) {
            r.signedIndex(boneIndexSize);
            r.i32();  // iteration count
            r.f32();  // limit angle
            const int32_t linkCount = r.i32();
            for (int32_t i = 0; i < linkCount; ++i) {
                r.signedIndex(boneIndexSize);
                const uint8_t enableLimit = r.u8();
                if (enableLimit != 0) { r.vec3(); r.vec3(); }
            }
        }
    }
    // Morph / display / rigid body / joint sections follow; geometry, materials
    // and the bone tree are complete, so the parser intentionally stops here.

    // --- build Model --------------------------------------------------------
    ModelBuilder builder;
    const size_t meshIndex = builder.addMesh(modelName.empty() ? "PMXMesh" : modelName);
    for (const PmxVertex &vertex : vertices) {
        MeshVertex out;
        out.position = vertex.position;
        out.normal = vertex.normal;
        out.texCoord = vertex.uv;
        builder.addVertex(out);
    }

    const std::string baseDir = modelformats::baseDirectoryFor(path, options);
    uint32_t cursor = 0;
    for (size_t i = 0; i < materials.size(); ++i) {
        const PmxMaterial &source = materials[i];
        Material material = modelformats::makePhongMaterial(
            source.name.empty() ? "material" + std::to_string(i) : source.name,
            source.diffuse, source.specular, source.shininess,
            (source.drawMode & 0x01) != 0);
        const int materialIndex = builder.addMaterial(material);
        const int textureIndex = modelformats::loadSideCarTexture(builder.model(), baseDir, source.textureName);
        if (textureIndex >= 0) builder.model().materials[static_cast<size_t>(materialIndex)].baseColorTexture = textureIndex;

        const uint32_t attributes = VertexAttribute::ePosition | VertexAttribute::eNormal |
                                    VertexAttribute::eTexCoord;
        builder.addPrimitive(meshIndex, materialIndex, attributes);
        uint32_t count = source.faceVertexCount > 0 ? static_cast<uint32_t>(source.faceVertexCount) : 0;
        if (i + 1 == materials.size()) count = static_cast<uint32_t>(faces.size()) - cursor; // last one absorbs the rest
        count = std::min<uint32_t>(count, static_cast<uint32_t>(faces.size()) - cursor);
        for (uint32_t k = 0; k < count; ++k) builder.addIndex(faces[cursor + k]);
        cursor += count;
    }
    if (materials.empty() && !faces.empty()) {
        builder.addPrimitive(meshIndex, -1, VertexAttribute::ePosition | VertexAttribute::eNormal |
                                                VertexAttribute::eTexCoord);
        for (uint32_t index : faces) builder.addIndex(index);
    }

    // One node for the mesh, then one node per bone with the file's parent
    // links.  inverseBind = inverse(translate(bone position)) is exact for a
    // bind pose with no rotation, which is all PMX stores.
    const int meshNode = builder.addNode("mesh", glm::mat4(1.0f), static_cast<int>(meshIndex));
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
        for (const PmxVertex &vertex : vertices) builder.addBoneData(vertex.boneIndices, vertex.boneWeights);
    }
    (void)meshNode;
    (void)stats;

    return builder.build();
}
