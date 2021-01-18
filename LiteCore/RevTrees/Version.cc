//
// Version.cc
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

#include "Version.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "varint.hh"
#include <algorithm>


namespace litecore {
    using namespace std;
    using namespace fleece;


    // Utility that allocates a buffer, lets the callback write into it, then trims the buffer.
    static inline alloc_slice writeAlloced(size_t maxSize, function_ref<bool(slice*)> writer) {
        alloc_slice buf(maxSize);
        slice out = buf;
        Assert( writer(&out) );
        buf.shorten(buf.size - out.size);
        return buf;
    }


#pragma mark - VERSION:


    Version::Version(slice ascii, peerID myPeerID) {
        slice in = ascii;
        _gen = in.readHex();
        if (in.readByte() != '@' || _gen == 0)
            throwBadASCII(ascii);
        if (in == "*"_sl) {
#if 0
            if (myPeerID != kMePeerID) {
                // If I'm given an explicit peer ID for me, then '*' is not valid; the string
                // is expected to contain that explicit ID instead.
                error::_throw(error::BadRevisionID,
                              "A '*' is not valid in this version string: '%.*s'", SPLAT(ascii));
            }
#endif
            _author = kMePeerID;
        } else {
            _author.id = in.readHex();
            if (in.size > 0 || _author == kMePeerID)
                throwBadASCII(ascii);
            if (_author == myPeerID)
                _author = kMePeerID;    // Abbreviate my ID
        }
    }

    
    Version::Version(slice *dataP) {
        if (!ReadUVarInt(dataP, &_gen) || !ReadUVarInt(dataP, &_author.id))
            throwBadBinary();
        validate();
    }


    void Version::validate() const {
        if (_gen == 0)
            error::_throw(error::BadRevisionID);
    }


    bool Version::writeBinary(slice *out, peerID myID) const {
        uint64_t id = (_author == kMePeerID) ? myID.id : _author.id;
        return WriteUVarInt(out, _gen) && WriteUVarInt(out, id);
    }


    bool Version::writeASCII(slice *out, peerID myID) const {
        if (!out->writeHex(_gen) || !out->writeByte('@'))
            return false;
        auto author = (_author != kMePeerID) ? _author : myID;
        if (author != kMePeerID)
            return out->writeHex(author.id);
        else
            return out->writeByte('*');
    }


    alloc_slice Version::asASCII(peerID myID) const {
        return writeAlloced(kMaxASCIILength, [&](slice *out) {
            return writeASCII(out, myID);
        });
    }


    versionOrder Version::compareGen(generation a, generation b) {
        if (a > b)
            return kNewer;
        else if (a < b)
            return kOlder;
        return kSame;
    }


    void Version::throwBadBinary() {
        error::_throw(error::BadRevisionID, "Invalid binary version ID");
    }


    void Version::throwBadASCII(slice string) {
        if (string)
            error::_throw(error::BadRevisionID, "Invalid version string '%.*s'", SPLAT(string));
        else
            error::_throw(error::BadRevisionID, "Invalid version string");
    }


}
