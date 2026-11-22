#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "Camera.h"
#include "DescriptorLayoutConfig.h"
#include "IRenderPass.h"

// Vulkan uses a Y-down NDC after projection, hence the [1][1] flip.
inline CameraMatrix MakeCameraMatrix(const Camera &camera, float width, float height, float fov = 45.0f)
{
    CameraMatrix result;
    result.view = camera.GetViewMatrix();
    result.proj = glm::perspective(glm::radians(fov), width / height, 0.1f, 100.0f);
    result.proj[1][1] *= -1;
    return result;
}

// Spin about the model's own Y axis plus a fixed -20 degree X tilt so that
// three faces are visible at all times (spinning about Z looks like a flat
// square with zero-area side faces).
inline ObjectParams MakeSpinningModel(float seconds, const glm::vec3 &translation = glm::vec3(0.0f))
{
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, translation);
    model = glm::rotate(model, glm::radians(-20.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, seconds * glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    return ObjectParams{model};
}

inline glm::vec4 MakeEyePoint(const Camera &camera)
{
    return glm::vec4(camera.Position, 1.0f);
}

// Collect the offscreen resolve image views of an offscreen render pass.
inline std::vector<vk::ImageView> SwapChainImageViews(const std::vector<std::shared_ptr<vk::ImageView>> &views)
{
    std::vector<vk::ImageView> result;
    for (const auto &view : views) {
        if (view) {
            result.push_back(*view);
        }
    }
    return result;
}
