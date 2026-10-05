#include "udf_listing.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace pulse::preview {
namespace {
// ECMA-167, parts 3/10 and 4/14; UDF 2.60 sections 2.2.10 and 2.2.13.
uint16_t U16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const unsigned char* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t U64(const unsigned char* p) { return U32(p) | (uint64_t(U32(p+4)) << 32); }
uint16_t Crc(const unsigned char* p, size_t size) {
    uint16_t value = 0;
    for (size_t i=0; i<size; ++i) {
        value ^= static_cast<uint16_t>(p[i] << 8);
        for (int n=0; n<8; ++n) value = static_cast<uint16_t>((value << 1) ^ ((value & 0x8000) ? 0x1021 : 0));
    }
    return value;
}
bool Tag(const unsigned char* p, size_t size, uint16_t id, size_t minimum, uint32_t location = UINT32_MAX) {
    if (size < 16 || U16(p) != id || (U16(p+2) != 2 && U16(p+2) != 3) || p[5]) return false;
    unsigned sum = 0;
    for (size_t i=0; i<16; ++i) if (i != 4) sum += p[i];
    const size_t crc_size = U16(p+10);
    return static_cast<unsigned char>(sum) == p[4] && crc_size <= size-16 && crc_size+16 >= minimum &&
        (location == UINT32_MAX || U32(p+12) == location) && Crc(p+16, crc_size) == U16(p+8);
}
struct Address { uint32_t block = 0; uint16_t partition = 0; };
Address LongAddress(const unsigned char* p) { return {U32(p+4), U16(p+8)}; }
struct Partition { uint16_t number; uint32_t start, length, sequence; };
struct Extent { uint64_t offset; uint64_t length; };
struct Map {
    uint16_t partition = 0;
    bool supported = false, metadata = false;
    uint32_t metadata_file = UINT32_MAX, mirror_file = UINT32_MAX;
    std::vector<Extent> extents;
};
struct File {
    std::vector<unsigned char> descriptor;
    uint64_t size = 0;
    size_t allocation = 0, allocation_size = 0;
    uint16_t allocation_type = 0;
    unsigned char type = 0;
    SYSTEMTIME modified{};
    bool has_time = false;
};
class Reader {
public:
    Reader(HANDLE file, uint64_t size, UdfListing& out, const UdfLimits& limits)
        : file_(file), size_(size), out_(out), limits_(limits), started_(GetTickCount64()) {}
    bool Run() {
        std::vector<unsigned char> anchor;
        bool found = false;
        for (uint32_t block : {2048u, 512u, 4096u}) {
            if (size_/block < 257) continue;
            const uint64_t last = size_/block-1;
            for (uint64_t at : {uint64_t{256}, last, last-256}) {
                if (at > UINT32_MAX) continue;
                if (Read(at*block, block, anchor) && Tag(anchor.data(),anchor.size(),2,32,static_cast<uint32_t>(at))) {
                    block_ = block; found = true; break;
                }
                if (Stopped()) return false;
            }
            if (found) break;
        }
        if (!found) return Fail(L"udf-invalid");
        bool volume = false;
        for (size_t field : {size_t{16}, size_t{24}}) {
            partitions_.clear(); maps_.clear();
            if (Volume(U32(anchor.data()+field+4), U32(anchor.data()+field))) { out_.reason.clear(); volume = true; break; }
            if (Stopped()) return false;
        }
        if (!volume) return Fail(L"udf-invalid");
        for (size_t i=0; i<maps_.size(); ++i) {
            Map& map = maps_[i];
            if (!map.supported) { Partial(L"udf-unsupported-partition"); continue; }
            if (map.metadata) {
                const std::wstring previous = out_.reason;
                bool loaded = Metadata(map, map.metadata_file);
                if (!loaded && !Stopped()) loaded = Metadata(map, map.mirror_file);
                if (loaded) out_.reason = previous;
                else {
                    map.supported = false;
                    Partial(L"udf-unsupported-partition");
                }
            }
        }
        std::vector<unsigned char> fsd;
        if (!Block(file_set_,fsd) || !Tag(fsd.data(),fsd.size(),256,464,file_set_.block)) return Fail(CurrentReason());
        if (U32(fsd.data()+448) != 0) Partial(L"udf-unsupported-volume");
        if (U32(fsd.data()+400)<176 || U32(fsd.data()+400)>block_) return Fail(L"udf-invalid");
        const Address root = LongAddress(fsd.data()+400);
        File root_file;
        if (!Entry(root,root_file) || root_file.type != 4) return Fail(CurrentReason());
        return Walk(root,root_file,L"",0);
    }
private:
    bool Fail(const wchar_t* reason) { if (out_.reason.empty()) out_.reason = reason; return false; }
    const wchar_t* CurrentReason() const { return out_.reason.empty() ? L"udf-invalid" : out_.reason.c_str(); }
    void Partial(const wchar_t* reason) { out_.incomplete = true; if (out_.reason.empty()) out_.reason = reason; }
    bool Stopped() const { return out_.reason == L"udf-limit" || out_.reason == L"udf-cancelled"; }
    bool Budget() {
        if (limits_.cancelled && limits_.cancelled->load(std::memory_order_relaxed)) { out_.reason=L"udf-cancelled"; return false; }
        if (GetTickCount64()-started_ >= limits_.max_ms) { out_.reason=L"udf-limit"; return false; }
        return true;
    }
    bool Read(uint64_t at, size_t bytes, std::vector<unsigned char>& data) {
        if (!Budget()) return false;
        if (bytes > limits_.max_bytes || out_.bytes_read > limits_.max_bytes-bytes) { out_.reason=L"udf-limit"; return false; }
        if (at > size_ || bytes > size_-at || bytes > MAXDWORD) return false;
        LARGE_INTEGER pos{}; pos.QuadPart=static_cast<LONGLONG>(at);
        if (!SetFilePointerEx(file_,pos,nullptr,FILE_BEGIN)) return false;
        data.resize(bytes);
        DWORD got=0;
        if (!ReadFile(file_,data.data(),static_cast<DWORD>(bytes),&got,nullptr)) return false;
        out_.bytes_read += got;
        return got == bytes;
    }
    const Partition* Part(uint16_t number) const {
        for (const auto& p : partitions_) if (p.number == number) return &p;
        return nullptr;
    }
    bool Physical(uint16_t partition, uint64_t byte, size_t bytes, std::vector<unsigned char>& data) {
        const auto* p=Part(partition);
        if (!p || byte > uint64_t(p->length)*block_ || bytes > uint64_t(p->length)*block_-byte) return false;
        return Read(uint64_t(p->start)*block_+byte,bytes,data);
    }
    bool Mapped(uint16_t ref, uint64_t byte, size_t bytes, std::vector<unsigned char>& data) {
        if (ref >= maps_.size() || !maps_[ref].supported) return Fail(L"udf-unsupported-partition");
        const auto& map=maps_[ref];
        if (!map.metadata) return Physical(map.partition,byte,bytes,data);
        data.clear();
        uint64_t start=0;
        for (const auto& extent : map.extents) {
            if (byte >= start+extent.length) { start+=extent.length; continue; }
            const uint64_t within=byte-start;
            const size_t n=static_cast<size_t>((std::min)(uint64_t(bytes-data.size()),extent.length-within));
            std::vector<unsigned char> chunk;
            if (!Read(extent.offset+within,n,chunk)) return false;
            data.insert(data.end(),chunk.begin(),chunk.end()); byte+=n; start+=extent.length;
            if (data.size()==bytes) return true;
        }
        return false;
    }
    bool Block(Address address, std::vector<unsigned char>& data) { return Mapped(address.partition,uint64_t(address.block)*block_,block_,data); }
    bool Volume(uint32_t at, uint32_t length) {
        if (!length || length > 16u*1024*1024 || length % block_) return false;
        std::vector<unsigned char> lvd;
        uint32_t sequence=0;
        for (uint64_t i=0; i<length/block_; ++i) {
            if (uint64_t(at)+i > UINT32_MAX) return false;
            std::vector<unsigned char> d;
            if (!Read((uint64_t(at)+i)*block_,block_,d)) return false;
            const uint16_t id=U16(d.data());
            if (!Tag(d.data(),d.size(),id,16,static_cast<uint32_t>(at+i))) return false;
            if (id==8) break;
            if (id==5) {
                if (!Tag(d.data(),d.size(),5,196,static_cast<uint32_t>(at+i))) return false;
                if (std::memcmp(d.data()+25,"+NSR02",6)!=0 && std::memcmp(d.data()+25,"+NSR03",6)!=0)
                    return Fail(L"udf-unsupported-partition");
                Partition p{U16(d.data()+22),U32(d.data()+188),U32(d.data()+192),U32(d.data()+16)};
                if (uint64_t(p.start)+p.length > size_/block_) return false;
                auto found=std::find_if(partitions_.begin(),partitions_.end(),[&](const auto& old) {return old.number==p.number;});
                if (found==partitions_.end()) { if (partitions_.size()>=64) return false; partitions_.push_back(p); }
                else if (p.sequence>=found->sequence) *found=p;
            } else if (id==6) {
                if (!Tag(d.data(),d.size(),6,440,static_cast<uint32_t>(at+i))) return false;
                if (!lvd.empty() && std::memcmp(lvd.data()+84,d.data()+84,128)!=0)
                    return Fail(L"udf-unsupported-volume");
                if (lvd.empty() || U32(d.data()+16)>=sequence) { sequence=U32(d.data()+16); lvd=std::move(d); }
            } else if (id==3) return Fail(L"udf-unsupported-volume");
        }
        if (lvd.empty() || U32(lvd.data()+212)!=block_) return Fail(L"udf-unsupported-volume");
        const uint16_t revision=U16(lvd.data()+240);
        if (std::memcmp(lvd.data()+217,"*OSTA UDF Compliant",19)!=0 ||
            (revision!=0x102 && revision!=0x150 && revision!=0x200 && revision!=0x201 && revision!=0x250 && revision!=0x260))
            return Fail(L"udf-unsupported-volume");
        if (U32(lvd.data()+248)<512 || U32(lvd.data()+248)>16u*1024*1024) return false;
        const uint32_t bytes=U32(lvd.data()+264), count=U32(lvd.data()+268);
        if (!count || count>64 || bytes>lvd.size()-440 || !Tag(lvd.data(),lvd.size(),6,440+bytes)) return false;
        file_set_=LongAddress(lvd.data()+248);
        size_t pos=440;
        for (uint32_t i=0; i<count; ++i) {
            if (pos+2>440+bytes || lvd[pos+1]<2 || lvd[pos+1]>440+bytes-pos) return false;
            const auto* p=lvd.data()+pos;
            Map map;
            if (p[0]==1 && p[1]==6) { map.partition=U16(p+4); map.supported=U16(p+2)==1; }
            else if (p[0]==2 && p[1]==64) {
                map.partition=U16(p+38);
                if (std::memcmp(p+5,"*UDF Metadata Partition",23)==0) {
                    map.supported=U16(p+36)==1; map.metadata=true; map.metadata_file=U32(p+40); map.mirror_file=U32(p+44);
                }
            }
            if (!Part(map.partition)) map.supported=false;
            maps_.push_back(std::move(map)); pos+=p[1];
        }
        return pos==440+bytes;
    }
    bool ParseFile(std::vector<unsigned char> data, File& file, uint32_t location) {
        const uint16_t id=U16(data.data());
        const bool extended=id==266;
        const size_t base=extended ? 216 : 176;
        if ((id!=261 && id!=266) || !Tag(data.data(),data.size(),id,base,location)) return false;
        // Only the direct ICB strategy is supported; transformed streams are not ordinary files.
        if (U16(data.data()+20)!=4 || (U16(data.data()+34)&0x0800)) return Fail(L"udf-unsupported-allocation");
        file.type=data[27]; file.size=U64(data.data()+56); file.allocation_type=U16(data.data()+34)&7;
        const uint32_t attributes=U32(data.data()+base-8), allocation=U32(data.data()+base-4);
        if (attributes>data.size()-base || allocation>data.size()-base-attributes) return false;
        file.allocation=base+attributes; file.allocation_size=allocation;
        if (!Tag(data.data(),data.size(),id,file.allocation+allocation,location)) return false;
        const auto* time=data.data()+(extended ? 92 : 84);
        file.modified.wYear=U16(time+2); file.modified.wMonth=time[4]; file.modified.wDay=time[5];
        file.modified.wHour=time[6]; file.modified.wMinute=time[7]; file.modified.wSecond=time[8];
        FILETIME checked{}; file.has_time=SystemTimeToFileTime(&file.modified,&checked)!=FALSE;
        file.descriptor=std::move(data);
        return true;
    }
    bool Entry(Address address, File& file) {
        std::vector<unsigned char> d;
        return Block(address,d) && ParseFile(std::move(d),file,address.block);
    }
    bool Metadata(Map& map, uint32_t at) {
        if (at==UINT32_MAX) return false;
        std::vector<unsigned char> data;
        File file;
        if (!Physical(map.partition,uint64_t(at)*block_,block_,data) || !ParseFile(std::move(data),file,at) ||
            (file.type!=250 && file.type!=251) || file.allocation_type!=0 || file.allocation_size%8) return false;
        const auto* partition=Part(map.partition);
        if (!partition) return false;
        std::vector<Extent> extents;
        uint64_t total=0;
        for (size_t i=0; i<file.allocation_size; i+=8) {
            const auto* p=file.descriptor.data()+file.allocation+i;
            const uint32_t length=U32(p), block=U32(p+4);
            if ((length>>30)!=0 || !length || uint64_t(block)*block_+length>uint64_t(partition->length)*block_) return false;
            extents.push_back({(uint64_t(partition->start)+block)*block_,length}); total+=length;
        }
        if (total<file.size || !file.size) return false;
        uint64_t remaining=file.size;
        for (auto& extent : extents) { extent.length=(std::min)(extent.length,remaining); remaining-=extent.length; }
        map.extents=std::move(extents);
        return true;
    }
    bool Directory(Address address, const File& file, std::vector<unsigned char>& contents) {
        if (file.size>16ull*1024*1024) { out_.reason=L"udf-limit"; return false; }
        if (file.allocation_type==3) {
            if (file.size>file.allocation_size) return false;
            const auto first=file.descriptor.begin()+file.allocation;
            contents.assign(first,first+static_cast<size_t>(file.size)); return true;
        }
        const size_t stride=file.allocation_type==0 ? 8 : file.allocation_type==1 ? 16 : 0;
        if (!stride || file.allocation_size%stride) return Fail(L"udf-unsupported-allocation");
        contents.clear();
        for (size_t i=0; i<file.allocation_size && contents.size()<file.size; i+=stride) {
            const auto* p=file.descriptor.data()+file.allocation+i;
            const uint32_t extent=U32(p);
            if ((extent>>30)!=0) return Fail(L"udf-unsupported-allocation");
            const uint16_t ref=stride==8 ? address.partition : U16(p+8);
            const size_t length=static_cast<size_t>((std::min)(uint64_t(extent),file.size-contents.size()));
            std::vector<unsigned char> chunk;
            if (!Mapped(ref,uint64_t(U32(p+4))*block_,length,chunk)) return false;
            contents.insert(contents.end(),chunk.begin(),chunk.end());
        }
        return contents.size()==file.size;
    }
    bool Name(const unsigned char* p, size_t size, std::wstring& out) {
        if (!size || (p[0]!=8 && p[0]!=16) || (p[0]==16 && (size%2)!=1)) return false;
        for (size_t i=1; i<size; i+=p[0]==8 ? 1 : 2) {
            const wchar_t c=static_cast<wchar_t>(p[0]==8 ? p[i] : (p[i]<<8)|p[i+1]);
            if (!c || c==L'/' || c==L'\\' || c<32) return false;
            out+=c;
        }
        return !out.empty() && out!=L"." && out!=L"..";
    }
    bool Walk(Address address, const File& file, const std::wstring& parent, uint32_t depth) {
        const uint64_t key=(uint64_t(address.partition)<<32)|address.block;
        if (depth>limits_.max_depth || !ancestors_.insert(key).second) { Partial(L"udf-limit"); return false; }
        struct Erase { std::set<uint64_t>& set; uint64_t key; ~Erase() {set.erase(key);} } erase{ancestors_,key};
        std::vector<unsigned char> data;
        if (!Directory(address,file,data)) { Partial(CurrentReason()); return false; }
        size_t pos=0;
        std::set<std::wstring> names;
        while (pos<data.size()) {
            if (!Budget() || out_.entries.size()>=limits_.max_entries) { Partial(L"udf-limit"); return false; }
            if (data.size()-pos<38) { Partial(L"udf-invalid"); return false; }
            const auto* p=data.data()+pos;
            const size_t length=(38+size_t(U16(p+36))+p[19]+3)&~size_t{3};
            if (length>data.size()-pos || !Tag(p,length,257,38+size_t(U16(p+36))+p[19])) { Partial(L"udf-invalid"); return false; }
            pos+=length;
            if (p[18] & (4|8)) continue; // Deleted entries and the parent identifier.
            std::wstring name;
            if (!Name(p+38+U16(p+36),p[19],name)) { Partial(L"udf-invalid"); continue; }
            if (!names.insert(name).second || U32(p+20)<176 || U32(p+20)>block_) { Partial(L"udf-invalid"); continue; }
            const Address child=LongAddress(p+20);
            File item;
            if (!Entry(child,item)) { Partial(CurrentReason()); if (Stopped()) return false; continue; }
            const bool directory=item.type==4;
            if (directory!=((p[18]&2)!=0)) { Partial(L"udf-invalid"); continue; }
            const std::wstring path=parent.empty() ? name : parent+L"/"+name;
            if (path.size()>32768 || path.size()>4u*1024*1024-path_chars_) { out_.reason=L"udf-limit"; Partial(L"udf-limit"); return false; }
            path_chars_+=path.size();
            out_.entries.push_back({path,directory ? 0 : item.size,item.modified,item.has_time,directory});
            if (directory && !Walk(child,item,path,depth+1) && Stopped()) return false;
        }
        return true;
    }
    HANDLE file_;
    uint64_t size_;
    UdfListing& out_;
    const UdfLimits& limits_;
    uint64_t started_;
    uint32_t block_=2048;
    size_t path_chars_=0;
    Address file_set_{};
    std::vector<Partition> partitions_;
    std::vector<Map> maps_;
    std::set<uint64_t> ancestors_;
};
}
bool ReadUdfListing(HANDLE file, uint64_t file_size, UdfListing& out, const UdfLimits& limits) {
    out={};
    Reader reader(file,file_size,out,limits);
    const bool complete=reader.Run();
    if (!complete && !out.entries.empty()) out.incomplete=true;
    return complete || !out.entries.empty();
}
} // namespace pulse::preview
