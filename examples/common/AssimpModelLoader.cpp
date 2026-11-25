// Assimp-backed loader.
//
// The importer is a Win32 .dll, and the headers are vendored (external/assimp)
// so the API is known at compile time -- but *no* assimp symbol is referenced at
// link time.  Every entry point below is fetched with GetProcAddress.
//
// Why: example_common links the registry into all 21 example binaries.  A
// link-time import would put an "assimp-vc140-mt.dll missing" failure in front of
// every one of them, including the 20 that never touch a .dae.  Run-time binding
// keeps the blast radius at "this loader reports why it cannot work".
//
// This forces one style choice: the C++ aiMaterial / aiScene accessors are thin
// inline wrappers over exported C functions (material.inl calls
// aiGetMaterialTextureCount, ...), so the loader talks to the C API directly and
// reads the scene's plain structs.
#include "AssimpModelLoader.h"

#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <stb_image.h>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef RENDER_DEMO_ASSIMP_DLL_HINT
// Set by CMake when it can see an assimp SDK.  Empty means: rely on the bare
// name (exe directory / PATH) or on RENDER_DEMO_ASSIMP_DLL.
#    define RENDER_DEMO_ASSIMP_DLL_HINT ""
#endif

namespace {

// ---------------------------------------------------------------------------
// Runtime binding
// ---------------------------------------------------------------------------

using ImportFileFn = const aiScene *(*)(const char *, unsigned int);
using ReleaseImportFn = void (*)(const aiScene *);
using GetErrorStringFn = const char *(*)();
using MaterialTextureCountFn = unsigned int (*)(const aiMaterial *, aiTextureType);
using MaterialTextureFn = aiReturn (*)(const aiMaterial *, aiTextureType, unsigned int, aiString *,
                                       aiTextureMapping *, unsigned int *, float *, aiTextureOp *,
                                       aiTextureMapMode *, unsigned int *);
using MaterialColorFn = aiReturn (*)(const aiMaterial *, const char *, unsigned int, unsigned int, aiColor4D *);
using MaterialStringFn = aiReturn (*)(const aiMaterial *, const char *, unsigned int, unsigned int, aiString *);
using MaterialFloatArrayFn =
    aiReturn (*)(const aiMaterial *, const char *, unsigned int, unsigned int, float *, unsigned int *);

struct AssimpApi {
    ImportFileFn importFile = nullptr;
    ReleaseImportFn releaseImport = nullptr;
    GetErrorStringFn getErrorString = nullptr;
    MaterialTextureCountFn materialTextureCount = nullptr;
    MaterialTextureFn materialTexture = nullptr;
    MaterialColorFn materialColor = nullptr;
    MaterialStringFn materialString = nullptr;
    MaterialFloatArrayFn materialFloatArray = nullptr;

    bool valid() const
    {
        return importFile && releaseImport && getErrorString && materialTextureCount && materialTexture
               && materialColor && materialString && materialFloatArray;
    }
};

struct ResolvedRuntime {
    AssimpApi api;
    std::string loadedFrom;
    std::string problem;
};

#ifdef _WIN32
// LoadLibraryA would mangle non-ASCII paths on a CJK-locale machine; the
// candidate list includes an env var, so convert explicitly.
HMODULE loadLibraryUtf8(const std::string &path)
{
    if (path.empty()) {
        return nullptr;
    }
    const int wideLength = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wideLength <= 0) {
        return nullptr;
    }
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wideLength);
    return ::LoadLibraryW(wide.c_str());
}

template <class Fn>
Fn resolve(HMODULE module, const char *name)
{
    return reinterpret_cast<Fn>(::GetProcAddress(module, name));
}
#endif

const ResolvedRuntime &runtime()
{
    static const ResolvedRuntime resolved = [] {
        ResolvedRuntime result;
#ifdef _WIN32
        std::vector<std::string> candidates;
        if (const char *fromEnvironment = std::getenv("RENDER_DEMO_ASSIMP_DLL")) {
            candidates.emplace_back(fromEnvironment);
        }
        // A bare name is resolved against the executable's directory first,
        // which is where examples/model_loading's CMake copies the DLL.
        candidates.emplace_back("assimp-vc140-mt.dll");
        candidates.emplace_back(RENDER_DEMO_ASSIMP_DLL_HINT);

        for (const std::string &candidate : candidates) {
            HMODULE module = loadLibraryUtf8(candidate);
            if (!module) {
                continue;
            }
            AssimpApi api;
            api.importFile = resolve<ImportFileFn>(module, "aiImportFile");
            api.releaseImport = resolve<ReleaseImportFn>(module, "aiReleaseImport");
            api.getErrorString = resolve<GetErrorStringFn>(module, "aiGetErrorString");
            api.materialTextureCount = resolve<MaterialTextureCountFn>(module, "aiGetMaterialTextureCount");
            api.materialTexture = resolve<MaterialTextureFn>(module, "aiGetMaterialTexture");
            api.materialColor = resolve<MaterialColorFn>(module, "aiGetMaterialColor");
            api.materialString = resolve<MaterialStringFn>(module, "aiGetMaterialString");
            api.materialFloatArray = resolve<MaterialFloatArrayFn>(module, "aiGetMaterialFloatArray");
            if (!api.valid()) {
                result.problem = "'" + candidate + "' is not a usable assimp build (missing entry points)";
                continue;
            }
            result.api = api;
            result.loadedFrom = candidate;
            result.problem.clear();
            return result;
        }
        if (result.problem.empty()) {
            result.problem =
                "assimp-vc140-mt.dll was not found (put it next to the executable or set RENDER_DEMO_ASSIMP_DLL)";
        }
#else
        result.problem = "the assimp loader is only wired up for Windows in this repo";
#endif
        return result;
    }();
    return resolved;
}

// ---------------------------------------------------------------------------
// Scene -> Model conversion
// ---------------------------------------------------------------------------

glm::mat4 toGlm(const aiMatrix4x4 &m)
{
    // aiMatrix4x4 is row major (a1..a4 is the first row), glm is column major,
    // so the constructor arguments are the matrix' columns.
    return glm::mat4(m.a1, m.b1, m.c1, m.d1,
                     m.a2, m.b2, m.c2, m.d2,
                     m.a3, m.b3, m.c3, m.d3,
                     m.a4, m.b4, m.c4, m.d4);
}

std::string resolvePath(const std::string &baseDirectory, const std::string &name)
{
    if (name.empty()) {
        return {};
    }
    const std::filesystem::path candidate(name);
    if (candidate.is_absolute() || baseDirectory.empty()) {
        return name;
    }
    return (std::filesystem::path(baseDirectory) / candidate).string();
}

// Decodes a mesh material's texture once and returns its index in
// model.images, or -1.  Handles the three shapes assimp can report:
//   "textures/foo.png"  side-car file, resolved against the model directory
//   "*0"                compressed (png/jpg/...) bytes embedded in the file
//   "*0" with mHeight   raw BGRA texels embedded in the file
int resolveTexture(const aiString &path, const aiScene *scene, const std::string &baseDirectory,
                   std::vector<TextureImage> &images, std::unordered_map<std::string, int> &cache)
{
    const std::string name = path.C_Str();
    if (name.empty()) {
        return -1;
    }
    if (const auto cached = cache.find(name); cached != cache.end()) {
        return cached->second;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *pixels = nullptr;
    std::vector<uint8_t> converted;

    if (name[0] == '*') {
        const int index = std::atoi(name.c_str() + 1);
        if (index < 0 || index >= static_cast<int>(scene->mNumTextures)) {
            cache.emplace(name, -1);
            return -1;
        }
        const aiTexture *texture = scene->mTextures[index];
        if (!texture || !texture->pcData) {
            cache.emplace(name, -1);
            return -1;
        }
        if (texture->mHeight == 0) {
            // mWidth is the byte length of the encoded file, not a pixel count.
            pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(texture->pcData),
                                           static_cast<int>(texture->mWidth), &width, &height, &channels, 4);
        } else {
            width = static_cast<int>(texture->mWidth);
            height = static_cast<int>(texture->mHeight);
            converted.resize(static_cast<size_t>(width) * height * 4);
            for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
                // aiTexel is documented as ARGB8888 stored little-endian, i.e.
                // B,G,R,A in memory.
                converted[i * 4 + 0] = texture->pcData[i].r;
                converted[i * 4 + 1] = texture->pcData[i].g;
                converted[i * 4 + 2] = texture->pcData[i].b;
                converted[i * 4 + 3] = texture->pcData[i].a;
            }
        }
    } else {
        pixels = stbi_load(resolvePath(baseDirectory, name).c_str(), &width, &height, &channels, 4);
    }

    if (!pixels && converted.empty()) {
        // A missing map is routine: OBJ files reference textures that were
        // never shipped, and the geometry still renders with the base colour.
        cache.emplace(name, -1);
        return -1;
    }

    TextureImage image;
    image.name = name;
    image.width = width;
    image.height = height;
    if (!converted.empty()) {
        image.pixels = std::move(converted);
    } else {
        image.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
    }
    image.mipLevels = static_cast<uint32_t>(std::log2(static_cast<float>(std::max(width, height)))) + 1;

    const int imageIndex = static_cast<int>(images.size());
    images.push_back(std::move(image));
    cache.emplace(name, imageIndex);
    return imageIndex;
}

int firstTextureOfType(const aiMaterial *material, aiTextureType type, const aiScene *scene,
                       const std::string &baseDirectory, std::vector<TextureImage> &images,
                       std::unordered_map<std::string, int> &cache)
{
    if (runtime().api.materialTextureCount(material, type) == 0) {
        return -1;
    }
    aiString path;
    if (runtime().api.materialTexture(material, type, 0, &path, nullptr, nullptr, nullptr, nullptr, nullptr,
                                      nullptr)
        != AI_SUCCESS) {
        return -1;
    }
    return resolveTexture(path, scene, baseDirectory, images, cache);
}

Material convertMaterial(const aiMaterial *source, const aiScene *scene, const std::string &baseDirectory,
                         std::vector<TextureImage> &images, std::unordered_map<std::string, int> &cache)
{
    const AssimpApi &api = runtime().api;
    Material material;

    aiString name;
    if (api.materialString(source, AI_MATKEY_NAME, &name) == AI_SUCCESS) {
        material.name = name.C_Str();
    }

    aiColor4D diffuse(1.0f, 1.0f, 1.0f, 1.0f);
    if (api.materialColor(source, AI_MATKEY_COLOR_DIFFUSE, &diffuse) == AI_SUCCESS) {
        material.baseColorFactor =
            glm::vec4(diffuse.r, diffuse.g, diffuse.b, diffuse.a);
    }

    float opacity = 1.0f;
    unsigned int maxValues = 1;
    if (api.materialFloatArray(source, AI_MATKEY_OPACITY, &opacity, &maxValues) == AI_SUCCESS) {
        material.baseColorFactor.a = opacity;
        material.alphaMode = opacity < 0.999f ? AlphaMode::eBlend : AlphaMode::eOpaque;
    }

    aiColor4D emissive(0.0f, 0.0f, 0.0f, 1.0f);
    if (api.materialColor(source, AI_MATKEY_COLOR_EMISSIVE, &emissive) == AI_SUCCESS) {
        material.emissiveFactor = glm::vec3(emissive.r, emissive.g, emissive.b);
    }

    float shininess = 0.0f;
    maxValues = 1;
    if (api.materialFloatArray(source, AI_MATKEY_SHININESS, &shininess, &maxValues) == AI_SUCCESS) {
        // Same Phong -> roughness approximation the OBJ loader uses: there is no
        // metallic/roughness in most of these formats, so the exponent is the
        // only signal available.
        material.roughnessFactor = glm::clamp(1.0f - shininess / 1000.0f, 0.08f, 1.0f);
    }
    material.metallicFactor = 0.0f;
    // Triangulation and mixed exporters give no winding guarantee, so treat
    // every surface as two-sided (identical reasoning to the OBJ loader).
    material.doubleSided = true;

    material.baseColorTexture =
        firstTextureOfType(source, aiTextureType_DIFFUSE, scene, baseDirectory, images, cache);
    material.specularTexture =
        firstTextureOfType(source, aiTextureType_SPECULAR, scene, baseDirectory, images, cache);
    // Normal maps land in aiTextureType_NORMALS on modern importers, but a
    // height/bump map (what Blender's MTL exporter writes as map_Bump) arrives as
    // aiTextureType_HEIGHT -- this is exactly the fallback LearnOpenGL's model.h
    // encodes by asking for HEIGHT only.
    material.normalTexture =
        firstTextureOfType(source, aiTextureType_NORMALS, scene, baseDirectory, images, cache);
    if (material.normalTexture < 0) {
        material.normalTexture =
            firstTextureOfType(source, aiTextureType_HEIGHT, scene, baseDirectory, images, cache);
    }
    material.occlusionTexture =
        firstTextureOfType(source, aiTextureType_AMBIENT, scene, baseDirectory, images, cache);
    material.emissiveTexture =
        firstTextureOfType(source, aiTextureType_EMISSIVE, scene, baseDirectory, images, cache);
    return material;
}

// Appends one aiMesh as a primitive of the shared vertex/index buffers.
Primitive appendPrimitive(Model &model, const aiMesh *mesh)
{
    Primitive primitive;
    primitive.firstIndex = static_cast<uint32_t>(model.indices.size());
    primitive.materialIndex = mesh->mMaterialIndex < model.materials.size()
                                  ? static_cast<int>(mesh->mMaterialIndex)
                                  : -1;

    const bool hasNormals = mesh->HasNormals();
    const bool hasTexCoord = mesh->HasTextureCoords(0);
    const bool hasColors = mesh->HasVertexColors(0);
    const bool hasTangents = mesh->HasTangentsAndBitangents();

    const uint32_t vertexStart = static_cast<uint32_t>(model.vertices.size());
    model.vertices.reserve(model.vertices.size() + mesh->mNumVertices);
    for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
        MeshVertex vertex;
        vertex.position = glm::vec3(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
        if (hasNormals) {
            const glm::vec3 normal(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z);
            const float length = glm::length(normal);
            vertex.normal = length > 0.0f ? normal / length : glm::vec3(0.0f, 0.0f, 1.0f);
        }
        if (hasTexCoord) {
            vertex.texCoord = glm::vec2(mesh->mTextureCoords[0][v].x, mesh->mTextureCoords[0][v].y);
        }
        if (hasColors) {
            vertex.color = glm::vec3(mesh->mColors[0][v].r, mesh->mColors[0][v].g, mesh->mColors[0][v].b);
        }
        if (hasTangents) {
            vertex.tangent = glm::vec3(mesh->mTangents[v].x, mesh->mTangents[v].y, mesh->mTangents[v].z);
        }
        model.vertices.push_back(vertex);
    }

    for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        // aiProcess_Triangulate normally guarantees this; skipping anything else
        // keeps the index window a whole number of triangles if it does not.
        if (face.mNumIndices != 3) {
            continue;
        }
        for (unsigned j = 0; j < face.mNumIndices; ++j) {
            model.indices.push_back(vertexStart + face.mIndices[j]);
        }
    }

    primitive.indexCount = static_cast<uint32_t>(model.indices.size()) - primitive.firstIndex;
    primitive.attributes = VertexAttribute::ePosition;
    if (hasNormals) primitive.attributes |= VertexAttribute::eNormal;
    if (hasTexCoord) primitive.attributes |= VertexAttribute::eTexCoord;
    if (hasColors) primitive.attributes |= VertexAttribute::eColor;
    if (hasTangents) primitive.attributes |= VertexAttribute::eTangent;
    return primitive;
}

} // namespace

std::string AssimpModelLoader::runtimeDiagnostic()
{
    return runtime().problem;
}

bool AssimpModelLoader::isTopLeftV(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // glTF already stores texel (0,0) at the top-left, exactly like Vulkan's
    // sampled-image origin; everything else assimp reads is bottom-left.
    return extension != "gltf" && extension != "glb";
}

std::vector<std::string> AssimpModelLoader::extensions() const
{
    // Formats worth advertising: the ones this loader was actually exercised
    // with come first, the rest are "assimp reads them, nothing in this repo
    // tests them".  .gltf/.obj are listed too, so --loader assimp can be used to
    // A/B the third-party importer against the hand-written ones.
    return {"obj", "dae", "fbx", "3ds", "ply", "stl", "x3d", "md5mesh", "blend", "gltf", "glb"};
}

PostProcessPlan AssimpModelLoader::defaultPlan(const std::string &path) const
{
    PostProcessPlan plan;
    plan.deduplicate = true;
    plan.generateNormals = true;
    plan.generateTangents = true;
    plan.flipTexCoordV = isTopLeftV(path);
    return plan;
}

Model AssimpModelLoader::load(const std::string &path, const ModelLoadOptions &options) const
{
    const AssimpApi &api = runtime().api;
    if (!api.valid()) {
        throw std::runtime_error("assimp: " + runtime().problem);
    }

    // Only structural flags.  LearnOpenGL's model.h asks for
    // aiProcess_GenSmoothNormals | aiProcess_FlipUVs | aiProcess_CalcTangentSpace
    // as well, but each of those has an exact counterpart in the shared
    // post-process chain (generateMissingNormals / flipTexCoordV /
    // generateMissingTangents).  Requesting both would compute every attribute
    // twice and let the two implementations disagree about the same vertex --
    // assimp's smooth normals are angle weighted, the chain's are area weighted.
    // Triangulate has no counterpart because Primitive is triangles by
    // construction; SortByPType then guarantees the point/line meshes it can
    // produce are separate and can be skipped by primitive type.
    const unsigned int flags = aiProcess_Triangulate | aiProcess_SortByPType;

    const aiScene *scene = api.importFile(path.c_str(), flags);
    if (!scene) {
        throw std::runtime_error("assimp: failed to load '" + path + "': " + api.getErrorString());
    }
    struct SceneReleaser {
        ReleaseImportFn release;
        const aiScene *scene;
        ~SceneReleaser() { release(scene); }
    } releaser{api.releaseImport, scene};

    if (!scene->mRootNode || scene->mNumMeshes == 0) {
        throw std::runtime_error("assimp: '" + path + "' contains no meshes");
    }

    std::string baseDirectory = options.baseDirectory;
    if (baseDirectory.empty()) {
        baseDirectory = std::filesystem::path(path).parent_path().string();
    }

    Model model;

    std::unordered_map<std::string, int> textureCache;
    model.materials.resize(scene->mNumMaterials);
    for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
        model.materials[i] =
            convertMaterial(scene->mMaterials[i], scene, baseDirectory, model.images, textureCache);
    }

    // aiScene has no flat node array, only the tree, so the Model's node list is
    // built depth first.  One Mesh per aiNode keeps the graph and the draw list
    // in step even when a node references several aiMeshes: each of them becomes
    // one Primitive, and a Primitive is exactly what a draw needs.
    std::function<int(const aiNode *)> addNode = [&](const aiNode *source) -> int {
        ModelNode node;
        node.name = source->mName.C_Str();
        node.transform = toGlm(source->mTransformation);

        if (source->mNumMeshes > 0) {
            Mesh mesh;
            mesh.name = node.name;
            for (unsigned i = 0; i < source->mNumMeshes; ++i) {
                const aiMesh *sourceMesh = scene->mMeshes[source->mMeshes[i]];
                if (!sourceMesh || (sourceMesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) {
                    continue;
                }
                Primitive primitive = appendPrimitive(model, sourceMesh);
                if (primitive.indexCount > 0) {
                    mesh.primitives.push_back(primitive);
                }
            }
            if (!mesh.primitives.empty()) {
                node.meshIndex = static_cast<int>(model.meshes.size());
                model.meshes.push_back(std::move(mesh));
            }
        }

        const int nodeIndex = static_cast<int>(model.nodes.size());
        model.nodes.push_back(std::move(node));
        for (unsigned i = 0; i < source->mNumChildren; ++i) {
            const int childIndex = addNode(source->mChildren[i]);
            // Re-index instead of holding a reference: pushing a child may have
            // reallocated the vector.
            model.nodes[nodeIndex].children.push_back(childIndex);
        }
        return nodeIndex;
    };

    model.rootNodes.push_back(addNode(scene->mRootNode));

    if (model.meshes.empty()) {
        throw std::runtime_error("assimp: '" + path + "' contains no triangle geometry");
    }

    model.recomputeBounds();
    return model;
}
