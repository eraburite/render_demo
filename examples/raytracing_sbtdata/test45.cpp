// test45 - hardware ray tracing, per-record SBT data
//
// Migrated from Vulkan-Samples raytracingsbtdata.  One BLAS, six instances, six
// closest-hit *groups* that all point at the same SPIR-V module.  What differs
// between them is the payload appended after the group handle in the SBT record,
// which the shader reads through a `layout(shaderRecordEXT) buffer` block.
//
// This is the only RT example where shading constants live in the shader binding
// table rather than in a UBO or push constant, and it is the reason the extension
// layer carries recordData on ShaderGroupDesc: the handle stride has to grow to
// fit it, which changes the SBT layout arithmetic (see DESIGN.md 31.4).
//
// CLI: --self-test  (pure CPU: record stride, distinct colours, instance->record map)
//      --dump [--dump-after N] [--frames N]
//      --verify     (nearest-reference match at each instance centre)
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

struct CameraProperties {
    glm::mat4 viewInverse;
    glm::mat4 projInverse;
};

struct RtVertex {
    glm::vec3 position;
};

// The shaderRecordEXT block: one vec4 per record, std430.  Sixteen bytes is the
// smallest payload that keeps the standard alignment rules happy and makes the
// widened record stride (32 handle + 16 data, rounded to 32) visible in the
// self-test.
struct SbtRecordData {
    glm::vec4 baseColor;
};
constexpr uint32_t kRecordDataSize = sizeof(SbtRecordData);
static_assert(kRecordDataSize == 16, "SBT record payload is one vec4");

constexpr uint32_t kInstanceCount = 6;
constexpr uint32_t kColumns = 3;
constexpr uint32_t kRows = 2;
constexpr float kQuadHalfSize = 0.42f;
constexpr float kColumnSpacing = 1.0f;
constexpr float kRowSpacing = 1.24f;

const std::vector<RtVertex> kVertices = {
    {{-kQuadHalfSize, -kQuadHalfSize, 0.0f}},
    {{kQuadHalfSize, -kQuadHalfSize, 0.0f}},
    {{kQuadHalfSize, kQuadHalfSize, 0.0f}},
    {{-kQuadHalfSize, kQuadHalfSize, 0.0f}},
};
const std::vector<uint32_t> kIndices = {0, 1, 2, 0, 2, 3};

// Six records, six clearly separated colours.  --verify compares each probe
// against all six and requires an unambiguous nearest match, which doubles as a
// check that no two instances ended up sharing a record.
const glm::vec3 kRecordColors[kInstanceCount] = {
    glm::vec3(0.92f, 0.15f, 0.15f), // 0 red
    glm::vec3(0.15f, 0.85f, 0.25f), // 1 green
    glm::vec3(0.20f, 0.35f, 0.95f), // 2 blue
    glm::vec3(0.95f, 0.85f, 0.15f), // 3 yellow
    glm::vec3(0.90f, 0.20f, 0.85f), // 4 magenta
    glm::vec3(0.15f, 0.85f, 0.90f), // 5 cyan
};

// Grid centre of instance i: row = i / 3 (top row first), column = i % 3.
glm::vec3 InstanceCenter(uint32_t index)
{
    const float column = static_cast<float>(index % kColumns) - 1.0f;
    const float row = (index / kColumns) == 0 ? 1.0f : -1.0f;
    return glm::vec3(column * kColumnSpacing, row * kRowSpacing * 0.5f, 0.0f);
}

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

    rt::ShaderBindingLayout::RegionSpec raygen{1, {}};
    rt::ShaderBindingLayout::RegionSpec miss{1, {}};
    // Every hit record carries the same payload size.
    rt::ShaderBindingLayout::RegionSpec hit{kInstanceCount, std::vector<uint32_t>(kInstanceCount, kRecordDataSize)};
    rt::ShaderBindingLayout::RegionSpec callable{0, {}};
    const rt::ShaderBindingLayout layout =
        rt::ShaderBindingLayout::Compute(caps, raygen, miss, hit, callable);

    check(layout.handleSizeAligned == 32, "aligned handle size is 32");
    // 32 bytes of handle + 16 bytes of payload = 48, rounded up to the 32-byte
    // handle alignment.
    check(layout.hit.recordStride == 64, "record stride grows to hold handle + payload");
    check(layout.hit.recordStride >= layout.handleSizeAligned + kRecordDataSize,
          "stride leaves room for the record payload");
    check(layout.hit.groupCount == kInstanceCount, "one hit group per instance");
    check(layout.hit.byteSize == static_cast<uint64_t>(layout.hit.recordStride) * kInstanceCount,
          "hit region holds six widened records");
    check(layout.totalGroupCount == 1 + 1 + kInstanceCount, "raygen + miss + six hit groups");
    check(layout.IsValid(), "layout is valid");

    // Distinctness of the reference colours, in the 0..255 space --verify uses.
    const auto byteOf = [](float v) { return static_cast<int>(std::lround(v * 255.0f)); };
    int minimumDistance = 1 << 30;
    for (uint32_t a = 0; a < kInstanceCount; ++a) {
        for (uint32_t b = a + 1; b < kInstanceCount; ++b) {
            int distance = 0;
            for (int c = 0; c < 3; ++c) {
                distance += std::abs(byteOf(kRecordColors[a][c]) - byteOf(kRecordColors[b][c]));
            }
            minimumDistance = std::min(minimumDistance, distance);
        }
    }
    check(minimumDistance > 120, "record colours are far enough apart to identify a record");

    // The instance -> group mapping must be a permutation of 0..5, otherwise two
    // instances silently share a record and one colour disappears.
    std::array<bool, kInstanceCount> seen{};
    bool permutation = true;
    for (uint32_t i = 0; i < kInstanceCount; ++i) {
        const uint32_t group = i; // instanceShaderBindingTableRecordOffset = i
        if (group >= kInstanceCount || seen[group]) {
            permutation = false;
            break;
        }
        seen[group] = true;
    }
    check(permutation, "each instance maps to its own hit record");

    // Adjacent quads must not overlap, or a probe could land on the neighbour's
    // geometry and blame the wrong record.
    check(kColumnSpacing > 2.0f * kQuadHalfSize, "columns are spaced wider than a quad");
    check(kRowSpacing > 2.0f * kQuadHalfSize, "rows are spaced taller than a quad");

    std::cout << "[self-test] " << (failures == 0 ? "all checks passed" : "FAILURES") << " (" << failures
              << " failed)\n";
    return failures == 0 ? 0 : 1;
}

void render(const Options &options)
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.4f));
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
        throw std::runtime_error("raytracing_sbtdata: device does not expose the ray tracing pipeline chain");
    }
    rt::Dispatch dispatch;
    dispatch.Load(device);
    if (!dispatch.valid()) {
        throw std::runtime_error("raytracing_sbtdata: missing entry points: " + dispatch.missing());
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

    std::vector<vk::AccelerationStructureInstanceKHR> instances;
    for (uint32_t i = 0; i < kInstanceCount; ++i) {
        const glm::vec3 center = InstanceCenter(i);
        vk::AccelerationStructureInstanceKHR instance{};
        for (uint32_t row = 0; row < 3; ++row) {
            for (uint32_t column = 0; column < 4; ++column) {
                instance.transform.matrix[row][column] = (row == column) ? 1.0f : 0.0f;
            }
        }
        instance.transform.matrix[0][3] = center.x;
        instance.transform.matrix[1][3] = center.y;
        instance.transform.matrix[2][3] = center.z;
        instance.instanceCustomIndex = i;
        instance.mask = 0xFF;
        // This is what selects the SBT record: group i of the hit region.
        instance.instanceShaderBindingTableRecordOffset = i;
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

    rt::ShaderGroupDesc raygenGroup;
    raygenGroup.general = raygenStage;
    pipelineDesc.raygen.push_back(raygenGroup);

    rt::ShaderGroupDesc missGroup;
    missGroup.general = missStage;
    pipelineDesc.miss.push_back(missGroup);

    // Six groups, one stage: only the record payload differs.
    for (uint32_t i = 0; i < kInstanceCount; ++i) {
        rt::ShaderGroupDesc hitGroup;
        hitGroup.type = rt::ShaderGroupType::eTrianglesHit;
        hitGroup.closestHit = hitStage;
        const SbtRecordData data{glm::vec4(kRecordColors[i], 1.0f)};
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&data);
        hitGroup.recordData.assign(bytes, bytes + sizeof(data));
        pipelineDesc.hit.push_back(hitGroup);
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
    int matchedRecords = 0;
    int expectedMatches = static_cast<int>(kInstanceCount);

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

            // Predicted bytes for every record.  imageStore writes the linear
            // value verbatim (see rt::StorageByte), so this is a plain scale.
            int references[kInstanceCount][3];
            for (uint32_t r = 0; r < kInstanceCount; ++r) {
                for (uint32_t c = 0; c < 3; ++c) {
                    references[r][c] = static_cast<int>(rt::StorageByte(kRecordColors[r][c]));
                }
            }

            for (uint32_t i = 0; i < kInstanceCount; ++i) {
                const glm::vec4 clip = projection * view * glm::vec4(InstanceCenter(i), 1.0f);
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                const int px = static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(frameExtent.width));
                const int py =
                    static_cast<int>((1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(frameExtent.height));
                if (px < 0 || py < 0 || px >= static_cast<int>(frameExtent.width)
                    || py >= static_cast<int>(frameExtent.height)) {
                    std::cout << "[verify] instance " << i << " projects off-screen, skipped\n";
                    --expectedMatches;
                    continue;
                }
                const uint8_t *pixel =
                    capture.pixels.data() + (static_cast<size_t>(py) * frameExtent.width + px) * 4;
                const int rgb[3] = {pixel[channel[0]], pixel[channel[1]], pixel[channel[2]]};

                int best = -1;
                int bestDistance = 1 << 30;
                int secondDistance = 1 << 30;
                for (uint32_t r = 0; r < kInstanceCount; ++r) {
                    int distance = 0;
                    for (uint32_t c = 0; c < 3; ++c) {
                        distance += std::abs(rgb[c] - references[r][c]);
                    }
                    if (distance < bestDistance) {
                        secondDistance = bestDistance;
                        bestDistance = distance;
                        best = static_cast<int>(r);
                    } else if (distance < secondDistance) {
                        secondDistance = distance;
                    }
                }
                const bool ok = best == static_cast<int>(i) && bestDistance < 40 && secondDistance > bestDistance + 40;
                matchedRecords += ok ? 1 : 0;
                std::cout << "[verify] instance " << i << " at (" << px << "," << py << ") rgb=" << rgb[0] << ","
                          << rgb[1] << "," << rgb[2] << " record=" << best << " distance=" << bestDistance
                          << " runnerUp=" << secondDistance << (ok ? " PASS" : " FAIL") << "\n";
            }
            verifyDone = true;
            if (matchedRecords != expectedMatches) {
                throw std::runtime_error("raytracing_sbtdata: an instance did not read its own SBT record");
            }
        }

        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            DumpRequest request;
            request.label = "raytracing-sbtdata";
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
        std::cout << "[stats] frames=" << framesRendered << " instances=" << kInstanceCount
                  << " hitRecords=" << kInstanceCount << " recordStride=" << pipeline.layout().hit.recordStride
                  << " recordsMatched=" << matchedRecords << "/" << expectedMatches << "\n";
    }

    // Destroying a pipeline / SBT buffer still referenced by the frame that was
    // just presented is undefined behaviour; on this driver it surfaces later as
    // eErrorDeviceLost from the teardown waitIdle (see DESIGN.md 31.8).  The other
    // RT examples wait for idle the same way before tearing down.
    device.waitIdle();
    pipeline.Destroy(device, dispatch);
    sequence.reset();
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
    } catch (const std::exception &error) {
        std::cerr << "raytracing_sbtdata: " << error.what() << "\n";
        return 1;
    }
}
