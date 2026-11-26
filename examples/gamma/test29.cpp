// gamma: sRGB texture upload + gamma-encoded lighting, migrated from
// LearnOpenGL/src/5.advanced_lighting/2.gamma_correction.
//
// The reference toggles two things at once: it swaps a GL_SRGB texture for a
// plain GL_RGB texture and changes the attenuation from 1/d^2 to 1/d, then
// applies pow(color, 1/2.2) only in the gamma path.  Here the same PNG bytes
// are uploaded as two Vulkan textures differing only in format (eR8G8B8A8Srgb
// vs eR8G8B8A8Unorm) and the fragment shader switches both paths with one UBO
// flag.  That makes the difference measurable in a dump instead of requiring
// a keypress at runtime.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "Geometry.h"
#include "RenderCommon.h"
#include "TextureLoader.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "ImageDump.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "shader/gamma_frag.hpp"
#include "shader/gamma_vert.hpp"

namespace {

constexpr float kNearPlane = 0.1f;
constexpr float kFarPlane = 100.0f;

struct GammaUBO {
    alignas(16) glm::mat4 viewProj{1.0f};
    alignas(16) glm::vec4 eye{0.0f};
    alignas(16) glm::vec4 params{0.0f};
};
static_assert(sizeof(GammaUBO) == 96, "GammaUBO must match the std140 GammaBlock");

struct Instance {
    alignas(16) glm::mat4 model{1.0f};
    alignas(16) glm::vec4 material{1.0f};
};
static_assert(sizeof(Instance) == 80, "Instance must be model(64) + material(16)");

enum class GammaBlockComponent { eGamma };
using GammaBlockConfig = DescriptorComponentSpec<GammaBlockComponent>;
inline constexpr auto GammaBlockComponentTable = std::make_tuple(GammaBlockConfig{
    GammaBlockComponent::eGamma, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment});

enum class GammaTextureComponent { eLinear, eSrgb };
using GammaTextureConfig = DescriptorComponentSpec<GammaTextureComponent>;
inline constexpr auto GammaTextureComponentTable = std::make_tuple(
    GammaTextureConfig{GammaTextureComponent::eLinear, vk::DescriptorType::eCombinedImageSampler, 1,
                       vk::ShaderStageFlagBits::eFragment},
    GammaTextureConfig{GammaTextureComponent::eSrgb, vk::DescriptorType::eCombinedImageSampler, 1,
                       vk::ShaderStageFlagBits::eFragment});

enum class GammaInstanceComponent { eInstances };
using GammaInstanceConfig = DescriptorComponentSpec<GammaInstanceComponent>;
inline constexpr auto GammaInstanceComponentTable = std::make_tuple(GammaInstanceConfig{
    GammaInstanceComponent::eInstances, vk::DescriptorType::eStorageBuffer, 1,
    vk::ShaderStageFlagBits::eVertex});

struct Options {
    bool dump = false;
    bool noGamma = false;
    uint32_t dumpAfter = 3;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--no-gamma") {
            options.noGamma = true;
        } else if (arg == "--dump-after" && i + 1 < argc) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else {
            throw std::runtime_error("gamma: unknown argument " + arg);
        }
    }
    return options;
}

std::vector<Instance> makeScene()
{
    std::vector<Instance> instances;
    glm::mat4 floorModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.25f, 0.0f));
    floorModel = glm::scale(floorModel, glm::vec3(10.0f, 0.5f, 10.0f));
    instances.push_back({floorModel, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)});

    struct Light {
        glm::vec3 position;
        float intensity;
    };
    static const Light lights[] = {
        {glm::vec3(-3.0f, 1.0f, 0.0f), 0.25f},
        {glm::vec3(-1.0f, 1.0f, 0.0f), 0.50f},
        {glm::vec3(1.0f, 1.0f, 0.0f), 0.75f},
        {glm::vec3(3.0f, 1.0f, 0.0f), 1.00f},
    };
    for (const Light &light : lights) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), light.position);
        model = glm::scale(model, glm::vec3(0.12f));
        instances.push_back({model, glm::vec4(glm::vec3(light.intensity), 1.0f)});
    }
    return instances;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 2.2f, 6.5f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f,
                                           -18.0f);
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Format depthFormat = mgr->DefaultDepthFormat();
    const std::vector<Vertex> vertices = CubeVertices(glm::vec3(1.0f));
    const std::vector<Instance> instances = makeScene();
    const uint32_t instanceCount = static_cast<uint32_t>(instances.size());
    const LoadedTexture texture = loadTexture("assets/textures/container2.png");

    std::cout << "[gamma] " << instanceCount << " instances, 1 floor + " << (instanceCount - 1)
              << " light markers, " << texture.width << "x" << texture.height << " texture, gamma "
              << (options.noGamma ? "off" : "on") << "\n";

    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), depthFormat));

    // ----- descriptor layouts ----------------------------------------------
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto blockSet = DescriptorLayoutAtlas::makeSet<GammaBlockComponent, GammaBlockComponentTable>(
        0, {GammaBlockComponent::eGamma}, UpdateFrequency::eEachFrame, false);
    blockSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(GammaUBO)), DataTypes::eCustom};

    auto textureSet = DescriptorLayoutAtlas::makeSet<GammaTextureComponent, GammaTextureComponentTable>(
        1, {GammaTextureComponent::eLinear, GammaTextureComponent::eSrgb}, UpdateFrequency::eStatic, true);
    // Same bytes, two views: the GPU does the sRGB -> linear decode on fetch.
    textureSet.specs[0] = TextureImageSpec{static_cast<uint32_t>(texture.width),
                                           static_cast<uint32_t>(texture.height),
                                           static_cast<uint32_t>(texture.channels), texture.mipLevels};
    textureSet.specs[1] = textureSet.specs[0];
    std::get<TextureImageSpec>(textureSet.specs[0]).format = vk::Format::eR8G8B8A8Unorm;
    std::get<TextureImageSpec>(textureSet.specs[1]).format = vk::Format::eR8G8B8A8Srgb;

    auto instanceSet = DescriptorLayoutAtlas::makeSet<GammaInstanceComponent, GammaInstanceComponentTable>(
        2, {GammaInstanceComponent::eInstances}, UpdateFrequency::eStatic, true);
    instanceSet.specs[0] = GBufferSpec{instanceCount, static_cast<uint32_t>(sizeof(Instance)), DataTypes::eCustom};
    layout->setDetails({blockSet, textureSet, instanceSet});

    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::GAMMA_VERT_SPV.begin(),
                                                             shader::GAMMA_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::GAMMA_FRAG_SPV.begin(),
                                                             shader::GAMMA_FRAG_SPV.end()));

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal, VertexComponent::eUV},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, instanceCount);
    GraphicsPipelineConfig config;
    config.cullMode = vk::CullModeFlagBits::eNone; // the floor is seen from above
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, config);

    auto upload = mgr->UploadSequence();
    {
        auto sync = std::make_shared<CmdSyncDevice>(asset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
        sync->OnUpdateRawData(ResourceId(1, 0), 0u, texture.pixels);
        sync->OnUpdateRawData(ResourceId(1, 1), 0u, texture.pixels);
        sync->OnUpdateRawData(ResourceId(2, 0), 0u, instances);
        upload->submit(sync, asset);
    }

    auto sequence = mgr->DrawFrameSequence();
    auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    uint32_t framesRendered = 0;
    bool dumpDone = false;
    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height > 0 ? extent.height : 1);

        const glm::mat4 view = camera->GetViewMatrix();
        glm::mat4 projection = glm::perspective(glm::radians(camera->Zoom), aspect, kNearPlane, kFarPlane);
        projection[1][1] *= -1.0f;

        GammaUBO ubo;
        ubo.viewProj = projection * view;
        ubo.eye = glm::vec4(camera->Position, 1.0f);
        ubo.params = glm::vec4(options.noGamma ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f);
        draw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(renderPass, [&]() { sequence->record(draw); });
        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            auto readback = ImageReadback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                          mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                          *mgr->getQueueFamilyIndices().graphicsFamily);
            DumpRequest request;
            request.label = options.noGamma ? "frame-no-gamma" : "frame-gamma";
            request.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            request.format = *mgr->getSwapChain()->getImageFormat();
            request.extent = extent;
            request.layout = vk::ImageLayout::ePresentSrcKHR;
            request.encoding = DumpEncoding::eColor;
            const auto results = DumpImages(readback, {request}, "dump", 0);
            for (const DumpResult &result : results) {
                std::cout << "[dump] " << result.path.string() << " mean=(" << result.meanColor.r << ","
                          << result.meanColor.g << "," << result.meanColor.b << ")\n";
            }
            dumpDone = true;
            if (!sequence->presentFrame()) {
                continue;
            }
            break;
        }

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
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
