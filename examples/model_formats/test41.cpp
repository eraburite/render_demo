// model_formats: migration of Saba's Model/MMD + Model/XFile formats into the
// render_demo model-loading framework.
//
// The point of this example is the loading layer, not the rendering: it
// exercises the plugin registry, magic-number dispatch, the post-process
// pipeline and the four new loaders, then draws whatever geometry came back so
// --dump can prove the data really reached the GPU.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ModelBinaryReader.h"
#include "ModelBuilder.h"
#include "ModelFormatsCommon.h"
#include "ModelLoader.h"
#include "ModelLoaderPlugin.h"
#include "ModelPostProcess.h"
#include "RenderCommon.h"
#include "root_directory.h"

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

#include "shader/model_frag.hpp"
#include "shader/model_vert.hpp"

namespace {

constexpr const char *kFormatDir = PROJECT_ROOT_PATH "/assets/models/formats/";

struct Options {
    bool list = false;
    bool selfTest = false;
    bool stats = false;
    bool verify = false;
    bool dump = false;
    std::string probePath;
    std::string format = "pmx";
    std::string modelPath;
    std::string loader;
    uint32_t dumpAfter = 60;
    uint32_t frames = 120;
    uint32_t seed = 7;
};

std::string nextArg(int argc, char **argv, int &i)
{
    if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + argv[i]);
    return argv[++i];
}

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") options.list = true;
        else if (arg == "--self-test") options.selfTest = true;
        else if (arg == "--stats") options.stats = true;
        else if (arg == "--verify") options.verify = true;
        else if (arg == "--dump") options.dump = true;
        else if (arg == "--probe" && i + 1 < argc) options.probePath = nextArg(argc, argv, i);
        else if (arg == "--format" && i + 1 < argc) options.format = nextArg(argc, argv, i);
        else if (arg == "--model" && i + 1 < argc) options.modelPath = nextArg(argc, argv, i);
        else if (arg == "--loader" && i + 1 < argc) options.loader = nextArg(argc, argv, i);
        else if (arg == "--dump-after" && i + 1 < argc) options.dumpAfter = std::stoul(nextArg(argc, argv, i));
        else if (arg == "--frames" && i + 1 < argc) options.frames = std::stoul(nextArg(argc, argv, i));
        else if (arg == "--seed" && i + 1 < argc) options.seed = std::stoul(nextArg(argc, argv, i));
        else throw std::runtime_error("model_formats: unknown argument " + arg);
    }
    return options;
}

std::string defaultModelPath(const std::string &format)
{
    if (format == "pmx") return std::string(kFormatDir) + "sample.pmx";
    if (format == "pmd") return std::string(kFormatDir) + "sample.pmd";
    if (format == "x" || format == "xfile") return std::string(kFormatDir) + "sample.x";
    if (format == "vmd") return std::string(kFormatDir) + "sample.vmd";
    throw std::runtime_error("model_formats: unknown --format '" + format + "' (pmx|pmd|x|vmd)");
}

// ---------------------------------------------------------------------------
// CPU-only self-test.  No Vulkan device, no window, so it can run in CI.
// ---------------------------------------------------------------------------
int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string &what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cout << "[self-test] FAIL: " << what << "\n";
    }
}

std::vector<uint8_t> headOf(const std::string &path, size_t count = 64)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> bytes(count);
    file.read(reinterpret_cast<char *>(bytes.data()), count);
    bytes.resize(static_cast<size_t>(file.gcount()));
    return bytes;
}

class FakeLoader : public IModelLoader {
public:
    const char *name() const override { return "testplug"; }
    std::vector<std::string> extensions() const override { return {"fake"}; }
    Model load(const std::string &, const ModelLoadOptions &) const override
    {
        Model model;
        model.vertices.resize(3);
        model.indices = {0, 1, 2};
        Mesh mesh;
        Primitive primitive;
        primitive.indexCount = 3;
        mesh.primitives.push_back(primitive);
        model.meshes.push_back(mesh);
        model.recomputeBounds();
        return model;
    }
};

bool runSelfTest(const Options &options)
{
    auto &registry = ModelLoaderRegistry::instance();
    (void)options;

    // --- registry / catalog -------------------------------------------------
    const std::vector<std::string> catalog = registry.catalog();
    check(catalog.size() == 7, "seven built-in loaders are registered");
    const std::vector<std::string> extensions = registry.supportedExtensions();
    for (const char *extension : {"pmx", "pmd", "x", "vmd", "obj", "gltf", "glb", "dae", "fbx"}) {
        check(std::find(extensions.begin(), extensions.end(), extension) != extensions.end(),
              std::string("extension .") + extension + " is advertised");
    }
    check(registry.findByName("pmx") != nullptr && registry.findByName("vmd") != nullptr,
          "new loaders are reachable by name");

    // --- Chain of Responsibility: magic probes ------------------------------
    const std::string pmxPath = defaultModelPath("pmx");
    const std::string pmdPath = defaultModelPath("pmd");
    const std::string xPath = defaultModelPath("x");
    const std::string vmdPath = defaultModelPath("vmd");
    const auto pmxHead = headOf(pmxPath), pmdHead = headOf(pmdPath);
    const auto xHead = headOf(xPath), vmdHead = headOf(vmdPath);
    check(registry.findByName("pmx")->probe(pmxHead.data(), pmxHead.size()) >= 100, "PMX magic probe scores 100");
    check(registry.findByName("pmd")->probe(pmdHead.data(), pmdHead.size()) >= 100, "PMD magic probe scores 100");
    check(registry.findByName("xfile")->probe(xHead.data(), xHead.size()) >= 100, "XFile magic probe scores 100");
    check(registry.findByName("vmd")->probe(vmdHead.data(), vmdHead.size()) >= 100, "VMD magic probe scores 100");
    check(registry.findByName("obj")->probe(pmxHead.data(), pmxHead.size()) == 0, "an unrelated loader does not vote");
    check(registry.findByName("pmx")->probe(pmdHead.data(), pmdHead.size()) == 0, "PMX does not claim a PMD file");

    check(registry.resolve(pmxPath).reason.rfind("magic: pmx", 0) == 0, "PMX resolves by magic, not extension");
    check(registry.resolve(pmdPath).reason.rfind("magic: pmd", 0) == 0, "PMD resolves by magic");
    check(registry.resolve(xPath).reason.rfind("magic: xfile", 0) == 0, "XFile resolves by magic");
    check(registry.resolve(vmdPath).reason.rfind("magic: vmd", 0) == 0, "VMD resolves by magic");

    // A wrong/unknown extension must still dispatch by content.
    const std::filesystem::path tempPath = "dump/magic_probe.dat";
    std::filesystem::create_directories("dump");
    {
        std::ofstream out(tempPath, std::ios::binary);
        out.write(reinterpret_cast<const char *>(pmxHead.data()), pmxHead.size());
    }
    const ModelResolveInfo renamed = registry.resolve(tempPath.string());
    check(renamed.loader != nullptr && std::string(renamed.loader->name()) == "pmx",
          "a renamed .dat file still dispatches to PMX by magic");
    std::filesystem::remove(tempPath);

    // Extension fallback still picks the hand-written loader over assimp.
    const ModelResolveInfo obj = registry.resolve(PROJECT_ROOT_PATH "/assets/models/backpack/backpack.obj");
    check(obj.loader != nullptr && std::string(obj.loader->name()) == "obj",
          "extension fallback keeps .obj on the hand-written loader");

    // --- the four formats, byte-for-byte against the synthetic files ---------
    ModelLoadOptions loadOptions;
    loadOptions.log = false;
    const Model pmx = loadModel(pmxPath, loadOptions);
    const Model pmd = loadModel(pmdPath, loadOptions);
    const Model xfile = loadModel(xPath, loadOptions);

    auto checkCube = [](const Model &model, const std::string &label) {
        check(model.vertices.size() == 8, label + ": 8 vertices");
        check(model.indices.size() == 36, label + ": 36 indices");
        check(model.triangleCount() == 12, label + ": 12 triangles");
        check(!model.meshes.empty() && !model.meshes[0].primitives.empty(), label + ": has a primitive");
        bool windowsValid = true;
        uint32_t cursor = 0;
        for (const Mesh &mesh : model.meshes) {
            for (const Primitive &primitive : mesh.primitives) {
                if (primitive.firstIndex != cursor) windowsValid = false;
                cursor += primitive.indexCount;
                if (cursor > model.indices.size()) windowsValid = false;
            }
        }
        check(windowsValid && cursor == model.indices.size(), label + ": primitive index windows are contiguous");
    };
    checkCube(pmx, "pmx");
    checkCube(pmd, "pmd");
    checkCube(xfile, "xfile");

    check(pmx.materials.size() == 1 && pmd.materials.size() == 1, "PMX/PMD: one material each");
    check(pmx.isSkinned() && pmd.isSkinned(), "PMX/PMD: skinning data survives the post-process chain");
    check(pmx.skins.size() == 1 && pmx.skins[0].joints.size() == 2, "PMX: two joints");
    check(pmd.skins.size() == 1 && pmd.skins[0].joints.size() == 2, "PMD: two joints");
    check(pmx.boneWeights.size() == pmx.vertices.size() && pmd.boneWeights.size() == pmd.vertices.size(),
          "skinning arrays stay parallel to vertices");
    check(std::fabs(pmx.boneWeights[7].x - 0.30f) < 1.0e-5f &&
              std::fabs(pmx.boneWeights[7].y - 0.70f) < 1.0e-5f,
          "PMX BDEF2 weight is normalised into (0.3, 0.7)");
    check(pmx.materials[0].doubleSided, "MMD material is double sided after the loader hook");
    check(std::fabs(pmx.materials[0].roughnessFactor - std::sqrt(2.0f / 22.0f)) < 1.0e-4f,
          "Phong shininess 20 becomes roughness sqrt(2/22)");
    check(!xfile.isSkinned(), "XFile geometry is static");
    check(xfile.nodes.size() == 3, "XFile Frame hierarchy becomes three nodes");
    check(std::fabs(xfile.nodes[1].transform[3][1] - 1.0f) < 1.0e-5f,
          "XFile FrameTransformMatrix translation is preserved");

    // --- animation -----------------------------------------------------------
    const AnimationClip clip = loadModelAnimation(vmdPath);
    check(clip.channels.size() == 2, "VMD: two bone channels");
    uint32_t keyCount = 0;
    for (const AnimationChannel &channel : clip.channels) keyCount += channel.positions.size();
    check(keyCount == 3, "VMD: three motion keyframes");
    check(std::fabs(clip.duration - 30.0f) < 1.0e-5f && std::fabs(clip.ticksPerSecond - 30.0f) < 1.0e-5f,
          "VMD: 30 fps / duration 30 ticks");
    AnimationClip bound = clip;
    const uint32_t boundCount = modelformats::bindAnimationChannelsToModel(bound, pmx);
    check(boundCount == 2, "VMD bone names bind to PMX nodes");
    bool animationRejected = false;
    try {
        (void)loadModel(vmdPath, loadOptions);
    } catch (const std::runtime_error &) {
        animationRejected = true;
    }
    check(animationRejected, "loadModel rejects an animation-only format with a readable error");

    // --- pipeline / Builder / reader -----------------------------------------
    const model_post::Pipeline pipeline = model_post::makeDefaultPipeline(*registry.findByName("obj"));
    const std::vector<std::string> steps = pipeline.stepNames();
    check(steps.size() == 10 && steps.front() == "flatten" && steps.back().rfind("loader-hook:", 0) == 0,
          "default pipeline has ten ordered steps ending in the loader hook");
    const size_t before = pmx.vertices.size();
    Model flattened = pmx;
    modelformats::finalizeMaterials(flattened);
    model_post::flattenScene(flattened);
    model_post::deduplicateVertices(flattened);
    check(flattened.vertices.size() == before, "flatten/dedup are no-ops for a skinned model");

    bool builderThrows = false;
    try {
        ModelBuilder builder;
        builder.addVertex(MeshVertex{});
        builder.setJoints({Joint{}});
        builder.addBoneData(glm::uvec4(0), glm::vec4(1.0f));
        builder.addBoneData(glm::uvec4(0), glm::vec4(1.0f)); // deliberately misaligned
        (void)builder.build();
    } catch (const std::runtime_error &) {
        builderThrows = true;
    }
    check(builderThrows, "ModelBuilder rejects misaligned skinning arrays");

    const std::array<uint8_t, 4> shortBuffer = {1, 2, 3, 4};
    modelio::BinaryReader reader(shortBuffer.data(), shortBuffer.size());
    check(reader.u16() == 0x0201 && reader.remaining() == 2, "BinaryReader little-endian read");
    bool readerThrows = false;
    try {
        reader.u32();
    } catch (const std::runtime_error &) {
        readerThrows = true;
    }
    check(readerThrows, "BinaryReader throws on out-of-bounds reads");
    const std::array<uint8_t, 1> sentinel = {0xFF};
    modelio::BinaryReader indexReader(sentinel.data(), sentinel.size());
    check(indexReader.signedIndex(1) == -1, "PMX all-ones index decodes to -1");
    const std::array<uint8_t, 6> utf16 = {'A', 0, 'B', 0, 0, 0};
    check(modelio::utf16LeToUtf8(utf16.data(), utf16.size()) == "AB", "UTF-16LE PMX string decodes");

    // --- plugin self-registration + preferredLoader override -----------------
    const size_t pluginsBefore = registry.catalog().size();
    {
        ModelLoaderRegistrar registrar("testplug", {"fake"},
                                       []() -> std::unique_ptr<IModelLoader> { return std::make_unique<FakeLoader>(); },
                                       99, false);
        check(registry.catalog().size() == pluginsBefore + 1,
              "ModelLoaderRegistrar registers a plugin from an unrelated translation unit");
        check(registry.findByName("testplug") != nullptr, "the self-registered plugin is findable");
        ModelLoadOptions forced;
        forced.preferredLoader = "testplug";
        forced.deduplicate = Toggle::eOff; // the fake vertices are identical; keep all three
        forced.log = false;
        const Model forcedModel = loadModel(pmxPath, forced);
        check(forcedModel.vertices.size() == 3, "--loader style override bypasses magic dispatch");
    }

    bool unknownRejected = false;
    try {
        (void)loadModel("no_such_file.zzz", loadOptions);
    } catch (const std::runtime_error &e) {
        unknownRejected = std::string(e.what()).find("no loader for") != std::string::npos;
    }
    check(unknownRejected, "an unknown extension produces a readable 'no loader' error");

    std::cout << "[self-test] " << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}

void printCatalog()
{
    auto &registry = ModelLoaderRegistry::instance();
    std::cout << "[catalog] " << registry.catalog().size() << " registered loaders\n";
    for (const std::string &line : registry.catalog()) std::cout << "  " << line << "\n";
    std::cout << "[catalog] extensions:";
    for (const std::string &extension : registry.supportedExtensions()) std::cout << " ." << extension;
    std::cout << "\n";
    const model_post::Pipeline pipeline = model_post::makeDefaultPipeline(*registry.findByName("obj"));
    std::cout << "[pipeline]";
    for (const std::string &name : pipeline.stepNames()) std::cout << " " << name;
    std::cout << "\n";
}

void printProbe(const std::string &path)
{
    const ModelResolveInfo info = ModelLoaderRegistry::instance().resolve(path);
    std::cout << "[probe] " << path << "\n";
    for (const std::string &line : info.trace) std::cout << "  " << line << "\n";
    std::cout << "[probe] winner=" << (info.loader ? info.loader->name() : "none")
              << " reason=" << (info.reason.empty() ? "none" : info.reason) << "\n";
}

void printStats(const Options &options)
{
    const std::string path = options.modelPath.empty() ? defaultModelPath(options.format) : options.modelPath;
    if (options.format == "vmd") {
        const AnimationClip clip = loadModelAnimation(path);
        uint32_t keys = 0;
        for (const AnimationChannel &channel : clip.channels) keys += channel.positions.size();
        std::cout << "[stats] format=vmd channels=" << clip.channels.size() << " keys=" << keys
                  << " durationTicks=" << clip.duration << " ticksPerSecond=" << clip.ticksPerSecond << "\n";
        return;
    }
    ModelLoadOptions loadOptions;
    loadOptions.preferredLoader = options.loader;
    const Model model = loadModel(path, loadOptions);
    std::cout << "[stats] format=" << options.format << " vertices=" << model.vertices.size()
              << " indices=" << model.indices.size() << " triangles=" << model.triangleCount()
              << " meshes=" << model.meshes.size() << " materials=" << model.materials.size()
              << " images=" << model.images.size() << " nodes=" << model.nodes.size()
              << " skins=" << model.skins.size() << " skinned=" << (model.isSkinned() ? "yes" : "no")
              << " bounds=(" << model.minBounds.x << "," << model.minBounds.y << "," << model.minBounds.z
              << ")..(" << model.maxBounds.x << "," << model.maxBounds.y << "," << model.maxBounds.z << ")\n";
}

// ---------------------------------------------------------------------------
// GPU path: upload whatever the loader produced and dump one frame.
// ---------------------------------------------------------------------------
void render(const Options &options)
{
    if (options.format == "vmd") {
        throw std::runtime_error("model_formats: VMD has no geometry to render; use --stats or --self-test");
    }
    const std::string path = options.modelPath.empty() ? defaultModelPath(options.format) : options.modelPath;
    ModelLoadOptions loadOptions;
    loadOptions.preferredLoader = options.loader;
    Model model = loadModel(path, loadOptions);
    if (!model.valid()) throw std::runtime_error("model_formats: loaded model is empty");

    // MeshVertex -> the framework's interleaved Vertex layout.
    std::vector<Vertex> vertices;
    vertices.reserve(model.vertices.size());
    for (const MeshVertex &source : model.vertices) {
        Vertex vertex;
        vertex.position = source.position;
        vertex.normal = source.normal;
        vertex.texCoord = source.texCoord;
        vertex.tangent = source.tangent;
        // A per-format tint makes --format visible in the dump.
        const glm::vec3 tint = options.format == "pmd"  ? glm::vec3(0.45f, 0.75f, 1.0f)
                               : options.format == "x" || options.format == "xfile"
                                   ? glm::vec3(0.55f, 1.0f, 0.55f)
                                   : glm::vec3(1.0f, 0.72f, 0.45f);
        vertex.color = source.color * tint;
        vertices.push_back(vertex);
    }

    // Presentation-only fit: centre and scale the uploaded copy so a 20-unit
    // MMD character and a 2-unit test cube both fill the same view.  The loaded
    // Model (and therefore the loader contract) is left untouched; a real
    // renderer would apply the node world matrices instead.
    const glm::vec3 center = (model.minBounds + model.maxBounds) * 0.5f;
    const glm::vec3 extent = model.maxBounds - model.minBounds;
    const float longest = std::max({extent.x, extent.y, extent.z, 0.0001f});
    const float fit = 2.0f / longest;
    for (Vertex &vertex : vertices) {
        vertex.position = (vertex.position - center) * fit;
    }

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 6.0f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);
    const vk::Format swapFormat = *mgr->getSwapChain()->getImageFormat();

    auto layout = std::make_shared<DescriptorLayoutAtlas>(MAX_FRAMES_IN_FLIGHT);
    auto cameraSet = DescriptorLayoutAtlas::makeSet<EnvironmentComponent, EnvironmentComponentTable>(
        0, {EnvironmentComponent::eCameraMatrix}, UpdateFrequency::eEachFrame, false);
    cameraSet.specs[0] = GBufferSpec{1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom};
    layout->setDetails({cameraSet});

    auto vertexInput = std::make_shared<VertexInputAtlas>(
        0,
        std::vector<VertexComponent>{VertexComponent::ePosition, VertexComponent::eNormal,
                                     VertexComponent::eUV, VertexComponent::eColor},
        static_cast<uint32_t>(vertices.size()), static_cast<uint32_t>(model.indices.size()),
        DataTypes::eUnsignedInt);

    auto asset = mgr->CreateShaderAssetGroup(vertexInput, layout);
    asset->addShaderSpirvCode("vert", std::vector<uint32_t>(shader::MODEL_VERT_SPV.begin(),
                                                            shader::MODEL_VERT_SPV.end()));
    asset->addShaderSpirvCode("frag", std::vector<uint32_t>(shader::MODEL_FRAG_SPV.begin(),
                                                            shader::MODEL_FRAG_SPV.end()));

    auto renderPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, swapFormat, mgr->DefaultDepthFormat()));
    // GraphicsPipelineConfig defaults to cullMode = eNone, which is what MMD's
    // double-sided / mixed-winding geometry needs.
    auto pipeline = mgr->CreatePipeline(vertexInput, layout, asset, renderPass, 0);

    {
        auto upload = mgr->UploadSequence();
        auto sync = std::make_shared<CmdSyncDevice>(asset);
        sync->OnUpdateRawData(ResourceTypes::eVertex, vertices);
        sync->OnUpdateRawData(ResourceTypes::eIndex, model.indices);
        upload->submit(sync, asset);
    }

    if (options.verify) {
        auto probe = mgr->UploadSequence();
        const auto vb = probe->ReadBack(asset, ResourceTypes::eVertex, vertices.size() * sizeof(Vertex));
        const auto ib = probe->ReadBack(asset, ResourceTypes::eIndex, model.indices.size() * sizeof(uint32_t));
        const bool verticesMatch = vb.size() >= vertices.size() * sizeof(Vertex) &&
                                   std::memcmp(vb.data(), vertices.data(), vertices.size() * sizeof(Vertex)) == 0;
        const bool indicesMatch = ib.size() >= model.indices.size() * sizeof(uint32_t) &&
                                  std::memcmp(ib.data(), model.indices.data(),
                                              model.indices.size() * sizeof(uint32_t)) == 0;
        std::cout << "[verify] VBO " << vertices.size() << " x " << sizeof(Vertex)
                  << " B equals CPU: " << (verticesMatch ? "yes" : "NO") << "; IBO " << model.indices.size()
                  << " x 4 B equals CPU: " << (indicesMatch ? "yes" : "NO") << "\n";
        if (!verticesMatch || !indicesMatch) {
            throw std::runtime_error("model_formats: GPU buffers do not match the loaded model");
        }
    }

    auto sequence = mgr->DrawFrameSequence();
    auto draw = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto frameSync = std::make_shared<CmdSyncDevice>(asset);
    uint32_t framesRendered = 0;
    bool dumpDone = false;

    while (!window.shouldClose()) {
        window.poolEvents();
        float currentTime = 0.0f;
        window.processInputEvents(currentTime);
        if (!sequence->prepareFrame()) continue;

        const uint32_t frame = sequence->getFrameInFlight();
        const vk::Extent2D extent = *mgr->getSwapChain()->getExtent2D();
        const CameraMatrix cameraMatrix = MakeCameraMatrix(*camera, extent);
        draw->OnUpdateRawData(ResourceId(0, 0), frame, cameraMatrix);

        sequence->beginRecording();
        sequence->record(frameSync);
        sequence->recordPass(renderPass, [&]() { sequence->record(draw); });
        sequence->endRecording();
        sequence->submitFrame();
        ++framesRendered;

        if (options.dump && !dumpDone && framesRendered >= options.dumpAfter) {
            auto readback = ImageReadback(mgr->getLogicalDevice(), mgr->memoryProperties(),
                                          mgr->getMemoryAllocator(), mgr->getGraphicsQueue(),
                                          *mgr->getQueueFamilyIndices().graphicsFamily);
            DumpRequest request;
            request.label = "frame-" + options.format;
            request.image = mgr->getSwapChain()->getImages()[sequence->getImageIndex()];
            request.format = swapFormat;
            request.extent = extent;
            request.layout = vk::ImageLayout::ePresentSrcKHR;
            request.encoding = DumpEncoding::eColor;
            for (const DumpResult &result : DumpImages(readback, {request}, "dump", 0)) {
                std::cout << "[dump] " << result.path.string() << " mean=(" << result.meanColor.r << ","
                          << result.meanColor.g << "," << result.meanColor.b << ")\n";
            }
            dumpDone = true;
            if (!sequence->presentFrame()) continue;
            break;
        }
        if (!sequence->presentFrame()) continue;
        if (framesRendered >= options.frames) break;
    }

    if (options.stats) {
        std::cout << "[stats] frames=" << framesRendered << " format=" << options.format
                  << " vertices=" << vertices.size() << " indices=" << model.indices.size()
                  << " seed=" << options.seed << "\n";
    }
}

} // namespace

int main(int argc, char **argv)
{
    try {
        const Options options = parseOptions(argc, argv);
        if (options.selfTest) return runSelfTest(options);
        if (options.list) { printCatalog(); return 0; }
        if (!options.probePath.empty()) { printProbe(options.probePath); return 0; }
        if (options.stats && !options.dump && !options.verify) { printStats(options); return 0; }
        render(options);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "model_formats: " << e.what() << "\n";
        return 1;
    }
}
