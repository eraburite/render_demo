// scene_graph: LearnOpenGL guest 2021 1.scene, both halves in one example.
//
//   --mode chain   the scene-graph demo: 11 nested planets, each child offset
//                  and scaled by 0.75 relative to its parent, root spinning
//                  about Y.  Ten levels of parent transform compose per frame.
//   --mode grid    the frustum-culling demo: 400 planets on a 20x20 grid, camera
//                  inside the field so most of them are behind it.
//
// The invariant that makes the culling trustworthy: --no-cull must produce a
// pixel-identical frame.  Culling that drops something visible, or that uses
// planes which disagree with the projection, fails that comparison immediately.
//
// Two structural differences from the reference:
//
//   * The scene graph *is* a Model: one node tree, one mesh referenced by many
//     nodes, world matrices from computeNodeWorldMatrices().  The reference
//     clones a whole Model per entity (vertices, indices and textures included)
//     and keeps a std::list<unique_ptr<Entity>>.  Here the hierarchy is indices
//     into a flat array, so per-frame updates are a vector walk with no
//     allocation.
//   * Culling decides one draw parameter, not the command stream: survivors are
//     compacted to the front of the instance matrix table and the draw's
//     instance count is set to the survivor count, so the whole scene stays one
//     instanced draw.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "Frustum.h"
#include "GLFWwindow.h"
#include "ImageDump.h"
#include "ImageReadback.h"
#include "ModelAsset.h"
#include "ModelLoader.h"
#include "RenderCommon.h"
#include "root_directory.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "shader/scene_frag.hpp"
#include "shader/scene_vert.hpp"

namespace {

constexpr const char *kPlanetPath = PROJECT_ROOT_PATH "/data/models/planet/planet.obj";

// One diffuse map plus the 1x1 white fallback the other model examples use: a
// material without a map still has something bound.
struct MaterialTexture {
    TextureImageSpec spec;
    std::vector<uint8_t> pixels;
    std::string name;
};

struct Options {
    std::string mode = "grid";
    bool cull = true;
    bool hasTime = false;
    float time = 0.0f;
    bool dump = false;
    uint32_t dumpAfter = 0;
    uint32_t resizeAfter = 0;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            options.mode = argv[++i];
            if (options.mode != "chain" && options.mode != "grid") {
                throw std::runtime_error("scene_graph: --mode must be 'chain' or 'grid'");
            }
        } else if (arg == "--cull") {
            options.cull = true;
        } else if (arg == "--no-cull") {
            options.cull = false;
        } else if (arg == "--time" && i + 1 < argc) {
            options.hasTime = true;
            options.time = std::stof(argv[++i]);
        } else if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--dump-after" && i + 1 < argc) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--resize-after" && i + 1 < argc) {
            options.resizeAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
    }
    return options;
}

// One interleaved vertex buffer for the whole scene: every entity is this mesh.
struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    glm::vec3 boundsCenter{0.0f};
    glm::vec3 boundsExtent{0.0f};
    int materialIndex = -1;
};

// Flattens the single mesh/primitive of planet.obj into the engine's Vertex.
// A general multi-material model would need one asset per material (see
// model_loading); a scene-graph demo only needs one shared mesh, so this asserts
// what it assumes instead of pretending to be general.
Geometry buildGeometry(const Model &model)
{
    Geometry geometry;
    if (model.meshes.empty() || model.meshes.empty() || model.meshes[0].primitives.empty()) {
        throw std::runtime_error("scene_graph: model has no primitive to instantiate");
    }
    const Primitive &primitive = model.meshes[0].primitives[0];
    geometry.materialIndex = primitive.materialIndex;

    const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                            static_cast<uint32_t>(model.indices.size()));
    geometry.vertices.reserve(end - primitive.firstIndex);
    for (uint32_t i = primitive.firstIndex; i < end; ++i) {
        const uint32_t source = model.indices[i];
        if (source >= model.vertices.size()) {
            continue;
        }
        const MeshVertex &src = model.vertices[source];
        Vertex vertex;
        vertex.position = src.position;
        vertex.normal = src.normal;
        vertex.texCoord = src.texCoord;
        vertex.tangent = src.tangent;
        vertex.color = src.color;
        geometry.indices.push_back(static_cast<uint32_t>(geometry.vertices.size()));
        geometry.vertices.push_back(vertex);
    }

    // The culling box is this buffer's extents, not Model::minBounds: the loader
    // may have normalised or flattened, and the drawn geometry is what culling
    // has to bound.
    glm::vec3 low(std::numeric_limits<float>::max());
    glm::vec3 high(std::numeric_limits<float>::lowest());
    for (const Vertex &vertex : geometry.vertices) {
        low = glm::min(low, vertex.position);
        high = glm::max(high, vertex.position);
    }
    geometry.boundsCenter = (low + high) * 0.5f;
    geometry.boundsExtent = (high - low) * 0.5f;
    return geometry;
}

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

// set0 texture (static, shared) + set1 camera + set3 instance matrices.
// set2 has to exist as an empty placeholder: descriptor sets must stay dense.
std::shared_ptr<DescriptorLayoutAtlas> makeLayout(const TextureImageSpec &textureSpec, uint32_t maxInstances)
{
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor}, UpdateFrequency::eStatic, true);
    globalSet.specs[0] = textureSpec;

    auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};

    auto emptySet = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {}, UpdateFrequency::eEachFrame, false);

    auto instanceSet = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        3, {InstanceComponent::eModelMatrix}, UpdateFrequency::eEachInstance, false);
    instanceSet.specs[0] = GBufferSpec{maxInstances, static_cast<uint32_t>(sizeof(glm::mat4)), DataTypes::eCustom};

    layout->setDetails({globalSet, cameraSet, emptySet, instanceSet});
    return layout;
}

// A node of the scene graph.  Deliberately flat: parent/children are indices, so
// adding an entity never invalidates anything and the per-frame walk is a plain
// loop over a vector.  localPosition/localScale are what the reference calls
// Transform::setLocalPosition / setLocalScale; the only animated channel is the
// root's yaw.
struct SceneNode {
    int parent = -1;
    glm::vec3 localPosition{0.0f};
    float localScale = 1.0f;
    bool hasMesh = false;
};

struct Scene {
    std::vector<SceneNode> nodes;
    int root = 0;
    std::vector<glm::mat4> baseLocals; // local matrices with the root's yaw at 0
};

glm::mat4 localMatrix(const SceneNode &node, float yawRadians)
{
    glm::mat4 matrix = glm::translate(glm::mat4(1.0f), node.localPosition);
    matrix = glm::rotate(matrix, yawRadians, glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::scale(matrix, glm::vec3(node.localScale));
}

// chain: root at (10,0,0) scale 0.75, ten nested children each offset (10,0,0)
// relative to their parent and scaled 0.75 -- the reference's shrinking snake.
Scene buildChainScene()
{
    Scene scene;
    scene.nodes.push_back(SceneNode{-1, glm::vec3(10.0f, 0.0f, 0.0f), 0.75f, true});
    scene.root = 0;
    int parent = 0;
    for (int i = 0; i < 10; ++i) {
        scene.nodes.push_back(SceneNode{parent, glm::vec3(10.0f, 0.0f, 0.0f), 0.75f, true});
        parent = static_cast<int>(scene.nodes.size()) - 1;
    }
    return scene;
}

// grid: 20x20 planets, 10 units apart, all direct children of an invisible root.
// The root carries no mesh, so it contributes no draw but still owns the frame
// of reference the children are positioned in.
Scene buildGridScene()
{
    Scene scene;
    scene.nodes.push_back(SceneNode{-1, glm::vec3(0.0f), 1.0f, false});
    scene.root = 0;
    for (int z = 0; z < 20; ++z) {
        for (int x = 0; x < 20; ++x) {
            SceneNode node;
            node.parent = 0;
            node.localPosition = glm::vec3(static_cast<float>(x) * 10.0f - 100.0f, 0.0f,
                                          static_cast<float>(z) * 10.0f - 100.0f);
            node.localScale = 1.0f;
            node.hasMesh = true;
            scene.nodes.push_back(node);
        }
    }
    return scene;
}

// Parent-before-child order is guaranteed by construction (children are appended
// after their parent), so one forward pass is enough -- no recursion, no stack, no
// visited set.  This is the same contract computeNodeWorldMatrices() implements
// for Model node trees, expressed for a scene graph that owns its own nodes.
void updateWorldMatrices(const Scene &scene, const std::vector<glm::mat4> &locals, std::vector<glm::mat4> &world)
{
    world.resize(locals.size());
    for (size_t i = 0; i < locals.size(); ++i) {
        const int parent = scene.nodes[i].parent;
        world[i] = parent >= 0 ? world[static_cast<size_t>(parent)] * locals[i] : locals[i];
    }
}

// Coverage plus unique-colour count from the dump: the most frequent 8-bit colour
// is the background, so coverage needs no knowledge of the clear value.
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

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);

    // ----- Load the instanced mesh -----------------------------------------
    // No flattenScene and no normalizeSize: the scene graph supplies the
    // transforms, and culling bounds have to describe the mesh in its own space.
    ModelLoadOptions loadOptions;
    loadOptions.flattenScene = false;
    loadOptions.normalizeSize = false;
    const Model model = loadModel(kPlanetPath, loadOptions);
    const Geometry geometry = buildGeometry(model);

    Scene scene = options.mode == "chain" ? buildChainScene() : buildGridScene();
    scene.baseLocals.resize(scene.nodes.size());
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        scene.baseLocals[i] = localMatrix(scene.nodes[i], 0.0f);
    }

    uint32_t meshNodeCount = 0;
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        if (scene.nodes[i].hasMesh) {
            ++meshNodeCount;
        }
        // updateWorldMatrices() is one forward pass, which only works when a
        // parent comes before its children.  Both builders satisfy that by
        // construction; checking it here turns a future scene edit into an
        // exception instead of a silently wrong hierarchy.
        if (scene.nodes[i].parent >= static_cast<int>(i)) {
            throw std::runtime_error("scene_graph: node " + std::to_string(i) + " precedes its parent");
        }
    }

    // ----- Camera ----------------------------------------------------------
    // Both positions are picked so the interesting part of the scene sits inside
    // the fixed 0.1 .. 100 far plane of MakeCameraMatrix, which is what keeps the
    // far plane out of this example's list of variables.
    //   grid : 14 units up, 90 units out along +Z, looking down -Z and 8 degrees
    //          down -- about eleven rows of planets are in front of the camera
    //          and the other nine are behind it, which is the point.
    //   chain: the snake runs along +X from 10 to ~47 units, shrinking by 0.75
    //          per level, so the camera stands 28 units back along +Z and looks
    //          slightly down to fit its 37-unit width across the frame.
    glm::vec3 eye;
    float pitch = 0.0f;
    if (options.mode == "chain") {
        eye = glm::vec3(27.0f, 7.0f, 28.0f);
        pitch = -13.0f;
    } else {
        eye = glm::vec3(0.0f, 14.0f, 90.0f);
        pitch = -8.0f;
    }
    auto camera = std::make_shared<Camera>(eye, glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, pitch);
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    std::cout << "[scene_graph] mode " << options.mode << ": " << scene.nodes.size() << " node(s), "
              << meshNodeCount << " drawable, culling " << (options.cull ? "on" : "off") << std::endl;
    std::cout << "[scene_graph] mesh " << geometry.vertices.size() << " vertices, " << geometry.indices.size() / 3
              << " triangles, AABB centre (" << geometry.boundsCenter.x << "," << geometry.boundsCenter.y << ","
              << geometry.boundsCenter.z << ") extent (" << geometry.boundsExtent.x << ","
              << geometry.boundsExtent.y << "," << geometry.boundsExtent.z << ")" << std::endl;

    // ----- Render pass and pipeline ----------------------------------------
    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(),
        mgr->DefaultDepthFormat()));

    // Culling stays off at the raster level: an OBJ importer gives no winding
    // guarantee, same reasoning as model_loading.
    GraphicsPipelineConfig pipelineConfig;
    pipelineConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    pipelineConfig.cullMode = vk::CullModeFlagBits::eNone;

    const MaterialTexture texture = makeTexture(model, geometry.materialIndex);
    auto layout = makeLayout(texture.spec, meshNodeCount);
    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCENE_VERT_SPV.begin(),
                                                           shader::SCENE_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SCENE_FRAG_SPV.begin(),
                                                           shader::SCENE_FRAG_SPV.end()));

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eUV, VertexComponent::eNormal},
        static_cast<uint32_t>(geometry.vertices.size()), static_cast<uint32_t>(geometry.indices.size()),
        DataTypes::eUnsignedInt, false, 0u, meshNodeCount);
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, pipelineConfig);

    auto upload = mgr->UploadSequence();
    auto sync = std::make_shared<CmdSyncDevice>(asset);
    sync->OnUpdateRawData(ResourceTypes::eVertex, geometry.vertices);
    sync->OnUpdateRawData(ResourceTypes::eIndex, geometry.indices);
    sync->OnUpdateRawData(ResourceId(0, 0), 0u, texture.pixels);
    upload->submit(sync, asset);

    auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);

    std::cout << "[scene_graph] diffuse map '" << texture.name << "' (" << texture.spec.width << "x"
              << texture.spec.height << ", " << texture.spec.miplevels << " mips), one instanced draw"
              << std::endl;

    // ----- Frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();

    std::vector<glm::mat4> locals = scene.baseLocals;
    std::vector<glm::mat4> world;
    std::vector<glm::mat4> visibleMatrices;
    visibleMatrices.reserve(meshNodeCount);

    bool dumpPending = false;
    bool dumpDone = false;
    uint32_t renderedFrames = 0;
    bool resizeRequested = false;
    size_t lastVisible = 0;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (options.resizeAfter != 0 && !resizeRequested && renderedFrames >= options.resizeAfter) {
            resizeRequested = true;
            window.resize(1024, 600);
            std::cout << "[scene_graph] requested resize to 1024x600" << std::endl;
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

        if (options.dump && !dumpDone && !dumpPending && renderedFrames >= options.dumpAfter) {
            dumpPending = true;
        }

        // ----- Animate -----------------------------------------------------
        // Only the chain spins; a spinning grid would rotate the culling result
        // with it and make two runs hard to compare.  --time freezes the yaw so a
        // dump is reproducible.
        const float seconds = options.hasTime ? options.time : currentTime;
        const float yaw = options.mode == "chain" ? glm::radians(seconds * 20.0f) : 0.0f;
        locals[static_cast<size_t>(scene.root)] = localMatrix(scene.nodes[static_cast<size_t>(scene.root)], yaw);
        updateWorldMatrices(scene, locals, world);

        // ----- Cull --------------------------------------------------------
        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);
        const frustum::Frustum frustum =
            frustum::fromViewProjection(cameraMatrix.proj * cameraMatrix.view);
        visibleMatrices.clear();
        for (size_t i = 0; i < scene.nodes.size(); ++i) {
            if (!scene.nodes[i].hasMesh) {
                continue;
            }
            if (options.cull) {
                const frustum::WorldBox box =
                    frustum::transformBox(world[i], geometry.boundsCenter, geometry.boundsExtent);
                if (!frustum::intersects(frustum, box.center, box.extent)) {
                    continue;
                }
            }
            visibleMatrices.push_back(world[i]);
        }
        lastVisible = visibleMatrices.size();
        // An empty survivor list is a legal frame: instanceCount 0 draws nothing.
        pipeline->setInstanceCount(static_cast<uint32_t>(visibleMatrices.size()));
        if (!visibleMatrices.empty()) {
            draw->OnUpdateRawData(ResourceId(3, 0), frame, visibleMatrices);
        }
        draw->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);

        sequence->beginRecording();
        sequence->record(sync);
        sequence->recordPass(renderPass, [&]() { sequence->record(draw); });
        sequence->endRecording();
        sequence->submitFrame();

        if (dumpPending) {
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
                                   mgr->getGraphicsQueue(), *mgr->getQueueFamilyIndices().graphicsFamily);

            const std::vector<vk::Image> &swapImages = mgr->getSwapChain()->getImages();
            const uint32_t imageIndex = sequence->getImageIndex();
            if (imageIndex >= swapImages.size()) {
                throw std::runtime_error("scene_graph: swapchain image index out of range");
            }
            DumpRequest request;
            request.label = "scene";
            request.image = swapImages[imageIndex];
            request.format = *mgr->getSwapChain()->getImageFormat();
            request.extent = extent;
            request.layout = vk::ImageLayout::ePresentSrcKHR;
            request.encoding = DumpEncoding::eColor;
            const auto results = DumpImages(readback, {request}, "dump", 0);

            float coverage = 0.0f;
            size_t uniqueColors = 0;
            analyseFrame(results[0].rgb8, coverage, uniqueColors);
            std::cout << "[dump] " << results[0].path.string() << " visible=" << lastVisible << "/"
                      << meshNodeCount << " cull=" << (options.cull ? 1 : 0) << " coverage=" << coverage
                      << " uniqueColors=" << uniqueColors << " mean=(" << results[0].meanColor.r << ","
                      << results[0].meanColor.g << "," << results[0].meanColor.b << ")" << std::endl;

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

    if (!options.dump) {
        std::cout << "[scene_graph] last frame drew " << lastVisible << " of " << meshNodeCount << " entities"
                  << std::endl;
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
