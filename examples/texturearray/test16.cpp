// texturearray: one instanced draw of N spheres, each instance sampling its own
// slice of a 2D texture array (reference: examples/texturearray).  The instance
// index drives both the per-instance model matrix (set3 SSBO) and the array
// layer, so the two cannot drift apart.
#include <algorithm>
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

#include "shader/instancing_frag.hpp"
#include "shader/instancing_vert.hpp"

namespace {

struct EngineModel {
    std::vector<Vertex> vertices; // expanded: one entry per index
};

// Centre the mesh, fit it into a ~2 unit box and expand the indices into a
// plain vertex stream so the draw needs no index buffer -- same conversion as
// the gltf examples.
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

// One sphere per array layer, laid out in a row on the X axis and slightly
// rotated so the per-layer texture is easy to tell apart.
std::vector<glm::mat4> makeInstances(uint32_t count)
{
    const float spacing = 1.0f;
    const float first = -0.5f * spacing * static_cast<float>(count - 1);
    std::vector<glm::mat4> models;
    models.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                         glm::vec3(first + spacing * static_cast<float>(i), 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(18.0f * static_cast<float>(i)), glm::vec3(0.0f, 1.0f, 0.0f));
        // The mesh spans 2 units, so 0.45 leaves a small gap between neighbours.
        model = glm::scale(model, glm::vec3(0.45f));
        models.push_back(model);
    }
    return models;
}

} // namespace

void test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.8f, 7.5f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, -6.0f);
    GLFWwindow window(1024, 768, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ----- Assets ---------------------------------------------------------
    KtxTexture ktx = loadKtx(PROJECT_ROOT_PATH "/assets/textures/texturearray_rgba.ktx", /*preferSrgb=*/false);
    if (ktx.layers < 2) {
        throw std::runtime_error("texturearray: the asset does not contain an array texture");
    }
    const uint32_t layerCount = ktx.layers;
    TextureImageSpec arraySpec = ktx.imageSpec();

    Model gltf = loadModel(PROJECT_ROOT_PATH "/assets/models/sphere.gltf");
    if (!gltf.valid()) {
        throw std::runtime_error("texturearray: no triangle geometry in sphere.gltf");
    }
    EngineModel model = toEngineModel(gltf);
    const std::vector<glm::mat4> instances = makeInstances(layerCount);

    std::cout << "[texturearray] texture array " << ktx.width << "x" << ktx.height << ", " << ktx.layers
              << " layers, " << ktx.mipLevels << " mip, " << ktx.byteSize() << " bytes\n";
    std::cout << "[texturearray] sphere " << model.vertices.size() << " verts, " << layerCount << " instances\n";
    // Flushed so a redirected stdout still carries the banner even when the
    // process is terminated from the outside.
    std::cout.flush();

    // ----- Descriptor layout ---------------------------------------------
    // set0: texture array (fragment, static/shared).  set1: camera (vertex, per
    // frame).  set2: placeholder so set3 keeps the shared eEachInstance id.
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor}, UpdateFrequency::eStatic, true);
    globalSet.specs[0] = arraySpec;

    auto environmentSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    environmentSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};

    auto emptyFrameSet = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {}, UpdateFrequency::eEachFrame, false);

    auto instanceSet = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        3, {InstanceComponent::eModelMatrix}, UpdateFrequency::eEachInstance, false);
    instanceSet.specs[0] = GBufferSpec{layerCount, static_cast<uint32_t>(sizeof(glm::mat4)), DataTypes::eCustom};

    layout->setDetails({globalSet, environmentSet, emptyFrameSet, instanceSet});

    // ----- Pipeline -------------------------------------------------------
    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eUV},
        static_cast<uint32_t>(model.vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, layerCount);

    auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::INSTANCING_VERT_SPV.begin(),
                                                            shader::INSTANCING_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::INSTANCING_FRAG_SPV.begin(),
                                                            shader::INSTANCING_FRAG_SPV.end()));

    auto detail = std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eMSAA, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat());
    auto renderPass = mgr->CreateRenderPass(detail);
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0);

    // ----- Upload ---------------------------------------------------------
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, model.vertices);
    syncOp->OnUpdateRawData(ResourceId(0, 0), 0u, ktx.data);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        syncOp->OnUpdateRawData(ResourceId(3, 0), frame, instances);
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

        draw->OnUpdateRawData(ResourceId(1, 0), frame,
                              MakeCameraMatrix(*camera, *mgr->getSwapChain()->getExtent2D()));

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
