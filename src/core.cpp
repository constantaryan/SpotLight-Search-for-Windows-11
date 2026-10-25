#include "core.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <cwctype>
namespace sl {
double clock_ms(){ LARGE_INTEGER t,f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return 1000.0*t.QuadPart/f.QuadPart; }
std::wstring fold(std::wstring_view s){
    if(s.empty())return {};
    int n=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr,0);
    if(n<=0)return std::wstring(s);
    std::wstring out(static_cast<size_t>(n),L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,s.data(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr,0);
    return out;
}
int rank(std::wstring_view name,std::wstring_view q){
    if(q.empty())return 1;
    if(name==q)return 10000;
    if(name.starts_with(q))return 8000-static_cast<int>(std::min<size_t>(name.size(),200));
    auto pos=name.find(q);
    if(pos!=name.npos) return (pos>0 && !iswalnum(name[pos-1]) ? 6500:5000)-static_cast<int>(std::min<size_t>(pos,200));
    size_t at=0, first=0, last=0;
    for(size_t i=0;i<name.size() && at<q.size();++i)if(name[i]==q[at]){if(!at)first=i;last=i;++at;}
    return at==q.size()?2000-static_cast<int>(std::min<size_t>(last-first,500)):0;
}
std::wstring file_query(std::wstring_view s){
    // A quoted full-text phrase, with SQL apostrophes escaped.
    std::wstring literal;
    for(wchar_t c:s){if(c==L'\'')literal+=L"''";else literal+=c;}
    return L"SELECT TOP 100 System.ItemNameDisplay, System.ItemPathDisplay FROM SystemIndex WHERE CONTAINS(System.ItemNameDisplay, '\""+
        literal+L"*\"')";
}
bool is_absolute_path(std::wstring_view s){
    return s.size()>=3 && ((iswalpha(s[0]) && s[1]==L':' && (s[2]==L'\\'||s[2]==L'/')) ||
        (s[0]==L'\\' && s[1]==L'\\' && s[2]!=L'?' && s[2]!=L'.'));
}
std::wstring web_search_url(std::wstring_view query){
    // UI input is bounded to 512 UTF-16 units; bound callers here as well.
    if(query.empty() || query.size()>512)return {};
    int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,query.data(),static_cast<int>(query.size()),nullptr,0,nullptr,nullptr);
    if(count<=0)return {};
    std::string utf8(static_cast<size_t>(count),'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,query.data(),static_cast<int>(query.size()),utf8.data(),count,nullptr,nullptr))return {};
    std::wstring url=L"https://www.bing.com/search?q=";
    constexpr wchar_t hex[]=L"0123456789ABCDEF";
    for(unsigned char ch:utf8){
        if((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='.'||ch=='_'||ch=='~')url+=static_cast<wchar_t>(ch);
        else {url+=L'%';url+=hex[ch>>4];url+=hex[ch&15];}
    }
    return url;
}
void append_web_result(std::vector<Result>& results,std::wstring_view query){
    results.erase(std::remove_if(results.begin(),results.end(),[](const Result& r){return r.item.kind==Kind::Web;}),results.end());
    if(results.size()>VisibleResults)results.resize(VisibleResults);
    auto first=query.find_first_not_of(L" \t\r\n");
    if(first==query.npos)return;
    auto last=query.find_last_not_of(L" \t\r\n");query=query.substr(first,last-first+1);
    // A pasted filesystem path must not become an accidental external query.
    if(is_absolute_path(query))return;
    auto url=web_search_url(query);if(url.empty())return;
    if(results.size()>=VisibleResults)results.resize(VisibleResults-1);
    auto name=L"Search the web: "+std::wstring(query);
    results.push_back({{std::move(name),std::move(url),{},Kind::Web},-1});
}
std::vector<Result> match(const std::vector<Item>& items,std::wstring_view query,const std::map<std::wstring,unsigned>& history){
    const auto q=fold(query);
    // Rank references in a fixed array; allocate/copy strings only for the winners.
    struct Candidate {const Item* item=nullptr;int score=0;};
    Candidate best[VisibleResults]{};size_t count=0;
    for(const auto& item:items){
        int score=rank(item.folded,q);if(!score)continue;
        auto h=history.find(item.target);
        if(h!=history.end())score+=static_cast<int>(std::min(h->second,100u));
        size_t pos=0;
        while(pos<count && (best[pos].score>score || (best[pos].score==score && best[pos].item->name<item.name)))++pos;
        if(pos>=VisibleResults)continue;
        size_t last=std::min(count,VisibleResults-1);
        for(size_t j=last;j>pos;--j)best[j]=best[j-1];
        best[pos]={&item,score};if(count<VisibleResults)++count;
    }
    std::vector<Result> out;out.reserve(count);
    for(size_t i=0;i<count;++i)out.push_back({*best[i].item,best[i].score});
    return out;
}
static void scan_links(const std::wstring& root,std::vector<Item>& out,int depth,bool& capped){
    if(depth>12)return;
    WIN32_FIND_DATAW d{};
    HANDLE h=FindFirstFileExW((root+L"\\*").c_str(),FindExInfoBasic,&d,FindExSearchNameMatch,nullptr,0);
    if(h==INVALID_HANDLE_VALUE)return;
    do {
        if(out.size()>=MaxApps){capped=true;break;}
        if(d.cFileName[0]==L'.' && (!d.cFileName[1] || (d.cFileName[1]==L'.'&&!d.cFileName[2])))continue;
        std::wstring path=root+L"\\"+d.cFileName;
        if(d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
            if(!(d.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))scan_links(path,out,depth+1,capped);
        }else if(_wcsicmp(PathFindExtensionW(d.cFileName),L".lnk")==0){
            std::wstring name=d.cFileName;name.resize(name.size()-4);
            out.push_back({name,path,fold(name),Kind::App});
        }
    }while(FindNextFileW(h,&d));
    FindClose(h);
}
std::vector<Item> catalogue(bool& capped){
    std::vector<Item> out;out.reserve(512);capped=false;
    for(const auto* id:{&FOLDERID_Programs,&FOLDERID_CommonPrograms}){
        PWSTR path=nullptr;
        if(SUCCEEDED(SHGetKnownFolderPath(*id,0,nullptr,&path))){scan_links(path,out,0,capped);CoTaskMemFree(path);}
    }
    Com<IShellFolder> folder;
    Com<IShellItem> appsFolder;
    if(SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(appsFolder.put()))) &&
       SUCCEEDED(appsFolder->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(folder.put())))){
        Com<IEnumIDList> enumeration;
        if(SUCCEEDED(folder->EnumObjects(nullptr,SHCONTF_NONFOLDERS|SHCONTF_FOLDERS,enumeration.put())) && enumeration){
            PITEMID_CHILD child=nullptr;
            while(out.size()<MaxApps && enumeration->Next(1,&child,nullptr)==S_OK){
                Com<IShellItem> item;
                if(SUCCEEDED(SHCreateItemWithParent(nullptr,folder.p,child,IID_PPV_ARGS(item.put())))){
                    PWSTR name=nullptr,target=nullptr;
                    if(SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY,&name)) &&
                       SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&target))){
                        auto normalized=fold(name);
                        if(std::none_of(out.begin(),out.end(),[&](const Item& i){return i.folded==normalized;})){
                            std::wstring launchTarget=target;
                            // AppsFolder may return a bare AUMID instead of an absolute Shell parsing name.
                            if(!is_absolute_path(launchTarget) && !launchTarget.starts_with(L"::") && !launchTarget.starts_with(L"shell:"))
                                launchTarget=L"shell:AppsFolder\\"+launchTarget;
                            out.push_back({name,std::move(launchTarget),std::move(normalized),Kind::App});
                        }
                    }
                    CoTaskMemFree(name);CoTaskMemFree(target);
                }
                CoTaskMemFree(child);child=nullptr;
            }
            if(out.size()>=MaxApps)capped=true;
        }
    }
    std::sort(out.begin(),out.end(),[](const Item&a,const Item&b){return a.folded<b.folded;});
    return out;
}
HRESULT launch(const Item& item){
    SHELLEXECUTEINFOW e{sizeof(e)};
    e.fMask=SEE_MASK_FLAG_NO_UI|SEE_MASK_NOASYNC;
    e.nShow=SW_SHOWNORMAL;e.lpVerb=L"open";
    PIDLIST_ABSOLUTE pidl=nullptr;
    if(item.target.starts_with(L"::") || item.target.starts_with(L"shell:")){
        HRESULT hr=SHParseDisplayName(item.target.c_str(),nullptr,&pidl,0,nullptr);
        if(FAILED(hr))return hr;
        e.fMask|=SEE_MASK_IDLIST;e.lpIDList=pidl;
    }else e.lpFile=item.target.c_str();
    BOOL ok=ShellExecuteExW(&e);
    HRESULT hr=ok?S_OK:HRESULT_FROM_WIN32(GetLastError());
    CoTaskMemFree(pidl);return hr;
}
}
