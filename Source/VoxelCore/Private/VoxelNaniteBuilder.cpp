// Copyright Voxel Plugin SAS. All Rights Reserved.

#include "VoxelNaniteBuilder.h"
#include "VoxelNanite.h"
#include "VoxelRuntimeBulkData.h"
#include "Engine/StaticMesh.h"
#include "Rendering/NaniteResources.h"

#if VOXEL_ENGINE_VERSION >= 507
#include "Nanite/NaniteFixupChunk.h"
#endif

TUniquePtr<FStaticMeshRenderData> FVoxelNaniteBuilder::CreateRenderData(TSharedPtr<FVoxelRuntimeBulkData>* OutStreamingData)
{
	VOXEL_FUNCTION_COUNTER();
	check(Mesh.Positions.Num() == Mesh.Normals.Num());
	check(Mesh.Positions.Num() % 3 == 0 || (Mesh.Indices.Num() > 0 && Mesh.Indices.Num() % 3 == 0));

	if (!ensure(Mesh.Positions.Num() > 0))
	{
		return nullptr;
	}

	using namespace Voxel::Nanite;

	const FVoxelBox Bounds = FVoxelBox::FromPositions(Mesh.Positions);

	Nanite::FResources Resources;

	TVoxelArray<TUniquePtr<FCluster>> AllClusters = CreateClusters(Mesh);

	FEncodingSettings EncodingSettings;
	EncodingSettings.PositionPrecision = PositionPrecision;
	checkStatic(FEncodingSettings::NormalBits == NormalBits);

	TVoxelArray<TVoxelArray<TUniquePtr<FCluster>>> Pages = CreatePages(AllClusters, EncodingSettings);

    int32 NumRootPages = Pages.Num();
    bool bHasCoarseMesh = false;
    float ParentError = 0.0f;
    int32 NumClusters = AllClusters.Num();
#if VOXEL_ENGINE_VERSION == 508
    bHasCoarseMesh = !CoarseMesh.Positions.IsEmpty();
    if (bHasCoarseMesh)
    {
        if (!ensure(CoarseError > 0 && FMath::IsFinite(CoarseError) &&
            CoarseMesh.Positions.Num() == CoarseMesh.Normals.Num() &&
            CoarseMesh.Indices.Num() > 0 && CoarseMesh.Indices.Num() % 3 == 0))
            return nullptr;
        // Round upward so parent/child cuts use the same conservative half error.
        FFloat16 HalfError(CoarseError);
        if (float(HalfError) < CoarseError)
            ++HalfError.Encoded;
        ParentError = float(HalfError);
        if (!ensure(FMath::IsFinite(ParentError)))
            return nullptr;
        auto CoarseClusters = CreateClusters(CoarseMesh);
        NumClusters += CoarseClusters.Num();
        auto CoarsePages = CreatePages(CoarseClusters, EncodingSettings);
        NumRootPages = CoarsePages.Num();
        if (!ensure(Pages.Num() <= NANITE_PAGE_RANGE_KEY_MAX_COUNT))
            return nullptr;
        for (auto& Page : CoarsePages)
            for (auto& Cluster : Page)
            {
                Cluster->LODError = ParentError;
                Cluster->GroupLODBounds = Bounds;
                Cluster->FlagsOverride = NANITE_CLUSTER_FLAG_ROOT_GROUP |
                    NANITE_CLUSTER_FLAG_ROOT_LEAF | NANITE_CLUSTER_FLAG_STREAMING_LEAF;
            }
        for (auto& Page : Pages)
            for (auto& Cluster : Page)
            {
                Cluster->LODError = 0.0f;
                Cluster->GroupLODBounds = Bounds;
                Cluster->FlagsOverride = NANITE_CLUSTER_FLAG_FULL_LEAF |
                    NANITE_CLUSTER_FLAG_ROOT_LEAF | NANITE_CLUSTER_FLAG_STREAMING_LEAF;
            }
        CoarsePages.Append(MoveTemp(Pages));
        Pages = MoveTemp(CoarsePages);
    }
    else
        NumRootPages = FMath::Clamp(MaxResidentRootPages, 1, Pages.Num());
    if (NumRootPages < Pages.Num() && !ensureMsgf(OutStreamingData && !OutStreamingData->IsValid(),
        TEXT("Streamable Nanite data requires a fresh registration output owner")))
        return nullptr;
#endif
	TVoxelChunkedArray<uint8> RootData;

	FBuildData BuildData{
		Resources,
		EncodingSettings,
		Pages,
		RootData,
		NumClusters,
        NumRootPages,
        bHasCoarseMesh,
        ParentError,
		Bounds,
		FMath::FloorLog2(Mesh.Positions.Num()) + 1
	};
	if (!Build(BuildData))
	{
		return nullptr;
	}

    auto AllPageData = RootData.Array();
#if VOXEL_ENGINE_VERSION == 508
    if (NumRootPages < Pages.Num())
    {
        const auto& LastRoot = Resources.PageStreamingStates[NumRootPages - 1];
        const int32 RootEnd = LastRoot.BulkOffset + LastRoot.BulkSize;
        TArray<uint8> StreamingBytes;
        StreamingBytes.Append(AllPageData.GetData() + RootEnd, AllPageData.Num() - RootEnd);
        Resources.RootData.Append(AllPageData.GetData(), RootEnd);
        for (int32 PageIndex = NumRootPages; PageIndex < Resources.PageStreamingStates.Num(); PageIndex++)
            Resources.PageStreamingStates[PageIndex].BulkOffset -= RootEnd;
        *OutStreamingData = FVoxelRuntimeBulkData::Bind(Resources.StreamablePages, MoveTemp(StreamingBytes));
    }
    else
#endif
        Resources.RootData = MoveTemp(AllPageData);
	Resources.PositionPrecision = PositionPrecision;
	Resources.NormalPrecision = NormalBits;
	Resources.NumInputTriangles = Mesh.Indices.Num() / 3;
	Resources.NumInputVertices = Mesh.Positions.Num();
#if VOXEL_ENGINE_VERSION < 506
	Resources.NumInputMeshes = 1;
	Resources.NumInputTexCoords = Mesh.TextureCoordinates.Num();
#endif
	Resources.NumClusters = NumClusters;
	Resources.NumRootPages = NumRootPages;
	Resources.HierarchyRootOffsets.Add(0);
#if VOXEL_ENGINE_VERSION >= 507
	Resources.MeshBounds = Bounds.ToFBox3f();
#endif

	TUniquePtr<FStaticMeshRenderData> RenderData = MakeUnique<FStaticMeshRenderData>();
	RenderData->Bounds = Bounds.ToFBox();
	RenderData->NumInlinedLODs = 1;
	RenderData->NaniteResourcesPtr = MakePimpl<Nanite::FResources>(MoveTemp(Resources));

	FStaticMeshLODResources* LODResource = new FStaticMeshLODResources();
	LODResource->bBuffersInlined = true;
	LODResource->Sections.Emplace();

	// Ensure UStaticMesh::HasValidRenderData returns true
	// Use MAX_flt to try to not have the vertex picked by vertex snapping
	const TVoxelArray<FVector3f> DummyPositions = { FVector3f(MAX_flt) };

	LODResource->VertexBuffers.StaticMeshVertexBuffer.Init(DummyPositions.Num(), 1);
	LODResource->VertexBuffers.PositionVertexBuffer.Init(DummyPositions);
	LODResource->VertexBuffers.ColorVertexBuffer.Init(DummyPositions.Num());

	// Ensure FStaticMeshRenderData::GetFirstValidLODIdx doesn't return -1
	LODResource->BuffersSize = 1;

	RenderData->LODResources.Add(LODResource);

	RenderData->LODVertexFactories.Add(FStaticMeshVertexFactories(GMaxRHIFeatureLevel));

	return RenderData;
}

UStaticMesh* FVoxelNaniteBuilder::CreateStaticMesh(TSharedPtr<FVoxelRuntimeBulkData>* OutStreamingData)
{
    VOXEL_FUNCTION_COUNTER();
    auto Data = CreateRenderData(OutStreamingData);
    return Data ? CreateStaticMesh(MoveTemp(Data)) : nullptr;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

void FVoxelNaniteBuilder::ApplyRenderData(UStaticMesh& StaticMesh, TUniquePtr<FStaticMeshRenderData> RenderData)
{
	VOXEL_FUNCTION_COUNTER();

	StaticMesh.ReleaseResources();

	StaticMesh.SetStaticMaterials({ FStaticMaterial() });
	StaticMesh.SetRenderData(MoveTemp(RenderData));
	StaticMesh.CalculateExtendedBounds();
#if WITH_EDITOR
	StaticMesh.UE_507_SWITCH(NaniteSettings, GetNaniteSettings()).bEnabled = true;
#endif

	// Not supported, among other issues FSceneProxy::FSceneProxy crashes because GetNumVertices is always 0
	StaticMesh.bSupportRayTracing = false;

	VOXEL_SCOPE_COUNTER("UStaticMesh::InitResources");
	StaticMesh.InitResources();
}

UStaticMesh* FVoxelNaniteBuilder::CreateStaticMesh(TUniquePtr<FStaticMeshRenderData> RenderData)
{
	VOXEL_FUNCTION_COUNTER();
	check(IsInGameThread());

	UStaticMesh* StaticMesh = NewObject<UStaticMesh>();
	ApplyRenderData(*StaticMesh, MoveTemp(RenderData));
	return StaticMesh;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

bool FVoxelNaniteBuilder::Build(FBuildData& BuildData)
#if VOXEL_ENGINE_VERSION >= 507
{
	using namespace Voxel::Nanite;

	const int32 TreeDepth = FMath::FloorToInt32(FMath::LogX(4.f, FMath::Max(BuildData.NumClusters - 1, 1)));
	ensure(TreeDepth < NANITE_MAX_CLUSTER_HIERARCHY_DEPTH);

	const auto SetHierarchyBounds = [](Nanite::FPackedHierarchyNode& Node, const int32 Part, const FVoxelBox& Bounds)
	{
		Node.LODBounds[Part] = FVector4f(FVector3f(Bounds.GetCenter()), Bounds.Size().Length());
		Node.Misc0[Part].BoxBoundsCenter = FVector3f(Bounds.GetCenter());
		Node.Misc1[Part].BoxBoundsExtent = FVector3f(Bounds.GetExtent());
	};

	const auto MakeHierarchyNode = [&]
	{
		Nanite::FPackedHierarchyNode HierarchyNode;

		for (int32 Index = 0; Index < 4; Index++)
		{
			SetHierarchyBounds(HierarchyNode, Index, BuildData.Bounds);

			HierarchyNode.Misc0[Index].MinLODError_MaxParentLODError = FFloat16(1e10f).Encoded | (FFloat16(-1).Encoded << 16);
			HierarchyNode.Misc1[Index].ChildStartReference = 0xFFFFFFFF;
			HierarchyNode.Misc2[Index].ResourcePageRangeKey = NANITE_PAGE_RANGE_KEY_EMPTY_RANGE;
			HierarchyNode.Misc2[Index].GroupPartSize_AssemblyPartIndex = 0;
		}

		return HierarchyNode;
	};

	TVoxelArray<int32> LeafNodes;
	for (int32 Depth = 0; Depth <= TreeDepth; Depth++)
	{
		if (Depth == 0)
		{
			BuildData.Resources.HierarchyNodes.Add(MakeHierarchyNode());
			LeafNodes.Add(0);
			continue;
		}

		BuildData.Resources.HierarchyNodes.Reserve(BuildData.Resources.HierarchyNodes.Num() + 4 * LeafNodes.Num());

		TVoxelArray<int32> NewLeafNodes;
		for (const int32 ParentIndex : LeafNodes)
		{
			Nanite::FPackedHierarchyNode& ParentNode = BuildData.Resources.HierarchyNodes[ParentIndex];
			for (int32 Index = 0; Index < 4; Index++)
			{
				const int32 ChildIndex = BuildData.Resources.HierarchyNodes.Add(MakeHierarchyNode());

				ensure(ParentNode.Misc1[Index].ChildStartReference == 0xFFFFFFFF);
				ensure(
					ParentNode.Misc2[Index].ResourcePageRangeKey == NANITE_PAGE_RANGE_KEY_EMPTY_RANGE &&
					ParentNode.Misc2[Index].GroupPartSize_AssemblyPartIndex == 0);

				ParentNode.Misc1[Index].ChildStartReference = ChildIndex;

				ParentNode.Misc2[Index].ResourcePageRangeKey = 0xFFFFFFFFu;
				ParentNode.Misc2[Index].GroupPartSize_AssemblyPartIndex =
					(0xFFFFFFFFu & NANITE_HIERARCHY_MAX_ASSEMBLY_TRANSFORMS) |
					(0 << NANITE_HIERARCHY_ASSEMBLY_TRANSFORM_INDEX_BITS);

				NewLeafNodes.Add(ChildIndex);
			}
		}

		LeafNodes = MoveTemp(NewLeafNodes);
	}
	check(BuildData.NumClusters >= LeafNodes.Num());

	struct FClusterHierarchyNode
	{
		int32 HierarchyNodeIndex;
		int32 NodePartIndex;
	};
	TVoxelArray<FClusterHierarchyNode> ClusterIndexToLeafNode;
	ClusterIndexToLeafNode.Reserve(LeafNodes.Num() * 4);
	for (const int32 LeafNodeIndex : LeafNodes)
	{
		ClusterIndexToLeafNode.Add({ LeafNodeIndex, 0 });
		ClusterIndexToLeafNode.Add({ LeafNodeIndex, 1 });
		ClusterIndexToLeafNode.Add({ LeafNodeIndex, 2 });
		ClusterIndexToLeafNode.Add({ LeafNodeIndex, 3 });
	}
	if (!ensure(BuildData.NumClusters <= ClusterIndexToLeafNode.Num()))
	{
		return false;
	}

	int32 ClusterIndexOffset = 0;
	for (int32 PageIndex = 0; PageIndex < BuildData.Pages.Num(); PageIndex++)
	{
		TVoxelArray<TUniquePtr<FCluster>>& PageClusters = BuildData.Pages[PageIndex];
		ON_SCOPE_EXIT
		{
			ClusterIndexOffset += PageClusters.Num();
		};

		for (int32 ClusterIndex = 0; ClusterIndex < PageClusters.Num(); ClusterIndex++)
		{
			FCluster& Cluster = *PageClusters[ClusterIndex];

			const FClusterHierarchyNode& LeafNodeData = ClusterIndexToLeafNode[ClusterIndexOffset + ClusterIndex];
			Nanite::FPackedHierarchyNode& HierarchyNode = BuildData.Resources.HierarchyNodes[LeafNodeData.HierarchyNodeIndex];

			const FVoxelBox& ClusterBounds = Cluster.GetBounds();
			SetHierarchyBounds(HierarchyNode, LeafNodeData.NodePartIndex, ClusterBounds);

			ensure(HierarchyNode.Misc1[LeafNodeData.NodePartIndex].ChildStartReference == 0xFFFFFFFF);
			ensure(
				HierarchyNode.Misc2[LeafNodeData.NodePartIndex].ResourcePageRangeKey == NANITE_PAGE_RANGE_KEY_EMPTY_RANGE &&
				HierarchyNode.Misc2[LeafNodeData.NodePartIndex].GroupPartSize_AssemblyPartIndex == 0);

			static constexpr uint32 AssemblyPartIndex = 0xFFFFFFFFu;
			static constexpr int32 GroupPartSize = 1;
			const bool bParent = BuildData.bHasCoarseMesh && PageIndex < BuildData.NumRootPages;
            const bool bDetail = BuildData.bHasCoarseMesh && !bParent;
            if (BuildData.bHasCoarseMesh)
            {
                HierarchyNode.LODBounds[LeafNodeData.NodePartIndex] = FVector4f(FVector3f(BuildData.Bounds.GetCenter()), BuildData.Bounds.Size().Length());
                HierarchyNode.Misc0[LeafNodeData.NodePartIndex].MinLODError_MaxParentLODError =
                    FFloat16(bParent ? MAX_flt : BuildData.CoarseError).Encoded |
                    (uint32(FFloat16(bParent ? -BuildData.CoarseError : 0.0f).Encoded) << 16);
            }
            HierarchyNode.Misc2[LeafNodeData.NodePartIndex].ResourcePageRangeKey = bDetail
                ? Nanite::FPageRangeKey(BuildData.NumRootPages, BuildData.Pages.Num() - BuildData.NumRootPages, false, true).Value
                : Nanite::FPageRangeKey(PageIndex, 1, false, PageIndex >= BuildData.NumRootPages).Value;
			HierarchyNode.Misc2[LeafNodeData.NodePartIndex].GroupPartSize_AssemblyPartIndex =
				(AssemblyPartIndex & NANITE_HIERARCHY_MAX_ASSEMBLY_TRANSFORMS) |
				(GroupPartSize << NANITE_HIERARCHY_ASSEMBLY_TRANSFORM_INDEX_BITS);
		}

		Nanite::FPageStreamingState PageStreamingState{};
		PageStreamingState.BulkOffset = BuildData.RootData.Num();

		TArray<uint8> FixupChunkData;

        // Root pages each own their install fixups. With a coarse cut, the last
        // detailed page owns the atomic group's fixups; other pages reconsider it.
        {
            const bool bDetail = BuildData.bHasCoarseMesh && PageIndex >= BuildData.NumRootPages;
            const bool bLastDetail = bDetail && PageIndex == BuildData.Pages.Num() - 1;
            const bool bOwnGroup = !bDetail || bLastDetail;
            const int32 FirstPage = bDetail ? BuildData.NumRootPages : PageIndex;
            const int32 EndPage = bDetail ? BuildData.Pages.Num() : PageIndex + 1;
            int32 FirstCluster = 0;
            for (int32 Index = 0; Index < FirstPage; ++Index)
                FirstCluster += BuildData.Pages[Index].Num();
            int32 NumParts = 0, NumParents = 0;
            if (bOwnGroup)
                for (int32 Index = FirstPage; Index < EndPage; ++Index)
                    NumParts += BuildData.Pages[Index].Num();
            if (bLastDetail)
                for (int32 Index = 0; Index < BuildData.NumRootPages; ++Index)
                    NumParents += BuildData.Pages[Index].Num();
            const int32 NumReconsider = bDetail && !bLastDetail ? 1 : 0;
            const auto Size = Nanite::FFixupChunk::GetSize(bOwnGroup ? 1 : 0, NumParts,
                NumParents, NumParts, NumReconsider, NumParents);
            FixupChunkData.Init(0, Size);
            auto& Fixup = *reinterpret_cast<Nanite::FFixupChunk*>(FixupChunkData.GetData());
            Fixup.Header.Magic = NANITE_FIXUP_MAGIC;
            Fixup.Header.NumGroupFixups = bOwnGroup ? 1 : 0;
            Fixup.Header.NumPartFixups = IntCastChecked<uint16>(NumParts);
            Fixup.Header.NumClusters = IntCastChecked<uint16>(PageClusters.Num());
            Fixup.Header.NumParentFixups = NumParents;
            Fixup.Header.NumHierarchyLocations = NumParts;
            Fixup.Header.NumReconsiderPages = NumReconsider;
            Fixup.Header.NumClusterIndices = NumParents;
            if (NumReconsider)
                Fixup.GetReconsiderPageIndex(0) = IntCastChecked<uint16>(BuildData.Pages.Num() - 1);
            if (bOwnGroup)
            {
                auto& Group = Fixup.GetGroupFixup(0);
                Group.PageDependencies = Nanite::FPageRangeKey(FirstPage, EndPage - FirstPage, false, bDetail || PageIndex >= BuildData.NumRootPages);
                Group.NumPartFixups = IntCastChecked<uint16>(NumParts);
                Group.NumParentFixups = IntCastChecked<uint16>(NumParents);
                int32 PartIndex = 0, GlobalCluster = FirstCluster;
                for (int32 TargetPage = FirstPage; TargetPage < EndPage; ++TargetPage)
                    for (int32 LocalCluster = 0; LocalCluster < BuildData.Pages[TargetPage].Num(); ++LocalCluster, ++PartIndex, ++GlobalCluster)
                    {
                        auto& Part = Fixup.GetPartFixup(PartIndex);
                        Part.PageIndex = IntCastChecked<uint16>(TargetPage);
                        Part.StartClusterIndex = IntCastChecked<uint8>(LocalCluster);
                        Part.LeafCounter = BuildData.bHasCoarseMesh && !bDetail ? 1 : 0;
                        Part.FirstHierarchyLocation = PartIndex;
                        Part.NumHierarchyLocations = 1;
                        const auto& Leaf = ClusterIndexToLeafNode[GlobalCluster];
                        Fixup.GetHierarchyLocation(PartIndex) = Nanite::FFixupChunk::FHierarchyLocation(Leaf.HierarchyNodeIndex, Leaf.NodePartIndex);
                    }
                int32 ParentIndex = 0;
                for (int32 RootPage = 0; bLastDetail && RootPage < BuildData.NumRootPages; ++RootPage)
                    for (int32 LocalCluster = 0; LocalCluster < BuildData.Pages[RootPage].Num(); ++LocalCluster, ++ParentIndex)
                    {
                        auto& Parent = Fixup.GetParentFixup(ParentIndex);
                        Parent.PageIndex = IntCastChecked<uint16>(RootPage);
                        Parent.PartFixupPageIndex = IntCastChecked<uint16>(RootPage);
                        Parent.PartFixupIndex = IntCastChecked<uint16>(LocalCluster);
                        Parent.NumClusterIndices = 1;
                        Parent.FirstClusterIndex = IntCastChecked<uint16>(ParentIndex);
                        Fixup.GetClusterIndex(ParentIndex) = IntCastChecked<uint8>(LocalCluster);
                    }
            }
        }

		BuildData.RootData.Append(FixupChunkData);

		const int32 PageStartIndex = BuildData.RootData.Num();

		CreatePageData(
			PageClusters,
			BuildData.EncodingSettings,
			BuildData.RootData);

        PageStreamingState.DependenciesStart = BuildData.Resources.PageDependencies.Num();
        if (BuildData.bHasCoarseMesh && PageIndex >= BuildData.NumRootPages)
            for (int32 RootPage = 0; RootPage < BuildData.NumRootPages; ++RootPage)
                BuildData.Resources.PageDependencies.Add(IntCastChecked<uint16>(RootPage));
        PageStreamingState.DependenciesNum = IntCastChecked<uint16>(BuildData.Resources.PageDependencies.Num() - PageStreamingState.DependenciesStart);
		PageStreamingState.BulkSize = BuildData.RootData.Num() - PageStreamingState.BulkOffset;
		PageStreamingState.PageSize = BuildData.RootData.Num() - PageStartIndex;
		PageStreamingState.MaxHierarchyDepth = NANITE_MAX_CLUSTER_HIERARCHY_DEPTH;
		BuildData.Resources.PageStreamingStates.Add(PageStreamingState);
	}
	// Nodes are allocated breadth-first, so children are complete before their
	// parents in this reverse walk. Empty branches keep their existing metadata.
	TArray<FBox3f> SubtreeBounds;
	SubtreeBounds.Init(FBox3f(ForceInit), BuildData.Resources.HierarchyNodes.Num());
	for (int32 NodeIndex = BuildData.Resources.HierarchyNodes.Num() - 1; NodeIndex >= 0; NodeIndex--)
	{
		Nanite::FPackedHierarchyNode& Node = BuildData.Resources.HierarchyNodes[NodeIndex];
		for (int32 Part = 0; Part < 4; Part++)
		{
			const uint32 Child = Node.Misc1[Part].ChildStartReference;
			const bool bHasChildBounds = Child != 0xFFFFFFFFu && SubtreeBounds[Child].IsValid;
			if (bHasChildBounds)
			{
				SetHierarchyBounds(Node, Part, FVoxelBox(SubtreeBounds[Child]));
			}
			const uint32 LeafCount = Node.Misc2[Part].GroupPartSize_AssemblyPartIndex >> NANITE_HIERARCHY_ASSEMBLY_TRANSFORM_INDEX_BITS;
			if (LeafCount > 0 || bHasChildBounds)
			{
				const FVector3f Center = Node.Misc0[Part].BoxBoundsCenter;
				const FVector3f Extent = Node.Misc1[Part].BoxBoundsExtent;
				SubtreeBounds[NodeIndex] += FBox3f(Center - Extent, Center + Extent);
			}
		}
	}
	return true;
}
#else
{
	using namespace Voxel::Nanite;

	const int32 TreeDepth = FMath::CeilToInt(FMath::LogX(4.f, BuildData.NumClusters));
	ensure(TreeDepth < NANITE_MAX_CLUSTER_HIERARCHY_DEPTH);

	const auto MakeHierarchyNode = [&]
	{
		Nanite::FPackedHierarchyNode HierarchyNode;

		for (int32 Index = 0; Index < 4; Index++)
		{
			HierarchyNode.LODBounds[Index] = FVector4f(
				BuildData.Bounds.GetCenter().X,
				BuildData.Bounds.GetCenter().Y,
				BuildData.Bounds.GetCenter().Z,
				BuildData.Bounds.Size().Length());

			HierarchyNode.Misc0[Index].MinLODError_MaxParentLODError = FFloat16(-1).Encoded | (FFloat16(1e10f).Encoded << 16);
			HierarchyNode.Misc0[Index].BoxBoundsCenter = FVector3f(BuildData.Bounds.GetCenter());
			HierarchyNode.Misc1[Index].BoxBoundsExtent = FVector3f(BuildData.Bounds.GetExtent());
			HierarchyNode.Misc1[Index].ChildStartReference = 0xFFFFFFFF;
			HierarchyNode.Misc2[Index].ResourcePageIndex_NumPages_GroupPartSize = 0;
		}

		return HierarchyNode;
	};

	TVoxelArray<int32> LeafNodes;
	for (int32 Depth = 0; Depth <= TreeDepth; Depth++)
	{
		if (Depth == 0)
		{
			BuildData.Resources.HierarchyNodes.Add(MakeHierarchyNode());
			LeafNodes.Add(0);
			continue;
		}

		BuildData.Resources.HierarchyNodes.Reserve(BuildData.Resources.HierarchyNodes.Num() + 4 * LeafNodes.Num());

		TVoxelArray<int32> NewLeafNodes;
		for (const int32 ParentIndex : LeafNodes)
		{
			Nanite::FPackedHierarchyNode& ParentNode = BuildData.Resources.HierarchyNodes[ParentIndex];
			for (int32 Index = 0; Index < 4; Index++)
			{
				const int32 ChildIndex = BuildData.Resources.HierarchyNodes.Add(MakeHierarchyNode());

				ensure(ParentNode.Misc1[Index].ChildStartReference == 0xFFFFFFFF);
				ensure(ParentNode.Misc2[Index].ResourcePageIndex_NumPages_GroupPartSize == 0);

				ParentNode.Misc1[Index].ChildStartReference = ChildIndex;
				ParentNode.Misc2[Index].ResourcePageIndex_NumPages_GroupPartSize = 0xFFFFFFFF;

				NewLeafNodes.Add(ChildIndex);
			}
		}

		LeafNodes = MoveTemp(NewLeafNodes);
	}
	check(BuildData.NumClusters <= LeafNodes.Num());

	for (int32 ClusterIndex = 0; ClusterIndex < BuildData.NumClusters; ClusterIndex++)
	{
		Nanite::FPackedHierarchyNode& HierarchyNode = BuildData.Resources.HierarchyNodes[LeafNodes[ClusterIndex]];

		ensure(HierarchyNode.Misc1[0].ChildStartReference == 0xFFFFFFFF);
		ensure(HierarchyNode.Misc2[0].ResourcePageIndex_NumPages_GroupPartSize == 0);

		HierarchyNode.Misc1[0].ChildStartReference = BuildData.Resources.HierarchyNodes.Num() + ClusterIndex;
		HierarchyNode.Misc2[0].ResourcePageIndex_NumPages_GroupPartSize = 0xFFFFFFFF;
	}

	int32 VertexOffset = 0;
	int32 ClusterIndexOffset = 0;
	for (int32 PageIndex = 0; PageIndex < BuildData.Pages.Num(); PageIndex++)
	{
		TVoxelArray<TUniquePtr<FCluster>>& Clusters = BuildData.Pages[PageIndex];
		ON_SCOPE_EXIT
		{
			ClusterIndexOffset += Clusters.Num();
		};

		Nanite::FPageStreamingState PageStreamingState{};
		PageStreamingState.BulkOffset = BuildData.RootData.Num();

#if VOXEL_ENGINE_VERSION < 506
		Nanite::FFixupChunk FixupChunk;
		FixupChunk.Header.Magic = NANITE_FIXUP_MAGIC;
		FixupChunk.Header.NumClusters = Clusters.Num();
		FixupChunk.Header.UE_506_SWITCH(NumHierachyFixups, NumHierarchyFixups) = Clusters.Num();
		FixupChunk.Header.NumClusterFixups = Clusters.Num();
#else
		Nanite::FFixupChunkBuffer FixupChunkBuffer;
		Nanite::FFixupChunk& FixupChunk = FixupChunkBuffer.Add_GetRef(
			Clusters.Num(),
			Clusters.Num(),
			Clusters.Num());
#endif

#if VOXEL_ENGINE_VERSION < 506
		const TVoxelArrayView<uint8> HierarchyFixupsData = MakeVoxelArrayView(FixupChunk.Data).LeftOf(sizeof(Nanite::FHierarchyFixup) * Clusters.Num());
		const TVoxelArrayView<uint8> ClusterFixupsData = MakeVoxelArrayView(FixupChunk.Data).RightOf(sizeof(Nanite::FHierarchyFixup) * Clusters.Num());
		const TVoxelArrayView<Nanite::FHierarchyFixup> HierarchyFixups = HierarchyFixupsData.ReinterpretAs<Nanite::FHierarchyFixup>();
		const TVoxelArrayView<Nanite::FClusterFixup> ClusterFixups = ClusterFixupsData.ReinterpretAs<Nanite::FClusterFixup>().LeftOf(Clusters.Num());
#endif

		for (int32 Index = 0; Index < Clusters.Num(); Index++)
		{
			UE_506_SWITCH(HierarchyFixups[Index], FixupChunk.GetHierarchyFixup(Index)) = Nanite::FHierarchyFixup(
				PageIndex,
				BuildData.Resources.HierarchyNodes.Num() + ClusterIndexOffset + Index,
				0,
				Index,
				0,
				0);

			UE_506_SWITCH(ClusterFixups[Index], FixupChunk.GetClusterFixup(Index)) = Nanite::FClusterFixup(
				PageIndex,
				Index,
				0,
				0);
		}

#if VOXEL_ENGINE_VERSION < 506
		BuildData.RootData.Append(MakeByteVoxelArrayView(FixupChunk).LeftOf(FixupChunk.GetSize()));
#else
		BuildData.RootData.Append(TConstVoxelArrayView<uint8>(
			reinterpret_cast<uint8*>(&FixupChunk),
			FixupChunk.GetSize()));
#endif

		const int32 PageStartIndex = BuildData.RootData.Num();

		CreatePageData(
			Clusters,
			BuildData.EncodingSettings,
			BuildData.RootData);

		PageStreamingState.BulkSize = BuildData.RootData.Num() - PageStreamingState.BulkOffset;
		PageStreamingState.PageSize = BuildData.RootData.Num() - PageStartIndex;
		PageStreamingState.MaxHierarchyDepth = NANITE_MAX_CLUSTER_HIERARCHY_DEPTH;
		BuildData.Resources.PageStreamingStates.Add(PageStreamingState);
	}

	for (int32 ClusterIndex = 0; ClusterIndex < BuildData.NumClusters; ClusterIndex++)
	{
		Nanite::FPackedHierarchyNode PackedHierarchyNode;
		FMemory::Memzero(PackedHierarchyNode);

		PackedHierarchyNode.Misc0[0].BoxBoundsCenter = FVector3f(BuildData.Bounds.GetCenter());
		PackedHierarchyNode.Misc0[0].MinLODError_MaxParentLODError = FFloat16(-1).Encoded | (FFloat16(1e10f).Encoded << 16);

		PackedHierarchyNode.Misc1[0].BoxBoundsExtent = FVector3f(BuildData.Bounds.GetExtent());
		PackedHierarchyNode.Misc1[0].ChildStartReference = 0xFFFFFFFFu;

		const int32 PageIndexStart = 0;
		const int32 PageIndexNum = 0;
		const int32 GroupPartSize = 1;
		PackedHierarchyNode.Misc2[0].ResourcePageIndex_NumPages_GroupPartSize =
			(PageIndexStart << (NANITE_MAX_CLUSTERS_PER_GROUP_BITS + NANITE_MAX_GROUP_PARTS_BITS)) |
			(PageIndexNum << NANITE_MAX_CLUSTERS_PER_GROUP_BITS) |
			GroupPartSize;

		BuildData.Resources.HierarchyNodes.Add(PackedHierarchyNode);
	}

	return true;
}
#endif

TVoxelArray<TUniquePtr<Voxel::Nanite::FCluster>> FVoxelNaniteBuilder::CreateClusters(const FMesh& InMesh) const
{
	VOXEL_FUNCTION_COUNTER();

	using namespace Voxel::Nanite;

	TVoxelArray<TUniquePtr<FCluster>> AllClusters;
	AllClusters.Reserve(100);

	int32 NumRefs = 0;
	struct FVertex
	{
		int32 MeshVertex = 0;
		bool bNew = false;
	};
	const auto RotateTriangle = [&](
		FVertex& MeshVertexA,
		FVertex& MeshVertexB,
		FVertex& MeshVertexC)
	{
		switch (MeshVertexA.bNew + MeshVertexB.bNew + MeshVertexC.bNew)
		{
			// Need RRN
		case 1:
		{
			// N1 R2 R3 -> R2 R3 N1
			if (MeshVertexA.bNew)
			{
				// N1 R2 R3 -> R2 N1 R3
				Swap(MeshVertexA, MeshVertexB);
				// R2 N1 R3 -> R2 R3 N1
				Swap(MeshVertexB, MeshVertexC);
			}
			// R1 N2 R3 -> R3 R1 N2
			else if (MeshVertexB.bNew)
			{
				// R1 N2 R3 -> N2 R1 R3
				Swap(MeshVertexA, MeshVertexB);
				// N2 R1 R3 -> R3 R1 N2
				Swap(MeshVertexA, MeshVertexC);
			}
			break;
		}
			// Need RNN
		case 2:
		{
			// N1 N2 R3 -> R3 N1 N2
			if (MeshVertexA.bNew &&
				MeshVertexB.bNew)
			{
				// N1 N2 R3 -> N2 N1 R3
				Swap(MeshVertexA, MeshVertexB);
				// N2 N1 R3 -> R3 N1 N2
				Swap(MeshVertexA, MeshVertexC);
			}
			// N1 R2 N3 -> R2 N3 N1
			else if (
				MeshVertexA.bNew &&
				MeshVertexC.bNew)
			{
				// N1 R2 N3 -> R2 N1 N3
				Swap(MeshVertexA, MeshVertexB);
				// R2 N1 N3 -> R2 N3 N1
				Swap(MeshVertexB, MeshVertexC);
			}
			break;
		}
		case 0: break;
		case 3: break;
		default: ensure(false); break;
		}
	};

	const uint8 IndexSize = FMath::FloorLog2(InMesh.Positions.Num()) + 1;
	int32 NewTriangleIndex = 0;
	for (int32 TriangleIndex = 0; TriangleIndex < InMesh.Indices.Num() / 3; TriangleIndex++)
	{
		if (AllClusters.Num() == 0 ||
			AllClusters.Last()->NumTriangles() == NANITE_MAX_CLUSTER_TRIANGLES ||
			AllClusters.Last()->Positions.Num() + 3 > NANITE_MAX_CLUSTER_VERTICES)
		{
			VOXEL_SCOPE_COUNTER("Allocate cluster");

			NewTriangleIndex = 0;

			FCluster& Cluster = *AllClusters.Add_GetRef(MakeUnique<FCluster>());
			Cluster.TextureCoordinates.SetNum(InMesh.TextureCoordinates.Num());
			Cluster.MeshIndexToClusterIndex.Reserve(NANITE_MAX_CLUSTER_TRIANGLES * 3);
			Cluster.ExtendedData.Append(UniqueId, 32);
			Cluster.ExtendedData.Append(ChunkIndex, 32);
			Cluster.ExtendedData.Append(IndexSize, 6);
		}

		const uint32 ClusterTriangleIndex = NewTriangleIndex++;

		const uint32 DWordBucket = ClusterTriangleIndex >> 5;
		const uint32 DWordBitInBucket = ClusterTriangleIndex & 31;

		FCluster& Cluster = *AllClusters.Last();

		if (!bCompressVertices)
		{
			const auto AddVertex = [&](int32 MeshVertexIndex)
			{
				const uint8 NewClusterVertex = Cluster.Positions.Add(InMesh.Positions[MeshVertexIndex]);
				Cluster.Indices.Add(NewClusterVertex);
				Cluster.Normals.Add(InMesh.Normals[MeshVertexIndex]);

				if (InMesh.Colors.Num() > 0)
				{
					Cluster.Colors.Add(InMesh.Colors[MeshVertexIndex]);
				}

				for (int32 UVIndex = 0; UVIndex < Cluster.TextureCoordinates.Num(); UVIndex++)
				{
					Cluster.TextureCoordinates[UVIndex].Add(InMesh.TextureCoordinates[UVIndex][MeshVertexIndex]);
				}

				Cluster.MeshIndexToClusterIndex.FindOrAdd(MeshVertexIndex) = NewClusterVertex;
				Cluster.NewInDword[DWordBucket]++;
				Cluster.ExtendedData.Append(MeshVertexIndex, IndexSize);
			};

			AddVertex(InMesh.Indices[3 * TriangleIndex + 0]);
			AddVertex(InMesh.Indices[3 * TriangleIndex + 1]);
			AddVertex(InMesh.Indices[3 * TriangleIndex + 2]);

			Cluster.StripBitmaskDWords[3 * DWordBucket + 0] |= 1 << DWordBitInBucket;
			continue;
		}

		const auto MakeVertex = [&](const int32 MeshVertexIndex) -> FVertex
		{
			if (const uint8* ClusterVertexIndex = Cluster.MeshIndexToClusterIndex.Find(MeshVertexIndex))
			{
				return FVertex(MeshVertexIndex, uint32((Cluster.Positions.Num() - 1) - (*ClusterVertexIndex)) >= 30);
			}
			return FVertex(MeshVertexIndex, true);
		};

		FVertex VertexA = MakeVertex(InMesh.Indices[3 * TriangleIndex + 0]);
		FVertex VertexB = MakeVertex(InMesh.Indices[3 * TriangleIndex + 1]);
		FVertex VertexC = MakeVertex(InMesh.Indices[3 * TriangleIndex + 2]);

		RotateTriangle(VertexA, VertexB, VertexC);

		const auto AddVertex = [&](FVertex& Vertex)
		{
			if (!Vertex.bNew)
			{
				NumRefs++;
				const uint32 Delta = (Cluster.Positions.Num() - 1) - Cluster.MeshIndexToClusterIndex[Vertex.MeshVertex];
				ensure(Delta < 32);

				Cluster.DeltaWriter.Append(Delta, 5);
				Cluster.Indices.Add(Cluster.MeshIndexToClusterIndex[Vertex.MeshVertex]);
				Cluster.RefInDword[DWordBucket]++;
				return;
			}

			const uint8 NewClusterVertex = Cluster.Positions.Add(InMesh.Positions[Vertex.MeshVertex]);
			Cluster.Indices.Add(NewClusterVertex);
			Cluster.Normals.Add(InMesh.Normals[Vertex.MeshVertex]);

			if (InMesh.Colors.Num() > 0)
			{
				Cluster.Colors.Add(InMesh.Colors[Vertex.MeshVertex]);
			}

			for (int32 UVIndex = 0; UVIndex < Cluster.TextureCoordinates.Num(); UVIndex++)
			{
				Cluster.TextureCoordinates[UVIndex].Add(InMesh.TextureCoordinates[UVIndex][Vertex.MeshVertex]);
			}

			Cluster.MeshIndexToClusterIndex.FindOrAdd(Vertex.MeshVertex) = NewClusterVertex;
			Cluster.NewInDword[DWordBucket]++;
			Cluster.ExtendedData.Append(Vertex.MeshVertex, IndexSize);
		};

		AddVertex(VertexA);
		AddVertex(VertexB);
		AddVertex(VertexC);

		const uint32 NumWrittenIndices = 3u - (VertexA.bNew + VertexB.bNew + VertexC.bNew);
		const uint32 LowBit = NumWrittenIndices & 1u;
		const uint32 HighBit = (NumWrittenIndices >> 1) & 1u;

		Cluster.StripBitmaskDWords[3 * DWordBucket + 0] |= 1 << DWordBitInBucket;
		Cluster.StripBitmaskDWords[3 * DWordBucket + 1] |= HighBit << DWordBitInBucket;
		Cluster.StripBitmaskDWords[3 * DWordBucket + 2] |= LowBit << DWordBitInBucket;
	}
	return AllClusters;
}

TVoxelArray<TVoxelArray<TUniquePtr<Voxel::Nanite::FCluster>>> FVoxelNaniteBuilder::CreatePages(
	TVoxelArray<TUniquePtr<FCluster>>& Clusters,
	const Voxel::Nanite::FEncodingSettings& EncodingSettings) const
{
	using namespace Voxel::Nanite;

	TVoxelArray<TVoxelArray<TUniquePtr<FCluster>>> Pages;
	{
		int32 ClusterIndex = 0;
		while (ClusterIndex < Clusters.Num())
		{
			TVoxelArray<TUniquePtr<FCluster>>& PageClusters = Pages.Emplace_GetRef();
			int32 GpuSize = 0;

			while (
				ClusterIndex < Clusters.Num() &&
				PageClusters.Num() < NANITE_ROOT_PAGE_MAX_CLUSTERS)
			{
				TUniquePtr<FCluster>& Cluster = Clusters[ClusterIndex];

				const int32 ClusterGpuSize = Cluster->GetEncodingInfo(EncodingSettings).GpuSizes.GetTotal();
				if (GpuSize + ClusterGpuSize > NANITE_ROOT_PAGE_GPU_SIZE)
				{
					break;
				}

				PageClusters.Add(MoveTemp(Cluster));
				GpuSize += ClusterGpuSize;
				ClusterIndex++;
			}

			ensure(GpuSize <= NANITE_ROOT_PAGE_GPU_SIZE);
		}
		check(ClusterIndex == Clusters.Num());
	}
	return Pages;
}
