RWStructuredBuffer<float4> Output : register(u0);
Texture2D<float4> Input : register(t0);

#define HISTOGRAM_DIM 4
#define GROUP_SIZE 16

groupshared uint localHistogram[HISTOGRAM_DIM * HISTOGRAM_DIM * HISTOGRAM_DIM];

static const float kernel[3][3] = {
    { 1, 2, 1 },
    { 2, 4, 2 },
    { 1, 2, 1 }
};
static const float kernelWeight = 16.0;

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint2 imgSize;
    Input.GetDimensions(imgSize.x, imgSize.y);
    uint2 start = Gid.xy * GROUP_SIZE;
    uint2 end = min(start + GROUP_SIZE, imgSize);

    uint index = GTid.y * GROUP_SIZE + GTid.x;
    if (index < HISTOGRAM_DIM * HISTOGRAM_DIM * HISTOGRAM_DIM)
        localHistogram[index] = 0;

    GroupMemoryBarrierWithGroupSync();

    for (uint y = start.y + GTid.y; y < end.y; y += GROUP_SIZE)
    {
        for (uint x = start.x + GTid.x; x < end.x; x += GROUP_SIZE)
        {
            float3 sum = float3(0, 0, 0);
            [unroll]
            for (int ky = -1; ky <= 1; ky++)
            {
                [unroll]
                for (int kx = -1; kx <= 1; kx++)
                {
                    int2 coord = int2(x + kx, y + ky);
                    if (all(coord >= 0) && all(coord < imgSize))
                    {
                        float3 color = Input.Load(int3(coord, 0)).rgb;
                        sum += color * kernel[ky + 1][kx + 1];
                    }
                }
            }
            float3 blurred = sum / kernelWeight;

            uint3 histIndex = min(uint3(blurred * (HISTOGRAM_DIM - 1) + 0.5), HISTOGRAM_DIM - 1);
            uint histIdx = histIndex.r + histIndex.g * HISTOGRAM_DIM + histIndex.b * HISTOGRAM_DIM * HISTOGRAM_DIM;

            InterlockedAdd(localHistogram[histIdx], 1);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (index == 0)
    {
        uint maxCount = 0;
        uint maxIndex = 0;
        [unroll]
        for (uint i = 0; i < HISTOGRAM_DIM * HISTOGRAM_DIM * HISTOGRAM_DIM; i++)
        {
            if (localHistogram[i] > maxCount)
            {
                maxCount = localHistogram[i];
                maxIndex = i;
            }
        }

        uint3 dominantIndex = uint3(
            maxIndex % HISTOGRAM_DIM,
            (maxIndex / HISTOGRAM_DIM) % HISTOGRAM_DIM,
            maxIndex / (HISTOGRAM_DIM * HISTOGRAM_DIM)
        );

        float3 dominant = dominantIndex / float(HISTOGRAM_DIM - 1);

        uint2 dispatchSize = (imgSize + GROUP_SIZE - 1) / GROUP_SIZE;
        uint groupIndex = Gid.y * dispatchSize.x + Gid.x;

        Output[groupIndex] = float4(dominant, 1.0);
    }
}