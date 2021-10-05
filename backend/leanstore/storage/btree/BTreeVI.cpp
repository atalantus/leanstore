#include "BTreeVI.hpp"

#include "leanstore/concurrency-recovery/CRMG.hpp"
// -------------------------------------------------------------------------------------
#include "gflags/gflags.h"
// -------------------------------------------------------------------------------------
#include <signal.h>
// -------------------------------------------------------------------------------------
using namespace std;
using namespace leanstore::storage;
using OP_RESULT = leanstore::OP_RESULT;
// -------------------------------------------------------------------------------------
// Assumptions made in this implementation:
// 1) We don't insert an already removed key
// 2) Secondary Versions contain delta
// Keep in mind that garbage collection may leave pages completely empty
// Missing points: FatTuple::remove, garbage leaves can escape from us
namespace leanstore
{
namespace storage
{
namespace btree
{
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::lookup(u8* o_key, u16 o_key_length, function<void(const u8*, u16)> payload_callback)
{
   if (cr::activeTX().isSerializable()) {
      return lookupPessimistic(o_key, o_key_length, payload_callback);
   }
   const OP_RESULT ret = lookupOptimistic(o_key, o_key_length, payload_callback);
   if (ret == OP_RESULT::OTHER) {
      return lookupPessimistic(o_key, o_key_length, payload_callback);
   } else {
      return ret;
   }
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::lookupPessimistic(u8* key_buffer, const u16 key_length, function<void(const u8*, u16)> payload_callback)
{
   MutableSlice m_key(key_buffer, key_length);
   Slice key(key_buffer, key_length);
   jumpmuTry()
   {
      BTreeSharedIterator iterator(*static_cast<BTreeGeneric*>(this),
                                   cr::activeTX().isSerializable() ? LATCH_FALLBACK_MODE::EXCLUSIVE : LATCH_FALLBACK_MODE::SHARED);
      auto ret = iterator.seekExact(key);
      explainIfNot(ret == OP_RESULT::OK);
      if (ret != OP_RESULT::OK) {
         jumpmu_return OP_RESULT::NOT_FOUND;
      }
      [[maybe_unused]] const auto primary_version = *reinterpret_cast<const ChainedTuple*>(iterator.value().data());
      iterator.assembleKey();
      auto reconstruct = reconstructTuple(iterator.key(), iterator.value(), [&](Slice value) { payload_callback(value.data(), value.length()); });
      COUNTERS_BLOCK()
      {
         WorkerCounters::myCounters().cc_read_chains[dt_id]++;
         WorkerCounters::myCounters().cc_read_versions_visited[dt_id] += std::get<1>(reconstruct);
      }
      ret = std::get<0>(reconstruct);
      // -------------------------------------------------------------------------------------
      if (cr::activeTX().isOLAP() && ret == OP_RESULT::NOT_FOUND) {
         BTreeSharedIterator g_iterator(*static_cast<BTreeGeneric*>(graveyard));
         OP_RESULT ret = g_iterator.seekExact(key);
         if (ret == OP_RESULT::OK) {
            iterator.assembleKey();
            reconstruct = reconstructTuple(iterator.key(), iterator.value(), [&](Slice value) { payload_callback(value.data(), value.length()); });
         }
      }
      // -------------------------------------------------------------------------------------
      if (ret != OP_RESULT::ABORT_TX && ret != OP_RESULT::OK) {  // For debugging
         cout << endl;
         cout << u64(std::get<1>(reconstruct)) << " , " << dt_id << endl;
         raise(SIGTRAP);
      }
      jumpmu_return ret;
   }
   jumpmuCatch() {}
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::lookupOptimistic(const u8* key, const u16 key_length, function<void(const u8*, u16)> payload_callback)
{
   while (true) {
      jumpmuTry()
      {
         HybridPageGuard<BTreeNode> leaf;
         findLeafCanJump(leaf, key, key_length);
         // -------------------------------------------------------------------------------------
         s16 pos = leaf->lowerBound<true>(key, key_length);
         if (pos != -1) {
            auto& tuple = *reinterpret_cast<Tuple*>(leaf->getPayload(pos));
            if (isVisibleForMe(tuple.worker_id, tuple.tx_id, false)) {
               u32 offset = 0;
               if (tuple.tuple_format == TupleFormat::CHAINED) {
                  offset = sizeof(ChainedTuple);
               } else if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
                  offset = sizeof(FatTupleDifferentAttributes);
               } else {
                  leaf.recheck();
                  UNREACHABLE();
               }
               payload_callback(leaf->getPayload(pos) + offset, leaf->getPayloadLength(pos) - offset);
               leaf.recheck();
               COUNTERS_BLOCK()
               {
                  WorkerCounters::myCounters().cc_read_chains[dt_id]++;
                  WorkerCounters::myCounters().cc_read_versions_visited[dt_id] += 1;
               }
               jumpmu_return OP_RESULT::OK;
            } else {
               jumpmu_break;
            }
         } else {
            leaf.recheck();
            raise(SIGTRAP);
            jumpmu_return OP_RESULT::NOT_FOUND;
         }
      }
      jumpmuCatch() {}
   }
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::updateSameSizeInPlace(u8* o_key,
                                         u16 o_key_length,
                                         function<void(u8* value, u16 value_size)> callback,
                                         UpdateSameSizeInPlaceDescriptor& update_descriptor)
{
   assert(!cr::activeTX().isReadOnly());
   cr::Worker::my().walEnsureEnoughSpace(PAGE_SIZE * 1);
   Slice key(o_key, o_key_length);
   OP_RESULT ret;
   volatile bool tried_converting_to_fat_tuple = false;
   // -------------------------------------------------------------------------------------
   // 20K instructions more
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(this));
      ret = iterator.seekExact(key);
      if (ret != OP_RESULT::OK) {
         if (cr::activeTX().isOLAP() && ret == OP_RESULT::NOT_FOUND) {
            const bool removed_tuple_found = graveyard->lookup(o_key, o_key_length, [&](const u8*, u16) {}) == OP_RESULT::OK;
            if (removed_tuple_found) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         }
         // -------------------------------------------------------------------------------------
         raise(SIGTRAP);
         jumpmu_return ret;
      }
      // -------------------------------------------------------------------------------------
   restart : {
      MutableSlice primary_payload = iterator.mutableValue();
      auto& tuple = *reinterpret_cast<Tuple*>(primary_payload.data());
      if (tuple.isWriteLocked() || !isVisibleForMe(tuple.worker_id, tuple.tx_id, true)) {
         jumpmu_return OP_RESULT::ABORT_TX;
      }
      if (cr::activeTX().isSerializable()) {
         if (FLAGS_2pl) {
            if (tuple.read_lock_counter > 0 && tuple.read_lock_counter != (1ull << cr::Worker::my().workerID())) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         } else {
            if (tuple.read_ts > cr::activeTX().TTS()) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         }
      }
      tuple.writeLock();
      COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains[dt_id]++; }
      // -------------------------------------------------------------------------------------
      if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
         const bool res =
             reinterpret_cast<FatTupleDifferentAttributes*>(&tuple)->update(iterator, o_key, o_key_length, callback, update_descriptor, *this);
         ensure(res);  // TODO: what if it fails, then we have to do something else
         // Attention: tuple pointer is not valid here
         reinterpret_cast<Tuple*>(iterator.mutableValue().data())->unlock();
         // -------------------------------------------------------------------------------------
         if (cr::activeTX().isSingleStatement()) {
            cr::Worker::my().commitTX();
         }
         // -------------------------------------------------------------------------------------
         iterator.markAsDirty();
         iterator.contentionSplit();
         // -------------------------------------------------------------------------------------
         jumpmu_return OP_RESULT::OK;
      }
      // -------------------------------------------------------------------------------------
      auto& tuple_head = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
      bool convert_to_fat_tuple = FLAGS_vi_fat_tuple && !tried_converting_to_fat_tuple && tuple_head.can_convert_to_fat_tuple &&
                                  tuple_head.command_id != Tuple::INVALID_COMMANDID &&
                                  !(tuple_head.worker_id == cr::Worker::my().workerID() && tuple_head.tx_id == cr::activeTX().TTS());
      if (convert_to_fat_tuple) {
         convert_to_fat_tuple &= cr::Worker::my().local_oltp_lwm < tuple_head.tx_id;
      }
      if (convert_to_fat_tuple) {
         convert_to_fat_tuple &= utils::RandomGenerator::getRandU64(0, cr::Worker::my().workers_count) == 0;
      }
      if (convert_to_fat_tuple) {
         ensure(tuple.isWriteLocked());
         tried_converting_to_fat_tuple = true;
         const bool convert_ret = convertChainedToFatTupleDifferentAttributes(iterator);
         if (convert_ret) {
            iterator.leaf->has_garbage = true;
            COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_fat_tuple_convert[dt_id]++; }
         }
         goto restart;
         UNREACHABLE();
      }
      // -------------------------------------------------------------------------------------
   }
      bool update_without_versioning = (FLAGS_vi_update_version_elision || !FLAGS_mv || FLAGS_vi_fupdate_chained);
      if (update_without_versioning && !FLAGS_vi_fupdate_chained && FLAGS_vi_update_version_elision) {
         // Avoid creating version if all transactions are running in read-committed mode and the current tx is single-statement
         update_without_versioning &= cr::activeTX().isSingleStatement();
         for (u64 w_i = 0; w_i < cr::Worker::my().workers_count && update_without_versioning; w_i++) {
            update_without_versioning &= (cr::Worker::my().global_workers_in_progress_txid[w_i].load() & (1ull << 63));
         }
      }
      // -------------------------------------------------------------------------------------
      // Update in chained mode
      MutableSlice primary_payload = iterator.mutableValue();
      auto& tuple_head = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
      const u16 delta_and_descriptor_size = update_descriptor.size() + update_descriptor.diffLength();
      const u16 version_payload_length = delta_and_descriptor_size + sizeof(UpdateVersion);
      COMMANDID command_id = cr::Worker::my().command_id++;
      // -------------------------------------------------------------------------------------
      // Write the ChainedTupleDelta
      if (!update_without_versioning) {
         command_id = cr::Worker::my().insertVersion(dt_id, false, version_payload_length, [&](u8* version_payload) {
            auto& secondary_version = *new (version_payload) UpdateVersion(tuple_head.worker_id, tuple_head.tx_id, tuple_head.command_id, true);
            std::memcpy(secondary_version.payload, &update_descriptor, update_descriptor.size());
            BTreeLL::generateDiff(update_descriptor, secondary_version.payload + update_descriptor.size(), tuple_head.payload);
         });
         COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_versions_created[dt_id]++; }
      }
      // -------------------------------------------------------------------------------------
      // WAL
      auto wal_entry = iterator.leaf.reserveWALEntry<WALUpdateSSIP>(o_key_length + delta_and_descriptor_size);
      wal_entry->type = WAL_LOG_TYPE::WALUpdate;
      wal_entry->key_length = o_key_length;
      wal_entry->delta_length = delta_and_descriptor_size;
      wal_entry->before_worker_id = tuple_head.worker_id;
      wal_entry->before_tx_id = tuple_head.tx_id;
      wal_entry->before_command_id = tuple_head.command_id;
      std::memcpy(wal_entry->payload, o_key, o_key_length);
      std::memcpy(wal_entry->payload + o_key_length, &update_descriptor, update_descriptor.size());
      BTreeLL::generateDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), tuple_head.payload);
      callback(tuple_head.payload, primary_payload.length() - sizeof(ChainedTuple));  // Update
      BTreeLL::generateXORDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), tuple_head.payload);
      wal_entry.submit();
      // -------------------------------------------------------------------------------------
      tuple_head.worker_id = cr::Worker::my().workerID();
      tuple_head.tx_id = cr::activeTX().TTS();
      tuple_head.command_id = command_id;
      // -------------------------------------------------------------------------------------
      if (cr::activeTX().isSerializable()) {
         if (FLAGS_2pl) {
            // Nothing, the WorkerID + Commit HWM are the write lock
            tuple_head.read_lock_counter = (1ull << cr::Worker::my().workerID());
         } else {
            tuple_head.read_ts = cr::activeTX().TTS();
         }
      }
      // -------------------------------------------------------------------------------------
      tuple_head.unlock();
      iterator.markAsDirty();
      iterator.contentionSplit();
      // -------------------------------------------------------------------------------------
      if (cr::activeTX().isSingleStatement()) {
         cr::Worker::my().commitTX();
      }
      // -------------------------------------------------------------------------------------
      jumpmu_return OP_RESULT::OK;
   }
   jumpmuCatch() {}
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::insert(u8* o_key, u16 o_key_length, u8* value, u16 value_length)
{
   assert(!cr::activeTX().isReadOnly());
   cr::Worker::my().walEnsureEnoughSpace(PAGE_SIZE * 1);
   Slice key(o_key, o_key_length);
   const u16 payload_length = value_length + sizeof(ChainedTuple);
   // -------------------------------------------------------------------------------------
   while (true) {
      jumpmuTry()
      {
         BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(this));
         OP_RESULT ret = iterator.seekToInsert(key);
         if (ret == OP_RESULT::DUPLICATE) {
            MutableSlice primary_payload = iterator.mutableValue();
            auto& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
            if (primary_version.isWriteLocked() || !isVisibleForMe(primary_version.worker_id, primary_version.tx_id, true)) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
            ensure(false);  // Not implemented: maybe it has been removed but no GCed
         }
         ret = iterator.enoughSpaceInCurrentNode(key, payload_length);
         if (ret == OP_RESULT::NOT_ENOUGH_SPACE) {
            iterator.splitForKey(key);
            jumpmu_continue;
         }
         // -------------------------------------------------------------------------------------
         // WAL
         auto wal_entry = iterator.leaf.reserveWALEntry<WALInsert>(o_key_length + value_length);
         wal_entry->type = WAL_LOG_TYPE::WALInsert;
         wal_entry->key_length = o_key_length;
         wal_entry->value_length = value_length;
         std::memcpy(wal_entry->payload, o_key, o_key_length);
         std::memcpy(wal_entry->payload + o_key_length, value, value_length);
         wal_entry.submit();
         // -------------------------------------------------------------------------------------
         iterator.insertInCurrentNode(key, payload_length);
         MutableSlice payload = iterator.mutableValue();
         auto& primary_version = *new (payload.data()) ChainedTuple(cr::Worker::my().workerID(), cr::activeTX().TTS());
         std::memcpy(primary_version.payload, value, value_length);
         // -------------------------------------------------------------------------------------
         if (cr::activeTX().isSingleStatement()) {
            cr::Worker::my().commitTX();
         }
         // -------------------------------------------------------------------------------------
         iterator.markAsDirty();
         jumpmu_return OP_RESULT::OK;
      }
      jumpmuCatch() { UNREACHABLE(); }
   }
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::remove(u8* o_key, u16 o_key_length)
{
   // TODO: remove fat tuple
   assert(!cr::activeTX().isReadOnly());
   cr::Worker::my().walEnsureEnoughSpace(PAGE_SIZE * 1);
   Slice key(o_key, o_key_length);
   // -------------------------------------------------------------------------------------
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(this));
      OP_RESULT ret = iterator.seekExact(key);
      if (ret != OP_RESULT::OK) {
         if (cr::activeTX().isOLAP() && ret == OP_RESULT::NOT_FOUND) {
            const bool removed_tuple_found = graveyard->lookup(o_key, o_key_length, [&](const u8*, u16) {}) == OP_RESULT::OK;
            if (removed_tuple_found) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         }
         // -------------------------------------------------------------------------------------
         explainWhen(cr::activeTX().atLeastSI());
         jumpmu_return OP_RESULT::NOT_FOUND;
      }
      // -------------------------------------------------------------------------------------
      if (FLAGS_vi_fremove) {
         ret = iterator.removeCurrent();
         ensure(ret == OP_RESULT::OK);
         iterator.mergeIfNeeded();
         jumpmu_return OP_RESULT::OK;
      }
      // -------------------------------------------------------------------------------------
      auto payload = iterator.mutableValue();
      ChainedTuple& chain_head = *reinterpret_cast<ChainedTuple*>(payload.data());
      // -------------------------------------------------------------------------------------
      ensure(chain_head.tuple_format == TupleFormat::CHAINED);  // TODO: removing fat tuple is not supported atm
      if (chain_head.isWriteLocked() || !isVisibleForMe(chain_head.worker_id, chain_head.tx_id, true)) {
         jumpmu_return OP_RESULT::ABORT_TX;
      }
      if (cr::activeTX().isSerializable()) {
         if (FLAGS_2pl) {
            if (chain_head.read_lock_counter > 0 && chain_head.read_lock_counter != (1ull << cr::Worker::my().workerID())) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         } else {
            if (chain_head.read_ts > cr::activeTX().TTS()) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         }
      }
      ensure(!cr::activeTX().atLeastSI() || chain_head.is_removed == false);
      if (chain_head.is_removed) {
         jumpmu_return OP_RESULT::NOT_FOUND;
      }
      // -------------------------------------------------------------------------------------
      chain_head.writeLock();
      // -------------------------------------------------------------------------------------
      DanglingPointer dangling_pointer;
      dangling_pointer.bf = iterator.leaf.bf;
      dangling_pointer.latch_version_should_be = iterator.leaf.guard.version;
      dangling_pointer.head_slot = iterator.cur;
      const u16 value_length = iterator.value().length() - sizeof(ChainedTuple);
      const u16 version_payload_length = sizeof(RemoveVersion) + value_length + o_key_length;
      const COMMANDID command_id = cr::Worker::my().insertVersion(dt_id, true, version_payload_length, [&](u8* secondary_payload) {
         auto& secondary_version =
             *new (secondary_payload) RemoveVersion(chain_head.worker_id, chain_head.tx_id, chain_head.command_id, o_key_length, value_length);
         secondary_version.dangling_pointer = dangling_pointer;
         std::memcpy(secondary_version.payload, o_key, o_key_length);
         std::memcpy(secondary_version.payload + o_key_length, chain_head.payload, value_length);
      });
      // -------------------------------------------------------------------------------------
      // WAL
      auto wal_entry = iterator.leaf.reserveWALEntry<WALRemove>(o_key_length + value_length);
      wal_entry->type = WAL_LOG_TYPE::WALRemove;
      wal_entry->key_length = o_key_length;
      wal_entry->value_length = value_length;
      wal_entry->before_worker_id = chain_head.worker_id;
      wal_entry->before_tx_id = chain_head.tx_id;
      wal_entry->before_command_id = chain_head.command_id;
      std::memcpy(wal_entry->payload, o_key, o_key_length);
      std::memcpy(wal_entry->payload + o_key_length, chain_head.payload, value_length);
      wal_entry.submit();
      // -------------------------------------------------------------------------------------
      if (payload.length() - sizeof(ChainedTuple) > 1) {
         iterator.shorten(sizeof(ChainedTuple));
      }
      chain_head.is_removed = true;
      chain_head.worker_id = cr::Worker::my().workerID();
      chain_head.tx_id = cr::activeTX().TTS();
      chain_head.command_id = command_id;
      if (cr::activeTX().isSerializable()) {
         if (FLAGS_2pl) {
            chain_head.read_lock_counter = (1ull << cr::Worker::my().workerID());
         } else {
            chain_head.read_ts = cr::activeTX().TTS();
         }
      }
      // -------------------------------------------------------------------------------------
      chain_head.unlock();
      iterator.markAsDirty();
      // -------------------------------------------------------------------------------------
      if (cr::activeTX().isSingleStatement()) {
         cr::Worker::my().commitTX();
      }
      // -------------------------------------------------------------------------------------
      jumpmu_return OP_RESULT::OK;
   }
   jumpmuCatch() {}
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
// This undo implementation works only for rollback and not for undo operations during recovery
void BTreeVI::undo(void* btree_object, const u8* wal_entry_ptr, const u64)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   static_cast<void>(btree);
   const WALEntry& entry = *reinterpret_cast<const WALEntry*>(wal_entry_ptr);
   switch (entry.type) {
      case WAL_LOG_TYPE::WALInsert: {  // Assuming no insert after remove
         auto& insert_entry = *reinterpret_cast<const WALInsert*>(&entry);
         jumpmuTry()
         {
            Slice key(insert_entry.payload, insert_entry.key_length);
            // -------------------------------------------------------------------------------------
            BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
            OP_RESULT ret = iterator.seekExact(key);
            ensure(ret == OP_RESULT::OK);
            ret = iterator.removeCurrent();
            ensure(ret == OP_RESULT::OK);
            iterator.markAsDirty();  // TODO: write CLS
            iterator.mergeIfNeeded();
         }
         jumpmuCatch() {}
         break;
      }
      case WAL_LOG_TYPE::WALUpdate: {
         auto& update_entry = *reinterpret_cast<const WALUpdateSSIP*>(&entry);
         jumpmuTry()
         {
            Slice key(update_entry.payload, update_entry.key_length);
            // -------------------------------------------------------------------------------------
            BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
            OP_RESULT ret = iterator.seekExact(key);
            ensure(ret == OP_RESULT::OK);
            auto& tuple = *reinterpret_cast<Tuple*>(iterator.mutableValue().data());
            ensure(!tuple.isWriteLocked());
            if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
               reinterpret_cast<FatTupleDifferentAttributes*>(iterator.mutableValue().data())->undoLastUpdate();
            } else {
               auto& chain_head = *reinterpret_cast<ChainedTuple*>(iterator.mutableValue().data());
               // -------------------------------------------------------------------------------------
               chain_head.worker_id = update_entry.before_worker_id;
               chain_head.tx_id = update_entry.before_tx_id;
               chain_head.command_id = update_entry.before_command_id;
               const auto& update_descriptor =
                   *reinterpret_cast<const UpdateSameSizeInPlaceDescriptor*>(update_entry.payload + update_entry.key_length);
               BTreeLL::applyXORDiff(update_descriptor, chain_head.payload,
                                     update_entry.payload + update_entry.key_length + update_descriptor.size());
            }
            // -------------------------------------------------------------------------------------
            iterator.markAsDirty();
            jumpmu_return;
         }
         jumpmuCatch() { UNREACHABLE(); }
         break;
      }
      case WAL_LOG_TYPE::WALRemove: {
         auto& remove_entry = *reinterpret_cast<const WALRemove*>(&entry);
         Slice key(remove_entry.payload, remove_entry.key_length);
         // -------------------------------------------------------------------------------------
         jumpmuTry()
         {
            BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
            // -------------------------------------------------------------------------------------
            OP_RESULT ret = iterator.seekExact(key);
            ensure(ret == OP_RESULT::OK);
            // Resize
            const u16 new_primary_payload_length = remove_entry.value_length + sizeof(ChainedTuple);
            const Slice old_primary_payload = iterator.value();
            if (old_primary_payload.length() < new_primary_payload_length) {
               iterator.extendPayload(new_primary_payload_length);
            } else {
               iterator.shorten(new_primary_payload_length);
            }
            MutableSlice primary_payload = iterator.mutableValue();
            auto& primary_version = *new (primary_payload.data()) ChainedTuple(remove_entry.before_worker_id, remove_entry.before_tx_id);
            std::memcpy(primary_version.payload, remove_entry.payload + remove_entry.key_length, remove_entry.value_length);
            primary_version.command_id = remove_entry.before_command_id;
            ensure(primary_version.is_removed == false);
            primary_version.unlock();
            iterator.markAsDirty();
         }
         jumpmuCatch() { UNREACHABLE(); }
         break;
      }
      default: {
         break;
      }
   }
}
// -------------------------------------------------------------------------------------
SpaceCheckResult BTreeVI::checkSpaceUtilization(void* btree_object, BufferFrame& bf)
{
   if (!FLAGS_xmerge) {
      return SpaceCheckResult::NOTHING;
   }
   // -------------------------------------------------------------------------------------
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   Guard bf_guard(bf.header.latch);
   bf_guard.toOptimisticOrJump();
   if (bf.page.dt_id != btree.dt_id) {
      jumpmu::jump();
   }
   HybridPageGuard<BTreeNode> c_guard(std::move(bf_guard), &bf);
   if (!c_guard->is_leaf || !triggerPageWiseGarbageCollection(c_guard)) {
      return BTreeGeneric::checkSpaceUtilization(static_cast<BTreeGeneric*>(&btree), bf);
   }
   // -------------------------------------------------------------------------------------
   c_guard.toExclusive();
   c_guard.incrementGSN();
   for (u16 s_i = 0; s_i < c_guard->count; s_i++) {
      auto& tuple = *reinterpret_cast<Tuple*>(c_guard->getPayload(s_i));
      if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
         auto& fat_tuple = *reinterpret_cast<FatTupleDifferentAttributes*>(c_guard->getPayload(s_i));
         u64 offset = fat_tuple.value_length;
         auto delta = reinterpret_cast<FatTupleDifferentAttributes::Delta*>(fat_tuple.payload + offset);
         auto update_descriptor = reinterpret_cast<UpdateSameSizeInPlaceDescriptor*>(delta->payload);
         WORKERID prev_worker_id = fat_tuple.worker_id;
         TXID prev_tx_id = fat_tuple.tx_id;
         COMMANDID prev_command_id = fat_tuple.command_id;
         for (u64 v_i = 0; v_i < fat_tuple.deltas_count; v_i++) {
            const u16 delta_and_descriptor_size = update_descriptor->size() + update_descriptor->diffLength();
            const u16 version_payload_length = delta_and_descriptor_size + sizeof(UpdateVersion);
            cr::Worker::my().versions_space.insertVersion(
                prev_worker_id, prev_tx_id, prev_command_id, btree.dt_id, false, version_payload_length,
                [&](u8* version_payload) {
                   auto& secondary_version = *new (version_payload) UpdateVersion(delta->worker_id, delta->worker_tx_id, delta->command_id, true);
                   std::memcpy(secondary_version.payload, update_descriptor, delta_and_descriptor_size);
                },
                false);
            // -------------------------------------------------------------------------------------
            prev_worker_id = delta->worker_id;
            prev_tx_id = delta->worker_tx_id;
            prev_command_id = delta->command_id;
            // -------------------------------------------------------------------------------------
            offset += sizeof(FatTupleDifferentAttributes::Delta) + delta_and_descriptor_size;
            delta = reinterpret_cast<FatTupleDifferentAttributes::Delta*>(fat_tuple.payload + offset);
            update_descriptor = reinterpret_cast<UpdateSameSizeInPlaceDescriptor*>(delta->payload);
         }
         // -------------------------------------------------------------------------------------
         const FatTupleDifferentAttributes old_fat_tuple = fat_tuple;
         u8* value = fat_tuple.payload;
         auto& chained_tuple = *new (c_guard->getPayload(s_i)) ChainedTuple(old_fat_tuple.worker_id, old_fat_tuple.tx_id);
         chained_tuple.command_id = old_fat_tuple.command_id;
         std::memmove(chained_tuple.payload, value, old_fat_tuple.value_length);
         const u16 new_length = old_fat_tuple.value_length + sizeof(ChainedTuple);
         ensure(new_length < c_guard->getPayloadLength(s_i));
         c_guard->shortenPayload(s_i, new_length);
         ensure(tuple.tuple_format == TupleFormat::CHAINED);
         COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_fat_tuple_decompose[btree.dt_id]++; }
      }
   }
   c_guard->has_garbage = false;
   c_guard.unlock();
   // -------------------------------------------------------------------------------------
   const SpaceCheckResult xmerge_ret = BTreeGeneric::checkSpaceUtilization(static_cast<BTreeGeneric*>(&btree), bf);
   if (xmerge_ret == SpaceCheckResult::PICK_ANOTHER_BF) {
      return SpaceCheckResult::PICK_ANOTHER_BF;
   } else {
      return SpaceCheckResult::RESTART_SAME_BF;
   }
}
// -------------------------------------------------------------------------------------
void BTreeVI::todo(void* btree_object, const u8* entry_ptr, const u64 version_worker_id, const u64 version_tx_id, const bool called_before)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   // Only point-gc and for removed tuples
   const auto& version = *reinterpret_cast<const RemoveVersion*>(entry_ptr);
   if (FLAGS_vi_dangling_pointer) {
      assert(version.dangling_pointer.bf != nullptr);
      // Optimistic fast path
      jumpmuTry()
      {
         BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree), version.dangling_pointer.bf,
                                         version.dangling_pointer.latch_version_should_be);
         auto& node = iterator.leaf;
         auto& head = *reinterpret_cast<ChainedTuple*>(node->getPayload(version.dangling_pointer.head_slot));
         // Being chained is implicit because we check for version, so the state can not be changed after staging the todo
         ensure(head.tuple_format == TupleFormat::CHAINED && !head.isWriteLocked() && head.worker_id == version_worker_id &&
                head.tx_id == version_tx_id && head.is_removed);
         node->removeSlot(version.dangling_pointer.head_slot);
         iterator.markAsDirty();
         iterator.mergeIfNeeded();
         jumpmu_return;
      }
      jumpmuCatch() {}
   }
   // -------------------------------------------------------------------------------------
   Slice key(version.payload, version.key_length);
   OP_RESULT ret;
   // -------------------------------------------------------------------------------------
   if (called_before) {
      // Delete from graveyard
      ensure(version_tx_id < cr::Worker::my().local_olap_lwm);
      jumpmuTry()
      {
         BTreeExclusiveIterator g_iterator(*static_cast<BTreeGeneric*>(btree.graveyard));
         ret = g_iterator.seekExact(key);
         ensure(ret == OP_RESULT::OK);
         ret = g_iterator.removeCurrent();
         ensure(ret == OP_RESULT::OK);
         g_iterator.markAsDirty();
      }
      jumpmuCatch() {}
      return;
   }
   // -------------------------------------------------------------------------------------
   // TODO: Corner cases if the tuple got inserted after a remove
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
      ret = iterator.seekExact(key);
      if (ret != OP_RESULT::OK) {
         jumpmu_return;  // TODO:
      }
      // ensure(ret == OP_RESULT::OK);
      // -------------------------------------------------------------------------------------
      MutableSlice primary_payload = iterator.mutableValue();
      {
         // Checks
         const auto& tuple = *reinterpret_cast<const Tuple*>(primary_payload.data());
         if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
            jumpmu_return;
         }
      }
      // -------------------------------------------------------------------------------------
      ChainedTuple& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
      if (!primary_version.isWriteLocked()) {
         if (primary_version.worker_id == version_worker_id && primary_version.tx_id == version_tx_id && primary_version.is_removed) {
            if (primary_version.tx_id < cr::Worker::my().local_olap_lwm) {
               ret = iterator.removeCurrent();
               iterator.markAsDirty();
               ensure(ret == OP_RESULT::OK);
               iterator.mergeIfNeeded();
               COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_todo_removed[btree.dt_id]++; }
            } else if (primary_version.tx_id < cr::Worker::my().local_oltp_lwm) {
               // Move to graveyard
               {
                  BTreeExclusiveIterator g_iterator(*static_cast<BTreeGeneric*>(btree.graveyard));
                  OP_RESULT g_ret = g_iterator.insertKV(key, iterator.value());
                  ensure(g_ret == OP_RESULT::OK);
                  g_iterator.markAsDirty();
               }
               ret = iterator.removeCurrent();
               ensure(ret == OP_RESULT::OK);
               iterator.markAsDirty();
               iterator.mergeIfNeeded();
               COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_todo_moved_gy[btree.dt_id]++; }
            }
         }
      }
   }
   jumpmuCatch() { UNREACHABLE(); }
}
// -------------------------------------------------------------------------------------
void BTreeVI::unlock(void* btree_object, const u8* entry_ptr)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   const auto& todo_entry = *reinterpret_cast<const UnlockEntry*>(entry_ptr);
   // -------------------------------------------------------------------------------------
   Slice key(todo_entry.key, todo_entry.key_length);
   OP_RESULT ret;
   // -------------------------------------------------------------------------------------
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
      ret = iterator.seekExact(key);
      ensure(ret == OP_RESULT::OK);
      // -------------------------------------------------------------------------------------
      MutableSlice primary_payload = iterator.mutableValue();
      Tuple& primary_version = *reinterpret_cast<Tuple*>(primary_payload.data());
      primary_version.read_lock_counter &= ~(1ull << cr::Worker::my().workerID());
   }
   jumpmuCatch() { UNREACHABLE(); }
}
// -------------------------------------------------------------------------------------
struct DTRegistry::DTMeta BTreeVI::getMeta()
{
   DTRegistry::DTMeta btree_meta = {.iterate_children = iterateChildrenSwips,
                                    .find_parent = findParent,
                                    .check_space_utilization = checkSpaceUtilization,
                                    .checkpoint = checkpoint,
                                    .undo = undo,
                                    .todo = todo,
                                    .unlock = unlock,
                                    .serialize = serialize,
                                    .deserialize = deserialize};
   return btree_meta;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::scanDesc(u8* o_key, u16 o_key_length, function<bool(const u8*, u16, const u8*, u16)> callback, function<void()>)
{
   if (cr::activeTX().isOLAP()) {
      TODOException();
      return OP_RESULT::ABORT_TX;
   } else {
      return scan<false>(o_key, o_key_length, callback);
   }
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::scanAsc(u8* o_key,
                           u16 o_key_length,
                           function<bool(const u8* key, u16 key_length, const u8* value, u16 value_length)> callback,
                           function<void()>)
{
   if (cr::activeTX().isOLAP()) {
      return scanOLAP(o_key, o_key_length, callback);
   } else {
      return scan<true>(o_key, o_key_length, callback);
   }
}
// -------------------------------------------------------------------------------------
// TODO: Implement inserts after remove cases
std::tuple<OP_RESULT, u16> BTreeVI::reconstructChainedTuple([[maybe_unused]] Slice key, Slice payload, std::function<void(Slice value)> callback)
{
   u16 chain_length = 1;
   u16 materialized_value_length;
   std::unique_ptr<u8[]> materialized_value;
   const ChainedTuple& chain_head = *reinterpret_cast<const ChainedTuple*>(payload.data());
   if (isVisibleForMe(chain_head.worker_id, chain_head.tx_id, false)) {
      if (chain_head.is_removed) {
         return {OP_RESULT::NOT_FOUND, 1};
      } else {
         callback(Slice(chain_head.payload, payload.length() - sizeof(ChainedTuple)));
         return {OP_RESULT::OK, 1};
      }
   }
   // -------------------------------------------------------------------------------------
   // Head is not visible
   materialized_value_length = payload.length() - sizeof(ChainedTuple);
   materialized_value = std::make_unique<u8[]>(materialized_value_length);
   std::memcpy(materialized_value.get(), chain_head.payload, materialized_value_length);
   WORKERID next_worker_id = chain_head.worker_id;
   TXID next_tx_id = chain_head.tx_id;
   COMMANDID next_command_id = chain_head.command_id;
   // -------------------------------------------------------------------------------------
   while (true) {
      bool found = cr::Worker::my().retrieveVersion(next_worker_id, next_tx_id, next_command_id, [&](const u8* version_payload, u64 version_length) {
         const auto& version = *reinterpret_cast<const Version*>(version_payload);
         if (version.type == Version::TYPE::UPDATE) {
            const auto& update_version = *reinterpret_cast<const UpdateVersion*>(version_payload);
            if (update_version.is_delta) {
               // Apply delta
               const auto& update_descriptor = *reinterpret_cast<const UpdateSameSizeInPlaceDescriptor*>(update_version.payload);
               BTreeLL::applyDiff(update_descriptor, materialized_value.get(), update_version.payload + update_descriptor.size());
            } else {
               materialized_value_length = version_length - sizeof(UpdateVersion);
               materialized_value = std::make_unique<u8[]>(materialized_value_length);
               std::memcpy(materialized_value.get(), update_version.payload, materialized_value_length);
            }
         } else if (version.type == Version::TYPE::REMOVE) {
            const auto& remove_version = *reinterpret_cast<const RemoveVersion*>(version_payload);
            materialized_value_length = remove_version.value_length;
            materialized_value = std::make_unique<u8[]>(materialized_value_length);
            std::memcpy(materialized_value.get(), remove_version.payload, materialized_value_length);
         } else {
            UNREACHABLE();
         }
         // -------------------------------------------------------------------------------------
         next_worker_id = version.worker_id;
         next_tx_id = version.tx_id;
         next_command_id = version.command_id;
      });
      if (!found) {
         return {OP_RESULT::NOT_FOUND, chain_length};
      }
      if (isVisibleForMe(next_worker_id, next_tx_id, false)) {
         callback(Slice(materialized_value.get(), materialized_value_length));
         return {OP_RESULT::OK, chain_length};
      }
      chain_length++;
      ensure(chain_length <= FLAGS_vi_max_chain_length);
   }
   return {OP_RESULT::NOT_FOUND, chain_length};
}
// -------------------------------------------------------------------------------------
}  // namespace btree
}  // namespace storage
}  // namespace leanstore
