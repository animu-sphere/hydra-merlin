[[vk::binding(8, 0)]] Texture2D<float4> source_color : register(t0);
[[vk::binding(11, 0)]] RWTexture2D<float4> destination_color : register(u1);

[shader("compute")]
[numthreads(8, 8, 1)]
void merlin_convert_color(uint3 pixel : SV_DispatchThreadID)
{
    uint width, height;
    destination_color.GetDimensions(width, height);
    if (pixel.x < width && pixel.y < height)
        destination_color[pixel.xy] = source_color.Load(int3(pixel.xy, 0));
}
