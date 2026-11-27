#pragma once
#include "ModelAsset.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Model loading front end.
//
// Four layers, deliberately separated so that adding a format is additive:
//
//   IModelLoader          one class per file format; its only job is to turn
//                         bytes into a Model.  It never post-processes.
//   ModelLoaderDescriptor  how a loader enters the system (name, extensions,
//                         priority, factory).  The core registry knows only
//                         this value type, never a concrete loader class.
//   PostProcessPipeline   dedup / normals / tangents / transforms, an ordered
//                         list of pluggable steps shared by every loader.
//   ModelLoaderRegistry   resolves a loader (magic probe first, extension
//                         second), runs the chain, then the loader's own hook.
//
// Keeping post-processing out of the loaders turns N formats times M fix-ups
// from N*M hand-written combinations into N+M, and a new format gets the whole
// chain for free.  Keeping concrete loaders out of the registry turns "add a
// format" from editing framework code into appending one row to a table.
// ---------------------------------------------------------------------------

// Three-state switch for an optional post-process step.  eAuto means "let the
// loader's own recommendation decide", which matters because the right answer
// is format specific: glTF primitives arrive already indexed, OBJ faces arrive
// fully exploded.
enum class Toggle {
    eAuto,
    eOn,
    eOff,
};

struct ModelLoadOptions {
    Toggle deduplicate = Toggle::eAuto;
    Toggle generateNormals = Toggle::eAuto;
    Toggle generateTangents = Toggle::eAuto;

    // V axis convention.  glTF puts texel (0,0) at the top-left, matching
    // Vulkan's sampled-image origin, so nothing is flipped.  OBJ/MTL follows
    // the older bottom-left convention, so its loader recommends the flip.
    Toggle flipTexCoordV = Toggle::eAuto;

    float scale = 1.0f;
    bool centerAtOrigin = false;
    bool normalizeSize = false; // longest axis becomes 2.0 after centring

    // Bakes every node's world matrix into the vertices and collapses the scene
    // to one identity root per mesh instance.
    bool flattenScene = false;

    // Directory used to resolve side-car files (MTL, textures).  Empty means
    // "the model file's own directory".
    std::string baseDirectory;

    // Name of an IModelLoader to use instead of dispatch ("" = dispatch).
    // A/B-ing two loaders on the same file needs this: assimp and tiny_obj both
    // read .obj, and dispatch always hands the file to the hand-written one.
    std::string preferredLoader;

    // Print a one-line summary to stdout: which loader ran, what the file
    // contained, and what the post-process chain actually changed.
    bool log = true;
};

// What a loader recommends for the Toggle::eAuto steps.
struct PostProcessPlan {
    bool deduplicate = true;
    bool flipTexCoordV = false;
    bool generateNormals = true;
    bool generateTangents = true;
};

// ---------------------------------------------------------------------------
// IModelLoader -- the Strategy.
//
// Every virtual added after the first three has a default implementation, so a
// loader written against the old interface keeps compiling unchanged.  That is
// what lets the registry grow capabilities (content sniffing, format-specific
// hooks, animation-only resources) without a flag day across all formats.
// ---------------------------------------------------------------------------
class IModelLoader {
public:
    virtual ~IModelLoader() = default;

    // Stable identifier used in logs and by --loader style options.
    virtual const char *name() const = 0;

    // Lower-case extensions without the dot, e.g. {"gltf", "glb"}.
    virtual std::vector<std::string> extensions() const = 0;

    // Throws std::runtime_error with a readable message on malformed input.
    // Implementations fill Primitive::attributes honestly: the post-process
    // chain trusts those bits.  Const because loaders are stateless.
    virtual Model load(const std::string &path, const ModelLoadOptions &options) const = 0;

    // Takes the path because the recommendation can be *format* specific inside
    // one loader: assimp covers dozens of formats, and their V axis conventions
    // do not agree.
    virtual PostProcessPlan defaultPlan(const std::string &path) const
    {
        (void)path;
        return {};
    }

    // --- Chain of Responsibility: content sniffing --------------------------
    // Inspect up to `size` leading bytes and return a confidence score.
    //   0     = "no opinion", let the extension decide (the safe default)
    //   1..99 = weak/medium hint (e.g. a generic text header)
    //   >=100 = a magic number unique to this format, authoritative
    // The registry takes the highest score across all loaders before it ever
    // looks at the file name, so a renamed or extension-less file still loads,
    // and a format whose extension overlaps another one (PMX vs PMD vs .x)
    // cannot be mis-dispatched.
    virtual int probe(const uint8_t *data, size_t size) const
    {
        (void)data;
        (void)size;
        return 0;
    }

    // --- Template Method hook ------------------------------------------------
    // Runs once, after the shared pipeline.  This is the escape hatch for a
    // format convention no generic step should encode (MMD's left-handed +Y up
    // with +Z into the screen, toon/sphere slots, double-sided materials).
    virtual void postProcess(Model &model, const ModelLoadOptions &options) const
    {
        (void)model;
        (void)options;
    }

    // --- Optional resource kind: animation -----------------------------------
    // A loader that registers an animation-only format (.vmd) reports true;
    // load() then throws a readable "animation-only" error rather than trying
    // to invent geometry.  Registry::loadAnimation() dispatches to these.
    virtual bool isAnimationOnly() const { return false; }
    virtual AnimationClip loadAnimation(const std::string &path) const;

    // --- Observability -------------------------------------------------------
    // Empty when the loader is usable; otherwise a reason suitable for
    // appending to "no loader for this file".  Replaces the old hard-coded
    // AssimpModelLoader::runtimeDiagnostic() call inside the registry.
    virtual std::string diagnostic() const { return {}; }
};

// ---------------------------------------------------------------------------
// Plugin descriptor / self-registration.
//
// The registry owns descriptors, not loader classes.  Built-ins are listed in
// BuiltinModelLoaders.cpp (a link-time guarantee that every loader TU is pulled
// into the static library); third parties can instead drop a
// ModelLoaderRegistrar in their own translation unit and register the same way
// without touching any framework file.
// ---------------------------------------------------------------------------
using ModelLoaderFactory = std::function<std::unique_ptr<IModelLoader>()>;

struct ModelLoaderDescriptor {
    std::string name;
    std::vector<std::string> extensions;
    int priority = 0;      // tie-break when two loaders claim the same extension
    bool magicProbe = false; // advertised in the catalog; true when probe() reads a magic number
    ModelLoaderFactory factory;
};

// Why a given loader won.  Kept as a value so --probe can print the whole
// decision instead of leaving dispatch as a black box.
struct ModelResolveInfo {
    const IModelLoader *loader = nullptr;
    std::string reason;              // "magic: pmx (score 100)" / "extension: .obj"
    std::vector<std::string> trace;  // one line per candidate considered
};

class ModelLoaderRegistry {
public:
    static ModelLoaderRegistry &instance();

    // Descriptor form: the framework core never has to name a concrete class.
    void registerLoader(ModelLoaderDescriptor descriptor);

    // Legacy form, kept so tests can override a built-in with a live instance.
    void registerLoader(std::unique_ptr<IModelLoader> loader);

    const IModelLoader *find(const std::string &path) const;
    const IModelLoader *findByName(const std::string &name) const;
    bool supports(const std::string &path) const;
    std::vector<std::string> supportedExtensions() const;

    // Full decision record: magic probes first, then extension fallback.
    ModelResolveInfo resolve(const std::string &path) const;

    // One line per registered plugin: name, extensions, probe capability.
    std::vector<std::string> catalog() const;

    Model load(const std::string &path, const ModelLoadOptions &options = {}) const;

    // Animation-only formats go through the same resolution, but dispatch to
    // IModelLoader::loadAnimation() instead of load().
    AnimationClip loadAnimation(const std::string &path, const ModelLoadOptions &options = {}) const;

private:
    ModelLoaderRegistry();

    struct Entry {
        ModelLoaderDescriptor descriptor;
        mutable std::unique_ptr<IModelLoader> instance; // lazily created
        const IModelLoader *get() const;
    };
    std::vector<Entry> m_loaders;
};

// Installed by BuiltinModelLoaders.cpp.  Called once from the registry ctor;
// kept as a free function so ModelLoader.cpp needs no concrete includes.
void registerBuiltinModelLoaders(ModelLoaderRegistry &registry);

// Convenience wrappers for the registry singleton.
Model loadModel(const std::string &path, const ModelLoadOptions &options = {});
AnimationClip loadModelAnimation(const std::string &path, const ModelLoadOptions &options = {});

// Individual steps, exposed so a loader or a test can drive them directly.
// Each respects the per-primitive index windows: dedup and attribute
// generation never merge or mix vertices across two primitives.
namespace model_post {

void flattenScene(Model &model);
void deduplicateVertices(Model &model);
void generateMissingNormals(Model &model);
void generateMissingTangents(Model &model);
void flipTexCoordV(Model &model);
void transformPositions(Model &model, const glm::mat4 &transform);
void centerAtOrigin(Model &model);
void normalizeSize(Model &model, float targetSize);
void scalePositions(Model &model, float scale);

} // namespace model_post
