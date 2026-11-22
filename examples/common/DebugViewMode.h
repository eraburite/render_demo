#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

// ---------------------------------------------------------------------------
// Debug View: on-screen visualisation of intermediate render targets.
//
// The recurring need while debugging a renderer is to "look at the buffer".
// Is the G-Buffer normal written correctly, is it flipped, is the depth range
// sane, is the offscreen image upside down?  The premise of this component is
// that **no extra pass, pipeline or descriptor set is needed**: a fullscreen
// pass (deferred / quad) already samples those intermediates, so switching its
// fragment shader output with a push constant is enough.  Enabling a debug view
// therefore costs no extra draw call, needs no pipeline rebuild (only the push
// constant changes) and can be toggled per frame.
//
// Three layers must stay in sync (this file, the shaders, DESIGN.md):
//   1. `DebugViewMode` is the shared vocabulary across examples and the value
//      written into the push constant's `mode` field;
//   2. each example declares the **subset** it supports via
//      `DebugViewState({...})`; digit keys `0`..`N-1` select the i-th entry of
//      that subset rather than the enum value itself, so examples with
//      different capabilities keep an identical, local key layout while the
//      shader still branches on the semantic enum;
//   3. the colour mapping rules are global (see the comment above
//      DebugViewModeName).  If two examples encoded the same channel
//      differently, comparing them would cost more than it saves.
//
// This header is deliberately header-only and GLFW-free: key lookup is injected
// as a callback so the same mode semantics can be reused on an offline path.
// ---------------------------------------------------------------------------
enum class DebugViewMode : uint32_t {
    // ---- deferred / G-Buffer path ------------------------------------------
    eFinal = 0,    // normal shaded result (default, supported everywhere)
    eAlbedo,       // G-Buffer albedo, written out unchanged
    eNormal,       // G-Buffer world-space normal, n * 0.5 + 0.5
    ePosition,     // G-Buffer world-space position, pos / referenceScale * 0.5 + 0.5
    eLinearDepth,  // distance to the eye, near = bright
    eLightingOnly, // lighting contribution with albedo forced to white
    // ---- forward / offscreen path ------------------------------------------
    eUv,           // screen (u, v) gradient: shows sampling direction and V flip at a glance
    eLuminance,    // luminance greyscale: exposure / contrast check
    eNativeScale,  // intermediate shown 1:1 in the centre, distinct from the stretched blit
    eCount
};

// Colour mapping rules, shared by every shader and mirrored by the PPM dump
// (examples/common/ImageDump.h):
//   eAlbedo       -> as-is (G-Buffer is UNORM / SFLOAT, no tone mapping)
//   eNormal       -> n * 0.5 + 0.5 (world space: +X red, +Y green, +Z blue)
//   ePosition     -> clamp(pos / referenceScale * 0.5 + 0.5, 0, 1)
//   eLinearDepth  -> grey, 1 - clamp((d - near) / (far - near), 0, 1)
//   eLightingOnly -> ambient + diffuse * NdotL + specular (albedo excluded)
//   eUv           -> vec4(u, v, 0, 1)
//   eLuminance    -> grey, dot(rgb, vec3(0.2126, 0.7152, 0.0722))
//   eNativeScale  -> content drawn 1:1 in the centre, background outside
inline std::string_view DebugViewModeName(DebugViewMode mode)
{
    switch (mode) {
    case DebugViewMode::eFinal: return "final";
    case DebugViewMode::eAlbedo: return "albedo";
    case DebugViewMode::eNormal: return "normal";
    case DebugViewMode::ePosition: return "position";
    case DebugViewMode::eLinearDepth: return "linear-depth";
    case DebugViewMode::eLightingOnly: return "lighting-only";
    case DebugViewMode::eUv: return "uv";
    case DebugViewMode::eLuminance: return "luminance";
    case DebugViewMode::eNativeScale: return "native-1:1";
    case DebugViewMode::eCount: break;
    }
    return "unknown";
}

// Mirrors the layout(push_constant) block in the shaders field for field.
// Every member is deliberately a 4-byte scalar (never a vec2) so there is no
// alignment or padding assumption to get wrong: 8 fields = 32 bytes, offsets
// 0/4/8/.../28.
struct DebugViewPushConstant {
    uint32_t mode = 0;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    float referenceScale = 5.0f;
    float viewportWidth = 800.0f;
    float viewportHeight = 600.0f;
    float contentWidth = 512.0f;
    float contentHeight = 512.0f;
};
static_assert(sizeof(DebugViewPushConstant) == 32,
              "DebugViewPushConstant must stay 32 bytes to match the GLSL push_constant block");

// Digit keys `0`..`N-1` pick a mode directly.  Edge triggered: a key only acts
// on the frame it is pressed, so holding it down does not flicker the mode.
class DebugViewState {
public:
    explicit DebugViewState(std::vector<DebugViewMode> available) : m_available(std::move(available))
    {
        if (m_available.empty()) {
            m_available.push_back(DebugViewMode::eFinal);
        }
        // There are only ten digit keys.
        if (m_available.size() > m_digitHeld.size()) {
            m_available.resize(m_digitHeld.size());
        }
    }

    // isKeyDown: injected key query (examples pass window.isKeyDown);
    // firstDigitKey: key code of the digit 0 (examples pass GLFW_KEY_0).
    void poll(const std::function<bool(int)> &isKeyDown, int firstDigitKey)
    {
        m_changed = false;
        const uint32_t modeCount = static_cast<uint32_t>(m_available.size());
        for (uint32_t digit = 0; digit < m_digitHeld.size(); ++digit) {
            const bool down = isKeyDown(firstDigitKey + static_cast<int>(digit));
            const bool pressed = down && !m_digitHeld[digit];
            m_digitHeld[digit] = down;
            if (pressed && digit < modeCount && digit != m_index) {
                m_index = digit;
                m_changed = true;
            }
        }
    }

    DebugViewMode mode() const { return m_available[m_index]; }
    uint32_t modeIndex() const { return m_index; }
    // Programmatic selection, the same effect as pressing the digit key.  Kept
    // for scripted/reproducible captures; out-of-range values are ignored.
    void select(uint32_t index)
    {
        if (index < m_available.size() && index != m_index) {
            m_index = index;
            m_changed = true;
        }
    }
    const std::vector<DebugViewMode> &availableModes() const { return m_available; }
    // Whether the mode changed on the last poll(); used to print one log line.
    bool changed() const { return m_changed; }
    void clearChanged() { m_changed = false; }

    // viewport is the swapchain size in pixels, content is the intermediate
    // image size in pixels.  content is only read by modes that need the source
    // resolution, such as eNativeScale.
    DebugViewPushConstant pushConstant(vk::Extent2D viewport, vk::Extent2D content) const
    {
        DebugViewPushConstant pc;
        pc.mode = static_cast<uint32_t>(mode());
        // Must match MakeCameraMatrix(): glm::perspective(45deg, aspect, 0.1f, 100.0f).
        pc.nearPlane = 0.1f;
        pc.farPlane = 100.0f;
        // The demo scenes live within roughly +/-3 units, so 5 keeps the position
        // view's colour coding visible without saturating everywhere.
        pc.referenceScale = 5.0f;
        pc.viewportWidth = viewport.width > 0 ? static_cast<float>(viewport.width) : 1.0f;
        pc.viewportHeight = viewport.height > 0 ? static_cast<float>(viewport.height) : 1.0f;
        pc.contentWidth = content.width > 0 ? static_cast<float>(content.width) : 1.0f;
        pc.contentHeight = content.height > 0 ? static_cast<float>(content.height) : 1.0f;
        return pc;
    }

private:
    std::vector<DebugViewMode> m_available;
    uint32_t m_index = 0;
    std::array<bool, 10> m_digitHeld{};
    bool m_changed = false;
};

// Edge-triggered wrapper for the single keys the debug tooling owns (for
// example the "dump the G-Buffer" key).  `held` is the caller's one-bit state
// for that key; it keeps a held key from firing on every frame.
inline bool debugKeyJustPressed(const std::function<bool(int)> &isKeyDown, int key, bool &held)
{
    const bool down = isKeyDown(key);
    const bool pressed = down && !held;
    held = down;
    return pressed;
}
