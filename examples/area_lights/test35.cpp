// area_lights: linearly transformed cosines (LTC) area lights, migrated from
// LearnOpenGL/src/8.guest/2022/7.area_lights (both 1.area_light and
// 2.multiple_area_lights).
//
// LTC replaces the GGX lobe with a warped cosine distribution whose integral
// over a polygon has a closed form, so an area light costs four edge integrals
// per shaded point instead of an importance-sampled loop.  The two 64x64 RGBA32F
// tables are the ones shipped with the reference (ltc_matrix.hpp), converted to
// raw little-endian float32 in assets/ltc/ so the example does not have to
// compile a 300 KB header.
//
// Differences from the reference (all documented in DESIGN.md):
//   * the light quads are drawn from the SSBO corner positions, so there is no
//     per-light model matrix on the host;
//   * the random light layout uses a fixed seed, so a dump is reproducible;
//   * the checker floor is procedural instead of a concrete texture;
//   * no manual gamma encode: the swapchain is *_SRGB and the hardware does it.
//
// Command line:
//   --lights <n>        1..32 area lights (default 16)
//   --intensity <f>     per-light intensity (default 4.0; the reference uses 2.0)
//   --roughness <f>     GGX roughness of the floor (default 0.5)
//   --diffuse-only      skip the LTC specular term (Mat3 identity only)
//   --one-sided         only light the +x face of every quad (the reference's default is two-sided)
//   --no-quads          hide the emitter quads
//   --dump / --dump-after <n>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "ImageDump.h"
#include "VertexInputStateConfig.h"

#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "root_directory.h"

#include "shader/light_frag.hpp"
#include "shader/light_vert.hpp"
#include "shader/plane_frag.hpp"
#include "shader/plane_vert.hpp"

namespace {

constexpr uint32_t kMaxLights = 32;
constexpr float kPlaneHalfSize = 10.0f;

struct SceneBlock {
    alignas(16) glm::mat4 viewProj{1.0f};
    alignas(16) glm::vec4 eye{0.0f};
    alignas(16) glm::vec4 material{0.6f, 0.6f, 0.6f, 0.5f}; // rgb = albedo, w = roughness
    alignas(16) glm::vec4 params{0.0f};                     // x = count, y = intensity, z/w = flags
};
static_assert(sizeof(SceneBlock) == 112, "SceneBlock must match the std140 SceneBlock");

// std430 layout: four corners, colour and parameters, all 16-byte aligned so the
// GLSL and C++ sides agree without padding surprises.
struct GpuAreaLight {
    alignas(16) glm::vec4 p0{0.0f};
    alignas(16) glm::vec4 p1{0.0f};
    alignas(16) glm::vec4 p2{0.0f};
    alignas(16) glm::vec4 p3{0.0f};
    alignas(16) glm::vec4 color{1.0f};
    alignas(16) glm::vec4 params{1.0f}; // x = intensity, y = two-sided
};
static_assert(sizeof(GpuAreaLight) == 96, "GpuAreaLight must match the std430 AreaLight");

enum class CameraComponent { eBlock };
using CameraConfig = DescriptorComponentSpec<CameraComponent>;
inline constexpr auto CameraComponentTable = std::make_tuple(CameraConfig{
    CameraComponent::eBlock, vk::DescriptorType::eUniformBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment});

enum class LtcComponent { eLtc1, eLtc2 };
using LtcConfig = DescriptorComponentSpec<LtcComponent>;
inline constexpr auto LtcComponentTable = std::make_tuple(
    LtcConfig{LtcComponent::eLtc1, vk::DescriptorType::eCombinedImageSampler, 1,
              vk::ShaderStageFlagBits::eFragment},
    LtcConfig{LtcComponent::eLtc2, vk::DescriptorType::eCombinedImageSampler, 1,
              vk::ShaderStageFlagBits::eFragment});

enum class LightComponent { eLights };
using LightConfig = DescriptorComponentSpec<LightComponent>;
inline constexpr auto LightComponentTable = std::make_tuple(LightConfig{
    LightComponent::eLights, vk::DescriptorType::eStorageBuffer, 1,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment});

struct Options {
    uint32_t lights = 16;
    float intensity = 4.0f;
    float roughness = 0.5f;
    bool diffuseOnly = false;
    bool twoSided = true;
    bool noQuads = false;
    bool dump = false;
    uint32_t dumpAfter = 3;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--lights" && hasValue) {
            options.lights = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--intensity" && hasValue) {
            options.intensity = std::stof(argv[++i]);
        } else if (arg == "--roughness" && hasValue) {
            options.roughness = std::stof(argv[++i]);
        } else if (arg == "--diffuse-only") {
            options.diffuseOnly = true;
        } else if (arg == "--one-sided") {
            options.twoSided = false;
        } else if (arg == "--no-quads") {
            options.noQuads = true;
        } else if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--dump-after" && hasValue) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else {
            throw std::runtime_error("area_lights: unknown argument " + arg);
        }
    }
    if (options.lights == 0 || options.lights > kMaxLights) {
        throw std::runtime_error("area_lights: --lights must be in 1..32");
    }
    options.intensity = std::clamp(options.intensity, 0.0f, 100.0f);
    options.roughness = std::clamp(options.roughness, 0.02f, 1.0f);
    return options;
}

std::vector<uint8_t> readBinary(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("area_lights: cannot open " + path.string());
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) {
        throw std::runtime_error("area_lights: failed to read " + path.string());
    }
    const size_t expected = 64u * 64u * 4u * sizeof(float);
    if (bytes.size() != expected) {
        throw std::runtime_error("area_lights: " + path.string() + " holds " + std::to_string(bytes.size()) +
                                 " bytes, expected " + std::to_string(expected));
    }
    return bytes;
}

// The reference's plane: two triangles, 20x20, normal +Y.
std::vector<Vertex> makePlaneVertices()
{
    const float s = kPlaneHalfSize;
    struct Corner {
        glm::vec3 position;
        glm::vec2 uv;
    };
    const std::array<Corner, 6> corners = {Corner{{-s, 0.0f, -s}, {0.0f, 0.0f}}, Corner{{-s, 0.0f, s}, {0.0f, 1.0f}},
                                           Corner{{s, 0.0f, s}, {1.0f, 1.0f}},   Corner{{-s, 0.0f, -s}, {0.0f, 0.0f}},
                                           Corner{{s, 0.0f, s}, {1.0f, 1.0f}},   Corner{{s, 0.0f, -s}, {1.0f, 0.0f}}};
    std::vector<Vertex> vertices;
    vertices.reserve(corners.size());
    for (const Corner &corner : corners) {
        Vertex vertex;
        vertex.position = corner.position;
        vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        vertex.color = glm::vec3(1.0f);
        vertex.texCoord = corner.uv;
        vertices.push_back(vertex);
    }
    return vertices;
}

// Local quad of one emitter: 2 m wide (z), 2 m tall (y), facing +x.  Rotating it
// around Y and translating it into the scene is what the reference does with a
// per-light model matrix; here it happens once on the host.
std::array<glm::vec3, 4> lightCorners(glm::vec3 offset, float yRotation)
{
    const std::array<glm::vec3, 4> local = {glm::vec3(-1.0f, 2.4f, -1.0f), glm::vec3(-1.0f, 2.4f, 1.0f),
                                            glm::vec3(-1.0f, 0.4f, 1.0f), glm::vec3(-1.0f, 0.4f, -1.0f)};
    const glm::mat4 model = glm::rotate(glm::translate(glm::mat4(1.0f), offset), yRotation, glm::vec3(0.0f, 1.0f, 0.0f));
    std::array<glm::vec3, 4> world{};
    for (size_t i = 0; i < local.size(); ++i) {
        world[i] = glm::vec3(model * glm::vec4(local[i], 1.0f));
    }
    return world;
}

// Fixed seed: the reference seeds from the clock, so two runs never look alike
// and a dumped frame cannot be compared.  Same layout, reproducible.
std::vector<GpuAreaLight> makeLights(uint32_t count, float intensity, bool twoSided)
{
    std::mt19937 generator(20221127u);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);

    // BurlyWood-ish base, tinted per light so neighbouring patches stay readable.
    const glm::vec3 baseColor(0.87f, 0.72f, 0.53f);

    std::vector<GpuAreaLight> lights;
    lights.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        float x = uniform(generator);
        x = x > 0.5f ? x : -x;
        float z = uniform(generator);
        z = z > 0.5f ? z : -z;
        const glm::vec3 offset(x * 8.0f, 0.0f, z * 8.0f);
        const float yRotation = uniform(generator) * glm::two_pi<float>();
        const glm::vec3 tint = glm::mix(baseColor, glm::vec3(uniform(generator), uniform(generator), uniform(generator)),
                                        0.35f);

        const std::array<glm::vec3, 4> corners = lightCorners(offset, yRotation);
        GpuAreaLight light;
        light.p0 = glm::vec4(corners[0], 1.0f);
        light.p1 = glm::vec4(corners[1], 1.0f);
        light.p2 = glm::vec4(corners[2], 1.0f);
        light.p3 = glm::vec4(corners[3], 1.0f);
        light.color = glm::vec4(tint, 1.0f);
        light.params = glm::vec4(intensity, twoSided ? 1.0f : 0.0f, 0.0f, 0.0f);
        lights.push_back(light);
    }
    return lights;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 12.0f, 19.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f,
                                           -34.0f);
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    const std::filesystem::path assetRoot = std::filesystem::path(std::string(PROJECT_ROOT_PATH)) / "assets" / "ltc";
    const std::vector<uint8_t> ltc1 = readBinary(assetRoot / "ltc1.f32");
    const std::vector<uint8_t> ltc2 = readBinary(assetRoot / "ltc2.f32");

    const std::vector<Vertex> vertices = makePlaneVertices();
    const std::vector<GpuAreaLight> lights = makeLights(options.lights, options.intensity, options.twoSided);

    std::cout << "[area_lights] " << options.lights << " LTC lights, intensity " << options.intensity
              << ", roughness " << options.roughness << (options.diffuseOnly ? ", diffuse only" : "")
              << (options.twoSided ? ", two-sided" : "") << "\n";

    // ----- descriptors ------------------------------------------------------
    TextureImageSpec ltcSpec;
    ltcSpec.width = 64;
    ltcSpec.height = 64;
    ltcSpec.channels = 4;
    ltcSpec.format = vk::Format::eR32G32B32A32Sfloat;
    ltcSpec.anisotropy = false;
    ltcSpec.addressMode = vk::SamplerAddressMode::eClampToEdge;

    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto cameraSet = DescriptorLayoutAtlas::makeSet<CameraComponent, CameraComponentTable>(
        0, {CameraComponent::eBlock}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(SceneBlock)), DataTypes::eCustom};
    // The two tables never change, so one shared copy serves every frame; the
    // camera block above must stay per-frame (it is rewritten every frame).
    auto ltcSet = DescriptorLayoutAtlas::makeSet<LtcComponent, LtcComponentTable>(
        1, {LtcComponent::eLtc1, LtcComponent::eLtc2}, UpdateFrequency::eStatic, true);
    ltcSet.specs[0] = ltcSpec;
    ltcSet.specs[1] = ltcSpec;
    auto lightSet = DescriptorLayoutAtlas::makeSet<LightComponent, LightComponentTable>(
        2, {LightComponent::eLights}, UpdateFrequency::eStatic, true);
    lightSet.specs[0] = GBufferSpec{options.lights, static_cast<uint32_t>(sizeof(GpuAreaLight)),
                                    DataTypes::eCustom};
    layout->setDetails({cameraSet, ltcSet, lightSet});

    // One asset holds all four modules; the two pipelines below pick different
    // stage keys, so they share the descriptor sets (and therefore the UBO
    // upload) instead of duplicating them.
    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("plane_vert",
                              std::vector<uint32_t>(shader::PLANE_VERT_SPV.begin(), shader::PLANE_VERT_SPV.end()));
    asset->addShaderSpirvCode("plane_frag",
                              std::vector<uint32_t>(shader::PLANE_FRAG_SPV.begin(), shader::PLANE_FRAG_SPV.end()));
    asset->addShaderSpirvCode("light_vert",
                              std::vector<uint32_t>(shader::LIGHT_VERT_SPV.begin(), shader::LIGHT_VERT_SPV.end()));
    asset->addShaderSpirvCode("light_frag",
                              std::vector<uint32_t>(shader::LIGHT_FRAG_SPV.begin(), shader::LIGHT_FRAG_SPV.end()));

    // ----- passes and pipelines ---------------------------------------------
    auto pass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));

    auto planeVertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal, VertexComponent::eUV},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, 1u);
    // The emitter quads reuse the same vertex buffer (the shader ignores it) and
    // one instance per light.
    auto quadVertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal, VertexComponent::eUV},
        static_cast<uint32_t>(vertices.size()), 0u, DataTypes::eUnsignedInt, false, 0u, options.lights);

    GraphicsPipelineConfig planeConfig;
    planeConfig.cullMode = vk::CullModeFlagBits::eNone;
    planeConfig.stages = {{"plane_vert", vk::ShaderStageFlagBits::eVertex},
                          {"plane_frag", vk::ShaderStageFlagBits::eFragment}};
    auto planePipeline = mgr->CreatePipeline(planeVertexInput, layout, asset, pass, 0, planeConfig);

    GraphicsPipelineConfig lightConfig;
    lightConfig.cullMode = vk::CullModeFlagBits::eNone;
    lightConfig.stages = {{"light_vert", vk::ShaderStageFlagBits::eVertex},
                          {"light_frag", vk::ShaderStageFlagBits::eFragment}};
    auto lightPipeline = mgr->CreatePipeline(quadVertexInput, layout, asset, pass, 0, lightConfig);

    // ----- uploads ----------------------------------------------------------
    // After CreatePipeline: the vertex buffer is an *input* resource that only
    // exists once a pipeline asked for it, so uploading earlier silently drops
    // the data (the plane then draws six zeroed vertices -- invisible, and with
    // no validation error).
    auto upload = mgr->UploadSequence();
    {
        auto sync = std::make_shared<CmdSyncDevice>(asset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
        // Both textures and the light array live in shared (frame-0) sets, so a
        // single upload covers every frame slot.
        sync->OnUpdateRawData(ResourceId(1, 0), 0u, ltc1);
        sync->OnUpdateRawData(ResourceId(1, 1), 0u, ltc2);
        sync->OnUpdateRawData(ResourceId(2, 0), 0u, lights);
        upload->submit(sync, asset);
    }

    // ----- frame loop -------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto planeDraw = std::make_shared<CmdOpDrawFrame>(planePipeline);
    auto lightDraw = std::make_shared<CmdOpDrawFrame>(lightPipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    uint32_t framesRendered = 0;
    bool dumpDone = false;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height > 0 ? extent.height : 1);

        const glm::mat4 view = camera->GetViewMatrix();
        glm::mat4 projection = glm::perspective(glm::radians(camera->Zoom), aspect, 0.1f, 100.0f);
        projection[1][1] *= -1.0f;

        SceneBlock ubo;
        ubo.viewProj = projection * view;
        ubo.eye = glm::vec4(camera->Position, 1.0f);
        ubo.material = glm::vec4(0.62f, 0.60f, 0.58f, options.roughness);
        ubo.params = glm::vec4(static_cast<float>(options.lights), 1.0f, options.diffuseOnly ? 1.0f : 0.0f,
                               options.twoSided ? 1.0f : 0.0f);
        planeDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);
        lightDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(pass, [&]() {
            sequence->record(planeDraw);
            if (!options.noQuads) {
                sequence->record(lightDraw);
            }
        });
        sequence->endRecording();
        sequence->submitFrame();

        ++framesRendered;
        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            auto readback = ImageReadback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                          mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                          *mgr->getQueueFamilyIndices().graphicsFamily);
            DumpRequest frameRequest;
            frameRequest.label = options.diffuseOnly ? "frame-diffuse-only" : "frame";
            frameRequest.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;

            const auto results = DumpImages(readback, {frameRequest}, "dump", 0);
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
