// terraintessellation: a procedural height-field terrain whose quad patches
// are subdivided on the GPU according to their distance from the camera.
// --wireframe starts in line mode; press W to toggle wireframe at runtime.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
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
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "shader/terrain_frag.hpp"
#include "shader/terrain_tesc.hpp"
#include "shader/terrain_tese.hpp"
#include "shader/terrain_vert.hpp"

namespace {

// 32 x 32 quad patches spanning 128 x 128 world units (4 units per patch).
// Each patch carries four control points -> 4096 vertices in one buffer.
constexpr int kGrid = 32;
constexpr float kTerrainSize = 128.0f;
constexpr float kHalfSize = kTerrainSize * 0.5f;
constexpr float kCellSize = kTerrainSize / static_cast<float>(kGrid);

// set0 binding0: camera viewProj + eye position + LOD parameters.  All four
// tessellation-related stages consume the same UBO.
enum class TerrainComponent { eCamera };
using TerrainConfig = DescriptorComponentSpec<TerrainComponent>;
inline constexpr auto TerrainComponentTable = std::make_tuple(TerrainConfig{
    TerrainComponent::eCamera, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl
        | vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eFragment});

struct TerrainUBO {
    alignas(16) glm::mat4 viewProj{};
    alignas(16) glm::vec4 eye{};
    // near distance, far distance, minimum tess level, maximum tess level
    alignas(16) glm::vec4 tessParams{20.0f, 110.0f, 1.0f, 8.0f};
};

std::shared_ptr<DescriptorLayoutAtlas> makeLayout()
{
    auto layout = std::make_shared<DescriptorLayoutAtlas>();
    layout->setDetails({DescriptorLayoutAtlas::makeSet<TerrainComponent, TerrainComponentTable>(
        0, {TerrainComponent::eCamera}, UpdateFrequency::eEachFrame, false)});
    return layout;
}

// Sum a few sine waves of different frequency plus a gentle radial falloff.
float heightAt(float x, float z)
{
    float h = 6.0f * std::sin(x * 0.08f) * std::cos(z * 0.08f);
    h += 3.0f * std::sin(x * 0.19f + 1.3f) * std::cos(z * 0.17f - 0.7f);
    h += 1.5f * std::sin(x * 0.41f + z * 0.33f);
    const float r = std::sqrt(x * x + z * z) / kHalfSize;
    h -= 3.0f * r * r;
    return h;
}

glm::vec3 normalAt(float x, float z)
{
    constexpr float e = 0.5f;
    const float dx = heightAt(x + e, z) - heightAt(x - e, z);
    const float dz = heightAt(x, z + e) - heightAt(x, z - e);
    return glm::normalize(glm::vec3(-dx, 2.0f * e, -dz));
}

glm::vec3 colorAt(float height)
{
    const glm::vec3 low(0.12f, 0.32f, 0.10f);
    const glm::vec3 mid(0.42f, 0.34f, 0.22f);
    const glm::vec3 high(0.86f, 0.88f, 0.92f);
    const glm::vec3 grassRock = glm::mix(low, mid, glm::clamp((height + 2.0f) / 8.0f, 0.0f, 1.0f));
    return glm::mix(grassRock, high, glm::clamp((height - 6.0f) / 5.0f, 0.0f, 1.0f));
}

// OpenGL-style patch ordering: 0:(x0,z0) 1:(x1,z0) 2:(x1,z1) 3:(x0,z1).
std::vector<Vertex> buildTerrain()
{
    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(kGrid) * kGrid * 4);
    const float xs[4] = {0.0f, kCellSize, kCellSize, 0.0f};
    const float zs[4] = {0.0f, 0.0f, kCellSize, kCellSize};

    for (int j = 0; j < kGrid; ++j) {
        for (int i = 0; i < kGrid; ++i) {
            const float x0 = -kHalfSize + static_cast<float>(i) * kCellSize;
            const float z0 = -kHalfSize + static_cast<float>(j) * kCellSize;
            for (int k = 0; k < 4; ++k) {
                const float x = x0 + xs[k];
                const float z = z0 + zs[k];
                const float h = heightAt(x, z);
                Vertex v;
                v.position = glm::vec3(x, h, z);
                v.normal = normalAt(x, z);
                v.color = colorAt(h);
                v.texCoord = glm::vec2((x + kHalfSize) / kTerrainSize, (z + kHalfSize) / kTerrainSize);
                v.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
                vertices.push_back(v);
            }
        }
    }
    return vertices;
}

bool justPressed(bool current, bool &held)
{
    const bool result = current && !held;
    held = current;
    return result;
}

GraphicsPipelineConfig makeConfig(vk::PolygonMode mode)
{
    GraphicsPipelineConfig cfg;
    cfg.addStage("vert", vk::ShaderStageFlagBits::eVertex)
        .addStage("tesc", vk::ShaderStageFlagBits::eTessellationControl)
        .addStage("tese", vk::ShaderStageFlagBits::eTessellationEvaluation)
        .addStage("frag", vk::ShaderStageFlagBits::eFragment);
    cfg.topology = vk::PrimitiveTopology::ePatchList;
    cfg.patchControlPoints = 4;
    cfg.cullMode = vk::CullModeFlagBits::eNone;
    cfg.polygonMode = mode;
    return cfg;
}

} // namespace

void test(int argc, char **argv)
{
    bool wireframe = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--wireframe") {
            wireframe = true;
        }
    }

    // Slightly pitched down so the terrain fills the view.
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 20.0f, 30.0f), glm::vec3(0.0f, 1.0f, 0.0f), YAW,
                                           -25.0f);
    GLFWwindow window(1280, 720, camera);

    DeviceRequirement req;
    req.tessellationShader = true;
    req.fillModeNonSolid = true;
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT, req);

    const std::vector<Vertex> vertices = buildTerrain();
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
    std::cout << "[terraintessellation] " << (kGrid * kGrid) << " quad patches, " << vertexCount
              << " control points\n";

    auto layout = makeLayout();
    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("vert",
                              std::vector<uint32_t>(shader::TERRAIN_VERT_SPV.begin(), shader::TERRAIN_VERT_SPV.end()));
    asset->addShaderSpirvCode("tesc",
                              std::vector<uint32_t>(shader::TERRAIN_TESC_SPV.begin(), shader::TERRAIN_TESC_SPV.end()));
    asset->addShaderSpirvCode("tese",
                              std::vector<uint32_t>(shader::TERRAIN_TESE_SPV.begin(), shader::TERRAIN_TESE_SPV.end()));
    asset->addShaderSpirvCode("frag",
                              std::vector<uint32_t>(shader::TERRAIN_FRAG_SPV.begin(), shader::TERRAIN_FRAG_SPV.end()));

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor,
                                        VertexComponent::eUV, VertexComponent::eNormal},
        vertexCount, 0, DataTypes::eUnsignedInt);

    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));

    auto fillPipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, makeConfig(vk::PolygonMode::eFill));
    auto linePipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, makeConfig(vk::PolygonMode::eLine));

    // ----- Upload patches ---------------------------------------------------
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    upload->submit(syncOp, asset);

    auto sequence = mgr->DrawFrameSequence();
    auto fillDraw = std::make_shared<CmdOpDrawFrame>(fillPipeline);
    auto lineDraw = std::make_shared<CmdOpDrawFrame>(linePipeline);

    bool wHeld = false;
    std::cout << "[terraintessellation] press W to toggle wireframe (start: "
              << (wireframe ? "wireframe" : "solid") << ")\n";

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (justPressed(window.isKeyDown(GLFW_KEY_W), wHeld)) {
            wireframe = !wireframe;
            std::cout << "[terraintessellation] wireframe " << (wireframe ? "ON" : "OFF") << "\n";
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const auto extent = mgr->getSwapChain()->getExtent2D();
        const CameraMatrix matrices = MakeCameraMatrix(*camera, static_cast<float>(extent->width),
                                                       static_cast<float>(extent->height));
        TerrainUBO ubo;
        ubo.viewProj = matrices.proj * matrices.view;
        ubo.eye = glm::vec4(camera->Position, 1.0f);
        fillDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);
        lineDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);

        auto draw = wireframe ? lineDraw : fillDraw;
        sequence->beginRecording();
        // The uniform block is a per-frame resource: updateRawData only marks it
        // dirty, and CmdSyncDevice is what emits the staging -> device copy.
        // Without it the GPU keeps the values from the one-shot upload, so a
        // camera move or a window resize never reaches the shader.
        sequence->record(syncOp);
        sequence->recordPass(renderPass, [&]() { sequence->record(draw); });
        sequence->endRecording();
        sequence->submitFrame();
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
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
