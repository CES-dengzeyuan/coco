//
// Created by Yi Lu on 9/19/18.
//

#pragma once

#include <algorithm>
#include <atomic>
#include <thread>

#include "core/Partitioner.h"
#include "core/Table.h"
#include "protocol/Scar/ScarHelper.h"
#include "protocol/Scar/ScarMessage.h"
#include "protocol/Scar/ScarTransaction.h"
#include <glog/logging.h>

namespace scar {

template <class Database> class Scar {
public:
  using DatabaseType = Database;
  using MetaDataType = std::atomic<uint64_t>;
  using ContextType = typename DatabaseType::ContextType;
  using TableType = ITable<MetaDataType>;
  using MessageType = ScarMessage;
  using TransactionType = ScarTransaction;

  using MessageFactoryType = ScarMessageFactory;
  using MessageHandlerType = ScarMessageHandler;

  static_assert(
      std::is_same<typename DatabaseType::TableType, TableType>::value,
      "The database table type is different from the one in protocol.");

  Scar(DatabaseType &db, const ContextType &context, Partitioner &partitioner)
      : db(db), context(context), partitioner(partitioner) {}

  uint64_t search(std::size_t table_id, std::size_t partition_id,
                  const void *key, void *value) const {

    TableType *table = db.find_table(table_id, partition_id);
    auto value_bytes = table->value_size();
    auto row = table->search(key);
    return ScarHelper::read(row, value, value_bytes);
  }

  void abort(TransactionType &txn,
             std::vector<std::unique_ptr<Message>> &messages) {

    auto &writeSet = txn.writeSet;

    // unlock locked records

    for (auto i = 0u; i < writeSet.size(); i++) {
      auto &writeKey = writeSet[i];
      // only unlock locked records
      if (!writeKey.get_write_lock_bit())
        continue;
      auto tableId = writeKey.get_table_id();
      auto partitionId = writeKey.get_partition_id();
      auto table = db.find_table(tableId, partitionId);
      if (partitioner.has_master_partition(partitionId)) {
        auto key = writeKey.get_key();
        std::atomic<uint64_t> &tid = table->search_metadata(key);
        ScarHelper::unlock(tid);
      } else {
        auto coordinatorID = partitioner.master_coordinator(partitionId);
        txn.network_size += MessageFactoryType::new_abort_message(
            *messages[coordinatorID], *table, writeKey.get_key());
      }
    }

    sync_messages(txn, false);
  }

  bool commit(TransactionType &txn,
              std::vector<std::unique_ptr<Message>> &messages) {

    // lock write set
    if (lock_write_set(txn, messages)) {
      abort(txn, messages);
      return false;
    }

    compute_commit_ts(txn);

    // commit phase 2, read validation
    if (!validate_read_set(txn, messages)) {
      abort(txn, messages);
      return false;
    }

    // write and replicate
    write_and_replicate(txn, messages);

    // release locks
    release_lock(txn, messages);

    return true;
  }

private:
  bool lock_write_set(TransactionType &txn,
                      std::vector<std::unique_ptr<Message>> &messages) {

    auto &readSet = txn.readSet;
    auto &writeSet = txn.writeSet;

    // lock records in any order. there might be dead lock.

    for (auto i = 0u; i < writeSet.size(); i++) {

      auto &writeKey = writeSet[i];
      auto tableId = writeKey.get_table_id();
      auto partitionId = writeKey.get_partition_id();
      auto table = db.find_table(tableId, partitionId);

      // lock local records
      if (partitioner.has_master_partition(partitionId)) {

        auto key = writeKey.get_key();
        std::atomic<uint64_t> &tid = table->search_metadata(key);
        bool success;
        uint64_t latestTid = ScarHelper::lock(tid, success);

        if (!success) {
          txn.abort_lock = true;
          break;
        }

        writeKey.set_write_lock_bit();
        writeKey.set_tid(latestTid);

        auto readKeyPtr = txn.get_read_key(key);
        // assume no blind write
        DCHECK(readKeyPtr != nullptr);
        uint64_t tidOnRead = readKeyPtr->get_tid();
        if (ScarHelper::get_wts(latestTid) != ScarHelper::get_wts(tidOnRead)) {
          txn.abort_lock = true;
          break;
        }

      } else {
        txn.pendingResponses++;
        auto coordinatorID = partitioner.master_coordinator(partitionId);
        txn.network_size += MessageFactoryType::new_lock_message(
            *messages[coordinatorID], *table, writeKey.get_key(), i);
      }
    }

    sync_messages(txn);

    return txn.abort_lock;
  }

  void compute_commit_ts(TransactionType &txn) {

    auto &readSet = txn.readSet;
    auto &writeSet = txn.writeSet;

    uint64_t ts = 0;

    for (auto i = 0u; i < readSet.size(); i++) {
      ts = std::max(ts, ScarHelper::get_wts(readSet[i].get_tid()));
    }

    txn.commit_rts = ts;

    for (auto i = 0u; i < writeSet.size(); i++) {
      ts = std::max(ts, ScarHelper::get_rts(writeSet[i].get_tid()) + 1);
    }

    txn.commit_wts = ts;
  }

  bool validate_read_set(TransactionType &txn,
                         std::vector<std::unique_ptr<Message>> &messages) {

    auto &readSet = txn.readSet;
    auto &writeSet = txn.writeSet;

    // TODO: change to commit_rts if the transaction validates in SI
    uint64_t commit_ts = txn.commit_wts;

    auto isKeyInWriteSet = [&writeSet](const void *key) {
      for (auto &writeKey : writeSet) {
        if (writeKey.get_key() == key) {
          return true;
        }
      }
      return false;
    };

    for (auto i = 0u; i < readSet.size(); i++) {
      auto &readKey = readSet[i];

      if (readKey.get_local_index_read_bit()) {
        continue; // read only index does not need to validate
      }

      bool in_write_set = isKeyInWriteSet(readKey.get_key());
      if (in_write_set) {
        continue; // already validated in lock write set
      }

      auto tableId = readKey.get_table_id();
      auto partitionId = readKey.get_partition_id();
      auto table = db.find_table(tableId, partitionId);
      auto key = readKey.get_key();
      uint64_t tid = readKey.get_tid();

      if (partitioner.has_master_partition(partitionId)) {

        std::atomic<uint64_t> &latest_tid = table->search_metadata(key);
        uint64_t written_ts = tid;
        DCHECK(ScarHelper::is_locked(written_ts) == false);
        if (ScarHelper::validate_read_key(latest_tid, tid, commit_ts,
                                          written_ts)) {
          readKey.set_read_validation_success_bit();
          if (ScarHelper::get_wts(written_ts) != ScarHelper::get_wts(tid)) {
            DCHECK(ScarHelper::get_wts(written_ts) > ScarHelper::get_wts(tid));
            readKey.set_wts_change_in_read_validation_bit();
            readKey.set_tid(written_ts);
          }
        } else {
          txn.abort_read_validation = true;
          break;
        }
      } else {
        txn.pendingResponses++;
        auto coordinatorID = partitioner.master_coordinator(partitionId);
        txn.network_size += MessageFactoryType::new_read_validation_message(
            *messages[coordinatorID], *table, key, i, tid, commit_ts);
      }
    }

    sync_messages(txn);

    return !txn.abort_read_validation;
  }

  void write_and_replicate(TransactionType &txn,
                           std::vector<std::unique_ptr<Message>> &messages) {

    // no operation replication in Scar

    auto &readSet = txn.readSet;
    auto &writeSet = txn.writeSet;

    uint64_t commit_wts = txn.commit_wts;

    for (auto i = 0u; i < writeSet.size(); i++) {
      auto &writeKey = writeSet[i];
      auto tableId = writeKey.get_table_id();
      auto partitionId = writeKey.get_partition_id();
      auto table = db.find_table(tableId, partitionId);

      // write
      if (partitioner.has_master_partition(partitionId)) {
        auto key = writeKey.get_key();
        auto value = writeKey.get_value();
        table->update(key, value);
      } else {
        txn.pendingResponses++;
        auto coordinatorID = partitioner.master_coordinator(partitionId);
        txn.network_size += MessageFactoryType::new_write_message(
            *messages[coordinatorID], *table, writeKey.get_key(),
            writeKey.get_value());
      }

      // value replicate

      std::size_t replicate_count = 0;

      for (auto k = 0u; k < partitioner.total_coordinators(); k++) {

        // k does not have this partition
        if (!partitioner.is_partition_replicated_on(partitionId, k)) {
          continue;
        }

        // already write
        if (k == partitioner.master_coordinator(partitionId)) {
          continue;
        }

        replicate_count++;

        // local replicate
        if (k == txn.coordinator_id) {
          auto key = writeKey.get_key();
          auto value = writeKey.get_value();
          std::atomic<uint64_t> &tid = table->search_metadata(key);

          uint64_t last_tid = ScarHelper::lock(tid);
          DCHECK(ScarHelper::get_wts(last_tid) < commit_wts);
          table->update(key, value);
          ScarHelper::unlock(tid, commit_wts);
        } else {
          txn.pendingResponses++;
          auto coordinatorID = k;
          txn.network_size += MessageFactoryType::new_replication_message(
              *messages[coordinatorID], *table, writeKey.get_key(),
              writeKey.get_value(), commit_wts);
        }
      }

      DCHECK(replicate_count == partitioner.replica_num() - 1);
    }

    sync_messages(txn);
  }

  void release_lock(TransactionType &txn,
                    std::vector<std::unique_ptr<Message>> &messages) {

    auto &readSet = txn.readSet;
    auto &writeSet = txn.writeSet;

    uint64_t commit_wts = txn.commit_wts;

    for (auto i = 0u; i < writeSet.size(); i++) {
      auto &writeKey = writeSet[i];
      auto tableId = writeKey.get_table_id();
      auto partitionId = writeKey.get_partition_id();
      auto table = db.find_table(tableId, partitionId);

      // write
      if (partitioner.has_master_partition(partitionId)) {
        auto key = writeKey.get_key();
        auto value = writeKey.get_value();
        std::atomic<uint64_t> &tid = table->search_metadata(key);
        table->update(key, value);
        ScarHelper::unlock(tid, commit_wts);
      } else {
        auto coordinatorID = partitioner.master_coordinator(partitionId);
        txn.network_size += MessageFactoryType::new_release_lock_message(
            *messages[coordinatorID], *table, writeKey.get_key(), commit_wts);
      }
    }

    sync_messages(txn, false);
  }

  void sync_messages(TransactionType &txn, bool wait_response = true) {
    txn.message_flusher();
    if (wait_response) {
      while (txn.pendingResponses > 0) {
        txn.remote_request_handler();
      }
    }
  }

private:
  DatabaseType &db;
  const ContextType &context;
  Partitioner &partitioner;
};

} // namespace scar
