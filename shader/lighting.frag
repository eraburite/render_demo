#version 450

struct MaterialParams {
    int baseColorTexId;
    int diffuseTexIdx;
    float roughness;
    float metallic;
};

layout(set = 2, binding = 0) uniform LightSource
{
    vec4 position;
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
}
lightSource;

layout(set = 2, binding = 1) uniform ViewEyePoint
{
    vec4 position;
}
viewEyePoint;

layout(set = 0, binding = 0) uniform sampler2D baseColorSampler;
layout(set = 0, binding = 1) uniform sampler2D specularSampler;
layout(set = 0, binding = 2, std430) readonly buffer MaterialPool
{
    MaterialParams params[];
}
matPool;

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec2 fragTexCoord;

layout(location = 3) in vec3 fragNormal;
layout(location = 0) out vec4 outColor;

void main()
{
    // test ssbo
    MaterialParams mat = matPool.params[0];
    float r = mat.roughness;
    float m = mat.metallic;
    int baseColorTexId = mat.baseColorTexId;
    int diffuseTexIdx = mat.diffuseTexIdx;
    mat = matPool.params[1];
    r = mat.roughness;
    m = mat.metallic;
    baseColorTexId = mat.baseColorTexId;
    diffuseTexIdx = mat.diffuseTexIdx;
    vec3 color = texture(baseColorSampler, fragTexCoord).rgb;
    vec3 ambient = lightSource.ambient.xyz * color;
    vec3 lightDir = normalize(lightSource.position.xyz - fragPos);
    vec3 normal = normalize(fragNormal);
    float diff = max(dot(lightDir, normal), 0.0);
    vec3 diffuse = lightSource.diffuse.xyz * (diff * color);

    vec3 viewDir = normalize(viewEyePoint.position.xyz - fragPos);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    // vec3 halfwayDir = normalize(lightDir+viewDir);
    // float spec = pow(max(dot( normal,halfwayDir), 0.0), 8.0);
    // vec3 specular = lightSource. specular. xyz*spec;
    vec3 specular = lightSource.specular.xyz * spec * texture(specularSampler, fragTexCoord).rgb;
    outColor = vec4(ambient + diffuse + specular, 1.0);
}
