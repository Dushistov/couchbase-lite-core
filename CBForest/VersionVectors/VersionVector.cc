//
//  VersionVector.cc
//  CBForest
//
//  Created by Jens Alfke on 5/23/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "VersionVector.hh"
#include "SecureDigest.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "varint.hh"
#include <sstream>
#include <unordered_map>


namespace cbforest {


#pragma mark - VERSION:


    const peerID kCASServerPeerID = {"$", 1};
    const peerID kMePeerID        = {"*", 1};

    version::version(slice string, bool validateAuthor) {
        if (string[0] == '^') {
            gen = 0;
            author = string;
            author.moveStart(1);
            if (validateAuthor)
                validate();
        } else {
            gen = string.readDecimal();                             // read generation
            if (gen == 0 || string.readByte() != '@'                // read '@'
                         || string.size < 1 || string.size > kMaxAuthorSize)
                error::_throw(error::BadVersionVector);
            if (validateAuthor)
                if (author.findByte(',') || author.findByte('\0'))
                    error::_throw(error::BadVersionVector);
            author = string;                                        // read peer ID
        }
    }

    void version::validate() const {
        if (author.size < 1 || author.size > kMaxAuthorSize)
            error::_throw(error::BadVersionVector);
        if (isMerge()) {
            for (size_t i = 0; i < author.size; i++) {
                char c = author[i];
                if (!isalpha(c) && !isnumber(c) && c != '+' && c != '/' && c != '=')
                    error::_throw(error::BadVersionVector);
            }
        } else {
            if (author.findByte(',') || author.findByte('\0'))
                error::_throw(error::BadVersionVector);
        }
    }

    generation version::CAS() const {
        return author == kCASServerPeerID ? gen : 0;
    }

    alloc_slice version::asString() const {
        if (isMerge()) {
            char buf[2 + author.size];
            return alloc_slice(buf, sprintf(buf, "^%.*s", (int)author.size, author.buf));
        } else {
            char buf[30 + author.size];
            return alloc_slice(buf, sprintf(buf, "%llu@%.*s", gen, (int)author.size, author.buf));
        }
    }

    std::ostream& operator << (std::ostream &out, const version &v) {
        if (v.isMerge())
            out << "^";
        else
            out << v.gen << "@";
        out.write((const char*)v.author.buf, v.author.size);
        return out;
    }

    versionOrder version::compareGen(generation a, generation b) {
        if (a > b)
            return kNewer;
        else if (a < b)
            return kOlder;
        return kSame;
    }

    versionOrder version::compareTo(const VersionVector &vv) const {
        versionOrder o = vv.compareTo(*this);
        if (o == kOlder)
            return kNewer;
        else if (o == kNewer)
            return kOlder;
        else
            return o;
    }


#pragma mark - LIFECYCLE:


    VersionVector::VersionVector(slice string)
    :_string(string)
    {
        if (string.size == 0 || string.findByte('\0'))
            error::_throw(error::BadVersionVector);

        while (string.size > 0) {
            const void *comma = string.findByte(',') ?: string.end();
            _vers.push_back( version(string.upTo(comma), false) );
            string = string.from(comma);
            if (string.size > 0)
                string.moveStart(1); // skip comma
        }
    }

    VersionVector::VersionVector(const fleece::Value* val) {
        readFrom(val);
    }

    VersionVector::VersionVector(const VersionVector &v) {
        *this = v;
    }

    VersionVector::VersionVector(VersionVector &&oldv)
    :_string(std::move(oldv._string)),
     _vers(std::move(oldv._vers)),
     _addedAuthors(std::move(oldv._addedAuthors)),
     _changed(oldv._changed)
    { }

    void VersionVector::reset() {
        _string = slice::null;
        _vers.clear();
        _addedAuthors.clear();
        _changed = false;
    }

    VersionVector& VersionVector::operator=(const VersionVector &v) {
        reset();
        for (auto vers : v._vers)
            append(vers);
        return *this;
    }


#pragma mark - CONVERSION:


    void VersionVector::readFrom(const fleece::Value *val) {
        reset();
        auto *arr = val->asArray();
        if (!arr)
            error::_throw(error::BadVersionVector);
        fleece::Array::iterator i(arr);
        if (i.count() % 2 != 0)
            error::_throw(error::BadVersionVector);
        for (; i; i += 2)
            _vers.push_back( version((generation)i[0]->asUnsigned(), i[1]->asString()) );
    }


    std::string VersionVector::asString() const {
        if (!_changed && _string.buf)
            return (std::string)_string;
        return exportAsString(kMePeerID);   // leaves "*" unchanged
    }


    std::string VersionVector::exportAsString(peerID myID) const {
        std::stringstream out;
        for (auto v = _vers.begin(); v != _vers.end(); ++v) {
            if (v != _vers.begin())
                out << ",";
            if (v->author == kMePeerID)
                out << version(v->gen, myID);
            else
                out << *v;
        }
        return out.str();
    }


    std::string VersionVector::canonicalString(peerID myPeerID) const {
        auto vec = *this; // copy before sorting
        vec.expandMyPeerID(myPeerID);
        std::sort(vec._vers.begin(), vec._vers.end(),
                  [myPeerID](const version &a, const version &b) {
                      return a.author < b.author;
                  });
        vec._changed = true;
        return vec.asString();
    }
    
    
    void VersionVector::writeTo(fleece::Encoder &encoder) const {
        encoder.beginArray();
        for (auto v : _vers) {
            encoder << v.gen;
            encoder << v.author;
        }
        encoder.endArray();
    }


#pragma mark - OPERATIONS:


    versionOrder VersionVector::compareTo(const version& v) const {
        auto mine = const_cast<VersionVector*>(this)->findPeerIter(v.author);
        if (mine == _vers.end())
            return kOlder;
        else if (mine->gen < v.gen)
            return kOlder;
        else if (mine->gen == v.gen && mine == _vers.begin())
            return kSame;
        else
            return kNewer;
    }


    versionOrder VersionVector::compareTo(const VersionVector &other) const {
        int o = kSame;
        ssize_t countDiff = count() - other.count();
        if (countDiff < 0)
            o = kOlder;
        else if (countDiff > 0)
            o = kNewer;

        for (auto &v : _vers) {
            auto othergen = other[v.author];
            if (v.gen < othergen)
                o |= kOlder;
            else if (v.gen > othergen)
                o |= kNewer;
            else if (o == kSame)
                break; // first revs are identical so vectors are equal
            if (o == kConflicting)
                break;
        }
        return (versionOrder)o;
    }

    std::vector<version>::iterator VersionVector::findPeerIter(peerID author) {
        auto v = _vers.begin();
        for (; v != _vers.end(); ++v) {
            if (v->author == author)
                break;
        }
        return v;
    }

    generation VersionVector::genOfAuthor(peerID author) const {
        auto v = const_cast<VersionVector*>(this)->findPeerIter(author);
        return (v != _vers.end()) ? v->gen : 0;
    }

    void VersionVector::incrementGen(peerID author) {
        auto versP = findPeerIter(author);
        version v;
        if (versP != _vers.end()) {
            v = *versP;
            if (v.isMerge())
                error::_throw(error::BadVersionVector);
            v.gen++;
            _vers.erase(versP);
        } else {
            v.gen = 1;
            v.author = copyAuthor(author);
            v.validate();
        }
        _vers.insert(_vers.begin(), v);
        _changed = true;
    }

#pragma mark - MODIFICATION:


    void VersionVector::append(version vers) {
        vers.validate();
        vers.author = copyAuthor(vers.author);
        _vers.push_back(vers);
        _changed = true;
    }

    alloc_slice VersionVector::copyAuthor(peerID author) {
        return *_addedAuthors.emplace(_addedAuthors.begin(), author);
    }

    void VersionVector::compactMyPeerID(peerID myID) {
        auto versP = findPeerIter(myID);
        if (versP != _vers.end()) {
            versP->author = kMePeerID;
            _changed = true;
        }
    }

    void VersionVector::expandMyPeerID(peerID myID) {
        auto versP = findPeerIter(kMePeerID);
        if (versP != _vers.end()) {
            versP->author = copyAuthor(myID);
            _changed = true;
        }
    }


#pragma mark - MERGING:


    // A hash table mapping peerID->generation, as an optimization for versionVector operations
    class versionMap {
    public:
        versionMap(const VersionVector &vec) {
            _map.reserve(vec.count());
            for (auto &v : vec._vers)
                add(v);
        }

        void add(const version &vers) {
            _map[vers.author] = vers.gen;
        }

        generation operator[] (peerID author) {
            auto i = _map.find(author);
            return (i == _map.end()) ? 0 : i->second;
        }

    private:
        std::unordered_map<peerID, generation, fleece::sliceHash> _map;
    };
    
    
    VersionVector VersionVector::mergedWith(const VersionVector &other) const {
        // Walk through the two vectors in parallel, adding the current component from each if it's
        // newer than the corresponding component in the other. This isn't going to produce the
        // optimal ordering, but it should be pretty close.
        versionMap myMap(*this), otherMap(other);
        VersionVector result;
        for (size_t i = 0; i < std::max(_vers.size(), other._vers.size()); ++i) {
            peerID author;
            if (i < _vers.size()) {
                auto &vers = _vers[i];
                auto othergen = otherMap[vers.author];
                if (vers.gen >= othergen)
                    result.append(vers);
            }
            if (i < other._vers.size()) {
                auto &vers = other._vers[i];
                auto mygen = myMap[vers.author];
                if (vers.gen > mygen)
                    result.append(vers);
            }
        }
        return result;
    }


    void VersionVector::insertMergeRevID(peerID myPeerID, slice revisionBody) {
        // SHA-1 digest of version vector + nul byte + revision body
        sha1Context ctx;
        sha1_begin(&ctx);
        auto versString = canonicalString(myPeerID);
        sha1_add(&ctx, versString.data(), versString.size());
        sha1_add(&ctx ,"\0", 1);
        sha1_add(&ctx, revisionBody.buf, revisionBody.size);
        char digest[20];
        sha1_end(&ctx, digest);

        // Convert to base64:
        fleece::Writer w;
        w.writeBase64(slice(&digest, sizeof(digest)));

        // Prepend a version representing the merge:
        version mergeVers(0, copyAuthor(w.extractOutput()));
        _vers.insert(_vers.begin(), mergeVers);
        _changed = true;
    }


}
