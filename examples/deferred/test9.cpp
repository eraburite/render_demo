// deferred: MRT G-Buffer geometry pass followed by a fullscreen-triangle
// lighting pass that accumulates six point lights (Blinn-Phong) per pixel.
// The geometry is the armor glTF model; CubeVertices() is a fallback.
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "Geometry.h"
#include "ModelLoader.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "ImageDump.h"
#include "ImageReadback.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "TextureSamplerCombine.h"
#include "root_directory.h"

#include "shader/deferred_frag.hpp"
#include "shader/deferred_vert.hpp"
#include "shader/mrt_frag.hpp"
#include "shader/mrt_vert.hpp"

namespace {

ViewEyePoint makeEye(const Camera &camera)
{
    return ViewEyePoint{MakeEyePoint(camera)};
}

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
        throw std::runtime_error("deferred: failed to create G-Buffer sampler");
    }
    return std::shared_ptr<vk::Sampler>(new vk::Sampler(sampler), [device](vk::Sampler *p) {
        if (p) {
            device->destroySampler(*p);
        }
        delete p;
    });
}

// Apply each mesh node's world transform (armor.gltf rotates/scale its meshes),
// then center and uniformly scale into a ~2-unit box and give the surface a
// steel-tinted albedo so the six coloured lights have something to play on.
std::vector<Vertex> toEngineVertices(const Model &model)
{
    const std::vector<glm::mat4> world = computeNodeWorldMatrices(model);
    std::vector<glm::mat4> transforms(model.vertices.size(), glm::mat4(1.0f));
    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
        const ModelNode &node = model.nodes[nodeIndex];
        if (node.meshIndex < 0 || node.meshIndex >= static_cast<int>(model.meshes.size())
            || nodeIndex >= world.size()) {
            continue;
        }
        for (const Primitive &prim : model.meshes[node.meshIndex].primitives) {
            for (uint32_t i = prim.firstIndex; i < prim.firstIndex + prim.indexCount && i < model.indices.size();
                 ++i) {
                const uint32_t vertexIndex = model.indices[i];
                if (vertexIndex < transforms.size()) {
                    transforms[vertexIndex] = world[nodeIndex];
                }
            }
        }
    }

    std::vector<glm::vec3> positions(model.vertices.size());
    std::vector<glm::vec3> normals(model.vertices.size());
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
    for (size_t i = 0; i < model.vertices.size(); ++i) {
        const glm::mat4 &m = transforms[i];
        positions[i] = glm::vec3(m * glm::vec4(model.vertices[i].position, 1.0f));
        normals[i] = glm::normalize(glm::mat3(m) * model.vertices[i].normal);
        minBounds = glm::min(minBounds, positions[i]);
        maxBounds = glm::max(maxBounds, positions[i]);
    }
    const glm::vec3 extent = maxBounds - minBounds;
    const float maxExtent = std::max({extent.x, extent.y, extent.z, 0.0001f});
    const float scale = 2.0f / maxExtent;
    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;

    std::vector<Vertex> result;
    result.reserve(model.vertices.size());
    for (size_t i = 0; i < model.vertices.size(); ++i) {
        Vertex dst;
        dst.position = (positions[i] - center) * scale;
        dst.normal = normals[i];
        dst.texCoord = model.vertices[i].texCoord;
        dst.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
        dst.color = glm::vec3(0.80f, 0.82f, 0.88f);
        result.push_back(dst);
    }
    return result;
}

// Fallback: three side-by-side cubes when the armor model cannot be loaded.
std::vector<glm::mat4> makeCubeModels(uint32_t count)
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
    (void)argc;
    (void)argv;

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.5f));
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ---- geometry: prefer armor, fall back to cubes ------------------------
    std::vector<Vertex> vertices;
    std::vector<glm::mat4> models;

    try {
        Model gltf = loadModel(PROJECT_ROOT_PATH "/assets/models/armor/armor.gltf");
        if (gltf.valid()) {
            vertices = toEngineVertices(gltf);
            // The framework's index-buffer binding infers the index type from the
            // resource spec, which CreatePipeline hard-codes as eCustom (unsupported
            // there).  Expand the index list into a plain triangle list so the
            // geometry goes through the well-tested non-indexed draw path.
            std::vector<Vertex> expanded;
            expanded.reserve(gltf.indices.size());
            for (uint32_t idx : gltf.indices) {
                if (idx < vertices.size()) {
                    expanded.push_back(vertices[idx]);
                }
            }
            vertices = std::move(expanded);
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::rotate(model, glm::radians(25.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            models.push_back(model);
            std::cout << "[deferred] armor: expanded to " << vertices.size() << " vertices\n";
        }
    } catch (const std::exception &e) {
        std::cout << "[deferred] armor load failed (" << e.what() << "); using cubes\n";
    }
    if (vertices.empty()) {
        vertices = CubeVertices(glm::vec3(0.80f, 0.82f, 0.88f));
        models = makeCubeModels(3);
        std::cout << "[deferred] cube fallback: " << vertices.size() << " vertices\n";
    }

    // ---- MRT layout: empty set0/set2, camera at set1, instances at set3 ----
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
    instanceSet.specs[0] = GBufferSpec{static_cast<uint32_t>(models.size()),
                                       static_cast<uint32_t>(sizeof(glm::mat4)), DataTypes::eCustom};
    mrtLayout->setDetails({empty0, cameraSet, empty2, instanceSet});

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

    // ---- deferred layout: eye at set2, G-Buffer samplers at set4 -----------
    auto deferredLayout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto dEmpty1 = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {}, UpdateFrequency::eEachFrame, false);
    auto dEmpty3 = DescriptorLayoutAtlas::makeSet<InstanceComponent, InstanceComponentTable>(
        3, {}, UpdateFrequency::eEachInstance, false);
    auto eyeSet = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {FrameComponent::eViewEyePosition}, UpdateFrequency::eEachFrame, false);
    eyeSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ViewEyePoint)), DataTypes::eCustom};
    auto inputSet = DescriptorLayoutAtlas::makeSet<InputAttachmentComponent, InputAttachmentComponentTable>(
        4,
        {InputAttachmentComponent::ePosition, InputAttachmentComponent::eNormalMap,
         InputAttachmentComponent::eAlbedo},
        UpdateFrequency::eEachFrame, false);
    deferredLayout->setDetails({empty0, dEmpty1, eyeSet, dEmpty3, inputSet});

    auto deferredAsset = mgr->CreateShaderAssetGroup(deferredLayout);
    deferredAsset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::DEFERRED_VERT_SPV.begin(),
                                                                    shader::DEFERRED_VERT_SPV.end()));
    deferredAsset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::DEFERRED_FRAG_SPV.begin(),
                                                                    shader::DEFERRED_FRAG_SPV.end()));

    // ---- render passes ----------------------------------------------------
    const vk::Extent2D swapExtent = *mgr->getSwapChain()->getExtent2D();
    auto gbufferPass = mgr->CreateRenderPass(std::make_shared<GBufferAttachmentDetail>(
        *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat(), swapExtent.width, swapExtent.height));
    gbufferPass->createFramebuffers({}, gbufferPass->extent());
    auto compositePass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));

    // ---- bind G-Buffer color views as sampled images -----------------------
    const auto &offscreenViews = gbufferPass->offscreenViews();
    const uint32_t colorCount = gbufferPass->offscreenColorCountPerSet();
    if (colorCount != 3 || offscreenViews.size() < colorCount) {
        throw std::runtime_error("deferred: G-Buffer pass did not expose three color views");
    }
    const uint32_t setCount = offscreenViews.size() / colorCount;
    auto sampler = makeSampler(mgr->getLogicalDevice());
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const uint32_t set = frame % setCount;
        for (uint32_t slot = 0; slot < colorCount; ++slot) {
            const vk::ImageView view = offscreenViews[set * colorCount + slot];
            auto combine = std::shared_ptr<TextureSamplerCombine>(
                new TextureSamplerCombine(view, *sampler, vk::ImageLayout::eShaderReadOnlyOptimal),
                [sampler](TextureSamplerCombine *p) { delete p; });
            deferredAsset->addDescriptorResource(ResourceKey::make_key(ResourceId(4, slot), frame), combine);
        }
    }

    // ---- pipelines --------------------------------------------------------
    auto gbufferPipeline = mgr->CreatePipeline(vertexInput, mrtLayout, mrtAsset, gbufferPass, 0);
    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, 0u);
    auto compositePipeline = mgr->CreatePipeline(emptyVertexInput, deferredLayout, deferredAsset, compositePass, 0);

    // ---- upload static data -----------------------------------------------
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(mrtAsset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        syncOp->OnUpdateRawData(ResourceId(3, 0), frame, models);
    }
    upload->submit(syncOp, mrtAsset);

    // ---- frame loop -------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto gbufferDraw = std::make_shared<CmdOpDrawFrame>(gbufferPipeline);
    auto compositeDraw = std::make_shared<CmdOpDrawFrame>(compositePipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(mrtAsset);
    auto compositeFrameSync = std::make_shared<CmdSyncDevice>(deferredAsset);

    std::cout << "[deferred] MRT G-Buffer + 6 point lights; close the window to exit\n";

    // Optional `--dump` captures the G-Buffer channels of the first frame into
    // dump/*.ppm, which is handy for verifying geometry without a visible window.
    bool dumpOnce = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--dump") {
            dumpOnce = true;
        }
    }
    ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
                           mgr->getGraphicsQueue(), *mgr->getQueueFamilyIndices().graphicsFamily);
    uint32_t framesRendered = 0;

    while (!window.shouldClose()) {
        // Poll so resize/close events actually reach the window state.
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);

        gbufferDraw->OnUpdateRawData(ResourceId(1, 0), frame, cameraMatrix);
        compositeDraw->OnUpdateRawData(ResourceId(2, 0), frame, makeEye(*camera));

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->record(compositeFrameSync);
        sequence->recordPass(gbufferPass, [&]() { sequence->record(gbufferDraw); });
        sequence->recordPass(compositePass, [&]() { sequence->record(compositeDraw); });
        sequence->endRecording();
        sequence->submitFrame();

        if (dumpOnce && framesRendered == 0) {
            const auto &images = gbufferPass->offscreenImages();
            const uint32_t set = frame % setCount;
            const vk::Extent2D gbufferExtent = gbufferPass->extent();
            const auto request = [&](std::string label, uint32_t slot, DumpEncoding encoding) {
                DumpRequest r;
                r.label = std::move(label);
                r.image = images[set * colorCount + slot];
                r.format = gbufferPass->offscreenColorFormat(slot);
                r.extent = gbufferExtent;
                r.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
                r.encoding = encoding;
                r.referenceScale = 5.0f;
                r.eyePosition = camera->Position;
                r.nearPlane = 0.1f;
                r.farPlane = 100.0f;
                return r;
            };
            std::vector<DumpRequest> requests;
            requests.push_back(request("deferred-albedo", 2, DumpEncoding::eColor));
            requests.push_back(request("deferred-normal", 1, DumpEncoding::eNormal));
            requests.push_back(request("deferred-position", 0, DumpEncoding::ePosition));
            const auto results = DumpImages(readback, requests, "dump", 0);
            for (const auto &r : results) {
                std::cout << "[dump] " << r.path.string() << " coverage=" << r.alphaCoverage
                          << " mean=(" << r.meanColor.r << "," << r.meanColor.g << "," << r.meanColor.b << ")"
                          << " |n|=" << r.meanNormalLength << "\n";
            }
        }
        if (!sequence->presentFrame()) {
            continue;
        }
        ++framesRendered;
        if (dumpOnce && framesRendered >= 2) {
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
