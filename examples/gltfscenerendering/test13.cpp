// gltfscenerendering: recursive scene-graph traversal with per-node visibility.
//
// A glTF mesh is loaded once, then instanced into a small node hierarchy
// (group -> group -> mesh leaves).  World matrices are computed recursively on
// the CPU and uploaded into one SSBO; the vertex shader selects a node with
// gl_InstanceIndex, so a single draw renders the whole tree.  Every
// node carries a visibility flag: hiding a group hides its whole subtree.
// Press V to toggle a branch, or use --toggle-node <index> at startup.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ModelLoader.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "root_directory.h"

#include "shader/scene_frag.hpp"
#include "shader/scene_vert.hpp"

namespace {

// set0 binding0: camera viewProj + eye (vertex stage).
enum class SceneComponent { eCameraMatrix };
using SceneConfig = DescriptorComponentSpec<SceneComponent>;
inline constexpr auto SceneComponentTable = std::make_tuple(SceneConfig{
    SceneComponent::eCameraMatrix, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex});

struct CameraUBO {
    alignas(16) glm::mat4 viewProj{};
    alignas(16) glm::vec4 eye{};
};

// One node of the display scene graph.  meshIndex >= 0 means the node draws
// that glTF mesh; children are visited recursively.
struct SceneNode {
    std::string name;
    int meshIndex = -1;
    glm::mat4 local{1.0f};
    std::vector<int> children;
    bool visible = true;
};

struct Options {
    std::string model = PROJECT_ROOT_PATH "/data/models/deer.gltf";
    int toggleNode = -1;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--model" && i + 1 < argc) {
            options.model = argv[++i];
        } else if (arg == "--toggle-node" && i + 1 < argc) {
            options.toggleNode = std::atoi(argv[++i]);
        }
    }
    return options;
}

// Normalise the loaded glTF vertices into the same unit-ish box used by the
// other glTF examples (centred at the origin, largest dimension == 2).  The
// engine's indexed-draw path cannot infer an index type from its buffer spec,
// so the index list is expanded into a plain vertex stream (draw, not
// drawIndexed); geometry and node matrices are unchanged.
std::vector<Vertex> toEngineVertices(const Model &model)
{
    const glm::vec3 extent = model.maxBounds - model.minBounds;
    const float maxExtent = std::max({extent.x, extent.y, extent.z, 0.0001f});
    const float scale = 2.0f / maxExtent;
    const glm::vec3 center = (model.minBounds + model.maxBounds) * 0.5f;

    std::vector<Vertex> source;
    source.reserve(model.vertices.size());
    for (const MeshVertex &src : model.vertices) {
        Vertex dst;
        dst.position = (src.position - center) * scale;
        dst.normal = src.normal;
        dst.texCoord = src.texCoord;
        dst.color = src.color;
        dst.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
        source.push_back(dst);
    }

    std::vector<Vertex> result;
    result.reserve(model.indices.size());
    for (uint32_t index : model.indices) {
        if (index < source.size()) {
            result.push_back(source[index]);
        }
    }
    return result;
}

// Two branches, each holding two mesh leaves.  The group transforms make the
// recursion observable: a leaf's world matrix is the product of its whole
// ancestor chain.
std::vector<SceneNode> buildSceneGraph()
{
    std::vector<SceneNode> nodes;
    nodes.reserve(7);
    nodes.push_back({"SceneRoot", -1, glm::mat4(1.0f), {1, 2}, true});
    nodes.push_back({"BranchLeft", -1,
                     glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, 0.0f)), {3, 4}, true});
    nodes.push_back({"BranchRight", -1,
                     glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f)), {5, 6}, true});

    const auto leaf = [](const char *name, float y, float angle, float scale) {
        glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, y, 0.0f));
        local = glm::rotate(local, glm::radians(angle), glm::vec3(0.0f, 1.0f, 0.0f));
        local = glm::scale(local, glm::vec3(scale));
        return SceneNode{name, 0, local, {}, true};
    };
    nodes.push_back(leaf("DeerLeftUpper", 0.62f, 25.0f, 0.5f));
    nodes.push_back(leaf("DeerLeftLower", -0.62f, -25.0f, 0.5f));
    nodes.push_back(leaf("DeerRightUpper", 0.62f, 155.0f, 0.5f));
    nodes.push_back(leaf("DeerRightLower", -0.62f, -155.0f, 0.5f));
    return nodes;
}

// Depth-first world-matrix computation, parallel to computeNodeWorldMatrices()
// in the glTF loader.  effectiveVisible inherits the parent's visibility, so
// hiding a group hides every descendant.
void computeNodeWorldMatrices(const std::vector<SceneNode> &nodes, int index, const glm::mat4 &parentWorld,
                              bool parentVisible, std::vector<glm::mat4> &worlds,
                              std::vector<uint8_t> &effectiveVisible)
{
    effectiveVisible[index] = parentVisible && nodes[index].visible;
    worlds[index] = parentWorld * nodes[index].local;
    for (int child : nodes[index].children) {
        computeNodeWorldMatrices(nodes, child, worlds[index], effectiveVisible[index] != 0,
                                 worlds, effectiveVisible);
    }
}

// One SSBO entry per mesh node; gl_InstanceIndex is the index in this vector.
// model[3][3] doubles as the visibility flag consumed by scene.vert.
std::vector<glm::mat4> makeInstanceMatrices(const std::vector<SceneNode> &nodes,
                                            const std::vector<glm::mat4> &worlds,
                                            const std::vector<uint8_t> &effectiveVisible)
{
    std::vector<glm::mat4> result;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].meshIndex < 0) {
            continue;
        }
        glm::mat4 model = worlds[i];
        if (!effectiveVisible[i]) {
            model[3][3] = 0.0f;
        }
        result.push_back(model);
    }
    return result;
}

void printNode(const std::vector<SceneNode> &nodes, int index, int depth)
{
    std::cout << std::string(static_cast<size_t>(depth) * 2, ' ') << "[" << index << "] " << nodes[index].name
              << (nodes[index].meshIndex >= 0 ? " (mesh)" : " (group)")
              << (nodes[index].visible ? "" : " [hidden]") << "\n";
    for (int child : nodes[index].children) {
        printNode(nodes, child, depth + 1);
    }
}

bool justPressed(bool current, bool &held)
{
    const bool result = current && !held;
    held = current;
    return result;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 5.0f));
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ----- Load glTF mesh and glTF node hierarchy -------------------------
    const Model gltf = loadModel(options.model);
    if (!gltf.valid()) {
        throw std::runtime_error("gltfscenerendering: failed to load " + options.model);
    }
    const std::vector<Vertex> vertices = toEngineVertices(gltf);
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    const std::vector<glm::mat4> gltfNodeWorld = computeNodeWorldMatrices(gltf);
    std::cout << "[gltfscenerendering] loaded " << gltf.vertices.size() << " source vertices / "
              << gltf.indices.size() << " indices -> " << vertexCount
              << " expanded vertices from " << options.model << "\n";
    std::cout << "[gltfscenerendering] glTF nodes: " << gltf.nodes.size()
              << " (world matrices: " << gltfNodeWorld.size() << ")\n";

    // ----- Build and print the display scene graph ------------------------
    std::vector<SceneNode> nodes = buildSceneGraph();
    int keyToggleNode = 2; // BranchRight by default; overridden by --toggle-node.
    if (options.toggleNode >= 0 && options.toggleNode < static_cast<int>(nodes.size())) {
        nodes[options.toggleNode].visible = false;
        keyToggleNode = options.toggleNode;
    }
    std::cout << "[gltfscenerendering] scene graph (recursive traversal):\n";
    printNode(nodes, 0, 0);

    std::vector<glm::mat4> nodeWorlds(nodes.size(), glm::mat4(1.0f));
    std::vector<uint8_t> nodeVisible(nodes.size(), 1u);
    computeNodeWorldMatrices(nodes, 0, glm::mat4(1.0f), true, nodeWorlds, nodeVisible);
    std::vector<glm::mat4> instanceMatrices = makeInstanceMatrices(nodes, nodeWorlds, nodeVisible);
    const uint32_t instanceCount = static_cast<uint32_t>(instanceMatrices.size());
    std::cout << "[gltfscenerendering] " << instanceCount << " mesh nodes -> " << instanceCount
              << " instances in one draw\n";

    // ----- Descriptor layout: set0 camera UBO, set1 per-node matrix SSBO ---
    auto layout = std::make_shared<DescriptorLayoutAtlas>();
    auto cameraSet = DescriptorLayoutAtlas::makeSet<SceneComponent, SceneComponentTable>(
        0, {SceneComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraUBO)), DataTypes::eCustom};
    auto instanceSet = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        1, {InstanceComponent::eModelMatrix}, UpdateFrequency::eEachInstance, false);
    instanceSet.specs[0] = GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(glm::mat4)),
                                       DataTypes::eCustom};
    layout->setDetails({cameraSet, instanceSet});

    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("vert",
                              std::vector<uint32_t>(shader::SCENE_VERT_SPV.begin(), shader::SCENE_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag",
                              std::vector<uint32_t>(shader::SCENE_FRAG_SPV.begin(), shader::SCENE_FRAG_SPV.end()));

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor,
                                     VertexComponent::eUV, VertexComponent::eNormal},
        vertexCount, 0u, DataTypes::eUnsignedInt, false, 0u, instanceCount);

    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(),
        mgr->DefaultDepthFormat()));

    GraphicsPipelineConfig pipelineConfig;
    pipelineConfig.cullMode = vk::CullModeFlagBits::eBack;
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, pipelineConfig);

    // ----- Upload static geometry and the initial node matrices ----------
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        syncOp->OnUpdateRawData(ResourceId(1, 0), frame, instanceMatrices);
    }
    upload->submit(syncOp, asset);

    // ----- Frame loop -----------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    bool vHeld = false;
    std::cout << "[gltfscenerendering] press V to toggle node " << keyToggleNode << " (\""
              << nodes[keyToggleNode].name << "\")\n";

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (justPressed(window.isKeyDown(GLFW_KEY_V), vHeld)) {
            nodes[keyToggleNode].visible = !nodes[keyToggleNode].visible;
            std::cout << "[gltfscenerendering] node " << keyToggleNode << " (\"" << nodes[keyToggleNode].name
                      << "\") " << (nodes[keyToggleNode].visible ? "VISIBLE" : "HIDDEN") << "\n";
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const auto extent = mgr->getSwapChain()->getExtent2D();
        const CameraMatrix matrices = MakeCameraMatrix(*camera, static_cast<float>(extent->width),
                                                       static_cast<float>(extent->height));
        CameraUBO ubo;
        ubo.viewProj = matrices.proj * matrices.view;
        ubo.eye = glm::vec4(camera->Position, 1.0f);
        draw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);

        // Visibility is CPU state: recompute world matrices recursively and
        // refresh the small matrix SSBO for this in-flight frame.
        computeNodeWorldMatrices(nodes, 0, glm::mat4(1.0f), true, nodeWorlds, nodeVisible);
        instanceMatrices = makeInstanceMatrices(nodes, nodeWorlds, nodeVisible);
        draw->OnUpdateRawData(ResourceId(1, 0), frame, instanceMatrices);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(renderPass, [&]() { sequence->record(draw); });
        sequence->endRecording();
        sequence->submitFrame();
        if (!sequence->presentFrame()) {
            continue;
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
