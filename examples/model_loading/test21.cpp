// model_loading: draw a model read off disk, textured, no lighting.
// Reference: D:/LearnOpenGL/src/3.model_loading/1.model_loading (model_loading.cpp).
//
// What the port keeps: one dark-cleared, depth-tested pass; a single uniform
// model matrix (the tutorial's `scale(model, 0.25)`); a fragment shader that
// samples one diffuse map and writes it out; a free-flying camera with mouse
// look, WASD and scroll zoom.
//
// What it changes, all of it forced by the engine rather than chosen:
//   * the transforms are one std140 block in set 1 instead of three loose
//     uniforms, because the framework's descriptor layout is the only way data
//     reaches a shader;
//   * the scene is flattened (ModelLoadOptions::flattenScene).  The tutorial
//     walks assimp's node tree but drops every aiNode::mTransformation, which
//     silently flattens hierarchical files; the loader instead bakes each node's
//     world matrix into the vertices.  Backpack's 79 nodes are identity, so its
//     output is unaffected -- anything with real node transforms now lands in the
//     right place, and the bounds used for framing describe what is drawn;
//   * materials own a pipeline.  The diffuse map lives in a static descriptor
//     set (set 0), so two materials cannot share one; a batch is "all primitives
//     that share a material", and DrawRange keeps that batch to a single
//     pipeline bind and one draw per primitive window.
//
// Loading is format-agnostic: --loader assimp forces the assimp importer on the
// same .obj the tiny_obj loader reads by default, which is how the two paths get
// compared (see DESIGN.md §19).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ModelLoader.h"
#include "RenderCommon.h"

#include "AttachmentDetailConfig.h"
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
#include "root_directory.h"

#include "shader/model_frag.hpp"
#include "shader/model_vert.hpp"

namespace {

// Scale comes from the loader, not from this file.
//
// The tutorial hard-codes `glm::scale(model, glm::vec3(0.25f))`, which is only
// right for the one asset it ships with.  This example takes --model, and real
// files differ by orders of magnitude (the LearnOpenGL backpack is ~4 units tall,
// its vampire .dae is 210 because the nodes carry a scale), so the load below asks
// for ModelLoadOptions::normalizeSize: centred at the origin, longest axis 2 units.
// Everything downstream -- the framing maths, and the fixed near/far inside
// RenderCommon::MakeCameraMatrix -- then works for any input.

struct ModelViewBlock {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::mat4 model;
};
static_assert(sizeof(ModelViewBlock) == 192, "std140 block must be 3 mat4");

struct Options {
    std::string modelPath = PROJECT_ROOT_PATH "/data/models/backpack/backpack.obj";
    std::string loader; // "" = extension dispatch
    bool dump = false;
    uint32_t dumpAfter = 0;
    uint32_t resizeAfter = 0;
    float yawDegrees = 0.0f;
};

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--model" && i + 1 < argc) {
            options.modelPath = argv[++i];
        } else if (arg == "--loader" && i + 1 < argc) {
            options.loader = argv[++i];
        } else if (arg == "--yaw" && i + 1 < argc) {
            options.yawDegrees = std::stof(argv[++i]);
        } else if (arg == "--dump-after" && i + 1 < argc) {
            options.dumpAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--resize-after" && i + 1 < argc) {
            options.resizeAfter = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
    }
    return options;
}

// Camera placement derived from the model's bounds.
//
// The reference hard-codes eye (0, 3, 5) looking straight down -Z, which only
// works because it also hard-codes one asset: with backpack's own bounds that eye
// puts the model's centre 2.9 units below the view axis at ~5 units distance, i.e.
// outside a 45 degree frustum.  Since this example takes --model, it frames
// whatever it loaded instead: the eye sits on the model's vertical axis offset by
// a fixed fraction of its radius, at the distance where the bounding sphere just
// fits the vertical field of view.
struct CameraFraming {
    glm::vec3 eye{0.0f, 0.0f, 0.0f};
    float pitchDegrees = 0.0f;
    float distance = 0.0f;
};

CameraFraming frameModel(const Model &model)
{
    const glm::vec3 low = model.minBounds;
    const glm::vec3 high = model.maxBounds;
    const glm::vec3 size = high - low;
    const glm::vec3 center = (low + high) * 0.5f;

    // Half of the longest axis, not half of the 3D diagonal: a box diagonal is
    // up to sqrt(3) times a single side, and fitting that sphere leaves the
    // object filling barely half the viewport.
    const float radius = std::max(0.001f, 0.5f * std::max({size.x, size.y, size.z}));

    // Sphere-fit distance; 0.85 of the half-angle leaves a margin so nothing
    // touches the frame edge.  The half depth is added on top because the fit
    // has to hold at the *nearest* surface, not at the centre -- without it the
    // bottom of a deep object gets clipped.
    constexpr float kVerticalFov = 45.0f;
    constexpr float kFillFraction = 0.85f;
    const float halfFov = glm::radians(kVerticalFov * 0.5f);
    const float distance = radius / (kFillFraction * std::sin(halfFov)) + 0.5f * size.z;

    CameraFraming framing;
    framing.distance = distance;
    // Eye on the model's vertical axis, a little above the centre, so the object
    // is seen slightly from above like the reference's (0, 3, 5) eye.
    framing.eye = center + glm::vec3(0.0f, 0.45f * radius, distance);
    // Yaw -90 looks down -Z; the pitch aims the eye at the centre and comes out
    // negative because the eye sits above it.
    framing.pitchDegrees = glm::degrees(std::atan2(center.y - framing.eye.y, distance));
    return framing;
}

// The engine's vertex layout, filled from a loader-neutral MeshVertex after the
// node's world matrix is applied.
struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    // material index -> index windows of every (node, primitive) using it.
    std::map<int, std::vector<DrawRange>> batches;
    uint32_t primitiveCount = 0;
};

Geometry buildGeometry(const Model &model)
{
    Geometry geometry;
    // The load below asks for ModelLoaderOptions::flattenScene, so the model
    // arrives as world-space geometry with identity nodes and this only has to
    // re-pack each primitive into the engine's interleaved Vertex and remember
    // its index window.  Without the flatten step the same loop would have to
    // apply node matrices, and -- more importantly -- the bounds that decide the
    // framing would not describe what is drawn (see DESIGN.md §19.4).
    //
    // Re-packing per primitive rather than expanding the whole model into one
    // triangle stream is what keeps the index windows, and therefore the material
    // split, intact.
    for (const Mesh &mesh : model.meshes) {
        for (const Primitive &primitive : mesh.primitives) {
            const uint32_t firstIndex = static_cast<uint32_t>(geometry.indices.size());
            const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                    static_cast<uint32_t>(model.indices.size()));

            std::unordered_map<uint32_t, uint32_t> remap;
            for (uint32_t i = primitive.firstIndex; i < end; ++i) {
                const uint32_t source = model.indices[i];
                if (source >= model.vertices.size()) {
                    continue;
                }
                const auto existing = remap.find(source);
                if (existing != remap.end()) {
                    geometry.indices.push_back(existing->second);
                    continue;
                }
                const MeshVertex &src = model.vertices[source];
                Vertex vertex;
                vertex.position = src.position;
                vertex.normal = src.normal;
                vertex.texCoord = src.texCoord;
                vertex.tangent = src.tangent;
                vertex.color = src.color;
                const uint32_t index = static_cast<uint32_t>(geometry.vertices.size());
                geometry.vertices.push_back(vertex);
                remap.emplace(source, index);
                geometry.indices.push_back(index);
            }

            const uint32_t indexCount = static_cast<uint32_t>(geometry.indices.size()) - firstIndex;
            if (indexCount == 0) {
                continue;
            }
            DrawRange range;
            range.firstIndex = firstIndex;
            range.indexCount = indexCount;
            geometry.batches[primitive.materialIndex].push_back(range);
            ++geometry.primitiveCount;
        }
    }
    return geometry;
}

// One sampleable image per material: the diffuse map, or a 1x1 white stand-in so
// a material without one still has something bound (the tutorial would sample an
// unbound GL texture, which reads as black).
struct MaterialTexture {
    TextureImageSpec spec;
    std::vector<uint8_t> pixels;
    std::string name;
};

MaterialTexture makeTexture(const Model &model, int materialIndex)
{
    const Material *material = (materialIndex >= 0 && materialIndex < static_cast<int>(model.materials.size()))
                                   ? &model.materials[materialIndex]
                                   : nullptr;
    const int imageIndex = material ? material->baseColorTexture : -1;
    if (imageIndex >= 0 && imageIndex < static_cast<int>(model.images.size())) {
        const TextureImage &image = model.images[imageIndex];
        if (image.width > 0 && image.height > 0 && !image.pixels.empty()) {
            MaterialTexture texture;
            texture.spec.width = static_cast<uint32_t>(image.width);
            texture.spec.height = static_cast<uint32_t>(image.height);
            texture.spec.channels = 4;
            texture.spec.miplevels = std::max(1u, image.mipLevels);
            // sRGB: the swapchain encodes on write, so a linear-format upload
            // would come out washed out.  The GPU blits the mip chain
            // (mipsInData = false), matching the tutorial's glGenerateMipmap.
            texture.spec.format = vk::Format::eR8G8B8A8Srgb;
            texture.spec.mipsInData = false;
            texture.spec.addressMode = vk::SamplerAddressMode::eRepeat; // GL_REPEAT
            texture.pixels = image.pixels;
            texture.name = image.name;
            return texture;
        }
    }

    MaterialTexture fallback;
    fallback.spec.width = 1;
    fallback.spec.height = 1;
    fallback.spec.channels = 4;
    fallback.spec.miplevels = 1;
    fallback.spec.format = vk::Format::eR8G8B8A8Srgb;
    fallback.spec.mipsInData = false;
    fallback.pixels = {255, 255, 255, 255};
    fallback.name = "(none)";
    return fallback;
}

std::shared_ptr<DescriptorLayoutAtlas> makeLayout(const TextureImageSpec &textureSpec)
{
    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);

    auto globalSet = DescriptorLayoutAtlas::makeSet<GlobalComponent, GlobalComponentTable>(
        0, {GlobalComponent::ebaseColor}, UpdateFrequency::eStatic, true);
    globalSet.specs[0] = textureSpec;

    auto environmentSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        1, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    environmentSet.specs[0] =
        GBufferSpec{1, static_cast<uint32_t>(sizeof(ModelViewBlock)), DataTypes::eCustom};

    layout->setDetails({globalSet, environmentSet});
    return layout;
}

// Coverage from the dump itself: the pass clears every texel, so the most
// frequent 8-bit colour *is* the background, and everything else is geometry.
// Deriving it instead of hard-coding the clear value keeps the measurement valid
// on an sRGB swapchain, where the stored bytes are the encoded form of the clear
// colour (0.05 -> 63, not 13).
//
// Unique-colour count is the cheap proof that the diffuse map was really
// sampled: a flat or single-colour surface lands in single digits, a photographic
// map in the hundreds or thousands.
void analyseFrame(const std::vector<uint8_t> &rgb, float &coverage, size_t &uniqueColors)
{
    std::unordered_map<uint32_t, uint32_t> histogram;
    histogram.reserve(4096);
    const size_t texels = rgb.size() / 3;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const uint32_t key = (static_cast<uint32_t>(rgb[i]) << 16) | (static_cast<uint32_t>(rgb[i + 1]) << 8)
                             | static_cast<uint32_t>(rgb[i + 2]);
        ++histogram[key];
    }

    uint32_t backgroundCount = 0;
    uniqueColors = 0;
    for (const auto &[key, count] : histogram) {
        (void)key;
        backgroundCount = std::max(backgroundCount, count);
        // 16 texels filters out single-pixel noise at geometry edges.
        if (count >= 16) {
            ++uniqueColors;
        }
    }
    coverage = texels > 0 ? 1.0f - static_cast<float>(backgroundCount) / static_cast<float>(texels) : 0.0f;
}

} // namespace

void test(int argc, char **argv)
{
    const Options options = parseOptions(argc, argv);

    // ----- Load ------------------------------------------------------------
    // Before the window: the camera is framed from the loaded bounds, and the
    // loader needs nothing but the file path.
    ModelLoadOptions loadOptions;
    loadOptions.preferredLoader = options.loader;
    // See the comment above: the loader (not this example) owns the scale, and it
    // has to flatten the scene first for the scale to mean anything.
    loadOptions.flattenScene = true;
    loadOptions.normalizeSize = true;
    const Model model = loadModel(options.modelPath, loadOptions);
    if (!model.valid()) {
        throw std::runtime_error("model_loading: no triangle geometry in " + options.modelPath);
    }

    const Geometry geometry = buildGeometry(model);
    if (geometry.batches.empty()) {
        throw std::runtime_error("model_loading: model produced no drawable primitives");
    }
    const CameraFraming framing = frameModel(model);

    auto camera =
        std::make_shared<Camera>(framing.eye, glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, framing.pitchDegrees);
    GLFWwindow window(1280, 720, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    std::cout << "[model_loading] " << options.modelPath << ": " << geometry.vertices.size() << " vertices, "
              << geometry.indices.size() << " indices (" << geometry.indices.size() / 3 << " triangles), "
              << geometry.primitiveCount << " primitive(s) in " << geometry.batches.size() << " material batch(es), "
              << model.nodes.size() << " node(s), " << model.images.size() << " image(s)" << std::endl;
    std::cout << "[model_loading] bounds " << model.minBounds.x << "," << model.minBounds.y << ","
              << model.minBounds.z << " .. " << model.maxBounds.x << "," << model.maxBounds.y << ","
              << model.maxBounds.z << std::endl;
    std::cout << "[model_loading] framed at eye (" << framing.eye.x << "," << framing.eye.y << ","
              << framing.eye.z << ") pitch " << framing.pitchDegrees << " deg, distance " << framing.distance
              << std::endl;

    // ----- Render pass -----------------------------------------------------
    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(),
        mgr->DefaultDepthFormat()));

    // ----- One pipeline per material ---------------------------------------
    // Culling stays off: the reference never enables GL_CULL_FACE, and an OBJ
    // importer has no winding guarantee to cull against.
    GraphicsPipelineConfig pipelineConfig;
    pipelineConfig.depthCompareOp = vk::CompareOp::eLessOrEqual;
    pipelineConfig.cullMode = vk::CullModeFlagBits::eNone;

    auto upload = mgr->UploadSequence();
    std::vector<std::shared_ptr<CmdOpDrawFrame>> draws;
    std::vector<std::shared_ptr<CmdSyncDevice>> frameSyncs;

    for (const auto &[materialIndex, ranges] : geometry.batches) {
        const MaterialTexture texture = makeTexture(model, materialIndex);
        const std::string materialName =
            (materialIndex >= 0 && materialIndex < static_cast<int>(model.materials.size()))
                ? model.materials[materialIndex].name
                : std::string("(no material)");

        auto layout = makeLayout(texture.spec);
        auto asset = mgr->CreateShaderAssetGroup(layout);
        asset->addShaderSpirvCode("vert",
                                  std::vector<uint32_t>(shader::MODEL_VERT_SPV.begin(), shader::MODEL_VERT_SPV.end()));
        asset->addShaderSpirvCode("frag",
                                  std::vector<uint32_t>(shader::MODEL_FRAG_SPV.begin(), shader::MODEL_FRAG_SPV.end()));

        auto vertexInput = std::make_shared<VertexInputAtlas>(
            0, std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eUV},
            static_cast<uint32_t>(geometry.vertices.size()), static_cast<uint32_t>(geometry.indices.size()),
            DataTypes::eUnsignedInt);
        auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0, pipelineConfig);

        // Each material owns its asset, and CreatePipeline gives every asset its
        // own vertex/index buffer, so the geometry is uploaded once per batch.
        // With one material that is one upload; a many-material file trades
        // memory for the per-material descriptor set the engine is built around.
        auto sync = std::make_shared<CmdSyncDevice>(asset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, geometry.vertices);
        sync->OnUpdateRawData(ResourceTypes::eIndex, geometry.indices);
        sync->OnUpdateRawData(ResourceId(0, 0), 0u, texture.pixels);
        upload->submit(sync, asset);
        // The same op is re-recorded every frame: it issues the staging copies
        // for whatever got dirty (the per-frame UBO), and skips everything that
        // did not (geometry, texture).
        frameSyncs.push_back(sync);

        auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);
        draw->setDrawRanges(ranges);
        draws.push_back(draw);

        std::cout << "[model_loading] material " << materialIndex << " '" << materialName << "': "
                  << ranges.size() << " primitive window(s), diffuse map '" << texture.name << "' ("
                  << texture.spec.width << "x" << texture.spec.height << ", " << texture.spec.miplevels << " mips)"
                  << std::endl;
    }

    // ----- Frame loop ------------------------------------------------------
    auto sequence = mgr->DrawFrameSequence();

    // Identity apart from the optional inspection yaw: the model already arrives
    // centred and scaled by the loader.
    const glm::mat4 modelMatrix =
        glm::rotate(glm::mat4(1.0f), glm::radians(options.yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));

    std::cout << "[model_loading] " << draws.size() << " draw(s) per frame, yaw " << options.yawDegrees << " deg"
              << std::endl;


    bool dumpPending = false;
    bool dumpDone = false;
    uint32_t renderedFrames = 0;
    bool resizeRequested = false;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);

        if (options.resizeAfter != 0 && !resizeRequested && renderedFrames >= options.resizeAfter) {
            resizeRequested = true;
            window.resize(1024, 600);
            std::cout << "[model_loading] requested resize to 1024x600" << std::endl;
        }

        if (!sequence->prepareFrame()) {
            continue;
        }
        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();

        if (options.dump && !dumpDone && !dumpPending && renderedFrames >= options.dumpAfter) {
            dumpPending = true;
        }

        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);
        ModelViewBlock block{};
        block.view = cameraMatrix.view;
        block.proj = cameraMatrix.proj;
        block.model = modelMatrix;
        for (const auto &draw : draws) {
            draw->OnUpdateRawData(ResourceId(1, 0), frame, block);
        }

        sequence->beginRecording();
        for (const auto &sync : frameSyncs) {
            sequence->record(sync);
        }
        sequence->recordPass(renderPass, [&]() {
            for (const auto &draw : draws) {
                sequence->record(draw);
            }
        });
        sequence->endRecording();
        sequence->submitFrame();

        if (dumpPending) {
            ImageReadback readback(mgr->getLogicalDevice(), mgr->memoryProperties(), mgr->getMemoryAllocator(),
                                   mgr->getGraphicsQueue(), *mgr->getQueueFamilyIndices().graphicsFamily);

            const std::vector<vk::Image> &swapImages = mgr->getSwapChain()->getImages();
            const uint32_t imageIndex = sequence->getImageIndex();
            if (imageIndex >= swapImages.size()) {
                throw std::runtime_error("model_loading: swapchain image index out of range");
            }
            DumpRequest request;
            request.label = "model";
            request.image = swapImages[imageIndex];
            request.format = *mgr->getSwapChain()->getImageFormat();
            request.extent = extent;
            request.layout = vk::ImageLayout::ePresentSrcKHR;
            request.encoding = DumpEncoding::eColor;
            const auto results = DumpImages(readback, {request}, "dump", 0);

            float coverage = 0.0f;
            size_t uniqueColors = 0;
            analyseFrame(results[0].rgb8, coverage, uniqueColors);
            std::cout << "[dump] " << results[0].path.string() << " coverage=" << coverage
                      << " uniqueColors=" << uniqueColors << " mean=(" << results[0].meanColor.r << ","
                      << results[0].meanColor.g << "," << results[0].meanColor.b << ")" << std::endl;

            dumpPending = false;
            dumpDone = true;
        }

        if (!sequence->presentFrame()) {
            continue;
        }
        ++renderedFrames;
        if (options.dump && dumpDone) {
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
