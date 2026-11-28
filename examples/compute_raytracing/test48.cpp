// test48 - ray tracing without any ray tracing extension
//
// Migrated from Vulkan-Samples `computeraytracing`.  There is no acceleration
// structure, no ray query and no ray tracing pipeline here: the scene is a signed
// distance field, the rays march it, and the whole thing is a compute shader.
// That is why this example also serves as the documented fallback -- it needs
// nothing beyond a compute queue, so it runs on hardware that supports none of
// the extensions the other RT examples require.
//
// The reference voxelises Sponza and accumulates samples over frames.  This port
// ray-marches an analytic SDF (floor + two boxes) instead, which keeps the
// example self-contained; the marshalling of primary rays, shadow rays and the
// per-pixel loop is the same technique.
//
// CLI: --self-test  (pure CPU: the same SDF, mirrored, so the shader's field is
//                    checked from outside the shader)
//      --dump [--dump-after N] [--frames N]
//      --verify     (A/B: shadow rays on vs off)
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
#include "RayTracingCommon.h"

#include "shader/raymarch_comp.hpp"

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
    glm::vec4 flags;
};
static_assert(sizeof(CameraProperties) == 144, "camera UBO layout matches std140");

// ---- the scene, mirrored from raymarch.comp --------------------------------
//
// Duplicating four lines of SDF in C++ is the point: --self-test can then assert
// the signs the shader depends on without a GPU, and a typo in either copy shows
// up as a failed check instead of a wrong picture.
constexpr glm::vec3 kBoxACenter(-1.15f, 0.55f, 0.10f);
constexpr glm::vec3 kBoxAHalf(0.55f);
constexpr glm::vec3 kBoxBCenter(1.30f, 0.80f, -1.10f);
constexpr glm::vec3 kBoxBHalf(0.80f);

float SdBox(glm::vec3 p, glm::vec3 center, glm::vec3 halfExtent)
{
    const glm::vec3 q = glm::abs(p - center) - halfExtent;
    return glm::length(glm::max(q, glm::vec3(0.0f)))
           + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
}

float SceneDistance(glm::vec3 p)
{
    return std::min(p.y, std::min(SdBox(p, kBoxACenter, kBoxAHalf), SdBox(p, kBoxBCenter, kBoxBHalf)));
}

const glm::vec3 kLightDirection = glm::normalize(glm::vec3(0.75f, 0.60f, 0.35f));
const glm::vec3 kCameraPosition(0.0f, 2.2f, 4.4f);

int runSelfTest()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char *what) {
        std::cout << "[self-test] " << (condition ? "PASS " : "FAIL ") << what << "\n";
        if (!condition) {
            ++failures;
        }
    };

    // Sign checks: a distance field is only usable if "inside" is negative and
    // "outside" is the true distance, because the marcher steps by the value.
    check(SceneDistance(kBoxACenter) < 0.0f, "box A centre is inside the field");
    check(SceneDistance(kBoxBCenter) < 0.0f, "box B centre is inside the field");
    check(SceneDistance(glm::vec3(0.0f, 4.0f, 0.0f)) > 0.0f, "the air above the scene is outside");
    check(std::abs(SceneDistance(glm::vec3(0.0f, 2.5f, 5.0f)) - 2.5f) < 1e-4f,
          "away from the boxes the field is the floor height");

    // A point just above the floor must be positive but small: that is the range
    // the march converges through.
    const float nearFloor = SceneDistance(glm::vec3(3.0f, 0.01f, 3.0f));
    check(nearFloor > 0.0f && nearFloor < 0.05f, "just above the floor the field is small and positive");

    // The boxes must not intersect each other or the camera: overlapping volumes
    // would make min() pick one of them and silently shade the union.
    const glm::vec3 gap = glm::abs(kBoxACenter - kBoxBCenter) - (kBoxAHalf + kBoxBHalf);
    check(gap.x > 0.0f || gap.z > 0.0f, "the two boxes are disjoint");
    check(SceneDistance(kCameraPosition) > 0.1f, "the camera starts outside the geometry");

    // The light has to come from above, or the floor receives no light at all and
    // the shadow test has nothing to remove.
    check(kLightDirection.y > 0.4f, "light points down onto the floor");
    check(std::abs(glm::length(kLightDirection) - 1.0f) < 1e-4f, "light direction is normalised");

    std::cout << "[self-test] " << (failures == 0 ? "all checks passed" : "FAILURES") << " (" << failures
              << " failed)\n";
    return failures == 0 ? 0 : 1;
}

void render(const Options &options)
{
    auto camera = std::make_shared<Camera>(kCameraPosition);
    camera->Zoom = 45.0f;
    GLFWwindow window(900, 640, camera);

    // No DeviceRequirement flags at all: this example must run on a device that
    // supports none of the ray tracing extensions.
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const vk::Device device = *mgr->getLogicalDevice();
    const vk::PhysicalDeviceMemoryProperties memProps = mgr->memoryProperties();
    const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
    const vk::Format swapFormat = *mgr->getSwapChain()->getImageFormat();
    const uint32_t queueFamily = *mgr->getQueueFamilyIndices().graphicsFamily;

    rt::StorageImage storage;
    storage.Create(device, memProps, swapFormat, extent);
    storage.TransitionToGeneral(device, *mgr->getGraphicsQueue(), queueFamily);

    // A plain mapped UBO: this example has no acceleration structure or device
    // address machinery behind it (see RecordCopyToSwapchain for the only shared
    // helper it uses).
    vk::BufferCreateInfo bufferInfo{};
    bufferInfo.size = sizeof(CameraProperties);
    bufferInfo.usage = vk::BufferUsageFlagBits::eUniformBuffer;
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;
    const vk::Buffer cameraBuffer = device.createBuffer(bufferInfo);

    vk::MemoryRequirements requirements{};
    device.getBufferMemoryRequirements(cameraBuffer, &requirements);
    uint32_t memoryType = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i))
            && (memProps.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible)
                   == vk::MemoryPropertyFlagBits::eHostVisible) {
            memoryType = i;
            break;
        }
    }
    vk::MemoryAllocateInfo allocateInfo{};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    const vk::DeviceMemory cameraMemory = device.allocateMemory(allocateInfo);
    device.bindBufferMemory(cameraBuffer, cameraMemory, 0);
    void *mapped = device.mapMemory(cameraMemory, 0, sizeof(CameraProperties));

    // ---- descriptor set -----------------------------------------------------
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {
        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eStorageImage, 1,
                                       vk::ShaderStageFlagBits::eCompute, nullptr},
        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eUniformBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute, nullptr},
    };
    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    const vk::DescriptorSetLayout descriptorLayout = device.createDescriptorSetLayout(layoutInfo);

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
    const vk::PipelineLayout pipelineLayout = device.createPipelineLayout(pipelineLayoutInfo);

    vk::ShaderModuleCreateInfo moduleInfo{};
    moduleInfo.codeSize = shader::RAYMARCH_COMP_SPV.size() * sizeof(uint32_t);
    moduleInfo.pCode = shader::RAYMARCH_COMP_SPV.data();
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

    const std::array<vk::DescriptorPoolSize, 2> poolSizes = {
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 1},
    };
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    const vk::DescriptorPool descriptorPool = device.createDescriptorPool(poolInfo);

    vk::DescriptorSetAllocateInfo setAllocateInfo{};
    setAllocateInfo.descriptorPool = descriptorPool;
    setAllocateInfo.descriptorSetCount = 1;
    setAllocateInfo.pSetLayouts = &descriptorLayout;
    const vk::DescriptorSet descriptorSet = device.allocateDescriptorSets(setAllocateInfo).front();

    vk::DescriptorImageInfo imageInfo{};
    imageInfo.imageView = storage.view();
    imageInfo.imageLayout = vk::ImageLayout::eGeneral;

    vk::WriteDescriptorSet imageDescriptor{};
    imageDescriptor.dstSet = descriptorSet;
    imageDescriptor.dstBinding = 0;
    imageDescriptor.descriptorCount = 1;
    imageDescriptor.descriptorType = vk::DescriptorType::eStorageImage;
    imageDescriptor.pImageInfo = &imageInfo;

    vk::DescriptorBufferInfo cameraBufferInfo{cameraBuffer, 0, sizeof(CameraProperties)};

    vk::WriteDescriptorSet cameraDescriptor{};
    cameraDescriptor.dstSet = descriptorSet;
    cameraDescriptor.dstBinding = 1;
    cameraDescriptor.descriptorCount = 1;
    cameraDescriptor.descriptorType = vk::DescriptorType::eUniformBuffer;
    cameraDescriptor.pBufferInfo = &cameraBufferInfo;

    const std::array<vk::WriteDescriptorSet, 2> writes = {imageDescriptor, cameraDescriptor};
    device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    auto sequence = mgr->DrawFrameSequence();
    uint32_t framesRendered = 0;
    bool dumpDone = false;
    bool verifyDone = false;
    int verifyStage = 0;
    std::vector<uint8_t> litFrame;
    bool shadowsEnabled = true;
    float changedFraction = 0.0f;
    float meanShadowDepth = 0.0f;

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
        std::memcpy(mapped, &cameraProperties, sizeof(cameraProperties));

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

        ImageReadback readback(mgr->getLogicalDevice(), memProps, mgr->getMemoryAllocator(),
                               mgr->getGraphicsQueue(), queueFamily);

        if (options.verify && !verifyDone) {
            const ImageReadback::Capture capture =
                readback.capture(storage.image(), swapFormat, frameExtent, vk::ImageLayout::eGeneral);
            const bool bgra = IsBgraFormat(swapFormat);
            const uint32_t channel[3] = {bgra ? 2u : 0u, 1u, bgra ? 0u : 2u};
            const uint32_t width = frameExtent.width;
            const uint32_t height = frameExtent.height;

            if (verifyStage == 0) {
                litFrame = capture.pixels;
                shadowsEnabled = false;
                verifyStage = 1;
            } else {
                double sumAll = 0.0;
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
                        sumAll += delta / 3.0;
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
                const float meanDifference = static_cast<float>(sumAll / pixelCount);
                changedFraction = static_cast<float>(changed / pixelCount);
                meanShadowDepth = changed == 0 ? 0.0f : static_cast<float>(sumChanged / static_cast<double>(changed));
                const float darkerShare = changed == 0 ? 0.0f
                                                       : static_cast<float>(darker) / static_cast<float>(changed);
                std::cout << "[verify] shadows-on vs off: meanAll=" << meanDifference
                          << " changedFraction=" << changedFraction << " meanShadowDepth=" << meanShadowDepth
                          << " darkerShare=" << darkerShare << "\n";
                verifyDone = true;

                if (changedFraction < 0.01f) {
                    throw std::runtime_error("compute_raytracing: the shadow march changed too few pixels");
                }
                if (meanShadowDepth < 20.0f) {
                    throw std::runtime_error("compute_raytracing: the shadowed pixels are barely darker");
                }
                if (darkerShare < 0.95f) {
                    throw std::runtime_error("compute_raytracing: shadow rays brightened part of the frame");
                }
            }
        }

        const bool verifyBlocksDump = options.verify && !verifyDone;
        if (options.dump && !dumpDone && !verifyBlocksDump && framesRendered >= options.dumpAfter) {
            DumpRequest request;
            request.label = "compute-raytracing";
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
        std::cout << "[stats] frames=" << framesRendered << " dispatch=" << groupCountX << "x" << groupCountY
                  << " changedFraction=" << changedFraction << " meanShadowDepth=" << meanShadowDepth << "\n";
    }

    device.waitIdle();
    device.unmapMemory(cameraMemory);
    device.destroyBuffer(cameraBuffer);
    device.freeMemory(cameraMemory);
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
        std::cerr << "compute_raytracing: " << error.what() << "\n";
        return 1;
    }
}
