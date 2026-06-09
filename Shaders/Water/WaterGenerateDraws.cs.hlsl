struct FTileSource {
    uint Data0;
    uint Data1;
    uint Data2;
    uint Pad;
};

struct FTileInstance {
    uint Data0;
    uint Data1;
    uint Data2;
};

struct FDrawBucketSource {
    uint IndexStart;
    uint IndexCount;
    uint InstanceStart;
    uint InstanceCount;
};

struct FDrawIndexedArgs {
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    uint BaseVertexLocation;
    uint StartInstanceLocation;
};

cbuffer GenerateDrawsCB : register(b0) {
    uint TileCount;
    uint TileBase;
    uint BucketBase;
    uint BucketCount;
    uint PassMode;
    uint ScratchBase;
    uint OutputBase;
    uint BuildLevelCount;
    uint BuildRingRadius;
    uint BuildDepth;
    uint BuildCameraCellX;
    uint BuildCameraCellZ;
    uint DebugBase;
    uint Pad1;
    uint Pad2;
    uint Pad3;
    float4 ViewProjRow0;
    float4 ViewProjRow1;
    float4 ViewProjRow2;
    float4 ViewProjRow3;
    float4 BuildWorldParams; // x=rootX y=rootZ z=leafSize w=waterLevel
    float4 BuildCullParams;  // x=boundsPad y=enableFrustumCull
};

StructuredBuffer<FTileSource> TileSources : register(t0);
StructuredBuffer<FDrawBucketSource> DrawBuckets : register(t1);
RWStructuredBuffer<FTileInstance> TileInstances : register(u0);
RWStructuredBuffer<FDrawIndexedArgs> DrawArgs : register(u1);
RWStructuredBuffer<uint> BucketScratch : register(u2);
RWStructuredBuffer<FTileSource> GpuTileSources : register(u3);
RWStructuredBuffer<uint> DebugCounters : register(u4);

static const uint kDebugCandidateThreads = 0u;
static const uint kDebugValidTiles       = 1u;
static const uint kDebugOutOfBounds      = 2u;
static const uint kDebugCoveredByFiner   = 3u;
static const uint kDebugFrustumCulled    = 4u;
static const uint kDebugCountedTiles     = 5u;
static const uint kDebugDrawCommands     = 6u;
static const uint kDebugScatteredTiles   = 7u;
static const uint kDebugTileCount        = 8u;
static const uint kDebugLevelCount       = 9u;
static const uint kDebugRingRadius       = 10u;
static const uint kDebugDepth            = 11u;
static const uint kDebugCameraCellX      = 12u;
static const uint kDebugCameraCellZ      = 13u;
static const uint kDebugCounterCount     = 16u;

uint PackTileData0(uint NodeX, uint NodeZ, uint NodeScaleLOD, uint DensityIndex) {
    return (NodeX & 0x7FFu) |
           ((NodeZ & 0x7FFu) << 11u) |
           ((NodeScaleLOD & 0x1Fu) << 22u) |
           ((DensityIndex & 0x1Fu) << 27u);
}

uint PackTileData2(uint MorphUnorm, uint Pattern, uint CoverageUnorm) {
    return (MorphUnorm & 0xFFu) |
           ((Pattern & 0xFFu) << 8u) |
           ((CoverageUnorm & 0xFFu) << 16u);
}

uint PatternIndex(uint Left, uint Right, uint Bottom, uint Top) {
    return Left + Right * 3u + Bottom * 9u + Top * 27u;
}

uint IAbs(int V) {
    return (V < 0) ? (uint)(-V) : (uint)V;
}

uint DensityForLevel(uint Level) {
    return min(Level >> 1u, 2u);
}

uint EdgePatternToCoarserLevel(uint Level) {
    if (Level + 1u >= BuildLevelCount) {
        return 0u;
    }

    const uint Density = DensityForLevel(Level);
    const uint CoarserDensity = DensityForLevel(Level + 1u);
    const uint CellScale = 2u << (CoarserDensity - Density);
    return (CellScale >= 4u) ? 2u : 1u;
}

bool IsCoveredByFinerLevels(uint Level, uint NodeX, uint NodeZ, uint ScaleCells) {
    if (Level == 0u) {
        return false;
    }

    const uint RootCells = 1u << BuildDepth;
    const uint PrevScaleCells = max(ScaleCells >> 1u, 1u);
    const uint PrevCenterNodeX = BuildCameraCellX / PrevScaleCells;
    const uint PrevCenterNodeZ = BuildCameraCellZ / PrevScaleCells;

    const uint PrevMinNodeX = (PrevCenterNodeX > BuildRingRadius) ? (PrevCenterNodeX - BuildRingRadius) : 0u;
    const uint PrevMinNodeZ = (PrevCenterNodeZ > BuildRingRadius) ? (PrevCenterNodeZ - BuildRingRadius) : 0u;
    const uint PrevMaxNodeX = PrevCenterNodeX + BuildRingRadius;
    const uint PrevMaxNodeZ = PrevCenterNodeZ + BuildRingRadius;

    const uint PrevMinCellX = min(PrevMinNodeX * PrevScaleCells, RootCells - 1u);
    const uint PrevMinCellZ = min(PrevMinNodeZ * PrevScaleCells, RootCells - 1u);
    const uint PrevMaxCellX = min((PrevMaxNodeX + 1u) * PrevScaleCells - 1u, RootCells - 1u);
    const uint PrevMaxCellZ = min((PrevMaxNodeZ + 1u) * PrevScaleCells - 1u, RootCells - 1u);

    const uint MinCellX = min(NodeX * ScaleCells, RootCells - 1u);
    const uint MinCellZ = min(NodeZ * ScaleCells, RootCells - 1u);
    const uint MaxCellX = min((NodeX + 1u) * ScaleCells - 1u, RootCells - 1u);
    const uint MaxCellZ = min((NodeZ + 1u) * ScaleCells - 1u, RootCells - 1u);

    return MinCellX >= PrevMinCellX && MaxCellX <= PrevMaxCellX &&
           MinCellZ >= PrevMinCellZ && MaxCellZ <= PrevMaxCellZ;
}

float4 ClipAt(float X, float Y, float Z) {
    return float4(
        X * ViewProjRow0.x + Y * ViewProjRow1.x + Z * ViewProjRow2.x + ViewProjRow3.x,
        X * ViewProjRow0.y + Y * ViewProjRow1.y + Z * ViewProjRow2.y + ViewProjRow3.y,
        X * ViewProjRow0.z + Y * ViewProjRow1.z + Z * ViewProjRow2.z + ViewProjRow3.z,
        X * ViewProjRow0.w + Y * ViewProjRow1.w + Z * ViewProjRow2.w + ViewProjRow3.w
    );
}

bool TileIntersectsFrustum(uint NodeX, uint NodeZ, uint ScaleCells) {
    if (BuildCullParams.y < 0.5f) {
        return true;
    }

    const float TileSize = BuildWorldParams.z * (float)ScaleCells;
    const float Pad = max(BuildCullParams.x, 0.0f);
    const float X0 = BuildWorldParams.x + (float)NodeX * TileSize - Pad;
    const float X1 = BuildWorldParams.x + (float)(NodeX + 1u) * TileSize + Pad;
    const float Z0 = BuildWorldParams.y + (float)NodeZ * TileSize - Pad;
    const float Z1 = BuildWorldParams.y + (float)(NodeZ + 1u) * TileSize + Pad;
    const float Y0 = BuildWorldParams.w - Pad;
    const float Y1 = BuildWorldParams.w + Pad;

    bool OutsideLeft = true;
    bool OutsideRight = true;
    bool OutsideBottom = true;
    bool OutsideTop = true;
    bool OutsideNear = true;
    bool OutsideFar = true;

    [unroll]
    for (uint Xi = 0u; Xi < 2u; ++Xi) {
        [unroll]
        for (uint Yi = 0u; Yi < 2u; ++Yi) {
            [unroll]
            for (uint Zi = 0u; Zi < 2u; ++Zi) {
                const float4 C = ClipAt(Xi != 0u ? X1 : X0,
                                        Yi != 0u ? Y1 : Y0,
                                        Zi != 0u ? Z1 : Z0);
                OutsideLeft = OutsideLeft && (C.x < -C.w);
                OutsideRight = OutsideRight && (C.x > C.w);
                OutsideBottom = OutsideBottom && (C.y < -C.w);
                OutsideTop = OutsideTop && (C.y > C.w);
                OutsideNear = OutsideNear && (C.z < 0.0f);
                OutsideFar = OutsideFar && (C.z > C.w);
            }
        }
    }

    return !(OutsideLeft || OutsideRight || OutsideBottom || OutsideTop ||
             OutsideNear || OutsideFar);
}

FTileSource InvalidTileSource() {
    FTileSource Source;
    Source.Data0 = 0u;
    Source.Data1 = 0xFFFFFFFFu;
    Source.Data2 = 0u;
    Source.Pad = 0u;
    return Source;
}

[numthreads(64, 1, 1)]
void main(uint3 DispatchThreadId : SV_DispatchThreadID) {
    const uint I = DispatchThreadId.x;
    const uint CountsBase = ScratchBase;
    const uint OffsetsBase = ScratchBase + BucketCount;
    const uint CursorsBase = ScratchBase + BucketCount * 2u;

    if (PassMode == 4u) {
        if (I >= TileCount) {
            return;
        }
        InterlockedAdd(DebugCounters[DebugBase + kDebugCandidateThreads], 1u);

        const uint CellsPerSide = BuildRingRadius * 2u + 1u;
        const uint CandidatesPerLevel = CellsPerSide * CellsPerSide;
        const uint Level = I / CandidatesPerLevel;
        const uint Local = I - Level * CandidatesPerLevel;
        const uint LocalX = Local % CellsPerSide;
        const uint LocalZ = Local / CellsPerSide;
        const int Dx = (int)LocalX - (int)BuildRingRadius;
        const int Dz = (int)LocalZ - (int)BuildRingRadius;

        FTileSource Source = InvalidTileSource();
        if (Level < BuildLevelCount && Level <= BuildDepth) {
            const uint ScaleCells = 1u << Level;
            const uint NodesPerAxis = 1u << (BuildDepth - Level);

            const int CenterNodeX = (int)(BuildCameraCellX / ScaleCells);
            const int CenterNodeZ = (int)(BuildCameraCellZ / ScaleCells);
            const int NodeXi = CenterNodeX + Dx;
            const int NodeZi = CenterNodeZ + Dz;

            if (NodeXi >= 0 && NodeZi >= 0 &&
                (uint)NodeXi < NodesPerAxis && (uint)NodeZi < NodesPerAxis) {
                const uint NodeX = (uint)NodeXi;
                const uint NodeZ = (uint)NodeZi;
                const bool CoveredByFiner = IsCoveredByFinerLevels(Level, NodeX, NodeZ, ScaleCells);
                const bool Visible = !CoveredByFiner && TileIntersectsFrustum(NodeX, NodeZ, ScaleCells);
                if (Visible) {
                    const uint CoarserPattern = EdgePatternToCoarserLevel(Level);

                    const uint Left = (CoarserPattern != 0u && Dx == -(int)BuildRingRadius && NodeX > 0u) ? CoarserPattern : 0u;
                    const uint Right = (CoarserPattern != 0u && Dx == (int)BuildRingRadius && NodeX + 1u < NodesPerAxis) ? CoarserPattern : 0u;
                    const uint Top = (CoarserPattern != 0u && Dz == -(int)BuildRingRadius && NodeZ > 0u) ? CoarserPattern : 0u;
                    const uint Bottom = (CoarserPattern != 0u && Dz == (int)BuildRingRadius && NodeZ + 1u < NodesPerAxis) ? CoarserPattern : 0u;
                    const uint Pattern = PatternIndex(Left, Right, Bottom, Top);
                    const uint DensityIndex = DensityForLevel(Level);
                    const uint RangeIndex = DensityIndex * 81u + Pattern;

                    Source.Data0 = PackTileData0(NodeX, NodeZ, Level, DensityIndex);
                    Source.Data1 = RangeIndex;
                    Source.Data2 = PackTileData2(0u, Pattern, 255u);
                    Source.Pad = 0u;
                    InterlockedAdd(DebugCounters[DebugBase + kDebugValidTiles], 1u);
                } else if (CoveredByFiner) {
                    InterlockedAdd(DebugCounters[DebugBase + kDebugCoveredByFiner], 1u);
                } else {
                    InterlockedAdd(DebugCounters[DebugBase + kDebugFrustumCulled], 1u);
                }
            } else {
                InterlockedAdd(DebugCounters[DebugBase + kDebugOutOfBounds], 1u);
            }
        } else {
            InterlockedAdd(DebugCounters[DebugBase + kDebugOutOfBounds], 1u);
        }

        GpuTileSources[TileBase + I] = Source;
        return;
    }

    if (PassMode == 0u) {
        if (I < kDebugCounterCount) {
            DebugCounters[DebugBase + I] = 0u;
            if (I == kDebugTileCount) DebugCounters[DebugBase + I] = TileCount;
            if (I == kDebugLevelCount) DebugCounters[DebugBase + I] = BuildLevelCount;
            if (I == kDebugRingRadius) DebugCounters[DebugBase + I] = BuildRingRadius;
            if (I == kDebugDepth) DebugCounters[DebugBase + I] = BuildDepth;
            if (I == kDebugCameraCellX) DebugCounters[DebugBase + I] = BuildCameraCellX;
            if (I == kDebugCameraCellZ) DebugCounters[DebugBase + I] = BuildCameraCellZ;
        }
        if (I >= BucketCount) {
            return;
        }

        BucketScratch[CountsBase + I] = 0u;
        BucketScratch[OffsetsBase + I] = 0u;
        BucketScratch[CursorsBase + I] = 0u;

        const FDrawBucketSource Bucket = DrawBuckets[BucketBase + I];
        FDrawIndexedArgs Args;
        Args.IndexCountPerInstance = Bucket.IndexCount;
        Args.InstanceCount = 0u;
        Args.StartIndexLocation = Bucket.IndexStart;
        Args.BaseVertexLocation = 0u;
        Args.StartInstanceLocation = 0u;
        DrawArgs[BucketBase + I] = Args;
        return;
    }

    if (PassMode == 1u) {
        if (I >= TileCount) {
            return;
        }

        const FTileSource Source = TileSources[TileBase + I];
        const uint BucketIndex = Source.Data1;
        if (BucketIndex < BucketCount &&
            DrawBuckets[BucketBase + BucketIndex].IndexCount > 0u) {
            uint OldValue;
            InterlockedAdd(BucketScratch[CountsBase + BucketIndex], 1u, OldValue);
            InterlockedAdd(DebugCounters[DebugBase + kDebugCountedTiles], 1u);
        }
        return;
    }

    if (PassMode == 2u) {
        if (I != 0u) {
            return;
        }

        uint Prefix = 0u;
        for (uint BucketIndex = 0u; BucketIndex < BucketCount; ++BucketIndex) {
            const uint Count = BucketScratch[CountsBase + BucketIndex];
            BucketScratch[OffsetsBase + BucketIndex] = Prefix;
            BucketScratch[CursorsBase + BucketIndex] = 0u;

            const FDrawBucketSource Bucket = DrawBuckets[BucketBase + BucketIndex];
            FDrawIndexedArgs Args;
            Args.IndexCountPerInstance = Bucket.IndexCount;
            Args.InstanceCount = Count;
            Args.StartIndexLocation = Bucket.IndexStart;
            Args.BaseVertexLocation = 0u;
            Args.StartInstanceLocation = Prefix;
            DrawArgs[BucketBase + BucketIndex] = Args;

            if (Count > 0u && Bucket.IndexCount > 0u) {
                DebugCounters[DebugBase + kDebugDrawCommands] += 1u;
            }
            Prefix += Count;
        }
        return;
    }

    if (PassMode == 3u) {
        if (I >= TileCount) {
            return;
        }

        const FTileSource Source = TileSources[TileBase + I];
        const uint BucketIndex = Source.Data1;
        if (BucketIndex >= BucketCount ||
            DrawBuckets[BucketBase + BucketIndex].IndexCount == 0u) {
            return;
        }

        uint LocalIndex;
        InterlockedAdd(BucketScratch[CursorsBase + BucketIndex], 1u, LocalIndex);

        const uint Dst = OutputBase + BucketScratch[OffsetsBase + BucketIndex] + LocalIndex;
        FTileInstance Instance;
        Instance.Data0 = Source.Data0;
        Instance.Data1 = Source.Data1;
        Instance.Data2 = Source.Data2;
        TileInstances[Dst] = Instance;
        InterlockedAdd(DebugCounters[DebugBase + kDebugScatteredTiles], 1u);
    }
}
