//
// BackgroundDB.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "BackgroundDB.hh"
#include "DataFile.hh"
#include "Database.hh"
#include "SequenceTracker.hh"
#include "c4ExceptionUtils.hh"

namespace litecore {
    using namespace actor;
    using namespace std::placeholders;
    using namespace std;


    BackgroundDB::BackgroundDB(Database *db)
    :access_lock(db->dataFile()->openAnother(this))
    ,_database(db)
    { }


    void BackgroundDB::close() {
        use([this](DataFile* &df) {
            delete df;
            df = nullptr;
        });
    }

    BackgroundDB::~BackgroundDB() {
        close();
    }


    alloc_slice BackgroundDB::blobAccessor(const fleece::impl::Dict *dict) const {
        return _database->blobAccessor(dict);
    }


    void BackgroundDB::externalTransactionCommitted(const SequenceTracker &sourceTracker) {
        notifyTransactionObservers();
    }


    void BackgroundDB::useInTransaction(TransactionTask task) {
        use([=](DataFile* dataFile) {
            if (!dataFile)
                return;
            Transaction t(dataFile);
            SequenceTracker sequenceTracker;
            sequenceTracker.beginTransaction();

            bool commit;
            try {
                commit = task(dataFile, &sequenceTracker);
            } catch (const exception &) {
                t.abort();
                sequenceTracker.endTransaction(false);
                throw;
            }

            if (!commit) {
                t.abort();
                sequenceTracker.endTransaction(false);
                return;
            }

            t.commit();
            // Notify other Database instances of any changes:
            t.notifyCommitted(sequenceTracker);
            sequenceTracker.endTransaction(true);
            // Notify my own observers:
            notifyTransactionObservers();
        });
    }


    void BackgroundDB::addTransactionObserver(TransactionObserver *obs) {
        LOCK(_transactionObserversMutex);
        _transactionObservers.push_back(obs);
    }


    void BackgroundDB::removeTransactionObserver(TransactionObserver* obs) {
        LOCK(_transactionObserversMutex);
        auto i = std::find(_transactionObservers.begin(), _transactionObservers.end(), obs);
        if (i != _transactionObservers.end())
            _transactionObservers.erase(i);
    }


    void BackgroundDB::notifyTransactionObservers() {
        LOCK(_transactionObserversMutex);
        for (auto obs : _transactionObservers)
            obs->transactionCommitted();
    }

}
