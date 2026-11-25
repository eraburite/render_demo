// stencil: outline rendering with the stencil buffer (reference:
// D:/Vulkan-samples/examples/stencilbuffer).
//
//   draw 1  toon-shaded object; stencil op ALWAYS -> REPLACE writes reference R
//           into every visible texel
//   draw 2  the same mesh extruded along its normals; stencil test NOT_EQUAL R
//           rejects everything draw 1 covered, so only the silhouette ring
//           survives (depth test off)
//
// The point of the example is the *two-layer* split: the render pass declares
// the S8 attachment (IAttachmentDetail / RenderPassBlueprint::stencil), while
// the reference / compare mask / write mask are dynamic pipeline state supplied
// at record time by CmdOpStencilState.  Nothing is baked into a pipeline, so
// --no-fill, --ref and --clear-mid all change the outline without rebuilding
// anything.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "GlTFLoader.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
#include "DescriptorLayoutConfig.h"
#include "ImageDump.h"
#include "VertexInputStateConfig.h"

#include "CmdOpClearStencil.h"
#include "CmdOpDrawFrame.h"
#include "CmdOpStencilState.h"
#include "CmdSyncDevice.h"
#include "ImageReadback.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"
#include "root_directory.h"

#include "shader/outline_frag.hpp"
#include "shader/outline_vert.hpp"
#include "shader/toon_frag.hpp"
#include "shader/toon_vert.hpp"

namespace {

// std140 block shared by both vertex shaders.  Both must declare the whole
// block identically even though only params.x is used by outline.vert.
struct StencilOutlineUbo {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::mat4 model;
    alignas(16) glm::vec4 lightDir;
    alignas(16) glm::vec4 params; // x = outline width
};
static_assert(sizeof(StencilOutlineUbo) == 224, "UBO must match the std140 block (3 mat4 + 2 vec4)");

constexpr float kTargetHeight = 2.0f;

// Expands every glTF primitive into a plain non-indexed triangle stream in
// world space, applies the node transform and the material base colour, then
// normalises the whole mesh to a fixed height around the origin.
std::vector<Vertex> loadMesh(const std::string &path)
{
    const GlTFModel model = loadGlTF(path);
    if (!model.valid()) {
        throw std::runtime_error("stencil: " + path + " produced no geometry");
    }
    const std::vector<glm::mat4> world = computeNodeWorldMatrices(model);

    std::vector<Vertex> vertices;
    glm::vec3 low(std::numeric_limits<float>::max());
    glm::vec3 high(std::numeric_limits<float>::lowest());

    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
        const GlTFNode &node = model.nodes[nodeIndex];
        if (node.meshIndex < 0 || node.meshIndex >= static_cast<int>(model.meshes.size())
            || nodeIndex >= world.size()) {
            continue;
        }
        const glm::mat4 &transform = world[nodeIndex];
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

        for (const GlTFPrimitive &primitive : model.meshes[node.meshIndex].primitives) {
            glm::vec3 tint(1.0f);
            if (primitive.materialIndex >= 0
                && primitive.materialIndex < static_cast<int>(model.materials.size())) {
                tint = glm::vec3(model.materials[primitive.materialIndex].baseColorFactor);
            }
            for (uint32_t i = primitive.firstIndex;
                 i < primitive.firstIndex + primitive.indexCount && i < model.indices.size(); ++i) {
                const uint32_t index = model.indices[i];
                if (index >= model.vertices.size()) {
                    continue;
                }
                const GlTFVertex &src = model.vertices[index];
                Vertex vertex;
                vertex.position = glm::vec3(transform * glm::vec4(src.position, 1.0f));
                vertex.normal = glm::normalize(normalMatrix * src.normal);
                vertex.color = src.color * tint;
                vertices.push_back(vertex);
                low = glm::min(low, vertex.position);
                high = glm::max(high, vertex.position);
            }
        }
    }

    const glm::vec3 span = high - low;
    const float maxSpan = std::max({span.x, span.y, span.z, 0.0001f});
    const float scale = kTargetHeight / maxSpan;
    const glm::vec3 center = (low + high) * 0.5f;
    for (Vertex &vertex : vertices) {
        vertex.position = (vertex.position - center) * scale;
    }
    return vertices;
}

// Magenta outline colour (outline.frag) is far from the toon blue and the dark
// background, so a simple threshold is a reliable pixel counter.
// The swapchain is normally an sRGB format, so the shader's linear magenta
// (1.0, 0.12, 0.72) is stored as roughly (255, 97, 222).  Test "red and blue
// both well above green" instead of a hard threshold on the green byte: that
// rejects the dark background and the toon blue (green-dominant) equally well
// on both sRGB and UNORM swapchains.
size_t countOutlinePixels(const std::vector<uint8_t> &rgb)
{
    size_t count = 0;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const int r = rgb[i];
        const int g = rgb[i + 1];
        const int b = rgb[i + 2];
        if (r > 150 && r > g + 60 && b > g + 60) {
            ++count;
        }
    }
    return count;
}

} // namespace

void test(int argc, char **argv)
{
    bool dumpOnce = false;
    bool noFill = false;
    bool clearMid = false;
    uint32_t fillRef = 1;
    uint32_t outlineRef = 1;
    uint32_t resizeAfter = 0;
    uint32_t dumpAfter = 0;
    float outlineWidth = 0.035f;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump") {
            dumpOnce = true;
        } else if (arg == "--no-fill") {
            noFill = true;
        } else if (arg == "--clear-mid") {
            clearMid = true;
        } else if (arg == "--fill-ref" && i + 1 < argc) {
            fillRef = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--ref" && i + 1 < argc) {
            outlineRef = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--width" && i + 1 < argc) {
            outlineWidth = std::stof(argv[++i]);
        } else if (arg == "--resize-after" && i + 1 < argc) {
            resizeAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--dump-after" && i + 1 < argc) {
            dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
    }

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.15f, 3.8f));
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ---- geometry ---------------------------------------------------------
    const std::vector<Vertex> vertices =
        loadMesh(PROJECT_ROOT_PATH "/data/models/suzanne.gltf");
    std::cout << "[stencil] suzanne " << vertices.size() << " expanded vertices, outline width "
              << outlineWidth << std::endl;

    // ---- one descriptor layout / one asset shared by both pipelines -------
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        0, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(StencilOutlineUbo)), DataTypes::eCustom};
    layout->setDetails({cameraSet});

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eColor,
                                     VertexComponent::eNormal},
        static_cast<uint32_t>(vertices.size()));

    auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    asset->addShaderSpirvCode("toon_vert", std::vector<uint32_t>(shader::TOON_VERT_SPV.begin(),
                                                                 shader::TOON_VERT_SPV.end()));
    asset->addShaderSpirvCode("toon_frag", std::vector<uint32_t>(shader::TOON_FRAG_SPV.begin(),
                                                                 shader::TOON_FRAG_SPV.end()));
    asset->addShaderSpirvCode("outline_vert", std::vector<uint32_t>(shader::OUTLINE_VERT_SPV.begin(),
                                                                     shader::OUTLINE_VERT_SPV.end()));
    asset->addShaderSpirvCode("outline_frag", std::vector<uint32_t>(shader::OUTLINE_FRAG_SPV.begin(),
                                                                     shader::OUTLINE_FRAG_SPV.end()));

    // ---- render pass: colour + a depth-stencil attachment with an S8 plane --
    StencilAttachmentSpec stencilSpec;
    stencilSpec.loadOp = vk::AttachmentLoadOp::eClear;
    stencilSpec.storeOp = vk::AttachmentStoreOp::eStore;
    stencilSpec.clearValue = 0;
    stencilSpec.hostReadable = true; // --dump reads the S8 plane back
    auto detail = std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(),
        mgr->DepthStencilFormat(), vk::ImageLayout::ePresentSrcKHR, DependencyPreset::eSwapChain,
        stencilSpec);
    auto renderPass = mgr->CreateRenderPass(detail);
    if (!renderPass->hasStencil() || !renderPass->depthStencilImage(0)) {
        throw std::runtime_error("stencil: render pass did not expose a depth-stencil attachment");
    }
    std::cout << "[stencil] depth-stencil format " << vk::to_string(renderPass->depthStencilFormat()) << std::endl;

    // ---- two pipelines, same asset, different stage keys ------------------
    // Pass 1: ALWAYS / REPLACE.  depthFailOp=KEEP means only the front-most
    // visible surface is marked; back faces hidden by depth do not tag texels.
    vk::StencilOpState fill{};
    fill.failOp = vk::StencilOp::eKeep;
    fill.passOp = vk::StencilOp::eReplace;
    fill.depthFailOp = vk::StencilOp::eKeep;
    fill.compareOp = vk::CompareOp::eAlways;
    fill.compareMask = 0xff;
    fill.writeMask = 0xff;
    fill.reference = fillRef;

    GraphicsPipelineConfig toonConfig;
    toonConfig.addStage("toon_vert", vk::ShaderStageFlagBits::eVertex)
        .addStage("toon_frag", vk::ShaderStageFlagBits::eFragment);
    toonConfig.cullMode = vk::CullModeFlagBits::eBack;
    toonConfig.frontFace = vk::FrontFace::eCounterClockwise;
    toonConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    toonConfig.setStencil(fill, fill).setDynamicStencil(true, true, true);
    auto toonPipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, toonConfig);

    // Pass 2: NOT_EQUAL, no depth test.  writeMask is set to 0 by the CmdOp so
    // the outline never changes the stencil plane -- the dump then measures
    // exactly what pass 1 wrote.
    vk::StencilOpState outlineState{};
    outlineState.failOp = vk::StencilOp::eKeep;
    outlineState.passOp = vk::StencilOp::eKeep;
    outlineState.depthFailOp = vk::StencilOp::eKeep;
    outlineState.compareOp = vk::CompareOp::eNotEqual;
    outlineState.compareMask = 0xff;
    outlineState.writeMask = 0;
    outlineState.reference = outlineRef;

    GraphicsPipelineConfig outlineConfig;
    outlineConfig.addStage("outline_vert", vk::ShaderStageFlagBits::eVertex)
        .addStage("outline_frag", vk::ShaderStageFlagBits::eFragment);
    outlineConfig.cullMode = vk::CullModeFlagBits::eNone;
    outlineConfig.depthTestEnable = false;
    outlineConfig.depthWriteEnable = false;
    outlineConfig.setStencil(outlineState, outlineState).setDynamicStencil(true, true, true);
    auto outlinePipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, outlineConfig);

    // ---- uploads ----------------------------------------------------------
    auto upload = mgr->UploadSequence();
    auto uploadSync = std::make_shared<CmdSyncDevice>(asset);
    uploadSync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
    upload->submit(uploadSync, asset);

    // ---- frame loop -------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();
    auto toonDraw = std::make_shared<CmdOpDrawFrame>(toonPipeline);
    auto outlineDraw = std::make_shared<CmdOpDrawFrame>(outlinePipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);

    // Dynamic stencil state, attached to the draws so it is always recorded
    // after vkCmdBindPipeline (GraphicsPipeline::AfterBindHook).
    auto fillDynamic = std::make_shared<CmdOpStencilState>();
    fillDynamic->setReference(fillRef);
    fillDynamic->setCompareMask(0xff);
    fillDynamic->setWriteMask(noFill ? 0u : 0xffu);
    auto outlineDynamic = std::make_shared<CmdOpStencilState>();
    outlineDynamic->setReference(outlineRef);
    outlineDynamic->setCompareMask(0xff);
    outlineDynamic->setWriteMask(0u);
    toonDraw->setStencilState(fillDynamic);
    outlineDraw->setStencilState(outlineDynamic);

    auto clearStencil = std::make_shared<CmdOpClearStencil>(
        *mgr->getSwapChain()->getExtent2D(), 0u);

    std::cout << "[stencil] keys: F toggle stencil write, C toggle mid-pass clear, "
                 "+/- outline width, R cycle outline reference" << std::endl;

    bool dumpPending = false;
    bool dumpDone = false;
    uint32_t renderedFrames = 0;
    bool resizeRequested = false;
    uint64_t attachmentGeneration = renderPass->attachmentGeneration();
    bool keyFHeld = false;
    bool keyCHeld = false;
    bool keyRHeld = false;

    const glm::mat4 model = glm::rotate(
        glm::rotate(glm::mat4(1.0f), glm::radians(-18.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
        glm::radians(8.0f), glm::vec3(1.0f, 0.0f, 0.0f));

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (resizeAfter != 0 && !resizeRequested && renderedFrames >= resizeAfter) {
            resizeRequested = true;
            window.resize(1024, 600);
            std::cout << "[stencil] requested resize to 1024x600" << std::endl;
        }

        const bool keyF = window.isKeyDown(GLFW_KEY_F);
        if (keyF && !keyFHeld) {
            noFill = !noFill;
            fillDynamic->setWriteMask(noFill ? 0u : 0xffu);
            std::cout << "[stencil] fill write mask " << (noFill ? 0 : 0xff) << std::endl;
        }
        keyFHeld = keyF;
        const bool keyC = window.isKeyDown(GLFW_KEY_C);
        if (keyC && !keyCHeld) {
            clearMid = !clearMid;
            std::cout << "[stencil] mid-pass clear " << (clearMid ? "on" : "off") << std::endl;
        }
        keyCHeld = keyC;
        const bool keyR = window.isKeyDown(GLFW_KEY_R);
        if (keyR && !keyRHeld) {
            outlineRef = outlineRef == 1 ? 2 : 1;
            outlineDynamic->setReference(outlineRef);
            std::cout << "[stencil] outline reference " << outlineRef << std::endl;
        }
        keyRHeld = keyR;
        if (window.isKeyDown(GLFW_KEY_EQUAL)) {
            outlineWidth = std::min(outlineWidth + 0.005f, 0.30f);
        }
        if (window.isKeyDown(GLFW_KEY_MINUS)) {
            outlineWidth = std::max(outlineWidth - 0.005f, 0.0f);
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
        clearStencil->setExtent(extent);

        if (renderPass->attachmentGeneration() != attachmentGeneration) {
            attachmentGeneration = renderPass->attachmentGeneration();
            std::cout << "[stencil] attachments rebuilt; depth-stencil extent " << extent.width << "x"
                      << extent.height << std::endl;
        }
        if (dumpOnce && !dumpDone && !dumpPending && renderedFrames >= dumpAfter) {
            dumpPending = true;
        }

        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);
        StencilOutlineUbo ubo{};
        ubo.view = cameraMatrix.view;
        ubo.proj = cameraMatrix.proj;
        ubo.model = model;
        ubo.lightDir = glm::vec4(0.45f, 0.8f, 0.55f, 0.0f);
        ubo.params = glm::vec4(outlineWidth, 0.0f, 0.0f, 0.0f);
        toonDraw->OnUpdateRawData(ResourceId(0, 0), frame, ubo);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(renderPass, [&]() {
            sequence->record(toonDraw);
            if (clearMid) {
                sequence->record(clearStencil);
            }
            sequence->record(outlineDraw);
        });
        sequence->endRecording();
        sequence->submitFrame();

        if (dumpPending) {
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                   mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                   *mgr->getQueueFamilyIndices().graphicsFamily);

            // (1) the presented frame, so the outline can be counted by colour
            std::vector<DumpRequest> requests;
            const std::vector<vk::Image> &swapImages = mgr->getSwapChain()->getImages();
            const uint32_t imageIndex = sequence->getImageIndex();
            if (imageIndex >= swapImages.size()) {
                throw std::runtime_error("stencil: swapchain image index out of range");
            }
            DumpRequest frameRequest;
            frameRequest.label = "stencil-frame";
            frameRequest.image = swapImages[imageIndex];
            frameRequest.format = *mgr->getSwapChain()->getImageFormat();
            frameRequest.extent = extent;
            frameRequest.layout = vk::ImageLayout::ePresentSrcKHR;
            frameRequest.encoding = DumpEncoding::eColor;
            requests.push_back(frameRequest);
            const auto results = DumpImages(readback, requests, "dump", 0);
            for (const auto &result : results) {
                std::cout << "[dump] " << result.path.string() << " coverage=" << result.alphaCoverage
                          << " mean=(" << result.meanColor.r << "," << result.meanColor.g << ","
                          << result.meanColor.b << ")" << std::endl;
            }

            // (2) the S8 plane itself.  Copying a single aspect is mandatory:
            // a DEPTH|STENCIL copy region violates VUID 00188.
            const ImageReadback::Capture stencil = readback.capture(
                renderPass->depthStencilImage(0), vk::Format::eS8Uint, extent,
                vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ImageAspectFlagBits::eStencil,
                vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil);
            const size_t texelCount = static_cast<size_t>(extent.width) * extent.height;
            size_t marked = 0;
            std::vector<uint8_t> maskRgb(texelCount * 3);
            for (size_t i = 0; i < texelCount; ++i) {
                const uint8_t value = stencil.pixels[i];
                marked += value != 0 ? 1u : 0u;
                const uint8_t level = value != 0 ? 255 : 0;
                maskRgb[i * 3 + 0] = level;
                maskRgb[i * 3 + 1] = level;
                maskRgb[i * 3 + 2] = level;
            }
            const std::vector<std::string> maskComments = {
                "render_demo stencil plane (S8)", "white = stencil != 0",
                "fill reference " + std::to_string(fillRef),
                "fill write mask " + std::to_string(fillDynamic->writeMask()),
            };
            WritePpmRgb8("dump/0001_stencil-mask.ppm", extent.width, extent.height, maskRgb, maskComments);
            std::cout << "[stencil] marked texels " << marked << "/" << texelCount << " (fraction "
                      << (texelCount ? static_cast<double>(marked) / texelCount : 0.0) << ")" << std::endl;
            const size_t outlinePixels = countOutlinePixels(results[0].rgb8);
            std::cout << "[stencil] outline pixels " << outlinePixels << " (fraction "
                      << (texelCount ? static_cast<double>(outlinePixels) / texelCount : 0.0) << ")"
                      << std::endl;

            dumpPending = false;
            dumpDone = true;
        }

        if (!sequence->presentFrame()) {
            continue;
        }
        ++renderedFrames;
        if (dumpOnce && dumpDone) {
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
