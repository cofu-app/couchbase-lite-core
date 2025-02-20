//
// Replicator.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
//  https://github.com/couchbase/couchbase-lite-core/wiki/Replication-Protocol

#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "Pusher.hh"
#include "Puller.hh"
#include "Checkpoint.hh"
#include "DBAccess.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "Logging.hh"
#include "Headers.hh"
#include "BLIP.hh"
#include "Address.hh"
#include "Instrumentation.hh"
#include "c4DocEnumerator.h"
#include "c4Socket.h"           // for error codes
#include "c4Transaction.hh"

using namespace std;
using namespace std::placeholders;
using namespace fleece;
using namespace litecore::blip;


namespace litecore { namespace repl {

    struct StoppingErrorEntry {
        C4Error err;
        bool isFatal;
        slice msg;
    };

    // Errors treated specially by onError()
    static constexpr StoppingErrorEntry StoppingErrors[] = {
        {{ LiteCoreDomain, kC4ErrorUnexpectedError,0 }, true, "An exception was thrown"_sl},
        {{ WebSocketDomain, 403, 0}, true, "An attempt was made to perform an unauthorized action"_sl},
        {{ WebSocketDomain, 503, 0 }, false, "The server is over capacity"_sl}
    };


    Replicator::Replicator(C4Database* db,
                           websocket::WebSocket *webSocket,
                           Delegate &delegate,
                           Options options)
    :Worker(new Connection(webSocket, options.properties, *this),
            nullptr,
            options,
            make_shared<DBAccess>(db, options.properties["disable_blob_support"_sl].asBool()),
            "Repl")
    ,_delegate(&delegate)
    ,_connectionState(connection().state())
    ,_pushStatus(options.push == kC4Disabled ? kC4Stopped : kC4Busy)
    ,_pullStatus(options.pull == kC4Disabled ? kC4Stopped : kC4Busy)
    ,_docsEnded(this, "docsEnded", &Replicator::notifyEndedDocuments, tuning::kMinDocEndedInterval, 100)
    ,_checkpointer(_options, webSocket->url())
    {
        _loggingID = string(alloc_slice(c4db_getPath(db))) + " " + _loggingID;
        _passive = _options.pull <= kC4Passive && _options.push <= kC4Passive;
        _important = 2;

        logInfo("%s", string(options).c_str());

        if (options.push != kC4Disabled) {
            _pusher = new Pusher(this, _checkpointer);
        } else {
            for (auto profile : {"subChanges", "getAttachment", "proveAttachment"})
                registerHandler(profile,  &Replicator::returnForbidden);
        }
        
        if (options.pull != kC4Disabled) {
            _puller = new Puller(this);
        } else {
            for (auto profile : {"changes", "proposeChanges", "rev", "norev"})
                registerHandler(profile,  &Replicator::returnForbidden);
        }

        actor::Timer::duration saveDelay = tuning::kDefaultCheckpointSaveDelay;
        if (auto i = options.properties[kC4ReplicatorCheckpointInterval].asInt(); i > 0)
            saveDelay = chrono::seconds(i);
        _checkpointer.enableAutosave(saveDelay, bind(&Replicator::saveCheckpoint, this, _1));

        registerHandler("getCheckpoint",    &Replicator::handleGetCheckpoint);
        registerHandler("setCheckpoint",    &Replicator::handleSetCheckpoint);
    }


    void Replicator::start(bool reset, bool synchronous) {
        if (synchronous)
            _start(reset);
        else
            enqueue(FUNCTION_TO_QUEUE(Replicator::_start), reset);
    }


    void Replicator::_start(bool reset) {
        Assert(_connectionState == Connection::kClosed);
        Signpost::begin(Signpost::replication, uintptr_t(this));
        _connectionState = Connection::kConnecting;
        connection().start();
        // Now wait for _onConnect or _onClose...
        
        _findExistingConflicts();

        if (_options.push > kC4Passive || _options.pull > kC4Passive) {
            // Get the remote DB ID:
            slice key = _checkpointer.remoteDBIDString();
            C4Error err;
            C4RemoteID remoteDBID = _db->lookUpRemoteDBID(key, &err);
            if (remoteDBID) {
                logVerbose("Remote-DB ID %u found for target <%.*s>", remoteDBID, SPLAT(key));
            } else {
                warn("Couldn't get remote-DB ID for target <%.*s>: error %d/%d",
                     SPLAT(key), err.domain, err.code);
                gotError(err);
                stop();
            }

            // Get the checkpoints:
            if (getLocalCheckpoint(reset)) {
                getRemoteCheckpoint(false);
            }
        }
    }


    void Replicator::_findExistingConflicts() {
        if (_options.pull <= kC4Passive) // only check in pull mode
            return;

        Stopwatch st;
        C4Error err;
        c4::ref<C4DocEnumerator> e = _db->unresolvedDocsEnumerator(false, &err);
        if (e) {
            logInfo("Scanning for pre-existing conflicts...");
            unsigned nConflicts = 0;
            while (c4enum_next(e, &err)) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                auto rev = retained(new RevToInsert(nullptr,        /* incoming rev */
                                                    info.docID,
                                                    info.revID,
                                                    nullslice,      /* history buf */
                                                    info.flags & kDocDeleted,
                                                    false));
                rev->error = c4error_make(LiteCoreDomain, kC4ErrorConflict, {});
                _docsEnded.push(rev);
                ++nConflicts;
            }
            logInfo("Found %u conflicted docs in %.3f sec", nConflicts, st.elapsed());
        } else {
            warn("Couldn't get unresolved docs enumerator: error %d/%d", err.domain, err.code);
            gotError(err);
        }
    }


    void Replicator::_stop() {
        logInfo("Told to stop!");
        _disconnect(websocket::kCodeNormal, {});
    }


    void Replicator::terminate() {
        logDebug("terminate() called...");
        if (connected()) {
            logDebug("...connected() was true, doing extra stuff...");
            Assert(_connectionState == Connection::kClosed);
            connection().terminate();
            _pusher = nullptr;
            _puller = nullptr;
        }

        // CBL-1061: This used to be inside the connected(), but static analysis shows
        // terminate() is only called from the destructor of the _delegate itself, so it is
        // dangerous to leave it around.  Set it to null here to avoid using it further.
        _delegate = nullptr;
        _db.reset();
        logDebug("...done with terminate()");
    }


    void Replicator::_disconnect(websocket::CloseCode closeCode, slice message) {
        if (connected()) {
            connection().close(closeCode, message);
            _connectionState = Connection::kClosing;
        }
    }


    // Called after the checkpoint is established.
    void Replicator::startReplicating() {
        if (_options.push > kC4Passive)
            _pusher->start();
        if (_options.pull > kC4Passive)
            _puller->start(_checkpointer.remoteMinSequence());
    }


    void Replicator::docRemoteAncestorChanged(alloc_slice docID, alloc_slice revID) {
        Retained<Pusher> pusher = _pusher;
        if (pusher)
            pusher->docRemoteAncestorChanged(docID, revID);
    }


    void Replicator::returnForbidden(Retained<blip::MessageIn> request) {
        if (_options.push != kC4Disabled) {
            request->respondWithError(Error("HTTP"_sl, 403, "Attempting to push to a pull-only replicator"_sl));
        } else {
            request->respondWithError(Error("HTTP"_sl, 403, "Attempting to pull from a push-only replicator"_sl));
        }
    }


#pragma mark - STATUS:


    // The status of one of the actors has changed; update mine
    void Replicator::_childChangedStatus(Worker *task, Status taskStatus) {
        if (status().level == kC4Stopped)       // I've already stopped & cleared refs; ignore this
            return;

        if (task == _pusher) {
            _pushStatus = taskStatus;
        } else if (task == _puller) {
            _pullStatus = taskStatus;
        }

        setProgress(_pushStatus.progress + _pullStatus.progress);

        if (SyncBusyLog.willLog(LogLevel::Info)) {
            logInfo("pushStatus=%-s, pullStatus=%-s, progress=%" PRIu64 "/%" PRIu64 "",
                    kC4ReplicatorActivityLevelNames[_pushStatus.level],
                    kC4ReplicatorActivityLevelNames[_pullStatus.level],
                    status().progress.unitsCompleted, status().progress.unitsTotal);
        }

        if (_pullStatus.error.code)
            onError(_pullStatus.error);
        else if (_pushStatus.error.code)
            onError(_pushStatus.error);

        // Save a checkpoint immediately when push or pull finishes or goes idle:
        if ((taskStatus.level == kC4Stopped || taskStatus.level == kC4Idle)
                && (task == _pusher || task == _puller))
            _checkpointer.save();
    }


    Worker::ActivityLevel Replicator::computeActivityLevel() const {
        // Once I've announced I've stopped, don't return any other status again:
        auto currentLevel = status().level;
        if (currentLevel == kC4Stopped)
            return kC4Stopped;

        ActivityLevel level = kC4Busy;
        switch (_connectionState) {
            case Connection::kConnecting:
                level = kC4Connecting;
                break;
            case Connection::kConnected: {
                if (_checkpointer.isUnsaved())
                    level = kC4Busy;
                else
                    level = Worker::computeActivityLevel();
                level = max(level, max(_pushStatus.level, _pullStatus.level));
                if (level == kC4Idle && !isContinuous() && !isOpenServer()) {
                    // Detect that a non-continuous active push or pull replication is done:
                    logInfo("Replication complete! Closing connection");
                    const_cast<Replicator*>(this)->_stop();
                    level = kC4Busy;
                }
                DebugAssert(level > kC4Stopped);
                break;
            }
            case Connection::kClosing:
                // Remain active while I wait for the connection to finish closing:
                logDebug("Connection closing... (activityLevel=busy)waiting to finish");
                level = kC4Busy;
                break;
            case Connection::kDisconnected:
            case Connection::kClosed:
                // After connection closes, remain Busy (or Connecting) while I wait for db to
                // finish writes and for myself to process any pending messages; then go to Stopped.
                level = Worker::computeActivityLevel();
                level = max(level, max(_pushStatus.level, _pullStatus.level));
                if (level < kC4Busy)
                    level = kC4Stopped;
                else if (currentLevel == kC4Connecting)
                    level = kC4Connecting;
                break;
        }
        if (SyncBusyLog.willLog(LogLevel::Info)) {
            logInfo("activityLevel=%-s: connectionState=%d, savingChkpt=%d",
                    kC4ReplicatorActivityLevelNames[level], _connectionState, _checkpointer.isUnsaved());
        }
        return level;
    }


    void Replicator::onError(C4Error error) {
        if(status().error.code != 0 && error.domain == WebSocketDomain &&
           (error.code == kWebSocketCloseAppPermanent || error.code == kWebSocketCloseAppTransient)) {
            // CBL-1178: If we already have an error code, it is more relevant than the web socket close code, so keep it
            // intact so that the consumer can know what went wrong
            logVerbose("kWebSocketCloseAppPermanent or kWebSocketCloseAppTransient received, ignoring (only relevant for underlying connection...)");
            return;
        }
        
        Worker::onError(error);
        for (const StoppingErrorEntry& stoppingErr : StoppingErrors) {
            if (stoppingErr.err == error) {
                alloc_slice message( c4error_getDescription(error) );
                if (stoppingErr.isFatal) {
                    logError("Stopping due to fatal error: %.*s", SPLAT(message));
                    _disconnect(websocket::kCloseAppPermanent, stoppingErr.msg);
                } else {
                    logError("Stopping due to error: %.*s", SPLAT(message));
                    _disconnect(websocket::kCloseAppTransient, stoppingErr.msg);
                }
                return;
            }
        }
    }


    void Replicator::changedStatus() {
        if (status().level == kC4Stopped) {
            DebugAssert(!connected());  // must already have gotten _onClose() delegate callback
            _pusher = nullptr;
            _puller = nullptr;
            Signpost::end(Signpost::replication, uintptr_t(this));
        }
        if (_delegate) {
            // Notify the delegate of the current status, but not too often:
            auto waitFor = tuning::kMinDelegateCallInterval - _sinceDelegateCall.elapsedDuration();
            if (waitFor <= 0s || status().level != _lastDelegateCallLevel) {
                reportStatus();
            } else if (!_waitingToCallDelegate) {
                _waitingToCallDelegate = true;
                enqueueAfter(waitFor, FUNCTION_TO_QUEUE(Replicator::reportStatus));
            }
        }
    }


    void Replicator::reportStatus() {
        _waitingToCallDelegate = false;
        _lastDelegateCallLevel = status().level;
        _sinceDelegateCall.reset();
        if (_delegate) {
            notifyEndedDocuments();
            _delegate->replicatorStatusChanged(this, status());
        }
        if (status().level == kC4Stopped)
            _delegate = nullptr;        // Never call delegate after telling it I've stopped
    }


    void Replicator::endedDocument(ReplicatedRev *d) {
        logInfo("documentEnded %.*s %.*s flags=%02x (%d/%d)", SPLAT(d->docID), SPLAT(d->revID),
                 d->flags, d->error.domain, d->error.code);
        d->trim(); // free up unneeded stuff
        if (_delegate) {
            if (d->isWarning && (d->flags & kRevIsConflict)) {
                // Inserter::insertRevisionNow set this flag to indicate that the rev caused a
                // conflict (though it did get inserted), so notify the delegate of the conflict:
                d->error = c4error_make(LiteCoreDomain, kC4ErrorConflict, nullslice);
                d->errorIsTransient = true;
            }
            _docsEnded.push(d);
        }
    }


    void Replicator::notifyEndedDocuments(int gen) {
        auto docs = _docsEnded.pop(gen);
        if (docs && !docs->empty() && _delegate)
            _delegate->replicatorDocumentsEnded(this, *docs);
    }


    void Replicator::_onBlobProgress(BlobProgress p) {
        if (_delegate)
            _delegate->replicatorBlobProgress(this, p);
    }


#pragma mark - BLIP DELEGATE:


    void Replicator::onTLSCertificate(slice certData) {
        if (_delegate)
            _delegate->replicatorGotTLSCertificate(certData);
    }


    void Replicator::onHTTPResponse(int status, const websocket::Headers &headers) {
        enqueue(FUNCTION_TO_QUEUE(Replicator::_onHTTPResponse), status, headers);
    }


    void Replicator::_onHTTPResponse(int status, websocket::Headers headers) {
        if (status == 101 && !headers["Sec-WebSocket-Protocol"_sl]) {
            gotError(c4error_make(WebSocketDomain, kWebSocketCloseProtocolError,
                                  "Incompatible replication protocol "
                                  "(missing 'Sec-WebSocket-Protocol' response header)"_sl));
        }
        if (_delegate)
            _delegate->replicatorGotHTTPResponse(this, status, headers);
    }


    void Replicator::_onConnect() {
        logInfo("Connected!");
        Signpost::mark(Signpost::replicatorConnect, uintptr_t(this));
        if (_connectionState != Connection::kClosing) {     // skip this if stop() already called
            _connectionState = Connection::kConnected;
            if (_options.push > kC4Passive || _options.pull > kC4Passive)
                getRemoteCheckpoint(false);
        }
    }


    void Replicator::_onClose(Connection::CloseStatus status, Connection::State state) {
        logInfo("Connection closed with %-s %d: \"%.*s\" (state=%d)",
            status.reasonName(), status.code, SPLAT(status.message), _connectionState);
        Signpost::mark(Signpost::replicatorDisconnect, uintptr_t(this));

        bool closedByPeer = (_connectionState != Connection::kClosing);
        _connectionState = state;

        _checkpointer.stopAutosave();
        
        // Clear connection() and notify the other agents to do the same:
        _connectionClosed();
        if (_pusher)
            _pusher->connectionClosed();
        if (_puller)
            _puller->connectionClosed();

        if (status.isNormal() && closedByPeer && (_options.push > kC4Passive || _options.pull > kC4Passive)) {
            logInfo("I didn't initiate the close; treating this as code 1001 (GoingAway)");
            status.code = websocket::kCodeGoingAway;
            status.message = alloc_slice("WebSocket connection closed by peer");
        }

        static const C4ErrorDomain kDomainForReason[] = {WebSocketDomain, POSIXDomain,
                                                         NetworkDomain, LiteCoreDomain};

        // If this was an unclean close, set my error property:
        if (status.reason != websocket::kWebSocketClose || status.code != websocket::kCodeNormal) {
            int code = status.code;
            C4ErrorDomain domain;
            if (status.reason < sizeof(kDomainForReason)/sizeof(C4ErrorDomain)) {
                domain = kDomainForReason[status.reason];
            } else {
                domain = LiteCoreDomain;
                code = kC4ErrorRemoteError;
            }
            gotError(c4error_make(domain, code, status.message));
        }

        if (_delegate) {
            notifyEndedDocuments();
            _delegate->replicatorConnectionClosed(this, status);
        }
    }


    // This only gets called if none of the registered handlers were triggered.
    void Replicator::_onRequestReceived(Retained<MessageIn> msg) {
        warn("Received unrecognized BLIP request #%" PRIu64 " with Profile '%.*s', %zu bytes",
                msg->number(), SPLAT(msg->property("Profile"_sl)), msg->body().size);
        msg->notHandled();
    }


#pragma mark - CHECKPOINT:


    // Start off by getting the local checkpoint, if this is an active replicator:
    bool Replicator::getLocalCheckpoint(bool reset) {
        return _db->use<bool>([&](C4Database *db) {
            C4Error error;
            if (_checkpointer.read(db, reset, &error)) {
                auto remote = _checkpointer.remoteMinSequence();
                logInfo("Read local checkpoint '%.*s': %.*s",
                        SPLAT(_checkpointer.initialCheckpointID()),
                        SPLAT(_checkpointer.checkpointJSON()));
                _hadLocalCheckpoint = true;
            } else if (error.code != 0) {
                logInfo("Fatal error getting local checkpoint");
                gotError(error);
                stop();
                return false;
            } else if (reset) {
                logInfo("Ignoring local checkpoint ('reset' option is set)");
            } else {
                logInfo("No local checkpoint '%.*s'", SPLAT(_checkpointer.initialCheckpointID()));
                // If pulling into an empty db with no checkpoint, it's safe to skip deleted
                // revisions as an optimization.
                if (_options.pull > kC4Passive && _puller && c4db_getLastSequence(db) == 0)
                    _puller->setSkipDeleted();
            }
            return true;
        });
    }


    // Get the remote checkpoint, after we've got the local one and the BLIP connection is up.
    void Replicator::getRemoteCheckpoint(bool refresh) {
        if (_remoteCheckpointRequested)
            return;     // already in progress
        if (!_remoteCheckpointDocID)
            _remoteCheckpointDocID = _checkpointer.initialCheckpointID();
        if (!_remoteCheckpointDocID || _connectionState != Connection::kConnected)
            return;     // not ready yet

        logVerbose("Requesting remote checkpoint '%.*s'", SPLAT(_remoteCheckpointDocID));
        MessageBuilder msg("getCheckpoint"_sl);
        msg["client"_sl] = _remoteCheckpointDocID;
        Signpost::begin(Signpost::blipSent);
        sendRequest(msg, [this, refresh](MessageProgress progress) {
            // ...after the checkpoint is received:
            if (progress.state != MessageProgress::kComplete)
                return;
            Signpost::end(Signpost::blipSent);
            MessageIn *response = progress.reply;
            Checkpoint remoteCheckpoint;

            if (response->isError()) {
                auto err = response->getError();
                if (!(err.domain == "HTTP"_sl && err.code == 404))
                    return gotError(response);
                logInfo("No remote checkpoint '%.*s'", SPLAT(_remoteCheckpointDocID));
                _remoteCheckpointRevID.reset();
            } else {
                remoteCheckpoint.readJSON(response->body());
                _remoteCheckpointRevID = response->property("rev"_sl);
                logInfo("Received remote checkpoint (rev='%.*s'): %.*s",
                        SPLAT(_remoteCheckpointRevID), SPLAT(response->body()));
            }
            _remoteCheckpointReceived = true;

            if (!refresh && _hadLocalCheckpoint) {
                // Compare checkpoints, reset if mismatched:
                bool valid = _checkpointer.validateWith(remoteCheckpoint);
                if (!valid && _pusher)
                    _pusher->checkpointIsInvalid();

                // Now we have the checkpoints! Time to start replicating:
                startReplicating();
            }

            if (_checkpointJSONToSave)
                saveCheckpointNow();    // _saveCheckpoint() was waiting for _remoteCheckpointRevID
        });

        _remoteCheckpointRequested = true;

        // If there's no local checkpoint, we know we're starting from zero and don't need to
        // wait for the remote one before getting started:
        if (!refresh && !_hadLocalCheckpoint)
            startReplicating();
    }


    void Replicator::_saveCheckpoint(alloc_slice json) {
        if (!connected())
            return;
        _checkpointJSONToSave = move(json);
        if (_remoteCheckpointReceived)
            saveCheckpointNow();
        // ...else wait until checkpoint received (see above), which will call saveCheckpointNow().
    }


    void Replicator::saveCheckpointNow() {
        // Switch to the permanent checkpoint ID:
        alloc_slice checkpointID = _checkpointer.checkpointID();
        if (checkpointID != _remoteCheckpointDocID) {
            _remoteCheckpointDocID = checkpointID;
            _remoteCheckpointRevID = nullslice;
        }

        alloc_slice json = move(_checkpointJSONToSave);

        logVerbose("Saving remote checkpoint '%.*s' over rev='%.*s': %.*s ...",
                   SPLAT(_remoteCheckpointDocID), SPLAT(_remoteCheckpointRevID), SPLAT(json));
        Assert(_remoteCheckpointReceived);
        Assert(json);

        MessageBuilder msg("setCheckpoint"_sl);
        msg["client"_sl] = _remoteCheckpointDocID;
        msg["rev"_sl] = _remoteCheckpointRevID;
        msg << json;
        Signpost::begin(Signpost::blipSent);
        sendRequest(msg, [=](MessageProgress progress) {
            if (progress.state != MessageProgress::kComplete)
                return;
            Signpost::end(Signpost::blipSent);
            MessageIn *response = progress.reply;
            if (response->isError()) {
                Error responseErr = response->getError();
                if (responseErr.domain == "HTTP"_sl && responseErr.code == 409) {
                    // On conflict, read the remote checkpoint to get the real revID:
                    _checkpointJSONToSave = json; // move() has no effect here
                    _remoteCheckpointRequested = _remoteCheckpointReceived = false;
                    getRemoteCheckpoint(true);
                } else {
                    gotError(response);
                    warn("Failed to save remote checkpoint!");
                    // If the checkpoint didn't save, something's wrong; but if we don't mark it as
                    // saved, the replicator will stay busy (see computeActivityLevel, line 169).
                    _checkpointer.saveCompleted();
                }
            } else {
                // Remote checkpoint saved, so update local one:
                _remoteCheckpointRevID = response->property("rev"_sl);
                logInfo("Saved remote checkpoint '%.*s' as rev='%.*s'",
                    SPLAT(_remoteCheckpointDocID), SPLAT(_remoteCheckpointRevID));

                C4Error err;
                bool ok = _db->use<bool>([&](C4Database *db) {
                    _db->markRevsSyncedNow();
                    return _checkpointer.write(db, json, &err);
                });
                if (ok)
                    logInfo("Saved local checkpoint '%.*s': %.*s",
                            SPLAT(_remoteCheckpointDocID), SPLAT(json));
                else
                    gotError(err);
                _checkpointer.saveCompleted();
            }
        });
    }


    bool Replicator::pendingDocumentIDs(Checkpointer::PendingDocCallback callback, C4Error* outErr){
        return _db->use<bool>([&](C4Database *db) {
            return _checkpointer.pendingDocumentIDs(db, callback, outErr);
        });
    }


    bool Replicator::isDocumentPending(slice docID, C4Error* outErr) {
        return _db->use<bool>([&](C4Database *db) {
            return _checkpointer.isDocumentPending(db, docID, outErr);
        });
    }


#pragma mark - PEER CHECKPOINT ACCESS:


    // Gets the ID from a checkpoint request
    slice Replicator::getPeerCheckpointDocID(MessageIn* request, const char *whatFor) const {
        slice checkpointID = request->property("client"_sl);
        if (checkpointID)
            logInfo("Request to %s peer checkpoint '%.*s'", whatFor, SPLAT(checkpointID));
        else
            request->respondWithError({"BLIP"_sl, 400, "missing checkpoint ID"_sl});
        return checkpointID;
    }


    // Handles a "getCheckpoint" request by looking up a peer checkpoint.
    void Replicator::handleGetCheckpoint(Retained<MessageIn> request) {
        slice checkpointID = getPeerCheckpointDocID(request, "get");
        if (!checkpointID)
            return;
        
        alloc_slice body, revID;
        C4Error err;
        bool ok = _db->use<bool>([&](C4Database *db) {
            return Checkpointer::getPeerCheckpoint(db, checkpointID, body, revID, &err);
        });
        if (!ok) {
            const int status = isNotFoundError(err) ? 404 : 502;
            request->respondWithError({"HTTP"_sl, status});
            return;
        }

        MessageBuilder response(request);
        response["rev"_sl] = revID;
        response << body;
        request->respond(response);
    }


    // Handles a "setCheckpoint" request by storing a peer checkpoint.
    void Replicator::handleSetCheckpoint(Retained<MessageIn> request) {
        slice checkpointID = getPeerCheckpointDocID(request, "set");
        if (!checkpointID)
            return;

        alloc_slice newRevID;
        C4Error err;
        bool ok = _db->use<bool>([&](C4Database *db) {
            return Checkpointer::savePeerCheckpoint(db, checkpointID,
                                                    request->body(),
                                                    request->property("rev"_sl),
                                                    newRevID, &err);
        });
        if (!ok) {
            if (err.domain == LiteCoreDomain && err.code == kC4ErrorConflict)
                request->respondWithError({"HTTP"_sl, 409, alloc_slice("revision ID mismatch"_sl)});
            else
                request->respondWithError(c4ToBLIPError(err));
            return;
        }

        MessageBuilder response(request);
        response["rev"_sl] = newRevID;
        request->respond(response);
    }

} }
