// skeletal_animation: play one animation clip on a skinned model.
// Reference: D:/LearnOpenGL/src/8.guest/2020/skeletal_animation
//            (skeletal_animation.cpp + anim_model.vs/fs, animator.h,
//             animation.h, bone.h, model_animation.h).
//
// What the port keeps: one dark-cleared, depth-tested pass; a shader that blends
// four joint matrices per vertex and samples a single diffuse map; a free-flying
// camera; a clip that loops for as long as the window is open.
//
// What it changes:
//   * the skeleton and the clips come out of the *same* import that produced the
//     geometry.  The reference builds the Model with one Assimp::Importer and
//     then re-imports the same file in Animation's constructor to rebuild the
//     node tree by hand; both halves now live in ModelAsset (skins + clips), so
//     there is no second parse and no second copy of the hierarchy;
//   * bone data reaches the shader as a uvec4/vec4 pair with unused slots at
//     index 0 / weight 0, instead of the reference's ivec4 with a -1 sentinel
//     plus an explicit skip;
//   * normalisation goes through the `model` matrix, not into the vertices (see
//     DESIGN.md §21.3) -- and the scene is never flattened, because the joint
//     hierarchy is exactly what a skinned draw needs;
//   * the camera is framed from the bind-pose world bounds instead of being
//     hard-coded, for the same reason as model_loading (§19.1).
//
// --time freezes the pose, which is what makes dumps comparable; without it the
// clip plays in real time.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ModelLoader.h"
#include "RenderCommon.h"
#include "SkeletonAnimator.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "ImageDump.h"
#include "IVertexInputStateGroup.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "ImageReadback.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "root_directory.h"

#include "shader/skeletal_frag.hpp"
#include "shader/skeletal_vert.hpp"

namespace {

// The vampire's clip drives 52 joints; 64 leaves room for the next asset without
// turning the per-frame block into a storage-buffer problem.  67 mat4 = 4288
// bytes, well inside the 64 KB uniform limit.
constexpr uint32_t kMaxJoints = 64;

// The engine's Vertex is a convenience atlas shared by every example; a skinned
// vertex needs two attributes nobody else uses, so the extra bytes stay local to
// this example instead of growing all 23 vertex buffers by 32 bytes a vertex.
struct SkinnedVertex {
    glm::vec3 position{};
    glm::vec2 texCoord{};
    // Unused influences are index 0 with weight 0, so the shader can sum all
    // four slots without a sentinel.
    glm::uvec4 boneIndices{0u};
    glm::vec4 boneWeights{0.0f};
};
// What matters is that the members are laid out in the order the attributes are
// declared, because that order *is* the location number.  The total size is not
// asserted: glm may put a 16-byte alignment pad in front of the vec4s, and the
// pipeline reads sizeof() anyway.
static_assert(offsetof(SkinnedVertex, position) < offsetof(SkinnedVertex, texCoord)
                  && offsetof(SkinnedVertex, texCoord) < offsetof(SkinnedVertex, boneIndices)
                  && offsetof(SkinnedVertex, boneIndices) < offsetof(SkinnedVertex, boneWeights),
              "vertex attribute order must match the shader's location order");

// Vertex input state for SkinnedVertex.  Locations follow the attribute order:
// 0 position, 1 uv, 2 bone indices, 3 bone weights -- the same order the
// generated shader declares.
class SkinnedVertexInputAtlas : public IVertexInputStateGroup {
public:
    SkinnedVertexInputAtlas(uint32_t binding, uint32_t vertexCount, uint32_t indexCount) :
        m_binding(binding), m_vertexCount(vertexCount), m_indexCount(indexCount)
    {
        rebuild();
    }

    SkinnedVertexInputAtlas(const SkinnedVertexInputAtlas &other) :
        m_binding(other.m_binding), m_vertexCount(other.m_vertexCount), m_indexCount(other.m_indexCount)
    {
        rebuild();
    }

    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputState() const override { return m_inputState; }
    uint32_t vertexByteSize() const override { return sizeof(SkinnedVertex); }
    uint32_t vertexCount() const override { return m_vertexCount; }
    uint32_t indexCount() const override { return m_indexCount; }
    DataTypes indexType() const override { return DataTypes::eUnsignedInt; }
    bool hasIndexBuffer() const override { return m_indexCount > 0; }
    bool isEmptyInputState() const override { return false; }
    uint32_t getVertexCount() const override { return m_vertexCount; }
    uint32_t drawInstanceCount() const override { return 1; }
    void setDrawInstanceCount(uint32_t) override {}
    std::shared_ptr<IVertexInputStateGroup> sharedClone() const override
    {
        return std::make_shared<SkinnedVertexInputAtlas>(*this);
    }

private:
    void rebuild()
    {
        m_bindings.clear();
        m_attributes.clear();

        vk::VertexInputBindingDescription bindingDesc{};
        bindingDesc.binding = m_binding;
        bindingDesc.stride = sizeof(SkinnedVertex);
        bindingDesc.inputRate = vk::VertexInputRate::eVertex;
        m_bindings.push_back(bindingDesc);

        const auto add = [&](uint32_t location, vk::Format format, uint32_t offset) {
            m_attributes.push_back(vk::VertexInputAttributeDescription(location, m_binding, format, offset));
        };
        add(0, vk::Format::eR32G32B32Sfloat, offsetof(SkinnedVertex, position));
        add(1, vk::Format::eR32G32Sfloat, offsetof(SkinnedVertex, texCoord));
        // Unsigned on purpose: a joint index is never -1 here, because slot
        // occupancy is encoded by the weight instead.
        add(2, vk::Format::eR32G32B32A32Uint, offsetof(SkinnedVertex, boneIndices));
        add(3, vk::Format::eR32G32B32A32Sfloat, offsetof(SkinnedVertex, boneWeights));

        m_inputState = vk::PipelineVertexInputStateCreateInfo{};
        m_inputState.setVertexBindingDescriptionCount(static_cast<uint32_t>(m_bindings.size()))
            .setPVertexBindingDescriptions(m_bindings.data())
            .setVertexAttributeDescriptionCount(static_cast<uint32_t>(m_attributes.size()))
            .setPVertexAttributeDescriptions(m_attributes.data());
    }

    uint32_t m_binding = 0;
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;
    std::vector<vk::VertexInputBindingDescription> m_bindings;
    std::vector<vk::VertexInputAttributeDescription> m_attributes;
    vk::PipelineVertexInputStateCreateInfo m_inputState{};
};

struct SkeletonBlock {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 joints[kMaxJoints];
};
static_assert(sizeof(SkeletonBlock) == (3 + kMaxJoints) * 64, "std140: three mat4 plus the joint array");

struct Options {
    std::string modelPath = PROJECT_ROOT_PATH "/data/models/vampire/dancing_vampire.dae";
    std::string loader; // "" = extension dispatch
    int clip = 0;
    bool hasTime = false;
    float time = 0.0f;
    bool staticPose = false;
    bool bindMesh = false;
    bool dump = false;
    uint32_t dumpAfter = 0;
    uint32_t resizeAfter = 0;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--static") {
            options.staticPose = true;
        } else if (arg == "--bind-mesh") {
            options.bindMesh = true;
        } else if (arg == "--model" && i + 1 < argc) {
            options.modelPath = argv[++i];
        } else if (arg == "--loader" && i + 1 < argc) {
            options.loader = argv[++i];
        } else if (arg == "--clip" && i + 1 < argc) {
            options.clip = std::stoi(argv[++i]);
        } else if (arg == "--time" && i + 1 < argc) {
            options.hasTime = true;
            options.time = std::stof(argv[++i]);
        } else if (arg == "--dump-after" && i + 1 < argc) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--resize-after" && i + 1 < argc) {
            options.resizeAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
    }
    return options;
}

// Camera framing, same idea as model_loading's: derive the eye from the bounds
// instead of hard-coding an asset-specific position.
//
// Two variants, because the two modes want different things:
//   frameBox     fits the bounding box per axis, so a wide-but-short box (this
//                character struts about two body widths sideways while it dances,
//                which makes its clip envelope 3.6 x 1.8) fills the frame instead
//                of being padded out to a sphere around its longest axis;
//   frameSphere  model_loading's exact formula, used by --bind-mesh so its dump
//                can be compared against model_loading's byte for byte (§21.5).
struct CameraFraming {
    glm::vec3 eye{0.0f, 0.0f, 0.0f};
    float pitchDegrees = 0.0f;
    float distance = 0.0f;
};

constexpr float kVerticalFov = 45.0f;
constexpr float kFillFraction = 0.85f;

CameraFraming frameBox(const glm::vec3 &low, const glm::vec3 &high, float aspect)
{
    const glm::vec3 size = glm::max(high - low, glm::vec3(0.001f));
    const glm::vec3 center = (low + high) * 0.5f;
    const glm::vec3 half = size * 0.5f;

    const float tanHalfFov = std::tan(glm::radians(kVerticalFov * 0.5f));
    const float distanceForHeight = half.y / (kFillFraction * tanHalfFov);
    const float distanceForWidth = half.x / (kFillFraction * tanHalfFov * aspect);
    // The depth has to fit at the *nearest* surface, not at the centre.
    const float distance = std::max(distanceForHeight, distanceForWidth) + half.z;

    CameraFraming framing;
    framing.distance = distance;
    framing.eye = center + glm::vec3(0.0f, 0.45f * half.y, distance);
    framing.pitchDegrees = glm::degrees(std::atan2(center.y - framing.eye.y, distance));
    return framing;
}

CameraFraming frameSphere(const glm::vec3 &low, const glm::vec3 &high)
{
    const glm::vec3 size = high - low;
    const glm::vec3 center = (low + high) * 0.5f;
    const float radius = std::max(0.001f, 0.5f * std::max({size.x, size.y, size.z}));

    const float halfFov = glm::radians(kVerticalFov * 0.5f);
    const float distance = radius / (kFillFraction * std::sin(halfFov)) + 0.5f * size.z;

    CameraFraming framing;
    framing.distance = distance;
    framing.eye = center + glm::vec3(0.0f, 0.45f * radius, distance);
    framing.pitchDegrees = glm::degrees(std::atan2(center.y - framing.eye.y, distance));
    return framing;
}

struct Geometry {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::map<int, std::vector<DrawRange>> batches;
    uint32_t primitiveCount = 0;
};

// No dedup and no flatten, so Model's vertex array is already the packed buffer:
// the primitives' index windows are valid as they stand and the bone arrays line
// up 1:1.  Vertices are copied, never re-ordered, which is what keeps
// boneIndices[i] describing vertices[i].
Geometry buildGeometry(const Model &model)
{
    if (!model.isSkinned()) {
        throw std::runtime_error("skeletal_animation: the model carries no skinning data");
    }

    Geometry geometry;
    geometry.vertices.reserve(model.vertices.size());
    for (size_t i = 0; i < model.vertices.size(); ++i) {
        const MeshVertex &source = model.vertices[i];
        SkinnedVertex vertex;
        vertex.position = source.position;
        vertex.texCoord = source.texCoord;
        vertex.boneIndices = model.boneIndices[i];
        vertex.boneWeights = model.boneWeights[i];
        geometry.vertices.push_back(vertex);
    }
    geometry.indices = model.indices;

    for (const Mesh &mesh : model.meshes) {
        for (const Primitive &primitive : mesh.primitives) {
            if (primitive.indexCount == 0) {
                continue;
            }
            DrawRange range;
            range.firstIndex = primitive.firstIndex;
            range.indexCount = primitive.indexCount;
            geometry.batches[primitive.materialIndex].push_back(range);
            ++geometry.primitiveCount;
        }
    }
    return geometry;
}

struct MaterialTexture {
    TextureImageSpec spec;
    std::vector<uint8_t> pixels;
    std::string name;
};

MaterialTexture makeTexture(const Model &model, int materialIndex)
{
    const Material *material = (materialIndex >= 0 && materialIndex < static_cast<int>(model.materials.size()))
                                   ? &model.materials[materialIndex]
                                   : nullptr;
    const int imageIndex = material ? material->baseColorTexture : -1;
    if (imageIndex >= 0 && imageIndex < static_cast<int>(model.images.size())) {
        const TextureImage &image = model.images[imageIndex];
        if (image.width > 0 && image.height > 0 && !image.pixels.empty()) {
            MaterialTexture texture;
            texture.spec.width = static_cast<uint32_t>(image.width);
            texture.spec.height = static_cast<uint32_t>(image.height);
            texture.spec.channels = 4;
            texture.spec.miplevels = std::max(1u, image.mipLevels);
            texture.spec.format = vk::Format::eR8G8B8A8Srgb;
            texture.spec.mipsInData = false;
            texture.spec.addressMode = vk::SamplerAddressMode::eRepeat;
            texture.pixels = image.pixels;
            texture.name = image.name;
            return texture;
        }
    }

    MaterialTexture fallback;
    fallback.spec.width = 1;
    fallback.spec.height = 1;
    fallback.spec.channels = 4;
    fallback.spec.miplevels = 1;
    fallback.spec.format = vk::Format::eR8G8B8A8Srgb;
    fallback.spec.mipsInData = false;
    fallback.pixels = {255, 255, 255, 255};
    fallback.name = "(none)";
    return fallback;
}

std::shared_ptr<DescriptorLayoutAtlas> makeLayout(const TextureImageSpec &textureSpec)
{
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor}, UpdateFrequency::eStatic, true);
    globalSet.specs[0] = textureSpec;

    auto environmentSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    environmentSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(SkeletonBlock)), DataTypes::eCustom};

    layout->setDetails({globalSet, environmentSet});
    return layout;
}

// Same background-mode coverage trick as model_loading (§19): the pass clears
// every texel, so the most frequent 8-bit colour is the background.
void analyseFrame(const std::vector<uint8_t> &rgb, float &coverage, size_t &uniqueColors)
{
    std::unordered_map<uint32_t, uint32_t> histogram;
    histogram.reserve(4096);
    const size_t texels = rgb.size() / 3;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const uint32_t key = (static_cast<uint32_t>(rgb[i]) << 16) | (static_cast<uint32_t>(rgb[i + 1]) << 8)
                             | static_cast<uint32_t>(rgb[i + 2]);
        ++histogram[key];
    }

    uint32_t backgroundCount = 0;
    uniqueColors = 0;
    for (const auto &[key, count] : histogram) {
        (void)key;
        backgroundCount = std::max(backgroundCount, count);
        if (count >= 16) {
            ++uniqueColors;
        }
    }
    coverage = texels > 0 ? 1.0f - static_cast<float>(backgroundCount) / static_cast<float>(texels) : 0.0f;
}

// Fingerprint of the current pose: the sum of every joint matrix element.  Two
// runs of the same --time must print the same number, and two different times
// must not -- which is what proves the clip is actually being sampled rather
// than a static mesh being drawn.
double jointFingerprint(const std::vector<glm::mat4> &joints)
{
    double sum = 0.0;
    for (const glm::mat4 &matrix : joints) {
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                sum += static_cast<double>(matrix[column][row]);
            }
        }
    }
    return sum;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);

    // ----- Load ------------------------------------------------------------
    ModelLoadOptions loadOptions;
    loadOptions.preferredLoader = options.loader;
    // Skinning is a per-vertex attribute problem, not a geometry problem, so the
    // loader must not merge vertices (the guard in deduplicateVertices would
    // refuse anyway) and must not bake node transforms into them.
    loadOptions.deduplicate = Toggle::eOff;
    loadOptions.generateTangents = Toggle::eOff;
    loadOptions.flattenScene = false;
    loadOptions.normalizeSize = false;
    const Model model = loadModel(options.modelPath, loadOptions);
    if (!model.valid() || !model.isSkinned()) {
        throw std::runtime_error("skeletal_animation: '" + options.modelPath + "' is not a skinned model");
    }
    if (model.skins[0].joints.size() > kMaxJoints) {
        throw std::runtime_error("skeletal_animation: " + options.modelPath + " has "
                                 + std::to_string(model.skins[0].joints.size()) + " joints, more than the "
                                 + std::to_string(kMaxJoints) + " this example's uniform block holds");
    }
    if (model.clips.empty()) {
        throw std::runtime_error("skeletal_animation: '" + options.modelPath + "' contains no animation clip");
    }
    if (options.clip < 0 || options.clip >= static_cast<int>(model.clips.size())) {
        throw std::runtime_error("skeletal_animation: --clip " + std::to_string(options.clip) + " is out of range, '"
                                 + options.modelPath + "' has " + std::to_string(model.clips.size()) + " clip(s)");
    }

    const Geometry geometry = buildGeometry(model);

    SkeletonAnimator animator;
    animator.bind(model, options.clip);

    // Pose the animator at whatever the user asked for.  The envelope sampling
    // below drives it through the whole clip, so this is re-applied afterwards.
    const auto applyRequestedPose = [&]() {
        if (options.staticPose) {
            animator.bindPose();
        } else if (options.hasTime) {
            animator.setPlaying(false);
            animator.setTime(options.time);
        } else {
            animator.setTime(0.0f);
        }
    };
    applyRequestedPose();

    // ----- Bounds, normalisation and framing --------------------------------
    // Three different "bounds" are in play and only the last one is where the
    // pixels land:
    //   mesh space        Model::minBounds -- 270 units wide, the exporter's units
    //   world space       computeWorldBounds -- the .dae root carries a 0.01 scale
    //   skinned envelope  what the joints actually put on screen
    // This file's scene pose also sits a unit to the left of its mesh, so framing
    // on anything but the envelope leaves the character off-centre.
    glm::vec3 worldMin(0.0f);
    glm::vec3 worldMax(0.0f);
    computeWorldBounds(model, worldMin, worldMax);

    glm::vec3 poseMin(0.0f);
    glm::vec3 poseMax(0.0f);
    animator.computeSkinnedBounds(poseMin, poseMax);

    glm::vec3 envelopeMin = poseMin;
    glm::vec3 envelopeMax = poseMax;
    if (options.bindMesh) {
        // The forced-bind render must frame exactly like model_loading, so it uses
        // the same undeformed world bounds rather than the clip's envelope.
        envelopeMin = worldMin;
        envelopeMax = worldMax;
    } else if (animator.hasClip()) {
        // Union over the clip so the camera never has to move while it plays; a
        // single pose would clip as soon as the character reached for something.
        constexpr int kSamples = 24;
        const float duration = animator.duration();
        for (int sample = 0; sample <= kSamples; ++sample) {
            animator.setTime(duration * static_cast<float>(sample) / static_cast<float>(kSamples));
            glm::vec3 lo(0.0f);
            glm::vec3 hi(0.0f);
            animator.computeSkinnedBounds(lo, hi);
            envelopeMin = glm::min(envelopeMin, lo);
            envelopeMax = glm::max(envelopeMax, hi);
        }
    }
    applyRequestedPose();

    const glm::vec3 extent = envelopeMax - envelopeMin;
    const float longest = std::max({extent.x, extent.y, extent.z, 0.0001f});
    const glm::vec3 center = (envelopeMin + envelopeMax) * 0.5f;
    const float scale = 2.0f / longest;
    // Identical arithmetic to model_post::normalizeSize, but expressed as a
    // matrix.  Baking it into the vertices would be wrong: every joint has its
    // own matrix, so no single vertex-space translation shifts the animated
    // result correctly (see DESIGN.md §21.3).
    const glm::mat4 modelMatrix =
        glm::scale(glm::mat4(1.0f), glm::vec3(scale)) * glm::translate(glm::mat4(1.0f), -center);

    const glm::vec3 normalizedMin = (envelopeMin - center) * scale;
    const glm::vec3 normalizedMax = (envelopeMax - center) * scale;
    constexpr float kInitialAspect = 1280.0f / 720.0f;
    const CameraFraming framing = options.bindMesh ? frameSphere(normalizedMin, normalizedMax)
                                                   : frameBox(normalizedMin, normalizedMax, kInitialAspect);

    // Test hook: every joint resolves to the mesh node's world matrix, so skinning
    // collapses to "transform by the node" -- which is exactly the geometry
    // model_loading draws from the same file.  Pixel-comparing the two renders is
    // the check that the vertex layout, the weights and the uniform block agree
    // with a draw path that has no skinning at all (§21.5).
    std::vector<glm::mat4> bindMeshJoints;
    if (options.bindMesh) {
        glm::mat4 meshWorld(1.0f);
        bool found = false;
        forEachMeshInstance(model, [&](const glm::mat4 &world, int, const Mesh &, const Primitive &) {
            if (!found) {
                meshWorld = world;
                found = true;
            }
        });
        bindMeshJoints.assign(animator.jointCount(), meshWorld);
    }

    auto camera =
        std::make_shared<Camera>(framing.eye, glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, framing.pitchDegrees);
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    std::cout << "[skeletal] " << options.modelPath << ": " << geometry.vertices.size() << " vertices, "
              << geometry.indices.size() << " indices (" << geometry.indices.size() / 3 << " triangles), "
              << geometry.primitiveCount << " primitive(s) in " << geometry.batches.size() << " material batch(es), "
              << model.nodes.size() << " node(s), " << model.skins[0].joints.size() << " joint(s), "
              << model.clips.size() << " clip(s)" << std::endl;
    std::cout << "[skeletal] clip " << options.clip << " '" << model.clips[options.clip].name << "': duration "
              << model.clips[options.clip].durationSeconds() << " s at "
              << model.clips[options.clip].ticksPerSecond << " ticks/s, "
              << model.clips[options.clip].channels.size() << " channel(s)" << std::endl;
    std::cout << "[skeletal] bounds: mesh " << model.minBounds.x << "," << model.minBounds.y << ","
              << model.minBounds.z << " .. " << model.maxBounds.x << "," << model.maxBounds.y << ","
              << model.maxBounds.z << std::endl;
    std::cout << "[skeletal]         world " << worldMin.x << "," << worldMin.y << "," << worldMin.z << " .. "
              << worldMax.x << "," << worldMax.y << "," << worldMax.z << std::endl;
    std::cout << "[skeletal]         pose  " << poseMin.x << "," << poseMin.y << "," << poseMin.z << " .. "
              << poseMax.x << "," << poseMax.y << "," << poseMax.z << std::endl;
    std::cout << "[skeletal]         envelope " << envelopeMin.x << "," << envelopeMin.y << "," << envelopeMin.z
              << " .. " << envelopeMax.x << "," << envelopeMax.y << "," << envelopeMax.z << " -> scale " << scale
              << std::endl;
    std::cout << "[skeletal] framed at eye (" << framing.eye.x << "," << framing.eye.y << "," << framing.eye.z
              << ") pitch " << framing.pitchDegrees << " deg, distance " << framing.distance << std::endl;

    // ----- Render pass -----------------------------------------------------
    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(),
        mgr->DefaultDepthFormat()));

    GraphicsPipelineConfig pipelineConfig;
    pipelineConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    pipelineConfig.cullMode = vk::CullModeFlagBits::eNone;

    auto upload = mgr->UploadSequence();
    std::vector<std::shared_ptr<CmdOpDrawFrame>> draws;
    std::vector<std::shared_ptr<CmdSyncDevice>> frameSyncs;

    for (const auto &[materialIndex, ranges] : geometry.batches) {
        const MaterialTexture texture = makeTexture(model, materialIndex);
        auto layout = makeLayout(texture.spec);
        auto asset = mgr->CreateShaderAssetGroup(layout);
        asset->addShaderSpirvCode(
            "vert", std::vector<uint32_t>(shader::SKELETAL_VERT_SPV.begin(), shader::SKELETAL_VERT_SPV.end()));
        asset->addShaderSpirvCode(
            "frag", std::vector<uint32_t>(shader::SKELETAL_FRAG_SPV.begin(), shader::SKELETAL_FRAG_SPV.end()));

        auto vertexInput = std::make_shared<SkinnedVertexInputAtlas>(
            0, static_cast<uint32_t>(geometry.vertices.size()), static_cast<uint32_t>(geometry.indices.size()));
        auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, pipelineConfig);

        auto sync = std::make_shared<CmdSyncDevice>(asset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, geometry.vertices);
        sync->OnUpdateRawData(ResourceTypes::eIndex, geometry.indices);
        sync->OnUpdateRawData(ResourceId(0, 0), 0u, texture.pixels);
        upload->submit(sync, asset);
        frameSyncs.push_back(sync);

        auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);
        draw->setDrawRanges(ranges);
        draws.push_back(draw);

        std::cout << "[skeletal] material " << materialIndex << ": " << ranges.size() << " primitive window(s), "
                  << "diffuse map '" << texture.name << "' (" << texture.spec.width << "x" << texture.spec.height
                  << ")" << std::endl;
    }

    // ----- Frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();

    bool dumpPending = false;
    bool dumpDone = false;
    uint32_t renderedFrames = 0;
    bool resizeRequested = false;
    float lastTime = 0.0f;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (!options.hasTime && !options.staticPose) {
            animator.update(currentTime - lastTime);
        }
        lastTime = currentTime;

        if (options.resizeAfter != 0 && !resizeRequested && renderedFrames >= options.resizeAfter) {
            resizeRequested = true;
            window.resize(1024, 600);
            std::cout << "[skeletal] requested resize to 1024x600" << std::endl;
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

        if (options.dump && !dumpDone && !dumpPending && renderedFrames >= options.dumpAfter) {
            dumpPending = true;
        }

        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);
        SkeletonBlock block{};
        block.view = cameraMatrix.view;
        block.proj = cameraMatrix.proj;
        block.model = modelMatrix;
        const std::vector<glm::mat4> &joints =
            options.bindMesh ? bindMeshJoints : animator.jointMatrices();
        for (size_t j = 0; j < joints.size() && j < kMaxJoints; ++j) {
            block.joints[j] = joints[j];
        }
        for (const auto &draw : draws) {
            draw->OnUpdateRawData(ResourceId(1, 0), frame, block);
        }

        sequence->beginRecording();
        for (const auto &sync : frameSyncs) {
            sequence->record(sync);
        }
        sequence->recordPass(renderPass, [&]() {
            for (const auto &draw : draws) {
                sequence->record(draw);
            }
        });
        sequence->endRecording();
        sequence->submitFrame();

        if (dumpPending) {
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
                                   mgr->getGraphicsQueue(), *mgr->getQueueFamilyIndices().graphicsFamily);

            const std::vector<vk::Image> &swapImages = mgr->getSwapChain()->getImages();
            const uint32_t imageIndex = sequence->getImageIndex();
            if (imageIndex >= swapImages.size()) {
                throw std::runtime_error("skeletal_animation: swapchain image index out of range");
            }
            DumpRequest request;
            request.label = "skeletal";
            request.image = swapImages[imageIndex];
            request.format = *mgr->getSwapChain()->getImageFormat();
            request.extent = extent;
            request.layout = vk::ImageLayout::ePresentSrcKHR;
            request.encoding = DumpEncoding::eColor;
            const auto results = DumpImages(readback, {request}, "dump", 0);

            float coverage = 0.0f;
            size_t uniqueColors = 0;
            analyseFrame(results[0].rgb8, coverage, uniqueColors);
            std::cout << "[dump] " << results[0].path.string() << " coverage=" << coverage
                      << " uniqueColors=" << uniqueColors << " mean=(" << results[0].meanColor.r << ","
                      << results[0].meanColor.g << "," << results[0].meanColor.b << ")"
                      << " t=" << animator.time() << " jointFingerprint=" << jointFingerprint(joints) << std::endl;

            dumpPending = false;
            dumpDone = true;
        }

        if (!sequence->presentFrame()) {
            continue;
        }
        ++renderedFrames;
        if (options.dump && dumpDone) {
            break;
        }
    }
}

int main(int argc, char **argv)
{
    try {
        test(argc, argv);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
