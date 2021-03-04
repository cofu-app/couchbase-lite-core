//
// c4Database.hh
//
// Copyright (c) 2019 Couchbase, Inc All rights reserved.
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
#include "c4Database.h"
#include "c4DocumentTypes.h"
#include "c4Query.h"
#include "c4Index.h"
#include "function_ref.hh"
#include <functional>
#include <memory>
#include <optional>
#include <vector>

C4_ASSUME_NONNULL_BEGIN


struct C4ReplicatorParameters;

namespace c4Internal {
    class Database;
    Database* asInternal(C4Database*);
}
namespace litecore::websocket {
    class WebSocket;
}


/// Derives an encryption key from a user-entered password.
std::optional<C4EncryptionKey> C4EncryptionKeyFromPassword(fleece::slice password,
                                                           C4EncryptionAlgorithm = kC4EncryptionAES256);


/// A LiteCore database connection.
struct C4Database final : public fleece::RefCounted, public C4Base {
public:
    // Lifecycle:

    static bool fileExists(slice name, slice inDirectory);
    static void copyFile(slice sourcePath,
                         slice destinationName,
                         const C4DatabaseConfig2&);
    static bool deleteFile(slice name, slice inDirectory);
    static bool deleteFile(slice path);

    static Retained<C4Database> open(slice name,
                                     const C4DatabaseConfig2&);
    
    static Retained<C4Database> open(slice path,
                                     C4DatabaseFlags,
                                     const C4EncryptionKey* C4NULLABLE = nullptr);

    static void shutdownLiteCore();

    Retained<C4Database> openAgain();

    void close();
    void closeAndDeleteFile();
    void rekey(const C4EncryptionKey* C4NULLABLE key);
    void maintenance(C4MaintenanceType t);

    // Attributes:

    slice name() const noexcept FLPURE;
    alloc_slice path() const;
    const C4DatabaseConfig2& config() const noexcept FLPURE;
    alloc_slice peerIDString() const;
    C4UUID publicUUID() const;
    C4UUID privateUUID() const;

    uint64_t getDocumentCount() const;
    C4SequenceNumber getLastSequence() const;

    // Transactions:

    /** Manages a transaction safely. The constructor begins a transaction, and \ref commit
        commits it. If the Transaction object exits scope without being committed, it aborts. */
    class Transaction {
    public:
        explicit Transaction(C4Database* db):_db(db) {db->beginTransaction();}
        void commit()                       {auto db = _db; _db = nullptr; db->endTransaction(true);}
        ~Transaction()                      {if (_db) _db->endTransaction(false);}
    private:
        C4Database* C4NULLABLE _db;
    };

    bool isInTransaction() const noexcept FLPURE;

    // Documents:

    Retained<C4Document> getDocument(slice docID,
                                     bool mustExist = true,
                                     C4DocContentLevel content = kDocGetCurrentRev) const;

    Retained<C4Document> getDocumentBySequence(C4SequenceNumber sequence) const;

    Retained<C4Document> putDocument(const C4DocPutRequest &rq,
                                     size_t* C4NULLABLE outCommonAncestorIndex,
                                     C4Error *outError);

    std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs,
                                           const std::vector<slice> &revIDs,
                                           unsigned maxAncestors,
                                           bool mustHaveBodies,
                                           C4RemoteID remoteDBID) const;

    bool markDocumentSynced(slice docID,
                            slice revID,
                            C4SequenceNumber sequence,
                            C4RemoteID remoteID);

    bool getRawDocument(slice storeName,
                        slice key,
                        fleece::function_ref<void(C4RawDocument* C4NULLABLE)> callback);

    void putRawDocument(slice storeName, const C4RawDocument&);

    bool purgeDocument(slice docID);

    // Fleece-related utilities for document encoding:

    alloc_slice encodeJSON(slice jsonData) const;
    FLEncoder createFleeceEncoder() const;
    FLEncoder sharedFleeceEncoder() const;
    FLSharedKeys sharedFleeceKeys() const;

    // Observers:

    using DatabaseObserverCallback = std::function<void(C4DatabaseObserver*)>;
    using DocumentObserverCallback = std::function<void(C4DocumentObserver*,
                                                        slice docID,
                                                        C4SequenceNumber)>;

    std::unique_ptr<C4DatabaseObserver> observe(DatabaseObserverCallback);

    std::unique_ptr<C4DocumentObserver> observeDocument(slice docID,
                                                        DocumentObserverCallback);

    // Expiration:

    bool mayHaveExpiration() const;
    bool startHousekeeping();
    int64_t purgeExpiredDocs();

    bool setExpiration(slice docID, C4Timestamp timestamp);
    C4Timestamp getExpiration(slice docID) const;
    C4Timestamp nextDocExpiration() const;

    // Blobs:

    C4BlobStore& blobStore();

    // Queries & Indexes:

    Retained<C4Query> newQuery(C4QueryLanguage language,
                               C4Slice queryExpression,
                               int* C4NULLABLE outErrorPos = nullptr);

    void createIndex(slice name,
                     slice indexSpecJSON,
                     C4IndexType indexType,
                     const C4IndexOptions* C4NULLABLE indexOptions);

    void deleteIndex(slice name);

    alloc_slice getIndexesInfo(bool fullInfo = true) const;

    alloc_slice getIndexRows(slice name) const;

    // Replicator:

    C4RemoteID getRemoteDBID(slice remoteAddress,
                             bool canCreate);
    alloc_slice getRemoteDBAddress(C4RemoteID remoteID);

    Retained<C4Replicator> newReplicator(C4Address serverAddress,
                                         slice remoteDatabaseName,
                                         const C4ReplicatorParameters &params);

    Retained<C4Replicator> newReplicator(C4Socket *openSocket,
                                         const C4ReplicatorParameters &params);
    Retained<C4Replicator> newReplicator(litecore::websocket::WebSocket *openSocket,
                                         const C4ReplicatorParameters &params);

#ifdef COUCHBASE_ENTERPRISE
    Retained<C4Replicator> newLocalReplicator(C4Database *otherLocalDB,
                                              const C4ReplicatorParameters &params);
#endif

// internal or deprecated:
    void beginTransaction();
    void endTransaction(bool commit);

    // Evaluates a SQLite (not N1QL!) query and returns the results. Used only by the `cblite` tool.
    alloc_slice rawQuery(slice sqliteQuery);

    static Retained<C4Database> open(slice path, const C4DatabaseConfig &config) {
        return new C4Database(path, config);
    }

    static void copyFileToPath(slice sourcePath, slice destinationPath, const C4DatabaseConfig&);
    const C4DatabaseConfig& configV1() const noexcept FLPURE;
    void lockClientMutex() noexcept;
    void unlockClientMutex() noexcept;

    C4ExtraInfo extraInfo { };

private:
    friend c4Internal::Database* c4Internal::asInternal(C4Database *db);
    
    C4Database(slice path, C4DatabaseConfig config);
    ~C4Database();

    Retained<c4Internal::Database> _db;
    std::unique_ptr<C4BlobStore> _blobStore;
};

C4_ASSUME_NONNULL_END
