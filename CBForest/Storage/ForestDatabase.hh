//
//  ForestDatabase.hh
//  CBNano
//
//  Created by Jens Alfke on 7/25/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef ForestDatabase_hh
#define ForestDatabase_hh

#include "Database.hh"
#include "fdb_types.h"

namespace cbforest {

    using namespace std;

    class ForestKeyStore;


    class ForestDatabase : public Database, public AutoCompacting {
    public:
        static fdb_config defaultConfig();
        static void setDefaultConfig(const fdb_config&);

        ForestDatabase(const string &path, Database::Options options);
        ForestDatabase(const string &path, Database::Options options, const fdb_config&);
        ~ForestDatabase();

        fdb_file_info info() const;
        fdb_config config() const                   {return _config;}

        static bool isAnyCompacting();

        void rekey(const fdb_encryption_key&);

        static void deleteDatabase(const string &path, const fdb_config&);

        // Inherited methods:
        bool isOpen() const override;
        void close() override;
        void deleteDatabase() override;
        void reopen() override;
        vector<string> allKeyStoreNames() override;
        void compact() override;
        void setAutoCompact(bool autoCompact) override;

    protected:
        KeyStore* newKeyStore(const string &name, KeyStore::Options) override;
        void deleteKeyStore(const string &name) override;
        void _beginTransaction(Transaction*) override;
        void _endTransaction(Transaction*) override;

    private:
        friend class ForestKeyStore;

        static fdb_compact_decision compactionCallback(fdb_file_handle *fhandle,
                                                       fdb_compaction_status status,
                                                       const char *kv_store_name,
                                                       fdb_doc *doc,
                                                       uint64_t last_oldfile_offset,
                                                       uint64_t last_newfile_offset,
                                                       void *ctx);
        bool onCompact(fdb_compaction_status status,
                       const char *kv_store_name,
                       fdb_doc *doc,
                       uint64_t lastOldFileOffset,
                       uint64_t lastNewFileOffset);

        fdb_config _config;
        fdb_file_handle* _fileHandle {nullptr};
    };



    class ForestKeyStore : public KeyStore {
    public:
        uint64_t documentCount() const override;
        sequence lastSequence() const override;

        Document get(sequence, ContentOptions) const override;
        bool read(Document &doc, ContentOptions options) const override;
        void readBody(Document &doc) const override;

        sequence set(slice key, slice meta, slice value, Transaction&) override;

        void erase() override;

    protected:
        ForestKeyStore(ForestDatabase&, const string &name, KeyStore::Options options);
        ~ForestKeyStore();

        void reopen() override;
        void close() override;

        bool _del(slice key, Transaction&) override;
        bool _del(sequence s, Transaction&) override;

        void setDocNoKey(Document &doc, fdb_doc &fdoc) const;
        void setDoc(Document &doc, fdb_doc &fdoc) const;

        DocEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey, DocEnumerator::Options&) override;
        DocEnumerator::Impl* newEnumeratorImpl(sequence min, sequence max, DocEnumerator::Options&) override;

    private:
        friend class ForestDatabase;
        friend class ForestEnumerator;

        fdb_kvs_handle* _handle {nullptr};
    };
}


#endif /* ForestDatabase_hh */
