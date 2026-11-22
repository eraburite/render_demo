#version 450
layout(set = 0, binding = 0) uniform sampler2D offscreenColor;

// Debug View switch (see examples/common/DebugViewMode.h).  The field order and
// types match DebugViewPushConstant exactly: eight 4-byte scalars = 32 bytes.
layout(push_constant) uniform DebugView {
    uint mode;
    float nearPlane;
    float farPlane;
    float referenceScale;
    float viewportWidth;
    float viewportHeight;
    float contentWidth;
    float contentHeight;
} debugView;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

const vec3 BACKGROUND = vec3(0.05, 0.06, 0.09);

void main() {
    // 6 = eUv: the (u, v) gradient is the fastest way to spot a flipped or
    // rotated sample; pass A's content should stay upright (u right, v down).
    if (debugView.mode == 6u) {
        outColor = vec4(fragTexCoord, 0.0, 1.0);
        return;
    }

    // 8 = eNativeScale: show the offscreen image at its own pixel size, centred,
    // instead of stretching it over the whole window.  Comparing this against the
    // stretched view tells you whether an apparent distortion comes from pass A
    // or from the blit.
    if (debugView.mode == 8u) {
        vec2 content = vec2(max(debugView.contentWidth, 1.0), max(debugView.contentHeight, 1.0));
        vec2 viewport = vec2(max(debugView.viewportWidth, 1.0), max(debugView.viewportHeight, 1.0));
        vec2 pixel = fragTexCoord * viewport - (viewport - content) * 0.5;
        if (pixel.x < 0.0 || pixel.y < 0.0 || pixel.x >= content.x || pixel.y >= content.y) {
            outColor = vec4(BACKGROUND, 1.0);
            return;
        }
        outColor = texture(offscreenColor, pixel / content);
        return;
    }

    vec4 sampled = texture(offscreenColor, fragTexCoord);

    // 7 = eLuminance: exposure / contrast check.
    if (debugView.mode == 7u) {
        float luma = dot(sampled.rgb, vec3(0.2126, 0.7152, 0.0722));
        outColor = vec4(vec3(luma), 1.0);
        return;
    }

    outColor = sampled;
}
