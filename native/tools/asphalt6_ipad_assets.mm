// Read iPad assets without loading any code from the IPA. The Mac game keeps
// its own executable, resource index, meshes, shaders, UI and save formats.
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <zlib.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "../third_party/powervr/PVRTDecompress.h"
#define STB_DXT_IMPLEMENTATION
#include "../third_party/stb/stb_dxt.h"

using Bytes = std::vector<uint8_t>;
static void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
static uint16_t u16(const uint8_t* p) { return p[0] | uint16_t(p[1]) << 8; }
static uint32_t u32(const uint8_t* p) { return u16(p) | uint32_t(u16(p + 2)) << 16; }
static void put16(Bytes& b, uint16_t v) { b.push_back(v); b.push_back(v >> 8); }
static void put32(Bytes& b, uint32_t v) { put16(b, v); put16(b, v >> 16); }
static void set32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.at(at + i) = v >> (i * 8);
}
static NSString* ns(const std::string& s) {
    NSString* value = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    require(value != nil, "An archive filename is not UTF-8.");
    return value;
}
static std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
static NSString* hash(const uint8_t* bytes, size_t count) {
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    for (size_t at = 0; at < count;) {
        size_t n = std::min(count - at, size_t(1024 * 1024));
        CC_SHA256_Update(&context, bytes + at, CC_LONG(n));
        at += n;
    }
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    NSMutableString* value = [NSMutableString string];
    for (uint8_t b : digest) [value appendFormat:@"%02x", b];
    return value;
}
static NSString* hash(const Bytes& b) { return hash(b.data(), b.size()); }

struct Entry {
    std::string name;
    uint16_t method, flags, time, date;
    uint32_t crc, packed, size, offset;
};

// Gameloft's packs retain ZIP central directories, but replace the first
// PK0304 local header with QM0608. Never extract names onto the filesystem.
struct Archive {
    NSData* data;
    std::vector<Entry> entries;
    explicit Archive(NSData* bytes) : data(bytes) {
        require(data.length >= 22, "Truncated ZIP archive.");
        const uint8_t* p = (const uint8_t*)data.bytes;
        size_t end = data.length - 22, floor = data.length > 65557 ? data.length - 65557 : 0;
        while (true) {
            if (u32(p + end) == 0x06054b50 && end + 22 + u16(p + end + 20) == data.length) break;
            require(end != floor, "Missing ZIP directory.");
            --end;
        }
        uint16_t count = u16(p + end + 10);
        size_t at = u32(p + end + 16), length = u32(p + end + 12);
        require(!u16(p + end + 4) && !u16(p + end + 6) && count == u16(p + end + 8)
                && count < 10000 && at <= end && length == end - at,
                "Unsupported ZIP directory or ZIP64 archive.");
        const size_t limit = end;
        std::set<std::string> names;
        for (unsigned i = 0; i < count; ++i) {
            require(at <= limit && limit - at >= 46 && u32(p + at) == 0x02014b50, "Invalid ZIP directory entry.");
            size_t n = u16(p + at + 28), extra = u16(p + at + 30), comment = u16(p + at + 32);
            require(n && 46 + n + extra + comment <= limit - at, "Invalid ZIP filename.");
            std::string name((const char*)p + at + 46, n);
            require(name.find('\0') == std::string::npos && names.insert(name).second, "Duplicate or invalid ZIP filename.");
            entries.push_back({name, u16(p + at + 10), u16(p + at + 8), u16(p + at + 12), u16(p + at + 14),
                               u32(p + at + 16), u32(p + at + 20), u32(p + at + 24), u32(p + at + 42)});
            const Entry& e = entries.back();
            require(!(e.flags & 1) && (e.method == 0 || e.method == 8) && !u16(p + at + 34), "Encrypted or unsupported ZIP entry.");
            size_t o = e.offset;
            require(o <= at && at - o >= 30 && (u32(p + o) == 0x04034b50 || u32(p + o) == 0x08064d51),
                    "Invalid ZIP local header.");
            size_t header = 30 + u16(p + o + 26) + u16(p + o + 28);
            require(header <= at - o && e.packed <= at - o - header
                    && u16(p + o + 8) == e.method && u16(p + o + 6) == e.flags
                    && u16(p + o + 26) == n && !memcmp(p + o + 30, name.data(), n), "Inconsistent ZIP local entry.");
            at += 46 + n + extra + comment;
        }
        require(at == end, "Unexpected ZIP directory data.");
    }
    const uint8_t* compressed(const Entry& e) const {
        const uint8_t* p = (const uint8_t*)data.bytes + e.offset;
        return p + 30 + u16(p + 26) + u16(p + 28);
    }
    Bytes read(const Entry& e) const {
        require(e.size <= 512 * 1024 * 1024, "An asset exceeds the supported size.");
        Bytes out(std::max(e.size, 1u));
        if (e.method == 0) {
            require(e.size == e.packed, "Invalid stored ZIP size.");
            memcpy(out.data(), compressed(e), e.size);
        } else {
            z_stream stream{};
            stream.next_in = (Bytef*)compressed(e);
            stream.avail_in = e.packed;
            stream.next_out = out.data();
            stream.avail_out = unsigned(out.size());
            require(inflateInit2(&stream, -15) == Z_OK, "Could not initialize ZIP decompression.");
            int result = inflate(&stream, Z_FINISH);
            bool valid = result == Z_STREAM_END && stream.total_out == e.size && stream.total_in == e.packed;
            inflateEnd(&stream);
            require(valid, "Corrupt compressed asset: " + e.name);
        }
        out.resize(e.size);
        require(uint32_t(crc32(0, out.data(), unsigned(out.size()))) == e.crc, "Asset CRC mismatch: " + e.name);
        return out;
    }
    const Entry* find(const std::string& name) const {
        for (const auto& e : entries) if (e.name == name) return &e;
        return nullptr;
    }
};

struct Texture {
    uint32_t width = 0, height = 0, levels = 0, bpp = 0;
    bool pvr = false;
};
static Texture texture(const Bytes& b) {
    if (b.size() >= 128 && !memcmp(b.data(), "DDS ", 4) && u32(b.data() + 4) == 124) {
        return {u32(b.data() + 16), u32(b.data() + 12), std::max(1u, u32(b.data() + 28)), 0, false};
    }
    if (b.size() >= 52 && u32(b.data()) == 52 && !memcmp(b.data() + 44, "PVR!", 4)) {
        uint32_t format = u32(b.data() + 16) & 255;
        if (format == 24 || format == 25)
            return {u32(b.data() + 8), u32(b.data() + 4), u32(b.data() + 12) + 1, format == 24 ? 2u : 4u, true};
    }
    return {};
}
static std::string key(std::string name, bool retina) {
    name = lower(name);
    for (const std::string token : {".pvrtc2", ".pvrtc4"}) {
        size_t at = name.find(token + ".");
        if (at != std::string::npos) name.erase(at, token.size());
    }
    if (retina) {
        size_t at = name.find("_x2.");
        if (at != std::string::npos) name.erase(at, 3);
    }
    // Keep _IPAD2 and NOMIPMAP distinct: those may have different layouts or
    // sampling rules. A matching base name plus _x2 is the Retina alternative.
    return name;
}
static bool powerOfTwo(uint32_t v) { return v && !(v & (v - 1)); }

// Re-encode each supplied mip, without resizing or generating image detail.
// Explicit DXT3 alpha matches the original Mac pack and avoids a new loader path.
static Bytes dds(const Bytes& pvr, Texture t, bool mipmaps) {
    require(t.pvr && powerOfTwo(t.width) && powerOfTwo(t.height) && t.width <= 4096 && t.height <= 4096
            && t.levels >= 1 && t.levels <= 13 && u32(pvr.data() + 48) == 1 && !(u32(pvr.data() + 16) & 0x10000),
            "Unsupported PVRTC texture geometry, surfaces or orientation.");
    uint32_t fullLevels = 1;
    for (uint32_t v = std::max(t.width, t.height); v > 1; v >>= 1) ++fullLevels;
    require(!mipmaps || t.levels == fullLevels, "The iPad texture has an incomplete mip chain.");
    require(u32(pvr.data() + 20) == pvr.size() - 52, "The PVRTC payload size is inconsistent.");
    const uint32_t levels = mipmaps ? fullLevels : 1;
    Bytes result(128, 0);
    memcpy(result.data(), "DDS ", 4);
    set32(result, 4, 124);
    set32(result, 8, 0x81007 | (levels > 1 ? 0x20000 : 0));
    set32(result, 12, t.height); set32(result, 16, t.width);
    set32(result, 20, ((t.width + 3) / 4) * ((t.height + 3) / 4) * 16);
    set32(result, 28, levels); set32(result, 76, 32); set32(result, 80, 4);
    memcpy(result.data() + 84, "DXT3", 4);
    set32(result, 108, 0x1000 | (levels > 1 ? 0x400008 : 0));
    size_t offset = 52;
    uint32_t w = t.width, h = t.height;
    for (uint32_t level = 0; level < levels; ++level) {
        size_t packed = size_t(std::max(w, t.bpp == 2 ? 16u : 8u)) * std::max(h, 8u) * t.bpp / 8;
        require(offset <= pvr.size() && packed <= pvr.size() - offset, "Truncated PVRTC mip level.");
        Bytes pixels(size_t(w) * h * 4);
        pvr::PVRTDecompressPVRTC(pvr.data() + offset, t.bpp == 2, w, h, pixels.data());
        if (!u32(pvr.data() + 40)) for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
        for (uint32_t y = 0; y < h; y += 4) for (uint32_t x = 0; x < w; x += 4) {
            uint8_t block[64], encoded[16]{};
            for (unsigned by = 0; by < 4; ++by) for (unsigned bx = 0; bx < 4; ++bx) {
                unsigned i = by * 4 + bx;
                const uint8_t* pixel = pixels.data() + (size_t(std::min(y + by, h - 1)) * w + std::min(x + bx, w - 1)) * 4;
                memcpy(block + i * 4, pixel, 4);
                encoded[i / 2] |= uint8_t((unsigned(pixel[3]) * 15 + 127) / 255) << ((i & 1) * 4);
            }
            stb_compress_dxt_block(encoded + 8, block, 0, STB_DXT_HIGHQUAL);
            result.insert(result.end(), encoded, encoded + 16);
        }
        offset += packed;
        w = std::max(1u, w / 2); h = std::max(1u, h / 2);
    }
    return result;
}

struct Writer {
    std::ofstream file;
    std::vector<Entry> entries;
    explicit Writer(const char* path) : file(path, std::ios::binary) { require(bool(file), "Cannot create the output track pack."); }
    void write(const uint8_t* p, size_t n) { file.write((const char*)p, n); require(bool(file), "Could not write the track pack."); }
    void write(const Bytes& b) { write(b.data(), b.size()); }
    uint32_t position() {
        auto at = file.tellp();
        require(at >= 0 && uint64_t(at) <= UINT32_MAX, "The track pack exceeds ZIP32 limits.");
        return uint32_t(at);
    }
    void add(const Archive& original, const Entry& old, const Bytes* replacement) {
        Entry e = old;
        e.offset = position(); e.flags &= ~8;
        Bytes compressed;
        const uint8_t* payload = original.compressed(old);
        if (replacement) {
            e.size = uint32_t(replacement->size());
            e.crc = uint32_t(crc32(0, replacement->data(), e.size));
            z_stream stream{};
            require(deflateInit2(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK, "Could not initialize compression.");
            compressed.resize(deflateBound(&stream, e.size));
            stream.next_in = (Bytef*)replacement->data(); stream.avail_in = e.size;
            stream.next_out = compressed.data(); stream.avail_out = unsigned(compressed.size());
            int code = deflate(&stream, Z_FINISH);
            e.packed = uint32_t(stream.total_out); e.method = 8;
            deflateEnd(&stream);
            require(code == Z_STREAM_END, "Could not compress the texture.");
            payload = compressed.data();
        }
        Bytes header;
        put32(header, u32((const uint8_t*)original.data.bytes + old.offset));
        put16(header, 20); put16(header, e.flags); put16(header, e.method);
        put16(header, e.time); put16(header, e.date); put32(header, e.crc); put32(header, e.packed); put32(header, e.size);
        put16(header, uint16_t(e.name.size())); put16(header, 0);
        header.insert(header.end(), e.name.begin(), e.name.end());
        write(header); write(payload, e.packed); entries.push_back(e);
    }
    void finish() {
        uint32_t start = position();
        for (const auto& e : entries) {
            Bytes h;
            put32(h, 0x02014b50); put16(h, 20); put16(h, 20); put16(h, e.flags); put16(h, e.method);
            put16(h, e.time); put16(h, e.date); put32(h, e.crc); put32(h, e.packed); put32(h, e.size);
            put16(h, uint16_t(e.name.size())); put16(h, 0); put16(h, 0); put16(h, 0); put16(h, 0);
            put32(h, 0); put32(h, e.offset); h.insert(h.end(), e.name.begin(), e.name.end()); write(h);
        }
        uint32_t length = position() - start;
        Bytes h; put32(h, 0x06054b50); put16(h, 0); put16(h, 0);
        put16(h, uint16_t(entries.size())); put16(h, uint16_t(entries.size()));
        put32(h, length); put32(h, start); put16(h, 0); write(h);
        file.close(); require(bool(file), "Could not finish the track pack.");
    }
};

static NSData* load(NSString* path) {
    NSError* error = nil;
    NSData* data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:&error];
    require(data != nil, std::string("Could not read ") + path.UTF8String);
    return data;
}
static NSDictionary* describe(const Entry& e, const Bytes& bytes) {
    Texture t = texture(bytes);
    NSMutableDictionary* d = [@{@"name": ns(e.name), @"sha256": hash(bytes), @"bytes": @(bytes.size())} mutableCopy];
    if (t.width) {
        d[@"width"] = @(t.width); d[@"height"] = @(t.height); d[@"mipLevels"] = @(t.levels);
        d[@"format"] = t.pvr ? (t.bpp == 2 ? @"PVRTC1_2bpp" : @"PVRTC1_4bpp") : @"DDS";
    }
    return d;
}

static void process(const char* ipaPath, const char* macPath, const char* output, const char* reportPath) {
    Archive ipa(load(@(ipaPath))), mac(load(@(macPath)));
    const Entry* plist = nullptr;
    for (const auto& e : ipa.entries) {
        if ([ns(e.name) hasPrefix:@"Payload/"] && [ns(e.name) hasSuffix:@".app/Info.plist"]
                && [[ns(e.name) componentsSeparatedByString:@"/"] count] == 3) {
            require(!plist, "The IPA contains more than one app."); plist = &e;
        }
    }
    require(plist, "The IPA is missing its app Info.plist.");
    Bytes infoBytes = ipa.read(*plist);
    id info = [NSPropertyListSerialization propertyListWithData:[NSData dataWithBytes:infoBytes.data() length:infoBytes.size()]
                                                      options:NSPropertyListImmutable format:nullptr error:nullptr];
    require([info isKindOfClass:NSDictionary.class] && [info[@"CFBundleIdentifier"] isEqual:@"com.gameloft.Asphalt6ipad"]
            && [info[@"CFBundleVersion"] isEqual:@"1.5.1"], "Choose an Asphalt 6: Adrenaline HD 1.5.1 iPad IPA.");
    std::string appRoot = plist->name.substr(0, plist->name.size() - strlen("Info.plist"));
    const Entry* pack = ipa.find(appRoot + "data/file000019.dat");
    require(pack, "The iPad 1.5.1 track texture pack is missing.");
    Bytes trackBytes = ipa.read(*pack);
    Archive ipad([NSData dataWithBytes:trackBytes.data() length:trackBytes.size()]);
    NSMutableArray* ipadReport = [NSMutableArray array];
    NSMutableArray* macReport = [NSMutableArray array];
    NSMutableArray* replacements = [NSMutableArray array];
    std::map<std::string, NSDictionary*> ipadHashes;
    std::map<std::string, std::vector<const Entry*>> candidates;
    for (const auto& e : ipad.entries) {
        Bytes bytes = ipad.read(e);
        NSDictionary* d = describe(e, bytes);
        [ipadReport addObject:d]; ipadHashes[e.name] = d;
        if (e.name.rfind("Data/#temp/TrackTga/", 0) == 0 && texture(bytes).pvr)
            candidates[key(e.name, true)].push_back(&e);
    }
    unsigned sameNames = 0, sameBytes = 0, upgraded = 0;
    std::unique_ptr<Writer> writer;
    if (output) writer = std::make_unique<Writer>(output);
    for (const auto& e : mac.entries) {
        Bytes original = mac.read(e), replacement;
        NSMutableDictionary* row = [describe(e, original) mutableCopy];
        auto exact = ipadHashes.find(e.name);
        if (exact != ipadHashes.end()) {
            ++sameNames;
            bool identical = [row[@"sha256"] isEqual:exact->second[@"sha256"]];
            sameBytes += identical;
            row[@"ipadSameNameSHA256"] = exact->second[@"sha256"];
            row[@"byteIdentical"] = @(identical);
        }
        Texture base = texture(original);
        const Entry* chosen = nullptr;
        Texture target;
        // Only replace the existing desktop DXT3 track textures. Never add
        // iPad meshes, UI, material definitions, extra cars or resource indexes.
        if (base.width && !base.pvr && !memcmp(original.data() + 84, "DXT3", 4)) {
            for (const auto* candidate : candidates[key(e.name, false)]) {
                NSDictionary* d = ipadHashes.at(candidate->name);
                Texture t{[d[@"width"] unsignedIntValue], [d[@"height"] unsignedIntValue], [d[@"mipLevels"] unsignedIntValue], 0, true};
                if (t.width > base.width && t.height > base.height && uint64_t(t.width) * base.height == uint64_t(t.height) * base.width
                        && (base.levels == 1 || t.levels > 1) && uint64_t(t.width) * t.height > uint64_t(target.width) * target.height) {
                    chosen = candidate; target = t;
                }
            }
        }
        if (chosen) {
            Bytes pvr = ipad.read(*chosen);
            replacement = dds(pvr, texture(pvr), base.levels > 1);
            NSDictionary* detail = @{@"macName": ns(e.name), @"ipadName": ns(chosen->name),
                @"macWidth": @(base.width), @"macHeight": @(base.height), @"ipadWidth": @(target.width), @"ipadHeight": @(target.height),
                @"macSHA256": row[@"sha256"], @"ipadSHA256": ipadHashes.at(chosen->name)[@"sha256"], @"convertedSHA256": hash(replacement)};
            [replacements addObject:detail]; ++upgraded;
            fprintf(stderr, "iPad textures: %u upgraded, %s\n", upgraded, e.name.c_str());
        }
        if (writer) writer->add(mac, e, chosen ? &replacement : nullptr);
        [macReport addObject:row];
    }
    require(upgraded != 0, "No compatible higher-resolution iPad textures were found for this Mac game.");
    if (writer) writer->finish();
    NSDictionary* report = @{@"schemaVersion": @1, @"ipadBundleIdentifier": info[@"CFBundleIdentifier"], @"ipadVersion": info[@"CFBundleVersion"],
        @"ipadPack": ns(pack->name), @"macPackSHA256": hash((const uint8_t*)mac.data.bytes, mac.data.length),
        @"ipadPackSHA256": hash(trackBytes), @"sameNameTextures": @(sameNames), @"byteIdenticalTextures": @(sameBytes),
        @"upgradedTextures": @(upgraded), @"macTextures": macReport, @"ipadTextures": ipadReport, @"replacements": replacements,
        @"hashMeaning": @"SHA-256 of each decompressed archive entry, including its texture header and compressed texture payload. PVRTC and DDS hashes differ even when the artwork is the same."};
    NSError* error = nil;
    NSData* json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:&error];
    require(json && [json writeToFile:@(reportPath) options:NSDataWritingAtomic error:&error], "Could not write the texture comparison report.");
    fprintf(stderr, "iPad textures: %u upgraded; %u/%u same-name entries are byte-identical.\n", upgraded, sameBytes, sameNames);
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        const char* output = nullptr;
        const char* report = nullptr;
        bool newOutputs = false;
        try {
            bool importing = argc == 6 && !strcmp(argv[1], "--import");
            bool comparing = argc == 5 && !strcmp(argv[1], "--compare");
            require(importing || comparing, "Usage: asphalt6-ipad-assets --import IPA MAC_PACK OUTPUT_PACK REPORT_JSON\n       asphalt6-ipad-assets --compare IPA MAC_PACK REPORT_JSON");
            output = importing ? argv[4] : nullptr;
            report = argv[importing ? 5 : 4];
            NSFileManager* fm = NSFileManager.defaultManager;
            require(![fm fileExistsAtPath:@(report)] && (!output || ![fm fileExistsAtPath:@(output)]), "Choose new output paths; existing files are never replaced.");
            newOutputs = true;
            process(argv[2], argv[3], output, report);
            return 0;
        } catch (const std::exception& e) {
            if (newOutputs) {
                if (output) [NSFileManager.defaultManager removeItemAtPath:@(output) error:nil];
                [NSFileManager.defaultManager removeItemAtPath:@(report) error:nil];
            }
            fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
}
