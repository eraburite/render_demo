// particle: one compute pass updates 8192 particles in an SSBO, the graphics
// pass expands them into camera-facing quads reading the very same asset.
//
// The compute side demonstrates three ComputePipeline features:
//   * the workgroup width is a specialization constant, so `--local-size` picks
//     between 64 / 128 / 256 / 512 invocations per group without a shader
//     rebuild (a literal local_size would need one);
//   * the shader guards on gl_GlobalInvocationID.x against particleCount, which
//     is what makes a non-divisor width safe;
//   * dispatch size is derived from the same number, so the CPU never dispatches
//     more groups than there is work for.
//
// Controls: Space pauses the simulation, Esc closes the window.
// Command line: --local-size <n>  compute workgroup width (default 256)
//               --frames <n>      stop after n presented frames (smoke tests)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "ComputeLayoutConfig.h"
#include "DebugViewMode.h"
#include "VertexInputStateConfig.h"

#include "CmdOpComputeDispatch.h"
#include "CmdOpDrawFrame.h"
#include "CmdSyncDevice.h"
#include "ComputePipeline.h"
#include "GraphicsPipeline.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "shader/particle_comp.hpp"
#include "shader/particle_frag.hpp"
#include "shader/particle_vert.hpp"

namespace {

constexpr uint32_t PARTICLE_COUNT = 8192;
constexpr uint32_t DEFAULT_LOCAL_SIZE_X = 256;
// Specialization constant id shared with particle.comp
// (layout(constant_id = 0) const uint LOCAL_SIZE_X).
constexpr uint32_t LOCAL_SIZE_CONSTANT_ID = 0;

struct Particle {
    glm::vec4 position;
    glm::vec4 velocity;
    glm::vec4 color;
};

// Push constant payload shared by compute and vertex stages.  The vertex shader
// needs the view basis (first two rows of the view matrix) to billboard a quad
// towards the camera, and view * proj alone cannot recover it.  128 bytes is
// exactly the minimum guaranteed maxPushConstantsSize.
struct ViewProjection {
    glm::mat4 view;
    glm::mat4 proj;
};

// Mirrors `Params` in particle.comp: one float followed by three 4-byte scalars,
// all scalars so C++ and GLSL agree without any std140 padding assumption.
struct ParticleParams {
    float deltaTime = 0.0f;
    uint32_t particleCount = PARTICLE_COUNT;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
};
static_assert(sizeof(ParticleParams) == 16, "ParticleParams must match the UBO block size");

struct AppOptions {
    uint32_t localSize = DEFAULT_LOCAL_SIZE_X;
    uint32_t frames = 0; // 0 = run until the window is closed
    // Reads the particle SSBO back after the last frame and reports how many
    // particles were never touched.  That is the direct check that the dispatch
    // covered every particle -- with a specialised workgroup width a wrong group
    // count silently leaves a tail un-simulated, which looks like "some particles
    // are frozen" much later.
    bool verify = false;
};

uint32_t parseCount(const char *flag, const char *text)
{
    try {
        const unsigned long value = std::stoul(text);
        return static_cast<uint32_t>(value);
    } catch (const std::exception &) {
        throw std::runtime_error(std::string(flag) + " expects a positive integer, got '" + text + "'");
    }
}

AppOptions parseOptions(int argc, char **argv)
{
    AppOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--local-size" && i + 1 < argc) {
            options.localSize = parseCount("--local-size", argv[++i]);
        } else if (arg == "--frames" && i + 1 < argc) {
            options.frames = parseCount("--frames", argv[++i]);
        } else if (arg == "--verify") {
            options.verify = true;
        } else {
            // Unknown switch: ignore rather than abort, so an extra argument from
            // a wrapper script does not break the demo.
            std::cout << "[particle] ignoring argument '" << arg << "'\n";
        }
    }
    return options;
}

std::vector<Particle> makeParticles()
{
    std::vector<Particle> particles;
    particles.reserve(PARTICLE_COUNT);
    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);  // rx, rz in [-1,1]
    std::uniform_real_distribution<float> unitDist(0.0f, 1.0f); // ry in [0,1]
    // Seed state mirrors the respawn state in particle.comp exactly: a narrow
    // column near the top (y = 1.0 + ry*0.25), with horizontal velocity derived
    // from the same rx/ry so the very first frame is already a fountain cone.
    // Using the full frustum, or an independent random vy, made the first second
    // look like wallpaper and only converged after one full fall cycle.
    for (uint32_t i = 0; i < PARTICLE_COUNT; ++i) {
        Particle p{};
        const float rx = dist(rng);
        const float ry = unitDist(rng);
        const float rz = dist(rng) * 0.4f;
        p.position = glm::vec4(rx * 0.25f, 1.0f + ry * 0.25f, rz, 1.0f);
        p.velocity = glm::vec4(rx * 0.5f, 0.6f + ry * 0.6f, 0.0f, 0.0f);
        p.color = glm::vec4(0.5f + 0.5f * dist(rng), 0.5f + 0.5f * dist(rng), 0.5f + 0.5f * dist(rng), 1.0f);
        particles.push_back(p);
    }
    return particles;
}

} // namespace

void test(int argc, char **argv)
{
    const AppOptions options = parseOptions(argc, argv);

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // The specialization constant only takes effect if it is a legal workgroup
    // size on this device; check up front so the failure names the limit instead
    // of surfacing as a pipeline-creation validation error.
    const vk::PhysicalDeviceLimits &limits = mgr->deviceProperties().limits;
    const uint32_t maxLocalSize =
        std::min(limits.maxComputeWorkGroupSize[0], limits.maxComputeWorkGroupInvocations);
    if (options.localSize == 0 || options.localSize > maxLocalSize) {
        throw std::runtime_error("--local-size must be in [1, " + std::to_string(maxLocalSize) + "], got " +
                                 std::to_string(options.localSize));
    }
    if ((options.localSize & (options.localSize - 1u)) != 0u) {
        std::cout << "[particle] warning: local size " << options.localSize
                  << " is not a power of two; occupancy is usually better with one\n";
    }

    // ---- one compute layout, shared by compute and graphics --------------
    PushConstantDetail viewProjPush{static_cast<uint32_t>(sizeof(ViewProjection)),
                                    vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex, 0};
    auto layout = std::make_shared<ComputeLayoutAtlas>(
        std::vector<ComputeComponent>{ComputeComponent::eParameter, ComputeComponent::eParticleInput,
                                      ComputeComponent::eParticleOutput},
        MAX_FRAMES_IN_FLIGHT, false, viewProjPush);
    layout->setSpec(0, GBufferSpec{1, static_cast<uint32_t>(sizeof(ParticleParams)), DataTypes::eCustom});
    layout->setSpec(1, GBufferSpec{PARTICLE_COUNT, static_cast<uint32_t>(sizeof(Particle)), DataTypes::eCustom});
    layout->setSpec(2, GBufferSpec{PARTICLE_COUNT, static_cast<uint32_t>(sizeof(Particle)), DataTypes::eCustom});

    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("comp", std::vector<uint32_t>(shader::PARTICLE_COMP_SPV.begin(),
                                                            shader::PARTICLE_COMP_SPV.end()));
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::PARTICLE_VERT_SPV.begin(),
                                                            shader::PARTICLE_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::PARTICLE_FRAG_SPV.begin(),
                                                            shader::PARTICLE_FRAG_SPV.end()));

    // Ping-pong: input[frame] reads what output[frame-1] wrote last time.
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const uint32_t source = (frame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
        const ResourceKey inputKey = ResourceKey::make_key(ResourceId(0, 1), frame);
        const ResourceKey outputKey = ResourceKey::make_key(ResourceId(0, 2), source);
        auto output = asset->DescriptorResource(outputKey);
        if (!output) {
            throw std::runtime_error("particle output buffer was not created");
        }
        asset->addDescriptorResource(inputKey, output);
        asset->AliasDescriptorResource(inputKey, outputKey);
    }

    // ---- pipelines -------------------------------------------------------
    // The workgroup width travels as specialization constant 0, so the SPIR-V
    // module stays one binary for every local size the host picks.
    ComputePipelineOptions computeOptions;
    computeOptions.specializationConstants.push_back(
        makeSpecializationConstant<uint32_t>(LOCAL_SIZE_CONSTANT_ID, options.localSize));
    auto computePipeline = mgr->CreateComputePipeline(layout, asset, "comp", std::move(computeOptions));

    auto swapPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));

    auto emptyVertexInput = std::make_shared<VertexInputAtlas>(0, std::vector<VertexComponent>{}, 0u, 0u,
                                                               DataTypes::eUnsignedInt, true, PARTICLE_COUNT * 6);
    auto graphicsPipeline = mgr->CreatePipeline(emptyVertexInput, layout, asset, swapPass, 0);

    // ---- seed every output buffer with the initial particle state --------
    auto particles = makeParticles();
    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        syncOp->OnUpdateRawData(ResourceId(0, 2), frame, particles);
    }
    upload->submit(syncOp, asset);

    // ---- frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto dispatchOp = std::make_shared<CmdOpComputeDispatch>(
        computePipeline, DispatchSize::FromInvocationCount(PARTICLE_COUNT, options.localSize));
    auto drawOp = std::make_shared<CmdOpDrawFrame>(graphicsPipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    std::cout << "[particle] " << PARTICLE_COUNT << " particles, local size " << options.localSize << " -> "
              << DispatchSize::FromInvocationCount(PARTICLE_COUNT, options.localSize).groupCountX
              << " workgroups; Space pauses\n";

    // Wall-clock delta so the simulation speed does not depend on the refresh
    // rate; the old fixed 1/60 step drifted whenever frames were slower.
    float previousTime = 0.0f;
    bool paused = false;
    bool pauseKeyHeld = false;
    uint32_t framesRendered = 0;
    uint32_t lastFrameSlot = UINT32_MAX;

    while (!window.shouldClose()) {
        // Pump the GLFW message queue first: processInputEvents() only reads the
        // cached key state, so without polling the resize callback never fires
        // (no swapchain rebuild) and the close button / ESC are ignored.
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        const auto isKeyDown = [&window](int key) { return window.isKeyDown(key); };
        if (debugKeyJustPressed(isKeyDown, GLFW_KEY_SPACE, pauseKeyHeld)) {
            paused = !paused;
            std::cout << "[particle] " << (paused ? "paused" : "running") << "\n";
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();

        const float wallDelta =
            previousTime > 0.0f ? std::min(std::max(currentTime - previousTime, 0.0f), 0.1f) : 1.0f / 60.0f;
        previousTime = currentTime;
        const float deltaTime = paused ? 0.0f : wallDelta;

        // A zero delta still runs the dispatch: the SSBO ping-pong has to stay
        // consistent with what the vertex shader reads this frame.
        ParticleParams params;
        params.deltaTime = deltaTime;
        params.particleCount = PARTICLE_COUNT;
        dispatchOp->OnUpdateRawData(ResourceId(0, 0), frame, params);

        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);
        const ViewProjection viewProjection{cameraMatrix.view, cameraMatrix.proj};
        drawOp->OnUpdateRawData(ResourceTypes::ePushConst, frame, viewProjection);

        sequence->beginComputeRecording();
        sequence->recordDispatch(dispatchOp);
        sequence->recordComputeBarrier();
        sequence->endComputeRecording(true);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(swapPass, [&]() { sequence->record(drawOp); });
        sequence->endRecording();
        sequence->submitFrame();
        if (!sequence->presentFrame()) {
            continue;
        }

        if (options.frames > 0 && ++framesRendered >= options.frames) {
            std::cout << "[particle] stopping after " << framesRendered << " frames\n";
            lastFrameSlot = frame;
            break;
        }
    }

    if (options.verify) {
        if (lastFrameSlot == UINT32_MAX) {
            std::cout << "[particle] verify skipped: no frame was presented\n";
            return;
        }
        // The buffer (binding 2) of the last slot holds the newest simulation
        // state; the copy is submitted after the frame, so it sees that state.
        auto once = mgr->UploadSequence();
        const auto bytes = once->ReadBack(asset, ResourceKey::make_key(ResourceId(0, 2), lastFrameSlot),
                                          static_cast<size_t>(PARTICLE_COUNT) * sizeof(Particle));
        if (bytes.size() < static_cast<size_t>(PARTICLE_COUNT) * sizeof(Particle)) {
            throw std::runtime_error("particle verify: readback returned no data");
        }

        const auto *gpu = reinterpret_cast<const Particle *>(bytes.data());
        const std::vector<Particle> &seed = particles;
        size_t untouched = 0;
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        double sumY = 0.0;
        for (uint32_t i = 0; i < PARTICLE_COUNT; ++i) {
            if (std::memcmp(&gpu[i], &seed[i], sizeof(Particle)) == 0) {
                ++untouched;
            }
            minY = std::min(minY, gpu[i].position.y);
            maxY = std::max(maxY, gpu[i].position.y);
            sumY += static_cast<double>(gpu[i].position.y);
        }
        std::cout << "[particle] verify: untouched " << untouched << " / " << PARTICLE_COUNT << ", y in [" << minY
                  << ", " << maxY << "], mean y " << (sumY / PARTICLE_COUNT) << "\n";
        if (untouched != 0) {
            throw std::runtime_error("particle verify: " + std::to_string(untouched) +
                                     " particles were never simulated (dispatch did not cover them)");
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
