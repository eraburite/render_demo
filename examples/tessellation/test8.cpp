// tessellation: PN-triangles vs. pass-through subdivision, side by side.
//
// The deer glTF mesh is drawn twice into the same full-screen render pass.
// Both pipelines share one asset (vertex/index buffers + camera UBO) and use
// TESS_LEVEL / SIDE / TESS_ALPHA specialization constants to select the
// algorithm and the screen half.  The tessellation evaluation shaders remap
// clip-space X into [-1,0] (left, flat) or [0,1] (right, PN-smoothed), so the
// two draws do not overlap even though viewport/scissor are dynamic.
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ModelLoader.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "ComputePipeline.h" // makeSpecializationConstant
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "root_directory.h"

#include "shader/passthrough_tesc.hpp"
#include "shader/passthrough_tese.hpp"
#include "shader/pntriangles_tesc.hpp"
#include "shader/pntriangles_tese.hpp"
#include "shader/tess_frag.hpp"
#include "shader/tess_vert.hpp"

namespace {

constexpr float kTessLevel = 6.0f;
constexpr float kTessAlpha = 1.0f;

// set0 binding0: camera viewProj, read by the tessellation evaluation stages.
enum class TessComponent { eCameraMatrix };
using TessConfig = DescriptorComponentSpec<TessComponent>;
inline constexpr auto TessComponentTable = std::make_tuple(TessConfig{
    TessComponent::eCameraMatrix, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl
        | vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eFragment});

struct CameraUBO {
    alignas(16) glm::mat4 viewProj{};
};

std::shared_ptr<DescriptorLayoutAtlas> makeLayout()
{
    auto layout = std::make_shared<DescriptorLayoutAtlas>();
    auto cameraSet = DescriptorLayoutAtlas::makeSet<TessComponent, TessComponentTable>(
        0, {TessComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraUBO)), DataTypes::eCustom};
    layout->setDetails({cameraSet});
    return layout;
}

// Convert loaded glTF vertices into the engine Vertex layout and fit the
// model into a ~2 unit box centred at the origin (same as geometryshader).
std::vector<Vertex> toEngineVertices(const Model &model, float &outScale)
{
    const glm::vec3 extent = model.maxBounds - model.minBounds;
    const float maxExtent = std::max({extent.x, extent.y, extent.z, 0.0001f});
    outScale = 2.0f / maxExtent;
    const glm::vec3 center = (model.minBounds + model.maxBounds) * 0.5f;

    std::vector<Vertex> result;
    result.reserve(model.vertices.size());
    for (const MeshVertex &src : model.vertices) {
        Vertex dst;
        dst.position = (src.position - center) * outScale;
        dst.normal = src.normal;
        dst.texCoord = src.texCoord;
        dst.color = src.color;
        dst.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
        result.push_back(dst);
    }
    return result;
}

GraphicsPipelineConfig makeTessConfig(const char *tescKey, const char *teseKey, uint32_t side)
{
    GraphicsPipelineConfig cfg;
    cfg.addStage("vert", vk::ShaderStageFlagBits::eVertex)
        .addStage(tescKey, vk::ShaderStageFlagBits::eTessellationControl)
        .addStage(teseKey, vk::ShaderStageFlagBits::eTessellationEvaluation)
        .addStage("frag", vk::ShaderStageFlagBits::eFragment);
    cfg.topology = vk::PrimitiveTopology::ePatchList;
    cfg.patchControlPoints = 3;
    cfg.cullMode = vk::CullModeFlagBits::eNone;

    // constant_id 1 = tessLevel (both tesc shaders); id 0 = side and
    // id 2 = tessAlpha (tese shaders only).
    cfg.addSpecialization(tescKey, {makeSpecializationConstant<float>(1, kTessLevel)});
    if (teseKey[0] == 'n') {
        cfg.addSpecialization(teseKey, {makeSpecializationConstant<uint32_t>(0, side),
                                        makeSpecializationConstant<float>(2, kTessAlpha)});
    } else {
        cfg.addSpecialization(teseKey, {makeSpecializationConstant<uint32_t>(0, side)});
    }
    return cfg;
}

} // namespace

void test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 2.5f));
    GLFWwindow window(1280, 720, camera);

    DeviceRequirement req;
    req.tessellationShader = true;
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT, req);

    // ----- Load model -------------------------------------------------------
    const std::string modelPath = PROJECT_ROOT_PATH "/assets/models/deer.gltf";
    Model gltf = loadModel(modelPath);
    if (!gltf.valid()) {
        throw std::runtime_error("tessellation: failed to load " + modelPath);
    }
    float scale = 1.0f;
    const std::vector<Vertex> sourceVertices = toEngineVertices(gltf, scale);
    // The framework's index-buffer binding cannot infer an index type from its
    // buffer spec, so expand the indexed mesh into a non-indexed vertex stream.
    // Every three consecutive vertices then form one tessellation patch.
    std::vector<Vertex> vertices;
    vertices.reserve(gltf.indices.size());
    for (uint32_t index : gltf.indices) {
        vertices.push_back(sourceVertices[index]);
    }
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    std::cout << "[tessellation] loaded " << sourceVertices.size() << " vertices / " << gltf.indices.size()
              << " indices (" << (vertexCount / 3) << " patches) from " << modelPath << "\n";

    // ----- Shared asset / layout -------------------------------------------
    auto layout = makeLayout();
    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::TESS_VERT_SPV.begin(), shader::TESS_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::TESS_FRAG_SPV.begin(), shader::TESS_FRAG_SPV.end()));
    asset->addShaderSpirvCode("ptesc", std::vector<uint32_t>(shader::PASSTHROUGH_TESC_SPV.begin(),
                                                            shader::PASSTHROUGH_TESC_SPV.end()));
    asset->addShaderSpirvCode("ptese", std::vector<uint32_t>(shader::PASSTHROUGH_TESE_SPV.begin(),
                                                            shader::PASSTHROUGH_TESE_SPV.end()));
    asset->addShaderSpirvCode("ntesc", std::vector<uint32_t>(shader::PNTRIANGLES_TESC_SPV.begin(),
                                                            shader::PNTRIANGLES_TESC_SPV.end()));
    asset->addShaderSpirvCode("ntese", std::vector<uint32_t>(shader::PNTRIANGLES_TESE_SPV.begin(),
                                                            shader::PNTRIANGLES_TESE_SPV.end()));

    // Location 0 = position, 1 = normal; every 3 vertices form one patch.
    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal}, vertexCount, 0u,
        DataTypes::eUnsignedInt);

    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(),
        mgr->DefaultDepthFormat()));

    // ----- Two pipelines: left = pass-through, right = PN triangles --------
    auto passThroughPipeline = mgr->CreatePipeline(
        vertexInput, layout, asset, renderPass, 0, makeTessConfig("ptesc", "ptese", 0u));
    auto pnPipeline = mgr->CreatePipeline(
        vertexInput, layout, asset, renderPass, 0, makeTessConfig("ntesc", "ntese", 1u));

    // ----- Upload vertices -------------------------------------------------
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    upload->submit(syncOp, asset);

    auto sequence = mgr->DrawFrameSequence();
    auto leftDraw = std::make_shared<CmdOpDrawFrame>(passThroughPipeline);
    auto rightDraw = std::make_shared<CmdOpDrawFrame>(pnPipeline);

    std::cout << "[tessellation] left = pass-through, right = PN-triangles, tessLevel = " << kTessLevel << "\n";

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (!sequence->prepareFrame()) {
            continue;
        }
        const auto extent = mgr->getSwapChain()->getExtent2D();
        const CameraMatrix matrices = MakeCameraMatrix(*camera, static_cast<float>(extent->width),
                                                       static_cast<float>(extent->height));
        CameraUBO ubo;
        ubo.viewProj = matrices.proj * matrices.view;
        // Both pipelines share the same asset, so one UBO update feeds both draws.
        for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            leftDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);
        }

        sequence->beginRecording();
        sequence->recordPass(renderPass, [&]() {
            sequence->record(leftDraw);
            sequence->record(rightDraw);
        });
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
        test(0, nullptr);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
