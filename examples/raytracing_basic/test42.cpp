// test42 - hardware ray tracing, basic pipeline
//
// Migrated from Vulkan-Samples raytracingbasic.  Demonstrates the full KHR
// object chain end to end: bottom-level acceleration structure over a two-
// triangle quad, a one-instance top-level structure, a raygen / miss / closest-
// hit pipeline, its shader binding table, and the storage-image -> swapchain
// copy that presents the result (see DESIGN.md 31).
//
// The scene is deliberately tiny so the dump is predictable: a quad covering the
// centre of the frame, coloured by barycentric coordinates.  --verify measures
// the hit ratio from the storage image, which proves the acceleration structure
// and the trace dispatch both worked.
//
// CLI: --self-test  (pure CPU SBT arithmetic, needs no GPU)
//      --dump [--dump-after N] [--frames N]
//      --verify     (read back the storage image and check the hit ratio)
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
#include "shader/raygen_rgen.hpp"

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

// Matches CameraProperties in the raygen shader (two std140 mat4).
struct CameraProperties {
    glm::mat4 viewInverse;
    glm::mat4 projInverse;
};

// position only: the acceleration structure consumes R32G32B32_SFLOAT
struct RtVertex {
    glm::vec3 position;
};

const std::vector<RtVertex> kVertices = {
    {{-1.0f, -1.0f, 0.0f}},
    {{1.0f, -1.0f, 0.0f}},
    {{1.0f, 1.0f, 0.0f}},
    {{-1.0f, 1.0f, 0.0f}},
};
const std::vector<uint32_t> kIndices = {0, 1, 2, 0, 2, 3};

// Background written by miss.rmiss.  The linear value is the single source of
// truth: --verify re-encodes it for whatever format the swapchain handed us
// (a UNORM surface stores it verbatim, an _SRGB surface applies the transfer
// function on store), so the check cannot drift away from the shader.
constexpr glm::vec3 kBackgroundLinear(0.05f, 0.08f, 0.20f);

int runSelfTest()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char *what) {
        std::cout << "[self-test] " << (condition ? "PASS " : "FAIL ") << what << "\n";
        if (!condition) {
            ++failures;
        }
    };

    // A synthetic capability set so the arithmetic runs on any machine.
    rt::Capabilities caps;
    caps.pipelineProperties.shaderGroupHandleSize = 32;
    caps.pipelineProperties.shaderGroupHandleAlignment = 32;
    caps.pipelineProperties.shaderGroupBaseAlignment = 64;

    // 1) no record data: stride is exactly the aligned handle size.
    rt::ShaderBindingLayout::RegionSpec raygen{1, {}};
    rt::ShaderBindingLayout::RegionSpec miss{1, {}};
    rt::ShaderBindingLayout::RegionSpec hit{1, {}};
    rt::ShaderBindingLayout::RegionSpec callable{0, {}};
    rt::ShaderBindingLayout layout = rt::ShaderBindingLayout::Compute(caps, raygen, miss, hit, callable);

    check(layout.handleSizeAligned == 32, "aligned handle size == 32");
    check(layout.raygen.recordStride == 32, "raygen stride == 32 without record data");
    check(layout.miss.offset == 64, "miss region starts at the next base-aligned offset");
    check(layout.hit.offset == 128, "hit region follows miss at a base-aligned offset");
    check(layout.callable.groupCount == 0 && layout.callable.byteSize == 0, "empty callable region has zero size");
    check(layout.IsValid(), "plain layout is valid");

    // 2) a region with 12 bytes of record data rounds the record up to 64.
    rt::ShaderBindingLayout::RegionSpec hitWithData{1, {12}};
    rt::ShaderBindingLayout withData = rt::ShaderBindingLayout::Compute(caps, raygen, miss, hitWithData, callable);
    check(withData.hit.recordStride == 64, "12-byte record data rounds the 32-byte handle up to 64");
    check(withData.IsValid(), "layout with record data is valid");

    // 3) two miss groups and a larger handle size.
    caps.pipelineProperties.shaderGroupHandleSize = 64;
    caps.pipelineProperties.shaderGroupHandleAlignment = 64;
    caps.pipelineProperties.shaderGroupBaseAlignment = 256;
    rt::ShaderBindingLayout::RegionSpec twoMiss{2, {}};
    rt::ShaderBindingLayout multi = rt::ShaderBindingLayout::Compute(caps, raygen, twoMiss, hit, callable);
    check(multi.handleSizeAligned == 64, "64-byte handle stays aligned");
    check(multi.miss.byteSize == 128, "two miss records occupy two strides");
    check(multi.callable.offset % 256 == 0, "every region offset is base-aligned");
    check(multi.IsValid(), "multi-group layout is valid");

    // 4) scene sanity: a quad is two triangles.
    check(kVertices.size() == 4 && kIndices.size() == 6, "scene is a two-triangle quad");

    std::cout << "[self-test] " << (failures == 0 ? "all checks passed" : "FAILURES") << " (" << failures
              << " failed)\n";
    return failures == 0 ? 0 : 1;
}

void render(const Options &options)
{

    // 3.0 units back with a 45-degree vertical FOV leaves visible background
    // around the 2x2 quad, so --verify can measure a hit ratio strictly between
    // 0 and 1 (a full-frame hit would pass even if the miss shader never ran).
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
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
    std::cerr << "[rt] " << caps.Describe() << std::endl;
    if (!caps.canTraceRays()) {
        throw std::runtime_error("raytracing_basic: device does not expose the ray tracing pipeline chain");
    }
    rt::Dispatch dispatch;
    dispatch.Load(device);
    if (!dispatch.valid()) {
        throw std::runtime_error("raytracing_basic: missing entry points: " + dispatch.missing());
    }

    // ---- geometry buffers the acceleration structure reads ----------------
    const vk::BufferUsageFlags inputUsage = vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
                                            | vk::BufferUsageFlagBits::eStorageBuffer;
    rt::DeviceAddressBuffer vertexBuffer;
    vertexBuffer.Create(device, memProps, dispatch, kVertices.size() * sizeof(RtVertex), inputUsage, true);
    vertexBuffer.Upload(kVertices.data(), kVertices.size() * sizeof(RtVertex));
    rt::DeviceAddressBuffer indexBuffer;
    indexBuffer.Create(device, memProps, dispatch, kIndices.size() * sizeof(uint32_t), inputUsage, true);
    indexBuffer.Upload(kIndices.data(), kIndices.size() * sizeof(uint32_t));

    // ---- acceleration structures -----------------------------------------
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
    // 3x4 row-major identity: vulkan-hpp exposes it as a 2D array wrapper, so
    // fill it element-wise rather than with a flat initializer list.
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            instance.transform.matrix[row][column] = (row == column) ? 1.0f : 0.0f;
        }
    }
    instance.instanceCustomIndex = 0;
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    // This field is the raw VkGeometryInstanceFlagsKHR in vulkan-hpp, not a
    // Flags<> wrapper, so the C constant is used directly.
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = bottomLevel.address;
    rt::AccelerationStructure topLevel = builder.BuildTopLevel({instance});

    // ---- descriptor layout: AS, storage image, camera UBO ------------------
    vk::DescriptorSetLayoutBinding asBinding{};
    asBinding.binding = 0;
    asBinding.descriptorType = vk::DescriptorType::eAccelerationStructureKHR;
    asBinding.descriptorCount = 1;
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

    // ---- storage image + camera UBO ---------------------------------------
    const vk::Format swapFormat = *mgr->getSwapChain()->getImageFormat();
    const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

    rt::StorageImage storage;
    storage.Create(device, memProps, swapFormat, extent);
    storage.TransitionToGeneral(device, queue, queueFamily);

    rt::DeviceAddressBuffer cameraUbo;
    cameraUbo.Create(device, memProps, dispatch, sizeof(CameraProperties), vk::BufferUsageFlagBits::eUniformBuffer,
                     true);

    // ---- ray tracing pipeline + SBT ---------------------------------------
    rt::PipelineDesc pipelineDesc;
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.descriptorLayout = descriptorLayout;
    pipelineDesc.pipelineLayout = pipelineLayout;

    rt::ShaderStageDesc raygenStage;
    raygenStage.stage = vk::ShaderStageFlagBits::eRaygenKHR;
    raygenStage.spirv.assign(shader::RAYGEN_RGEN_SPV.begin(), shader::RAYGEN_RGEN_SPV.end());
    pipelineDesc.stages.push_back(raygenStage);

    rt::ShaderStageDesc missStage;
    missStage.stage = vk::ShaderStageFlagBits::eMissKHR;
    missStage.spirv.assign(shader::MISS_RMISS_SPV.begin(), shader::MISS_RMISS_SPV.end());
    pipelineDesc.stages.push_back(missStage);

    rt::ShaderStageDesc hitStage;
    hitStage.stage = vk::ShaderStageFlagBits::eClosestHitKHR;
    hitStage.spirv.assign(shader::CLOSESTHIT_RCHIT_SPV.begin(), shader::CLOSESTHIT_RCHIT_SPV.end());
    pipelineDesc.stages.push_back(hitStage);

    rt::ShaderGroupDesc raygenGroup;
    raygenGroup.type = rt::ShaderGroupType::eGeneral;
    raygenGroup.general = 0;
    pipelineDesc.raygen.push_back(raygenGroup);

    rt::ShaderGroupDesc missGroup;
    missGroup.type = rt::ShaderGroupType::eGeneral;
    missGroup.general = 1;
    pipelineDesc.miss.push_back(missGroup);

    rt::ShaderGroupDesc hitGroup;
    hitGroup.type = rt::ShaderGroupType::eTrianglesHit;
    hitGroup.closestHit = 2;
    pipelineDesc.hit.push_back(hitGroup);

    rt::RayTracingPipeline pipeline;
    pipeline.Create(device, memProps, dispatch, caps, pipelineDesc);

    // ---- descriptor pool / set --------------------------------------------
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
    cameraBufferInfo.offset = 0;
    cameraBufferInfo.range = sizeof(CameraProperties);

    vk::WriteDescriptorSet cameraDescriptor{};
    cameraDescriptor.dstSet = descriptorSet;
    cameraDescriptor.dstBinding = 2;
    cameraDescriptor.descriptorCount = 1;
    cameraDescriptor.descriptorType = vk::DescriptorType::eUniformBuffer;
    cameraDescriptor.pBufferInfo = &cameraBufferInfo;

    const std::array<vk::WriteDescriptorSet, 3> writes = {asDescriptor, imageDescriptor, cameraDescriptor};
    device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // ---- frame loop --------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    uint32_t framesRendered = 0;
    bool dumpDone = false;
    bool verifyDone = false;
    float hitRatio = 0.0f;

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
            // imageStore writes the linear payload verbatim -- a 0.05 channel
            // lands as byte 13, not the 62 an sRGB encode would produce, even
            // though the storage image inherits the surface format -- so the
            // comparison is a plain byte match.  Only the channel order depends
            // on the surface format, and IsBgraFormat() is the single owner of
            // that fact (the PPM writer reads it too).
            const bool bgra = IsBgraFormat(swapFormat);
            const uint32_t channel[3] = {bgra ? 2u : 0u, 1u, bgra ? 0u : 2u};
            const uint8_t expected[3] = {rt::StorageByte(kBackgroundLinear.r),
                                         rt::StorageByte(kBackgroundLinear.g),
                                         rt::StorageByte(kBackgroundLinear.b)};
            uint32_t hits = 0;
            const uint32_t pixelCount = frameExtent.width * frameExtent.height;
            for (uint32_t i = 0; i < pixelCount; ++i) {
                const uint8_t *pixel = capture.pixels.data() + static_cast<size_t>(i) * 4;
                int distance = 0;
                for (uint32_t c = 0; c < 3; ++c) {
                    distance += std::abs(static_cast<int>(pixel[channel[c]]) - expected[c]);
                }
                if (distance > 90) {
                    ++hits;
                }
            }
            hitRatio = pixelCount == 0 ? 0.0f : static_cast<float>(hits) / static_cast<float>(pixelCount);
            std::cout << "[verify] storage image " << frameExtent.width << "x" << frameExtent.height
                      << " hit " << hits << " px, ratio=" << hitRatio << " ("
                      << vk::to_string(swapFormat) << ", background " << int(expected[0]) << ","
                      << int(expected[1]) << "," << int(expected[2]) << ")\n";
            verifyDone = true;
            // Both bounds matter: too few hits means the acceleration structure
            // or trace is broken, too many means the camera is so close the
            // background never appears and the miss shader is not exercised.
            if (hitRatio < 0.2f || hitRatio > 0.9f) {
                throw std::runtime_error("raytracing_basic: hit ratio outside the expected band");
            }
        }

        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            DumpRequest request;
            request.label = "raytracing-basic";
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
        std::cout << "[stats] frames=" << framesRendered << " vertices=" << kVertices.size()
                  << " triangles=" << kIndices.size() / 3 << " handleSize=" << caps.shaderGroupHandleSize()
                  << " hitRatio=" << hitRatio << "\n";
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
        std::cerr << "raytracing_basic: " << e.what() << "\n";
        return 1;
    }
}
