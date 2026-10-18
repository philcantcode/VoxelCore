// Copyright Voxel Plugin SAS. All Rights Reserved.

#pragma once

#include "VoxelMinimal.h"
#include "StaticMeshResources.h"

class FVoxelRuntimeBulkData;

namespace Voxel::Nanite
{
	class FCluster;
	struct FEncodingSettings;
}

struct VOXELCORE_API FVoxelNaniteBuilder
{
public:
	// Triangle list
	struct FMesh
	{
		TConstVoxelArrayView<int32> Indices;
		TConstVoxelArrayView<FVector3f> Positions;
		TConstVoxelArrayView<FVoxelOctahedron> Normals;
		// Optional
		TConstVoxelArrayView<FColor> Colors;
		// Optional
		TVoxelArray<TConstVoxelArrayView<FVector2f>> TextureCoordinates;
	};
	FMesh Mesh;

	// Step is 2^(-PositionPrecision)
	int32 PositionPrecision = 4;
	static constexpr int32 NormalBits = 8;

	bool bCompressVertices = false;
#if VOXEL_ENGINE_VERSION == 508
    // Opt-in page streaming; this does not construct simplified parent geometry.
    int32 MaxResidentRootPages = MAX_int32;
#endif
	uint32 UniqueId = -1;
	int32 ChunkIndex = -1;

    // Streaming requires a fresh output owner, retained until resources/readers retire.
    TUniquePtr<FStaticMeshRenderData> CreateRenderData(TSharedPtr<FVoxelRuntimeBulkData>* OutStreamingData = nullptr);
    UStaticMesh* CreateStaticMesh(TSharedPtr<FVoxelRuntimeBulkData>* OutStreamingData = nullptr);

public:
	static void ApplyRenderData(
		UStaticMesh& StaticMesh,
		TUniquePtr<FStaticMeshRenderData> RenderData);

	static UStaticMesh* CreateStaticMesh(TUniquePtr<FStaticMeshRenderData> RenderData);

private:
	using FCluster = Voxel::Nanite::FCluster;

	struct FBuildData
	{
		Nanite::FResources& Resources;
		const Voxel::Nanite::FEncodingSettings& EncodingSettings;
		TVoxelArray<TVoxelArray<TUniquePtr<FCluster>>>& Pages;
		TVoxelChunkedArray<uint8>& RootData;
		int32 NumClusters;
        int32 NumRootPages;
		const FVoxelBox& Bounds;
		const uint8 NumBitsPerIndex;
	};

	bool Build(FBuildData& BuildData);

	TVoxelArray<TUniquePtr<FCluster>> CreateClusters() const;

	TVoxelArray<TVoxelArray<TUniquePtr<FCluster>>> CreatePages(
		TVoxelArray<TUniquePtr<FCluster>>& Clusters,
		const Voxel::Nanite::FEncodingSettings& EncodingSettings) const;
};