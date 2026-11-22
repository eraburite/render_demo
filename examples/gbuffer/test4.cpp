// gbuffer: MRT geometry pass into fixed-size G-Buffer attachments, followed by
// a deferred fullscreen-triangle lighting pass into the swapchain.
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "Geometry.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DebugViewMode.h"
#include "DescriptorLayoutConfig.h"
#include "ImageDump.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "ImageReadback.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "TextureSamplerCombine.h"

#include "shader/deferred_frag.hpp"
#include "shader/deferred_vert.hpp"
#include "shader/mrt_frag.hpp"
#include "shader/mrt_vert.hpp"

namespace {

ViewEyePoint makeEye(const Camera &camera)
{
    return ViewEyePoint{MakeEyePoint(camera)};
}

struct SamplerOwner {
    std::shared_ptr<vk::Device> device;
    void operator()(vk::Sampler sampler) const { device->destroySampler(sampler, nullptr); }
};

std::shared_ptr<vk::Sampler> makeSampler(const std::shared_ptr<vk::Device> &device)
{
    vk::SamplerCreateInfo info{};
    info.magFilter = vk::Filter::eLinear;
    info.minFilter = vk::Filter::eLinear;
    info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    info.minLod = 0.0f;
    info.maxLod = 1.0f;

    vk::Sampler sampler{};
    if (device->createSampler(&info, nullptr, &sampler) != vk::Result::eSuccess) {
        throw std::runtime_error("failed to create gbuffer sampler");
    }
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *p) { if (p) { device->destroySampler(*p); } delete p; });
}

// Three cubes side by side.  Spacing and camera distance are chosen together so
// the rotated cubes (bounding radius ~0.87) stay inside the 45-degree frustum:
// half-width at z = 0 is 4.5 * tan(22.5) * 4/3 = 2.49 > 1.4 + 0.87.
std::vector<glm::mat4> makeModels(uint32_t count)
{
    std::vector<glm::mat4> models;
    models.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, glm::vec3(static_cast<float>(i) * 1.4f - 1.4f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(20.0f * static_cast<float>(i)), glm::vec3(1.0f, 0.3f, 0.5f));
        models.push_back(model);
    }
    return models;
}

} // namespace

void test(int argc, char **argv)
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 4.5f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);
    auto vertices = CubeVertices(glm::vec3(1.0f));
    const auto models = makeModels(3);

    // ---- MRT layout: empty placeholders keep set IDs 1 and 3 in place -----
    auto mrtLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto empty0 = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {}, UpdateFrequency::eStatic, true);
    auto empty2 = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {}, UpdateFrequency::eEachFrame, false);
    auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};
    auto instanceSet = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        3, {InstanceComponent::eModelMatrix}, UpdateFrequency::eEachInstance, false);
    instanceSet.specs[0] = GBufferSpec{static_cast<uint32_t>(models.size()), static_cast<uint32_t>(sizeof(glm::mat4)), DataTypes::eCustom};
    mrtLayout->setDetails({empty0, cameraSet, empty2, instanceSet});
    mrtLayout->setPushConstant(static_cast<uint32_t>(sizeof(ObjectParams)), vk::ShaderStageFlagBits::eVertex);

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor, VertexComponent::eUV,
                                     VertexComponent::eNormal},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u,
        static_cast<uint32_t>(models.size()));
    auto mrtAsset = mgr->CreateShaderAssetGroup(vertexInput, mrtLayout);
    mrtAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::MRT_VERT_SPV.begin(),
                                                               shader::MRT_VERT_SPV.end()));
    mrtAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::MRT_FRAG_SPV.begin(),
                                                               shader::MRT_FRAG_SPV.end()));

    // ---- deferred layout: set2 frame data + set4 gbuffer samplers ---------
    auto deferredLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto empty1 = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {}, UpdateFrequency::eEachFrame, false);
    auto empty3 = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        3, {}, UpdateFrequency::eEachInstance, false);
    auto frameSet = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {FrameComponent::eViewEyePosition, FrameComponent::eLightSource}, UpdateFrequency::eEachFrame, false);
    frameSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ViewEyePoint)), DataTypes::eCustom};
    frameSet.specs[1] = GBufferSpec{1, static_cast<uint32_t>(sizeof(LightSource)), DataTypes::eCustom};
    auto inputSet = DescriptorLayoutAtlas::makeSet<InputAttachmentComponent, InputAttachmentComponentTable>(
        4,
        {InputAttachmentComponent::ePosition, InputAttachmentComponent::eNormalMap,
         InputAttachmentComponent::eAlbedo},
        UpdateFrequency::eEachFrame, false);
    deferredLayout->setDetails({empty0, empty1, frameSet, empty3, inputSet});
    // Debug View switch lives in deferred.frag; one 32-byte push constant is all
    // it needs (see DebugViewPushConstant).
    deferredLayout->setPushConstant(static_cast<uint32_t>(sizeof(DebugViewPushConstant)),
                                    vk::ShaderStageFlagBits::eFragment);

    auto deferredAsset = mgr->CreateShaderAssetGroup(deferredLayout);
    deferredAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::DEFERRED_VERT_SPV.begin(),
                                                                    shader::DEFERRED_VERT_SPV.end()));
    deferredAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::DEFERRED_FRAG_SPV.begin(),
                                                                    shader::DEFERRED_FRAG_SPV.end()));

    // ---- render passes ---------------------------------------------------
    auto passA = mgr->CreateRenderPass(std::make_shared<GBufferAttachmentDetail>(
        *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat(), 2048, 2048));
    passA->createFramebuffers({}, passA->extent());
    auto passB = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));

    const auto &offscreenViews = passA->offscreenViews();
    const uint32_t colorCount = passA->offscreenColorCountPerSet();
    if (colorCount != 3 || offscreenViews.size() < colorCount) {
        throw std::runtime_error("GBuffer pass did not expose three color views per frame set");
    }
    const uint32_t setCount = offscreenViews.size() / colorCount;
    auto sampler = makeSampler(mgr->getLogicalDevice());
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const uint32_t set = frame % setCount;
        for (uint32_t slot = 0; slot < colorCount; ++slot) {
            // Views are laid out frame-major: [set * colorCount + slot].  Each
            // in-flight frame samples the G-Buffer set it rendered into, so the
            // deferred pass never reads an attachment another frame is writing.
            const vk::ImageView view = offscreenViews[set * colorCount + slot];
            auto combineDeleter = [sampler](TextureSamplerCombine *p) { delete p; };
            auto combine = std::shared_ptr<TextureSamplerCombine>(
                new TextureSamplerCombine(view, *sampler, vk::ImageLayout::eShaderReadOnlyOptimal), combineDeleter);
            deferredAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(4, slot), frame), combine);
        }
    }

    auto mrtPipeline = mgr->CreatePipeline(vertexInput, mrtLayout, mrtAsset, passA, 0);
    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    auto deferredPipeline = mgr->CreatePipeline(emptyVertexInput, deferredLayout, deferredAsset, passB, 0);

    // ---- upload static data ----------------------------------------------
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(mrtAsset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        syncOp->OnUpdateRawData(ResourceId(3, 0), frame, models);
    }
    upload->submit(syncOp, mrtAsset);

    // ---- frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto mrtDraw = std::make_shared<CmdOpDrawFrame>(mrtPipeline);
    auto deferredDraw = std::make_shared<CmdOpDrawFrame>(deferredPipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(mrtAsset);
    auto deferredFrameSync = std::make_shared<CmdSyncDevice>(deferredAsset);

    // Keys 0..5 select the Debug View; slot 0 (final) is the default.  The list is
    // the supported subset for the deferred path, and the key index maps onto it,
    // so the keys stay 0..5 regardless of the shared enum's ordering.
    DebugViewState debugView({DebugViewMode::eFinal, DebugViewMode::eAlbedo, DebugViewMode::eNormal,
                              DebugViewMode::ePosition, DebugViewMode::eLinearDepth,
                              DebugViewMode::eLightingOnly});

    // ---- debug image dump -------------------------------------------------
    // The Debug View answers "does it look right?" on screen; P additionally
    // freezes the current frame's G-Buffer into dump/*.ppm, which keeps the raw
    // texel values (pick colours, measure ranges, diff two runs).
    ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
                           mgr->getGraphicsQueue(), *mgr->getQueueFamilyIndices().graphicsFamily);
    const std::filesystem::path dumpDirectory = "dump";
    uint32_t nextDumpIndex = 0;
    bool dumpKeyHeld = false;

    // Colour slots follow GBufferAttachmentDetail::build(): 0 = position,
    // 1 = normal, 2 = albedo.
    const auto dumpGBuffer = [&](uint32_t frame) {
        const std::vector<vk::Image> &images = passA->offscreenImages();
        const uint32_t set = frame % setCount;
        const vk::Extent2D gbufferExtent = passA->extent();

        const auto makeRequest = [&](std::string label, uint32_t slot, DumpEncoding encoding) {
            DumpRequest request;
            request.label = std::move(label);
            request.image = images[set * colorCount + slot];
            request.format = passA->offscreenColorFormat(slot);
            request.extent = gbufferExtent;
            request.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            request.encoding = encoding;
            // Same constants as DebugViewState::pushConstant(), so the file and
            // the on-screen view encode a channel identically.
            request.referenceScale = 5.0f;
            request.eyePosition = camera->Position;
            request.nearPlane = 0.1f;
            request.farPlane = 100.0f;
            return request;
        };

        std::vector<DumpRequest> requests;
        requests.push_back(makeRequest("gbuffer-albedo", 2, DumpEncoding::eColor));
        requests.push_back(makeRequest("gbuffer-normal", 1, DumpEncoding::eNormal));
        requests.push_back(makeRequest("gbuffer-position", 0, DumpEncoding::ePosition));
        // Depth is not stored (the pass discards it), so it is reconstructed
        // from the position channel -- the same thing deferred.frag does for
        // DebugViewMode::eLinearDepth.
        requests.push_back(makeRequest("gbuffer-linear-depth", 0, DumpEncoding::eLinearDepth));

        const std::vector<DumpResult> results = DumpImages(readback, requests, dumpDirectory, nextDumpIndex);
        nextDumpIndex += static_cast<uint32_t>(requests.size());

        std::cout << std::fixed << std::setprecision(3);
        for (const DumpResult &result : results) {
            std::cout << "[dump] " << result.path.string() << "  " << result.extent.width << "x"
                      << result.extent.height << "  alpha-coverage=" << result.alphaCoverage << "  mean=("
                      << result.meanColor.r << ", " << result.meanColor.g << ", " << result.meanColor.b << ")";
            if (result.meanNormalLength > 0.0f) {
                std::cout << "  |n|=" << result.meanNormalLength;
            }
            std::cout << "\n";
        }
    };

    std::cout << "[debug] press 0..5 to inspect the G-Buffer channels, P to dump them to dump/*.ppm\n"
              << "[debug] scripted runs: --view <index> --capture <frames>\n";

    const DebugCaptureOptions captureOptions = ParseDebugCaptureOptions(argc, argv);
    if (captureOptions.viewSpecified) {
        debugView.select(captureOptions.initialViewIndex);
    }
    if (captureOptions.requested()) {
        std::cout << "[debug] auto-capture after " << captureOptions.framesBeforeCapture << " frames\n";
    }
    uint32_t framesRendered = 0;
    // A dump survives a skipped frame (resize / out-of-date swapchain), so the
    // key press is never silently lost.
    bool pendingDump = false;

    while (!window.shouldClose()) {
        // Pump the GLFW message queue first: processInputEvents() only reads the
        // cached key state, so without polling the resize callback never fires
        // (no swapchain rebuild) and the close button / ESC are ignored.
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        const auto isKeyDown = [&](int key) { return window.isKeyDown(key); };
        debugView.poll(isKeyDown, GLFW_KEY_0);
        if (debugView.changed()) {
            std::cout << "[debug] view = " << DebugViewModeName(debugView.mode()) << "\n";
        }
        const bool dumpRequested =
            debugKeyJustPressed(isKeyDown, GLFW_KEY_P, dumpKeyHeld) || captureOptions.isCaptureFrame(framesRendered);
        pendingDump = pendingDump || dumpRequested;
        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
        auto cameraMatrix = MakeCameraMatrix(*camera, extent);
        auto model = MakeSpinningModel(currentTime);
        LightSource light{};
        light.position = glm::vec4(1.5f, 2.0f, 2.0f, 1.0f);
        light.ambient = glm::vec4(0.12f, 0.12f, 0.14f, 1.0f);
        light.diffuse = glm::vec4(0.7f, 0.7f, 0.65f, 1.0f);
        light.specular = glm::vec4(0.9f, 0.9f, 0.85f, 1.0f);

        mrtDraw->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);
        // Declared by the layout but not read by mrt.vert (kept for parity).
        mrtDraw->OnUpdateRawData(ResourceTypes::ePushConst, frame, model);
        deferredDraw->OnUpdateRawData(ResourceId(2, 0), frame, makeEye(*camera));
        deferredDraw->OnUpdateRawData(ResourceId(2, 1), frame, light);
        // The debug switch is per-frame state: push constant storage is indexed by
        // frame-in-flight, so it has to be re-uploaded every frame.
        deferredDraw->OnUpdateRawData(ResourceTypes::ePushConst, frame,
                                      debugView.pushConstant(extent, vk::Extent2D{2048, 2048}));

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->record(deferredFrameSync);
        sequence->recordPass(passA, [&]() { sequence->record(mrtDraw); });
        sequence->recordPass(passB, [&]() { sequence->record(deferredDraw); });
        sequence->endRecording();
        sequence->submitFrame();
        // Dump after the submit: the capture's transfer barrier is ordered
        // behind this frame's rendering on the same queue, so the G-Buffer slot
        // it reads is the one this frame just filled.
        if (pendingDump) {
            dumpGBuffer(frame);
            pendingDump = false;
        }
        if (!sequence->presentFrame()) {
            continue;
        }
        ++framesRendered;
        if (captureOptions.shouldStop(framesRendered)) {
            break;
        }
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
