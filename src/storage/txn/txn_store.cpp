// Copyright(C) 2023 InfiniFlow, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

module;

#include <vector>

module txn_store;

import stl;
import third_party;

import status;
import infinity_exception;
import data_block;
import logger;
import data_access_state;
import txn;
import default_values;
import table_entry;
import catalog;
import catalog_delta_entry;
import internal_types;
import data_type;
import background_process;
import bg_task;
import compact_segments_task;
import build_fast_rough_filter_task;

namespace infinity {

TxnSegmentStore TxnSegmentStore::AddSegmentStore(SegmentEntry *segment_entry) {
    TxnSegmentStore txn_segment_store(segment_entry);
    for (auto &block_entry : segment_entry->block_entries()) {
        txn_segment_store.block_entries_.emplace(block_entry->block_id(), block_entry.get());
    }
    return txn_segment_store;
}

TxnSegmentStore::TxnSegmentStore(SegmentEntry *segment_entry) : segment_entry_(segment_entry) {}

void TxnSegmentStore::AddDeltaOp(CatalogDeltaEntry *local_delta_ops, AppendState *append_state, Txn *txn, bool set_sealed) const {
    TxnTimeStamp commit_ts = txn->CommitTS();

    if (set_sealed) {
        // build minmax filter
        BuildFastRoughFilterTask::ExecuteOnNewSealedSegment(segment_entry_, txn->buffer_mgr(), commit_ts);
        // now have minmax filter and optional bloom filter
        // serialize filter
        local_delta_ops->AddOperation(
            MakeUnique<AddSegmentEntryOp>(segment_entry_, commit_ts, segment_entry_->GetFastRoughFilter()->SerializeToString()));
    } else {
        local_delta_ops->AddOperation(MakeUnique<AddSegmentEntryOp>(segment_entry_, commit_ts));
    }
    for (auto [block_id, block_entry] : block_entries_) {
        if (set_sealed) {
            local_delta_ops->AddOperation(
                MakeUnique<AddBlockEntryOp>(block_entry, commit_ts, block_entry->GetFastRoughFilter()->SerializeToString()));
        } else {
            local_delta_ops->AddOperation(MakeUnique<AddBlockEntryOp>(block_entry, commit_ts));
        }
        for (const auto &column_entry : block_entry->columns()) {
            local_delta_ops->AddOperation(MakeUnique<AddColumnEntryOp>(column_entry.get(), commit_ts));
        }
    }
}

///-----------------------------------------------------------------------------

TxnIndexStore::TxnIndexStore(TableIndexEntry *table_index_entry) : table_index_entry_(table_index_entry) {}

void TxnIndexStore::AddDeltaOp(CatalogDeltaEntry *local_delta_ops, TxnTimeStamp commit_ts) const {
    for (auto [segment_id, segment_index_entry] : index_entry_map_) {
        local_delta_ops->AddOperation(MakeUnique<AddSegmentIndexEntryOp>(segment_index_entry, commit_ts));
    }
    for (auto *chunk_index_entry : chunk_index_entries_) {
        local_delta_ops->AddOperation(MakeUnique<AddChunkIndexEntryOp>(chunk_index_entry, commit_ts));
    }
}

void TxnIndexStore::PrepareCommit(TransactionID txn_id, TxnTimeStamp commit_ts, BufferManager *buffer_mgr) {
    for (auto [segment_index_entry, new_chunk, old_chunks] : optimize_data_) {
        segment_index_entry->CommitOptimize(new_chunk, old_chunks, commit_ts);
    }
}

void TxnIndexStore::Commit(TransactionID txn_id, TxnTimeStamp commit_ts) const {
    for (const auto &[segment_id, segment_index_entry] : index_entry_map_) {
        segment_index_entry->CommitSegmentIndex(txn_id, commit_ts);
    }
    for (auto *chunk_index_entry : chunk_index_entries_) {
        // uncommitted: insert or import, create index
        // committed: create index, insert or import
        if (!chunk_index_entry->Committed()) {
            chunk_index_entry->Commit(commit_ts);
        }
    }
}

///-----------------------------------------------------------------------------

TxnCompactStore::TxnCompactStore() : task_type_(CompactSegmentsTaskType::kInvalid) {}

TxnCompactStore::TxnCompactStore(CompactSegmentsTaskType type) : task_type_(type) {}

///-----------------------------------------------------------------------------

Tuple<UniquePtr<String>, Status> TxnTableStore::Import(SharedPtr<SegmentEntry> segment_entry, Txn *txn) {
    this->AddSegmentStore(segment_entry.get());
    this->AddSealedSegment(segment_entry.get());
    this->flushed_segments_.emplace_back(segment_entry.get());
    table_entry_->Import(std::move(segment_entry), txn);
    return {nullptr, Status::OK()};
}

Tuple<UniquePtr<String>, Status> TxnTableStore::Append(const SharedPtr<DataBlock> &input_block) {
    SizeT column_count = table_entry_->ColumnCount();
    if (input_block->column_count() != column_count) {
        String err_msg = fmt::format("Attempt to insert different column count data block into transaction table store");
        LOG_ERROR(err_msg);
        return {MakeUnique<String>(err_msg), Status::ColumnCountMismatch(err_msg)};
    }

    Vector<SharedPtr<DataType>> column_types;
    for (SizeT col_id = 0; col_id < column_count; ++col_id) {
        column_types.emplace_back(table_entry_->GetColumnDefByID(col_id)->type());
        if (*column_types.back() != *input_block->column_vectors[col_id]->data_type()) {
            String err_msg = fmt::format("Attempt to insert different type data into transaction table store");
            LOG_ERROR(err_msg);
            return {MakeUnique<String>(err_msg),
                    Status::DataTypeMismatch(column_types.back()->ToString(), input_block->column_vectors[col_id]->data_type()->ToString())};
        }
    }

    DataBlock *current_block{nullptr};
    if (blocks_.empty()) {
        blocks_.emplace_back(DataBlock::Make());
        current_block_id_ = 0;
        blocks_.back()->Init(column_types);
    }
    current_block = blocks_[current_block_id_].get();

    if (current_block->row_count() + input_block->row_count() > current_block->capacity()) {
        SizeT to_append = current_block->capacity() - current_block->row_count();
        current_block->AppendWith(input_block.get(), 0, to_append);
        current_block->Finalize();

        blocks_.emplace_back(DataBlock::Make());
        blocks_.back()->Init(column_types);
        ++current_block_id_;
        current_block = blocks_[current_block_id_].get();
        current_block->AppendWith(input_block.get(), to_append, input_block->row_count() - to_append);
    } else {
        SizeT to_append = input_block->row_count();
        current_block->AppendWith(input_block.get(), 0, to_append);
    }
    current_block->Finalize();

    return {nullptr, Status::OK()};
}

void TxnTableStore::AddIndexStore(TableIndexEntry *table_index_entry) { txn_indexes_.emplace(table_index_entry, ptr_seq_n_++); }

void TxnTableStore::AddSegmentIndexesStore(TableIndexEntry *table_index_entry, const Vector<SegmentIndexEntry *> &segment_index_entries) {
    auto *txn_index_store = this->GetIndexStore(table_index_entry);
    for (auto *segment_index_entry : segment_index_entries) {
        auto [iter, insert_ok] = txn_index_store->index_entry_map_.emplace(segment_index_entry->segment_id(), segment_index_entry);
        if (!insert_ok) {
            UnrecoverableError(fmt::format("Attempt to add segment index of segment {} store twice", segment_index_entry->segment_id()));
        }
    }
}

void TxnTableStore::AddChunkIndexStore(TableIndexEntry *table_index_entry, ChunkIndexEntry *chunk_index_entry) {
    auto *txn_index_store = this->GetIndexStore(table_index_entry);
    txn_index_store->chunk_index_entries_.push_back(chunk_index_entry);
}

void TxnTableStore::DropIndexStore(TableIndexEntry *table_index_entry) {
    if (txn_indexes_.contains(table_index_entry)) {
        table_index_entry->Cleanup();
        txn_indexes_.erase(table_index_entry);
        txn_indexes_store_.erase(*table_index_entry->GetIndexName());
    } else {
        txn_indexes_.emplace(table_index_entry, ptr_seq_n_++);
    }
}

TxnIndexStore *TxnTableStore::GetIndexStore(TableIndexEntry *table_index_entry) {
    const String &index_name = *table_index_entry->GetIndexName();
    if (auto iter = txn_indexes_store_.find(index_name); iter != txn_indexes_store_.end()) {
        return iter->second.get();
    }
    auto txn_index_store = MakeUnique<TxnIndexStore>(table_index_entry);
    auto *ptr = txn_index_store.get();
    txn_indexes_store_.emplace(index_name, std::move(txn_index_store));
    return ptr;
}

Tuple<UniquePtr<String>, Status> TxnTableStore::Delete(const Vector<RowID> &row_ids) {
    HashMap<SegmentID, HashMap<BlockID, Vector<BlockOffset>>> &row_hash_table = delete_state_.rows_;
    for (auto row_id : row_ids) {
        BlockID block_id = row_id.segment_offset_ / DEFAULT_BLOCK_CAPACITY;
        BlockOffset block_offset = row_id.segment_offset_ % DEFAULT_BLOCK_CAPACITY;
        auto &seg_map = row_hash_table[row_id.segment_id_];
        auto &block_vec = seg_map[block_id];
        block_vec.emplace_back(block_offset);
        if (block_vec.size() > DEFAULT_BLOCK_CAPACITY) {
            UnrecoverableError("Delete row exceed block capacity");
        }
    }

    return {nullptr, Status::OK()};
}

Tuple<UniquePtr<String>, Status> TxnTableStore::Compact(Vector<Pair<SharedPtr<SegmentEntry>, Vector<SegmentEntry *>>> &&segment_data,
                                                        CompactSegmentsTaskType type) {
    if (compact_state_.task_type_ != CompactSegmentsTaskType::kInvalid) {
        UnrecoverableError("Attempt to compact table store twice");
    }
    compact_state_ = TxnCompactStore(type);
    for (auto &[new_segment, old_segments] : segment_data) {
        auto txn_segment_store = TxnSegmentStore::AddSegmentStore(new_segment.get());
        compact_state_.compact_data_.emplace_back(std::move(txn_segment_store), std::move(old_segments));

        this->AddSegmentStore(new_segment.get());
        this->AddSealedSegment(new_segment.get());
        this->flushed_segments_.emplace_back(new_segment.get());
        table_entry_->AddCompactNew(std::move(new_segment));
    }
    return {nullptr, Status::OK()};
}

void TxnTableStore::Rollback(TransactionID txn_id, TxnTimeStamp abort_ts) {
    if (append_state_.get() != nullptr) {
        // Rollback the data already been appended.
        Catalog::RollbackAppend(table_entry_, txn_id, abort_ts, this);
        LOG_TRACE(fmt::format("Rollback prepare appended data in table: {}", *table_entry_->GetTableName()));
    }
    // for (auto &[index_name, txn_index_store] : txn_indexes_store_) {
    //     Catalog::RollbackPopulateIndex(txn_index_store.get(), txn_);
    // }
    Catalog::RollbackCompact(table_entry_, txn_id, abort_ts, compact_state_);
    blocks_.clear();

    for (auto &[table_index_entry, ptr_seq_n] : txn_indexes_) {
        table_index_entry->Cleanup();
        Catalog::RemoveIndexEntry(table_index_entry, txn_id); // fix me
    }
}

bool TxnTableStore::CheckConflict(Catalog *catalog) const {
    TransactionID txn_id = txn_->TxnID();
    TableEntry *latest_table_entry;
    const auto &db_name = *table_entry_->GetDBName();
    const auto &table_name = *table_entry_->GetTableName();
    TxnTimeStamp begin_ts = txn_->BeginTS();
    bool conflict = catalog->CheckTableConflict(db_name, table_name, txn_id, begin_ts, latest_table_entry);
    if (conflict) {
        return true;
    }
    if (latest_table_entry != table_entry_) {
        UnrecoverableError(fmt::format("Table entry should conflict, table name: {}", table_name));
    }
    return false;
}

void TxnTableStore::PrepareCommit1() {
    TxnTimeStamp commit_ts = txn_->CommitTS();
    for (auto *segment_entry : flushed_segments_) {
        segment_entry->CommitFlushed(commit_ts);
    }
}

// TODO: remove commit_ts
void TxnTableStore::PrepareCommit(TransactionID txn_id, TxnTimeStamp commit_ts, BufferManager *buffer_mgr) {
    // Init append state
    append_state_ = MakeUnique<AppendState>(this->blocks_);

    // Start to append
    LOG_TRACE(fmt::format("Transaction local storage table: {}, Start to prepare commit", *table_entry_->GetTableName()));
    Catalog::Append(table_entry_, txn_id, this, commit_ts, buffer_mgr);

    // Attention: "compact" needs to be ahead of "delete"
    if (compact_state_.task_type_ != CompactSegmentsTaskType::kInvalid) {
        LOG_TRACE(fmt::format("Commit compact, table dir: {}, commit ts: {}", *table_entry_->TableEntryDir(), commit_ts));
        Catalog::CommitCompact(table_entry_, txn_id, commit_ts, compact_state_);
    }

    Catalog::Delete(table_entry_, txn_id, this, commit_ts, delete_state_);

    for (const auto &[index_name, txn_index_store] : txn_indexes_store_) {
        txn_index_store->PrepareCommit(txn_id, commit_ts, buffer_mgr);
    }

    for (auto *sealed_segment : set_sealed_segments_) {
        if (!sealed_segment->SetSealed()) {
            UnrecoverableError(fmt::format("Set sealed segment failed, segment id: {}", sealed_segment->segment_id()));
        }
    }

    LOG_TRACE(fmt::format("Transaction local storage table: {}, Complete commit preparing", *table_entry_->GetTableName()));
}

/**
 * @brief Call for really commit the data to disk.
 */
void TxnTableStore::Commit(TransactionID txn_id, TxnTimeStamp commit_ts) const {
    Catalog::CommitWrite(table_entry_, txn_id, commit_ts, txn_segments_store_);
    for (const auto &[index_name, txn_index_store] : txn_indexes_store_) {
        Catalog::CommitCreateIndex(txn_index_store.get(), commit_ts);
        txn_index_store->Commit(txn_id, commit_ts);
    }
    for (auto [table_index_entry, ptr_seq_n] : txn_indexes_) {
        table_index_entry->Commit(commit_ts);
    }
}

void TxnTableStore::MaintainCompactionAlg() const {
    for (auto *sealed_segment : set_sealed_segments_) {
        table_entry_->AddSegmentToCompactionAlg(sealed_segment);
    }
    for (const auto &[segment_id, delete_map] : delete_state_.rows_) {
        table_entry_->AddDeleteToCompactionAlg(segment_id);
    }
}

void TxnTableStore::AddSegmentStore(SegmentEntry *segment_entry) {
    auto [iter, insert_ok] = txn_segments_store_.emplace(segment_entry->segment_id(), TxnSegmentStore::AddSegmentStore(segment_entry));
    if (!insert_ok) {
        UnrecoverableError(fmt::format("Attempt to add segment store twice"));
    }
}

void TxnTableStore::AddBlockStore(SegmentEntry *segment_entry, BlockEntry *block_entry) {
    auto iter = txn_segments_store_.find(segment_entry->segment_id());
    if (iter == txn_segments_store_.end()) {
        iter = txn_segments_store_.emplace(segment_entry->segment_id(), TxnSegmentStore(segment_entry)).first;
    }
    iter->second.block_entries_.emplace(block_entry->block_id(), block_entry);
}

void TxnTableStore::AddSealedSegment(SegmentEntry *segment_entry) { set_sealed_segments_.emplace(segment_entry); }

void TxnTableStore::AddDeltaOp(CatalogDeltaEntry *local_delta_ops, TxnManager *txn_mgr, TxnTimeStamp commit_ts) const {
    local_delta_ops->AddOperation(MakeUnique<AddTableEntryOp>(table_entry_, commit_ts));

    Vector<Pair<TableIndexEntry *, int>> txn_indexes_vec(txn_indexes_.begin(), txn_indexes_.end());
    std::sort(txn_indexes_vec.begin(), txn_indexes_vec.end(), [](const auto &lhs, const auto &rhs) { return lhs.second < rhs.second; });
    for (auto [table_index_entry, ptr_seq_n] : txn_indexes_vec) {
        local_delta_ops->AddOperation(MakeUnique<AddTableIndexEntryOp>(table_index_entry, commit_ts));
    }
    for (const auto &[index_name, index_store] : txn_indexes_store_) {
        index_store->AddDeltaOp(local_delta_ops, commit_ts);
    }
    for (const auto &[segment_id, segment_store] : txn_segments_store_) {
        bool set_sealed = set_sealed_segments_.contains(segment_store.segment_entry_);
        segment_store.AddDeltaOp(local_delta_ops, append_state_.get(), txn_, set_sealed);
    }
}

TxnStore::TxnStore(Txn *txn, Catalog *catalog) : txn_(txn), catalog_(catalog) {}

void TxnStore::AddDBStore(DBEntry *db_entry) { txn_dbs_.emplace(db_entry, ptr_seq_n_++); }

void TxnStore::DropDBStore(DBEntry *dropped_db_entry) {
    if (txn_dbs_.contains(dropped_db_entry)) {
        dropped_db_entry->Cleanup();
        txn_dbs_.erase(dropped_db_entry);
    } else {
        txn_dbs_.emplace(dropped_db_entry, ptr_seq_n_++);
    }
}

void TxnStore::AddTableStore(TableEntry *table_entry) { txn_tables_.emplace(table_entry, ptr_seq_n_++); }

void TxnStore::DropTableStore(TableEntry *dropped_table_entry) {
    if (txn_tables_.contains(dropped_table_entry)) {
        txn_tables_.erase(dropped_table_entry);
        dropped_table_entry->Cleanup();
        txn_tables_store_.erase(*dropped_table_entry->GetTableName());
    } else {
        txn_tables_.emplace(dropped_table_entry, ptr_seq_n_++);
    }
}

TxnTableStore *TxnStore::GetTxnTableStore(const String &table_name) {
    auto iter = txn_tables_store_.find(table_name);
    return iter == txn_tables_store_.end() ? nullptr : iter->second.get();
}

TxnTableStore *TxnStore::GetTxnTableStore(TableEntry *table_entry) {
    const String &table_name = *table_entry->GetTableName();
    if (auto iter = txn_tables_store_.find(table_name); iter != txn_tables_store_.end()) {
        return iter->second.get();
    }
    auto [iter, insert_ok] = txn_tables_store_.emplace(table_name, MakeUnique<TxnTableStore>(txn_, table_entry));
    return iter->second.get();
}

void TxnStore::AddDeltaOp(CatalogDeltaEntry *local_delta_ops, TxnManager *txn_mgr) const {
    TxnTimeStamp commit_ts = txn_->CommitTS();
    Vector<Pair<DBEntry *, int>> txn_dbs_vec(txn_dbs_.begin(), txn_dbs_.end());
    std::sort(txn_dbs_vec.begin(), txn_dbs_vec.end(), [](const auto &lhs, const auto &rhs) { return lhs.second < rhs.second; });
    for (auto [db_entry, ptr_seq_n] : txn_dbs_vec) {
        local_delta_ops->AddOperation(MakeUnique<AddDBEntryOp>(db_entry, commit_ts));
    }

    Vector<Pair<TableEntry *, int>> txn_tables_vec(txn_tables_.begin(), txn_tables_.end());
    std::sort(txn_tables_vec.begin(), txn_tables_vec.end(), [](const auto &lhs, const auto &rhs) { return lhs.second < rhs.second; });
    for (auto [table_entry, ptr_seq_n] : txn_tables_vec) {
        local_delta_ops->AddOperation(MakeUnique<AddTableEntryOp>(table_entry, commit_ts));
    }
    for (const auto &[table_name, table_store] : txn_tables_store_) {
        table_store->AddDeltaOp(local_delta_ops, txn_mgr, commit_ts);
    }
}

void TxnStore::MaintainCompactionAlg() const {
    for (const auto &[table_name, table_store] : txn_tables_store_) {
        table_store->MaintainCompactionAlg();
    }
}

bool TxnStore::CheckConflict() const {
    for (const auto &[table_name, table_store] : txn_tables_store_) {
        if (table_store->CheckConflict(catalog_)) {
            return true;
        }
    }
    return false;
}

void TxnStore::PrepareCommit1() {
    for (const auto &[table_name, table_store] : txn_tables_store_) {
        table_store->PrepareCommit1();
    }
}

void TxnStore::PrepareCommit(TransactionID txn_id, TxnTimeStamp commit_ts, BufferManager *buffer_mgr) {
    for (const auto &[table_name, table_store] : txn_tables_store_) {
        table_store->PrepareCommit(txn_id, commit_ts, buffer_mgr);
    }
}

void TxnStore::CommitBottom(TransactionID txn_id, TxnTimeStamp commit_ts) {
    // Commit the prepared data
    for (const auto &[table_name, table_store] : txn_tables_store_) {
        table_store->Commit(txn_id, commit_ts);
    }

    // Commit databases to memory catalog
    for (auto [db_entry, ptr_seq_n] : txn_dbs_) {
        db_entry->Commit(commit_ts);
    }

    // Commit tables to memory catalog
    for (auto [table_entry, ptr_seq_n] : txn_tables_) {
        table_entry->Commit(commit_ts);
    }
}

void TxnStore::Rollback(TransactionID txn_id, TxnTimeStamp abort_ts) {
    // Rollback the prepared data
    for (const auto &name_table_pair : txn_tables_store_) {
        TxnTableStore *table_local_store = name_table_pair.second.get();
        table_local_store->Rollback(txn_id, abort_ts);
    }

    for (auto [table_entry, ptr_seq_n] : txn_tables_) {
        table_entry->Cleanup();
        Catalog::RemoveTableEntry(table_entry, txn_id);
    }

    for (auto [db_entry, ptr_seq_n] : txn_dbs_) {
        db_entry->Cleanup();
        catalog_->RemoveDBEntry(db_entry, txn_id);
    }
}

bool TxnStore::Empty() const { return txn_dbs_.empty() && txn_tables_.empty() && txn_tables_store_.empty(); }

} // namespace infinity
