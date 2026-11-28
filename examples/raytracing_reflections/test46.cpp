// test46 - hardware ray tracing, recursive reflections
//
// Migrated from Vulkan-Samples raytracingreflections.  A ground plane and four
// walls made of scaled instances of one box; the floor and the two blocks are
// partially reflective, so every pixel may trace up to MAX_RECURSION rays before
// the path leaves the scene.
//
// Three things this example adds to the earlier ones:
//   * a payload *struct* carrying colour / distance / normal / reflectivity, so
//     the raygen can continue a path from the point it just hit,
//   * the bounce budget fed in as a specialization constant (constant_id = 0),
//   * a closest-hit shader that reconstructs its triangle from the vertex and
//     index SSBOs, because there is no vertex fetch stage in a ray tracing
//     pipeline.
//
// The reference does its recursion iteratively in the raygen for exactly the
// reason documented in raygen.rgen: nested traceRayEXT calls from a closest-hit
// shader each need their own payload location.
//
// CLI: --self-test  (pure CPU: box topology, outward normals, material table,
//                    bounce budget)
//      --dump [--dump-after N] [--frames N]
//      --verify     (bounce histogram from alpha + floor mirror asymmetry)
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

// Must match the `vec4 materialData[]` block in closesthit.rchit: two vec4 per
// material, albedo.rgb + reflectivity in params.x.
struct GpuMaterial {
    glm::vec4 albedo;
    glm::vec4 params;
};
static_assert(sizeof(GpuMaterial) == 32, "material packing is two vec4");

constexpr int kMaxRecursion = 4;

// ---- scene ----------------------------------------------------------------
//
// One unit box, 4 vertices per face.  Sharing 8 corners would interpolate the
// normals across the edges and turn the box into a lumpy blob, which a mirror
// floor makes very obvious.
struct BoxMesh {
    std::vector<glm::vec4> positions;
    std::vector<glm::vec4> normals;
    std::vector<uint32_t> indices;
};

BoxMesh BuildUnitBox()
{
    BoxMesh mesh;
    const glm::vec3 faces[6][3] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   // +X
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // -X
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},   // +Y
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  // -Y
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // +Z
        {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},  // -Z
    };
    for (const auto &face : faces) {
        const glm::vec3 normal = face[0];
        const glm::vec3 tangent = face[1];
        const glm::vec3 bitangent = face[2];
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
        for (const glm::vec2 corner : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)}) {
            const glm::vec3 position =
                0.5f * normal + 0.5f * corner.x * tangent + 0.5f * corner.y * bitangent;
            mesh.positions.push_back(glm::vec4(position, 1.0f));
            mesh.normals.push_back(glm::vec4(normal, 0.0f));
        }
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return mesh;
}

struct InstanceSpec {
    glm::vec3 scale;
    glm::vec3 translation;
    glm::vec3 albedo;
    float reflectivity;
};

// Floor plus three walls plus two blocks.  The left wall is red and the right one
// green on purpose: with a 55% reflective floor the red wall shows up in the
// floor's left half and the green one in its right half, which is what --verify
// measures.
const std::array<InstanceSpec, 6> kInstances = {{
    {{6.0f, 0.1f, 6.0f}, {0.0f, -0.05f, 0.0f}, {0.72f, 0.72f, 0.75f}, 0.55f}, // floor
    {{6.0f, 2.4f, 0.1f}, {0.0f, 1.2f, -3.0f}, {0.78f, 0.76f, 0.72f}, 0.0f},  // back wall
    {{0.1f, 2.4f, 6.0f}, {-3.0f, 1.2f, 0.0f}, {0.82f, 0.18f, 0.16f}, 0.0f},  // left wall, red
    {{0.1f, 2.4f, 6.0f}, {3.0f, 1.2f, 0.0f}, {0.18f, 0.62f, 0.28f}, 0.0f},   // right wall, green
    {{0.9f, 0.9f, 0.9f}, {-1.1f, 0.45f, -0.4f}, {0.95f, 0.80f, 0.25f}, 0.35f}, // yellow block
    {{1.2f, 1.2f, 1.2f}, {1.1f, 0.60f, -1.5f}, {0.25f, 0.48f, 0.95f}, 0.35f},  // blue block
}};

vk::TransformMatrixKHR TransformOf(const InstanceSpec &spec)
{
    vk::TransformMatrixKHR transform{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            transform.matrix[row][column] = 0.0f;
        }
    }
    transform.matrix[0][0] = spec.scale.x;
    transform.matrix[1][1] = spec.scale.y;
    transform.matrix[2][2] = spec.scale.z;
    transform.matrix[0][3] = spec.translation.x;
    transform.matrix[1][3] = spec.translation.y;
    transform.matrix[2][3] = spec.translation.z;
    return transform;
}

int runSelfTest()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char *what) {
        std::cout << "[self-test] " << (condition ? "PASS " : "FAIL ") << what << "\n";
        if (!condition) {
            ++failures;
        }
    };

    const BoxMesh box = BuildUnitBox();
    check(box.positions.size() == 24, "box has four vertices per face");
    check(box.normals.size() == box.positions.size(), "one normal per vertex");
    check(box.indices.size() == 36, "box has twelve triangles");

    // Every face normal must point away from the centre; a flipped face shows up
    // as a black facet the moment a mirror reflects it.
    bool outward = true;
    for (size_t face = 0; face < 6; ++face) {
        const glm::vec3 normal = glm::vec3(box.normals[face * 4]);
        const glm::vec3 centroid = 0.25f * (glm::vec3(box.positions[face * 4]) + glm::vec3(box.positions[face * 4 + 1])
                                            + glm::vec3(box.positions[face * 4 + 2])
                                            + glm::vec3(box.positions[face * 4 + 3]));
        if (glm::dot(normal, centroid) <= 0.0f) {
            outward = false;
        }
    }
    check(outward, "every box face normal points outward");

    // A budget of one would make this example indistinguishable from test42: the
    // loop has to be allowed at least one bounce to reflect anything.
    check(kMaxRecursion >= 2, "bounce budget allows at least one reflection");

    check(kInstances.size() == 6, "floor + three walls + two blocks");
    bool floorLowest = true;
    for (size_t i = 1; i < 3; ++i) {
        if (kInstances[i].translation.y <= kInstances[0].translation.y) {
            floorLowest = false;
        }
    }
    check(floorLowest, "floor sits below the walls");
    check(kInstances[2].albedo.r > kInstances[2].albedo.g, "left wall is red");
    check(kInstances[3].albedo.g > kInstances[3].albedo.r, "right wall is green");
    check(kInstances[0].reflectivity > 0.3f, "the floor actually reflects");

    std::cout << "[self-test] " << (failures == 0 ? "all checks passed" : "FAILURES") << " (" << failures
              << " failed)\n";
    return failures == 0 ? 0 : 1;
}

void render(const Options &options)
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 1.5f, 5.6f));
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
        throw std::runtime_error("raytracing_reflections: device does not expose the ray tracing pipeline chain");
    }
    rt::Dispatch dispatch;
    dispatch.Load(device);
    if (!dispatch.valid()) {
        throw std::runtime_error("raytracing_reflections: missing entry points: " + dispatch.missing());
    }

    const BoxMesh box = BuildUnitBox();

    const vk::BufferUsageFlags inputUsage = vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
                                            | vk::BufferUsageFlagBits::eStorageBuffer;
    const vk::BufferUsageFlags storageUsage = vk::BufferUsageFlagBits::eStorageBuffer;

    rt::DeviceAddressBuffer positionBuffer;
    positionBuffer.Create(device, memProps, dispatch, box.positions.size() * sizeof(glm::vec4), inputUsage, true);
    positionBuffer.Upload(box.positions.data(), box.positions.size() * sizeof(glm::vec4));

    rt::DeviceAddressBuffer normalBuffer;
    normalBuffer.Create(device, memProps, dispatch, box.normals.size() * sizeof(glm::vec4), inputUsage, true);
    normalBuffer.Upload(box.normals.data(), box.normals.size() * sizeof(glm::vec4));

    rt::DeviceAddressBuffer indexBuffer;
    indexBuffer.Create(device, memProps, dispatch, box.indices.size() * sizeof(uint32_t), inputUsage, true);
    indexBuffer.Upload(box.indices.data(), box.indices.size() * sizeof(uint32_t));

    std::vector<GpuMaterial> materials;
    materials.reserve(kInstances.size());
    for (const InstanceSpec &spec : kInstances) {
        materials.push_back(GpuMaterial{glm::vec4(spec.albedo, 1.0f), glm::vec4(spec.reflectivity, 0.0f, 0.0f, 0.0f)});
    }
    rt::DeviceAddressBuffer materialBuffer;
    materialBuffer.Create(device, memProps, dispatch, materials.size() * sizeof(GpuMaterial), storageUsage, true);
    materialBuffer.Upload(materials.data(), materials.size() * sizeof(GpuMaterial));

    rt::AccelerationStructureBuilder builder(device, memProps, queue, queueFamily, dispatch);
    rt::TriangleGeometry geometry;
    geometry.vertexBuffer = positionBuffer;
    geometry.indexBuffer = indexBuffer;
    geometry.vertexCount = static_cast<uint32_t>(box.positions.size());
    geometry.indexCount = static_cast<uint32_t>(box.indices.size());
    geometry.vertexStride = sizeof(glm::vec4);
    geometry.hasIndices = true;
    rt::AccelerationStructure bottomLevel = builder.BuildBottomLevel(geometry);

    std::vector<vk::AccelerationStructureInstanceKHR> instances;
    instances.reserve(kInstances.size());
    for (uint32_t i = 0; i < kInstances.size(); ++i) {
        vk::AccelerationStructureInstanceKHR instance{};
        instance.transform = TransformOf(kInstances[i]);
        instance.instanceCustomIndex = i;
        instance.mask = 0xFF;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = bottomLevel.address;
        instances.push_back(instance);
    }
    rt::AccelerationStructure topLevel = builder.BuildTopLevel(instances);

    // ---- descriptors -------------------------------------------------------
    const auto makeBinding = [](uint32_t binding, vk::DescriptorType type,
                                vk::ShaderStageFlags stages) {
        vk::DescriptorSetLayoutBinding entry{};
        entry.binding = binding;
        entry.descriptorType = type;
        entry.descriptorCount = 1;
        entry.stageFlags = stages;
        return entry;
    };
    const vk::ShaderStageFlags raygenOnly = vk::ShaderStageFlagBits::eRaygenKHR;
    const vk::ShaderStageFlags hitOnly = vk::ShaderStageFlagBits::eClosestHitKHR;

    const std::array<vk::DescriptorSetLayoutBinding, 7> bindings = {
        makeBinding(0, vk::DescriptorType::eAccelerationStructureKHR, raygenOnly),
        makeBinding(1, vk::DescriptorType::eStorageImage, raygenOnly),
        makeBinding(2, vk::DescriptorType::eUniformBuffer, raygenOnly),
        makeBinding(3, vk::DescriptorType::eStorageBuffer, hitOnly),
        makeBinding(4, vk::DescriptorType::eStorageBuffer, hitOnly),
        makeBinding(5, vk::DescriptorType::eStorageBuffer, hitOnly),
        makeBinding(6, vk::DescriptorType::eStorageBuffer, hitOnly),
    };
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
    pipelineDesc.maxRecursionDepth = 1; // iterative reflection needs a single level
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

    // constant_id = 0 -> max recursion, without recompiling the shader.
    rt::ShaderStageDesc &raygenDesc = pipelineDesc.stages[raygenStage];
    raygenDesc.specEntries.push_back(vk::SpecializationMapEntry{0, 0, sizeof(uint32_t)});
    raygenDesc.specData.resize(sizeof(uint32_t));
    const uint32_t recursion = static_cast<uint32_t>(kMaxRecursion);
    std::memcpy(raygenDesc.specData.data(), &recursion, sizeof(recursion));

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

    rt::RayTracingPipeline pipeline;
    pipeline.Create(device, memProps, dispatch, caps, pipelineDesc);

    const std::array<vk::DescriptorPoolSize, 4> poolSizes = {
        vk::DescriptorPoolSize{vk::DescriptorType::eAccelerationStructureKHR, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 4},
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

    vk::DescriptorBufferInfo positionInfo{positionBuffer.buffer, 0, box.positions.size() * sizeof(glm::vec4)};
    vk::DescriptorBufferInfo normalInfo{normalBuffer.buffer, 0, box.normals.size() * sizeof(glm::vec4)};
    vk::DescriptorBufferInfo indexInfo{indexBuffer.buffer, 0, box.indices.size() * sizeof(uint32_t)};
    vk::DescriptorBufferInfo materialInfo{materialBuffer.buffer, 0, materials.size() * sizeof(GpuMaterial)};

    std::array<vk::WriteDescriptorSet, 4> bufferDescriptors{};
    const std::array<vk::DescriptorBufferInfo *, 4> bufferInfos = {&materialInfo, &positionInfo, &normalInfo,
                                                                   &indexInfo};
    for (uint32_t i = 0; i < bufferDescriptors.size(); ++i) {
        bufferDescriptors[i].dstSet = descriptorSet;
        bufferDescriptors[i].dstBinding = 3 + i;
        bufferDescriptors[i].descriptorCount = 1;
        bufferDescriptors[i].descriptorType = vk::DescriptorType::eStorageBuffer;
        bufferDescriptors[i].pBufferInfo = bufferInfos[i];
    }

    std::vector<vk::WriteDescriptorSet> writes = {asDescriptor, imageDescriptor, cameraDescriptor};
    writes.insert(writes.end(), bufferDescriptors.begin(), bufferDescriptors.end());
    device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    auto sequence = mgr->DrawFrameSequence();
    uint32_t framesRendered = 0;
    bool dumpDone = false;
    bool verifyDone = false;
    int verifyStage = 0;
    std::vector<uint8_t> reflectiveFrame;
    float floorAsymmetry = 0.0f;
    float leftRedness = 0.0f;
    float rightRedness = 0.0f;

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
            const uint32_t width = frameExtent.width;
            const uint32_t height = frameExtent.height;

            const auto meanRedness = [&capture, &channel, width](uint32_t x0, uint32_t x1, uint32_t y0,
                                                                 uint32_t y1) {
                double sum = 0.0;
                uint32_t count = 0;
                for (uint32_t y = y0; y < y1; ++y) {
                    for (uint32_t x = x0; x < x1; ++x) {
                        const uint8_t *pixel = capture.pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
                        sum += static_cast<double>(pixel[channel[0]]) - static_cast<double>(pixel[channel[1]]);
                        ++count;
                    }
                }
                return count == 0 ? 0.0f : static_cast<float>(sum / count);
            };

            if (verifyStage == 0) {
                // Pass 1: the real frame.  Keep the pixels and re-run the same
                // geometry, camera and lighting with every reflectivity forced to
                // zero, so pass 2 differs only in whether the loop recursed.
                reflectiveFrame = capture.pixels;

                // Mirror asymmetry.  The floor band just below the horizon reflects
                // the red wall on its left and the green one on its right, so
                // red-minus-green must differ between the two halves.  Patching the
                // loop away (pass 2) leaves the floor near-neutral on both sides,
                // which is what makes this a reflection test and not a lighting one.
                const uint32_t bandTop = height * 52 / 100;
                const uint32_t bandBottom = height * 64 / 100;
                leftRedness = meanRedness(width * 15 / 100, width * 32 / 100, bandTop, bandBottom);
                rightRedness = meanRedness(width * 68 / 100, width * 85 / 100, bandTop, bandBottom);
                floorAsymmetry = leftRedness - rightRedness;
                std::cout << "[verify] floor redness left=" << leftRedness << " right=" << rightRedness
                          << " asymmetry=" << floorAsymmetry << "\n";

                std::vector<GpuMaterial> diffuse = materials;
                for (GpuMaterial &material : diffuse) {
                    material.params.x = 0.0f;
                }
                // Both the frame that was just submitted and the readback still
                // reference this buffer.
                device.waitIdle();
                materialBuffer.Upload(diffuse.data(), diffuse.size() * sizeof(GpuMaterial));
                verifyStage = 1;
            } else {
                // Pass 2: identical frame, reflections off.  The delta is exactly
                // what the reflection loop contributes.
                double sum = 0.0;
                uint32_t differing = 0;
                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                        int delta = 0;
                        for (uint32_t c = 0; c < 3; ++c) {
                            delta += std::abs(static_cast<int>(reflectiveFrame[offset + channel[c]])
                                              - static_cast<int>(capture.pixels[offset + channel[c]]));
                        }
                        sum += delta / 3.0;
                        if (delta > 24) {
                            ++differing;
                        }
                    }
                }
                const double pixelCount = static_cast<double>(width) * static_cast<double>(height);
                const float meanDifference = static_cast<float>(sum / pixelCount);
                const float differingFraction = static_cast<float>(differing / pixelCount);
                std::cout << "[verify] reflections-off difference mean=" << meanDifference
                          << " changedFraction=" << differingFraction << "\n";
                verifyDone = true;

                if (meanDifference < 3.0f) {
                    throw std::runtime_error("raytracing_reflections: reflections barely change the frame");
                }
                if (differingFraction < 0.15f) {
                    throw std::runtime_error("raytracing_reflections: too few pixels driven by reflections");
                }
                if (floorAsymmetry < 12.0f) {
                    throw std::runtime_error("raytracing_reflections: floor does not mirror the coloured walls");
                }
            }
        }

        // --verify needs a second frame (reflections off), so the dump must wait
        // for the verification to finish rather than stopping at dumpAfter.
        const bool verifyBlocksDump = options.verify && !verifyDone;
        if (options.dump && !dumpDone && !verifyBlocksDump && framesRendered >= options.dumpAfter) {
            DumpRequest request;
            request.label = "raytracing-reflections";
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
        std::cout << "[stats] frames=" << framesRendered << " instances=" << kInstances.size()
                  << " maxRecursion=" << kMaxRecursion << " triangleCount=" << box.indices.size() / 3
                  << " floorAsymmetry=" << floorAsymmetry << "\n";
    }

    pipeline.Destroy(device, dispatch);
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
        std::cerr << "raytracing_reflections: " << error.what() << "\n";
        return 1;
    }
}
