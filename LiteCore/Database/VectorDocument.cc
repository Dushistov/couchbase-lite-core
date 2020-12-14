//
// VectorDocument.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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

#include "VectorDocument.hh"
#include "NuDocument.hh"
#include "VersionVector.hh"
#include "c4Private.h"
#include "StringUtil.hh"
#include <set>

namespace c4Internal {
    using namespace fleece;


    class VectorDocument : public Document {
    public:
        VectorDocument(Database* database, C4Slice docID, ContentOption whichContent)
        :Document(database, docID)
        ,_doc(database->defaultKeyStore(), docID, whichContent)
        {
            _initialize();
        }


        VectorDocument(Database *database, const Record &doc)
        :Document(database, doc.key())
        ,_doc(database->defaultKeyStore(), doc)
        {
            _initialize();
        }


        ~VectorDocument() {
            _doc.owner = nullptr;
        }


        void _initialize() {
            _doc.owner = this;
            _updateDocFields();
            _selectRemote(RemoteID::Local);
        }


        void _updateDocFields() {
            _revIDBuf = _expandRevID(_doc.revID());
            revID = _revIDBuf;

            flags = C4DocumentFlags(_doc.flags());
            if (_doc.exists())
                flags |= kDocExists;
            sequence = _doc.sequence();
        }


        peerID myPeerID() {
            return peerID{database()->myPeerID()};
        }


        alloc_slice _expandRevID(revid rev, peerID myID =kMePeerID) {
            if (!rev)
                return nullslice;
            return rev.asVersion().asASCII(myID);
        }


        revidBuffer _parseRevID(slice revID) {
            if (revID) {
                if (revidBuffer binaryID(revID); binaryID.isVersion()) {
                    // If it's a version in global form, convert it to local form:
                    if (auto vers = binaryID.asVersion(); vers.author() == myPeerID())
                    binaryID = Version(revID, myPeerID());
                    return binaryID;
                }
            }
            error::_throw(error::BadRevisionID, "Not a version string: '%.*s'", SPLAT(revID));
        }


#pragma mark - SELECTING REVISIONS:


        optional<pair<RemoteID, Revision>> _findRemote(slice revID) {
            RemoteID remote = RemoteID::Local;
            if (revID.findByte(',')) {
                // It's a version vector; look for an exact match:
                VersionVector vers = VersionVector::fromASCII(revID, myPeerID());
                alloc_slice binary = vers.asBinary();
                while (auto rev = _doc.remoteRevision(remote)) {
                    if (rev->revID == binary)
                        return {{remote, *rev}};
                    remote = _doc.nextRemoteID(remote);
                }
            } else {
                // It's a single version, so find a vector that starts with it:
                Version vers = _parseRevID(revID).asVersion();
                while (auto rev = _doc.remoteRevision(remote)) {
                    if (rev->revID && rev->version() == vers)
                        return {{remote, *rev}};
                    remote = _doc.nextRemoteID(remote);
                }
            }
            return nullopt;
        }


        bool _selectRemote(RemoteID remote) {
            if (auto rev = _doc.remoteRevision(remote); rev && rev->revID) {
                return _selectRemote(remote, *rev);
            } else {
                _remoteID = nullopt;
                clearSelectedRevision();
                return false;
            }
        }


        bool _selectRemote(RemoteID remote, Revision &rev) {
            _remoteID = remote;
            _selectedRevIDBuf = _expandRevID(rev.revID);
            selectedRev.revID = _selectedRevIDBuf;
            selectedRev.sequence = _doc.sequence(); // NuDocument doesn't have per-rev sequence

            selectedRev.flags = 0;
            if (remote == RemoteID::Local)  selectedRev.flags |= kRevLeaf;
            if (rev.isDeleted())            selectedRev.flags |= kRevDeleted;
            if (rev.hasAttachments())       selectedRev.flags |= kRevHasAttachments;
            if (rev.isConflicted())         selectedRev.flags |= kRevIsConflict | kRevLeaf;
            return true;
        }


        bool selectRevision(C4Slice revID, bool withBody) override {
            if (auto r = _findRemote(revID); r) {
                return _selectRemote(r->first, r->second);
            } else {
                _remoteID = nullopt;
                clearSelectedRevision();
                return false;
            }
        }


        optional<Revision> _selectedRevision() {
            return _remoteID ? _doc.remoteRevision(*_remoteID) : nullopt;
        }


        bool selectCurrentRevision() noexcept override {
            return _selectRemote(RemoteID::Local);
        }


        bool selectNextRevision() override {
            return _remoteID && _selectRemote(_doc.nextRemoteID(*_remoteID));
        }


        bool selectNextLeafRevision(bool includeDeleted) override {
            while (selectNextRevision()) {
                if (selectedRev.flags & kRevLeaf)
                    return true;
            }
            return false;
        }

        
#pragma mark - ACCESSORS:


        slice getSelectedRevBody() noexcept override {
            if (!_remoteID)
                return nullslice;
            else if (*_remoteID != RemoteID::Local)
                error::_throw(error::Unimplemented);    // FIXME: IMPLEMENT
            else if (_doc.contentAvailable() < kCurrentRevOnly)
                return nullslice;
            else
                return _doc.currentRevisionData();
        }


        FLDict getSelectedRevRoot() noexcept override {
            auto rev = _selectedRevision();
            return rev ? rev->properties : nullptr;
        }


        alloc_slice getSelectedRevIDGlobalForm() override {
            auto rev = _selectedRevision();
            return rev ? _expandRevID(rev->revID, myPeerID()) : nullslice;
        }


        alloc_slice getSelectedRevHistory(unsigned maxRevs) override {
            if (auto rev = _selectedRevision(); rev) {
                VersionVector vers;
                vers.readBinary(rev->revID);
                if (maxRevs > 0 && vers.count() > maxRevs)
                    vers.limitCount(maxRevs);
                // Easter egg: if maxRevs is 0, don't replace '*' with my peer ID [tests use this]
                return vers.asASCII(maxRevs ? myPeerID() : kMePeerID);
            } else {
                return nullslice;
            }
        }


        alloc_slice remoteAncestorRevID(C4RemoteID remote) override {
            if (auto rev = _doc.remoteRevision(RemoteID(remote)))
                return rev->revID.expanded();
            return nullptr;
        }


        void setRemoteAncestorRevID(C4RemoteID remote, C4String revID) override {
            Assert(RemoteID(remote) != RemoteID::Local);
            Revision revision;
            revidBuffer vers(revID);
            if (auto r = _findRemote(revID); r)
                revision = r->second;
            else
                revision.revID = vers;
            _doc.setRemoteRevision(RemoteID(remote), revision);
        }


#pragma mark - EXISTENCE / LOADING:


        bool exists() override {
            return _doc.exists();
        }


        bool hasRevisionBody() noexcept override {
            return _doc.exists() && _remoteID;
        }


        bool loadSelectedRevBody() override {
            if (!_remoteID)
                return false;
            auto which = (*_remoteID == RemoteID::Local) ? kCurrentRevOnly : kEntireBody;
            return _doc.loadData(which);
        }


#pragma mark - UPDATING:


        VersionVector _currentVersionVector() {
            auto curRevID = _doc.revID();
            return curRevID ? curRevID.asVersionVector() : VersionVector();
        }


        static DocumentFlags convertNewRevisionFlags(C4RevisionFlags revFlags) {
            DocumentFlags docFlags = {};
            if (revFlags & kRevDeleted)        docFlags |= DocumentFlags::kDeleted;
            if (revFlags & kRevHasAttachments) docFlags |= DocumentFlags::kHasAttachments;
            return docFlags;
        }

        
        fleece::Doc _newProperties(const C4DocPutRequest &rq, C4Error *outError) {
            alloc_slice body;
            if (rq.deltaCB == nullptr) {
                body = (rq.allocedBody.buf)? rq.allocedBody : alloc_slice(rq.body);
            } else {
                // Apply a delta via a callback:
                slice delta = (rq.allocedBody.buf)? slice(rq.allocedBody) : slice(rq.body);
                if (!rq.deltaSourceRevID.buf || !selectRevision(rq.deltaSourceRevID, true)) {
                    recordError(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                "Unknown source revision ID for delta", outError);
                    return nullptr;
                } else if (!getSelectedRevBody()) {
                    recordError(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                "Missing source revision body for delta", outError);
                    return nullptr;
                } else {
                    body = rq.deltaCB(rq.deltaCBContext, this, delta, outError);
                }
            }
            return _newProperties(body);
        }


        fleece::Doc _newProperties(alloc_slice body) {
            if (body.size > 0)
                database()->validateRevisionBody(body);
            else
                body = alloc_slice{(FLDict)Dict::emptyDict(), 2};
            Doc fldoc = Doc(body, kFLUntrusted, (FLSharedKeys)database()->documentKeys());
            Assert(fldoc.asDict());     // validateRevisionBody should have preflighted this
            return fldoc;
        }


        // Handles `c4doc_put` when `rq.existingRevision` is false (a regular save.)
        // The caller has already done most of the checking, incl. MVCC.
        bool putNewRevision(const C4DocPutRequest &rq, C4Error *outError) override {
            // Update the flags:
            Revision newRev;
            newRev.flags = convertNewRevisionFlags(rq.revFlags);

            // Update the version vector:
            auto newVers = _currentVersionVector();
            newVers.incrementGen(kMePeerID);
            alloc_slice newRevID = newVers.asBinary();
            newRev.revID = revid(newRevID);

            // Update the local body:
            C4Error err;
            Doc fldoc = _newProperties(rq, &err);
            if (!fldoc)
                return false;
            newRev.properties = fldoc.asDict();

            _db->dataFile()->_logVerbose("putNewRevision '%.*s' %s ; currently %s",
                    SPLAT(docID),
                    string(newVers.asASCII()).c_str(),
                    string(_currentVersionVector().asASCII()).c_str());

            // Store in NuDocument, and update C4Document properties:
            _doc.setCurrentRevision(newRev);
            _selectRemote(RemoteID::Local);
            return _saveNewRev(rq, newRev, outError);
        }


        // Handles `c4doc_put` when `rq.existingRevision` is true (called by the Pusher)
        int32_t putExistingRevision(const C4DocPutRequest &rq, C4Error *outError) override {
            Revision newRev;
            newRev.flags = convertNewRevisionFlags(rq.revFlags);
            Doc fldoc = _newProperties(rq, outError);
            if (!fldoc)
                return -1;
            newRev.properties = fldoc.asDict();

            // Parse the history array:
            VersionVector newVers;
            newVers.readHistory((slice*)rq.history, rq.historyCount, myPeerID());
            alloc_slice newVersBinary = newVers.asBinary();
            newRev.revID = revid(newVersBinary);

            // Does it fit the current revision?
            auto remote = RemoteID(rq.remoteDBID);
            int commonAncestor = 1;
            auto order = kNewer;
            if (_doc.exists()) {
                // See whether to update the local revision:
                order = newVers.compareTo(_currentVersionVector());
            }

            // Log the update. Normally verbose, but a conflict is info (if from the replicator)
            // or error (if local).
            if (DBLog.willLog(LogLevel::Verbose) || order == kConflicting) {
                static constexpr const char* kOrderName[4] = {"same", "older", "newer", "conflict"};
                alloc_slice newVersStr = newVers.asASCII();
                alloc_slice oldVersStr = _currentVersionVector().asASCII();
                if (order != kConflicting)
                    _db->dataFile()->_logVerbose(
                        "putExistingRevision '%.*s' #%.*s ; currently #%.*s --> %s (remote %d)",
                        SPLAT(docID), SPLAT(newVersStr), SPLAT(oldVersStr),
                        kOrderName[order], rq.remoteDBID);
                else if (remote != RemoteID::Local)
                    _db->dataFile()->_logInfo(
                        "putExistingRevision '%.*s' #%.*s ; currently #%.*s --> conflict (remote %d)",
                        SPLAT(docID), SPLAT(newVersStr), SPLAT(oldVersStr), rq.remoteDBID);
                else
                    _db->dataFile()->_logWarning(
                        "putExistingRevision '%.*s' #%.*s ; currently #%.*s --> conflict (remote %d)",
                        SPLAT(docID), SPLAT(newVersStr), SPLAT(oldVersStr), rq.remoteDBID);
            }

            switch (order) {
                case kSame:
                case kOlder:
                    // I already have this revision, don't update local
                    commonAncestor = 0;
                    break;
                case kNewer:
                    // It's newer, so update local to this revision:
                    _doc.setCurrentRevision(newRev);
                    break;
                case kConflicting:
                    // Conflict, so update only the remote (if any):
                    if (remote == RemoteID::Local) {
                        c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
                        return -1;
                    }
                    newRev.flags |= DocumentFlags::kConflicted;
                    break;
            }
            
            if (remote != RemoteID::Local) {
                // If this is a revision from a remote, update it in the doc:
                _doc.setRemoteRevision(remote, newRev);
            }

            // Update C4Document.selectedRev:
            _selectRemote(remote);

            // Save to DB, if requested:
            if (!_saveNewRev(rq, newRev, outError))
                return -1;

            return commonAncestor;
        }


        void resolveConflict(C4String winningRevID,
                             C4String losingRevID,
                             C4Slice mergedBody,
                             C4RevisionFlags mergedFlags,
                             bool pruneLosingBranch =true) override
        {
            auto won = _findRemote(winningRevID);
            auto lost = _findRemote(losingRevID);
            if (!won || !lost)
                error::_throw(error::NotFound, "Revision not found");
            if (won->first == lost->first)
                error::_throw(error::InvalidParameter, "That's the same revision");
            // One has to be Local, the other has to be a conflict:
            bool localWon = (won->first == RemoteID::Local);
            auto localRev = localWon ? won->second : lost->second;
            auto [remote, remoteRev] = localWon ? *lost : *won;
            if (!(remoteRev.flags & DocumentFlags::kConflicted))
                error::_throw(error::Conflict, "Revisions are not in conflict");

            // Construct a merged version vector:
            auto localVersion = localRev.versionVector();
            auto remoteVersion = remoteRev.versionVector();
            auto mergedVersion = localVersion.mergedWith(remoteVersion);
            mergedVersion.incrementGen(kMePeerID);
            alloc_slice mergedRevID = mergedVersion.asBinary();

            // Update the local/current revision with the resulting merge:
            Doc mergedDoc;
            localRev.revID = revid(mergedRevID);
            localRev.flags = convertNewRevisionFlags(mergedFlags);
            if (mergedBody.buf) {
                mergedDoc = _newProperties(alloc_slice(mergedBody));
                localRev.properties = mergedDoc.asDict();
            } else {
                localRev.properties = won->second.properties;
            }
            _doc.setCurrentRevision(localRev);

            // Remote rev is no longer a conflict:
            remoteRev.flags = remoteRev.flags - DocumentFlags::kConflicted;
            _doc.setRemoteRevision(remote, remoteRev);

            _updateDocFields();
            _selectRemote(RemoteID::Local);
            LogTo(DBLog, "Resolved conflict in '%.*s' between #%s and #%s -> #%s",
                  SPLAT(docID),
                  string(localVersion.asASCII()).c_str(),
                  string(remoteVersion.asASCII()).c_str(),
                  string(mergedVersion.asASCII()).c_str() );
        }


        bool _saveNewRev(const C4DocPutRequest &rq, const Revision &newRev, C4Error *outError) {
            if (rq.save && !save()) {
                c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
                return false;
            }
            return true;
        }


        bool save(unsigned maxRevTreeDepth =0) override {
            requireValidDocID();
            switch (_doc.save(database()->transaction())) {
                case NuDocument::kNoSave:
                    return true;
                case NuDocument::kNoNewSequence:
                    _updateDocFields();  // flags may have changed
                    return true;
                case NuDocument::kConflict:
                    return false;
                case NuDocument::kNewSequence:
                    _updateDocFields();
                    _selectRemote(RemoteID::Local);
                    if (_doc.sequence() > sequence)
                        sequence = selectedRev.sequence = _doc.sequence();
                    if (_db->dataFile()->willLog(LogLevel::Verbose)) {
                        alloc_slice revID = _doc.revID().expanded();
                        _db->dataFile()->_logVerbose( "%-s '%.*s' rev #%.*s as seq %" PRIu64,
                                                     ((flags & kRevDeleted) ? "Deleted" : "Saved"),
                                                     SPLAT(docID), SPLAT(revID), sequence);
                    }
                    database()->documentSaved(this);
                    return true;
            }
        }


    private:
        NuDocument          _doc;
        optional<RemoteID>  _remoteID;    // Identifies selected revision
    };


#pragma mark - FACTORY:


    Retained<Document> VectorDocumentFactory::newDocumentInstance(C4Slice docID) {
        return new VectorDocument(database(), docID, kEntireBody);
    }


    Retained<Document> VectorDocumentFactory::newDocumentInstance(const Record &record) {
        return new VectorDocument(database(), record);
    }


    Retained<Document> VectorDocumentFactory::newLeafDocumentInstance(C4Slice docID,
                                                                      C4Slice revID,
                                                                      bool withBody)
    {
        ContentOption opt = kMetaOnly;
        if (revID.buf)
            opt = kEntireBody;
        else if (withBody)
            opt = kCurrentRevOnly;
        Retained<VectorDocument> doc = new VectorDocument(database(), docID, opt);
        if (revID.buf)
            doc->selectRevision(revID, true);
        return doc;
    }


    Document* VectorDocumentFactory::documentContaining(FLValue value) {
        if (auto nuDoc = NuDocument::containing(value); nuDoc)
            return (VectorDocument*)nuDoc->owner;
        else
            return nullptr;
    }


    vector<alloc_slice> VectorDocumentFactory::findAncestors(const vector<slice> &docIDs,
                                                             const vector<slice> &revIDs,
                                                             unsigned maxAncestors,
                                                             bool mustHaveBodies,
                                                             C4RemoteID remoteDBID)
    {
        // Map docID->revID for faster lookup in the callback:
        unordered_map<slice,slice> revMap(docIDs.size());
        for (ssize_t i = docIDs.size() - 1; i >= 0; --i)
            revMap[docIDs[i]] = revIDs[i];
        peerID myPeerID {database()->myPeerID()};
        stringstream result;
        VersionVector aVers;

        auto callback = [&](const RecordLite &rec) -> alloc_slice {
            // --- This callback runs inside the SQLite query ---
            // --- It will be called once for each docID in the vector ---
            
            // Convert revID to encoded binary form:
            auto vers = VersionVector::fromASCII(revMap[rec.key], myPeerID);
            // It might be a single version not a vector:
            optional<Version> singleVers;
            if (vers.count() == 1)
                singleVers = vers[0];

            // First check whether the document has this exact version:
            bool found = false, notCurrent = false;
            NuDocument::forAllRevIDs(rec, [&](revid aRev, RemoteID aRemote) {
                aVers.readBinary(aRev);
                auto cmp = singleVers ? aVers.compareTo(*singleVers) : aVers.compareTo(vers);
                if (cmp == kSame || cmp == kNewer)
                    found = true;
                if (cmp != kSame && remoteDBID && C4RemoteID(aRemote) == remoteDBID)
                    notCurrent = true;
            });

            if (found) {
                if (notCurrent) {
                    return alloc_slice(kC4AncestorExistsButNotCurrent);
                } else {
                    static alloc_slice kAncestorExists = alloc_slice(kC4AncestorExists);
                    return kAncestorExists;
                }
            }

            // Find revs that could be ancestors of it and write them as a JSON array:
            result.str("");
            result << '[';
            unsigned n = 0;

            std::set<alloc_slice> added;
            NuDocument::forAllRevIDs(rec, [&](revid aRev, RemoteID) {
                if (n < maxAncestors) {
                    aVers.readBinary(aRev);
                    auto cmp = singleVers ? aVers.compareTo(*singleVers) : aVers.compareTo(vers);
                    if (cmp == kOlder) {
                        alloc_slice vector = aVers.asASCII(myPeerID);
                        if (added.insert(vector).second) {      // skip identical vectors
                            if (n++ == 0)
                                result << '"';
                            else
                                result << "\",\"";
                            result << vector;
                        }
                    }
                }
            });

            if (n > 0)
                result << '"';
            result << ']';
            return alloc_slice(result.str());
        };
        return database()->dataFile()->defaultKeyStore().withDocBodies(docIDs, callback);
    }


}
