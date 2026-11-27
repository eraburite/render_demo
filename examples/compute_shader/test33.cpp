// compute_shader: the smallest useful compute-shader example, migrated from
// LearnOpenGL/src/8.guest/2022/5.computeshader_helloworld.
//
// The reference dispatches a 10x10 workgroup grid over a 1000x1000 RGBA32F
// texture, writes one texel per invocation with imageStore(), inserts a
// GL_SHADER_IMAGE_ACCESS_BARRIER_BIT and then draws the texture on a fullscreen
// quad.  The Vulkan mapping is:
//
//   * the texture becomes a storage image (STORAGE usage, always GENERAL,
//     TextureImageSpec::storageImage) so one image is both the compute output
//     and the sampled input of the screen pass;
//   * the GL image barrier becomes Dispatch -> Fragment memory dependency
//     (LoopSequence::recordComputeBarrier) -- the layout never changes, so no
//     image barrier is involved at all;
//   * the dispatch size is derived from --size instead of being two literals.
//
// The output is checked against a CPU reference of the same expression, so a
// wrong invocation index, a swapped axis or a dispatch that covers only part of
// the image shows up as an error instead of a plausible-looking gradient.
//
// Command line:
//   --size <n>          texture edge length (default 1000, as in the reference)
//   --time <seconds>    animation time baked into the output (default 25)
//   --animate           use wall-clock time instead of the fixed --time
//   --speed <texels/s>  horizontal scroll speed (default 100)
//   --dump              capture the frame (and the storage image) once, then exit
//   --dump-after <n>    capture on frame n (default 2)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ImageDump.h"

#include "AttachmentDetailConfig.h"
#include "CmdOpComputeBarrier.h"
#include "CmdOpComputeDispatch.h"
#include "CmdOpDrawFrame.h"
#include "CmdOpGroup.h"
#include "CmdSyncDevice.h"
#include "ComputePipeline.h"
#include "DescriptorLayoutConfig.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "TextureImage.h"
#include "TextureSamplerCombine.h"
#include "VertexInputStateConfig.h"

#include "shader/compute_comp.hpp"
#include "shader/screen_frag.hpp"
#include "shader/screen_vert.hpp"

namespace {

// Must match layout(local_size_*) in compute.comp.
constexpr uint32_t kLocalSize = 10;

// std140 block of compute.comp (two floats padded to 16 bytes).
struct ParamsBlock {
    float t = 0.0f;
    float speed = 100.0f;
    float padding[2] = {0.0f, 0.0f};
};
static_assert(sizeof(ParamsBlock) == 16, "ParamsBlock must match the std140 block");

enum class ComputeImageComponent { eOutputImage, eParams };
using ComputeImageConfig = DescriptorComponentSpec<ComputeImageComponent>;
inline constexpr auto ComputeImageComponentTable = std::make_tuple(
    ComputeImageConfig{ComputeImageComponent::eOutputImage, vk::DescriptorType::eStorageImage, 1,
                       vk::ShaderStageFlagBits::eCompute},
    ComputeImageConfig{ComputeImageComponent::eParams, vk::DescriptorType::eUniformBuffer, 1,
                       vk::ShaderStageFlagBits::eCompute});

enum class ScreenComponent { eImage };
using ScreenConfig = DescriptorComponentSpec<ScreenComponent>;
inline constexpr auto ScreenComponentTable = std::make_tuple(ScreenConfig{
    ScreenComponent::eImage, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment});

struct Options {
    uint32_t size = 1000;
    float time = 25.0f;
    float speed = 100.0f;
    bool animate = false;
    bool dump = false;
    uint32_t dumpAfter = 2;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--size" && hasValue) {
            options.size = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--time" && hasValue) {
            options.time = std::stof(argv[++i]);
        } else if (arg == "--speed" && hasValue) {
            options.speed = std::stof(argv[++i]);
        } else if (arg == "--animate") {
            options.animate = true;
        } else if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--dump-after" && hasValue) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else {
            throw std::runtime_error("compute_shader: unknown argument " + arg);
        }
    }
    if (options.size == 0 || options.size % kLocalSize != 0) {
        throw std::runtime_error("compute_shader: --size must be a non-zero multiple of " +
                                 std::to_string(kLocalSize));
    }
    return options;
}

DispatchSize makeDispatch(uint32_t size)
{
    DispatchSize dispatch;
    dispatch.groupCountX = (size + kLocalSize - 1) / kLocalSize;
    dispatch.groupCountY = dispatch.groupCountX;
    dispatch.groupCountZ = 1;
    return dispatch;
}

// The shader's expression, evaluated on the host.
glm::vec2 referenceTexel(uint32_t x, uint32_t y, uint32_t size, float time, float speed)
{
    const float width = static_cast<float>(size);
    return glm::vec2(std::fmod(static_cast<float>(x) + time * speed, width) / width,
                     static_cast<float>(y) / width);
}

std::shared_ptr<vk::Sampler> makeClampSampler(const std::shared_ptr<vk::Device> &device)
{
    vk::SamplerCreateInfo info{};
    info.magFilter = vk::Filter::eLinear;
    info.minFilter = vk::Filter::eLinear;
    info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    info.minLod = 0.0f;
    info.maxLod = 0.0f;
    vk::Sampler sampler{};
    if (device->createSampler(&info, nullptr, &sampler) != vk::Result::eSuccess) {
        throw std::runtime_error("compute_shader: failed to create the screen sampler");
    }
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *pointer) {
        if (pointer) {
            device->destroySampler(*pointer, nullptr);
        }
        delete pointer;
    });
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);
    const uint32_t size = options.size;
    const DispatchSize dispatch = makeDispatch(size);

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);


    // ---- descriptors -------------------------------------------------------
    TextureImageSpec imageSpec;
    imageSpec.width = size;
    imageSpec.height = size;
    imageSpec.channels = 4;
    imageSpec.format = vk::Format::eR32G32B32A32Sfloat;
    imageSpec.storageImage = true;
    imageSpec.anisotropy = false;

    auto computeLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto computeSet = DescriptorLayoutAtlas::makeSet<ComputeImageComponent, ComputeImageComponentTable>(
        0, {ComputeImageComponent::eOutputImage, ComputeImageComponent::eParams}, UpdateFrequency::eEachFrame,
        false);
    computeSet.specs[0] = imageSpec;
    computeSet.specs[1] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ParamsBlock)), DataTypes::eCustom};
    computeLayout->setDetails({computeSet});

    // The screen pass samples the very same image through a second, ordinary
    // sampler descriptor; only the descriptor layout differs.
    auto screenLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto screenSet = DescriptorLayoutAtlas::makeSet<ScreenComponent, ScreenComponentTable>(
        0, {ScreenComponent::eImage}, UpdateFrequency::eEachFrame, false);
    screenLayout->setDetails({screenSet});

    // ---- assets ------------------------------------------------------------
    auto computeAsset = mgr->CreateShaderAssetGroup(computeLayout);
    computeAsset->addShaderSpirvCode("comp",
                                     std::vector<uint32_t>(shader::COMPUTE_COMP_SPV.begin(),
                                                           shader::COMPUTE_COMP_SPV.end()));

    auto screenAsset = mgr->CreateShaderAssetGroup(screenLayout);
    screenAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                  shader::SCREEN_VERT_SPV.end()));
    screenAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SCREEN_FRAG_SPV.begin(),
                                                                  shader::SCREEN_FRAG_SPV.end()));

    const auto sampler = makeClampSampler(mgr->getLogicalDevice());
    std::vector<std::shared_ptr<TextureImage>> images;
    images.reserve(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        auto image = std::dynamic_pointer_cast<TextureImage>(
            computeAsset->DescriptorResource(ResourceId(0, 0), frame));
        if (!image) {
            throw std::runtime_error("compute_shader: the storage image resource was not created");
        }
        images.push_back(image);
        auto combine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(image->imageView(), *sampler, vk::ImageLayout::eGeneral),
            [](TextureSamplerCombine *pointer) { delete pointer; });
        screenAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(0, 0), frame), combine);
    }

    // ---- pipelines ---------------------------------------------------------
    auto computePipeline = mgr->CreateComputePipeline(computeLayout, computeAsset, "comp");

    auto swapPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));

    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    GraphicsPipelineConfig screenConfig;
    screenConfig.depthTestEnable = false;
    screenConfig.depthWriteEnable = false;
    auto screenPipeline = mgr->CreatePipeline(emptyVertexInput, screenLayout, screenAsset, swapPass, 0, screenConfig);

    // ---- first upload: zero-fill so every per-frame image leaves UNDEFINED --
    // The transition to GENERAL happens inside the upload, which is what makes
    // the storage-image descriptor legal before the first dispatch.
    {
        const std::vector<float> zeros(static_cast<size_t>(size) * size * 4u, 0.0f);
        auto upload = mgr->UploadSequence();
        auto sync = std::make_shared<CmdSyncDevice>(computeAsset);
        for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            sync->OnUpdateRawData(ResourceId(0, 0), frame, zeros);
        }
        upload->submit(sync, computeAsset);
    }

    // ---- verify one dispatch against the CPU reference ---------------------
    {
        const ParamsBlock params{options.time, options.speed, {0.0f, 0.0f}};
        // The UBO lives in device-local memory, so the host write only reaches a
        // staging buffer; CmdSyncDevice records the copy.  Without it the shader
        // reads an all-zero parameter block and time has no effect at all.
        auto paramsSync = std::make_shared<CmdSyncDevice>(computeAsset);
        paramsSync->OnUpdateRawData(ResourceId(0, 1), 0u, params);

        auto chain = std::make_shared<CmdOpGroup>();
        chain->add(paramsSync);
        chain->add(std::make_shared<CmdOpComputeDispatch>(computePipeline, dispatch));
        auto once = mgr->UploadSequence();
        once->submit(chain, computeAsset);

        auto readback = ImageReadback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                      mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                      *mgr->getQueueFamilyIndices().graphicsFamily);
        const ImageReadback::Capture capture =
            readback.capture(images[0]->imageHandle(), imageSpec.format, vk::Extent2D(size, size),
                             vk::ImageLayout::eGeneral);
        if (capture.pixels.size() < static_cast<size_t>(size) * size * 4u * sizeof(float)) {
            throw std::runtime_error("compute_shader: the storage image readback was short");
        }

        const auto *texels = reinterpret_cast<const float *>(capture.pixels.data());
        const auto texelAt = [&](uint32_t x, uint32_t y, uint32_t channel) {
            return texels[(static_cast<size_t>(y) * size + x) * 4u + channel];
        };
        float maxError = 0.0f;
        // Corners, the middle and a couple of arbitrary texels are enough to
        // catch an off-by-one in the invocation index or a transposed axis.
        const std::vector<std::pair<uint32_t, uint32_t>> probes = {
            {0u, 0u}, {size - 1u, 0u}, {0u, size - 1u}, {size - 1u, size - 1u},
            {size / 2u, size / 2u}, {size / 4u, 3u * size / 4u}};
        for (const auto &[x, y] : probes) {
            const glm::vec2 expected = referenceTexel(x, y, size, options.time, options.speed);
            for (uint32_t channel = 0; channel < 2; ++channel) {
                const float measured = texelAt(x, y, channel);
                const float wanted = channel == 0 ? expected.x : expected.y;
                maxError = std::max(maxError, std::fabs(measured - wanted));
            }
            // Blue is never written (stays 0) and alpha is always 1.
            maxError = std::max(maxError, std::fabs(texelAt(x, y, 2)));
            maxError = std::max(maxError, std::fabs(texelAt(x, y, 3) - 1.0f));
        }
        std::cout << "[compute_shader] reference check: max |gpu-cpu| = " << maxError << " over "
                  << probes.size() << " probe texels, t = " << options.time << "\n";
        // 1/1000 of a texel step is far below anything a wrong index produces.
        if (maxError > 1e-4f) {
            throw std::runtime_error("compute_shader: the compute output does not match the CPU reference");
        }
    }

    // ---- frame loop --------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto dispatchOp = std::make_shared<CmdOpComputeDispatch>(computePipeline, dispatch);
    auto drawOp = std::make_shared<CmdOpDrawFrame>(screenPipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(computeAsset);

    uint32_t framesRendered = 0;
    bool dumpDone = false;
    float elapsed = 0.0f;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        elapsed = currentTime;

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();

        const float shaderTime = options.animate ? elapsed : options.time;
        dispatchOp->OnUpdateRawData(ResourceId(0, 1), frame,
                                    ParamsBlock{shaderTime, options.speed, {0.0f, 0.0f}});

        sequence->beginComputeRecording();
        // Upload this frame's UBO on the compute submission: the dispatch is
        // submitted before the graphics one, so waiting for the graphics-side
        // sync would make the shader read last frame's parameters.
        sequence->recordDispatch(frameSync);
        sequence->recordDispatch(dispatchOp);
        // Publishes the imageStore writes to the fragment stage that samples the
        // image.  No layout transition: the image lives in GENERAL.
        sequence->recordComputeBarrier();
        sequence->endComputeRecording(true);

        sequence->beginRecording();
        sequence->recordPass(swapPass, [&]() { sequence->record(drawOp); });
        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
            auto readback = ImageReadback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                          mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                          *mgr->getQueueFamilyIndices().graphicsFamily);

            DumpRequest frameRequest;
            frameRequest.label = "frame";
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;

            DumpRequest imageRequest;
            imageRequest.label = "storage-image";
            imageRequest.image = images[frame]->imageHandle();
            imageRequest.format = imageSpec.format;
            imageRequest.extent = vk::Extent2D(size, size);
            imageRequest.layout = vk::ImageLayout::eGeneral;
            imageRequest.encoding = DumpEncoding::eColor;

            const auto results = DumpImages(readback, {frameRequest, imageRequest}, "dump", 0);
            for (const DumpResult &result : results) {
                std::cout << "[dump] " << result.path.string() << " mean=(" << result.meanColor.r << ","
                          << result.meanColor.g << "," << result.meanColor.b << ")\n";
            }
            dumpDone = true;
            (void)sequence->presentFrame();
            break;
        }

        if (!sequence->presentFrame()) {
            continue;
        }
    }
}

int main(int argc, char **argv)
{
    try {
        test(argc, argv);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
