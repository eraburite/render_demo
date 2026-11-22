#version 450
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec3 fragAlbedo;
layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;

void main() {
    outPosition = vec4(fragWorldPos, 1.0);
    outNormal = vec4(normalize(fragWorldNormal), 1.0);
    outAlbedo = vec4(fragAlbedo, 1.0);
}
