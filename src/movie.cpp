#include <iostream>
#include <stdexcept>
#include <boost/format.hpp>

#include "chunk.h"
#include "lingo.h"
#include "movie.h"
#include "stream.h"
#include "subchunk.h"
#include "util.h"

namespace ProjectorRays {

/* Movie */

void Movie::read(ReadStream *s) {
    stream = s;
    stream->endianness = kBigEndian; // we set this properly when we create the RIFX chunk

    // Meta
    auto metaFourCC = stream->readUint32();
    if (metaFourCC == FOURCC('X', 'F', 'I', 'R')) {
        stream->endianness = kLittleEndian;
    }
    stream->readInt32(); // meta length
    codec = stream->readUint32();

    // Codec-dependent map
    if (codec == FOURCC('M', 'V', '9', '3')) {
        readMemoryMap();
    } else if (codec == FOURCC('F', 'G', 'D', 'M')) {
        afterburned = true;
        if (!readAfterburnerMap())
            return;
    } else {
        throw std::runtime_error("Codec unsupported: " + fourCCToString(codec));
    }

    if (!readKeyTable())
        return;
    if (!readConfig())
        return;
    if (!readCasts())
        return;
}

void Movie::readMemoryMap() {
    // Initial map
    std::shared_ptr<InitialMapChunk> imap = std::static_pointer_cast<InitialMapChunk>(readChunk(FOURCC('i', 'm', 'a', 'p')));

    // Memory map
    stream->seek(imap->memoryMapOffset);
    std::shared_ptr<MemoryMapChunk> mmap = std::static_pointer_cast<MemoryMapChunk>(readChunk(FOURCC('m', 'm', 'a', 'p')));

    for (uint32_t i = 0; i < mmap->mapArray.size(); i++) {
        auto mapEntry = mmap->mapArray[i];

        if (mapEntry.fourCC == FOURCC('f', 'r', 'e', 'e') || mapEntry.fourCC == FOURCC('j', 'u', 'n', 'k'))
            continue;
        
        ChunkInfo info;
        info.id = i;
        info.fourCC = mapEntry.fourCC;
        info.len = mapEntry.len;
        info.uncompressedLen = mapEntry.len;
        info.offset = mapEntry.offset;
        info.compressionType = 0;
        chunkInfo[i] = info;

        chunkIDsByFourCC[mapEntry.fourCC].push_back(i);
    }
}

bool Movie::readAfterburnerMap() {
    uint32_t start, end;

    // File version
    if (stream->readUint32() != FOURCC('F', 'v', 'e', 'r')) {
        std::cout << "readAfterburnerMap(): Fver expected but not found\n";
        return false;
    }

    uint32_t fverLength = stream->readVarInt();
    start = stream->pos();
    uint32_t version = stream->readVarInt();
    std::cout << boost::format("Fver: version: %x\n") % version;
    end = stream->pos();

    if (end - start != fverLength) {
        std::cout << boost::format("readAfterburnerMap(): Expected Fver of length %d but read %d bytes\n")
                        % fverLength % (end - start);
        stream->seek(start + fverLength);
    }

    // Compression types
    if (stream->readUint32() != FOURCC('F', 'c', 'd', 'r')) {
        std::cout << "readAfterburnerMap(): Fcdr expected but not found\n";
        return false;
    }

    uint32_t fcdrLength = stream->readVarInt();
    stream->skip(fcdrLength);

    // Afterburner map
    if (stream->readUint32() != FOURCC('A', 'B', 'M', 'P')) {
        std::cout << "RIFXArchive::readAfterburnerMap(): ABMP expected but not found\n";
        return false;
    }
    uint32_t abmpLength = stream->readVarInt();
    uint32_t abmpEnd = stream->pos() + abmpLength;
    uint32_t abmpCompressionType = stream->readVarInt();
    unsigned long abmpUncompLength = stream->readVarInt();
    unsigned long abmpActualUncompLength = abmpUncompLength;
    std::cout << boost::format("ABMP: length: %d compressionType: %d uncompressedLength: %lu\n")
                    % abmpLength % abmpCompressionType % abmpUncompLength;

    auto abmpStream = stream->readZlibBytes(abmpEnd - stream->pos(), &abmpActualUncompLength);
    if (!abmpStream) {
        std::cout << "RIFXArchive::readAfterburnerMap(): Could not uncompress ABMP\n";
        return false;
    }
    if (abmpUncompLength != abmpActualUncompLength) {
        std::cout << boost::format("ABMP: Expected uncompressed length %lu but got length %lu\n")
                        % abmpUncompLength % abmpActualUncompLength;
    }

    uint32_t abmpUnk1 = abmpStream->readVarInt();
    uint32_t abmpUnk2 = abmpStream->readVarInt();
    uint32_t resCount = abmpStream->readVarInt();
    std::cout << boost::format("ABMP: unk1: %d unk2: %d resCount: %d\n")
                    % abmpUnk1 % abmpUnk2 % resCount;

    for (uint32_t i = 0; i < resCount; i++) {
        uint32_t resId = abmpStream->readVarInt();
        int32_t offset = abmpStream->readVarInt();
        uint32_t compSize = abmpStream->readVarInt();
        uint32_t uncompSize = abmpStream->readVarInt();
        uint32_t compressionType = abmpStream->readVarInt();
        uint32_t tag = abmpStream->readUint32();

        std::cout << boost::format("Found RIFX resource index %d: '%s', %d bytes (%d uncompressed) @ pos 0x%08x (%d), compressionType: %d\n")
                        % resId % fourCCToString(tag) % compSize % uncompSize % offset % offset % compressionType;

        ChunkInfo info;
        info.id = resId;
        info.fourCC = tag;
        info.len = compSize;
        info.uncompressedLen = uncompSize;
        info.offset = offset;
        info.compressionType = compressionType;
        chunkInfo[resId] = info;

        chunkIDsByFourCC[tag].push_back(resId);
    }

    // Initial load segment
    if (chunkInfo.find(2) == chunkInfo.end()) {
        std::cout << "readAfterburnerMap(): Map has no entry for ILS\n";
        return false;
    }
    if (stream->readUint32() != FOURCC('F', 'G', 'E', 'I')) {
        std::cout << "readAfterburnerMap(): FGEI expected but not found\n";
        return false;
    }

    ChunkInfo &ilsInfo = chunkInfo[2];
    uint32_t ilsUnk1 = stream->readVarInt();
    std::cout << boost::format("ILS: length: %d unk1: %d\n") % ilsInfo.len % ilsUnk1;
    _ilsBodyOffset = stream->pos();
    unsigned long ilsActualUncompLength = ilsInfo.uncompressedLen;
    auto ilsStream = stream->readZlibBytes(ilsInfo.len, &ilsActualUncompLength);
    if (!ilsStream) {
        std::cout << "readAfterburnerMap(): Could not uncompress FGEI\n";
        return false;
    }
    if (ilsInfo.uncompressedLen != ilsActualUncompLength) {
        std::cout << boost::format("ILS: Expected uncompressed length %d but got length %lu\n")
                        % ilsInfo.uncompressedLen % ilsActualUncompLength;
    }

    while (!ilsStream->eof()) {
        uint32_t resId = ilsStream->readVarInt();
        ChunkInfo &info = chunkInfo[resId];

        std::cout << boost::format("Loading ILS resource %d: '%s', %d bytes\n")
                        % resId % fourCCToString(info.fourCC) % info.len;

        auto data = ilsStream->copyBytes(info.len);
        if (data) {
            _cachedChunkData[resId] = std::move(data);
        } else {
            std::cout << boost::format("Could not load ILS resource %d\n") % resId;
        }
    }

    return true;
}

bool Movie::readKeyTable() {
    auto info = getFirstChunkInfo(FOURCC('K', 'E', 'Y', '*'));
    if (info) {
        keyTable = std::static_pointer_cast<KeyTableChunk>(getChunk(info->fourCC, info->id));
        return true;
    }

    std::cout << "No key chunk!\n";
    return false;
}

bool Movie::readConfig() {
    auto info = getFirstChunkInfo(FOURCC('V', 'W', 'C', 'F'));
    if (!info)
        info = getFirstChunkInfo(FOURCC('D', 'R', 'C', 'F'));

    if (info) {
        config = std::static_pointer_cast<ConfigChunk>(getChunk(info->fourCC, info->id));
        version = humanVersion(config->directorVersion);
        std::cout << "Director version: " + std::to_string(version) + "\n";

        return true;
    }

    std::cout << "No config chunk!\n";
    return false;
}

bool Movie::readCasts() {
    if (version >= 500) {
        auto info = getFirstChunkInfo(FOURCC('M', 'C', 's', 'L'));
        if (info) {
            auto castList = std::static_pointer_cast<CastListChunk>(getChunk(info->fourCC, info->id));
            for (const auto &castEntry : castList->entries) {
                std::cout << "Cast: " + castEntry.name + "\n";
                int32_t sectionID = -1;
                for (const auto &keyEntry : keyTable->entries) {
                    if (keyEntry.castID == castEntry.id && keyEntry.fourCC == FOURCC('C', 'A', 'S', '*')) {
                        sectionID = keyEntry.sectionID;
                        break;
                    }
                }
                if (sectionID > 0) {
                    auto cast = std::static_pointer_cast<CastChunk>(getChunk(FOURCC('C', 'A', 'S', '*'), sectionID));
                    cast->populate(castEntry.name, castEntry.id, castEntry.minMember);
                    casts.push_back(std::move(cast));
                }
            }

            return true;
        }

        std::cout << "No cast list!\n";
        return false;
    } else {
        auto info = getFirstChunkInfo(FOURCC('C', 'A', 'S', '*'));
        if (info) {
            auto cast = std::static_pointer_cast<CastChunk>(getChunk(info->fourCC, info->id));
            cast->populate("Internal", 1024, config->minMember);
            casts.push_back(std::move(cast));

            return true;
        }

        std::cout << "No cast!\n";
        return false;
    }

    return false;
}

const ChunkInfo *Movie::getFirstChunkInfo(uint32_t fourCC) {
    auto &chunkIDs = chunkIDsByFourCC[fourCC];
    if (chunkIDs.size() > 0) {
        return &chunkInfo[chunkIDs[0]];
    }
    return nullptr;
}

std::shared_ptr<Chunk> Movie::getChunk(uint32_t fourCC, int32_t id) {
    if (deserializedChunks.find(id) != deserializedChunks.end())
        return deserializedChunks[id];

    if (chunkInfo.find(id) == chunkInfo.end())
        throw std::runtime_error("Could not find chunk " + std::to_string(id));

    auto &info = chunkInfo[id];
    if (fourCC != info.fourCC) {
        throw std::runtime_error(
            "Expected chunk " + std::to_string(id) + " to be '" + fourCCToString(fourCC)
            + "', but is actually '" + fourCCToString(info.fourCC) + "'"
        );
    }

    std::shared_ptr<Chunk> chunk;
    if (_cachedChunkData.find(id) != _cachedChunkData.end()) {
        auto &data = _cachedChunkData[id];
        auto chunkStream = std::make_unique<ReadStream>(data, stream->endianness, 0, data->size());
        chunk = makeChunk(fourCC, *chunkStream);
    } else if (afterburned) {
        stream->seek(info.offset + _ilsBodyOffset);
        unsigned long actualUncompLength = info.uncompressedLen;
        auto chunkStream = stream->readZlibBytes(info.len, &actualUncompLength);
        if (!chunkStream) {
            throw std::runtime_error(boost::str(
                boost::format("Could not uncompress chunk %d") % id
            ));
        }
        if (info.uncompressedLen != actualUncompLength) {
            throw std::runtime_error(boost::str(
                boost::format("Chunk %d: Expected uncompressed length %d but got length %lu")
                    % id % info.uncompressedLen % actualUncompLength
            ));
        }
        chunk = makeChunk(fourCC, *chunkStream);
    } else {
        stream->seek(info.offset);
        chunk = readChunk(fourCC, info.len);
    }

    // don't cache the deserialized map chunks
    // we'll just generate a new one if we need to save
    if (fourCC != FOURCC('i', 'm', 'a', 'p') && fourCC != FOURCC('m', 'm', 'a', 'p')) {
        deserializedChunks[id] = chunk;
    }

    return chunk;
}

std::shared_ptr<Chunk> Movie::readChunk(uint32_t fourCC, uint32_t len) {
    auto offset = stream->pos();

    auto validFourCC = stream->readUint32();
    auto validLen = stream->readUint32();

    // use the valid length if mmap hasn't been read yet
    if (len == UINT32_MAX) {
        len = validLen;
    }

    // validate chunk
    if (fourCC != validFourCC || len != validLen) {
        throw std::runtime_error(
            "At offset " + std::to_string(offset)
            + " expected '" + fourCCToString(fourCC) + "' chunk with length " + std::to_string(len)
            + ", but got '" + fourCCToString(validFourCC) + "' chunk with length " + std::to_string(validLen)
        );
    } else {
        std::cout << "At offset " + std::to_string(offset) + " reading chunk '" + fourCCToString(fourCC) + "' with length " + std::to_string(len) + "\n";
    }

    auto chunkStream = stream->readBytes(len);
    return makeChunk(fourCC, *chunkStream);
}

std::shared_ptr<Chunk> Movie::makeChunk(uint32_t fourCC, ReadStream &stream) {
    std::shared_ptr<Chunk> res;
    switch (fourCC) {
    case FOURCC('i', 'm', 'a', 'p'):
        res = std::make_shared<InitialMapChunk>(this);
        break;
    case FOURCC('m', 'm', 'a', 'p'):
        res = std::make_shared<MemoryMapChunk>(this);
        break;
    case FOURCC('C', 'A', 'S', '*'):
        res = std::make_shared<CastChunk>(this);
        break;
    case FOURCC('C', 'A', 'S', 't'):
        res = std::make_shared<CastMemberChunk>(this);
        break;
    case FOURCC('K', 'E', 'Y', '*'):
        res = std::make_shared<KeyTableChunk>(this);
        break;
    case FOURCC('L', 'c', 't', 'X'):
        capitalX = true;
        // fall through
    case FOURCC('L', 'c', 't', 'x'):
        res = std::make_shared<ScriptContextChunk>(this);
        break;
    case FOURCC('L', 'n', 'a', 'm'):
        res = std::make_shared<ScriptNamesChunk>(this);
        break;
    case FOURCC('L', 's', 'c', 'r'):
        res = std::make_shared<ScriptChunk>(this);
        break;
    case FOURCC('V', 'W', 'C', 'F'):
    case FOURCC('D', 'R', 'C', 'F'):
        res = std::make_shared<ConfigChunk>(this);
        break;
    case FOURCC('M', 'C', 's', 'L'):
        res = std::make_shared<CastListChunk>(this);
        break;
    default:
        res = std::make_shared<Chunk>(this);
    }

    res->read(stream);

    return res;
}

}
