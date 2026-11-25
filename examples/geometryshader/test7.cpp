// geometryshader: render a glTF mesh with a Phong shader and visualise its
// per-vertex normals with a geometry shader emitting line segments.
// Press G to toggle the normal visualisation (line topology).
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "GlTFLoader.h"
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
#include "root_directory.h"

#include "shader/mesh_frag.hpp"
#include "shader/mesh_vert.hpp"
#include "shader/normaldebug_frag.hpp"
#include "shader/normaldebug_geom.hpp"

namespace {

// set0 binding0: camera viewProj (vertex + geometry stages).
enum class GeoComponent { eCameraMatrix };
using GeoConfig = DescriptorComponentSpec<GeoComponent>;
inline constexpr auto GeoComponentTable = std::make_tuple(
    GeoConfig{GeoComponent::eCameraMatrix, vk::DescriptorType::eUniformBuffer, 1,
              vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eGeometry});

struct CameraUBO {
    alignas(16) glm::mat4 viewProj{};
};

std::shared_ptr<DescriptorLayoutAtlas> makeLayout()
{
    auto layout = std::make_shared<DescriptorLayoutAtlas>();
    layout->setDetails({DescriptorLayoutAtlas::makeSet<GeoComponent, GeoComponentTable>(
        0, {GeoComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false)});
    return layout;
}

// Convert loaded glTF vertices into the engine Vertex layout and fit the
// model into a unit-ish box centred at the origin (suzanne is ~2 units wide).
std::vector<Vertex> toEngineVertices(const GlTFModel &model, float &outScale)
{
    const glm::vec3 extent = model.maxBounds - model.minBounds;
    const float maxExtent = std::max({extent.x, extent.y, extent.z, 0.0001f});
    outScale = 2.0f / maxExtent;
    const glm::vec3 center = (model.minBounds + model.maxBounds) * 0.5f;

    std::vector<Vertex> result;
    result.reserve(model.vertices.size());
    for (const GlTFVertex &src : model.vertices) {
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

bool justPressed(bool current, bool &held)
{
    const bool result = current && !held;
    held = current;
    return result;
}

} // namespace

void test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 2.5f));

    GLFWwindow window(1280, 720, camera);

    DeviceRequirement req;
    req.geometryShader = true;
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT, req);

    // ----- Load model -------------------------------------------------------
    std::string modelPath = PROJECT_ROOT_PATH "/data/models/suzanne.gltf";
    GlTFModel gltf = loadGlTF(modelPath);
    if (!gltf.valid()) {
        throw std::runtime_error("geometryshader: failed to load " + modelPath);
    }
    float scale = 1.0f;
    std::vector<Vertex> vertices = toEngineVertices(gltf, scale);
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    std::cout << "[geometryshader] loaded " << vertexCount << " vertices from " << modelPath << "\n";

    auto layout = makeLayout();
    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::MESH_VERT_SPV.begin(), shader::MESH_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::MESH_FRAG_SPV.begin(), shader::MESH_FRAG_SPV.end()));
    asset->addShaderSpirvCode("geom", std::vector<uint32_t>(shader::NORMALDEBUG_GEOM_SPV.begin(),
                                                            shader::NORMALDEBUG_GEOM_SPV.end()));
    asset->addShaderSpirvCode("normalfrag", std::vector<uint32_t>(shader::NORMALDEBUG_FRAG_SPV.begin(),
                                                                  shader::NORMALDEBUG_FRAG_SPV.end()));

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor,
                                        VertexComponent::eUV, VertexComponent::eNormal},
        vertexCount, 0, DataTypes::eUnsignedInt);

    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(),
        mgr->DefaultDepthFormat()));

    // Solid pipeline: vert + frag, triangle list.
    GraphicsPipelineConfig solidConfig;
    solidConfig.cullMode = vk::CullModeFlagBits::eBack;
    auto solidPipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, solidConfig);

    // Normal-debug pipeline: vert + geom + normalfrag, line strip topology.
    // line_strip with max_vertices=6 emits 6 vertices per triangle as 3 separate
    // primitives; triangle list input stays the same.
    GraphicsPipelineConfig normalConfig;
    normalConfig.addStage("vert", vk::ShaderStageFlagBits::eVertex)
        .addStage("geom", vk::ShaderStageFlagBits::eGeometry)
        .addStage("normalfrag", vk::ShaderStageFlagBits::eFragment);
    normalConfig.topology = vk::PrimitiveTopology::eTriangleList; // geom outputs line_strip
    // geometry shader emits line segments; the rasterizer sees line topology
    // from the geometry stage output automatically.
    normalConfig.cullMode = vk::CullModeFlagBits::eNone;
    auto normalPipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, normalConfig);

    // ----- Upload vertices --------------------------------------------------
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    upload->submit(syncOp, asset);

    auto sequence = mgr->DrawFrameSequence();
    auto solidDraw = std::make_shared<CmdOpDrawFrame>(solidPipeline);
    auto normalDraw = std::make_shared<CmdOpDrawFrame>(normalPipeline);

    bool showNormals = false;
    bool gHeld = false;
    std::cout << "[geometryshader] press G to toggle normal visualisation\n";

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (justPressed(window.isKeyDown(GLFW_KEY_G), gHeld)) {
            showNormals = !showNormals;
            std::cout << "[geometryshader] normals " << (showNormals ? "ON" : "OFF") << "\n";
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const auto extent = mgr->getSwapChain()->getExtent2D();
        const CameraMatrix matrices = MakeCameraMatrix(*camera, static_cast<float>(extent->width),
                                                       static_cast<float>(extent->height));
        CameraUBO ubo;
        ubo.viewProj = matrices.proj * matrices.view;
        for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            solidDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);
            normalDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);
        }

        auto draw = showNormals ? normalDraw : solidDraw;
        sequence->beginRecording();
        sequence->recordPass(renderPass, [&]() { sequence->record(draw); });
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
