#include "../preview_host/udf_listing.h"
#include "../preview_host/archive_listing.h"
#include "preview_host_client.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {
constexpr size_t kBlock=2048;
int failures=0;
void Check(bool value,const char* label) {std::printf("[%s] %s\n",value ? "PASS" : "FAIL",label);if(!value) ++failures;}
void W16(unsigned char* p,uint16_t v) {p[0]=static_cast<unsigned char>(v);p[1]=static_cast<unsigned char>(v>>8);}
void W32(unsigned char* p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
void W64(unsigned char* p,uint64_t v) {for(unsigned i=0;i<8;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
void Tag(unsigned char* p,uint16_t id,uint32_t location,size_t length) {
    W16(p,id);W16(p+2,3);p[4]=0;p[5]=0;W16(p+6,1);W16(p+10,static_cast<uint16_t>(length-16));W32(p+12,location);
    uint16_t crc=0;
    for(size_t i=16;i<length;++i) {
        crc^=static_cast<uint16_t>(p[i]<<8);
        for(int bit=0;bit<8;++bit)crc=static_cast<uint16_t>((crc<<1)^((crc&0x8000) ? 0x1021 : 0));
    }
    W16(p+8,crc);
    unsigned sum=0;for(size_t i=0;i<16;++i)sum+=p[i];p[4]=static_cast<unsigned char>(sum);
}
void Long(unsigned char* p,uint32_t length,uint32_t block,uint16_t partition) {W32(p,length);W32(p+4,block);W16(p+8,partition);}
std::vector<unsigned char> Fid(const std::wstring& name,uint32_t block,uint16_t partition,unsigned char flags,uint32_t location) {
    std::vector<unsigned char> encoded{16};
    for(wchar_t c:name){encoded.push_back(static_cast<unsigned char>(c>>8));encoded.push_back(static_cast<unsigned char>(c));}
    if(flags&8)encoded.clear();
    std::vector<unsigned char> d((38+encoded.size()+3)&~size_t{3});
    W16(d.data()+16,1);d[18]=flags;d[19]=static_cast<unsigned char>(encoded.size());
    Long(d.data()+20,kBlock,block,partition);
    std::copy(encoded.begin(),encoded.end(),d.begin()+38);
    Tag(d.data(),257,location,d.size());return d;
}
struct Fixture {
    std::vector<unsigned char> bytes=std::vector<unsigned char>(700*kBlock);
    bool metadata=false, extended=false;
    uint16_t mode=0, ref=0;
    unsigned char* Sector(size_t i){return bytes.data()+i*kBlock;}
    unsigned char* Logical(size_t i){return Sector(300+(metadata ? 20 : 0)+i);}
    void FileAt(unsigned char* p,uint32_t location,unsigned char type,uint64_t size,uint16_t allocation,
                uint32_t data_block,const std::vector<unsigned char>& inline_data={}) {
        const size_t base=extended ? 216 : 176;
        W16(p+20,4);p[27]=type;W16(p+34,allocation);W64(p+56,size);
        if(extended)W64(p+64,size);
        W64(p+(extended ? 72 : 64),allocation==3 ? 0 : (size+kBlock-1)/kBlock);
        auto* time=p+(extended ? 92 : 84);W16(time,0x1000);W16(time+2,2026);time[4]=10;time[5]=4;time[6]=12;
        size_t ad=allocation==3 ? inline_data.size() : allocation==1 ? 16 : 8;
        W32(p+base-4,static_cast<uint32_t>(ad));
        if(allocation==3)std::copy(inline_data.begin(),inline_data.end(),p+base);
        else if(allocation==1)Long(p+base,static_cast<uint32_t>(size),data_block,ref);
        else {W32(p+base,static_cast<uint32_t>(size));W32(p+base+4,data_block);}
        Tag(p,extended ? 266 : 261,location,base+ad);
    }
    Fixture(bool meta=false,uint16_t ad_mode=0,bool efe=false):metadata(meta),extended(efe),mode(ad_mode),ref(meta ? 1 : 0) {
        auto* vrs=Sector(16);std::memcpy(vrs+1,"BEA01",5);vrs[6]=1;
        vrs=Sector(17);std::memcpy(vrs+1,"NSR03",5);vrs[6]=1;
        vrs=Sector(18);std::memcpy(vrs+1,"TEA01",5);vrs[6]=1;
        auto* anchor=Sector(256);W32(anchor+16,3*kBlock);W32(anchor+20,257);W32(anchor+24,3*kBlock);W32(anchor+28,257);Tag(anchor,2,256,512);
        std::memcpy(Sector(699),anchor,kBlock);Tag(Sector(699),2,699,512);
        auto* pd=Sector(257);W32(pd+16,1);W16(pd+20,1);W16(pd+22,0);std::memcpy(pd+25,"+NSR03",6);W32(pd+184,1);W32(pd+188,300);W32(pd+192,300);Tag(pd,5,257,512);
        auto* lvd=Sector(258);W32(lvd+16,2);std::memcpy(lvd+21,"OSTA Compressed Unicode",23);W32(lvd+212,kBlock);std::memcpy(lvd+217,"*OSTA UDF Compliant",19);W16(lvd+240,meta ? 0x260 : 0x201);Long(lvd+248,kBlock,0,ref);
        W32(lvd+264,meta ? 70 : 6);W32(lvd+268,meta ? 2 : 1);lvd[440]=1;lvd[441]=6;W16(lvd+442,1);W16(lvd+444,0);
        if(meta){auto* map=lvd+446;map[0]=2;map[1]=64;std::memcpy(map+5,"*UDF Metadata Partition",23);W16(map+36,1);W16(map+38,0);W32(map+40,10);W32(map+44,11);W32(map+48,UINT32_MAX);W32(map+52,1);W16(map+56,1);}
        Tag(lvd,6,258,meta ? 510 : 446);Tag(Sector(259),8,259,512);
        if(meta){FileAt(Sector(310),10,250,50*kBlock,0,20);FileAt(Sector(311),11,251,50*kBlock,0,20);}
        auto* fsd=Logical(0);std::memcpy(fsd+49,"OSTA Compressed Unicode",23);std::memcpy(fsd+241,"OSTA Compressed Unicode",23);
        Long(fsd+400,kBlock,1,ref);std::memcpy(fsd+417,"*OSTA UDF Compliant",19);W16(fsd+440,meta ? 0x260 : 0x201);Tag(fsd,256,0,512);
        std::vector<unsigned char> root;
        for(const auto& fid : {Fid(L"",1,ref,8,mode==3 ? 1 : 2),Fid(L"阅读.txt",3,ref,0,mode==3 ? 1 : 2),Fid(L"folder",4,ref,2,mode==3 ? 1 : 2),Fid(L"deleted.txt",99,ref,4,mode==3 ? 1 : 2)})root.insert(root.end(),fid.begin(),fid.end());
        auto nested=Fid(L"nested.txt",6,ref,0,mode==3 ? 4 : 5);
        FileAt(Logical(1),1,4,root.size(),mode,2,root);
        FileAt(Logical(4),4,4,nested.size(),mode,5,nested);
        std::copy(root.begin(),root.end(),Logical(2));std::copy(nested.begin(),nested.end(),Logical(5));
        FileAt(Logical(3),3,5,12345,0,80);FileAt(Logical(6),6,5,54321,0,90);
    }
};
bool Write(const std::wstring& path,const std::vector<unsigned char>& bytes) {
    HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    DWORD got=0;const bool ok=WriteFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)&&got==bytes.size();CloseHandle(f);return ok;
}
bool Read(const std::wstring& path,pulse::preview::UdfListing& listing,const pulse::preview::UdfLimits& limits={}) {
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};GetFileSizeEx(f,&size);
    const bool result=pulse::preview::ReadUdfListing(f,static_cast<uint64_t>(size.QuadPart),listing,limits);CloseHandle(f);return result;
}
bool Contains(const pulse::preview::UdfListing& listing,const wchar_t* name,uint64_t size) {
    return std::any_of(listing.entries.begin(),listing.entries.end(),[&](const auto& e){return e.path==name&&e.size==size;});
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc==3 && std::wstring(argv[1])==L"--image") {
        pulse::preview::UdfListing listing;
        const bool ok=Read(argv[2],listing);
        Check(ok&&!listing.incomplete&&listing.reason.empty(),"external UDF image has complete native directory traversal");
        Check(Contains(listing,L"阅读.txt",12345)&&Contains(listing,L"folder/nested.txt",54321),"external authoring tool preserves Unicode nested paths and exact sizes");
        std::wprintf(L"reason=%s, entries=%zu\n",listing.reason.c_str(),listing.entries.size());
        return failures ? 1 : 0;
    }
    const auto root=std::filesystem::absolute(std::filesystem::path(L"bench_data")/(L"udf_native_"+std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    const std::wstring path=(root/L"physical.iso").wstring();
    pulse::preview::UdfListing listing;
    for(bool meta : {false,true})for(bool efe : {false,true})for(uint16_t mode : {uint16_t{0},uint16_t{1},uint16_t{3}}) {
        Fixture fixture(meta,mode,efe);Check(Write(path,fixture.bytes),"write physical/metadata FE/EFE allocation fixture");
        Check(Read(path,listing)&&!listing.incomplete&&listing.reason.empty()&&listing.entries.size()==3&&
              Contains(listing,L"阅读.txt",12345)&&Contains(listing,L"folder/nested.txt",54321),
              "UDF reads Unicode nested sizes with short/long/inline directory allocation");
        std::wstring text,error;uint32_t read=0;
        Check(pulse::preview::MakeArchiveListing(path,text,read,&error)&&text.starts_with(L"PULSEARC\t1\tUDF\t-\t0\n")&&
              text.find(L"阅读.txt")!=std::wstring::npos&&text.find(L"nested.txt")!=std::wstring::npos&&error.empty(),"archive integration emits complete native UDF tree");
    }
    for (uint16_t revision : {uint16_t{0x102},uint16_t{0x150},uint16_t{0x200},uint16_t{0x201},uint16_t{0x250},uint16_t{0x260}}) {
        Fixture revision_fixture;
        W16(revision_fixture.Sector(258)+240,revision);Tag(revision_fixture.Sector(258),6,258,446);
        Write(path,revision_fixture.bytes);
        Check(Read(path,listing)&&!listing.incomplete&&listing.entries.size()==3,"supported UDF revisions retain complete physical partition traversal");
    }
    Fixture fixture;
    std::fill(fixture.Sector(256),fixture.Sector(257),static_cast<unsigned char>(0));
    Write(path,fixture.bytes);Check(Read(path,listing)&&listing.entries.size()==3,"backup anchor is used when primary anchor is damaged");
    fixture=Fixture(true);fixture.Sector(310)[8]^=1;Write(path,fixture.bytes);
    Check(Read(path,listing)&&!listing.incomplete&&listing.reason.empty()&&listing.entries.size()==3,"metadata mirror file recovers damaged primary metadata descriptor");
    fixture=Fixture();fixture.Logical(6)[80]^=1;Write(path,fixture.bytes);
    Check(Read(path,listing)&&listing.incomplete&&listing.reason==L"udf-invalid"&&Contains(listing,L"阅读.txt",12345)&&!Contains(listing,L"folder/nested.txt",54321),"bad child descriptor CRC produces explicit partial tree");
    fixture=Fixture();W32(fixture.Logical(1)+176+4,0xfffffff0);Tag(fixture.Logical(1),261,1,184);Write(path,fixture.bytes);
    Check(!Read(path,listing)&&listing.reason==L"udf-invalid","directory extent outside partition is rejected");
    fixture=Fixture(true);std::memset(fixture.Sector(258)+451,0,23);std::memcpy(fixture.Sector(258)+451,"*UDF Virtual Partition",22);Tag(fixture.Sector(258),6,258,510);Write(path,fixture.bytes);
    Check(!Read(path,listing)&&listing.reason==L"udf-unsupported-partition","unsupported VAT partition reports explicit unsupported result");
    fixture=Fixture();
    auto cycle=Fid(L"loop",1,0,2,5);
    std::fill(fixture.Logical(5),fixture.Logical(6),static_cast<unsigned char>(0));
    std::copy(cycle.begin(),cycle.end(),fixture.Logical(5));
    fixture.FileAt(fixture.Logical(4),4,4,cycle.size(),0,5);
    Write(path,fixture.bytes);
    Check(Read(path,listing)&&listing.incomplete&&listing.reason==L"udf-limit"&&listing.entries.size()==3,"directory cycle is bounded and cannot become a complete tree");
    fixture=Fixture();Write(path,fixture.bytes);
    pulse::preview::UdfLimits limits;limits.max_entries=1;
    Check(Read(path,listing,limits)&&listing.incomplete&&listing.reason==L"udf-limit"&&listing.entries.size()==1,"entry budget retains only explicitly partial results");
    limits={};limits.max_ms=0;Check(!Read(path,listing,limits)&&listing.reason==L"udf-limit","expired deadline stops before reading");
    limits={};limits.max_bytes=1;Check(!Read(path,listing,limits)&&listing.reason==L"udf-limit","read-byte budget stops allocation and IO");
    std::atomic_bool cancel{true};limits={};limits.cancelled=&cancel;
    Check(!Read(path,listing,limits)&&listing.reason==L"udf-cancelled","explicit cancellation stops before reading");
    pulse_test::Host host;const bool started=host.Start();Check(started,"start real preview host for native UDF IPC");
    if(started){pulse_test::Result result;Check(host.Request(path,result)&&result.response.kind==pulse::ipc::PreviewContentKind::Archive&&
        result.text.starts_with(L"PULSEARC\t1\tUDF\t-\t0\n")&&result.text.find(L"阅读.txt")!=std::wstring::npos,"native Unicode UDF listing survives real preview-host IPC");}
    host.Stop();std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
