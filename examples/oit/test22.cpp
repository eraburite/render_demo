// oit: weighted-blended order-independent transparency.
//
// Reference: LearnOpenGL/src/8.guest/2020/oit/weighted_blended.cpp
//
// Three quads -- one opaque (red) and two transparent (green, blue) -- are
// recorded in two subpasses of a single render pass.  The opaque subpass writes
// color + depth; the transparent subpass writes two targets with per-attachment
// blend state:
//
//   accum   dst = src + dst              (one / one)
//   reveal  dst = dst * (1 - src.rgb)    (zero / one-minus-src-color)
//
// A separate screen pass then folds
//
//   avg = accum.rgb / accum.a
//   dst = mix(opaque, avg, 1 - reveal)
//
// which is algebraically identical to the reference's two-step "composite with
// SRC_ALPHA blending, then blit" sequence (see DESIGN.md 20.3).
//
// The property worth testing is order independence: both blend operations are
// commutative, so swapping the two transparent draws must yield a *bit-identical*
// frame.  `--reversed` performs that swap so the claim can be checked instead of
// asserted.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "ImageDump.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "TextureSamplerCombine.h"

#include "shader/composite_frag.hpp"
#include "shader/object_vert.hpp"
#include "shader/screen_vert.hpp"
#include "shader/solid_frag.hpp"
#include "shader/transparent_frag.hpp"

namespace {

struct Options {
    bool dump = false;
    uint32_t dumpAfter = 0;
    uint32_t resizeAfter = 0;
    // Drop the depth test in the transparent subpass.  With the reference's quad
    // positions the green quad sits behind the opaque red one and is rejected, so
    // by default only blue-over-red is visible.  Disabling the test is what makes
    // both transparent layers contribute, which is the only state in which the
    // order-independence check observes an actual two-layer accumulation.
    bool noDepthTest = false;
    // Swap the two transparent draws.  The resulting image must be identical.
    bool reversed = false;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--dump-after" && i + 1 < argc) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--resize-after" && i + 1 < argc) {
            options.resizeAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--no-depth-test") {
            options.noDepthTest = true;
        } else if (arg == "--reversed") {
            options.reversed = true;
        } else if (arg == "--help") {
            std::cout << "usage: oit [--dump [--dump-after N]] [--resize-after N] [--no-depth-test] "
                         "[--reversed]\n";
            std::exit(0);
        }
    }
    return options;
}

// One quad in the XY plane, two triangles.  Positions already span -1..1, so the
// same buffer serves as instanced geometry for the object passes and as the
// fullscreen quad for the screen pass.
std::vector<Vertex> makeQuadVertices()
{
    const glm::vec3 position[6] = {{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
                                   {1.0f, 1.0f, 0.0f},   {-1.0f, 1.0f, 0.0f}, {-1.0f, -1.0f, 0.0f}};
    const glm::vec2 uv[6] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f},
                             {1.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}};
    std::vector<Vertex> vertices(6);
    for (size_t i = 0; i < vertices.size(); ++i) {
        vertices[i].position = position[i];
        vertices[i].texCoord = uv[i];
    }
    return vertices;
}

OitObject makeObject(const glm::vec3 &position, const glm::vec4 &color)
{
    OitObject object{};
    object.model = glm::translate(glm::mat4(1.0f), position);
    object.color = color;
    return object;
}

std::shared_ptr<vk::Sampler> makeSampler(const std::shared_ptr<vk::Device> &device)
{
    vk::SamplerCreateInfo info{};
    info.magFilter = vk::Filter::eLinear;
    info.minFilter = vk::Filter::eLinear;
    info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    info.maxLod = 0.0f;
    vk::Sampler sampler{};
    if (device->createSampler(&info, nullptr, &sampler) != vk::Result::eSuccess) {
        throw std::runtime_error("oit: failed to create the offscreen sampler");
    }
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *p) {
        if (p) {
            device->destroySampler(*p);
        }
        delete p;
    });
}

// What the frame is made of.  The background never shows (the screen quad covers
// the whole viewport), so "coverage against a background" is meaningless here;
// the useful signal is how much of the image is pure red (opaque only), purple
// (transparent average over red) or greenish (a green contribution reached the
// composite).  `hash` is a byte-exact digest used to compare two runs.
struct FrameStats {
    size_t texels = 0;
    size_t redPixels = 0;
    size_t purplePixels = 0;
    size_t greenPixels = 0;
    glm::vec3 mean{0.0f};
    uint64_t hash = 0;
};

FrameStats analyseFrame(const std::vector<uint8_t> &rgb8)
{
    FrameStats stats{};
    stats.texels = rgb8.size() / 3;
    uint64_t hash = 1469598103934665603ull;
    glm::vec3 sum{0.0f};
    for (size_t i = 0; i + 2 < rgb8.size(); i += 3) {
        const float r = rgb8[i] / 255.0f;
        const float g = rgb8[i + 1] / 255.0f;
        const float b = rgb8[i + 2] / 255.0f;
        sum += glm::vec3(r, g, b);
        if (r > 0.35f && b < 0.15f && g < 0.15f) {
            ++stats.redPixels;
        } else if (r > 0.1f && b > 0.1f && g < 0.2f) {
            ++stats.purplePixels;
        } else if (g > 0.25f) {
            ++stats.greenPixels;
        }
        for (size_t byte = 0; byte < 3; ++byte) {
            hash ^= rgb8[i + byte];
            hash *= 1099511628211ull;
        }
    }
    if (stats.texels > 0) {
        stats.mean = sum / static_cast<float>(stats.texels);
    }
    stats.hash = hash;
    return stats;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);

    // The reference camera: eye (0,0,5) looking down -Z.
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 5.0f));
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const std::vector<Vertex> quad = makeQuadVertices();

    // Placement and colors straight from the reference: red opaque at z = 1,
    // green at z = 0 (furthest), blue at z = 2 (nearest).
    const OitObject red = makeObject(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    const OitObject green = makeObject(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 1.0f, 0.0f, 0.5f));
    const OitObject blue = makeObject(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec4(0.0f, 0.0f, 1.0f, 0.5f));

    // ---- render passes ----------------------------------------------------
    auto oitPass = mgr->CreateRenderPass(std::make_shared<OitAttachmentDetail>(mgr->DefaultDepthFormat()));
    auto screenPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), vk::Format::eUndefined));

    const std::vector<vk::ImageView> &oitViews = oitPass->offscreenViews();
    const uint32_t colorTargets = oitPass->offscreenColorCountPerSet();
    if (colorTargets != 3 || oitViews.empty()) {
        throw std::runtime_error("oit: the pass must expose opaque/accum/reveal as three color targets");
    }
    const uint32_t viewSetCount = static_cast<uint32_t>(oitViews.size()) / colorTargets;
    std::cout << "[oit] two subpasses over " << colorTargets << " offscreen target(s) x " << viewSetCount
              << " frame set(s); transparent depth test "
              << (options.noDepthTest ? "disabled" : "enabled") << ", draw order "
              << (options.reversed ? "blue-then-green" : "green-then-blue") << std::endl;

    // ---- descriptor layouts ----------------------------------------------
    // The object SSBO holds exactly the quads its own pipeline draws: the opaque
    // pass sees one entry, the transparent pass two.  One shared buffer would
    // work too, but every draw would then need an index offset; a per-pipeline
    // buffer keeps that arithmetic out of the shader.
    const auto makeObjectLayout = [&](uint32_t objectCount) {
        auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
        auto objectSet = DescriptorLayoutAtlas::makeSet<OitComponent, OitComponentTable>(
            0, {OitComponent::eObjects}, UpdateFrequency::eStatic, true);
        objectSet.specs[0] =
            GBufferSpec{1, static_cast<uint32_t>(sizeof(OitObject) * objectCount), DataTypes::eCustom};
        auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
            1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
        cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};
        layout->setDetails({objectSet, cameraSet});
        return layout;
    };

    auto solidLayout = makeObjectLayout(1);
    auto transparentLayout = makeObjectLayout(2);

    // The composite set is *not* shared across frames: each in-flight frame
    // renders into its own offscreen target set (views are frame-major), so a
    // single shared descriptor set would make every frame sample frame 0's
    // accum/reveal.  `shared = false` gives each frame its own set.
    auto screenLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto compositeSet = DescriptorLayoutAtlas::makeSet<OitCompositeComponent, OitCompositeComponentTable>(
        0,
        {OitCompositeComponent::eOpaque, OitCompositeComponent::eAccum, OitCompositeComponent::eReveal},
        UpdateFrequency::eEachFrame, false);
    screenLayout->setDetails({compositeSet});

    // ---- assets -----------------------------------------------------------
    auto solidVertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition}, static_cast<uint32_t>(quad.size()), 0u,
        DataTypes::eUnsignedInt, false, 0u, 1u);
    auto solidAsset = mgr->CreateShaderAssetGroup(solidVertexInput, solidLayout);
    solidAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::OBJECT_VERT_SPV.begin(),
                                                                 shader::OBJECT_VERT_SPV.end()));
    solidAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::SOLID_FRAG_SPV.begin(),
                                                                 shader::SOLID_FRAG_SPV.end()));

    auto transparentVertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition}, static_cast<uint32_t>(quad.size()), 0u,
        DataTypes::eUnsignedInt, false, 0u, 2u);
    auto transparentAsset = mgr->CreateShaderAssetGroup(transparentVertexInput, transparentLayout);
    transparentAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::OBJECT_VERT_SPV.begin(),
                                                                       shader::OBJECT_VERT_SPV.end()));
    transparentAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::TRANSPARENT_FRAG_SPV.begin(),
                                                                       shader::TRANSPARENT_FRAG_SPV.end()));

    auto screenVertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eUV},
        static_cast<uint32_t>(quad.size()));
    auto screenAsset = mgr->CreateShaderAssetGroup(screenVertexInput, screenLayout);
    screenAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::SCREEN_VERT_SPV.begin(),
                                                                 shader::SCREEN_VERT_SPV.end()));
    screenAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::COMPOSITE_FRAG_SPV.begin(),
                                                                 shader::COMPOSITE_FRAG_SPV.end()));

    // ---- offscreen views -> screen pass textures --------------------------
    // This has to happen *before* CreatePipeline: descriptor sets are written
    // once, when the pipeline is built.  A resource added afterwards is never
    // seen by vkUpdateDescriptorSets, and the validation layer reports it as
    // "never been updated" at draw time -- which reads as a black screen rather
    // than as a missing texture.
    auto sampler = makeSampler(mgr->getLogicalDevice());
    std::vector<std::vector<std::shared_ptr<TextureSamplerCombine>>> compositeResources(
        MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const uint32_t slot = viewSetCount == 0 ? 0u : frame % viewSetCount;
        compositeResources[frame].resize(colorTargets);
        for (uint32_t target = 0; target < colorTargets; ++target) {
            // The sampler is kept alive by the deleter capture, so it lives
            // exactly as long as the resources that reference it.
            auto combine = std::shared_ptr<TextureSamplerCombine>(
                new TextureSamplerCombine(oitViews[slot * colorTargets + target], *sampler,
                                          vk::ImageLayout::eShaderReadOnlyOptimal),
                [sampler](TextureSamplerCombine *p) { delete p; });
            screenAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(0, target), frame), combine);
            compositeResources[frame][target] = std::move(combine);
        }
    }

    // ---- pipelines --------------------------------------------------------
    // Opaque: depth write on, ordinary depth test, no blending.
    GraphicsPipelineConfig solidConfig;
    solidConfig.depthCompareOp = vk::CompareOp::eLess;
    solidConfig.depthWriteEnable = true;
    solidConfig.cullMode = vk::CullModeFlagBits::eNone;
    auto solidPipeline =
        mgr->CreatePipeline(solidVertexInput, solidLayout, solidAsset, oitPass, 0, solidConfig);

    // Transparent: depth write off -- order independence requires every layer to
    // reach the blend -- and one blend state per target.
    GraphicsPipelineConfig transparentConfig;
    transparentConfig.depthCompareOp = vk::CompareOp::eLess;
    transparentConfig.depthWriteEnable = false;
    transparentConfig.depthTestEnable = !options.noDepthTest;
    transparentConfig.cullMode = vk::CullModeFlagBits::eNone;
    transparentConfig.addBlendAttachment(ColorBlendAttachment::additive());
    transparentConfig.addBlendAttachment(ColorBlendAttachment::revealage());
    auto transparentPipeline = mgr->CreatePipeline(transparentVertexInput, transparentLayout, transparentAsset,
                                                   oitPass, 1, transparentConfig);

    // Screen: fullscreen quad, no depth, no blending.  The composite happens in
    // the shader, so SRC_ALPHA blending is not needed here.
    GraphicsPipelineConfig screenConfig;
    screenConfig.depthTestEnable = false;
    screenConfig.depthWriteEnable = false;
    screenConfig.blendEnable = false;
    screenConfig.cullMode = vk::CullModeFlagBits::eNone;
    auto screenPipeline =
        mgr->CreatePipeline(screenVertexInput, screenLayout, screenAsset, screenPass, 0, screenConfig);

    // ---- uploads ----------------------------------------------------------
    auto upload = mgr->UploadSequence();
    const auto uploadObjectAsset = [&](const std::shared_ptr<ShaderAssetGroup> &asset,
                                       const std::vector<OitObject> &objects) {
        auto sync = std::make_shared<CmdSyncDevice>(asset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, quad);
        sync->OnUpdateRawData(ResourceId(0, 0), 0u, objects.data(), sizeof(OitObject) * objects.size());
        upload->submit(sync, asset);
        return sync;
    };
    const std::vector<OitObject> transparentObjects =
        options.reversed ? std::vector<OitObject>{blue, green} : std::vector<OitObject>{green, blue};
    auto solidSync = uploadObjectAsset(solidAsset, {red});
    auto transparentSync = uploadObjectAsset(transparentAsset, transparentObjects);

    auto screenSync = std::make_shared<CmdSyncDevice>(screenAsset);
    screenSync->OnUpdateRawData(ResourceTypes::eVertex, quad);
    upload->submit(screenSync, screenAsset);

    // ---- frame loop -------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto solidDraw = std::make_shared<CmdOpDrawFrame>(solidPipeline);
    auto transparentDraw = std::make_shared<CmdOpDrawFrame>(transparentPipeline);
    auto screenDraw = std::make_shared<CmdOpDrawFrame>(screenPipeline);

    bool dumpPending = false;
    bool dumpDone = false;
    uint32_t renderedFrames = 0;
    bool resizeRequested = false;
    // Framebuffer attachments are recreated on resize, which invalidates the
    // views the screen pass' texture descriptors point at.  The pass bumps its
    // generation counter when that happens.
    uint64_t attachmentGeneration = oitPass->attachmentGeneration();

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (options.resizeAfter != 0 && !resizeRequested && renderedFrames >= options.resizeAfter) {
            resizeRequested = true;
            window.resize(1024, 600);
            std::cout << "[oit] requested resize to 1024x600" << std::endl;
        }

        if (options.dump && !dumpDone && !dumpPending && renderedFrames >= options.dumpAfter) {
            dumpPending = true;
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

        if (oitPass->attachmentGeneration() != attachmentGeneration) {
            // Descriptor sets still referenced by an in-flight submission must
            // not be updated, so drain the device first (resize only).
            mgr->getLogicalDevice()->waitIdle();
            const std::vector<vk::ImageView> &views = oitPass->offscreenViews();
            const uint32_t sets = static_cast<uint32_t>(views.size()) / colorTargets;
            if (sets == 0) {
                throw std::runtime_error("oit: the rebuilt pass exposes no color view set");
            }
            for (uint32_t slotFrame = 0; slotFrame < MAX_FRAMES_IN_FLIGHT; ++slotFrame) {
                const uint32_t viewSet = slotFrame % sets;
                for (uint32_t target = 0; target < colorTargets; ++target) {
                    compositeResources[slotFrame][target]->setImageView(views[viewSet * colorTargets + target]);
                }
            }
            screenPipeline->RefreshDescriptors();
            attachmentGeneration = oitPass->attachmentGeneration();
            std::cout << "[oit] attachments rebuilt, composite descriptors refreshed" << std::endl;
        }

        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);
        solidDraw->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);
        transparentDraw->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);

        sequence->beginRecording();
        sequence->record(solidSync);
        sequence->record(transparentSync);
        sequence->record(screenSync);
        sequence->recordPass(oitPass, [&]() {
            sequence->record(solidDraw);
            sequence->nextSubpass();
            sequence->record(transparentDraw);
        });
        sequence->recordPass(screenPass, [&]() { sequence->record(screenDraw); });
        sequence->endRecording();
        sequence->submitFrame();

        if (dumpPending) {
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                   mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                   *mgr->getQueueFamilyIndices().graphicsFamily);
            const std::vector<vk::Image> &swapImages = mgr->getSwapChain()->getImages();
            const uint32_t imageIndex = sequence->getImageIndex();
            if (imageIndex >= swapImages.size()) {
                throw std::runtime_error("oit: swapchain image index out of range");
            }
            DumpRequest request;
            request.label = "oit";
            request.image = swapImages[imageIndex];
            request.format = *mgr->getSwapChain()->getImageFormat();
            request.extent = extent;
            request.layout = vk::ImageLayout::ePresentSrcKHR;
            request.encoding = DumpEncoding::eColor;
            const auto results = DumpImages(readback, {request}, "dump", 0);

            const FrameStats stats = analyseFrame(results[0].rgb8);
            std::cout << "[dump] " << results[0].path.string() << " " << stats.texels << " texels red="
                      << stats.redPixels << " purple=" << stats.purplePixels << " green="
                      << stats.greenPixels << " mean=(" << stats.mean.r << "," << stats.mean.g << ","
                      << stats.mean.b << ") hash=0x" << std::hex << stats.hash << std::dec << std::endl;

            dumpPending = false;
            dumpDone = true;
        }

        if (!sequence->presentFrame()) {
            continue;
        }
        ++renderedFrames;
        if (options.dump && dumpDone) {
            break;
        }
    }
}

int main(int argc, char **argv)
{
    try {
        test(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "[oit] fatal: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
