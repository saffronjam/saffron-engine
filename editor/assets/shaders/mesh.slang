// Instanced mesh shader. Vertex inputs come from an interleaved buffer
// (position/normal/uv0); per-instance model + normal matrix + base color come from
// a storage buffer (set 2) indexed by the absolute Vulkan InstanceIndex
// (firstInstance + instance). SV_VulkanInstanceID is that builtin — plain
// SV_InstanceID is base-relative (excludes firstInstance), which would break batching.
// The push constant is the camera viewProj. Set 0 is albedo; set 1 is the directional
// light. Shading is base color * albedo * (ambient + N.L).

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldNormal : NORMAL;
    float2 uv0 : TEXCOORD0;
    float4 baseColor : COLOR0;
};

struct Camera
{
    float4x4 viewProj;
};
[[vk::push_constant]] Camera camera;

[[vk::binding(0, 0)]] Sampler2D albedo;

struct Light
{
    float4 directionAmbient;  // xyz = travel direction, w = ambient
    float4 colorIntensity;    // rgb = color, a = intensity
};
[[vk::binding(0, 1)]] ConstantBuffer<Light> light;

struct Instance
{
    float4x4 model;
    float4x4 normalMatrix;
    float4 baseColor;
};
[[vk::binding(0, 2)]] StructuredBuffer<Instance> instances;

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_VulkanInstanceID)
{
    Instance inst = instances[instanceIndex];
    VertexOutput output;
    float4 worldPos = mul(inst.model, float4(input.position, 1.0));
    output.position = mul(camera.viewProj, worldPos);
    output.worldNormal = mul((float3x3)inst.normalMatrix, input.normal);
    output.uv0 = input.uv0;
    output.baseColor = inst.baseColor;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float3 n = normalize(input.worldNormal);
    float3 lightDir = normalize(light.directionAmbient.xyz);
    float ndotl = max(dot(n, -lightDir), 0.0);
    float ambient = light.directionAmbient.w;
    float3 lit = light.colorIntensity.rgb * light.colorIntensity.a * ndotl + ambient;
    float4 tex = albedo.Sample(input.uv0);
    float3 color = tex.rgb * input.baseColor.rgb * lit;
    return float4(color, tex.a * input.baseColor.a);
}
