//
// c4Collection.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "c4Base.hh"
#include "c4DatabaseTypes.h"
#include "c4DocumentTypes.h"
#include "c4IndexTypes.h"
#include "c4QueryTypes.h"
#include "access_lock.hh"
#include "function_ref.hh"
#include <functional>
#include <memory>
#include <unordered_set>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {
    class C4CollectionObserverImpl;
    class C4DocumentObserverImpl;
    class DatabaseImpl;
    class ExclusiveTransaction;
    class KeyStore;
    class Record;
    class SequenceTracker;
}


struct C4Collection : public fleece::RefCounted,
                      public C4Base,
                      public fleece::InstanceCountedIn<C4Collection>
{
    // Accessors:
    
    slice name() const                          {return _name;}

    C4Database* database();
    const C4Database* database() const          {return const_cast<C4Collection*>(this)->database();}

    virtual uint64_t getDocumentCount() const =0;

    virtual C4SequenceNumber getLastSequence() const =0;

    // Documents:

    static C4Document* documentContainingValue(FLValue) noexcept;

    virtual Retained<C4Document> getDocument(slice docID,
                                             bool mustExist = true,
                                             C4DocContentLevel content = kDocGetCurrentRev) const =0;

    virtual Retained<C4Document> getDocumentBySequence(C4SequenceNumber sequence) const =0;

    virtual Retained<C4Document> putDocument(const C4DocPutRequest &rq,
                                             size_t* C4NULLABLE outCommonAncestorIndex,
                                             C4Error *outError) =0;

    virtual Retained<C4Document> createDocument(slice docID,
                                                slice revBody,
                                                C4RevisionFlags revFlags,
                                                C4Error *outError) =0;

    // Purging & Expiration:

    virtual bool purgeDoc(slice docID) =0;

    virtual bool setExpiration(slice docID, C4Timestamp timestamp) =0;
    virtual C4Timestamp getExpiration(slice docID) const =0;

    virtual C4Timestamp nextDocExpiration() const =0;
    virtual int64_t purgeExpiredDocs() =0;

    // Indexes:

    virtual void createIndex(slice name,
                             slice indexSpecJSON,
                             C4IndexType indexType,
                             const C4IndexOptions* C4NULLABLE indexOptions =nullptr) =0;

    virtual void deleteIndex(slice name) =0;

    virtual alloc_slice getIndexesInfo(bool fullInfo = true) const =0;

    virtual alloc_slice getIndexRows(slice name) const =0;

    // Observers:

    using CollectionObserverCallback = std::function<void(C4CollectionObserver*)>;
    using DocumentObserverCallback = std::function<void(C4DocumentObserver*,
                                                        slice docID,
                                                        C4SequenceNumber)>;

    virtual std::unique_ptr<C4CollectionObserver> observe(CollectionObserverCallback) =0;

    virtual std::unique_ptr<C4DocumentObserver> observeDocument(slice docID,
                                                                DocumentObserverCallback) =0;

    // Internal use only:

    static Retained<C4Collection> newCollection(C4Database*, slice name, litecore::KeyStore&);
    virtual void close() =0;

    virtual litecore::KeyStore& keyStore() const =0;
    virtual litecore::access_lock<litecore::SequenceTracker>& sequenceTracker() =0;

    virtual void transactionBegan() =0;
    virtual bool changedDuringTransaction() =0;
    virtual void transactionEnding(litecore::ExclusiveTransaction*, bool committing) =0;
    virtual void externalTransactionCommitted(const litecore::SequenceTracker &sourceTracker) =0;
    
    virtual Retained<C4Document> newDocumentInstance(const litecore::Record&) =0;
    virtual void documentSaved(C4Document*) =0;

    virtual std::vector<alloc_slice> findDocAncestors(const std::vector<slice> &docIDs,
                                                      const std::vector<slice> &revIDs,
                                                      unsigned maxAncestors,
                                                      bool mustHaveBodies,
                                                      C4RemoteID remoteDBID) const =0;
    virtual bool markDocumentSynced(slice docID,
                                    slice revID,
                                    C4SequenceNumber sequence,
                                    C4RemoteID remoteID) =0;

    virtual void findBlobReferences(const fleece::function_ref<bool(FLDict)>&) =0;

    virtual void startHousekeeping() =0;
    virtual bool stopHousekeeping() =0;

protected:
    friend class litecore::DatabaseImpl;
    friend class litecore::C4CollectionObserverImpl;
    friend class litecore::C4DocumentObserverImpl;

    C4Collection(C4Database*, slice name);

    C4Database* C4NULLABLE  _database;
    alloc_slice             _name;
};

C4_ASSUME_NONNULL_END