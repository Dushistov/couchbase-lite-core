//
//  c4PerfTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/20/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Fleece.h"     // including this before c4 makes FLSlice and C4Slice compatible
#include "c4Test.hh"
#include "Base.hh"
#include "Benchmark.hh"
#include <fcntl.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

#ifdef NDEBUG
#undef REQUIRE  // it slows down the tests significantly
#define REQUIRE(X) ({if (!(X)) abort();})
#endif

using namespace fleece;


static const char* kJSONFilePath = "C/tests/iTunesMusicLibrary.json";



struct totalContext {
    double total;
    char value[30];
};

// accumulate function that simply totals numeric values. `context` must point to a totalContext.
static void total_accumulate(void *context, C4Key *key, C4Slice value) {
    auto ctx = (totalContext*)context;
    FLValue v = FLValue_FromTrustedData(value);
    REQUIRE(FLValue_GetType(v) == kFLNumber);
    ctx->total += FLValue_AsDouble(v);
}

// reduce function that returns the row total. `context` must point to a totalContext.
static C4Slice total_reduce (void *context) {
    auto ctx = (totalContext*)context;
    sprintf(ctx->value, "%g", ctx->total);
    ctx->total = 0.0;
    return {ctx->value, strlen(ctx->value)};
}


class PerfTest : public C4Test {
public:
    PerfTest(int variation) :C4Test(variation) { }

    ~PerfTest() {
        c4view_free(artistsView);
        c4view_free(albumsView);
        c4view_free(tracksView);
    }

    // Reads a file into memory.
    alloc_slice readFile(const char *path) {
        int fd = ::open(path, O_RDONLY);
        REQUIRE(fd != -1);
        struct stat stat;
        fstat(fd, &stat);
        alloc_slice data(stat.st_size);
        ssize_t bytesRead = ::read(fd, (void*)data.buf, data.size);
        REQUIRE(bytesRead == data.size);
        ::close(fd);
        return data;
    }


    // Copies a Fleece dictionary key/value to an encoder
    static bool copyValue(FLDict srcDict, FLDictKey *key, FLEncoder enc) {
        FLValue value = FLDict_GetWithKey(srcDict, key);
        if (!value)
            return false;
        FLEncoder_WriteKey(enc, FLDictKey_GetString(key));
        FLEncoder_WriteValue(enc, value);
        return true;
    }


    unsigned insertDocs(FLArray docs) {
        FLDictKey typeKey   = FLDictKey_Init(FLSTR("Track Type"), true);
        FLDictKey idKey     = FLDictKey_Init(FLSTR("Persistent ID"), true);
        FLDictKey nameKey   = FLDictKey_Init(FLSTR("Name"), true);
        FLDictKey albumKey  = FLDictKey_Init(FLSTR("Album"), true);
        FLDictKey artistKey = FLDictKey_Init(FLSTR("Artist"), true);
        FLDictKey timeKey   = FLDictKey_Init(FLSTR("Total Time"), true);
        FLDictKey genreKey  = FLDictKey_Init(FLSTR("Genre"), true);
        FLDictKey yearKey   = FLDictKey_Init(FLSTR("Year"), true);
        FLDictKey trackNoKey= FLDictKey_Init(FLSTR("Track Number"), true);
        FLDictKey compKey   = FLDictKey_Init(FLSTR("Compilation"), true);

        TransactionHelper t(db);

        FLEncoder enc = FLEncoder_New();
        FLArrayIterator iter;
        FLArrayIterator_Begin(docs, &iter);
        unsigned numDocs = 0;
        while (FLArrayIterator_Next(&iter)) {
            // Check that track is correct type:
            FLDict track = FLValue_AsDict( FLArrayIterator_GetValue(&iter) );

            FLSlice trackType = FLValue_AsString( FLDict_GetWithKey(track, &typeKey) );
            if (0 != FLSlice_Compare(trackType, FLSTR("File")) &&
                0 != FLSlice_Compare(trackType, FLSTR("Remote")))
                continue;

            FLSlice trackID = FLValue_AsString( FLDict_GetWithKey(track, &idKey) );
            REQUIRE(trackID.buf);

            // Encode doc body:
            FLEncoder_BeginDict(enc, 0);
            REQUIRE(copyValue(track, &nameKey, enc));
            copyValue(track, &albumKey, enc);
            copyValue(track, &artistKey, enc);
            copyValue(track, &timeKey, enc);
            copyValue(track, &genreKey, enc);
            copyValue(track, &yearKey, enc);
            copyValue(track, &trackNoKey, enc);
            copyValue(track, &compKey, enc);
            FLEncoder_EndDict(enc);
            FLError error;
            FLSliceResult body = FLEncoder_Finish(enc, &error);
            REQUIRE(body.buf);
            FLEncoder_Reset(enc);

            // Save document:
            C4Error c4err;
            C4DocPutRequest rq = {};
            rq.docID = trackID;
            rq.body = (C4Slice)body;
            rq.save = true;
            C4Document *doc = c4doc_put(db, &rq, nullptr, &c4err);
            REQUIRE(doc != nullptr);
            c4doc_free(doc);
            FLSliceResult_Free(body);
            ++numDocs;
        }
        
        FLEncoder_Free(enc);
        return numDocs;
    }


    void indexViews() {
        FLDictKey nameKey   = FLDictKey_Init(FLSTR("Name"), true);
        FLDictKey albumKey  = FLDictKey_Init(FLSTR("Album"), true);
        FLDictKey artistKey = FLDictKey_Init(FLSTR("Artist"), true);
        FLDictKey timeKey   = FLDictKey_Init(FLSTR("Total Time"), true);
        FLDictKey trackNoKey= FLDictKey_Init(FLSTR("Track Number"), true);
        FLDictKey compKey   = FLDictKey_Init(FLSTR("Compilation"), true);

        auto enc = FLEncoder_New();
        auto key = c4key_new();

        C4Error error;
        if (!artistsView) {
            artistsView = c4view_open(db, kC4SliceNull, C4STR("Artists"),
                                      C4STR("1"), c4db_getConfig(db), &error);
            REQUIRE(artistsView);
        }
        if (!albumsView) {
            albumsView = c4view_open(db, kC4SliceNull, C4STR("Albums"),
                                     C4STR("1"), c4db_getConfig(db), &error);
            REQUIRE(albumsView);
        }

        C4View* views[2] = {artistsView, albumsView};
        C4Indexer *indexer = c4indexer_begin(db, views, 2, &error);
        REQUIRE(indexer);
        auto e = c4indexer_enumerateDocuments(indexer, &error);
        REQUIRE(e);
        while (c4enum_next(e, &error)) {
            auto doc = c4enum_getDocument(e, &error);
            FLDict body = FLValue_AsDict( FLValue_FromTrustedData(doc->selectedRev.body) );
            REQUIRE(body);


            FLSlice artist;
            if (FLValue_AsBool(FLDict_GetWithKey(body, &compKey)))
                artist = FLSTR("-Compilations-");
            else
                artist   = FLValue_AsString( FLDict_GetWithKey(body, &artistKey) );
            auto name    = FLValue_AsString( FLDict_GetWithKey(body, &nameKey) );
            auto album   = FLValue_AsString( FLDict_GetWithKey(body, &albumKey) );
            auto trackNo = FLValue_AsInt( FLDict_GetWithKey(body, &trackNoKey) );
            auto time    = FLDict_GetWithKey(body, &timeKey);

            // Generate value:
            FLEncoder_WriteValue(enc, time);
            FLError flError;
            FLSliceResult fval = FLEncoder_Finish(enc, &flError);
            FLEncoder_Reset(enc);
            REQUIRE(fval.buf);
            auto value = (C4Slice)fval;

            // Emit to artists view:
            unsigned nKeys = 0;
            if (artist.buf && name.buf) {
                nKeys = 1;
                // Generate key:
                c4key_beginArray(key);
                c4key_addString(key, artist);
                if (album.buf)
                    c4key_addString(key, album);
                else
                    c4key_addNull(key);
                c4key_addNumber(key, trackNo);
                c4key_addString(key, name);
                c4key_addNumber(key, 1);
                c4key_endArray(key);
            }
            REQUIRE(c4indexer_emit(indexer, doc, 0, nKeys, &key, &value, &error));
            c4key_reset(key);

            // Emit to albums view:
            nKeys = 0;
            if (album.buf) {
                nKeys = 1;
                // Generate key:
                c4key_beginArray(key);
                c4key_addString(key, album);
                if (artist.buf)
                    c4key_addString(key, artist);
                else
                    c4key_addNull(key);
                c4key_addNumber(key, trackNo);
                if (!name.buf)
                    name = FLSTR("");
                c4key_addString(key, name);
                c4key_addNumber(key, 1);
                c4key_endArray(key);
            }
            REQUIRE(c4indexer_emit(indexer, doc, 1, nKeys, &key, &value, &error));
            c4key_reset(key);

            FLSliceResult_Free(fval);
            c4doc_free(doc);
        }
        c4enum_free(e);
        REQUIRE(error.code == 0);

        REQUIRE(c4indexer_end(indexer, true, &error));
        FLEncoder_Free(enc);
        c4key_free(key);
    }


    void indexTracksView() {
        FLDictKey nameKey   = FLDictKey_Init(FLSTR("Name"), true);
        auto key = c4key_new();

        C4Error error;
        if (!tracksView) {
            tracksView = c4view_open(db, kC4SliceNull, C4STR("Tracks"),
                                     C4STR("1"), c4db_getConfig(db), &error);
            REQUIRE(tracksView);
        }
        C4View* views[1] = {tracksView};
        C4Indexer *indexer = c4indexer_begin(db, views, 1, &error);
        REQUIRE(indexer);
        auto e = c4indexer_enumerateDocuments(indexer, &error);
        REQUIRE(e);
        while (c4enum_next(e, &error)) {
            auto doc = c4enum_getDocument(e, &error);
            FLDict body = FLValue_AsDict( FLValue_FromTrustedData(doc->selectedRev.body) );
            REQUIRE(body);
            auto name    = FLValue_AsString( FLDict_GetWithKey(body, &nameKey) );

            c4key_reset(key);
            c4key_addString(key, name);

            C4Slice value = kC4SliceNull;
            REQUIRE(c4indexer_emit(indexer, doc, 0, 1, &key, &value, &error));
            c4key_reset(key);

            c4doc_free(doc);
        }
        c4enum_free(e);
        REQUIRE(error.code == 0);

        REQUIRE(c4indexer_end(indexer, true, &error));
        c4key_free(key);
    }


    unsigned queryAllArtists() {
        const bool verbose = false;
        std::vector<std::string> allArtists;
        allArtists.reserve(1200);

        totalContext context = {};
        C4ReduceFunction reduce = {total_accumulate, total_reduce, &context};

        C4QueryOptions options = kC4DefaultQueryOptions;
        options.reduce = &reduce;
        options.groupLevel = 1;
        C4Error error;
        auto query = c4view_query(artistsView, &options, &error);
        while (c4queryenum_next(query, &error)) {
            C4KeyReader key = query->key;
            REQUIRE(c4key_peek(&key) == kC4Array);
            c4key_skipToken(&key);
            C4SliceResult artistSlice = c4key_readString(&key);
            REQUIRE(artistSlice.buf);
            std::string artist((const char*)artistSlice.buf, artistSlice.size);
            if (verbose) std::cerr << artist << "  ";
            allArtists.push_back(artist);
            c4slice_free(artistSlice);
        }
        c4queryenum_free(query);
        if (verbose) std::cerr << "\n";
        return (unsigned) allArtists.size();
    }


    C4View *artistsView {nullptr};
    C4View *albumsView {nullptr};
    C4View *tracksView {nullptr};
};


N_WAY_TEST_CASE_METHOD(PerfTest, "Performance", "[Perf][C]") {
    auto jsonData = readFile(kJSONFilePath);
    FLError error;
    FLSliceResult fleeceData = FLData_ConvertJSON({jsonData.buf, jsonData.size}, &error);
    FLArray root = FLValue_AsArray(FLValue_FromTrustedData((C4Slice)fleeceData));
    unsigned numDocs;

    {
        Stopwatch st;
        numDocs = insertDocs(root);
        CHECK(numDocs == 12188);
#ifdef NDEBUG
        double ms = st.elapsedMS();
        fprintf(stderr, "Writing %u docs took %.3f ms (%.3f us/doc, or %.0f docs/sec)\n",
                numDocs, ms, ms/numDocs*1000.0, numDocs/ms*1000);
#endif
    }
    FLSliceResult_Free(fleeceData);
    {
        Stopwatch st;
        indexViews();
#ifdef NDEBUG
        double ms = st.elapsedMS();
        fprintf(stderr, "Indexing Artist/Album views took %.3f ms (%.3f us/doc, or %.0f docs/sec)\n",
                ms, ms/numDocs*1000.0, numDocs/ms*1000);
#endif
    }
    {
        Stopwatch st;
        indexTracksView();
#ifdef NDEBUG
        double ms = st.elapsedMS();
        fprintf(stderr, "Indexing Tracks view took %.3f ms (%.3f us/doc, or %.0f docs/sec)\n",
                ms, ms/numDocs*1000.0, numDocs/ms*1000);
#endif
    }
    {
        Stopwatch st;
        auto numArtists = queryAllArtists();
        CHECK(numArtists == 1141);
#ifdef NDEBUG
        double ms = st.elapsedMS();
        fprintf(stderr, "All-artists query, %u artists, took %.3f ms (%.3f us/doc, or %.0f docs/sec) (%.3f us/artist, or %.0f artists/sec)\n",
                numArtists, ms,
                ms/numDocs*1000.0, numDocs/ms*1000,
                ms/numArtists*1000.0, numArtists/ms*1000);
#endif
    }
}
