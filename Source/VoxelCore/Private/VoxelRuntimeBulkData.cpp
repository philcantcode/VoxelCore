// Distributed under the repository's MIT license.
#include "VoxelRuntimeBulkData.h"
#if VOXEL_ENGINE_VERSION == 508
#include "IO/IoDispatcherBackend.h"
#include "Serialization/BulkData.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeRWLock.h"

// UE 5.8 exposes range reads and custom I/O backends, but no public constructor
// for a runtime bulk chunk identity. Reuse the fork's checked member-access
// helper; no byte offsets, engine binary patches or editor-only path are used.
DEFINE_PRIVATE_ACCESS(FBulkData, BulkChunkId)
DEFINE_PRIVATE_ACCESS(FBulkData, BulkMeta)

class FVoxelRuntimeBulkDataBackend final : public IIoDispatcherBackend
{
public:
    FIoChunkId Add(TSharedRef<const TArray<uint8>> Bytes)
    {
        FWriteScopeLock Lock(PayloadLock);
        FIoChunkId Id;
        do
        {
            const FGuid Guid = FGuid::NewGuid();
            uint8 Raw[12];
            FMemory::Memcpy(Raw, &Guid, 11);
            Raw[11] = uint8(EIoChunkType::BulkData);
            Id.Set(Raw, sizeof(Raw));
        } while (Payloads.Contains(Id));
        Payloads.Add(Id, Bytes);
        return Id;
    }
    void Remove(const FIoChunkId& Id)
    {
        FWriteScopeLock Lock(PayloadLock);
        Payloads.Remove(Id);
    }
    virtual void Initialize(TSharedRef<const FIoDispatcherBackendContext> InContext) override
    {
        Context = InContext;
    }
    virtual void ResolveIoRequests(FIoRequestList Requests, FIoRequestList& OutUnresolved) override
    {
        while (FIoRequestImpl* Request = Requests.PopHead())
        {
            TSharedPtr<const TArray<uint8>> Bytes;
            {
                FReadScopeLock Lock(PayloadLock);
                if (const auto* Found = Payloads.Find(Request->ChunkId))
                    Bytes = *Found;
            }
            if (!Bytes)
            {
                OutUnresolved.AddTail(Request);
                continue;
            }
            if (!Request->IsCancelled())
            {
                const uint64 Offset = Request->Options.GetOffset();
                const uint64 Size = Request->Options.GetSize();
                const uint64 Total = uint64(Bytes->Num());
                if (Offset > Total || (Size != MAX_uint64 && Size > Total - Offset))
                    Request->SetLastBackendError(EIoErrorCode::ReadError);
                else
                {
                    const uint64 Count = Size == MAX_uint64 ? Total - Offset : Size;
                    Request->CreateBuffer(Count); // Also honors the caller's target buffer.
                    if (Count)
                        FMemory::Memcpy(Request->GetBuffer().GetData(), Bytes->GetData() + Offset, Count);
                }
            }
            {
                FScopeLock Lock(&CompletedLock);
                if (CompletedTail)
                    CompletedTail->NextRequest = Request;
                else
                    CompletedHead = Request;
                CompletedTail = Request;
            }
        }
        Context->WakeUpDispatcherThreadDelegate.ExecuteIfBound();
    }
    virtual FIoRequestImpl* GetCompletedIoRequests() override
    {
        FScopeLock Lock(&CompletedLock);
        FIoRequestImpl* Result = CompletedHead;
        CompletedHead = CompletedTail = nullptr;
        return Result;
    }
    virtual void CancelIoRequest(FIoRequestImpl* Request) override
    {
        Request->SetLastBackendError(EIoErrorCode::Cancelled);
    }
    virtual void UpdatePriorityForIoRequest(FIoRequestImpl*) override {}
    virtual bool DoesChunkExist(const FIoChunkId& Id) const override
    {
        FReadScopeLock Lock(PayloadLock);
        return Payloads.Contains(Id);
    }
    virtual TIoStatusOr<uint64> GetSizeForChunk(const FIoChunkId& Id) const override
    {
        FReadScopeLock Lock(PayloadLock);
        if (const auto* Bytes = Payloads.Find(Id))
            return uint64((*Bytes)->Num());
        return FIoStatus(EIoErrorCode::NotFound);
    }
    virtual TIoStatusOr<FIoMappedRegion> OpenMapped(const FIoChunkId&, const FIoReadOptions&) override
    {
        return FIoStatus(EIoErrorCode::ReadError);
    }
    virtual const TCHAR* GetName() const override { return TEXT("VoxelRuntimeBulkData"); }
private:
    mutable FRWLock PayloadLock;
    TMap<FIoChunkId, TSharedRef<const TArray<uint8>>> Payloads;
    FCriticalSection CompletedLock;
    FIoRequestImpl* CompletedHead = nullptr;
    FIoRequestImpl* CompletedTail = nullptr;
    TSharedPtr<const FIoDispatcherBackendContext> Context;
};

namespace
{
FCriticalSection RuntimeBackendLock;
TSharedPtr<FVoxelRuntimeBulkDataBackend> RuntimeBackend;
}
void FVoxelRuntimeBulkData::Initialize()
{
    check(IsInGameThread());
    check(FIoDispatcher::IsInitialized());
    FScopeLock Lock(&RuntimeBackendLock);
    if (!RuntimeBackend)
    {
        RuntimeBackend = MakeShared<FVoxelRuntimeBulkDataBackend>();
        FIoDispatcher::Get().Mount(RuntimeBackend.ToSharedRef());
    }
}
void FVoxelRuntimeBulkData::Shutdown()
{
    // The dispatcher retains its mounted backend until its own shutdown.
    // The module disallows dynamic unloading while that vtable is registered.
    FScopeLock Lock(&RuntimeBackendLock);
    RuntimeBackend.Reset();
}
TSharedRef<FVoxelRuntimeBulkData> FVoxelRuntimeBulkData::Bind(FBulkData& BulkData, TArray<uint8> Bytes)
{
    const auto Provider = [] {
        FScopeLock Lock(&RuntimeBackendLock);
        check(RuntimeBackend);
        return RuntimeBackend.ToSharedRef();
    }();
    check(BulkData.GetBulkDataSize() == 0 && !BulkData.CreateChunkId().IsValid());
    const auto Payload = MakeShared<const TArray<uint8>>(MoveTemp(Bytes));
    const FIoChunkId Id = Provider->Add(Payload);
    auto& Meta = PrivateAccess::BulkMeta(BulkData);
    Meta.SetSize(Payload->Num());
    Meta.SetOffset(0);
    Meta.SetSizeOnDisk(Payload->Num());
    Meta.SetFlags(EBulkDataFlags(BULKDATA_UsesIoDispatcher | BULKDATA_PayloadAtEndOfFile | BULKDATA_PayloadInSeparateFile));
    PrivateAccess::BulkChunkId(BulkData) = Id;
    return MakeShareable(new FVoxelRuntimeBulkData(Provider, Id, Payload->GetAllocatedSize()));
}
FVoxelRuntimeBulkData::FVoxelRuntimeBulkData(TSharedRef<FVoxelRuntimeBulkDataBackend> InBackend, FIoChunkId InChunkId, uint64 InAllocatedSize)
    : Backend(InBackend), ChunkId(InChunkId), AllocatedSize(InAllocatedSize) {}
FVoxelRuntimeBulkData::~FVoxelRuntimeBulkData()
{
    Backend->Remove(ChunkId);
}
#endif
