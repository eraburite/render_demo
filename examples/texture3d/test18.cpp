// texture3d: volume rendering of a procedurally generated 3D texture.
//
// Reference: examples/texture3d of Sascha Willems' Vulkan Examples.  The
// reference renders the volume by stacking one textured quad per slice and
// letting fixed-function blending accumulate them -- 128 draws, 128 uniform
// rewrites.  This framework's draw operation is one draw per record, so the
// volume is integrated inside a single fragment shader instead: the cube's near
// face is rasterised and the ray is marched through the volume with the same
// front-to-back alpha compositing the slice stack produced.  That is why there
// is no per-slice geometry, no second uniform buffer and only one draw call.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

#include "shader/texture3d_frag.hpp"
#include "shader/texture3d_vert.hpp"

namespace {

constexpr uint32_t kVolumeSize = 64;
constexpr int32_t kMaxSteps = 112;

// std430-compatible mirror of the shader's push block.  `inverseModel` is
// deliberately absent: with a second mat4 this would be 144 bytes, above the
// 128 bytes Vulkan guarantees (AMD and Intel expose exactly 128, so pipeline
// creation would fail there).  The fragment shader inverts `model` instead.
struct PushBlock {
    alignas(16) glm::mat4 model;
    float densityGain;
    float scroll;
    float time;
    int32_t steps;
};

static_assert(sizeof(PushBlock) == 80, "push block layout must match std430");

// ---- Deterministic value-noise lattice ------------------------------------
// No <random>, no platform hash: a fixed integer hash keeps the generated
// volume identical on every run, which makes the smoke test reproducible.
uint32_t hashUint(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float latticeValue(int32_t x, int32_t y, int32_t z)
{
    const uint32_t h = hashUint(static_cast<uint32_t>(x) * 0x9E3779B1u
                                ^ hashUint(static_cast<uint32_t>(y) * 0x85EBCA77u
                                           ^ hashUint(static_cast<uint32_t>(z) * 0xC2B2AE3Du)));
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

float fade(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

float valueNoise(const glm::vec3 &p)
{
    const glm::vec3 base = glm::floor(p);
    const glm::vec3 f = p - base;
    const int32_t x = static_cast<int32_t>(base.x);
    const int32_t y = static_cast<int32_t>(base.y);
    const int32_t z = static_cast<int32_t>(base.z);

    const float u = fade(f.x);
    const float v = fade(f.y);
    const float w = fade(f.z);

    auto lerp = [](float t, float a, float b) { return a + t * (b - a); };

    const float c000 = latticeValue(x, y, z);
    const float c100 = latticeValue(x + 1, y, z);
    const float c010 = latticeValue(x, y + 1, z);
    const float c110 = latticeValue(x + 1, y + 1, z);
    const float c001 = latticeValue(x, y, z + 1);
    const float c101 = latticeValue(x + 1, y, z + 1);
    const float c011 = latticeValue(x, y + 1, z + 1);
    const float c111 = latticeValue(x + 1, y + 1, z + 1);

    return lerp(w, lerp(v, lerp(u, c000, c100), lerp(u, c010, c110)),
                lerp(v, lerp(u, c001, c101), lerp(u, c011, c111)));
}

float fractalNoise(const glm::vec3 &p, int octaves)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float normalization = 0.0f;
    float frequency = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += valueNoise(p * frequency) * amplitude;
        normalization += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return normalization > 0.0f ? sum / normalization : 0.0f;
}

// RGBA8 volume: the same density in R, G, B and A.  A soft radial falloff keeps
// the cloud from filling the cube exactly edge to edge, which makes the 3D
// nature of the texture obvious while the cube slowly rotates.
std::vector<uint8_t> generateVolume(uint32_t size, uint32_t &denseVoxels)
{
    std::vector<uint8_t> data(static_cast<size_t>(size) * size * size * 4u, 0u);
    denseVoxels = 0;

    const glm::vec3 center(0.5f);
    for (uint32_t z = 0; z < size; ++z) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const glm::vec3 p((static_cast<float>(x) + 0.5f) / static_cast<float>(size),
                                  (static_cast<float>(y) + 0.5f) / static_cast<float>(size),
                                  (static_cast<float>(z) + 0.5f) / static_cast<float>(size));

                const float noise = fractalNoise(p * 4.0f, 4);
                const float falloff = 1.0f - glm::smoothstep(0.34f, 0.62f, glm::length(p - center));
                const float density = std::clamp(noise * falloff, 0.0f, 1.0f);
                const uint8_t value = static_cast<uint8_t>(std::lround(density * 255.0f));

                const size_t index = (static_cast<size_t>(z) * size * size + static_cast<size_t>(y) * size + x) * 4u;
                data[index + 0] = value;
                data[index + 1] = value;
                data[index + 2] = value;
                data[index + 3] = value;
                if (value > 64) {
                    ++denseVoxels;
                }
            }
        }
    }
    return data;
}

// Unit cube, one vertex per corner of each triangle, CCW as seen from outside.
// examples/common/Geometry.h's CubeVertices() cannot be used with back-face
// culling: three of its six faces are wound inward, so half the shell would be
// culled away and the volume would leak through the holes.
std::vector<Vertex> makeUnitCube()
{
    struct Face {
        glm::vec3 corners[4];
    };
    const Face faces[6] = {
        {{{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}}},       // +X
        {{{-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}}},    // -X
        {{{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}}},        // +Y
        {{{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}}},    // -Y
        {{{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}},        // +Z
        {{{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}}},    // -Z
    };
    const uint32_t order[6] = {0, 1, 2, 0, 2, 3};

    std::vector<Vertex> vertices;
    vertices.reserve(36);
    for (const Face &face : faces) {
        for (uint32_t i : order) {
            Vertex vertex;
            vertex.position = face.corners[i] * 0.5f;
            vertex.color = glm::vec3(1.0f);
            vertex.normal = glm::vec3(0.0f);
            vertex.tangent = glm::vec3(0.0f);
            vertices.push_back(vertex);
        }
    }
    return vertices;
}

} // namespace

void test(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(1024, 768, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ----- Procedural volume ---------------------------------------------
    uint32_t denseVoxels = 0;
    const std::vector<uint8_t> volume = generateVolume(kVolumeSize, denseVoxels);

    TextureImageSpec volumeSpec;
    volumeSpec.width = kVolumeSize;
    volumeSpec.height = kVolumeSize;
    volumeSpec.depth = kVolumeSize;
    volumeSpec.channels = 4;
    volumeSpec.miplevels = 1;
    volumeSpec.layers = 1;
    volumeSpec.faces = 1;
    volumeSpec.format = vk::Format::eR8G8B8A8Unorm;
    volumeSpec.mipsInData = false;
    volumeSpec.addressMode = vk::SamplerAddressMode::eRepeat;
    volumeSpec.anisotropy = false;

    std::cout << "[texture3d] procedural volume " << kVolumeSize << "x" << kVolumeSize << "x" << kVolumeSize
              << " rgba8 (R8G8B8A8_UNORM), 4 fBm octaves, " << denseVoxels << "/"
              << (kVolumeSize * kVolumeSize * kVolumeSize) << " dense voxels\n";

    const std::vector<Vertex> vertices = makeUnitCube();

    // ----- Descriptor layout ---------------------------------------------
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor}, UpdateFrequency::eStatic, true);
    globalSet.specs[0] = volumeSpec;

    auto environmentSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    environmentSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};

    auto frameSet = DescriptorLayoutAtlas::makeSet<FrameComponent, FrameComponentTable>(
        2, {FrameComponent::eViewEyePosition}, UpdateFrequency::eEachFrame, false);
    frameSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(ViewEyePoint)), DataTypes::eCustom};

    layout->setDetails({globalSet, environmentSet, frameSet});
    layout->setPushConstant(static_cast<uint32_t>(sizeof(PushBlock)),
                            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0, std::vector<VertexComponent>{VertexComponent::ePosition}, static_cast<uint32_t>(vertices.size()));

    auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::TEXTURE3D_VERT_SPV.begin(),
                                                           shader::TEXTURE3D_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::TEXTURE3D_FRAG_SPV.begin(),
                                                           shader::TEXTURE3D_FRAG_SPV.end()));

    auto detail = std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eMSAA, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat());
    auto renderPass = mgr->CreateRenderPass(detail);

    // Back-face culling leaves only the near shell, so each pixel marches the
    // ray exactly once.  Depth writes are off because the accumulated volume is
    // blended, not opaque.
    GraphicsPipelineConfig pipelineConfig;
    pipelineConfig.cullMode = vk::CullModeFlagBits::eBack;
    pipelineConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    pipelineConfig.depthWriteEnable = false;
    pipelineConfig.blendEnable = true;
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, pipelineConfig);

    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    syncOp->OnUpdateRawData(ResourceId(0, 0), 0u, volume);
    upload->submit(syncOp, asset);

    // ----- Frame loop -----------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto drawOp = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    float densityGain = 2.5f;

    std::cout << "[texture3d] ray march " << kMaxSteps << " max steps, one draw call\n";
    std::cout << "[texture3d] keys: +/- density gain (current " << densityGain << ")\n";
    // stdout is fully buffered when it is a pipe, and the smoke test terminates
    // the process, so the startup banner has to be flushed by hand.
    std::cout.flush();

    bool plusHeld = false;
    bool minusHeld = false;
    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        auto justPressed = [](bool down, bool &previous) {
            const bool edge = down && !previous;
            previous = down;
            return edge;
        };
        if (justPressed(window.isKeyDown(GLFW_KEY_EQUAL) || window.isKeyDown(GLFW_KEY_KP_ADD), plusHeld)) {
            densityGain = std::min(densityGain + 0.5f, 8.0f);
            std::cout << "[texture3d] density gain " << densityGain << "\n" << std::flush;
        }
        if (justPressed(window.isKeyDown(GLFW_KEY_MINUS) || window.isKeyDown(GLFW_KEY_KP_SUBTRACT), minusHeld)) {
            densityGain = std::max(densityGain - 0.5f, 0.5f);
            std::cout << "[texture3d] density gain " << densityGain << "\n" << std::flush;
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

        ViewEyePoint eye{};
        eye.position = MakeEyePoint(*camera);

        PushBlock push{};
        push.model = glm::scale(glm::rotate(glm::mat4(1.0f), currentTime * 0.35f, glm::vec3(0.0f, 1.0f, 0.0f)),
                                glm::vec3(1.5f));
        push.densityGain = densityGain;
        // Slow drift through the volume: the noise scrolls, the cube does not.
        push.scroll = currentTime * 0.05f;
        push.time = currentTime;
        push.steps = kMaxSteps;

        drawOp->OnUpdateRawData(ResourceId(1, 0), frame, MakeCameraMatrix(*camera, extent));
        drawOp->OnUpdateRawData(ResourceId(2, 0), frame, eye);
        drawOp->OnUpdateRawData(ResourceTypes::ePushConst, frame, push);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(renderPass, [&]() { sequence->record(drawOp); });
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
