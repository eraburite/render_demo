// test47 - ray query (VK_KHR_ray_query) in a compute shader
//
// Migrated from Vulkan-Samples `rayquery`.  The capability under test is the one
// the other examples do not touch: no ray tracing pipeline, no shader binding
// table, no traceRayEXT.  The acceleration structure is an ordinary descriptor
// and the shader walks it with rayQueryInitializeEXT / rayQueryProceedEXT, which
// is what lets an application add ray traced shadows to a rasteriser it already
// has.
//
// The reference rasterises the scene and runs the query in the fragment stage.
// This port keeps the mechanism and the hybrid shape -- camera-driven primary
// rays, analytic shading, ray-query shadow rays -- but runs it in a compute
// shader: the example-side descriptor atlas has no acceleration-structure
// component, and adding one would change the raster path shared by 41 examples
// (see DESIGN.md 31.7).
//
// CLI: --self-test  (pure CPU: geometry, material table, light direction)
//      --dump [--dump-after N] [--frames N]
//      --verify     (A/B: shadows on vs off, plus the shadowed fraction)
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
#include "ShaderBindingTable.h"

#include "shader/scene_comp.hpp"

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

// Must match the compute shader's CameraProperties block: two matrices plus a
// flags vector, whose x component toggles the shadow query.
struct CameraProperties {
    glm::mat4 viewInverse;
    glm::mat4 projInverse;
    glm::vec4 flags;
};
static_assert(sizeof(CameraProperties) == 144, "camera UBO layout matches std140");

struct GpuMaterial {
    glm::vec4 albedo;
    glm::vec4 params;
};
static_assert(sizeof(GpuMaterial) == 32, "material packing is two vec4");

// ---- scene ----------------------------------------------------------------

struct BoxMesh {
    std::vector<glm::vec4> positions;
    std::vector<glm::vec4> normals;
    std::vector<uint32_t> indices;
};

BoxMesh BuildUnitBox()
{
    BoxMesh mesh;
    const glm::vec3 faces[6][3] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    for (const auto &face : faces) {
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
        for (const glm::vec2 corner : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)}) {
            mesh.positions.push_back(glm::vec4(0.5f * face[0] + 0.5f * corner.x * face[1] + 0.5f * corner.y * face[2],
                                               1.0f));
            mesh.normals.push_back(glm::vec4(face[0], 0.0f));
        }
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return mesh;
}

struct InstanceSpec {
    glm::vec3 scale;
    glm::vec3 translation;
    glm::vec3 albedo;
};

// Floor plus two blocks.  Small on purpose: the point is the query, and three
// objects give three shading populations to check against each other.
const std::array<InstanceSpec, 3> kInstances = {{
    {{12.0f, 0.1f, 12.0f}, {0.0f, -0.05f, 0.0f}, {0.70f, 0.70f, 0.72f}},
    {{1.2f, 1.2f, 1.2f}, {-1.2f, 0.60f, 0.2f}, {0.90f, 0.72f, 0.20f}},
    {{1.8f, 1.8f, 1.8f}, {1.4f, 0.90f, -1.6f}, {0.22f, 0.45f, 0.92f}},
}};

// Must match kLightDirection in scene.comp.
const glm::vec3 kLightDirection = glm::normalize(glm::vec3(0.95f, 0.40f, 0.25f));

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
    check(box.positions.size() == 24 && box.indices.size() == 36, "box is 24 vertices / 12 triangles");

    bool outward = true;
    for (size_t face = 0; face < 6; ++face) {
        const glm::vec3 normal = glm::vec3(box.normals[face * 4]);
        const glm::vec3 centroid = 0.25f * (glm::vec3(box.positions[face * 4]) + glm::vec3(box.positions[face * 4 + 1])
                                            + glm::vec3(box.positions[face * 4 + 2])
                                            + glm::vec3(box.positions[face * 4 + 3]));
        outward = outward && glm::dot(normal, centroid) > 0.0f;
    }
    check(outward, "every box face normal points outward");

    // The light must come from above and slightly to one side, otherwise nothing
    // casts a shadow onto the floor and the shadow query has nothing to find.
    check(kLightDirection.y > 0.2f, "light points down onto the floor");
    check(std::abs(kLightDirection.x) > 0.1f, "light is off-axis, so shadows land beside the blocks");

    // Floor first, blocks above it: the shadow ray from the blocks has to reach
    // the floor to matter.
    check(kInstances[0].translation.y < kInstances[1].translation.y, "blocks stand on the floor");
    check(kInstances.size() == 3, "floor + two blocks");

    // The UBO carries a 16-byte flag vector; std140 would pad a bare float to 16
    // anyway, so making it explicit keeps the C++ and GLSL layouts identical.
    check(sizeof(CameraProperties) == 2 * 64 + 16, "camera UBO is two mat4 plus one vec4");

    std::cout << "[self-test] " << (failures == 0 ? "all checks passed" : "FAILURES") << " (" << failures
              << " failed)\n";
    return failures == 0 ? 0 : 1;
}

void render(const Options &options)
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 3.0f, 6.6f));
    camera->Zoom = 45.0f;
    GLFWwindow window(900, 640, camera);

    DeviceRequirement requirement;
    requirement.rayQuery = true;
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT, requirement);

    const vk::Device device = *mgr->getLogicalDevice();
    const vk::PhysicalDevice physicalDevice = *mgr->getPhysicalDevice();
    const vk::Queue queue = *mgr->getGraphicsQueue();
    const uint32_t queueFamily = *mgr->getQueueFamilyIndices().graphicsFamily;
    const vk::PhysicalDeviceMemoryProperties memProps = mgr->memoryProperties();

    const rt::Capabilities caps = rt::Capabilities::Query(physicalDevice);
    std::cout << "[rt] " << caps.Describe() << "\n";
    if (!caps.rayQuery) {
        throw std::runtime_error("ray_query: device does not support VK_KHR_ray_query");
    }
    rt::Dispatch dispatch;
    dispatch.Load(device);
    if (!dispatch.accelerationStructureReady()) {
        throw std::runtime_error("ray_query: missing entry points: " + dispatch.missing());
    }

    const BoxMesh box = BuildUnitBox();
    const vk::BufferUsageFlags inputUsage = vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR
                                            | vk::BufferUsageFlagBits::eStorageBuffer;

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
        materials.push_back(GpuMaterial{glm::vec4(spec.albedo, 1.0f), glm::vec4(0.0f)});
    }
    rt::DeviceAddressBuffer materialBuffer;
    materialBuffer.Create(device, memProps, dispatch, materials.size() * sizeof(GpuMaterial),
                          vk::BufferUsageFlagBits::eStorageBuffer, true);
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

    // ---- descriptor set: everything the compute shader reads ----------------
    const auto makeBinding = [](uint32_t binding, vk::DescriptorType type) {
        vk::DescriptorSetLayoutBinding entry{};
        entry.binding = binding;
        entry.descriptorType = type;
        entry.descriptorCount = 1;
        entry.stageFlags = vk::ShaderStageFlagBits::eCompute;
        return entry;
    };
    const std::array<vk::DescriptorSetLayoutBinding, 7> bindings = {
        makeBinding(0, vk::DescriptorType::eAccelerationStructureKHR),
        makeBinding(1, vk::DescriptorType::eStorageImage),
        makeBinding(2, vk::DescriptorType::eUniformBuffer),
        makeBinding(3, vk::DescriptorType::eStorageBuffer),
        makeBinding(4, vk::DescriptorType::eStorageBuffer),
        makeBinding(5, vk::DescriptorType::eStorageBuffer),
        makeBinding(6, vk::DescriptorType::eStorageBuffer),
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

    // ---- compute pipeline ---------------------------------------------------
    vk::ShaderModuleCreateInfo moduleInfo{};
    moduleInfo.codeSize = shader::SCENE_COMP_SPV.size() * sizeof(uint32_t);
    moduleInfo.pCode = shader::SCENE_COMP_SPV.data();
    const vk::ShaderModule module = device.createShaderModule(moduleInfo);

    vk::PipelineShaderStageCreateInfo stageInfo{};
    stageInfo.stage = vk::ShaderStageFlagBits::eCompute;
    stageInfo.module = module;
    stageInfo.pName = "main";

    vk::ComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = pipelineLayout;
    const vk::Pipeline pipeline = device.createComputePipeline(nullptr, pipelineInfo).value;
    device.destroyShaderModule(module);

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

    vk::DescriptorBufferInfo cameraBufferInfo{cameraUbo.buffer, 0, sizeof(CameraProperties)};

    vk::WriteDescriptorSet cameraDescriptor{};
    cameraDescriptor.dstSet = descriptorSet;
    cameraDescriptor.dstBinding = 2;
    cameraDescriptor.descriptorCount = 1;
    cameraDescriptor.descriptorType = vk::DescriptorType::eUniformBuffer;
    cameraDescriptor.pBufferInfo = &cameraBufferInfo;

    vk::DescriptorBufferInfo materialInfo{materialBuffer.buffer, 0, materials.size() * sizeof(GpuMaterial)};
    vk::DescriptorBufferInfo positionInfo{positionBuffer.buffer, 0, box.positions.size() * sizeof(glm::vec4)};
    vk::DescriptorBufferInfo normalInfo{normalBuffer.buffer, 0, box.normals.size() * sizeof(glm::vec4)};
    vk::DescriptorBufferInfo indexInfo{indexBuffer.buffer, 0, box.indices.size() * sizeof(uint32_t)};

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
    std::vector<uint8_t> litFrame;
    float shadowedFraction = 0.0f;
    // The loop re-uploads the camera UBO every frame, so the A/B toggle lives
    // here rather than being written once into the buffer (that write would be
    // overwritten by the next frame).
    bool shadowsEnabled = true;

    const uint32_t groupCountX = (extent.width + 7) / 8;
    const uint32_t groupCountY = (extent.height + 7) / 8;

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
        cameraProperties.flags = glm::vec4(shadowsEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        cameraUbo.Upload(&cameraProperties, sizeof(cameraProperties));

        sequence->beginRecording();
        sequence->recordRaw([&](vk::CommandBuffer cmd, uint32_t imageIndex) {
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipelineLayout, 0, 1, &descriptorSet, 0,
                                   nullptr);
            cmd.dispatch(groupCountX, groupCountY, 1);
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

            if (verifyStage == 0) {
                // Pass 1: shadow rays on.  Count how much of the frame the shadow
                // query darkened, then re-run the identical frame with the query
                // disabled.
                litFrame = capture.pixels;
                uint32_t dark = 0;
                for (uint32_t i = 0; i < width * height; ++i) {
                    const uint8_t *pixel = capture.pixels.data() + static_cast<size_t>(i) * 4;
                    const int luminance = (static_cast<int>(pixel[channel[0]]) + pixel[channel[1]]
                                           + pixel[channel[2]]) / 3;
                    if (luminance < 40) {
                        ++dark;
                    }
                }
                shadowedFraction = static_cast<float>(dark) / static_cast<float>(width * height);
                std::cout << "[verify] dark fraction with shadows = " << shadowedFraction << "\n";

                shadowsEnabled = false;
                verifyStage = 1;
            } else {
                // Pass 2: same geometry, shadow query skipped.  Every pixel that
                // changes is a pixel the shadow ray decided.
                double sum = 0.0;
                double sumChanged = 0.0;
                uint32_t changed = 0;
                uint32_t darker = 0;
                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                        int delta = 0;
                        int withShadows = 0;
                        int withoutShadows = 0;
                        for (uint32_t c = 0; c < 3; ++c) {
                            const int a = static_cast<int>(litFrame[offset + channel[c]]);
                            const int b = static_cast<int>(capture.pixels[offset + channel[c]]);
                            delta += std::abs(a - b);
                            withShadows += a;
                            withoutShadows += b;
                        }
                        sum += delta / 3.0;
                        if (delta > 24) {
                            ++changed;
                            sumChanged += delta / 3.0;
                            if (withShadows < withoutShadows) {
                                ++darker;
                            }
                        }
                    }
                }
                const double pixelCount = static_cast<double>(width) * static_cast<double>(height);
                const float meanDifference = static_cast<float>(sum / pixelCount);
                const float changedFraction = static_cast<float>(changed / pixelCount);
                const float darkerFraction = changed == 0 ? 0.0f
                                                         : static_cast<float>(darker) / static_cast<float>(changed);
                // Magnitude over the pixels that changed at all: a shadow is a
                // local effect, so averaging it over the whole frame (mostly sky)
                // would report a number that says more about the framing than
                // about the query.
                const float meanShadowDepth =
                    changed == 0 ? 0.0f : static_cast<float>(sumChanged / static_cast<double>(changed));
                std::cout << "[verify] shadows-on vs off: meanAll=" << meanDifference
                          << " changedFraction=" << changedFraction << " meanShadowDepth=" << meanShadowDepth
                          << " darkerShare=" << darkerFraction << "\n";
                verifyDone = true;

                if (changedFraction < 0.02f) {
                    throw std::runtime_error("ray_query: the shadow query changed too few pixels");
                }
                if (meanShadowDepth < 20.0f) {
                    throw std::runtime_error("ray_query: the shadowed pixels are barely darker");
                }
                // The query may only ever remove light: if the shadowed frame were
                // brighter anywhere, the ray would be pointing the wrong way.
                if (darkerFraction < 0.95f) {
                    throw std::runtime_error("ray_query: shadow rays brightened part of the frame");
                }
                if (shadowedFraction < 0.02f) {
                    throw std::runtime_error("ray_query: no visibly shadowed region");
                }
            }
        }

        // --verify needs a second frame (shadows off), so the dump waits for the
        // verification to finish instead of stopping at dumpAfter.
        const bool verifyBlocksDump = options.verify && !verifyDone;
        if (options.dump && !dumpDone && !verifyBlocksDump && framesRendered >= options.dumpAfter) {
            DumpRequest request;
            request.label = "ray-query";
            request.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            request.format = swapFormat;
            request.extent = frameExtent;
            request.layout = vk::ImageLayout::ePresentSrcKHR;
            request.encoding = DumpEncoding::eColor;
            for (const DumpResult &result : DumpImages(readback, {request}, "dump", 0)) {
                std::cout << "[dump] " << result.path.string() << " mean=(" << result.meanColor.r << ","
                          << result.meanColor.g << "," << result.meanColor.b << ")\n";
            }
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
                  << " dispatch=" << groupCountX << "x" << groupCountY << " shadowedFraction=" << shadowedFraction
                  << "\n";
    }

    device.waitIdle();
    device.destroyPipeline(pipeline);
    device.destroyPipelineLayout(pipelineLayout);
    device.destroyDescriptorSetLayout(descriptorLayout);
    device.destroyDescriptorPool(descriptorPool);
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
        std::cerr << "ray_query: " << error.what() << "\n";
        return 1;
    }
}
