// IbVbHash.hlsl -- stateless WWMI character recognition by the POSITION VB hash (3dmigoto's ini `hash=`).
// One dispatch of ONE group of 1024 threads (runs once per (ib, vb) pair, cached, deferred):
//   pass 1: max vertex index over the IB, each thread a strided subset, InterlockedMax into groupshared
//           -> vertexCount = max+1 (the vanilla vertex count, which the ini does not carry)
//   pass 2: crc32c of the VB prefix [0 : vertexCount*stride] in 1024 contiguous 4-byte-aligned slices,
//           one per thread, in place through the SRV; every thread writes (crc, sliceBytes)
// The CPU combines the 1024 slice CRCs (zlib's crc32_combine, Castagnoli polynomial) and chains the
// D3D11_BUFFER_DESC: crc32c_std(crc, {vbBytes, Usage=0, Bind=0x9 (VERTEX|SHADER_RESOURCE), 0,0,0}).
//
// PARALLEL: one thread over a 4.5 MB prefix holds the GPU for over a second (a TDR risk); 1024 slices of
// it are 4.4 KB each.
//
// CLAMPED to the VB view's extent (g_vbExtent), so the CPU never chains a ByteWidth the buffer does not have.
//
// Out layout: [0] vbBytes (clamped)  [4] maxIdx  then per thread t at 8 + t*8: (crc32c of its slice, bytes).

ByteAddressBuffer   IB   : register(t0);   // game index buffer  [aligned_base .. +g_ibTotalBytes]
ByteAddressBuffer   VB   : register(t1);   // game position VB   [base .. base+g_vbExtent]
ByteAddressBuffer   Args : register(t2);   // indirect draw args (when g_flags & 1), else dummy SRV
RWByteAddressBuffer Out  : register(u0);

cbuffer Params : register(b0) {
    uint g_ibBytes;
    uint g_idxSize;
    uint g_vbStride;
    uint g_vbExtent;
    uint g_ibShift;
    uint g_flags;
    uint g_argsOffset;
    uint g_ibTotalBytes;
    uint g_outBase;
};

#define THREADS 1024u

groupshared uint gMax;

uint crc_byte(uint crc, uint b)
{
    crc ^= (b & 0xFFu);
    [unroll] for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (0x82F63B78u & (uint)(-(int)(crc & 1u)));
    return crc;
}

[numthreads(1024, 1, 1)]
void main(uint tid : SV_GroupIndex)
{
    if (tid == 0u) gMax = 0u;
    GroupMemoryBarrierWithGroupSync();

    uint ibShift = g_ibShift;
    uint ibBytes = g_ibBytes;
    uint vbBase = 0u;
    uint vbAvail = g_vbExtent;
    uint skipCode = 0u;
    if ((g_flags & 1u) != 0u) {
        const uint idxCount = Args.Load(g_argsOffset + 0u);
        const uint firstIdx = Args.Load(g_argsOffset + 8u);
        const int baseVert = asint(Args.Load(g_argsOffset + 12u));
        if (idxCount == 0u) {
            ibBytes = 0u;
            vbAvail = 0u;
            skipCode = 0xFFFFFFFEu;
        }
        else if (idxCount > 65536u || firstIdx > 0x1FFFFFFFu || baseVert < 0) {
            ibBytes = 0u;
            vbAvail = 0u;
            skipCode = 0xFFFFFFFDu;
        }
        else {
            const uint startByte = g_ibShift + firstIdx * g_idxSize;
            const uint needBytes = idxCount * g_idxSize;
            const uint vbOff = (uint)baseVert * g_vbStride;
            if (startByte >= g_ibTotalBytes || needBytes > g_ibTotalBytes - startByte || vbOff >= g_vbExtent) {
                ibBytes = 0u;
                vbAvail = 0u;
                skipCode = 0xFFFFFFFDu;
            }
            else {
                ibShift = startByte;
                ibBytes = needBytes;
                vbBase = vbOff;
                vbAvail = g_vbExtent - vbOff;
            }
        }
    }

    // pass 1: max index over the IB, strided
    const uint nIdx = g_idxSize > 0u ? (ibBytes / g_idxSize) : 0u;
    uint localMax = 0u;
    for (uint i = tid; i < nIdx; i += THREADS) {
        uint v;
        if (g_idxSize == 2u) {
            uint b = ibShift + i * 2u;
            uint w = IB.Load(b & ~3u);
            v = (b & 2u) ? (w >> 16) : (w & 0xFFFFu);
        }
        else {
            v = IB.Load(ibShift + i * 4u);
            if (v == 0xFFFFFFFFu) v = 0u;
        }
        localMax = max(localMax, v);
    }
    InterlockedMax(gMax, localMax);
    GroupMemoryBarrierWithGroupSync();

    const uint maxIdx = gMax;
    uint vbBytes = 0u;
    if (skipCode == 0u && vbAvail > 0u && g_vbStride > 0u) {
        const uint maxVerts = vbAvail / g_vbStride;
        const uint vertCount = (maxIdx >= maxVerts) ? maxVerts : (maxIdx + 1u);
        vbBytes = min(vertCount * g_vbStride, vbAvail);
    }

    // pass 2: this thread's slice of the prefix, 4-byte aligned so every Load is aligned
    const uint slice = (((vbBytes + THREADS - 1u) / THREADS) + 3u) & ~3u;
    const uint lo = min(tid * slice, vbBytes);
    const uint hi = min(lo + slice, vbBytes);
    uint crc = 0xFFFFFFFFu;
    uint j = lo;
    for (; j + 4u <= hi; j += 4u) {
        uint w = VB.Load(vbBase + j);
        crc = crc_byte(crc, w);
        crc = crc_byte(crc, w >> 8);
        crc = crc_byte(crc, w >> 16);
        crc = crc_byte(crc, w >> 24);
    }
    if (j < hi) {
        uint w = VB.Load(vbBase + j);
        uint s = 0u;
        for (; j < hi; ++j, s += 8u) crc = crc_byte(crc, w >> s);
    }

    Out.Store2(g_outBase + 8u + tid * 8u, uint2(crc ^ 0xFFFFFFFFu, hi - lo));
    if (tid == 0u) Out.Store2(g_outBase, uint2(vbBytes, skipCode != 0u ? skipCode : maxIdx));
}
