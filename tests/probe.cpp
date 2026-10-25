#include "core.h"
#include <psapi.h>
#include <iostream>
#include <algorithm>
using namespace sl;
int wmain(int argc,wchar_t**argv){
    Apartment apartment(COINIT_MULTITHREADED);
    if(argc>1 && std::wstring_view(argv[1])==L"--search-suite"){
        const wchar_t* terms[]={L"readme",L"plan",L"report",L"document",L"photo",L"image",L"test",L"project",L"notes",L"setup",L"download",L"Microsoft",L"Windows",L"O'Brien",L"100%",L"file_name",L"[draft]",L"\u0939\u093f\u0928\u094d\u0926\u0940",L"no_matching_file_4a839",L"a"};
        std::atomic<unsigned> gen=1;int failures=0;std::cout<<"[";
        for(int i=0;i<100;++i){
            auto files=search_files(terms[i%20],gen,1);if(FAILED(files.error))++failures;
            if(i)std::cout<<",";
            std::cout<<"{\"query\":"<<(i%20)<<",\"run\":"<<(i/20)<<",\"hresult\":"<<static_cast<unsigned long>(files.error)<<",\"count\":"<<files.items.size()<<",\"skipped\":"<<files.skipped<<",\"ms\":"<<files.ms<<"}";
        }
        std::cout<<"]\n";return failures?1:0;
    }
    if(argc>1 && std::wstring_view(argv[1])==L"--search"){
        std::atomic<unsigned> gen=1;
        auto files=search_files(argc>2?argv[2]:L"readme",gen,1);
        std::cout<<"{\"hresult\":"<<static_cast<unsigned long>(files.error)<<",\"count\":"<<files.items.size()<<",\"skipped\":"<<files.skipped<<",\"ms\":"<<files.ms<<"}\n";
        return FAILED(files.error)?1:0;
    }
    bool capped=false;double start=clock_ms();auto apps=catalogue(capped);double elapsed=clock_ms()-start;
    std::vector<double> times;unsigned checksum=0;
    for(int i=0;i<1000;++i){auto t=clock_ms();auto results=match(apps,i%2?L"code":L"explorer",{});times.push_back(clock_ms()-t);checksum+=static_cast<unsigned>(results.size());}
    std::sort(times.begin(),times.end());
    PROCESS_MEMORY_COUNTERS_EX m{sizeof(m)};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m));
    std::cout<<"{\"apps\":"<<apps.size()<<",\"capped\":"<<capped<<",\"catalogue_ms\":"<<elapsed
    <<",\"rank_p50_ms\":"<<times[500]<<",\"rank_p95_ms\":"<<times[950]<<",\"private_bytes\":"<<m.PrivateUsage
    <<",\"working_set\":"<<m.WorkingSetSize<<",\"checksum\":"<<checksum<<"}\n";
}
