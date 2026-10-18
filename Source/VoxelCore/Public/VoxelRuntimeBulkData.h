// Distributed under the repository's MIT license.
#pragma once
#include "VoxelCoreMinimal.h"
#include "IO/IoChunkId.h"

class FBulkData;
class FVoxelRuntimeBulkDataBackend;

#if VOXEL_ENGINE_VERSION == 508
/** Owns an immutable runtime I/O chunk. Bind fresh byte-bulk metadata; retain until readers retire. */
class VOXELCORE_API FVoxelRuntimeBulkData
{
public:
    // Mounting is game-thread-only and is done once by the VoxelCore module.
    static void Initialize();
    static void Shutdown();
    static TSharedRef<FVoxelRuntimeBulkData> Bind(FBulkData& BulkData, TArray<uint8> Bytes);
    ~FVoxelRuntimeBulkData();
    const FIoChunkId& GetChunkId() const { return ChunkId; }
    uint64 GetAllocatedSize() const { return AllocatedSize; }

private:
    FVoxelRuntimeBulkData(TSharedRef<FVoxelRuntimeBulkDataBackend> InBackend, FIoChunkId InChunkId, uint64 InAllocatedSize);
    TSharedRef<FVoxelRuntimeBulkDataBackend> Backend;
    FIoChunkId ChunkId;
    uint64 AllocatedSize;
};
#endif
