// instancing (test40) -- migrated from Vulkan-Samples/examples/instancing
//
// N cubes are drawn in a single draw call.  Their transforms live in a second
// vertex buffer bound at binding 1 with VK_VERTEX_INPUT_RATE_INSTANCE, not in an
// SSBO: this is the framework's first multi-binding pipeline (see 28.3).
// --verify reads that buffer back and compares it with the CPU array.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "Geometry.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "ImageDump.h"
#include "ImageReadback.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "shader/instancing_frag.hpp"
#include "shader/instancing_vert.hpp"

namespace {

constexpr uint32_t kWidth = 800;
constexpr uint32_t kHeight = 600;
constexpr uint32_t kMaxInstances = 4096;
constexpr uint32_t kDefaultInstances = 512;
constexpr uint32_t kSeedDefault = 20221127;

struct Options {
    bool dump = false;
    bool stats = false;
    bool verify = false;
    uint32_t frames = 120;
    uint32_t dumpAfter = 120;
    uint32_t instances = kDefaultInstances;
    uint32_t seed = kSeedDefault;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("instancing: " + arg + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--stats") {
            options.stats = true;
        } else if (arg == "--verify") {
            options.verify = true;
        } else if (arg == "--frames") {
            options.frames = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--dump-after") {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--instances") {
            options.instances = static_cast<uint32_t>(std::stoul(next()));
            if (options.instances == 0 || options.instances > kMaxInstances) {
                throw std::runtime_error("instancing: --instances expects 1..4096");
            }
        } else if (arg == "--seed") {
            options.seed = static_cast<uint32_t>(std::stoul(next()));
        } else {
            throw std::runtime_error("instancing: unknown argument " + arg);
        }
    }
    return options;
}

// Fixed-seed layout so two runs produce byte-identical dumps.  The whole
// kMaxInstances array is uploaded once; --instances only changes the draw count,
// so switching N never re-uploads the buffer.
std::vector<InstanceData> makeInstances(uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::vector<InstanceData> result(kMaxInstances);
    for (auto &instance : result) {
        const float angle = unit(rng) * 6.2831853f;
        const float radius = 2.0f + unit(rng) * 7.0f;
        instance.position = glm::vec3(std::cos(angle) * radius, (unit(rng) - 0.5f) * 9.0f,
                                      (unit(rng) - 0.5f) * 8.0f);
        instance.rotation = glm::vec3(0.0f, unit(rng) * 6.2831853f, 0.0f);
        const float scale = 0.18f + unit(rng) * 0.22f;
        instance.scale = glm::vec3(scale);
        instance.color = glm::vec3(0.35f + unit(rng) * 0.65f, 0.35f + unit(rng) * 0.65f,
                                   0.35f + unit(rng) * 0.65f);
    }
    return result;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 18.0f));
    GLFWwindow window(kWidth, kHeight, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Format swapFormat = *mgr->getSwapChain()->getImageFormat();
    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, swapFormat, mgr->DefaultDepthFormat()));

    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        0, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};
    layout->setDetails({cameraSet});

    const std::vector<Vertex> vertices = CubeVertices(glm::vec3(1.0f));
    // Binding 0: per-vertex mesh.  Binding 1: per-instance transform/color.
    // instanceVertexCount is the *capacity*, so --instances can shrink the draw
    // without reallocating the device buffer.
    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor, VertexComponent::eUV,
                                     VertexComponent::eNormal},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, options.instances,
        std::vector<VertexInstanceComponent>{VertexInstanceComponent::ePosition, VertexInstanceComponent::eRotation,
                                             VertexInstanceComponent::eScale, VertexInstanceComponent::eColor},
        kMaxInstances);

    auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::INSTANCING_VERT_SPV.begin(),
                                                            shader::INSTANCING_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::INSTANCING_FRAG_SPV.begin(),
                                                            shader::INSTANCING_FRAG_SPV.end()));
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0);
    auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    const std::vector<InstanceData> instances = makeInstances(options.seed);
    {
        auto upload = mgr->UploadSequence();
        auto sync = std::make_shared<CmdSyncDevice>(asset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
        sync->OnUpdateRawData(ResourceTypes::eInstanceVertex, instances);
        upload->submit(sync, asset);
    }

    if (options.verify) {
        // Input resources (VBO / IBO / instance VBO) had no readback path before
        // this example; OnceSequence::ReadBack(asset, ResourceTypes, ...) is the
        // new one, mirroring the descriptor-resource version.
        auto probe = mgr->UploadSequence();
        const size_t expected = static_cast<size_t>(options.instances) * sizeof(InstanceData);
        const std::vector<std::byte> bytes = probe->ReadBack(asset, ResourceTypes::eInstanceVertex, expected);
        const bool same = bytes.size() >= expected
                          && std::memcmp(bytes.data(), instances.data(), expected) == 0;
        std::cout << "[verify] instance VBO: " << options.instances << " x " << sizeof(InstanceData)
                  << " B, equals CPU array: " << (same ? "yes" : "NO") << "\n";
        if (!same) {
            throw std::runtime_error("instancing: the instance vertex buffer does not match the CPU array");
        }
    }

    auto sequence = mgr->DrawFrameSequence();
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
        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);
        draw->OnUpdateRawData(ResourceId(0, 0), frame, cameraMatrix);

        sequence->beginRecording();
        // Staging copies must stay outside the render pass.
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
            request.label = "frame";
            request.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            request.format = swapFormat;
            request.extent = extent;
            request.layout = vk::ImageLayout::ePresentSrcKHR;
            request.encoding = DumpEncoding::eColor;
            for (const DumpResult &result : DumpImages(readback, {request}, "dump", 0)) {
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
        if (!options.dump && framesRendered >= options.frames) {
            break;
        }
    }

    if (options.stats) {
        std::cout << "[stats] frames=" << framesRendered << " instances=" << options.instances
                  << " seed=" << options.seed << " capacity=" << kMaxInstances
                  << " bindings=2 (0:vertex, 1:instance)\n";
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
