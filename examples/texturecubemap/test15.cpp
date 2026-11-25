// texturecubemap: cube map skybox plus reflective objects (reference:
// examples/texturecubemap).  Both pipelines share one render pass and one
// descriptor layout; the skybox is a cube centred on the camera, the objects
// sample the same cube map through their reflection vector.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ModelLoader.h"
#include "KtxLoader.h"
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

#include "shader/reflect_frag.hpp"
#include "shader/reflect_vert.hpp"
#include "shader/skybox_frag.hpp"
#include "shader/skybox_vert.hpp"

namespace {

// The skybox cube is scaled up around the camera position.  It must stay in
// front of the far plane (100 in MakeCameraMatrix), and a half extent of 10
// keeps the farthest corner at 10 * sqrt(3) ~ 17.
constexpr float SKYBOX_SCALE = 10.0f;

// std430-compatible mirror of PushBlock in skybox.vert / reflect.vert.
// Deliberately stays at 68 bytes: Vulkan only guarantees 128 bytes of push
// constant storage, and an inverse model matrix on top of `model` would push
// this to 144 and break devices at the guaranteed minimum.
struct PushBlock {
    glm::mat4 model;
    float lodBias;
};

struct EngineModel {
    std::vector<Vertex> vertices; // expanded: one entry per index
};

// Same conversion as the gltf examples: centre, fit into a ~2 unit box and
// expand the index list so the draw needs no index buffer.  The model matrix
// applied later places/scales the result.
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
        dst.tangent = src.tangent;
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

// Local helper: the engine vertex stream this example draws with, converted
// from the loader-neutral model.  Named differently from the global
// loadModel() so the name does not shadow it.
EngineModel loadEngineModel(const std::string &path)
{
    Model gltf = loadModel(path);
    if (!gltf.valid()) {
        throw std::runtime_error("texturecubemap: no triangle geometry in " + path);
    }
    return toEngineModel(gltf);
}

// One reflective object: its own vertex stream, pipeline and draw op (each
// mesh needs its own vertex buffer, so one asset/pipeline per object -- the
// same split multi-lighting uses for its two pipelines).
struct ReflectiveObject {
    std::string name;
    EngineModel model;
    glm::mat4 transform{1.0f};
    std::shared_ptr<GraphicsPipeline> pipeline;
    std::shared_ptr<CmdOpDrawFrame> draw;
};

} // namespace

void test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 1.0f, 6.5f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, -8.0f);
    GLFWwindow window(1024, 768, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ----- Assets ---------------------------------------------------------
    KtxTexture ktx = loadKtx(PROJECT_ROOT_PATH "/data/textures/cubemap_yokohama_rgba.ktx",
                             /*preferSrgb=*/false);
    if (ktx.faces != 6) {
        throw std::runtime_error("texturecubemap: the cube map asset has no 6 faces");
    }
    TextureImageSpec cubeSpec = ktx.imageSpec();

    EngineModel skybox = loadEngineModel(PROJECT_ROOT_PATH "/data/models/cube.gltf");

    std::vector<ReflectiveObject> objects;
    objects.push_back({"sphere", loadEngineModel(PROJECT_ROOT_PATH "/data/models/sphere.gltf"), glm::mat4(1.0f)});
    objects.push_back({"teapot", loadEngineModel(PROJECT_ROOT_PATH "/data/models/teapot.gltf"), glm::mat4(1.0f)});

    // The meshes are normalised to a 2 unit box, so a unit scale gives a
    // radius of ~1; +-1.8 keeps both objects apart and inside the 45 degree
    // frustum at z = 6.5.
    objects[0].transform = glm::translate(glm::mat4(1.0f), glm::vec3(-1.8f, 0.0f, 0.0f));
    objects[1].transform = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(1.8f, 0.0f, 0.0f)),
                                       glm::radians(-25.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    std::cout << "[texturecubemap] cubemap " << ktx.width << "x" << ktx.height << ", " << ktx.faces << " faces, "
              << ktx.mipLevels << " mips, " << ktx.byteSize() << " bytes\n";
    std::cout << "[texturecubemap] skybox " << skybox.vertices.size() << " verts, " << objects[0].name << " "
              << objects[0].model.vertices.size() << " verts, " << objects[1].name << " "
              << objects[1].model.vertices.size() << " verts\n";

    // ----- One descriptor layout for both pipelines -----------------------
    // set0: cube map (fragment, static/shared).  set1: camera (vertex, per
    // frame).  The ids have to stay dense, and every UBO/sampler is declared in
    // the shader stage its table row names.
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor}, UpdateFrequency::eStatic, true);
    globalSet.specs[0] = cubeSpec;

    auto environmentSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    environmentSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};

    layout->setDetails({globalSet, environmentSet});
    layout->setPushConstant(static_cast<uint32_t>(sizeof(PushBlock)),
                            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

    // ----- Shared render pass --------------------------------------------
    auto detail = std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eMSAA, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat());
    auto renderPass = mgr->CreateRenderPass(detail);

    // Skybox: drawn first, culls front faces (the viewer sits inside the cube),
    // never writes depth and only passes the `depth == clear` test.
    auto skyboxVertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition},
        static_cast<uint32_t>(skybox.vertices.size()));
    auto skyboxAsset = mgr->CreateShaderAssetGroup(skyboxVertexInput, layout);
    skyboxAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SKYBOX_VERT_SPV.begin(),
                                                                  shader::SKYBOX_VERT_SPV.end()));
    skyboxAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SKYBOX_FRAG_SPV.begin(),
                                                                  shader::SKYBOX_FRAG_SPV.end()));

    GraphicsPipelineConfig skyboxConfig;
    skyboxConfig.cullMode = vk::CullModeFlagBits::eFront;
    skyboxConfig.depthWriteEnable = false;
    skyboxConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    auto skyboxPipeline = mgr->CreatePipeline(skyboxVertexInput, layout, skyboxAsset, renderPass, 0, skyboxConfig);
    auto skyboxDraw = std::make_shared<CmdOpDrawFrame>(skyboxPipeline);

    // Reflective objects: back-face culling, depth test + write.
    for (ReflectiveObject &object : objects) {
        auto vertexInput = std::make_shared<VertexInputAtlas>(
            0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal},
            static_cast<uint32_t>(object.model.vertices.size()));
        auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
        asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::REFLECT_VERT_SPV.begin(),
                                                                shader::REFLECT_VERT_SPV.end()));
        asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::REFLECT_FRAG_SPV.begin(),
                                                                shader::REFLECT_FRAG_SPV.end()));

        GraphicsPipelineConfig config;
        config.cullMode = vk::CullModeFlagBits::eBack;
        object.pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, config);
        object.draw = std::make_shared<CmdOpDrawFrame>(object.pipeline);

        // Upload geometry + the cube map into this pipeline's own asset.
        auto upload = mgr->UploadSequence();
        auto syncOp = std::make_shared<CmdSyncDevice>(asset);
        syncOp->OnUpdateRawData(ResourceTypes::eVertex, object.model.vertices);
        syncOp->OnUpdateRawData(ResourceId(0, 0), 0u, ktx.data);
        upload->submit(syncOp, asset);
    }

    // ----- Skybox upload ---------------------------------------------------
    {
        auto upload = mgr->UploadSequence();
        auto syncOp = std::make_shared<CmdSyncDevice>(skyboxAsset);
        syncOp->OnUpdateRawData(ResourceTypes::eVertex, skybox.vertices);
        syncOp->OnUpdateRawData(ResourceId(0, 0), 0u, ktx.data);
        upload->submit(syncOp, skyboxAsset);
    }

    // ----- Frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto frameSync = std::make_shared<CmdSyncDevice>(skyboxAsset);

    float lodBias = 0.0f;
    std::cout << "[texturecubemap] +/- changes the LOD bias (current " << lodBias << ")\n";
    // The startup banner is flushed so a redirected stdout still carries it
    // even when the process is terminated from the outside.
    std::cout.flush();

    bool plusHeld = false;
    bool minusHeld = false;
    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        auto justPressed = [](bool down, bool &previous) {
            const bool edge = down && !previous;
            previous = down;
            return edge;
        };
        if (justPressed(window.isKeyDown(GLFW_KEY_EQUAL) || window.isKeyDown(GLFW_KEY_KP_ADD), plusHeld)) {
            lodBias = std::min(lodBias + 0.5f, 8.0f);
            std::cout << "[texturecubemap] lod bias " << lodBias << std::endl;
        }
        if (justPressed(window.isKeyDown(GLFW_KEY_MINUS) || window.isKeyDown(GLFW_KEY_KP_SUBTRACT), minusHeld)) {
            lodBias = std::max(lodBias - 0.5f, 0.0f);
            std::cout << "[texturecubemap] lod bias " << lodBias << std::endl;
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, *mgr->getSwapChain()->getExtent2D());

        // Cube follows the camera so its faces are always equally far away.
        const glm::mat4 skyboxModel =
            glm::scale(glm::translate(glm::mat4(1.0f), camera->Position), glm::vec3(SKYBOX_SCALE));
        PushBlock skyboxPush{};
        skyboxPush.model = skyboxModel;
        skyboxPush.lodBias = lodBias;

        skyboxDraw->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);
        skyboxDraw->OnUpdateRawData(ResourceTypes::ePushConst, frame, skyboxPush);

        std::vector<PushBlock> objectPush;
        objectPush.reserve(objects.size());
        for (ReflectiveObject &object : objects) {
            PushBlock push{};
            push.model = object.transform;
            push.lodBias = lodBias;
            objectPush.push_back(push);

            object.draw->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);
            object.draw->OnUpdateRawData(ResourceTypes::ePushConst, frame, push);
        }

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(renderPass, [&]() {
            sequence->record(skyboxDraw);
            for (const ReflectiveObject &object : objects) {
                sequence->record(object.draw);
            }
        });
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
