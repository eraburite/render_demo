// multi-lighting: one render pass, two pipelines (lighting + light_cube)
// sharing that render pass, with per-layer descriptor sets.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "Geometry.h"
#include "RenderCommon.h"
#include "TextureLoader.h"

#include "AttachmentDetailConfig.h"
#include "BufferObject.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "shader/light_cube_frag.hpp"
#include "shader/light_cube_vert.hpp"
#include "shader/lighting_frag.hpp"
#include "shader/lighting_vert.hpp"

namespace {

// std140-compatible mirrors of the structs declared in lighting.frag.
struct DirectionalLightData {
    alignas(16) glm::vec4 direction;
    alignas(16) glm::vec4 color;
};
struct PointLightData {
    alignas(16) glm::vec4 position;
    alignas(16) glm::vec4 color;
    alignas(16) glm::vec4 attenuation;
};
struct SpotLightData {
    alignas(16) glm::vec4 position;
    alignas(16) glm::vec4 direction;
    alignas(16) glm::vec4 color;
    alignas(16) glm::vec4 params;
};

const glm::vec3 POINT_LIGHT_POSITIONS[4] = {
    glm::vec3(2.0f, 2.0f, 1.5f), glm::vec3(-2.0f, 1.5f, -1.0f), glm::vec3(1.5f, -1.5f, 2.0f),
    glm::vec3(-1.5f, -0.5f, -2.0f)};

LoadedTexture loadTextureAuto(const std::string &primary, const std::string &fallback)
{
    try {
        return loadTexture(primary);
    } catch (const std::exception &) {
        return loadTexture(fallback);
    }
}

std::vector<glm::mat4> makeModels(uint32_t count)
{
    std::vector<glm::mat4> models;
    models.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, glm::vec3(static_cast<float>(i % 5) * 1.6f - 3.2f,
                                                static_cast<float>((i / 5) % 2) * 1.6f - 0.8f, 0.0f));
        model = glm::rotate(model, glm::radians(20.0f * static_cast<float>(i)), glm::vec3(1.0f, 0.3f, 0.5f));
        models.push_back(model);
    }
    return models;
}

} // namespace

void test()
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);
    const vk::Extent2D displayExtent = *mgr->getSwapChain()->getExtent2D();

    auto vertices = CubeVertices(glm::vec3(1.0f));
    auto baseColor = loadTextureAuto("assets/textures/awesomeface.png", "build/textures/awesomeface.png");
    auto specular = loadTextureAuto("assets/textures/container2_specular.png",
                                    "build/textures/container2_specular.png");

    const std::vector<MaterialParams> materials{{0, 1, 0.8f, 0.2f}, {1, 1, 0.6f, 0.1f}};
    const auto cubeModels = makeModels(10);
    const auto lightModels = makeModels(4);

    std::vector<PointLightData> pointLights;
    for (const glm::vec3 &position : POINT_LIGHT_POSITIONS) {
        pointLights.push_back(PointLightData{glm::vec4(position, 1.0f), glm::vec4(0.7f, 0.7f, 0.6f, 1.0f),
                                             glm::vec4(0.09f, 0.032f, 0.0f, 0.0f)});
    }

    // ---- descriptor layout (one atlas shared by both pipelines) ----------
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor, GlobalComponent::eSpecularMap, GlobalComponent::eMaterialParams},
        UpdateFrequency::eStatic, true);
    globalSet.specs[0] = TextureImageSpec{static_cast<uint32_t>(baseColor.width),
                                          static_cast<uint32_t>(baseColor.height), static_cast<uint32_t>(baseColor.channels),
                                          (baseColor.mipLevels)};
    globalSet.specs[1] = TextureImageSpec{static_cast<uint32_t>(specular.width),
                                          static_cast<uint32_t>(specular.height), static_cast<uint32_t>(specular.channels),
                                          (specular.mipLevels)};
    globalSet.specs[2] = GBufferSpec{static_cast<uint32_t>(materials.size()), static_cast<uint32_t>(sizeof(MaterialParams)),
                                     DataTypes::eCustom};

    auto environmentSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    environmentSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};

    // Binding order must match the shader: eye(0), light(1), directional(2),
    // pointLights(3), spot(4).
    auto frameSet = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2,
        {FrameComponent::eViewEyePosition, FrameComponent::eLightSource, FrameComponent::eDirectionalLight,
         FrameComponent::ePointLights, FrameComponent::eSpotlight},
        UpdateFrequency::eEachFrame, false);
    frameSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ViewEyePoint)), DataTypes::eCustom};
    frameSet.specs[1] = GBufferSpec{1, static_cast<uint32_t>(sizeof(LightSource)), DataTypes::eCustom};
    frameSet.specs[2] = GBufferSpec{1, static_cast<uint32_t>(sizeof(DirectionalLightData)), DataTypes::eCustom};
    frameSet.specs[3] = GBufferSpec{static_cast<uint32_t>(pointLights.size()), static_cast<uint32_t>(sizeof(PointLightData)),
                                    DataTypes::eCustom};
    frameSet.specs[4] = GBufferSpec{1, static_cast<uint32_t>(sizeof(SpotLightData)), DataTypes::eCustom};

    auto instanceSet = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        3, {InstanceComponent::eModelMatrix}, UpdateFrequency::eEachInstance, false);
    instanceSet.specs[0] = GBufferSpec{static_cast<uint32_t>(cubeModels.size()), static_cast<uint32_t>(sizeof(glm::mat4)),
                                       DataTypes::eCustom};

    layout->setDetails({globalSet, environmentSet, frameSet, instanceSet});
    layout->setPushConstant(static_cast<uint32_t>(sizeof(ObjectParams)), vk::ShaderStageFlagBits::eVertex);

    // ---- two independent assets over the same layout ---------------------
    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor, VertexComponent::eUV,
                                     VertexComponent::eNormal},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u);

    auto lightingAsset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    lightingAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::LIGHTING_VERT_SPV.begin(),
                                                                    shader::LIGHTING_VERT_SPV.end()));
    lightingAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::LIGHTING_FRAG_SPV.begin(),
                                                                    shader::LIGHTING_FRAG_SPV.end()));

    auto lightAsset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    lightAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::LIGHT_CUBE_VERT_SPV.begin(),
                                                                 shader::LIGHT_CUBE_VERT_SPV.end()));
    lightAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::LIGHT_CUBE_FRAG_SPV.begin(),
                                                                 shader::LIGHT_CUBE_FRAG_SPV.end()));

    // The light pipeline only needs four instance matrices: replace the
    // auto-created ten-matrix buffer by a dedicated four-matrix one.
    const std::vector<uint32_t> sharedFamilies = mgr->sharedQueueFamilies();
    auto lightInstanceBuffer = std::make_shared<BufferObject>(
        mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
        ResourceTypes::eStorage, MemoryTypes::eDevice,
        GBufferSpec{static_cast<uint32_t>(lightModels.size()), static_cast<uint32_t>(sizeof(glm::mat4)), DataTypes::eCustom},
        &sharedFamilies);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        lightAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(3, 0), frame), lightInstanceBuffer);
    }

    // ---- one shared render pass, two pipelines ---------------------------
    auto detail = std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eMSAA, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat());
    auto renderPass = mgr->CreateRenderPass(detail);
    auto lightingPipeline = mgr->CreatePipeline(vertexInput, layout, lightingAsset, renderPass, 0);
    auto lightPipeline = mgr->CreatePipeline(vertexInput, layout, lightAsset, renderPass, 0);

    // ---- one-shot upload -------------------------------------------------
    auto upload = mgr->UploadSequence();
    auto lightingSync = std::make_shared<CmdSyncDevice>(lightingAsset);
    lightingSync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    lightingSync->OnUpdateRawData(ResourceId(0, 0), 0u, baseColor.pixels);
    lightingSync->OnUpdateRawData(ResourceId(0, 1), 0u, specular.pixels);
    lightingSync->OnUpdateRawData(ResourceId(0, 2), 0u, materials);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        lightingSync->OnUpdateRawData(ResourceId(3, 0), frame, cubeModels);
    }
    upload->submit(lightingSync, lightingAsset);

    auto lightSync = std::make_shared<CmdSyncDevice>(lightAsset);
    lightSync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    lightSync->OnUpdateRawData(ResourceId(0, 0), 0u, baseColor.pixels);
    lightSync->OnUpdateRawData(ResourceId(0, 1), 0u, specular.pixels);
    lightSync->OnUpdateRawData(ResourceId(0, 2), 0u, materials);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        lightSync->OnUpdateRawData(ResourceId(3, 0), frame, lightModels);
    }
    upload->submit(lightSync, lightAsset);

    // ---- frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto lightingDraw = std::make_shared<CmdOpDrawFrame>(lightingPipeline);
    auto lightDraw = std::make_shared<CmdOpDrawFrame>(lightPipeline);
    auto lightingFrameSync = std::make_shared<CmdSyncDevice>(lightingAsset);
    auto lightFrameSync = std::make_shared<CmdSyncDevice>(lightAsset);

    while (!window.shouldClose()) {
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();

        auto cameraMatrix = MakeCameraMatrix(*camera, static_cast<float>(displayExtent.width),
                                             static_cast<float>(displayExtent.height));
        ViewEyePoint eye{};
        eye.position = MakeEyePoint(*camera);
        LightSource light{};
        light.position = glm::vec4(1.2f, 1.0f, 2.0f, 1.0f);
        light.ambient = glm::vec4(0.05f, 0.05f, 0.05f, 1.0f);
        light.diffuse = glm::vec4(0.55f, 0.55f, 0.5f, 1.0f);
        light.specular = glm::vec4(0.8f, 0.8f, 0.75f, 1.0f);
        DirectionalLightData directional{glm::vec4(-0.4f, -1.0f, -0.6f, 0.0f),
                                         glm::vec4(0.25f, 0.25f, 0.3f, 1.0f)};
        SpotLightData spot{glm::vec4(camera->Position, 1.0f), glm::vec4(camera->Front, 0.0f),
                           glm::vec4(0.9f, 0.9f, 0.8f, 1.0f),
                           glm::vec4(0.0f, glm::cos(glm::radians(18.0f)), 0.0f, 0.0f)};
        auto model = MakeSpinningModel(currentTime);

        auto updateDynamic = [&](const std::shared_ptr<CmdOpDrawFrame> &op) {
            op->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);
            op->OnUpdateRawData(ResourceId(2, 0), frame, eye);
            op->OnUpdateRawData(ResourceId(2, 1), frame, light);
            op->OnUpdateRawData(ResourceId(2, 2), frame, directional);
            op->OnUpdateRawData(ResourceId(2, 3), frame, pointLights);
            op->OnUpdateRawData(ResourceId(2, 4), frame, spot);
            op->OnUpdateRawData(ResourceTypes::ePushConst, frame, model);
        };
        updateDynamic(lightingDraw);
        updateDynamic(lightDraw);

        sequence->beginRecording();
        sequence->record(lightingFrameSync);
        sequence->record(lightFrameSync);
        sequence->recordPass(renderPass, [&]() {
            sequence->record(lightingDraw);
            sequence->record(lightDraw);
        });
        sequence->endRecording();
        sequence->submitFrame();
        if (!sequence->presentFrame()) {
            continue;
        }
    }
}

int main()
{
    try {
        test();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
