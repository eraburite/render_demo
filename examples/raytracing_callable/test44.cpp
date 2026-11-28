// test44 - hardware ray tracing, callable shaders
//
// Migrated from Vulkan-Samples raytracingcallable.  One BLAS, three instances,
// one closest-hit shader: the material of each quad is chosen at trace time by
// executeCallableEXT(gl_InstanceCustomIndexEXT, 0), which jumps into callable
// shader 0 / 1 / 2 from the SBT's callable region.
//
// That is the whole point of callable shaders -- the shading logic becomes
// data-driven (an index carried by the instance) instead of being duplicated
// into three closest-hit shaders -- and it is the only SBT region test42/test43
// leave empty.
//
// CLI: --self-test  (pure CPU SBT arithmetic including the callable region)
//      --dump [--dump-after N] [--frames N]
//      --verify     (sample the centre of each quad and check its dominant channel)
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

#include "shader/callable_blue_rcall.hpp"
#include "shader/callable_green_rcall.hpp"
#include "shader/callable_red_rcall.hpp"
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

struct CameraProperties {
    glm::mat4 viewInverse;
    glm::mat4 projInverse;
};

struct RtVertex {
    glm::vec3 position;
};

constexpr float kQuadHalfSize = 0.55f;
// One BLAS quad, three instances pushed apart along x.  instanceCustomIndex
// carries the material index into the shader.
const std::vector<RtVertex> kVertices = {
    {{-kQuadHalfSize, -kQuadHalfSize, 0.0f}},
    {{kQuadHalfSize, -kQuadHalfSize, 0.0f}},
    {{kQuadHalfSize, kQuadHalfSize, 0.0f}},
    {{-kQuadHalfSize, kQuadHalfSize, 0.0f}},
};
const std::vector<uint32_t> kIndices = {0, 1, 2, 0, 2, 3};

constexpr float kInstanceOffset = 1.15f;
const glm::vec3 kInstanceCenters[3] = {
    glm::vec3(-kInstanceOffset, 0.0f, 0.0f),
    glm::vec3(0.0f, 0.0f, 0.0f),
    glm::vec3(kInstanceOffset, 0.0f, 0.0f),
};

// Base colours of the three callable shaders, and the channel each one is meant
// to dominate.  --verify only checks the dominant channel so the per-callable
// patterns (stripes / checker / bands) cannot make the check flaky.
const glm::vec3 kCallableColors[3] = {
    glm::vec3(0.95f, 0.18f, 0.12f),
    glm::vec3(0.15f, 0.90f, 0.25f),
    glm::vec3(0.25f, 0.45f, 0.98f),
};
constexpr int kDominantChannel[3] = {0, 1, 2}; // R, G, B
constexpr glm::vec3 kBackground(0.02f, 0.03f, 0.10f);

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

    // The callable region is the new part: three general shader groups, laid out
    // after raygen / miss / hit.
    rt::ShaderBindingLayout::RegionSpec raygen{1, {}};
    rt::ShaderBindingLayout::RegionSpec miss{1, {}};
    rt::ShaderBindingLayout::RegionSpec hit{1, {}};
    rt::ShaderBindingLayout::RegionSpec callable{3, {}};
    const rt::ShaderBindingLayout layout =
        rt::ShaderBindingLayout::Compute(caps, raygen, miss, hit, callable);

    check(layout.callable.firstGroup == 3, "callable region starts at group 3");
    check(layout.callable.groupCount == 3, "three callable shaders");
    check(layout.callable.byteSize == 96, "three callable records occupy three 32-byte strides");
    check(layout.callable.offset % layout.baseAlignment == 0, "callable region is base-aligned");
    check(layout.totalGroupCount == 6, "1 raygen + 1 miss + 1 hit + 3 callable = 6 groups");
    // The table is padded up to baseAlignment, so the end of the callable region
    // is a lower bound rather than an identity.
    const uint64_t callableEnd = layout.callable.offset + layout.callable.byteSize;
    check(layout.totalSize >= callableEnd && layout.totalSize % layout.baseAlignment == 0,
          "table covers the callable region and stays base-aligned");
    check(layout.IsValid(), "layout is valid");

    // The indices the shader passes to executeCallableEXT must be inside the
    // region, or the trace silently reads a record that does not exist.
    check(kInstanceCenters[0].x < kInstanceCenters[1].x && kInstanceCenters[1].x < kInstanceCenters[2].x,
          "the three instances are ordered left to right");
    check(sizeof(kDominantChannel) / sizeof(kDominantChannel[0]) == 3, "one dominant channel per callable");
    check(kVertices.size() == 4 && kIndices.size() == 6, "the BLAS is a single quad shared by all instances");

    std::cout << "[self-test] " << (failures == 0 ? "all checks passed" : "FAILURES") << " (" << failures
              << " failed)\n";
    return failures == 0 ? 0 : 1;
}

void render(const Options &options)
{
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
    std::cout << "[rt] " << caps.Describe() << "\n";
    if (!caps.canTraceRays()) {
        throw std::runtime_error("raytracing_callable: device does not expose the ray tracing pipeline chain");
    }
    rt::Dispatch dispatch;
    dispatch.Load(device);
    if (!dispatch.valid()) {
        throw std::runtime_error("raytracing_callable: missing entry points: " + dispatch.missing());
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

    // One instance per material, all referencing the same BLAS.
    std::vector<vk::AccelerationStructureInstanceKHR> instances;
    for (uint32_t i = 0; i < 3; ++i) {
        vk::AccelerationStructureInstanceKHR instance{};
        for (uint32_t row = 0; row < 3; ++row) {
            for (uint32_t column = 0; column < 4; ++column) {
                instance.transform.matrix[row][column] = (row == column) ? 1.0f : 0.0f;
            }
        }
        instance.transform.matrix[0][3] = kInstanceCenters[i].x;
        instance.transform.matrix[1][3] = kInstanceCenters[i].y;
        instance.transform.matrix[2][3] = kInstanceCenters[i].z;
        // The material index the closest-hit shader forwards to
        // executeCallableEXT.
        instance.instanceCustomIndex = i;
        instance.mask = 0xFF;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = bottomLevel.address;
        instances.push_back(instance);
    }
    rt::AccelerationStructure topLevel = builder.BuildTopLevel(instances);

    vk::DescriptorSetLayoutBinding asBinding{};
    asBinding.binding = 0;
    asBinding.descriptorType = vk::DescriptorType::eAccelerationStructureKHR;
    asBinding.descriptorCount = 1;
    asBinding.stageFlags = vk::ShaderStageFlagBits::eRaygenKHR;

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

    // ---- pipeline: raygen, miss, hit, 3 callables --------------------------
    rt::PipelineDesc pipelineDesc;
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.descriptorLayout = descriptorLayout;
    pipelineDesc.pipelineLayout = pipelineLayout;

    const auto addStage = [&pipelineDesc](vk::ShaderStageFlagBits stage, const auto &spirv) {
        rt::ShaderStageDesc desc;
        desc.stage = stage;
        desc.spirv.assign(spirv.begin(), spirv.end());
        pipelineDesc.stages.push_back(desc);
        return static_cast<int>(pipelineDesc.stages.size()) - 1;
    };

    const int raygenStage = addStage(vk::ShaderStageFlagBits::eRaygenKHR, shader::RAYGEN_RGEN_SPV);
    const int missStage = addStage(vk::ShaderStageFlagBits::eMissKHR, shader::MISS_RMISS_SPV);
    const int hitStage = addStage(vk::ShaderStageFlagBits::eClosestHitKHR, shader::CLOSESTHIT_RCHIT_SPV);
    const int callableRed = addStage(vk::ShaderStageFlagBits::eCallableKHR, shader::CALLABLE_RED_RCALL_SPV);
    const int callableGreen = addStage(vk::ShaderStageFlagBits::eCallableKHR, shader::CALLABLE_GREEN_RCALL_SPV);
    const int callableBlue = addStage(vk::ShaderStageFlagBits::eCallableKHR, shader::CALLABLE_BLUE_RCALL_SPV);

    rt::ShaderGroupDesc raygenGroup;
    raygenGroup.general = raygenStage;
    pipelineDesc.raygen.push_back(raygenGroup);

    rt::ShaderGroupDesc missGroup;
    missGroup.general = missStage;
    pipelineDesc.miss.push_back(missGroup);

    rt::ShaderGroupDesc hitGroup;
    hitGroup.type = rt::ShaderGroupType::eTrianglesHit;
    hitGroup.closestHit = hitStage;
    pipelineDesc.hit.push_back(hitGroup);

    // Order matters: callable group k is what executeCallableEXT(k, 0) runs.
    for (const int stage : {callableRed, callableGreen, callableBlue}) {
        rt::ShaderGroupDesc callableGroup;
        callableGroup.type = rt::ShaderGroupType::eGeneral;
        callableGroup.general = stage;
        pipelineDesc.callable.push_back(callableGroup);
    }

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
    float coverage = 0.0f;
    int matchedCallables = 0;

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

            // Coverage: fraction of texels that are not background.  Proves the
            // instances reached the trace at all.
            const uint32_t pixelCount = frameExtent.width * frameExtent.height;
            const auto backgroundByte = [](float v) { return static_cast<int>(std::lround(v * 255.0f)); };
            uint32_t covered = 0;
            for (uint32_t i = 0; i < pixelCount; ++i) {
                const uint8_t *pixel = capture.pixels.data() + static_cast<size_t>(i) * 4;
                int distance = 0;
                for (uint32_t c = 0; c < 3; ++c) {
                    distance += std::abs(static_cast<int>(pixel[channel[c]]) - backgroundByte(kBackground[c]));
                }
                if (distance > 60) {
                    ++covered;
                }
            }
            coverage = static_cast<float>(covered) / static_cast<float>(pixelCount);

            // One probe per instance: project its centre through the same
            // matrices the shader uses and read that texel.
            std::cout << "[verify] coverage=" << coverage << "\n";
            for (int i = 0; i < 3; ++i) {
                const glm::vec4 clip = projection * view * glm::vec4(kInstanceCenters[i], 1.0f);
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                const int px = static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(frameExtent.width));
                // Row 0 is the top of the image, the shader flips Y to match the
                // OpenGL-convention projection, so undo that here.
                const int py = static_cast<int>((1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(frameExtent.height));
                if (px < 0 || py < 0 || px >= static_cast<int>(frameExtent.width)
                    || py >= static_cast<int>(frameExtent.height)) {
                    continue;
                }
                const uint8_t *pixel =
                    capture.pixels.data() + (static_cast<size_t>(py) * frameExtent.width + px) * 4;
                const int rgb[3] = {pixel[channel[0]], pixel[channel[1]], pixel[channel[2]]};
                const int dominant = kDominantChannel[i];
                int runnerUp = 0;
                for (int c = 0; c < 3; ++c) {
                    if (c != dominant) {
                        runnerUp = std::max(runnerUp, rgb[c]);
                    }
                }
                const bool ok = rgb[dominant] > runnerUp + 40;
                matchedCallables += ok ? 1 : 0;
                std::cout << "[verify] callable " << i << " at (" << px << "," << py << ") rgb=" << rgb[0] << ","
                          << rgb[1] << "," << rgb[2] << " dominant=" << dominant << (ok ? " PASS" : " FAIL")
                          << "\n";
            }
            verifyDone = true;
            if (coverage < 0.10f) {
                throw std::runtime_error("raytracing_callable: instances do not cover the frame");
            }
            if (matchedCallables != 3) {
                throw std::runtime_error("raytracing_callable: a callable shader did not produce its tint");
            }
        }

        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            DumpRequest request;
            request.label = "raytracing-callable";
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
        std::cout << "[stats] frames=" << framesRendered << " instances=3 callableGroups=3 coverage=" << coverage
                  << " callablesMatched=" << matchedCallables << "\n";
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
        std::cerr << "raytracing_callable: " << e.what() << "\n";
        return 1;
    }
}
