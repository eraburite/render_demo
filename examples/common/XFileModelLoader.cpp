#include "XFileModelLoader.h"

#include "ModelBinaryReader.h"
#include "ModelBuilder.h"
#include "ModelFormatsCommon.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// A tolerant DirectX X parser.  Instead of a full template grammar it extracts
// named brace-delimited data objects and treats everything else as a flat
// stream of numbers: that is exactly how Mesh / MeshNormals / Material store
// their payloads, and it survives the optional semicolon/comma punctuation and
// template definitions that real .x files carry.
struct XBlock {
    std::string kind;
    std::string name;
    std::string body; // scalar text with nested blocks removed
    std::vector<XBlock> children;
};

bool isIdentifierStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool isIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

void skipSpaceAndComments(const std::string &text, size_t &i)
{
    while (i < text.size()) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if ((c == '/' && i + 1 < text.size() && text[i + 1] == '/') || c == '#') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
            i = std::min(i + 2, text.size());
            continue;
        }
        break;
    }
}

std::string readIdentifier(const std::string &text, size_t &i)
{
    if (i >= text.size() || !isIdentifierStart(text[i])) return {};
    const size_t start = i;
    while (i < text.size() && isIdentifierChar(text[i])) ++i;
    return text.substr(start, i - start);
}

// Finds the brace matching text[open] and returns the index just past it.
size_t matchingBrace(const std::string &text, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}') {
            if (--depth == 0) return i + 1;
        }
    }
    throw std::runtime_error("XFile: unbalanced braces");
}

std::vector<XBlock> parseBlocks(const std::string &text, std::string &scalars)
{
    std::vector<XBlock> blocks;
    size_t i = 0;
    size_t chunkStart = 0;
    while (i < text.size()) {
        skipSpaceAndComments(text, i);
        if (i >= text.size()) break;
        if (!isIdentifierStart(text[i])) { ++i; continue; }

        const size_t tokenStart = i;
        const std::string kind = readIdentifier(text, i);
        skipSpaceAndComments(text, i);
        std::string name;
        if (i < text.size() && isIdentifierStart(text[i])) {
            name = readIdentifier(text, i);
            skipSpaceAndComments(text, i);
        }
        if (i < text.size() && text[i] == '{') {
            scalars += text.substr(chunkStart, tokenStart - chunkStart);
            const size_t end = matchingBrace(text, i);
            const std::string inner = text.substr(i + 1, end - i - 2);
            XBlock block;
            block.kind = kind;
            block.name = name;
            std::string childScalars;
            block.children = parseBlocks(inner, childScalars);
            block.body = childScalars;
            blocks.push_back(std::move(block));
            i = end;
            chunkStart = i;
        } else {
            i = tokenStart + kind.size(); // leave the token in the scalar stream
        }
    }
    scalars += text.substr(chunkStart);
    return blocks;
}

std::vector<float> numbers(const std::string &text)
{
    static const std::regex pattern("-?[0-9]+(?:\\.[0-9]*)?(?:[eE][+-]?[0-9]+)?");
    std::vector<float> result;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        result.push_back(std::stof(it->str()));
    }
    return result;
}

const XBlock *findChild(const XBlock &block, const std::string &kind)
{
    for (const XBlock &child : block.children) {
        if (child.kind == kind) return &child;
    }
    return nullptr;
}

struct ParsedMesh {
    std::string name;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<std::array<uint32_t, 3>> faces;
    std::vector<uint32_t> faceSource; // original face index for each triangle (polygon fan)
    std::vector<uint32_t> faceMaterials;
    std::vector<Material> materials;
};

ParsedMesh parseMesh(const XBlock &block)
{
    ParsedMesh mesh;
    mesh.name = block.name.empty() ? "XMesh" : block.name;
    const std::vector<float> n = numbers(block.body);
    size_t p = 0;
    if (n.empty()) throw std::runtime_error("XFile: Mesh has no vertex data");
    const uint32_t vertexCount = static_cast<uint32_t>(n[p++]);
    if (vertexCount > 1000000 || p + vertexCount * 3 > n.size()) {
        throw std::runtime_error("XFile: Mesh vertex count is out of range");
    }
    mesh.positions.resize(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        mesh.positions[i] = glm::vec3(n[p], n[p + 1], n[p + 2]);
        p += 3;
    }
    if (p >= n.size()) throw std::runtime_error("XFile: Mesh has no face data");
    const uint32_t faceCount = static_cast<uint32_t>(n[p++]);
    mesh.faces.reserve(faceCount);
    for (uint32_t f = 0; f < faceCount; ++f) {
        if (p >= n.size()) throw std::runtime_error("XFile: truncated face list");
        const uint32_t corners = static_cast<uint32_t>(n[p++]);
        if (corners < 3 || p + corners > n.size()) throw std::runtime_error("XFile: bad face corner count");
        const uint32_t a = static_cast<uint32_t>(n[p]);
        const uint32_t b = static_cast<uint32_t>(n[p + 1]);
        const uint32_t c = static_cast<uint32_t>(n[p + 2]);
        std::vector<uint32_t> polygon(corners);
        for (uint32_t k = 0; k < corners; ++k) polygon[k] = static_cast<uint32_t>(n[p + k]);
        p += corners;
        mesh.faces.push_back({a, b, c});
        mesh.faceSource.push_back(f);
        for (uint32_t k = 3; k < corners; ++k) {
            mesh.faces.push_back({polygon[0], polygon[k - 1], polygon[k]});
            mesh.faceSource.push_back(f);
        }
    }

    if (const XBlock *normals = findChild(block, "MeshNormals")) {
        const std::vector<float> nn = numbers(normals->body);
        if (!nn.empty()) {
            const uint32_t count = static_cast<uint32_t>(nn[0]);
            if (1 + count * 3 <= nn.size()) {
                mesh.normals.resize(count);
                for (uint32_t i = 0; i < count; ++i) {
                    mesh.normals[i] = glm::vec3(nn[1 + i * 3], nn[2 + i * 3], nn[3 + i * 3]);
                }
            }
        }
    }
    if (const XBlock *coords = findChild(block, "MeshTextureCoords")) {
        const std::vector<float> nt = numbers(coords->body);
        if (!nt.empty()) {
            const uint32_t count = static_cast<uint32_t>(nt[0]);
            if (1 + count * 2 <= nt.size()) {
                mesh.uvs.resize(count);
                for (uint32_t i = 0; i < count; ++i) mesh.uvs[i] = glm::vec2(nt[1 + i * 2], nt[2 + i * 2]);
            }
        }
    }

    mesh.faceMaterials.assign(mesh.faces.size(), 0);
    if (const XBlock *list = findChild(block, "MeshMaterialList")) {
        const std::vector<float> nl = numbers(list->body);
        std::vector<uint32_t> perFace;
        if (nl.size() >= 2) {
            const uint32_t faceIndices = static_cast<uint32_t>(nl[1]);
            perFace.assign(faceIndices, 0);
            for (uint32_t i = 0; i < faceIndices && 2 + i < nl.size(); ++i) {
                perFace[i] = static_cast<uint32_t>(nl[2 + i]);
            }
        }
        for (uint32_t i = 0; i < mesh.faces.size(); ++i) {
            mesh.faceMaterials[i] = (mesh.faceSource[i] < perFace.size()) ? perFace[mesh.faceSource[i]] : 0u;
        }
        for (const XBlock &child : list->children) {
            if (child.kind != "Material") continue;
            const std::vector<float> nm = numbers(child.body);
            Material material = modelformats::makePhongMaterial(
                child.name.empty() ? "material" + std::to_string(mesh.materials.size()) : child.name,
                nm.size() >= 4 ? glm::vec4(nm[0], nm[1], nm[2], nm[3]) : glm::vec4(0.8f, 0.8f, 0.8f, 1.0f),
                nm.size() >= 8 ? glm::vec3(nm[5], nm[6], nm[7]) : glm::vec3(1.0f),
                nm.size() >= 5 ? nm[4] : 2.0f, false);
            mesh.materials.push_back(std::move(material));
        }
        for (const XBlock &child : list->children) {
            if (child.kind == "TextureFilename") { /* path only, no bitmap decode here */ }
        }
    }
    if (mesh.materials.empty()) {
        mesh.materials.push_back(modelformats::makePhongMaterial("default", glm::vec4(0.8f), glm::vec3(1.0f), 2.0f, false));
    }
    return mesh;
}

glm::mat4 frameTransform(const XBlock &frame)
{
    if (const XBlock *matrix = findChild(frame, "FrameTransformMatrix")) {
        const std::vector<float> n = numbers(matrix->body);
        if (n.size() >= 16) {
            // XFile writes 16 floats in row-major order for DirectX's
            // row-vector convention.  Feeding that sequence to make_mat4
            // (column-major) already produces the transpose, which is exactly
            // the conversion to glm's column-vector convention -- transposing
            // a second time would move the translation back to the last row.
            return glm::make_mat4(n.data());
        }
    }
    return glm::mat4(1.0f);
}

struct MeshInstance {
    ParsedMesh mesh;
    glm::mat4 parentWorld{1.0f};
    int parentNode = -1;
};

void collectFrames(const XBlock &frame, const glm::mat4 &parentWorld, int parentNode,
                   ModelBuilder &builder, std::vector<MeshInstance> &out)
{
    const glm::mat4 local = frameTransform(frame);
    const int node = builder.addNode(frame.name.empty() ? "frame" : frame.name, local, -1, parentNode);
    const glm::mat4 world = parentWorld * local;
    for (const XBlock &child : frame.children) {
        if (child.kind == "Frame") {
            collectFrames(child, world, node, builder, out);
        } else if (child.kind == "Mesh") {
            MeshInstance instance;
            instance.mesh = parseMesh(child);
            instance.parentWorld = world;
            instance.parentNode = node;
            out.push_back(std::move(instance));
        }
    }
}

} // namespace

int XFileModelLoader::probe(const uint8_t *data, size_t size) const
{
    return modelio::startsWith(data, size, "xof ") ? 100 : 0;
}

PostProcessPlan XFileModelLoader::defaultPlan(const std::string &) const
{
    PostProcessPlan plan;
    plan.deduplicate = true;
    plan.flipTexCoordV = false;
    plan.generateNormals = true;
    plan.generateTangents = true;
    return plan;
}

void XFileModelLoader::postProcess(Model &model, const ModelLoadOptions &) const
{
    modelformats::finalizeMaterials(model);
}

Model XFileModelLoader::load(const std::string &path, const ModelLoadOptions &options) const
{
    std::ifstream file(path);
    if (!file) throw std::runtime_error("XFile: cannot open '" + path + "'");
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    if (text.rfind("xof ", 0) != 0) throw std::runtime_error("XFile: missing 'xof ' header in '" + path + "'");
    if (text.size() > 16 && text.substr(4, 8).find("bin") != std::string::npos) {
        throw std::runtime_error("XFile: binary .x is not supported, only the text flavour ('" + path + "')");
    }

    std::string scalars;
    const std::vector<XBlock> blocks = parseBlocks(text, scalars);
    (void)scalars;

    ModelBuilder builder;
    std::vector<MeshInstance> instances;
    for (const XBlock &block : blocks) {
        if (block.kind == "Frame") collectFrames(block, glm::mat4(1.0f), -1, builder, instances);
        else if (block.kind == "Mesh") {
            MeshInstance instance;
            instance.mesh = parseMesh(block);
            instances.push_back(std::move(instance));
        }
        // Header / template / other top-level objects are ignored.
    }
    if (instances.empty()) throw std::runtime_error("XFile: no Mesh found in '" + path + "'");

    const std::string baseDir = modelformats::baseDirectoryFor(path, options);
    (void)baseDir;
    for (MeshInstance &instance : instances) {
        ParsedMesh &source = instance.mesh;
        const size_t meshIndex = builder.addMesh(source.name);
        for (size_t i = 0; i < source.positions.size(); ++i) {
            MeshVertex vertex;
            vertex.position = source.positions[i];
            if (i < source.normals.size()) vertex.normal = source.normals[i];
            if (i < source.uvs.size()) vertex.texCoord = source.uvs[i];
            builder.addVertex(vertex);
        }
        uint32_t attributes = VertexAttribute::ePosition;
        if (source.normals.size() >= source.positions.size()) attributes |= VertexAttribute::eNormal;
        if (source.uvs.size() >= source.positions.size()) attributes |= VertexAttribute::eTexCoord;

        std::vector<int> materialIndices;
        for (Material &material : source.materials) materialIndices.push_back(builder.addMaterial(material));

        // One primitive per material, so every face lands in a contiguous
        // index window exactly like the other loaders.
        for (size_t m = 0; m < materialIndices.size(); ++m) {
            bool used = false;
            for (size_t f = 0; f < source.faces.size(); ++f) {
                if (source.faceMaterials[f] != m) continue;
                if (!used) {
                    builder.addPrimitive(meshIndex, materialIndices[m], attributes);
                    used = true;
                }
                const auto &face = source.faces[f];
                builder.addTriangle(face[0], face[1], face[2]);
            }
        }
        builder.addNode(source.name + "_instance", glm::mat4(1.0f), static_cast<int>(meshIndex),
                        instance.parentNode);
    }
    (void)options;
    return builder.build();
}
