//
// Database+Upgrade.cc
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

#include "Database.hh"
#include "NuDocument.hh"
#include "RecordEnumerator.hh"
#include "RevID.hh"
#include "RevTreeRecord.hh"
#include "VersionVector.hh"
#include "StringUtil.hh"
#include <vector>

namespace c4Internal {
    using namespace std;
    using namespace litecore;

    // The fake peer/source ID used for versions migrated from revIDs.
    static constexpr peerID kLegacyPeerID {0x7777777};

    static constexpr const char* kNameOfVersioning[3] = {
        "v2.x rev-trees", "v3.x rev-trees", "version vectors"};

    static pair<alloc_slice, alloc_slice>
    upgradeRemoteRevs(Database*, Record rec, RevTreeRecord&, alloc_slice currentVersion);


    static const Rev* commonAncestor(const Rev *a, const Rev *b) {
        if (a && b) {
            for (auto rev = b; rev; rev = rev->parent) {
                if (rev->isAncestorOf(a))
                    return rev;
            }
        }
        return nullptr;
    }


    void Database::upgradeDocumentVersioning(C4DocumentVersioning curVersioning,
                                             C4DocumentVersioning newVersioning,
                                             Transaction &t)
    {
        if (newVersioning == curVersioning)
            return;
        if (newVersioning < curVersioning)
            error::_throw(error::Unimplemented, "Cannot downgrade document versioning");
        if (_config.flags & (kC4DB_ReadOnly |kC4DB_NoUpgrade))
            error::_throw(error::CantUpgradeDatabase, "Document versioning needs upgrade");

        LogTo(DBLog, "*** Upgrading stored documents from %s to %s ***",
              kNameOfVersioning[curVersioning], kNameOfVersioning[newVersioning]);
        uint64_t docCount = 0;

        // Iterate over all documents:
        RecordEnumerator::Options options;
        options.sortOption = kUnsorted;
        options.includeDeleted = true;
        options.contentOption = kEntireBody;
        RecordEnumerator e(defaultKeyStore(), options);
        while (e.next()) {
            // Read the doc as a RevTreeRecord:
            const Record &rec = e.record();
            RevTreeRecord revTree(defaultKeyStore(), rec);

            if (newVersioning == kC4VectorVersioning) {
                // Upgrade from rev-trees (v2 or v3) to version-vectors:
                auto currentRev = revTree.currentRevision();
                auto remoteRev = revTree.latestRevisionOnRemote(RevTree::kDefaultRemoteID);
                auto baseRev = commonAncestor(currentRev, remoteRev);

                // Create a version vector:
                // - If there's a remote base revision, use its generation with the legacy peer ID.
                // - Add the current rev's generation (relative to the remote base, if any)
                //   with the local 'me' peer ID.
                VersionVector vv;
                int localChanges = int(currentRev->revID.generation());
                if (baseRev) {
                    vv.add({baseRev->revID.generation(), kLegacyPeerID});
                    localChanges -= int(baseRev->revID.generation());
                }
                if (localChanges > 0)
                    vv.add({generation(localChanges), kMePeerID});
                auto binaryVersion = vv.asBinary();

                // Propagate any saved remote revisions to the new document:
                slice body;
                alloc_slice allocedBody, extra;
                if (revTree.remoteRevisions().empty()) {
                    body = currentRev->body();
                } else {
                    std::tie(allocedBody, extra) = upgradeRemoteRevs(this, rec, revTree, binaryVersion);
                    body = allocedBody;
                }

                // Now save:
                RecordLite newRec;
                newRec.key = revTree.docID();
                newRec.flags = revTree.flags();
                newRec.body = body;
                newRec.extra = extra;
                newRec.version = binaryVersion;
                newRec.sequence = revTree.sequence();
                newRec.updateSequence = false;
                //TODO: Find conflicts and add them to newRec.extra
                defaultKeyStore().set(newRec, t);

                LogToAt(DBLog, Verbose, "  - Upgraded doc '%.*s', %s -> [%s], %zu bytes body, %zu bytes extra",
                        SPLAT(rec.key()),
                        revid(rec.version()).str().c_str(),
                        string(vv.asASCII()).c_str(),
                        newRec.body.size, newRec.extra.size);
            } else {
                // Upgrade v2 rev-tree storage to new db schema with `extra` column:
                auto result = revTree.save(t);
                Assert(result == RevTreeRecord::kNoNewSequence);
                LogToAt(DBLog, Verbose, "  - Upgraded doc '%.*s' #%s",
                        SPLAT(rec.key()),
                        revid(rec.version()).str().c_str());
            }

            ++docCount;
        }

        LogTo(DBLog, "*** %llu documents upgraded, now committing changes... ***", docCount);
    }


    static pair<alloc_slice, alloc_slice> upgradeRemoteRevs(Database *db,
                                                            Record rec,
                                                            RevTreeRecord &revTree,
                                                            alloc_slice currentVersion)
    {
        // Make an in-memory VV-based Record, with no remote revisions:
        const Rev *currentRev = revTree.currentRevision();
        rec.setVersion(currentVersion);
        rec.setBody(currentRev->body());
        rec.setExtra(nullslice);

        // Instantiate a NuDocument for this document, without reading the database:
        NuDocument nuDoc(db->defaultKeyStore(), Versioning::RevTrees, rec);
        nuDoc.setEncoder(db->sharedFLEncoder());

        // Add each remote revision:
        for (auto i = revTree.remoteRevisions().begin(); i != revTree.remoteRevisions().end(); ++i) {
            auto remoteID = RemoteID(i->first);
            const Rev *rev = i->second;
            Revision nuRev;
            alloc_slice binaryVers;
            if (rev == currentRev) {
                nuRev = nuDoc.currentRevision();
            } else {
                if (rev->body())
                    nuRev.properties = fleece::Value::fromData(rev->body(), kFLTrusted).asDict();
                nuRev.flags = {};
                if (rev->flags & Rev::kDeleted)        nuRev.flags |= DocumentFlags::kDeleted;
                if (rev->flags & Rev::kHasAttachments) nuRev.flags |= DocumentFlags::kHasAttachments;

                VersionVector vv;
                vv.add({rev->revID.generation(), kLegacyPeerID});
                binaryVers = vv.asBinary();
                nuRev.revID = revid(binaryVers);
            }
            nuDoc.setRemoteRevision(remoteID, nuRev);
        }

        return nuDoc.encodeBodyAndExtra();
    }


}
