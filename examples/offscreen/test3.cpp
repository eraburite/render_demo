// offscreen: render a phong-shaded cube into an offscreen target, then blit it
// to the swapchain with a fullscreen triangle.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "Geometry.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "TextureSamplerCombine.h"

#include "shader/phong_frag.hpp"
#include "shader/phong_vert.hpp"
#include "shader/quad_frag.hpp"
#include "shader/quad_vert.hpp"

namespace {

// IAttachmentDetail wrapper that pins the offscreen attachment to 512x512
// instead of the swapchain extent supplied by RenderManager::CreateRenderPass.
class FixedOffscreenDetail final : public IAttachmentDetail {
public:
    FixedOffscreenDetail() :
        m_inner(SimpleAttachmentDetail::Mode::eOffscreen, vk::Format::eR8G8B8A8Unorm,
                vk::Format::eD24UnormS8Uint, vk::ImageLayout::eShaderReadOnlyOptimal,
                DependencyPreset::eOffscreen)
    {
    }
    bool isSwapChainSized() const override { return false; }
    RenderPassBlueprint build(uint32_t, uint32_t, vk::SampleCountFlagBits msaa) const override
    {
        return m_inner.build(512, 512, msaa);
    }

private:
    SimpleAttachmentDetail m_inner;
};

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
        throw std::runtime_error("failed to create offscreen sampler");
    }
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *p) { if (p) { device->destroySampler(*p); } delete p; });
}

// The combined sampler owns the VkSampler via its deleter capture, so the
// sampler stays alive exactly as long as the resource using it.
std::shared_ptr<ShaderAssetGroup> makeCombinedAsset(
    const std::shared_ptr<RenderManager> &mgr, std::shared_ptr<const IDescriptorSetLayoutGroup> layout,
    const std::vector<vk::ImageView> &offscreenViews, uint32_t colorCountPerSet)
{
    if (offscreenViews.empty() || colorCountPerSet == 0) {
        throw std::runtime_error("offscreen render pass exposes no color views");
    }
    const uint32_t setCount = static_cast<uint32_t>(offscreenViews.size()) / colorCountPerSet;
    if (setCount == 0) {
        throw std::runtime_error("offscreen render pass exposes no complete color view set");
    }
    auto asset = mgr->CreateShaderAssetGroup(std::move(layout));
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::QUAD_VERT_SPV.begin(),
                                                            shader::QUAD_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::QUAD_FRAG_SPV.begin(),
                                                            shader::QUAD_FRAG_SPV.end()));

    auto sampler = makeSampler(mgr->getLogicalDevice());
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        // Views are frame-major: slot `frame` renders into and samples its own
        // offscreen image set, so no two in-flight frames share a target.
        const uint32_t slot = frame % setCount;
        const vk::ImageView view = offscreenViews[slot * colorCountPerSet];
        auto combineDeleter = [sampler](TextureSamplerCombine *p) { delete p; };
        auto combine = std::shared_ptr<TextureSamplerCombine>(
            new TextureSamplerCombine(view, *sampler, vk::ImageLayout::eShaderReadOnlyOptimal), combineDeleter);
        asset->addDescriptorResource(ResourceKey::make_key(ResourceId(0, 0), frame), combine);
    }
    return asset;
}

} // namespace

void test()
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);
    const vk::Extent2D displayExtent = *mgr->getSwapChain()->getExtent2D();
    auto vertices = CubeVertices(glm::vec3(1.0f));

    // ---- pass A: phong layout (empty set 0 keeps shader set IDs aligned) ---
    auto phongLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto emptySet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {}, UpdateFrequency::eStatic, true);
    auto environmentSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    environmentSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};
    auto frameSet = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {FrameComponent::eViewEyePosition, FrameComponent::eLightSource}, UpdateFrequency::eEachFrame, false);
    frameSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ViewEyePoint)), DataTypes::eCustom};
    frameSet.specs[1] = GBufferSpec{1, static_cast<uint32_t>(sizeof(LightSource)), DataTypes::eCustom};
    phongLayout->setDetails({emptySet, environmentSet, frameSet});
    phongLayout->setPushConstant(static_cast<uint32_t>(sizeof(ObjectParams)), vk::ShaderStageFlagBits::eVertex);

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor, VertexComponent::eUV,
                                     VertexComponent::eNormal},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u);
    auto phongAsset = mgr->CreateShaderAssetGroup(vertexInput, phongLayout);
    phongAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::PHONG_VERT_SPV.begin(),
                                                                 shader::PHONG_VERT_SPV.end()));
    phongAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::PHONG_FRAG_SPV.begin(),
                                                                 shader::PHONG_FRAG_SPV.end()));

    // ---- pass B: quad layout ---------------------------------------------
    auto quadLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto offscreenSet = DescriptorLayoutAtlas::makeSet<InputAttachmentComponent, InputAttachmentComponentTable>(
        0, {InputAttachmentComponent::ebaseColor}, UpdateFrequency::eEachFrame, false);
    quadLayout->setDetails({offscreenSet});

    // ---- render passes ---------------------------------------------------
    auto passA = mgr->CreateRenderPass(std::make_shared<FixedOffscreenDetail>());
    passA->createFramebuffers({}, passA->extent());
    auto passB = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));

    auto quadAsset = makeCombinedAsset(mgr, quadLayout, passA->offscreenViews(),
                                       passA->offscreenColorCountPerSet());

    auto phongPipeline = mgr->CreatePipeline(vertexInput, phongLayout, phongAsset, passA, 0);
    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    auto quadPipeline = mgr->CreatePipeline(emptyVertexInput, quadLayout, quadAsset, passB, 0);

    // ---- upload static vertex data ---------------------------------------
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(phongAsset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    upload->submit(syncOp, phongAsset);

    // ---- frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto phongDraw = std::make_shared<CmdOpDrawFrame>(phongPipeline);
    auto quadDraw = std::make_shared<CmdOpDrawFrame>(quadPipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(phongAsset);

    while (!window.shouldClose()) {
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        auto cameraMatrix = MakeCameraMatrix(*camera, static_cast<float>(displayExtent.width),
                                             static_cast<float>(displayExtent.height));
        ViewEyePoint eye{};
        eye.position = MakeEyePoint(*camera);
        LightSource light{};
        light.position = glm::vec4(2.0f, 3.0f, 2.0f, 1.0f);
        light.ambient = glm::vec4(0.12f, 0.12f, 0.14f, 1.0f);
        light.diffuse = glm::vec4(0.7f, 0.65f, 0.6f, 1.0f);
        light.specular = glm::vec4(0.9f, 0.9f, 0.85f, 1.0f);
        auto model = MakeSpinningModel(currentTime);

        phongDraw->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);
        phongDraw->OnUpdateRawData(ResourceId(2, 0), frame, eye);
        phongDraw->OnUpdateRawData(ResourceId(2, 1), frame, light);
        phongDraw->OnUpdateRawData(ResourceTypes::ePushConst, frame, model);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(passA, [&]() { sequence->record(phongDraw); });
        sequence->recordPass(passB, [&]() { sequence->record(quadDraw); });
        sequence->endRecording();
        sequence->submitFrame();
        if (!sequence->presentFrame()) {
            continue;
        }
    }
}

int main()
{
    try {
        test();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
