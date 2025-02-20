//
// RevFinder.cc
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

#include "RevFinder.hh"
#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "IncomingRev.hh"
#include "DBAccess.hh"
#include "Increment.hh"
#include "VersionVector.hh"
#include "StringUtil.hh"
#include "Instrumentation.hh"
#include "c4.hh"
#include "c4Transaction.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "BLIP.hh"
#include "fleece/Fleece.hh"

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore::repl {

    RevFinder::RevFinder(Replicator *replicator, Delegate *delegate)
    :Worker(replicator, "RevFinder")
    ,_delegate(delegate)
    {
        _passive = _options.pull <= kC4Passive;
        _mustBeProposed = _passive && _options.noIncomingConflicts()
                                   && !_db->usingVersionVectors();
        registerHandler("changes",          &RevFinder::handleChanges);
        registerHandler("proposeChanges",   &RevFinder::handleChanges);
    }


    // Receiving an incoming "changes" (or "proposeChanges") message
    void RevFinder::handleChanges(Retained<MessageIn> req) {
        if (pullerHasCapacity()) {
            handleChangesNow(req);
        } else {
            logVerbose("Queued '%.*s' REQ#%" PRIu64 " (now %zu)",
                       SPLAT(req->property("Profile"_sl)), req->number(),
                       _waitingChangesMessages.size() + 1);
            Signpost::begin(Signpost::handlingChanges, (uintptr_t)req->number());
            _waitingChangesMessages.push_back(move(req));
        }
    }


    void RevFinder::_reRequestingRev() {
        increment(_numRevsBeingRequested);
    }


    void RevFinder::_revReceived() {
        decrement(_numRevsBeingRequested);

        // Process waiting "changes" messages if not throttled:
        while (!_waitingChangesMessages.empty() && pullerHasCapacity()) {
            auto req = _waitingChangesMessages.front();
            _waitingChangesMessages.pop_front();
            handleChangesNow(req);
        }
    }


    // Actually handle a "changes" (or "proposeChanges") message:
    void RevFinder::handleChangesNow(MessageIn *req) {
        slice reqType = req->property("Profile"_sl);
        bool proposed = (reqType == "proposeChanges"_sl);
        logVerbose("Handling '%.*s' REQ#%" PRIu64, SPLAT(reqType), req->number());

        auto changes = req->JSONBody().asArray();
        auto nChanges = changes.count();
        if (!changes && req->body() != "null"_sl) {
            warn("Invalid body of 'changes' message");
            req->respondWithError({"BLIP"_sl, 400, "Invalid JSON body"_sl});
        } else if ((!proposed && _mustBeProposed) || (proposed && _db->usingVersionVectors())) {
            // In conflict-free mode plus rev-trees the protocol requires the pusher send
            // "proposeChanges" instead. But with version vectors, always use "changes".
            req->respondWithError({"BLIP"_sl, 409});
        } else if (nChanges == 0) {
            // Empty array indicates we've caught up. (This may have been sent no-reply)
            logInfo("Caught up with remote changes");
            _delegate->caughtUp();
            req->respond();
        } else if (req->noReply()) {
            warn("Got pointless noreply 'changes' message");
        } else {
            // Alright, let's look at the changes:
            if (proposed) {
                logInfo("Received %u changes", nChanges);
            } else if (willLog()) {
                alloc_slice firstSeq(changes[0].asArray()[0].toString());
                alloc_slice lastSeq (changes[nChanges-1].asArray()[0].toString());
                logInfo("Received %u changes (seq '%.*s'..'%.*s')",
                        nChanges, SPLAT(firstSeq), SPLAT(lastSeq));
            }

            if (!proposed)
                _db->markRevsSyncedNow();   // make sure foreign ancestors are up to date

            MessageBuilder response(req);
            response.compressed = true;
            _db->use([&](C4Database *db) {
                response["maxHistory"_sl] = c4db_getMaxRevTreeDepth(db);
            });
            if (!_db->disableBlobSupport())
                response["blobs"_sl] = "true"_sl;
            if ( !_announcedDeltaSupport && !_options.disableDeltaSupport()) {
                response["deltas"_sl] = "true"_sl;
                _announcedDeltaSupport = true;
            }

            Stopwatch st;

            vector<ChangeSequence> sequences; // the vector I will send to the delegate
            sequences.reserve(nChanges);

            C4Error error;
            auto &encoder = response.jsonBody();
            encoder.beginArray();
            int requested = proposed ? findProposedRevs(changes, encoder, sequences, &error)
                                     : findRevs(changes, encoder, sequences, &error);
            encoder.endArray();

            if (requested < 0) {
                gotError(error);
                req->respondWithError(c4ToBLIPError(error));
                return;
            }
            
            // CBL-1399: Important that the order be call expectSequences and *then* respond
            // to avoid rev messages comes in before the Puller knows about them (mostly 
            // applies to local to local replication where things can come back over the wire
            // very quickly)
            _numRevsBeingRequested += requested;
            _delegate->expectSequences(move(sequences));
            req->respond(response);

            logInfo("Responded to '%.*s' REQ#%" PRIu64 " w/request for %u revs in %.6f sec",
                    SPLAT(req->property("Profile"_sl)), req->number(), requested, st.elapsed());

        }

        Signpost::end(Signpost::handlingChanges, (uintptr_t)req->number());
    }


    bool RevFinder::checkDocAndRevID(slice docID, slice revID, C4Error *outError) {
        bool valid;
        if (docID.size < 1 || docID.size > 255)
            valid = false;
        else if (_db->usingVersionVectors())
            valid = revID.findByte('@') && !revID.findByte('*');     // require absolute form
        else
            valid = revID.findByte('-');
        if (!valid) {
            *outError = c4error_printf(LiteCoreDomain, kC4ErrorRemoteError,
                                       "Invalid docID/revID '%.*s' #%.*s in incoming change list",
                                       SPLAT(docID), SPLAT(revID));
        }
        return valid;
    }


    // Looks through the contents of a "changes" message, encodes the response,
    // adds each entry to `sequences`, and returns the number of new revs.
    int RevFinder::findRevs(Array changes,
                            Encoder &encoder,
                            vector<ChangeSequence> &sequences,
                            C4Error *outError)
    {
        // Compile the docIDs/revIDs into parallel vectors:
        unsigned itemsWritten = 0, requested = 0;
        vector<slice> docIDs, revIDs;
        auto nChanges = changes.count();
        docIDs.reserve(nChanges);
        revIDs.reserve(nChanges);
        for (auto item : changes) {
            // "changes" entry: [sequence, docID, revID, deleted?, bodySize?]
            auto change = item.asArray();
            slice docID = change[1].asString();
            slice revID = change[2].asString();
            if (!checkDocAndRevID(docID, revID, outError))
                return -1;
            docIDs.push_back(docID);
            revIDs.push_back(revID);
            sequences.push_back({RemoteSequence(change[0]),
                                 max(change[4].asUnsigned(), (uint64_t)1)});
        }

        // Ask the database to look up the ancestors:
        vector<C4StringResult> ancestors(nChanges);
        bool ok = _db->use<bool>([&](C4Database *db) {
            return c4db_findDocAncestors(db, nChanges, kMaxPossibleAncestors,
                                         !_options.disableDeltaSupport(),  // requireBodies
                                         _db->remoteDBID(),
                                         (C4String*)docIDs.data(), (C4String*)revIDs.data(),
                                         ancestors.data(), outError);
        });
        if (!ok) {
            return -1;
        } else {
            // Look through the database response:
            for (unsigned i = 0; i < nChanges; ++i) {
                slice docID = docIDs[i], revID = revIDs[i];
                alloc_slice anc(std::move(ancestors[i]));
                C4FindDocAncestorsResultFlags status = anc ? (anc[0] - '0') : kRevsLocalIsOlder;

                if (status & kRevsLocalIsOlder) {
                    // I have an older revision or a conflict:
                    // First, append zeros for any items I skipped:
                    // [use only writeRaw to avoid confusing JSONEncoder's comma mechanism, CBL-1208]
                    if (itemsWritten > 0)
                        encoder.writeRaw(",");      // comma after previous array item
                    while (itemsWritten++ < i)
                        encoder.writeRaw("0,");

                    if ((status & kRevsConflict) == kRevsConflict && passive()) {
                        // Passive puller refuses conflicts:
                        encoder.writeRaw("409");
                        sequences[i].bodySize = 0;
                        logDebug("    - '%.*s' #%.*s conflicts with local revision, rejecting",
                                 SPLAT(docID), SPLAT(revID));
                    } else {
                        // OK, I want it!
                        // Append array of ancestor revs I do have (it's already a JSON array):
                        ++requested;
                        slice jsonArray = (anc ? anc.from(1) : "[]"_sl);
                        encoder.writeRaw(jsonArray);
                        logDebug("    - Requesting '%.*s' #%.*s, I have ancestors %.*s",
                                 SPLAT(docID), SPLAT(revID), SPLAT(jsonArray));
                    }
                } else {
                    // I have an equal or newer revision; ignore this one:
                    // [Implicitly this appends a 0, but we're skipping trailing zeroes.]
                    sequences[i].bodySize = 0;
                    if (status & kRevsAtThisRemote) {
                        logDebug("    - Already have '%.*s' %.*s",
                                 SPLAT(docID), SPLAT(revID));
                    } else {
                        // This means the rev exists but is not marked as the latest from the
                        // remote server, so I better make it so:
                        logDebug("    - Already have '%.*s' %.*s but need to mark it as remote ancestor",
                                 SPLAT(docID), SPLAT(revID));
                        _db->setDocRemoteAncestor(docID, revID);
                        if (!_passive && !_db->usingVersionVectors())
                            replicator()->docRemoteAncestorChanged(alloc_slice(docID),
                                                                   alloc_slice(revID));
                    }
                }
            }
        }
        return requested;
    }


    // Same as `findOrRequestRevs`, but for "proposeChanges" messages.
    int RevFinder::findProposedRevs(Array changes,
                                    Encoder &encoder,
                                    vector<ChangeSequence> &sequences,
                                    C4Error *outError)
    {
        unsigned itemsWritten = 0, requested = 0;
        int i = -1;
        for (auto item : changes) {
            ++i;
            // Look up each revision in the `req` list:
            // "proposeChanges" entry: [docID, revID, parentRevID?, bodySize?]
            auto change = item.asArray();
            alloc_slice docID( change[0].asString() );
            slice revID = change[1].asString();
            if (!checkDocAndRevID(docID, revID, outError))
                return -1;

            slice parentRevID = change[2].asString();
            if (parentRevID.size == 0)
                parentRevID = nullslice;
            alloc_slice currentRevID;
            int status = findProposedChange(docID, revID, parentRevID, currentRevID);
            if (status == 0) {
                // Accept rev by (lazily) appending a 0:
                logDebug("    - Accepting proposed change '%.*s' #%.*s with parent %.*s",
                         SPLAT(docID), SPLAT(revID), SPLAT(parentRevID));
                ++requested;
                sequences.push_back({RemoteSequence(), max(change[3].asUnsigned(), (uint64_t)1)});
                // sequences[i].sequence remains null: proposeChanges entries have no sequence ID
            } else {
                // Reject rev by appending status code:
                logInfo("Rejecting proposed change '%.*s' #%.*s with parent %.*s (status %d; current rev is %.*s)",
                        SPLAT(docID), SPLAT(revID), SPLAT(parentRevID), status, SPLAT(currentRevID));
                while (itemsWritten++ < i)
                    encoder.writeInt(0);
                encoder.writeInt(status);
            }
        }
        return requested;
    }


    // Checks whether the revID (if any) is really current for the given doc.
    // Returns an HTTP-ish status code: 0=OK, 409=conflict, 500=internal error
    int RevFinder::findProposedChange(slice docID, slice revID, slice parentRevID,
                                     alloc_slice &outCurrentRevID)
    {
        C4DocumentFlags flags = 0;
        {
            // Get the local doc's current revID/vector and flags:
            outCurrentRevID = nullslice;
            C4Error err;
            if (c4::ref<C4Document> doc = _db->getDoc(docID, kDocGetMetadata, &err); doc) {
                flags = doc->flags;
                outCurrentRevID = c4doc_getSelectedRevIDGlobalForm(doc);
            } else if (!isNotFoundError(err)) {
                gotError(err);
                return 500;
            }
        }

        if (outCurrentRevID == revID) {
            // I already have this revision:
            return 304;
        } else if (_db->usingVersionVectors()) {
            // Version vectors:  (note that parentRevID is ignored; we don't need it)
            try {
                auto theirVers = VersionVector::fromASCII(revID);
                auto myVers = VersionVector::fromASCII(outCurrentRevID);
                switch (theirVers.compareTo(myVers)) {
                    case kSame:
                    case kOlder:        return 304;
                    case kNewer:        return 0;
                    case kConflicting:  return 409;
                }
                abort(); // unreachable
            } catch (const error &x) {
                if (x == error::BadRevisionID)
                    return 500;
                else
                    throw;
            }
        } else {
            // Rev-trees:
            if (outCurrentRevID == parentRevID)
                return 0;   // I don't have this revision and it's not a conflict, so I want it!
            else if (!parentRevID && (flags & kDocDeleted))
                return 0;   // Peer is creating a new doc; my doc is deleted, so that's OK
            else
                return 409; // Peer's revID isn't current, so this is a conflict
        }
    }


}
