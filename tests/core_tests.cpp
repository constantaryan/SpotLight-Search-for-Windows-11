#include "core.h"
#include <iostream>
#include <cstdlib>
using namespace sl;
void check(bool result,const char* name){if(!result){std::cerr<<"FAIL: "<<name<<"\n";std::exit(1);}}
int main(){
    check(rank(L"notepad",L"notepad")>rank(L"notepad++",L"notepad"),"exact beats prefix");
    check(rank(L"visual studio",L"studio")>rank(L"studiovisual",L"visual"),"word boundary beats substring");
    check(rank(L"visual studio code",L"vsc")>0,"subsequence");
    check(rank(L"notepad",L"xyz")==0,"miss");
    check(fold(L"HELLO")==L"hello","case fold");
    check(fold(L"\u0939\u093f\u0928\u094d\u0926\u0940")==L"\u0939\u093f\u0928\u094d\u0926\u0940","Hindi preserved");
    check(file_query(L"O'Brien_100%[x]").find(L"O''Brien_100%[x]")!=std::wstring::npos,"SQL apostrophe escaped inside phrase");
    check(file_query(L"x' OR 1=1 --").find(L"x'' OR 1=1 --")!=std::wstring::npos,"quote injection escaped");
    check(is_absolute_path(L"C:\\Users\\x"),"drive path");
    check(is_absolute_path(L"\\\\server\\share"),"UNC path");
    check(!is_absolute_path(L"C:relative"),"relative drive rejected");
    check(!is_absolute_path(L"https://example.com"),"URI not direct path");
    check(!is_absolute_path(L"\\\\.\\PhysicalDrive0"),"device path rejected");
    std::vector<Item> items;
    for(int i=0;i<100;++i){auto name=L"Application "+std::to_wstring(i);items.push_back({name,name,fold(name),Kind::App});}
    auto results=match(items,L"app",{});
    check(results.size()==VisibleResults,"bounded results");
    std::map<std::wstring,unsigned> history{{L"Application 90",80}};
    results=match(items,L"",history);
    check(results[0].item.target==L"Application 90","history tie break");
    std::atomic<unsigned> generation=2;
    auto stale=search_files(L"anything",generation,1);
    check(stale.items.empty() && SUCCEEDED(stale.error),"obsolete request exits before provider");
    auto shortQuery=search_files(L"a",generation,2);
    check(shortQuery.skipped && shortQuery.items.empty(),"one-character files skipped");
    auto syntax=search_files(L"bad\" OR x",generation,2);
    check(syntax.skipped && syntax.items.empty(),"embedded quote never reaches query parser");
    check(web_search_url(L"vs code")==L"https://www.bing.com/search?q=vs%20code","web spaces encoded");
    check(web_search_url(L"C++ & x#y?z=1%2")==L"https://www.bing.com/search?q=C%2B%2B%20%26%20x%23y%3Fz%3D1%252","web delimiters encoded");
    check(web_search_url(L"\u0939")==L"https://www.bing.com/search?q=%E0%A4%B9","web UTF-8 encoding");
    check(web_search_url(std::wstring(1,static_cast<wchar_t>(0xD800))).empty(),"malformed UTF-16 rejected");
    check(web_search_url(std::wstring(513,L'x')).empty(),"web query bounded");
    results=match(items,L"app",{});auto top=results.front().item.target;
    append_web_result(results,L" app ");
    check(results.size()==VisibleResults && results.front().item.target==top && results.back().item.kind==Kind::Web,"web preserves local priority and cap");
    append_web_result(results,L"updated");
    check(results.size()==VisibleResults && results.back().item.target.ends_with(L"updated"),"web action replaced not duplicated");
    append_web_result(results,L"C:\\Users\\private");
    bool hasWeb=false;for(const auto& result:results)hasWeb|=result.item.kind==Kind::Web;
    check(!hasWeb,"direct paths are not offered online");
    results.clear();append_web_result(results,L"missing application");
    check(results.size()==1 && results.front().item.kind==Kind::Web,"web available for local misses");
    append_web_result(results,L" \t\r\n");check(results.empty(),"blank query has no web action");
    std::cout<<"28 checks passed\n";
}
