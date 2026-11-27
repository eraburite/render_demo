// occlusion_query (test38) -- migrated from Vulkan-Samples/examples/occlusionquery
//
// One occluder is drawn into the depth buffer first.  Two test objects are then
// drawn, each wrapped in its own occlusion query, which counts the samples that
// survive the depth test.  The object behind the occluder reports zero, the one
// beside it reports a non-zero count.  --no-occluder removes the depth blocker
// so both queries light up (see DESIGN.md 28.2).
#include <array>
#include <cstdint>
#include <cstring>
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
constexpr uint32_t kQueryCount = 2;

struct Options {
    bool dump = false;
    bool stats = false;
    bool verify = false;
    bool noOccluder = false;
    uint32_t frames = 120;
    uint32_t dumpAfter = 120;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("occlusion_query: " + arg + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--stats") {
            options.stats = true;
        } else if (arg == "--verify") {
            options.verify = true;
        } else if (arg == "--no-occluder") {
            options.noOccluder = true;
        } else if (arg == "--frames") {
            options.frames = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--dump-after") {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(next()));
        } else {
            throw std::runtime_error("occlusion_query: unknown argument " + arg);
        }
    }
    return options;
}

// The three objects are laid out so object 0 is completely behind the occluder
// and object 1 is completely beside it.  Coordinates are in world space; the
// camera sits at (0,0,6) looking down -z.
struct ObjectSpec {
    const char *name;
    glm::vec3 color;
    glm::vec3 position;
    glm::vec3 scale;
};

constexpr std::array<ObjectSpec, 3> kObjects = {
    ObjectSpec{"occluder", glm::vec3(0.35f, 0.35f, 0.4f), glm::vec3(-1.5f, 0.0f, 0.0f),
               glm::vec3(2.0f, 4.0f, 0.3f)},
    ObjectSpec{"hidden", glm::vec3(0.9f, 0.2f, 0.2f), glm::vec3(-1.5f, 0.0f, -1.5f),
               glm::vec3(0.8f, 0.8f, 0.8f)},
    ObjectSpec{"visible", glm::vec3(0.2f, 0.5f, 0.95f), glm::vec3(2.0f, 0.0f, -1.5f),
               glm::vec3(0.8f, 0.8f, 0.8f)},
};

glm::mat4 modelMatrix(const ObjectSpec &spec)
{
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, spec.position);
    return glm::scale(model, spec.scale);
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
                                      const std::shared_ptr<IRenderPass> &renderPass, const ObjectSpec &spec)
{
    auto result = std::make_shared<Drawable>();
    result->vertices = CubeVertices(spec.color);
    result->object = ObjectParams{modelMatrix(spec)};

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

    // Static geometry: upload once through a one-shot sequence.
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
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 6.0f));
    GLFWwindow window(kWidth, kHeight, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Format swapFormat = *mgr->getSwapChain()->getImageFormat();
    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, swapFormat, mgr->DefaultDepthFormat()));

    std::array<std::shared_ptr<Drawable>, kObjects.size()> objects;
    for (size_t i = 0; i < kObjects.size(); ++i) {
        objects[i] = makeDrawable(mgr, renderPass, kObjects[i]);
    }

    // Occlusion queries need no device feature, unlike pipeline statistics.
    auto queryPool = QueryPool::Create(mgr->getLogicalDevice(), QueryPool::Kind::eOcclusion, kQueryCount);
    auto resetQuery = std::make_shared<CmdOpQuery>(queryPool, CmdOpQuery::Phase::eReset);
    std::array<std::shared_ptr<CmdOpQuery>, kQueryCount> beginQuery;
    std::array<std::shared_ptr<CmdOpQuery>, kQueryCount> endQuery;
    for (uint32_t q = 0; q < kQueryCount; ++q) {
        beginQuery[q] = std::make_shared<CmdOpQuery>(queryPool, CmdOpQuery::Phase::eBegin, q);
        endQuery[q] = std::make_shared<CmdOpQuery>(queryPool, CmdOpQuery::Phase::eEnd, q);
    }

    auto sequence = mgr->DrawFrameSequence();
    std::vector<uint64_t> lastSamples(kQueryCount, 0);
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
        for (const auto &object : objects) {
            object->draw->OnUpdateRawData(ResourceId(0, 0), frame, cameraMatrix);
            object->draw->OnUpdateRawData(ResourceTypes::ePushConst, frame, object->object);
        }

        sequence->beginRecording();
        // vkCmdResetQueryPool is only legal outside a render pass, and so are
        // the staging->device copies recorded by CmdSyncDevice.  Keep both
        // groups before recordPass(): copying a buffer inside a render pass
        // triggers VUID 00173 on every frame.
        sequence->record(resetQuery);
        for (const auto &object : objects) {
            sequence->record(object->frameSync);
        }
        sequence->recordPass(renderPass, [&]() {
            if (!options.noOccluder) {
                sequence->record(objects[0]->draw);
            }
            for (uint32_t q = 0; q < kQueryCount; ++q) {
                const auto &object = objects[q + 1];
                sequence->record(beginQuery[q]);
                sequence->record(object->draw);
                sequence->record(endQuery[q]);
            }
        });
        sequence->endRecording();
        sequence->submitFrame();

        // The reference waits for the results right after submission; doing the
        // same here also guarantees the next frame's reset cannot race the GPU.
        lastSamples = queryPool->fetchBlocking(0, kQueryCount);
        ++framesRendered;
        if (options.stats && (framesRendered == 1 || framesRendered % 30 == 0)) {
            std::cout << "[stats] frame=" << framesRendered << " hiddenSamples=" << lastSamples[0]
                      << " visibleSamples=" << lastSamples[1] << "\n";
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
        // With the occluder in place the first object is fully hidden, while
        // the second one is fully visible.  Without it both must be visible.
        const bool hiddenOk = options.noOccluder ? lastSamples[0] > 0 : lastSamples[0] == 0;
        const bool visibleOk = lastSamples[1] > 0;
        std::cout << "[verify] hiddenSamples=" << lastSamples[0] << " visibleSamples=" << lastSamples[1]
                  << " occluder=" << (options.noOccluder ? "off" : "on") << " -> "
                  << (hiddenOk && visibleOk ? "yes" : "NO") << "\n";
        if (!hiddenOk || !visibleOk) {
            throw std::runtime_error("occlusion_query: query results do not match the scene layout");
        }
    }
    if (options.stats) {
        std::cout << "[stats] frames=" << framesRendered << " finalHidden=" << lastSamples[0]
                  << " finalVisible=" << lastSamples[1] << "\n";
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
