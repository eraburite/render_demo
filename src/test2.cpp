#include <memory>
#include <chrono>
#include <unordered_map>
#include <filesystem>
#include "typedefs.h"
#include "GLFWwindow.h"
#include "RenderManager.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "GraphicsPipeline.h"
#include "CmdOpDrawFrame.h"
#include "CmdOpUpload.h"
#include "Camera.h"

#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtx/hash.hpp"

#include "shader/lighting_vert.hpp"
#include "shader/lighting_frag.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

namespace std {
template <>
struct hash<Vertex> {
    size_t operator()(Vertex const &vertex) const
    {
        return ((hash<glm::vec3>()(vertex.position) ^ (hash<glm::vec3>()(vertex.color) << 1)) >> 1)
               ^ (hash<glm::vec2>()(vertex.texCoord) << 1);
    }
};
}; // namespace std

#define USE_MODELS_VEX 0
#define USE_DRAW_INDEX 0

// TODO: test VertexBuffer data
#if !USE_MODELS_VEX
// std::vector<Vertex> vertices = {
//     {{10.0f, -0.5f, 10.0f}, {1.0f, 0.0f, 0.0f}, {10.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
//     {{-10.0f, -0.5f, 10.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
//     {{-10.0f, -0.5f, -10.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 10.0f}, {0.0f, 1.0f, 0.0f}},
//     {{10.0f, -0.5f, 10.0f}, {0.0f, 1.0f, 0.0f}, {10.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
//     {{-10.0f, -0.5f, -10.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 10.0f}, {0.0f, 1.0f, 0.0f}},
//     {{10.0f, -0.5f, -10.0f}, {1.0f, 1.0f, 1.0f}, {10.0f, 10.0f}, {0.0f, 1.0f, 0.0f}},
// };
// std::vector<uint32_t> indices = {0, 1, 2, 2, 3, 0};

// using cube data
std::vector<Vertex> vertices = {
    {{-0.5f, -0.5f, -0.5f}, {}, {0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
    {{0.5f, -0.5f, -0.5f}, {}, {1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
    {{0.5f, 0.5f, -0.5f}, {}, {1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}},
    {{0.5f, 0.5f, -0.5f}, {}, {1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f, 0.5f, -0.5f}, {}, {0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f, -0.5f, -0.5f}, {}, {0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f, -0.5f, 0.5f}, {}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{0.5f, -0.5f, 0.5f}, {}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{0.5f, 0.5f, 0.5f}, {}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
    {{0.5f, 0.5f, 0.5f}, {}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.5f}, {}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, -0.5f, 0.5f}, {}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.5f}, {}, {1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, 0.5f, -0.5f}, {}, {1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {}, {0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {}, {0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f, 0.5f}, {}, {0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, 0.5f, 0.5f}, {}, {1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, {}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.5f}, {}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, {}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {}, {0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {}, {1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
    {{0.5f, -0.5f, 0.5f}, {}, {1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}},
    {{0.5f, -0.5f, 0.5f}, {}, {1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}},
    {{-0.5f, -0.5f, 0.5f}, {}, {0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {}, {0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
    {{-0.5f, 0.5f, -0.5f}, {}, {0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {}, {1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, {}, {1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, {}, {1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f, 0.5f, 0.5f}, {}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f, 0.5f, -0.5f}, {}, {0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
};
std::vector<uint32_t> indices = {}; // not using drawIndexed# endif
#else
std::vector<Vertex> vertices;
std::vector<uint32_t> indices;
void loadModel();
#endif

const std::string MODEL_PATH = "models/viking_room/viking_room.obj";
const std::string TEXTURE_PATH = "textures/container2.png";
const std::string TEXTURE_PATH2 = "textures/container2_specular.png";
const unsigned int SCR_WIDTH = 800;
const unsigned int SCR_HEIGHT = 600;

// camera
auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
std::vector<MaterialParams> params{{0, 1, 0.8f, 0.2f}, {1, 1, 0.7f, 0.1f}};
// looping call
void generateUniformMat(uint32_t width, uint32_t height, ObjectParams &modelMatrix, CameraMatrix &cameraMatrix)
{
    // static auto startTime = std:: chrono:: high   resolution   clock:: now();
    // auto currentTime = std:: chrono:: high   resolution   clock:: now();
    // float time = std:: chrono:: duration< float, std:: chrono:: seconds:: period>(currentTime - startTime). count();
    // model. model = glm:: rotate(glm::mat4(1.0f), time * glm:: radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    // camera. view = glm:: look At(glm::vec3(2.0f, 2.0f, 2.0f),  glm::vec3(0.0f, 0.0f,0.0f), glm::vec3(0.0f,
    // 0.0f, 1.0f) camera. proj = glm:: perspective(glm:: radians(45.0f), width / (float) height, 0.1f, 10.0f); camera.
    // proj[1][1] *= - 1;
    modelMatrix.model = glm::mat4(1.0f);
    cameraMatrix.view = camera->GetViewMatrix();
    cameraMatrix.proj = glm::perspective(glm::radians(camera->Zoom), (float)width / (float)height, 0.1f, 100.0f);
    cameraMatrix.proj[1][1] *= -1;
}

void test()
{
    // initWindow();
    GLFWwindow window(SCR_WIDTH, SCR_HEIGHT, camera);
    // Load the compiled SPIR-V bytecode
    std::unordered_map<std::string, std::vector<uint32_t>> shadersSpirv = {
        {" vert", std::vector<uint32_t>(shader::LIGHTING_VERT_SPV.begin(), shader::LIGHTING_VERT_SPV.end())},
        {" frag", std::vector<uint32_t>(shader::LIGHTING_FRAG_SPV.begin(), shader::LIGHTING_FRAG_SPV.end())},
    };

    // Load texture
    int texWidth = 0, texHeight = 0, texChannels = 0;
    if (!std::filesystem::exists(TEXTURE_PATH)) {
        throw std::runtime_error(TEXTURE_PATH + "not exists.");
    }
    stbi_uc *pixels = stbi_load(TEXTURE_PATH.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    auto mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;
    std::cout << " width: " << texWidth << ", height: " << texHeight << ", channels: " << texChannels
              << ", mipLevels: " << mipLevels << "\n";

    if (texWidth == 0 || texHeight == 0) {
        throw std::runtime_error("invaild dimension  value");
    }

    int texWidth1 = 0, texHeight1 = 0, texChannels1 = 0;
    if (!std::filesystem::exists(TEXTURE_PATH2)) {
        throw std::runtime_error(TEXTURE_PATH2 + "not exists.");
    }

    stbi_uc *pixels1 = stbi_load(TEXTURE_PATH2.c_str(), &texWidth1, &texHeight1, &texChannels1, STBI_rgb_alpha);
    auto mipLevels1 = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth1, texHeight1)))) + 1;
    std::cout << "width1: " << texWidth1 << ", height1: " << texHeight1 << ", channels1: " << texChannels1
              << ", mipLevels1: " << mipLevels1 << "\n";

    if (texWidth1 == 0 || texHeight1 == 0) {
        throw std::runtime_error("invaild dimension  value");
    }

    // TODO: texChannels always need 4 ???
    texChannels = 4;
#if USE_MODELS_VEX
    loadModel(); // Load the model's vertices, indices
#endif

    std::cout << " vertices count: " << vertices.size() << ", vertices byte: " << vertices.size() * sizeof(vertices[0])
              << ", indices count: " << indices.size() << ", indices byte: " << indices.size() * sizeof(indices[0]);

    // check data
    for (const auto &v : vertices) {
        if (std::isnan(v.position.x) || std::isinf(v.position.x)) {
            throw std::runtime_error("Found toxic data in model!");
        }
    }
    for (const auto &i : indices) {
        if (std::isnan(i) || std::isinf(i) || i < 0 || i > 3565) {
            throw std::runtime_error("Found toxic data in model!");
        }
    }

    VertexInputParam inputLayout(0, {
                                        VertexInputParam::VertexComponent::ePosition,
                                        VertexInputParam::VertexComponent::eColor,
                                        VertexInputParam::VertexComponent::eUV,
                                        VertexInputParam::VertexComponent::eNormal,
                                    });
    inputLayout.SetSpec(ResourceTypes::eVertex, {static_cast<uint32_t>(vertices.size()),
                                                 static_cast<uint32_t>(sizeof(vertices[0])), DataTypes::eCustom});
#if USE_DRAW_INDEX
    inputLayout.SetSpec(ResourceTypes::eIndex, {static_cast<uint32_t>(indices.size()),
                                                static_cast<uint32_t>(sizeof(indices[0])), DataTypes::eUnsignedInt});
#endif
    // set0
    GlobalParam globalMeta(
        {GlobalComponent::ebaseColor, GlobalComponent::eSpecularMap, GlobalComponent::eMaterialParams});
    globalMeta.SetSpec(GlobalComponent::ebaseColor,
                       {static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight),
                        static_cast<uint32_t>(texChannels), static_cast<uint32_t>(mipLevels)});
    globalMeta.SetSpec(GlobalComponent::eSpecularMap,
                       {static_cast<uint32_t>(texWidth1), static_cast<uint32_t>(texHeight1),
                        static_cast<uint32_t>(texChannels1), static_cast<uint32_t>(mipLevels1)});
    globalMeta.SetSpec(
        GlobalComponent::eMaterialParams,
        {static_cast<uint32_t>(params.size()), static_cast<uint32_t>(sizeof(MaterialParams)), DataTypes::eCustom});
    // set1
    OccaUpdateParam occaUpdateMeta({
        OccaUpdateComponent::eCameraMatrix,
    });
    occaUpdateMeta.SetSpec(OccaUpdateComponent::eCameraMatrix,
                           {1, static_cast<uint32_t>(sizeof(CameraMatrix)), DataTypes::eCustom});
    // set2
    SceneFrameParam sceneMeta({
        SceneFrameComponent::eLightSource,
        SceneFrameComponent::eViewEyePosition,
    });
    sceneMeta.SetSpec(SceneFrameComponent::eLightSource,
                      {1, static_cast<uint32_t>(sizeof(LightSource)), DataTypes::eCustom});
    sceneMeta.SetSpec(SceneFrameComponent::eViewEyePosition,
                      {1, static_cast<uint32_t>(sizeof(ViewEyePoint)), DataTypes::eCustom});
    PushConstantParam pushconstantMeta({PushConstantComponent::eModelMatrix});
    pushconstantMeta.SetSpec(PushConstantComponent::eModelMatrix,
                             {1, static_cast<uint32_t>(sizeof(ObjectParams)), DataTypes::eCustom});
    // To build a Pipeline, you need to specify the layouts of VertexBuffer, IndexBuffer, and Uniformburgurs.
    PipelineDescriptorParams descLayout(globalMeta, occaUpdateMeta, sceneMeta, pushconstantMeta);
    uint32_t maxFrame = descLayout.MaxFrameCountInFlight();
    // initVulkan();
    auto mgr = std::make_shared<RenderManager>(window, maxFrame);
    auto pipeline = mgr->CreatePipeline(inputLayout, descLayout, shadersSpirv);
    // Construct a OnceSequence to assemble the VK command-uploaded static data, including VertexBuffer,
    // IndexBuffer, and UniformBuffers, and other actual data.
    std::shared_ptr<CmdOpBase> uploadCmd = std::make_shared<CmdOpUpload>(pipeline);
    uploadCmd->OnUpdateRawData(ResourceTypes::eVertex, vertices);
#if USE_DRAW_INDEX
    uploadCmd->OnUpdateRawData(ResourceTypes::eIndex, indices);
#endif

    auto baseColorBytes = globalMeta.getParamComponentBytes(GlobalComponent::ebaseColor);
    auto specularBytes = globalMeta.getParamComponentBytes(GlobalComponent::eSpecularMap);
    for (int i = 0; i < maxFrame; i++) {
        uploadCmd->OnUpdateRawData(ResourceId(0, 0), i, pixels, baseColorBytes);
        uploadCmd->OnUpdateRawData(ResourceId(0, 1), i, pixels1, specularBytes);
        uploadCmd->OnUpdateRawData(ResourceId(0, 2), i, params);
    }
    auto uploadSequence = mgr->UploadSequence();
    uploadSequence->eval(uploadCmd);
    stbi_image_free(pixels);
    // std:: cout << " finished scatic data upload in main\n";
    // Construct the LoopSequence, assemble the rendering VK command recordCommandBuffer, update dynamic data such
    // as UniformBuffers, and draw. mainLoop( );
    std::shared_ptr<CmdOpBase> drawCmd = std::make_shared<CmdOpDrawFrame>(pipeline);
    auto drawSequence = mgr->DrawFrameSequence();
    // std:: cout << " begin draw if main\n";
    while (!window.shouldClose()) {
        // std:: cout << " draw frame in mainloop, index: " << i << "\n"`
        float currentTime;
        auto start = std::chrono::steady_clock::now();
        window.processInputEvents(currentTime);
        auto end1 = std::chrono::steady_clock::now();
        ObjectParams modelMatrix{};
        CameraMatrix cameraMatrix{};
        auto extent2D = mgr->getSwapChain()->getExtent2D();
        generateUniformMat(extent2D->width, extent2D->height, modelMatrix, cameraMatrix);
        LightSource lightSource{};
        lightSource.position = glm::vec4(1.2f, 1.0f, 2.0f, 1.0f);
        glm::vec4 lightColor;
        lightColor.x = sin(currentTime * 2.0f);
        lightColor.y = sin(currentTime * 0.7f);
        lightColor.z = sin(currentTime * 1.3f);
        lightSource.ambient = glm::vec4(0.05f); // * light Color;
        lightSource.diffuse = glm::vec4(0.5f);  // * lightColor;
        lightSource.specular = glm::vec4(0.77f);
        ViewEyePoint eyePoint{};
        eyePoint.position = glm::vec4(camera->Position, 1.0f);
        auto currentFrame = drawSequence->getFrameInFlight();
        drawCmd->OnUpdateRawData(ResourceId(1, 0), currentFrame, cameraMatrix);
        drawCmd->OnUpdateRawData(ResourceId(2, 0), currentFrame, lightSource);
        drawCmd->OnUpdateRawData(ResourceId(2, 1), currentFrame, eyePoint);
        drawCmd->OnUpdateRawData(ResourceTypes::ePushConst, currentFrame, modelMatrix);
        auto end2 = std::chrono::steady_clock::now();
        drawSequence->eval(drawCmd);
        auto end3 = std::chrono::steady_clock::now();
        window.swapBuffers();
        const std::chrono::duration<double, std::milli> diff1 = end1 - start;
        const std::chrono::duration<double, std::milli> diff2 = end2 - end1;
        const std::chrono::duration<double, std::milli> diff3 = end3 - end2;
        // std:: cout << " mainloop draw cost(ms): " << diff1. count() << " " << diff2. count() <<
        //	<< "\n":
    }
    // cleanup();
    // By using the RAII mechanism
}

int main()
{
    test();
    return 0;
}

#if USE_MODELS_VEX
void loadModel()
{
    tinyobj::attrib t attrib;
    std::vector<tinyobj::shape t> shapes;
    std::vector<tinyobj::material t> materials;
    std::string warn, err;
    if (!tinyobj::Load0bj(&attrib, &shapes, &materials, &warn, &err, MODEL_PATH.c str())) {
        throw std::runtime error(warn + err);
    }
    std::unordered map<Vertex, uint32_t> uniqueVertices{};
    for (const auto &shape : shapes) {
        for (const auto &index : shape.mesh.indices) {
            Vertex vertex{};
            vertex.position = {attrib.vertics[3 * index.vertex index + 0],
                               attrib.vertices[3 * index.vertex index + 1] attrib.vertices[3 * index.vertex index + 2]};
            vertex.texCoord = {attrib.texcoords[2 * index.texcord index + 0],
                               1.0f - attrib.texcoords[2 * index.texcoord index + 1]};
            vertex.color = {1.0f, 1.0f, 1.0f};
            if (uniqueVertices.count(vertex) == 0) {
                uniqueVertices[vertex] = static_cast<uint32_t>(vertics.size());
                vertices.push back(vertex);

                indices.push back(uniqueVertices[vertex]);
            }
        }
    }
}
#endif
