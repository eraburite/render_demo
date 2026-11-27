#include "ModelLoader.h"

#include "ModelPostProcess.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t kProbeBytes = 64;

std::string lowerExtension(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::vector<uint8_t> readHeader(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::array<char, kProbeBytes> buffer{};
    file.read(buffer.data(), buffer.size());
    const auto got = file.gcount();
    if (got <= 0) return {};
    std::vector<uint8_t> header(static_cast<size_t>(got));
    for (size_t i = 0; i < header.size(); ++i) header[i] = static_cast<uint8_t>(buffer[i]);
    return header;
}

PostProcessPlan mergePlan(const IModelLoader &loader, const ModelLoadOptions &options, const std::string &path)
{
    PostProcessPlan plan = loader.defaultPlan(path);
    if (options.deduplicate != Toggle::eAuto) plan.deduplicate = options.deduplicate == Toggle::eOn;
    if (options.flipTexCoordV != Toggle::eAuto) plan.flipTexCoordV = options.flipTexCoordV == Toggle::eOn;
    if (options.generateNormals != Toggle::eAuto) plan.generateNormals = options.generateNormals == Toggle::eOn;
    if (options.generateTangents != Toggle::eAuto) plan.generateTangents = options.generateTangents == Toggle::eOn;
    return plan;
}

std::string join(const std::vector<std::string> &items, const std::string &separator)
{
    std::string result;
    for (const std::string &item : items) {
        if (!result.empty()) result += separator;
        result += item;
    }
    return result;
}

} // namespace

// --- IModelLoader default animation behaviour -------------------------------

AnimationClip IModelLoader::loadAnimation(const std::string &path) const
{
    throw std::runtime_error(std::string("ModelLoader: loader '") + name() +
                             "' does not provide animation loading (" + path + ")");
}

// --- Registry ---------------------------------------------------------------

const IModelLoader *ModelLoaderRegistry::Entry::get() const
{
    if (!instance && descriptor.factory) instance = descriptor.factory();
    return instance.get();
}

ModelLoaderRegistry &ModelLoaderRegistry::instance()
{
    static ModelLoaderRegistry registry;
    return registry;
}

ModelLoaderRegistry::ModelLoaderRegistry()
{
    // The core registry has no knowledge of any concrete format.  Built-ins are
    // installed by a separate translation unit that acts as the link anchor for
    // every loader object file in the static library.
    registerBuiltinModelLoaders(*this);
}

void ModelLoaderRegistry::registerLoader(ModelLoaderDescriptor descriptor)
{
    if (descriptor.name.empty() || !descriptor.factory) return;
    Entry entry;
    entry.descriptor = std::move(descriptor);
    m_loaders.push_back(std::move(entry));
}

void ModelLoaderRegistry::registerLoader(std::unique_ptr<IModelLoader> loader)
{
    if (!loader) return;
    ModelLoaderDescriptor descriptor;
    descriptor.name = loader->name();
    descriptor.extensions = loader->extensions();
    descriptor.priority = 0;
    Entry entry;
    entry.descriptor = std::move(descriptor);
    entry.instance = std::move(loader); // pre-instantiated: no factory needed
    m_loaders.push_back(std::move(entry));
}

ModelResolveInfo ModelLoaderRegistry::resolve(const std::string &path) const
{
    ModelResolveInfo info;

    // 1) Chain of Responsibility: every loader inspects the same header and
    //    votes.  A magic number (>= 100) outranks any file name, which makes a
    //    renamed / extension-less / mislabelled file resolve correctly.
    const std::vector<uint8_t> header = readHeader(path);
    if (!header.empty()) {
        int bestScore = 0;
        const IModelLoader *best = nullptr;
        std::string bestName;
        for (const Entry &entry : m_loaders) {
            const IModelLoader *loader = entry.get();
            if (!loader) continue;
            const int score = loader->probe(header.data(), header.size());
            info.trace.push_back("probe " + std::string(loader->name()) + " -> " + std::to_string(score));
            if (score > bestScore) {
                bestScore = score;
                best = loader;
                bestName = loader->name();
            }
        }
        if (best && bestScore > 0) {
            info.loader = best;
            info.reason = "magic: " + bestName + " (score " + std::to_string(bestScore) + ")";
            info.trace.push_back("winner " + info.reason);
            return info;
        }
    } else {
        info.trace.push_back("header unavailable");
    }

    // 2) Extension fallback, preserving the original semantics: a later
    //    registration wins, then descriptor priority breaks a deliberate tie.
    const std::string extension = lowerExtension(path);
    if (extension.empty()) {
        info.trace.push_back("no extension, no magic -> unhandled");
        return info;
    }
    int bestRank = std::numeric_limits<int>::min();
    for (size_t i = 0; i < m_loaders.size(); ++i) {
        const Entry &entry = m_loaders[i];
        const auto &exts = entry.descriptor.extensions;
        if (std::find(exts.begin(), exts.end(), extension) == exts.end()) continue;
        const int rank = entry.descriptor.priority * 1000 + static_cast<int>(i);
        info.trace.push_back("extension ." + extension + " -> " + entry.descriptor.name +
                             " (rank " + std::to_string(rank) + ")");
        if (rank > bestRank) {
            bestRank = rank;
            info.loader = entry.get();
        }
    }
    if (info.loader) {
        info.reason = "extension: ." + extension;
        info.trace.push_back("winner " + info.reason + " via " + info.loader->name());
    }
    return info;
}

const IModelLoader *ModelLoaderRegistry::find(const std::string &path) const
{
    return resolve(path).loader;
}

const IModelLoader *ModelLoaderRegistry::findByName(const std::string &name) const
{
    for (const Entry &entry : m_loaders) {
        const IModelLoader *loader = entry.get();
        if (loader && name == loader->name()) return loader;
    }
    return nullptr;
}

bool ModelLoaderRegistry::supports(const std::string &path) const
{
    return find(path) != nullptr;
}

std::vector<std::string> ModelLoaderRegistry::supportedExtensions() const
{
    std::vector<std::string> result;
    for (const Entry &entry : m_loaders) {
        for (const std::string &extension : entry.descriptor.extensions) result.push_back(extension);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::string> ModelLoaderRegistry::catalog() const
{
    std::vector<std::string> lines;
    lines.reserve(m_loaders.size());
    for (const Entry &entry : m_loaders) {
        const IModelLoader *loader = entry.get();
        std::ostringstream line;
        line << entry.descriptor.name << " [" << (loader ? loader->name() : "?") << "]"
             << " ext=";
        for (const std::string &extension : entry.descriptor.extensions) line << "." << extension << " ";
        line << "priority=" << entry.descriptor.priority;
        if (loader && loader->isAnimationOnly()) line << " kind=animation";
        else line << " kind=geometry";
        line << " magic=" << (entry.descriptor.magicProbe ? "yes" : "no");
        lines.push_back(line.str());
    }
    return lines;
}

Model ModelLoaderRegistry::load(const std::string &path, const ModelLoadOptions &options) const
{
    const IModelLoader *loader = nullptr;
    std::string resolution;
    if (!options.preferredLoader.empty()) {
        loader = findByName(options.preferredLoader);
        if (!loader) throw std::runtime_error("ModelLoader: unknown loader '" + options.preferredLoader + "'");
        resolution = "forced: " + options.preferredLoader;
    } else {
        const ModelResolveInfo info = resolve(path);
        loader = info.loader;
        resolution = info.reason;
    }
    if (!loader) {
        std::string supported;
        for (const std::string &extension : supportedExtensions()) {
            if (!supported.empty()) supported += ", ";
            supported += "." + extension;
        }
        std::string message = "ModelLoader: no loader for '" + path + "' (supported: " + supported + ")";
        for (const Entry &entry : m_loaders) {
            const IModelLoader *candidate = entry.get();
            if (!candidate) continue;
            const std::string problem = candidate->diagnostic();
            if (!problem.empty()) message += std::string("; ") + candidate->name() + ": " + problem;
        }
        throw std::runtime_error(message);
    }
    if (loader->isAnimationOnly()) {
        throw std::runtime_error(std::string("ModelLoader: '") + path + "' is an animation-only format (" +
                                 loader->name() + "); use loadModelAnimation()");
    }

    Model model = loader->load(path, options);
    const PostProcessPlan plan = mergePlan(*loader, options, path);
    const size_t verticesBefore = model.vertices.size();
    const size_t indicesBefore = model.indices.size();

    const model_post::Pipeline pipeline = model_post::makeDefaultPipeline(*loader);
    const std::vector<model_post::StepResult> ran = pipeline.run(model, options, plan);

    if (options.log) {
        std::vector<std::string> chain;
        for (const auto &step : ran) chain.push_back(step.name + (step.detail.empty() ? "" : "(" + step.detail + ")"));
        std::cout << "[model] " << loader->name() << ": " << path << " -> " << model.vertices.size() << " vertices ("
                  << verticesBefore << " before dedup), " << model.indices.size() << " indices (" << indicesBefore
                  << "), " << model.triangleCount() << " triangles, " << model.meshes.size() << " mesh(es), "
                  << model.materials.size() << " material(s), " << model.images.size() << " image(s), "
                  << model.instanceCount() << " node instance(s)"
                  << " | resolved by " << resolution << " | chain: " << join(chain, " > ") << std::endl;
    }
    return model;
}

AnimationClip ModelLoaderRegistry::loadAnimation(const std::string &path, const ModelLoadOptions &options) const
{
    const IModelLoader *loader = nullptr;
    if (!options.preferredLoader.empty()) {
        loader = findByName(options.preferredLoader);
        if (!loader) throw std::runtime_error("ModelLoader: unknown loader '" + options.preferredLoader + "'");
    } else {
        loader = find(path);
    }
    if (!loader) throw std::runtime_error("ModelLoader: no animation loader for '" + path + "'");
    return loader->loadAnimation(path);
}

Model loadModel(const std::string &path, const ModelLoadOptions &options)
{
    return ModelLoaderRegistry::instance().load(path, options);
}

AnimationClip loadModelAnimation(const std::string &path, const ModelLoadOptions &options)
{
    return ModelLoaderRegistry::instance().loadAnimation(path, options);
}
