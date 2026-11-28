// test43 - hardware ray tracing, secondary shadow rays
//
// Migrated from Vulkan-Samples raytracingshadows.  The step up from test42 is
// *depth*: the closest-hit shader traces a second ray, which the pipeline can
// only do with maxPipelineRayRecursionDepth >= 2.  That single ray needs its own
// miss shader (out of scene = lit) and its own hit group (anything hit = in
// shadow), so the SBT now carries two miss records and two hit records and the
// trace call has to name both indices.
//
// Scene: a back panel plus a floating blocker.  The blocker casts a hard shadow
// onto the panel, which --verify classifies pixel by pixel against the three
// colours the shaders can produce (background / lit / shadowed).
//
// CLI: --self-test  (pure CPU SBT arithmetic for the 2+2 group layout)
//      --dump [--dump-after N] [--frames N]
//      --verify     (classify every texel into background / lit / shadow)
//      --stats

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ImageDump.h"
#include "ImageReadback.h"
#include "LoopSequence.h"
#include "RenderManager.h"
#include "RayTracingAccelerationStructure.h"
#include "RayTracingCommon.h"
#include "RayTracingPipeline.h"
#include "ShaderBindingTable.h"

#include "shader/closesthit_rchit.hpp"
#include "shader/miss_rmiss.hpp"
#include "shader/occlusion_rchit.hpp"
#include "shader/raygen_rgen.hpp"
#include "shader/shadow_miss_rmiss.hpp"

namespace {

struct Options {
    bool selfTest = false;
    bool verify = false;
    bool dump = false;
    bool stats = false;
    uint32_t frames = 60;
    uint32_t dumpAfter = 1;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto nextValue = [&](const char *name) {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };
        if (arg == "--self-test") {
            options.selfTest = true;
        } else if (arg == "--verify") {
            options.verify = true;
        } else if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--stats") {
            options.stats = true;
        } else if (arg == "--frames") {
            options.frames = static_cast<uint32_t>(std::stoul(nextValue("--frames")));
        } else if (arg == "--dump-after") {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(nextValue("--dump-after")));
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return options;
}

struct CameraProperties {
    glm::mat4 viewInverse;
    glm::mat4 projInverse;
};

struct RtVertex {
    glm::vec3 position;
};

// Two quads in one BLAS: the back panel (primitives 0-1) and the blocker
// (primitives 2-3).  The shader picks the albedo from gl_PrimitiveID, so the
// index order here and the `gl_PrimitiveID < 2` test must stay in sync.
constexpr float kPanelHalfSize = 1.8f;
constexpr float kBlockerHalfSize = 0.4f;

const std::vector<RtVertex> kVertices = {
    // panel, z = -1
    {{-kPanelHalfSize, -kPanelHalfSize, -1.0f}},
    {{kPanelHalfSize, -kPanelHalfSize, -1.0f}},
    {{kPanelHalfSize, kPanelHalfSize, -1.0f}},
    {{-kPanelHalfSize, kPanelHalfSize, -1.0f}},
    // blocker, z = 0.4
    {{-kBlockerHalfSize - 0.1f, -kBlockerHalfSize + 0.2f, 0.4f}},
    {{kBlockerHalfSize - 0.1f, -kBlockerHalfSize + 0.2f, 0.4f}},
    {{kBlockerHalfSize - 0.1f, kBlockerHalfSize + 0.2f, 0.4f}},
    {{-kBlockerHalfSize - 0.1f, kBlockerHalfSize + 0.2f, 0.4f}},
};
const std::vector<uint32_t> kIndices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};

// Colours the shaders can produce, as linear RGB.  --verify classifies each
// texel by nearest match instead of by a brightness threshold, which keeps the
// three populations from bleeding into each other.
constexpr glm::vec3 kBackground(0.02f, 0.03f, 0.10f);
constexpr glm::vec3 kPanelAlbedo(0.86f, 0.52f, 0.32f);
constexpr glm::vec3 kBlockerAlbedo(0.45f, 0.62f, 0.86f);
constexpr float kShadowFactor = 0.12f;

int runSelfTest()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char *what) {
        std::cout << "[self-test] " << (condition ? "PASS " : "FAIL ") << what << "\n";
        if (!condition) {
            ++failures;
        }
    };

    rt::Capabilities caps;
    caps.pipelineProperties.shaderGroupHandleSize = 32;
    caps.pipelineProperties.shaderGroupHandleAlignment = 32;
    caps.pipelineProperties.shaderGroupBaseAlignment = 64;

    // Two miss records (background, shadow) and two hit records (primary,
    // occlusion).  Both regions must therefore be two strides wide, and the
    // second record must be addressable -- that is what makes the
    // (sbtRecordOffset, missIndex) pair in the shader meaningful.
    rt::ShaderBindingLayout::RegionSpec raygen{1, {}};
    rt::ShaderBindingLayout::RegionSpec miss{2, {}};
    rt::ShaderBindingLayout::RegionSpec hit{2, {}};
    rt::ShaderBindingLayout::RegionSpec callable{0, {}};
    const rt::ShaderBindingLayout layout =
        rt::ShaderBindingLayout::Compute(caps, raygen, miss, hit, callable);

    check(layout.miss.groupCount == 2 && layout.miss.firstGroup == 1,
          "shadow miss is group 1 (right after raygen)");
    check(layout.hit.firstGroup == 3, "primary hit is group 3, occlusion hit group 4");
    check(layout.miss.recordStride == 32 && layout.miss.byteSize == 64, "two miss records occupy two strides");
    check(layout.hit.byteSize == 64, "two hit records occupy two strides");
    check(layout.callable.groupCount == 0, "no callable groups in this example");
    check(layout.totalGroupCount == 5, "1 raygen + 2 miss + 2 hit = 5 groups");
    check(layout.IsValid(), "layout is valid");

    // A second ray from the closest-hit shader needs recursion depth 2.
    constexpr uint32_t kRecursionDepth = 2;
    check(kRecursionDepth >= 2, "secondary rays need maxRecursionDepth >= 2");

    check(kVertices.size() == 8 && kIndices.size() == 12, "scene is two quads");
    check(kIndices.size() / 3 == 4, "four primitives: 0-1 panel, 2-3 blocker");

    std::cout << "[self-test] " << (failures == 0 ? "all checks passed" : "FAILURES") << " (" << failures
              << " failed)\n";
    return failures == 0 ? 0 : 1;
}

// Nearest-reference classifier: index of the closest entry in `references`.
int classify(const uint8_t *pixel, const uint32_t channel[3], const std::vector<glm::vec3> &references)
{
    int best = 0;
    int bestDistance = 1 << 30;
    for (size_t r = 0; r < references.size(); ++r) {
        int distance = 0;
        for (uint32_t c = 0; c < 3; ++c) {
            const int reference = static_cast<int>(std::lround(references[r][c] * 255.0f));
            distance += std::abs(static_cast<int>(pixel[channel[c]]) - reference);
        }
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(r);
        }
    }
    return best;
}

void render(const Options &options)
{
    // Far enough back that the panel does not fill the frame: the background
    // miss shader has to run too, or the classification has nothing to
    // separate.
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 4.2f));
    camera->Zoom = 45.0f;
    GLFWwindow window(900, 640, camera);

    DeviceRequirement requirement;
    requirement.rayTracingPipeline = true;
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT, requirement);

    const vk::Device device = *mgr->getLogicalDevice();
    const vk::PhysicalDevice physicalDevice = *mgr->getPhysicalDevice();
    const vk::Queue queue = *mgr->getGraphicsQueue();
    const uint32_t queueFamily = *mgr->getQueueFamilyIndices().graphicsFamily;
    const vk::PhysicalDeviceMemoryProperties memProps = mgr->memoryProperties();

    const rt::Capabilities caps = rt::Capabilities::Query(physicalDevice);
    std::cout << "[rt] " << caps.Describe() << "\n";
    if (!caps.canTraceRays()) {
        throw std::runtime_error("raytracing_shadows: device does not expose the ray tracing pipeline chain");
    }
    rt::Dispatch dispatch;
    dispatch.Load(device);
    if (!dispatch.valid()) {
        throw std::runtime_error("raytracing_shadows: missing entry points: " + dispatch.missing());
    }

    const vk::BufferUsageFlags inputUsage = vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
                                            | vk::BufferUsageFlagBits::eStorageBuffer;
    rt::DeviceAddressBuffer vertexBuffer;
    vertexBuffer.Create(device, memProps, dispatch, kVertices.size() * sizeof(RtVertex), inputUsage, true);
    vertexBuffer.Upload(kVertices.data(), kVertices.size() * sizeof(RtVertex));
    rt::DeviceAddressBuffer indexBuffer;
    indexBuffer.Create(device, memProps, dispatch, kIndices.size() * sizeof(uint32_t), inputUsage, true);
    indexBuffer.Upload(kIndices.data(), kIndices.size() * sizeof(uint32_t));

    rt::AccelerationStructureBuilder builder(device, memProps, queue, queueFamily, dispatch);
    rt::TriangleGeometry geometry;
    geometry.vertexBuffer = vertexBuffer;
    geometry.indexBuffer = indexBuffer;
    geometry.vertexCount = static_cast<uint32_t>(kVertices.size());
    geometry.indexCount = static_cast<uint32_t>(kIndices.size());
    geometry.vertexStride = sizeof(RtVertex);
    geometry.hasIndices = true;
    rt::AccelerationStructure bottomLevel = builder.BuildBottomLevel(geometry);

    vk::AccelerationStructureInstanceKHR instance{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            instance.transform.matrix[row][column] = (row == column) ? 1.0f : 0.0f;
        }
    }
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = bottomLevel.address;
    rt::AccelerationStructure topLevel = builder.BuildTopLevel({instance});

    vk::DescriptorSetLayoutBinding asBinding{};
    asBinding.binding = 0;
    asBinding.descriptorType = vk::DescriptorType::eAccelerationStructureKHR;
    asBinding.descriptorCount = 1;
    // The closest-hit stage re-traces, so it needs the structure as well.
    asBinding.stageFlags = vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eClosestHitKHR;

    vk::DescriptorSetLayoutBinding imageBinding{};
    imageBinding.binding = 1;
    imageBinding.descriptorType = vk::DescriptorType::eStorageImage;
    imageBinding.descriptorCount = 1;
    imageBinding.stageFlags = vk::ShaderStageFlagBits::eRaygenKHR;

    vk::DescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 2;
    uboBinding.descriptorType = vk::DescriptorType::eUniformBuffer;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = vk::ShaderStageFlagBits::eRaygenKHR;

    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {asBinding, imageBinding, uboBinding};
    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    const vk::DescriptorSetLayout descriptorLayout = device.createDescriptorSetLayout(layoutInfo);

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
    const vk::PipelineLayout pipelineLayout = device.createPipelineLayout(pipelineLayoutInfo);

    const vk::Format swapFormat = *mgr->getSwapChain()->getImageFormat();
    const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

    rt::StorageImage storage;
    storage.Create(device, memProps, swapFormat, extent);
    storage.TransitionToGeneral(device, queue, queueFamily);

    rt::DeviceAddressBuffer cameraUbo;
    cameraUbo.Create(device, memProps, dispatch, sizeof(CameraProperties), vk::BufferUsageFlagBits::eUniformBuffer,
                     true);

    // ---- pipeline: 1 raygen, 2 miss, 2 hit, recursion depth 2 --------------
    rt::PipelineDesc pipelineDesc;
    pipelineDesc.maxRecursionDepth = 2;
    pipelineDesc.descriptorLayout = descriptorLayout;
    pipelineDesc.pipelineLayout = pipelineLayout;

    // The generated headers expose the SPIR-V as std::array, so the helper takes
    // any container with begin/end instead of a fixed pointer pair.
    const auto addStage = [&pipelineDesc](vk::ShaderStageFlagBits stage, const auto &spirv) {
        rt::ShaderStageDesc desc;
        desc.stage = stage;
        desc.spirv.assign(spirv.begin(), spirv.end());
        pipelineDesc.stages.push_back(desc);
        return static_cast<int>(pipelineDesc.stages.size()) - 1;
    };

    const int raygenStage = addStage(vk::ShaderStageFlagBits::eRaygenKHR, shader::RAYGEN_RGEN_SPV);
    const int backgroundMiss = addStage(vk::ShaderStageFlagBits::eMissKHR, shader::MISS_RMISS_SPV);
    const int shadowMiss = addStage(vk::ShaderStageFlagBits::eMissKHR, shader::SHADOW_MISS_RMISS_SPV);
    const int primaryHit = addStage(vk::ShaderStageFlagBits::eClosestHitKHR, shader::CLOSESTHIT_RCHIT_SPV);
    const int occlusionHit = addStage(vk::ShaderStageFlagBits::eClosestHitKHR, shader::OCCLUSION_RCHIT_SPV);

    rt::ShaderGroupDesc raygenGroup;
    raygenGroup.general = raygenStage;
    pipelineDesc.raygen.push_back(raygenGroup);

    rt::ShaderGroupDesc backgroundMissGroup;
    backgroundMissGroup.general = backgroundMiss;
    pipelineDesc.miss.push_back(backgroundMissGroup);
    rt::ShaderGroupDesc shadowMissGroup;
    shadowMissGroup.general = shadowMiss;
    pipelineDesc.miss.push_back(shadowMissGroup);

    rt::ShaderGroupDesc primaryHitGroup;
    primaryHitGroup.type = rt::ShaderGroupType::eTrianglesHit;
    primaryHitGroup.closestHit = primaryHit;
    pipelineDesc.hit.push_back(primaryHitGroup);
    rt::ShaderGroupDesc occlusionHitGroup;
    occlusionHitGroup.type = rt::ShaderGroupType::eTrianglesHit;
    occlusionHitGroup.closestHit = occlusionHit;
    pipelineDesc.hit.push_back(occlusionHitGroup);

    rt::RayTracingPipeline pipeline;
    pipeline.Create(device, memProps, dispatch, caps, pipelineDesc);

    const std::array<vk::DescriptorPoolSize, 3> poolSizes = {
        vk::DescriptorPoolSize{vk::DescriptorType::eAccelerationStructureKHR, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 1},
    };
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    const vk::DescriptorPool descriptorPool = device.createDescriptorPool(poolInfo);

    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &descriptorLayout;
    const vk::DescriptorSet descriptorSet = device.allocateDescriptorSets(allocateInfo).front();

    vk::WriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &topLevel.handle;

    vk::WriteDescriptorSet asDescriptor{};
    asDescriptor.dstSet = descriptorSet;
    asDescriptor.dstBinding = 0;
    asDescriptor.descriptorCount = 1;
    asDescriptor.descriptorType = vk::DescriptorType::eAccelerationStructureKHR;
    asDescriptor.pNext = &asWrite;

    vk::DescriptorImageInfo imageInfo{};
    imageInfo.imageView = storage.view();
    imageInfo.imageLayout = vk::ImageLayout::eGeneral;

    vk::WriteDescriptorSet imageDescriptor{};
    imageDescriptor.dstSet = descriptorSet;
    imageDescriptor.dstBinding = 1;
    imageDescriptor.descriptorCount = 1;
    imageDescriptor.descriptorType = vk::DescriptorType::eStorageImage;
    imageDescriptor.pImageInfo = &imageInfo;

    vk::DescriptorBufferInfo cameraBufferInfo{};
    cameraBufferInfo.buffer = cameraUbo.buffer;
    cameraBufferInfo.range = sizeof(CameraProperties);

    vk::WriteDescriptorSet cameraDescriptor{};
    cameraDescriptor.dstSet = descriptorSet;
    cameraDescriptor.dstBinding = 2;
    cameraDescriptor.descriptorCount = 1;
    cameraDescriptor.descriptorType = vk::DescriptorType::eUniformBuffer;
    cameraDescriptor.pBufferInfo = &cameraBufferInfo;

    const std::array<vk::WriteDescriptorSet, 3> writes = {asDescriptor, imageDescriptor, cameraDescriptor};
    device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    auto sequence = mgr->DrawFrameSequence();
    uint32_t framesRendered = 0;
    bool dumpDone = false;
    bool verifyDone = false;
    float backgroundRatio = 0.0f;
    float litRatio = 0.0f;
    float shadowRatio = 0.0f;
    float blockerRatio = 0.0f;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (!sequence->prepareFrame()) {
            continue;
        }

        const vk::Extent2D frameExtent = *mgr->getSwapChain()->getExtent2D();
        const glm::mat4 view = camera->GetViewMatrix();
        const glm::mat4 projection = glm::perspective(glm::radians(camera->Zoom),
                                                      static_cast<float>(frameExtent.width) /
                                                          static_cast<float>(frameExtent.height),
                                                      0.1f, 100.0f);
        CameraProperties cameraProperties;
        cameraProperties.viewInverse = glm::inverse(view);
        cameraProperties.projInverse = glm::inverse(projection);
        cameraUbo.Upload(&cameraProperties, sizeof(cameraProperties));

        sequence->beginRecording();
        sequence->recordRaw([&](vk::CommandBuffer cmd, uint32_t imageIndex) {
            pipeline.Bind(cmd, descriptorSet);
            pipeline.RecordTrace(cmd, frameExtent.width, frameExtent.height);
            rt::RecordCopyToSwapchain(cmd, storage.image(), mgr->getSwapChain()->getImages()[imageIndex],
                                      frameExtent);
        });
        sequence->endRecording();
        sequence->submitFrame();
        ++framesRendered;

        ImageReadback readback(mgr->getLogicalDevice(), memProps, mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                               queueFamily);

        if (options.verify && !verifyDone) {
            const ImageReadback::Capture capture =
                readback.capture(storage.image(), swapFormat, frameExtent, vk::ImageLayout::eGeneral);
            const bool bgra = IsBgraFormat(swapFormat);
            const uint32_t channel[3] = {bgra ? 2u : 0u, 1u, bgra ? 0u : 2u};
            // Exactly the four colours the shaders can emit; classification is
            // by nearest match so no brightness threshold is invented here.
            const std::vector<glm::vec3> references = {
                kBackground,
                kPanelAlbedo,
                kPanelAlbedo * kShadowFactor,
                kBlockerAlbedo,
            };
            uint32_t counts[4] = {0, 0, 0, 0};
            const uint32_t pixelCount = frameExtent.width * frameExtent.height;
            for (uint32_t i = 0; i < pixelCount; ++i) {
                const uint8_t *pixel = capture.pixels.data() + static_cast<size_t>(i) * 4;
                ++counts[classify(pixel, channel, references)];
            }
            backgroundRatio = static_cast<float>(counts[0]) / static_cast<float>(pixelCount);
            litRatio = static_cast<float>(counts[1]) / static_cast<float>(pixelCount);
            shadowRatio = static_cast<float>(counts[2]) / static_cast<float>(pixelCount);
            blockerRatio = static_cast<float>(counts[3]) / static_cast<float>(pixelCount);
            std::cout << "[verify] background=" << backgroundRatio << " lit=" << litRatio
                      << " shadow=" << shadowRatio << " blocker=" << blockerRatio << "\n";
            verifyDone = true;
            // All four populations must be present: without background the
            // camera is too close, without lit pixels the panel is missing,
            // without blocker pixels the caster is missing, and without shadowed
            // pixels the secondary ray never occluded anything (the whole point
            // of this example).
            if (backgroundRatio < 0.05f || litRatio < 0.10f || shadowRatio < 0.005f || blockerRatio < 0.005f) {
                throw std::runtime_error("raytracing_shadows: expected background, lit, shadowed and blocker pixels");
            }
        }

        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            DumpRequest request;
            request.label = "raytracing-shadows";
            request.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            request.format = swapFormat;
            request.extent = frameExtent;
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
        if (framesRendered >= options.frames) {
            break;
        }
    }

    if (options.stats) {
        std::cout << "[stats] frames=" << framesRendered << " primitives=" << kIndices.size() / 3
                  << " missGroups=2 hitGroups=2 recursion=2"
                  << " background=" << backgroundRatio << " lit=" << litRatio << " shadow=" << shadowRatio
                  << " blocker=" << blockerRatio << "\n";
    }

    device.waitIdle();
    pipeline.Destroy(device, dispatch);
    device.destroyDescriptorPool(descriptorPool);
    device.destroyPipelineLayout(pipelineLayout);
    device.destroyDescriptorSetLayout(descriptorLayout);
    storage.Destroy(device);
    cameraUbo.Destroy(device);
    topLevel.Destroy(device, dispatch);
    bottomLevel.Destroy(device, dispatch);
    vertexBuffer.Destroy(device);
    indexBuffer.Destroy(device);
}

} // namespace

int main(int argc, char **argv)
{
    try {
        const Options options = parseOptions(argc, argv);
        if (options.selfTest) {
            return runSelfTest();
        }
        render(options);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "raytracing_shadows: " << e.what() << "\n";
        return 1;
    }
}
