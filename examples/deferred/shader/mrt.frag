#version 450
// MRT outputs map 1:1 onto GBufferAttachmentDetail's three color attachments:
// location 0 = position (RGBA32F), 1 = normal (RGBA32F), 2 = albedo (RGBA8).
// Alpha 1 marks covered pixels; the clear color uses alpha 0 as a coverage mask.
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec3 fragAlbedo;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;

void main()
{
    outPosition = vec4(fragWorldPos, 1.0);
    outNormal   = vec4(normalize(fragWorldNormal), 1.0);
    outAlbedo   = vec4(fragAlbedo, 1.0);
}
