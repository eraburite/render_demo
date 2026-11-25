#pragma once
#include "ModelAsset.h"

#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Model loading front end.
//
// Three layers, deliberately separated so that adding a format is additive:
//
//   IModelLoader          one class per file format; its only job is to turn
//                         bytes into a Model.  It never posts-processes.
//   post-process chain    dedup / normals / tangents / transforms, shared by
//                         every loader and applied once by the registry.
//   ModelLoaderRegistry   picks a loader by file extension and runs the chain.
//
// Rationale for keeping post-processing out of the loaders: N formats times M
// fix-ups would otherwise be N*M hand-written combinations, and a fix-up bug
// would have to be fixed N times.  Here it is N+M, and a new format gets the
// whole chain for free.
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
    //
    // Needed before any geometry-space operation on a hierarchical file:
    // Model::recomputeBounds only sees the un-instanced vertex buffer, so for a
    // .dae whose nodes carry a 0.01 scale the bounds describe a model 100x larger
    // than what a renderer that applies those transforms will draw.  That makes
    // normalizeSize scale the wrong space and the fixed near/far of the
    // projection clip the result.
    bool flattenScene = false;

    // Directory used to resolve side-car files (MTL, textures).  Empty means
    // "the model file's own directory".
    std::string baseDirectory;

    // Name of an IModelLoader to use instead of extension dispatch ("" = dispatch).
    // A/B-ing two loaders on the same file needs this: assimp and tiny_obj both
    // read .obj, and dispatch always hands the file to the hand-written one.
    std::string preferredLoader;

    // Print a one-line summary to stdout: which loader ran, what the file
    // contained, and what the post-process chain actually changed.  Cheap
    // observability that makes a format regression obvious from any example.
    bool log = true;
};

// What a loader recommends for the Toggle::eAuto steps.
struct PostProcessPlan {
    bool deduplicate = true;
    bool flipTexCoordV = false;
    bool generateNormals = true;
    bool generateTangents = true;
};

class IModelLoader {
public:
    virtual ~IModelLoader() = default;

    // Stable identifier used in logs and by --loader style options.
    virtual const char *name() const = 0;

    // Lower-case extensions without the dot, e.g. {"gltf", "glb"}.
    virtual std::vector<std::string> extensions() const = 0;

    // Throws std::runtime_error with a readable message on malformed input.
    // Implementations fill Primitive::attributes honestly: the post-process
    // chain trusts those bits.  Const because loaders are stateless -- state
    // belongs to the returned Model, not to the loader.
    virtual Model load(const std::string &path, const ModelLoadOptions &options) const = 0;

    // Takes the path because the recommendation can be *format* specific inside
    // one loader: assimp covers dozens of formats, and their V axis conventions
    // do not agree.
    virtual PostProcessPlan defaultPlan(const std::string &path) const
    {
        (void)path;
        return {};
    }
};

class ModelLoaderRegistry {
public:
    static ModelLoaderRegistry &instance();

    // Takes ownership.  Re-registering an extension replaces the previous
    // loader, which lets a test override a built-in format.
    void registerLoader(std::unique_ptr<IModelLoader> loader);

    const IModelLoader *find(const std::string &path) const;
    const IModelLoader *findByName(const std::string &name) const;
    bool supports(const std::string &path) const;
    std::vector<std::string> supportedExtensions() const;

    // Finds a loader, calls load(), then runs the shared post-process chain.
    Model load(const std::string &path, const ModelLoadOptions &options = {}) const;

private:
    ModelLoaderRegistry();
    std::vector<std::unique_ptr<IModelLoader>> m_loaders;
};

// Convenience wrapper for the registry singleton.
Model loadModel(const std::string &path, const ModelLoadOptions &options = {});

// Individual steps, exposed so a loader or a test can drive them directly.
// Each respects the per-primitive index windows: dedup and attribute
// generation never merge or mix vertices across two primitives.
namespace model_post {

// Applies each node's world matrix to its primitives' vertices and emits one
// identity node per mesh instance (orphan meshes are kept, also identity).  Mesh
// names and the per-primitive material split survive; the hierarchy does not.
void flattenScene(Model &model);

void deduplicateVertices(Model &model);

// Area-weighted face normals for primitives whose attributes say the file had
// none.  Primitives that already carry normals are untouched.
void generateMissingNormals(Model &model);

// Tangent from UV derivatives, Gram-Schmidt orthogonalised against the normal.
// Skips primitives without texcoords or normals.
void generateMissingTangents(Model &model);

void flipTexCoordV(Model &model);

// Applies an affine transform to positions only (uniform scale assumed; a
// non-uniform matrix would need the inverse-transpose for normals).
void transformPositions(Model &model, const glm::mat4 &transform);

void centerAtOrigin(Model &model);
void normalizeSize(Model &model, float targetSize);
void scalePositions(Model &model, float scale);

} // namespace model_post
