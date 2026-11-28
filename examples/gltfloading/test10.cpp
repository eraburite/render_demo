// gltfloading: load a glTF file, walk its node hierarchy and render every
// mesh-bearing node with its own world matrix.  All nodes that share the mesh
// are drawn in one instanced call; their world matrices live in a per-frame
// SSBO and the vertex shader selects one with gl_InstanceIndex.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

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

#include "shader/mesh_frag.hpp"
#include "shader/mesh_vert.hpp"

namespace {

std::string parseModelPath(int argc, char **argv)
{
    std::string path = PROJECT_ROOT_PATH "/assets/models/deer.gltf";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--model") {
            path = argv[i + 1];
            break;
        }
    }
    return path;
}

// set0/set2 are empty placeholders so set1 (camera) and set3 (node matrices)
// keep their shared component ids, which also keeps the set indices dense.
std::shared_ptr<DescriptorLayoutAtlas> makeLayout(uint32_t instanceCount)
{
    auto layout = std::make_shared<DescriptorLayoutAtlas>();

    auto empty0 = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {}, UpdateFrequency::eStatic, true);
    auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};
    auto empty2 = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {}, UpdateFrequency::eEachFrame, false);
    auto instanceSet = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        3, {InstanceComponent::eModelMatrix}, UpdateFrequency::eEachInstance, false);
    instanceSet.specs[0] = GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(glm::mat4)),
                                       DataTypes::eCustom};

    layout->setDetails({empty0, cameraSet, empty2, instanceSet});
    return layout;
}

struct EngineModel {
    std::vector<Vertex> vertices; // expanded: one entry per index
};

// Center and uniformly fit the file into a ~[-1, 1] box so the demo tree's
// transforms have predictable proportions for any input model.  The engine's
// indexed-draw path cannot infer an index type from its buffer spec, so the
// index list is expanded into a plain vertex stream (draw instead of
// drawIndexed) -- the geometry and every node matrix are unchanged.
EngineModel toEngineModel(const Model &gltf)
{
    const glm::vec3 extent = gltf.maxBounds - gltf.minBounds;
    const float maxExtent = std::max({extent.x, extent.y, extent.z, 0.0001f});
    const float scale = 2.0f / maxExtent;
    const glm::vec3 center = (gltf.minBounds + gltf.maxBounds) * 0.5f;

    std::vector<Vertex> source;
    source.reserve(gltf.vertices.size());
    for (const MeshVertex &src : gltf.vertices) {
        Vertex dst;
        dst.position = (src.position - center) * scale;
        dst.normal = src.normal;
        dst.texCoord = src.texCoord;
        dst.color = src.color;
        dst.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
        source.push_back(dst);
    }

    EngineModel out;
    out.vertices.reserve(gltf.indices.size());
    for (uint32_t index : gltf.indices) {
        if (index < source.size()) {
            out.vertices.push_back(source[index]);
        }
    }
    return out;
}

// Turn the loaded file into an explicit scene graph that exercises hierarchy
// traversal: node 0 is a pivot, nodes 1..3 are its children and node 4 is a
// child of node 1 (it inherits two ancestor transforms).  deer.gltf and
// suzanne.gltf contain a single mesh node, so the extra nodes reuse that mesh
// to make the hierarchy visible.
void buildDemoTree(Model &gltf)
{
    const int meshIndex = gltf.meshes.empty() ? -1 : 0;
    gltf.nodes.clear();
    gltf.rootNodes.clear();

    auto addNode = [&](const char *name, const glm::mat4 &local, int mesh, std::vector<int> children) {
        ModelNode node;
        node.name = name;
        node.transform = local;
        node.meshIndex = mesh;
        node.children = std::move(children);
        gltf.nodes.push_back(node);
        return static_cast<int>(gltf.nodes.size() - 1);
    };

    addNode("ScenePivot", glm::mat4(1.0f), -1, {1, 2, 3});

    const glm::mat4 left = glm::scale(glm::rotate(
        glm::translate(glm::mat4(1.0f), glm::vec3(-1.35f, 0.0f, 0.0f)),
        glm::radians(18.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(0.8f));
    addNode("Body", left, meshIndex, {4});

    const glm::mat4 right = glm::scale(glm::rotate(
        glm::translate(glm::mat4(1.0f), glm::vec3(1.35f, 0.0f, 0.0f)),
        glm::radians(-18.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(0.8f));
    addNode("BodyRight", right, meshIndex, {});

    const glm::mat4 rear = glm::scale(glm::rotate(
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.8f)),
        glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(0.65f));
    addNode("BodyRear", rear, meshIndex, {});

    const glm::mat4 fawn = glm::scale(
        glm::translate(glm::mat4(1.0f), glm::vec3(0.55f, -0.85f, 0.55f)), glm::vec3(0.42f));
    addNode("Fawn", fawn, meshIndex, {});

    gltf.rootNodes.push_back(0);
}

} // namespace

void test(int argc, char **argv)
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f),
                                           -90.0f, -6.0f);
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ----- Load the glTF file ---------------------------------------------
    const std::string modelPath = parseModelPath(argc, argv);
    Model gltf = loadModel(modelPath);
    if (!gltf.valid()) {
        throw std::runtime_error("gltfloading: no triangle geometry in " + modelPath);
    }
    EngineModel model = toEngineModel(gltf);
    buildDemoTree(gltf);

    std::vector<int> meshNodes;
    for (size_t i = 0; i < gltf.nodes.size(); ++i) {
        if (gltf.nodes[i].meshIndex >= 0) {
            meshNodes.push_back(static_cast<int>(i));
        }
    }
    if (meshNodes.empty()) {
        throw std::runtime_error("gltfloading: no mesh nodes in " + modelPath);
    }
    const uint32_t instanceCount = static_cast<uint32_t>(meshNodes.size());

    // The root pivot is animated; computeNodeWorldMatrices propagates it to
    // every child, which is what makes this a node-tree walk rather than a
    // static transform list.
    const auto makeInstances = [&](float seconds) {
        gltf.nodes[0].transform =
            glm::rotate(glm::mat4(1.0f), seconds * 0.35f, glm::vec3(0.0f, 1.0f, 0.0f));
        const std::vector<glm::mat4> world = computeNodeWorldMatrices(gltf);
        std::vector<glm::mat4> instances;
        instances.reserve(meshNodes.size());
        for (int node : meshNodes) {
            instances.push_back(world[node]);
        }
        return instances;
    };

    std::cout << "[gltfloading] " << modelPath << ": " << gltf.vertices.size() << " source vertices, "
              << gltf.indices.size() << " indices -> " << model.vertices.size() << " draw vertices, "
              << gltf.nodes.size() << " nodes, " << instanceCount << " mesh instances\n";

    // ----- Descriptors / pipeline -----------------------------------------
    auto layout = makeLayout(instanceCount);
    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::MESH_VERT_SPV.begin(),
                                                            shader::MESH_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::MESH_FRAG_SPV.begin(),
                                                            shader::MESH_FRAG_SPV.end()));

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor,
                                     VertexComponent::eUV, VertexComponent::eNormal},
        static_cast<uint32_t>(model.vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, instanceCount);

    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(),
        mgr->DefaultDepthFormat()));
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0);

    // ----- Upload static geometry and the initial node matrices -----------
    const std::vector<glm::mat4> initialInstances = makeInstances(0.0f);
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, model.vertices);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        syncOp->OnUpdateRawData(ResourceId(3, 0), frame, initialInstances);
    }
    upload->submit(syncOp, asset);

    // ----- Frame loop -----------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, *mgr->getSwapChain()->getExtent2D());

        draw->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);
        draw->OnUpdateRawData(ResourceId(3, 0), frame, makeInstances(currentTime));

        sequence->beginRecording();
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
