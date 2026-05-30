// Instanced mesh shader. Vertex inputs come from an interleaved buffer
// (position/normal/uv0); per-instance model + normal matrix + base color come from
// a storage buffer (set 2) indexed by the absolute Vulkan InstanceIndex
// (firstInstance + instance). SV_VulkanInstanceID is that builtin — plain
// SV_InstanceID is base-relative (excludes firstInstance), which would break batching.
// The push constant is the camera viewProj. Set 0 is albedo; set 1 is the directional
// light (binding 0) plus the punctual point/spot lights (binding 1). Shading is
// base color * albedo * (ambient + directional + sum of punctual contributions).

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPos : POSITION0;
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

struct LightGlobals
{
    float4 directionAmbient;  // xyz = travel direction, w = ambient
    float4 colorIntensity;    // rgb = color, a = intensity
    uint4 counts;             // x = punctual light count
};
[[vk::binding(0, 1)]] ConstantBuffer<LightGlobals> globals;

struct GpuLight
{
    float4 positionRange;   // xyz = world position, w = range
    float4 colorIntensity;  // rgb = color, a = intensity
    float4 directionType;   // xyz = world direction (spot), w = type (0 point, 1 spot)
    float4 spotCos;         // x = cos(inner half-angle), y = cos(outer half-angle)
};
[[vk::binding(1, 1)]] StructuredBuffer<GpuLight> lights;

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
    output.worldPos = worldPos.xyz;
    output.position = mul(camera.viewProj, worldPos);
    output.worldNormal = mul((float3x3)inst.normalMatrix, input.normal);
    output.uv0 = input.uv0;
    output.baseColor = inst.baseColor;
    return output;
}

// UE4-style punctual attenuation: inverse-square with a smooth range window.
float distanceAttenuation(float dist, float range)
{
    float invSquare = 1.0 / max(dist * dist, 0.0001);
    float t = saturate(1.0 - pow(dist / range, 4.0));
    return invSquare * t * t;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float3 n = normalize(input.worldNormal);
    float4 tex = albedo.Sample(input.uv0);

    // Directional light + ambient.
    float3 lightDir = normalize(globals.directionAmbient.xyz);
    float ndotl = max(dot(n, -lightDir), 0.0);
    float3 lit = globals.colorIntensity.rgb * globals.colorIntensity.a * ndotl + globals.directionAmbient.w;

    // Punctual point/spot lights.
    for (uint i = 0; i < globals.counts.x; i = i + 1)
    {
        GpuLight lt = lights[i];
        float3 toLight = lt.positionRange.xyz - input.worldPos;
        float dist = length(toLight);
        if (dist > lt.positionRange.w)
        {
            continue;
        }
        float3 l = toLight / max(dist, 0.0001);
        float nl = max(dot(n, l), 0.0);
        float attenuation = distanceAttenuation(dist, lt.positionRange.w);

        float cone = 1.0;
        if (lt.directionType.w > 0.5)
        {
            float3 spotDir = normalize(lt.directionType.xyz);
            float cosAngle = dot(spotDir, -l);
            cone = smoothstep(lt.spotCos.y, lt.spotCos.x, cosAngle);
        }
        lit += lt.colorIntensity.rgb * lt.colorIntensity.a * nl * attenuation * cone;
    }

    float3 color = tex.rgb * input.baseColor.rgb * lit;
    return float4(color, tex.a * input.baseColor.a);
}
