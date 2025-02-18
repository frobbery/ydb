#pragma once
#include "defs.h"
#include "flat_table_misc.h"
#include "flat_part_store.h"
#include "flat_store_hotdog.h"
#include "flat_store_solid.h"
#include "flat_sausagecache.h"
#include "tablet_flat_executor.h"
#include "flat_executor_snapshot.h"
#include <ydb/core/util/pb.h>
#include <util/generic/hash_set.h>
#include <ydb/core/tablet_flat/flat_executor.pb.h>

namespace NKikimr {
namespace NTabletFlatExecutor {

    struct TPageCollectionReadEnv : public NTable::IPages {
        TPageCollectionReadEnv(TPrivatePageCache& cache)
            : Cache(cache)
        { }

    protected: /* NTable::IPages, page collection backend implementation */
        TResult Locate(const TMemTable *memTable, ui64 ref, ui32 tag) noexcept override
        {
            return NTable::MemTableRefLookup(memTable, ref, tag);
        }

        TResult Locate(const TPart *part, ui64 ref, ELargeObj lob) noexcept override
        {
            auto *partStore = CheckedCast<const NTable::TPartStore*>(part);

            return { true, Lookup(partStore->Locate(lob, ref), ref) };
        }

        const TSharedData* TryGetPage(const TPart* part, TPageId page, TGroupId groupId) override
        {
            auto *partStore = CheckedCast<const NTable::TPartStore*>(part);

            return Lookup(partStore->PageCollections.at(groupId.Index).Get(), page);
        }

    private:
        const TSharedData* Lookup(TPrivatePageCache::TInfo *info, TPageId id) noexcept
        {
            if (auto *page = Cache.Lookup(id, info)) {
                if (Touches[info].insert(id).second) {
                    ++CacheHits;
                }
                return page;
            } else {
                if (ToLoad[info].insert(id).second) {
                    ++CacheMisses;
                }
                return nullptr;
            }
        }

    public:
        TPrivatePageCache& Cache;

        /*_ Page collection cache pages load trace */

        THashMap<TPrivatePageCache::TInfo*, THashSet<ui32>> Touches;
        THashMap<TPrivatePageCache::TInfo*, THashSet<ui32>> ToLoad;
        size_t CacheHits = 0;
        size_t CacheMisses = 0;
    };

    struct TPageCollectionTxEnv : public TPageCollectionReadEnv, public IExecuting {
        TPageCollectionTxEnv(NTable::TDatabase& db, TPrivatePageCache& cache)
            : TPageCollectionReadEnv(cache)
            , DB(db)
        { }

        using TLogoId = TLogoBlobID;

        struct TBorrowSnap {
            TIntrusivePtr<TTableSnapshotContext> SnapContext;
        };

        struct TBorrowUpdate {
            TDeque<ui64> StoppedLoans;
        };

        struct TLoanConfirmation {
            const TLogoId BorrowId;
        };

        struct TLoanBundle {
            TLoanBundle(ui32 sourceTableId, ui32 localTableId, ui64 lender, NTable::TPartComponents&& pc)
                : SourceTableId(sourceTableId)
                , LocalTableId(localTableId)
                , Lender(lender)
                , PartComponents(std::move(pc))
            {}

            const ui32 SourceTableId;
            const ui32 LocalTableId;
            const ui64 Lender;

            NTable::TPartComponents PartComponents;
        };

        struct TLoanTxStatus {
            TLoanTxStatus(ui32 sourceTableId, ui32 localTableId, ui64 lender,
                          const NPageCollection::TLargeGlobId& dataId, NTable::TEpoch epoch,
                          const TString& data)
                : SourceTableId(sourceTableId)
                , LocalTableId(localTableId)
                , Lender(lender)
                , DataId(dataId)
                , Epoch(epoch)
                , Data(data)
            {}

            const ui32 SourceTableId;
            const ui32 LocalTableId;
            const ui64 Lender;
            const NPageCollection::TLargeGlobId DataId;
            const NTable::TEpoch Epoch;
            const TString Data;
        };

        struct TSnapshot {
            TVector<TIntrusivePtr<TTableSnapshotContext>> Context;
            std::optional<NTable::TEpoch> Epoch;
        };

        using TPageCollectionReadEnv::TPageCollectionReadEnv;

        bool HasChanges() const noexcept
        {
            return
                DropSnap
                || MakeSnap
                || LoanBundle
                || LoanTxStatus
                || BorrowUpdates
                || LoanConfirmation;
        }

    protected:
        void OnRollbackChanges() noexcept override {
            MakeSnap.clear();
            DropSnap.Reset();
            BorrowUpdates.clear();
            LoanBundle.clear();
            LoanTxStatus.clear();
            LoanConfirmation.clear();
        }

    protected: /* IExecuting, tx stage func implementation */
        void MakeSnapshot(TIntrusivePtr<TTableSnapshotContext> snap) override;

        void DropSnapshot(TIntrusivePtr<TTableSnapshotContext> snap) override
        {
            Y_VERIFY(!DropSnap, "only one snapshot per transaction");

            DropSnap.Reset(new TBorrowSnap{ snap });
        }

        void MoveSnapshot(const TTableSnapshotContext &snap, ui32 src, ui32 dst) override
        {
            snap.Impl->Moved(src, dst);
        }

        void ClearSnapshot(const TTableSnapshotContext &snap) override
        {
            snap.Impl->Clear();
        }

        // NOTE: It's allowed to add parts in the same Tx where the table gets created (and is not visible yet)
        void LoanTable(ui32 tableId, const TString &raw) override
        {
            TProtoBox<NKikimrExecutorFlat::TDatabaseBorrowPart> proto(raw);

            const ui64 lender = proto.GetLenderTablet();
            const ui32 source = proto.GetSourceTable();

            for (auto &part : proto.GetParts()) {
                Y_VERIFY(part.HasBundle(), "Cannot find attached hotdogs in borrow");

                LoanBundle.emplace_back(new TLoanBundle(source, tableId, lender,
                        TPageCollectionProtoHelper::MakePageCollectionComponents(part.GetBundle(), /* unsplit */ true)));
            }

            for (auto &part : proto.GetTxStatusParts()) {
                LoanTxStatus.emplace_back(new TLoanTxStatus(
                    source, tableId, lender,
                    TLargeGlobIdProto::Get(part.GetDataId()),
                    NTable::TEpoch(part.GetEpoch()),
                    part.GetData()));
            }
        }

        void CleanupLoan(const TLogoId &bundle, ui64 from) override
        {
            Y_VERIFY(!DropSnap, "must not drop snapshot and update loan in same transaction");
            BorrowUpdates[bundle].StoppedLoans.push_back(from);
        }

        void ConfirmLoan(const TLogoId &bundle, const TLogoId &borrow) override
        {
            LoanConfirmation.insert(std::make_pair(bundle, TLoanConfirmation{borrow}));
        }

    protected:
        NTable::TDatabase& DB;

    public:
        /*_ Pending database shanshots      */

        TMap<ui32, TSnapshot> MakeSnap;

        /*_ In tx tables borrow proto API   */

        THolder<TBorrowSnap> DropSnap;
        THashMap<TLogoId, TBorrowUpdate> BorrowUpdates;
        TVector<THolder<TLoanBundle>> LoanBundle;
        TVector<THolder<TLoanTxStatus>> LoanTxStatus;
        THashMap<TLogoId, TLoanConfirmation> LoanConfirmation;
    };

}
}
