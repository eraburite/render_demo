// pipeline_statistics (test39) -- migrated from Vulkan-Samples/examples/pipelinestatistics
//
// A single VK_QUERY_TYPE_PIPELINE_STATISTICS query wraps the scene draws.  Six
// counters come back per frame; the input-assembly / vertex-shader ones are
// exact functions of the CPU-side draw list, so --verify checks them against
// the CPU prediction (see DESIGN.md 28.2).  Unlike occlusion queries this needs
// the pipelineStatisticsQuery device feature, requested through DeviceRequirement.
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
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
#include "CmdOpQuery.h"
#include "CmdSyncDevice.h"
#include "ImageDump.h"
#include "ImageReadback.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "QueryPool.h"
#include "RenderManager.h"

#include "shader/scene_frag.hpp"
#include "shader/scene_vert.hpp"

namespace {

constexpr uint32_t kWidth = 800;
constexpr uint32_t kHeight = 600;
constexpr uint32_t kMaxObjects = 4;
constexpr uint32_t kCubeVertexCount = 36;
constexpr uint32_t kCubePrimitiveCount = 12;

// Bit order in the result array is the ascending bit order of the flag set.
constexpr vk::QueryPipelineStatisticFlags kStatistics =
    vk::QueryPipelineStatisticFlagBits::eInputAssemblyVertices |
    vk::QueryPipelineStatisticFlagBits::eInputAssemblyPrimitives |
    vk::QueryPipelineStatisticFlagBits::eVertexShaderInvocations |
    vk::QueryPipelineStatisticFlagBits::eClippingInvocations |
    vk::QueryPipelineStatisticFlagBits::eClippingPrimitives |
    vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations;

constexpr std::array<const char *, 6> kCounterNames = {
    "inputAssemblyVertices", "inputAssemblyPrimitives", "vertexShaderInvocations",
    "clippingInvocations", "clippingPrimitives", "fragmentShaderInvocations"};

struct Options {
    bool dump = false;
    bool stats = false;
    bool verify = false;
    uint32_t frames = 120;
    uint32_t dumpAfter = 120;
    uint32_t objects = 1;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("pipeline_statistics: " + arg + " needs a value");
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
        } else if (arg == "--objects") {
            options.objects = static_cast<uint32_t>(std::stoul(next()));
            if (options.objects == 0 || options.objects > kMaxObjects) {
                throw std::runtime_error("pipeline_statistics: --objects expects 1..4");
            }
        } else {
            throw std::runtime_error("pipeline_statistics: unknown argument " + arg);
        }
    }
    return options;
}

struct Drawable {
    std::shared_ptr<ShaderAssetGroup> asset;
    std::shared_ptr<GraphicsPipeline> pipeline;
    std::shared_ptr<CmdOpDrawFrame> draw;
    std::shared_ptr<CmdSyncDevice> frameSync;
    std::vector<Vertex> vertices;
    ObjectParams object;
};

std::shared_ptr<Drawable> makeDrawable(const std::shared_ptr<RenderManager> &mgr,
                                      const std::shared_ptr<IRenderPass> &renderPass, uint32_t index)
{
    auto result = std::make_shared<Drawable>();
    const std::array<glm::vec3, kMaxObjects> colors = {
        glm::vec3(0.9f, 0.3f, 0.2f), glm::vec3(0.2f, 0.8f, 0.4f), glm::vec3(0.25f, 0.5f, 0.95f),
        glm::vec3(0.95f, 0.8f, 0.25f)};
    result->vertices = CubeVertices(colors[index]);

    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, glm::vec3(-3.0f + 2.0f * static_cast<float>(index), 0.0f, 0.0f));
    model = glm::scale(model, glm::vec3(0.8f));
    result->object = ObjectParams{model};

    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        0, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};
    layout->setDetails({cameraSet});
    layout->setPushConstant(static_cast<uint32_t>(sizeof(ObjectParams)), vk::ShaderStageFlagBits::eVertex);

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor, VertexComponent::eUV,
                                     VertexComponent::eNormal},
        static_cast<uint32_t>(result->vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u);

    result->asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    result->asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCENE_VERT_SPV.begin(),
                                                                     shader::SCENE_VERT_SPV.end()));
    result->asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SCENE_FRAG_SPV.begin(),
                                                                     shader::SCENE_FRAG_SPV.end()));
    result->pipeline = mgr->CreatePipeline(vertexInput, layout, result->asset, renderPass, 0);
    result->draw = std::make_shared<CmdOpDrawFrame>(result->pipeline);
    result->frameSync = std::make_shared<CmdSyncDevice>(result->asset);

    auto upload = mgr->UploadSequence();
    auto sync = std::make_shared<CmdSyncDevice>(result->asset);
    sync->OnUpdateRawData(ResourceTypes::eVertex, result->vertices);
    upload->submit(sync, result->asset);
    return result;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 8.0f));
    GLFWwindow window(kWidth, kHeight, camera);

    DeviceRequirement requirement;
    requirement.pipelineStatisticsQuery = true;
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT, requirement);

    const vk::Format swapFormat = *mgr->getSwapChain()->getImageFormat();
    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, swapFormat, mgr->DefaultDepthFormat()));

    std::array<std::shared_ptr<Drawable>, kMaxObjects> objects;
    for (uint32_t i = 0; i < kMaxObjects; ++i) {
        objects[i] = makeDrawable(mgr, renderPass, i);
    }

    auto queryPool = QueryPool::Create(mgr->getLogicalDevice(), QueryPool::Kind::ePipelineStatistics, 1,
                                       kStatistics);
    auto resetQuery = std::make_shared<CmdOpQuery>(queryPool, CmdOpQuery::Phase::eReset);
    auto beginQuery = std::make_shared<CmdOpQuery>(queryPool, CmdOpQuery::Phase::eBegin, 0);
    auto endQuery = std::make_shared<CmdOpQuery>(queryPool, CmdOpQuery::Phase::eEnd, 0);

    auto sequence = mgr->DrawFrameSequence();
    std::vector<uint64_t> lastCounters(kCounterNames.size(), 0);
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
        for (uint32_t i = 0; i < options.objects; ++i) {
            objects[i]->draw->OnUpdateRawData(ResourceId(0, 0), frame, cameraMatrix);
            objects[i]->draw->OnUpdateRawData(ResourceTypes::ePushConst, frame, objects[i]->object);
        }

        sequence->beginRecording();
        // Reset and staging copies stay outside the render pass; the statistics
        // query itself wraps only the draws it is meant to measure.
        sequence->record(resetQuery);
        for (uint32_t i = 0; i < options.objects; ++i) {
            sequence->record(objects[i]->frameSync);
        }
        sequence->recordPass(renderPass, [&]() {
            sequence->record(beginQuery);
            for (uint32_t i = 0; i < options.objects; ++i) {
                sequence->record(objects[i]->draw);
            }
            sequence->record(endQuery);
        });
        sequence->endRecording();
        sequence->submitFrame();

        lastCounters = queryPool->fetchBlocking(0, 1);
        ++framesRendered;
        if (options.stats && framesRendered == 1) {
            for (size_t i = 0; i < kCounterNames.size() && i < lastCounters.size(); ++i) {
                std::cout << "[stats] " << kCounterNames[i] << "=" << lastCounters[i] << "\n";
            }
        }

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

    if (options.verify) {
        // Non-indexed triangle lists: 36 vertices / 12 primitives per cube, and
        // one vertex-shader invocation per vertex.  These three are exact and
        // independent of viewport coverage, unlike the fragment counter.
        const uint64_t vertices = static_cast<uint64_t>(kCubeVertexCount) * options.objects;
        const uint64_t primitives = static_cast<uint64_t>(kCubePrimitiveCount) * options.objects;
        const bool ok = lastCounters.size() >= 3 && lastCounters[0] == vertices
                        && lastCounters[1] == primitives && lastCounters[2] == vertices;
        std::cout << "[verify] objects=" << options.objects << " expected(vertices=" << vertices
                  << ",primitives=" << primitives << ") got(" << (lastCounters.empty() ? 0 : lastCounters[0])
                  << "," << (lastCounters.size() < 2 ? 0 : lastCounters[1]) << ","
                  << (lastCounters.size() < 3 ? 0 : lastCounters[2]) << ") -> " << (ok ? "yes" : "NO") << "\n";
        if (!ok) {
            throw std::runtime_error("pipeline_statistics: query counters do not match the CPU draw list");
        }
    }
    if (options.stats) {
        std::cout << "[stats] frames=" << framesRendered << " objects=" << options.objects
                  << " counters=" << lastCounters.size() << "\n";
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
