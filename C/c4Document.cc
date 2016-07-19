//
//  c4Document.cc
//  CBForest
//
//  Created by Jens Alfke on 11/6/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#define NOMINMAX
#include "c4Impl.hh"
#include "c4Document.h"
#include "c4Database.h"
#include "c4Private.h"

#include "c4DocInternal.hh"
#include "SecureRandomize.hh"
#include "SecureDigest.hh"
#include "varint.hh"
#include <ctime>
#include <algorithm>

#include <algorithm>

using namespace cbforest;


static const uint32_t kDefaultMaxRevTreeDepth = 20;


namespace cbforest {

    class C4DocumentV1 : public C4DocumentInternal {
    public:
        C4DocumentV1(C4Database* database, C4Slice docID)
        :C4DocumentInternal(database, docID),
         _versionedDoc(*database, docID),
         _selectedRev(NULL)
        {
            init();
        }


        C4DocumentV1(C4Database *database, Document &&doc)
        :C4DocumentInternal(database, std::move(doc)),
         _versionedDoc(*database, std::move(doc)),
         _selectedRev(NULL)
        {
            init();
        }


        void init() {
            docID = _versionedDoc.docID();
            flags = (C4DocumentFlags)_versionedDoc.flags();
            if (_versionedDoc.exists())
                flags = (C4DocumentFlags)(flags | kExists);

            initRevID();
            selectCurrentRevision();
        }

        void initRevID() {
            if (_versionedDoc.revID().size > 0) {
                _revIDBuf = _versionedDoc.revID().expanded();
            } else {
                _revIDBuf = slice::null;
            }
            revID = _revIDBuf;
            sequence = _versionedDoc.sequence();
        }

        C4SliceResult type() override {
            slice result = _versionedDoc.docType().copy();
            return {result.buf, result.size};
        }

        void setType(C4Slice docType) override {
            _versionedDoc.setDocType(docType);
        }


        bool exists() override {
            return _versionedDoc.exists();
        }

        bool selectRevision(C4Slice revID, bool withBody, C4Error *outError) override {
            if (revID.buf) {
                if (!loadRevisions(outError))
                    return false;
                const Revision *rev = _versionedDoc[revidBuffer(revID)];
                return selectRevision(rev, outError) && (!withBody || loadSelectedRevBody(outError));
            } else {
                selectRevision(NULL);
                return true;
            }
        }

        bool selectRevision(const Revision *rev, C4Error *outError =NULL) override {
            _selectedRev = rev;
            _loadedBody = slice::null;
            if (rev) {
                _selectedRevIDBuf = rev->revID.expanded();
                selectedRev.revID = _selectedRevIDBuf;
                selectedRev.flags = (C4RevisionFlags)rev->flags;
                selectedRev.sequence = rev->sequence;
                selectedRev.body = rev->inlineBody();
                return true;
            } else {
                _selectedRevIDBuf = slice::null;
                selectedRev.revID = slice::null;
                selectedRev.flags = (C4RevisionFlags)0;
                selectedRev.sequence = 0;
                selectedRev.body = slice::null;
                recordHTTPError(kC4HTTPNotFound, outError);
                return false;
            }
        }

        bool selectCurrentRevision() override {
            if (_versionedDoc.revsAvailable()) {
                return selectRevision(_versionedDoc.currentRevision());
            } else {
                // Doc body (rev tree) isn't available, but we know enough about the current rev:
                _selectedRev = NULL;
                selectedRev.revID = revID;
                selectedRev.sequence = sequence;
                int revFlags = 0;
                if (flags & kExists) {
                    revFlags |= kRevLeaf;
                    if (flags & kDeleted)
                        revFlags |= kRevDeleted;
                    if (flags & kHasAttachments)
                        revFlags |= kRevHasAttachments;
                }
                selectedRev.flags = (C4RevisionFlags)revFlags;
                selectedRev.body = slice::null;
                return true;
            }
        }

        bool revisionsLoaded() const override {
            return _versionedDoc.revsAvailable();
        }

        bool loadRevisions(C4Error *outError) override {
            if (_versionedDoc.revsAvailable())
                return true;
            try {
                WITH_LOCK(_db);
                _versionedDoc.read();
                _selectedRev = _versionedDoc.currentRevision();
                return true;
            } catchError(outError)
            return false;
        }

        bool hasRevisionBody() override {
            if (!revisionsLoaded())
                Warn("c4doc_hasRevisionBody called on doc loaded without kC4IncludeBodies");
            WITH_LOCK(database());
            return _selectedRev && _selectedRev->isBodyAvailable();
        }

        bool selectParentRevision() override {
            if (!revisionsLoaded())
                Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
            if (_selectedRev)
                selectRevision(_selectedRev->parent());
            return _selectedRev != NULL;
        }

        bool selectNextRevision() override {
            if (!revisionsLoaded())
                Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
            if (_selectedRev)
                selectRevision(_selectedRev->next());
            return _selectedRev != NULL;
        }

        bool selectNextLeafRevision(bool includeDeleted, bool withBody, C4Error *outError) override {
            if (!revisionsLoaded())
                Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
            auto rev = _selectedRev;
            if (rev) {
                do {
                    rev = rev->next();
                } while (rev && (!rev->isLeaf() || (!includeDeleted && rev->isDeleted())));
            }
            if (!selectRevision(rev, NULL)) {
                clearError(outError);  // Normal termination, not error
                return false;
            }
            return (!withBody || loadSelectedRevBody(outError));
        }

        bool loadSelectedRevBody(C4Error *outError) override {
            if (!loadRevisions(outError))
                return false;
            if (!_selectedRev)
                return true;
            if (selectedRev.body.buf)
                return true;  // already loaded
            try {
                WITH_LOCK(_db);
                _loadedBody = _selectedRev->readBody();
                selectedRev.body = _loadedBody;
                if (_loadedBody.buf)
                    return true;
                recordHTTPError(kC4HTTPGone, outError); // 410 Gone to denote body that's been compacted away
            } catchError(outError);
            return false;
        }

        void updateMeta() {
            _versionedDoc.updateMeta();
            flags = (C4DocumentFlags)(_versionedDoc.flags() | kExists);
            initRevID();
        }

        void save(unsigned maxRevTreeDepth) override {
            _versionedDoc.prune(maxRevTreeDepth);
            {
                WITH_LOCK(_db);
                _versionedDoc.save(*_db->transaction());
            }
            sequence = _versionedDoc.sequence();
        }

        int32_t purgeRevision(C4Slice revID) override {
            int32_t total = _versionedDoc.purge(revidBuffer(revID));
            if (total > 0) {
                updateMeta();
                if (_selectedRevIDBuf == revID)
                    selectRevision(_versionedDoc.currentRevision());
            }
            return total;
        }
            
        public:
            VersionedDocument _versionedDoc;
            const Revision *_selectedRev;

    };


    C4DocumentInternal* C4DocumentInternal::newInstance(C4Database* database, C4Slice docID) {
        if (database->schema <= 1)
            return new C4DocumentV1(database, docID);
        else
            return new C4DocumentV2(database, docID);
    }

    C4DocumentInternal* C4DocumentInternal::newInstance(C4Database* database, Document &&doc) {
        if (database->schema <= 1)
            return new C4DocumentV1(database, std::move(doc));
        else
            return new C4DocumentV2(database, std::move(doc));
    }

}


namespace c4Internal {
    static inline C4DocumentV1* internalV1(C4Document *doc) {
        return (C4DocumentV1*)internal(doc);
    }

    C4Document* newC4Document(C4Database *db, Document &&doc) {
        // Doesn't need to lock since Document is already in memory
        return C4DocumentInternal::newInstance(db, std::move(doc));
    }

    const VersionedDocument& versionedDocument(C4Document* doc) {
        return internalV1(doc)->_versionedDoc;
    }
}


#pragma mark - PUBLIC API:


void c4doc_free(C4Document *doc) {
    delete (C4DocumentInternal*)doc;
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = C4DocumentInternal::newInstance(database, docID);
        if (mustExist && !doc->exists()) {
            delete doc;
            doc = NULL;
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        
        
        return doc;
    } catchError(outError);
    return NULL;
}


C4Document* c4doc_getBySequence(C4Database *database,
                                C4SequenceNumber sequence,
                                C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = C4DocumentInternal::newInstance(database, database->get(sequence));
        if (!doc->exists()) {
            delete doc;
            doc = NULL;
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        return doc;
    } catchError(outError);
    return NULL;
}


C4SliceResult c4doc_getType(C4Document *doc) {
    return internal(doc)->type();
}

void c4doc_setType(C4Document *doc, C4Slice docType) {
    return internal(doc)->setType(docType);
}


#pragma mark - REVISIONS:


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError)
{
    try {
        return internal(doc)->selectRevision(revID, withBody, outError);
    } catchError(outError);
    return false;
}


bool c4doc_selectCurrentRevision(C4Document* doc)
{
    return internal(doc)->selectCurrentRevision();
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) {
    return internal(doc)->loadSelectedRevBody(outError);
}


bool c4doc_hasRevisionBody(C4Document* doc) {
    try {
        return internal(doc)->hasRevisionBody();
    } catchError(NULL);
    return false;
}


bool c4doc_selectParentRevision(C4Document* doc) {
    return internal(doc)->selectParentRevision();
}


bool c4doc_selectNextRevision(C4Document* doc) {
    return internal(doc)->selectNextRevision();
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError)
{
    internal(doc)->selectNextLeafRevision(includeDeleted, withBody, outError);
}


unsigned c4rev_getGeneration(C4Slice revID) {
    try {
        return revidBuffer(revID).generation();
    }catchError(NULL)
    return 0;
}


#pragma mark - INSERTING REVISIONS


// Internal form of c4doc_insertRevision that takes compressed revID and doesn't check preconditions
static int32_t insertRevision(C4DocumentV1 *idoc,
                              revid encodedRevID,
                              C4Slice body,
                              bool deletion,
                              bool hasAttachments,
                              bool allowConflict,
                              C4Error *outError)
{
    try {
        int httpStatus;
        auto newRev = idoc->_versionedDoc.insert(encodedRevID,
                                                 body,
                                                 deletion,
                                                 hasAttachments,
                                                 idoc->_selectedRev,
                                                 allowConflict,
                                                 httpStatus);
        if (newRev) {
            // Success:
            idoc->updateMeta();
            newRev = idoc->_versionedDoc.get(encodedRevID);
            idoc->selectRevision(newRev);
            return 1;
        } else if (httpStatus == 200) {
            // Revision already exists, so nothing was added. Not an error.
            c4doc_selectRevision(idoc, encodedRevID.expanded(), true, outError);
            return 0;
        }
        recordHTTPError(httpStatus, outError);
    } catchError(outError)
    return -1;
}


int32_t c4doc_insertRevision(C4Document *doc,
                             C4Slice revID,
                             C4Slice body,
                             bool deletion,
                             bool hasAttachments,
                             bool allowConflict,
                             C4Error *outError)
{
    auto idoc = internalV1(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(outError))
        return -1;
    try {
        revidBuffer encodedRevID(revID);  // this can throw!
        return insertRevision(idoc, encodedRevID, body, deletion, hasAttachments, allowConflict,
                              outError);
    } catchError(outError)
    return -1;
}


int32_t c4doc_insertRevisionWithHistory(C4Document *doc,
                                        C4Slice body,
                                        bool deleted,
                                        bool hasAttachments,
                                        const C4Slice history[],
                                        size_t historyCount,
                                        C4Error *outError)
{
    if (historyCount < 1)
        return 0;
    auto idoc = internalV1(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(outError))
        return -1;
    int32_t commonAncestor = -1;
    try {
        std::vector<revidBuffer> revIDBuffers(historyCount);
        for (size_t i = 0; i < historyCount; i++)
            revIDBuffers[i].parse(history[i]);
        commonAncestor = idoc->_versionedDoc.insertHistory(revIDBuffers,
                                                           body,
                                                           deleted,
                                                           hasAttachments);
        if (commonAncestor >= 0) {
            idoc->updateMeta();
            idoc->selectRevision(idoc->_versionedDoc[revidBuffer(history[0])]);
        } else {
            recordHTTPError(kC4HTTPBadRequest, outError); // must be invalid revision IDs
        }
    } catchError(outError)
    return commonAncestor;
}


int32_t c4doc_purgeRevision(C4Document *doc,
                            C4Slice revID,
                            C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(outError))
        return -1;
    try {
        return idoc->purgeRevision(revID);
    } catchError(outError)
    return -1;
}


bool c4doc_save(C4Document *doc,
                uint32_t maxRevTreeDepth,
                C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return false;
    try {
        idoc->save(maxRevTreeDepth ? maxRevTreeDepth : kDefaultMaxRevTreeDepth);
        return true;
    } catchError(outError)
    return false;
}

static alloc_slice createDocUUID() {
#if SECURE_RANDOMIZE_AVAILABLE
    static const char kBase64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                    "0123456789-_";
    const unsigned kLength = 22; // 22 random base64 chars = 132 bits of entropy
    uint8_t r[kLength];
    SecureRandomize({r, sizeof(r)});

    alloc_slice docIDSlice(1+kLength);
    char *docID = (char*)docIDSlice.buf;
    docID[0] = '-';
    for (unsigned i = 0; i < kLength; ++i)
        docID[i+1] = kBase64[r[i] % 64];
    return docIDSlice;
#else
    error::_throw(FDB_RESULT_CRYPTO_ERROR);
#endif
}


static bool sGenerateOldStyleRevIDs = false;


static revidBuffer generateDocRevID(C4Slice body, C4Slice parentRevID, bool deleted) {
#if SECURE_DIGEST_AVAILABLE
    uint8_t digestBuf[20];
    slice digest;
    if (sGenerateOldStyleRevIDs) {
        // Get MD5 digest of the (length-prefixed) parent rev ID, deletion flag, and revision body:
        md5Context ctx;
        md5_begin(&ctx);
        uint8_t revLen = (uint8_t)std::min((unsigned long)parentRevID.size, 255ul);
        if (revLen > 0)     // Intentionally repeat a bug in CBL's algorithm :)
            md5_add(&ctx, &revLen, 1);
        md5_add(&ctx, parentRevID.buf, revLen);
        uint8_t delByte = deleted;
        md5_add(&ctx, &delByte, 1);
        md5_add(&ctx, body.buf, body.size);
        md5_end(&ctx, digestBuf);
        digest = slice(digestBuf, 16);
    } else {
        // SHA-1 digest:
        sha1Context ctx;
        sha1_begin(&ctx);
        uint8_t revLen = (uint8_t)std::min((unsigned long)parentRevID.size, 255ul);
        sha1_add(&ctx, &revLen, 1);
        sha1_add(&ctx, parentRevID.buf, revLen);
        uint8_t delByte = deleted;
        sha1_add(&ctx, &delByte, 1);
        sha1_add(&ctx, body.buf, body.size);
        sha1_end(&ctx, digestBuf);
        digest = slice(digestBuf, 20);
    }

    // Derive new rev's generation #:
    unsigned generation = 1;
    if (parentRevID.buf) {
        revidBuffer parentID(parentRevID);
        generation = parentID.generation() + 1;
    }
    return revidBuffer(generation, digest, kDigestType);
#else
    error::_throw(FDB_RESULT_CRYPTO_ERROR);
#endif
}

C4SliceResult c4doc_generateRevID(C4Slice body, C4Slice parentRevID, bool deleted) {
    slice result = generateDocRevID(body, parentRevID, deleted).expanded().dontFree();
    return {result.buf, result.size};
}

void c4doc_generateOldStyleRevID(bool generateOldStyle) {
    sGenerateOldStyleRevIDs = generateOldStyle;
}

// Finds a document for a Put of a _new_ revision, and selects the existing parent revision.
// After this succeeds, you can call c4doc_insertRevision and then c4doc_save.
C4Document* c4doc_getForPut(C4Database *database,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError)
{
    if (!database->mustBeInTransaction(outError))
        return NULL;
    C4DocumentV1 *idoc = NULL;
    try {
        do {
            alloc_slice newDocID;
            bool isNewDoc = (!docID.buf);
            if (isNewDoc) {
                newDocID = createDocUUID();
                docID = newDocID;
            }

            idoc = new C4DocumentV1(database, docID);

            if (!isNewDoc && !idoc->loadRevisions(outError))
                break;

            if (parentRevID.buf) {
                // Updating an existing revision; make sure it exists and is a leaf:
                const Revision *rev = idoc->_versionedDoc[revidBuffer(parentRevID)];
                if (!idoc->selectRevision(rev, outError))
                    break;
                else if (!allowConflict && !rev->isLeaf()) {
                    recordHTTPError(kC4HTTPConflict, outError);
                    break;
                }
            } else {
                // No parent revision given:
                if (deleting) {
                    // Didn't specify a revision to delete: NotFound or a Conflict, depending
                    recordHTTPError(idoc->_versionedDoc.exists() ?kC4HTTPConflict :kC4HTTPNotFound,
                                    outError );
                    break;
                }
                // If doc exists, current rev must be in a deleted state or there will be a conflict:
                const Revision *rev = idoc->_versionedDoc.currentRevision();
                if (rev) {
                    if (!rev->isDeleted()) {
                        recordHTTPError(kC4HTTPConflict, outError);
                        break;
                    }
                    // New rev will be child of the tombstone:
                    // (T0D0: Write a horror novel called "Child Of The Tombstone"!)
                    if (!idoc->selectRevision(rev, outError))
                        break;
                }
            }
            return idoc;
        } while (false); // not a real loop; it's just to allow 'break' statements to exit

    } catchError(outError)
    delete idoc;
    return NULL;
}


C4Document* c4doc_put(C4Database *database,
                      const C4DocPutRequest *rq,
                      size_t *outCommonAncestorIndex,
                      C4Error *outError)
{
    if (!database->mustBeInTransaction(outError))
        return NULL;
    int inserted;
    C4Document *doc;
    if (rq->existingRevision) {
        // Existing revision:
        if (rq->docID.size == 0 || rq->historyCount == 0) {
            recordHTTPError(kC4HTTPBadRequest, outError);
            return NULL;
        }
        doc = c4doc_get(database, rq->docID, false, outError);
        if (!doc)
            return NULL;

        inserted = c4doc_insertRevisionWithHistory(doc, rq->body, rq->deletion, rq->hasAttachments,
                                                   rq->history, rq->historyCount, outError);
    } else {
        // New revision:
        C4Slice parentRevID;
        if (rq->historyCount == 1) {
            parentRevID = rq->history[0];
        } else if (rq->historyCount > 1) {
            recordHTTPError(kC4HTTPBadRequest, outError);
            return NULL;
        }
        doc = c4doc_getForPut(database, rq->docID, parentRevID, rq->deletion, rq->allowConflict,
                              outError);
        if (!doc)
            return NULL;

        revidBuffer revID = generateDocRevID(rq->body, doc->selectedRev.revID, rq->deletion);

        inserted = insertRevision(internalV1(doc), revID, rq->body, rq->deletion, rq->hasAttachments,
                                  rq->allowConflict, outError);
    }

    // Save:
    if (inserted < 0 || (inserted > 0 && rq->save && !c4doc_save(doc, rq->maxRevTreeDepth,
                                                                 outError))) {
        c4doc_free(doc);
        return NULL;
    }
    
    if (outCommonAncestorIndex)
        *outCommonAncestorIndex = inserted;
    return doc;
}
