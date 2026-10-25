#include "core.h"
#include <searchapi.h>
#include <oledb.h>
#include <oledberr.h>
#include <msdasc.h>
#include <cstddef>
namespace sl {
Files search_files(std::wstring_view query,const std::atomic<unsigned>& gen,unsigned expected){
    Files out;double start=clock_ms();
    auto finish=[&](HRESULT hr){
        out.error=hr;out.ms=clock_ms()-start;
        out.status=FAILED(hr)?L"File search unavailable. Apps still work.":L"Indexed filename words (prefix matches)";
        return std::move(out);
    };
    if(query.size()<2 || gen.load()!=expected){
        out.skipped=true;
        out.status=L"Type at least 2 characters to search files.";
        return out;
    }
    if(query.find_first_of(L"\"*?")!=query.npos){
        out.skipped=true;
        out.status=L"Use filename words without wildcard or quote characters.";
        out.ms=clock_ms()-start;
        return out;
    }
    Com<ISearchManager> manager;
    HRESULT hr=CoCreateInstance(CLSID_CSearchManager,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(manager.put()));
    if(FAILED(hr))return finish(hr);
    Com<ISearchCatalogManager> catalog;
    hr=manager->GetCatalog(L"SystemIndex",catalog.put());if(FAILED(hr))return finish(hr);
    Com<ISearchQueryHelper> helper;
    hr=catalog->GetQueryHelper(helper.put());if(FAILED(hr))return finish(hr);
    PWSTR connection=nullptr;
    hr=helper->get_ConnectionString(&connection);if(FAILED(hr))return finish(hr);
    Com<IDataInitialize> initializer;
    hr=CoCreateInstance(CLSID_MSDAINITIALIZE,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(initializer.put()));
    Com<IDBInitialize> database;
    if(SUCCEEDED(hr))hr=initializer->GetDataSource(nullptr,CLSCTX_INPROC_SERVER,connection,IID_IDBInitialize,reinterpret_cast<IUnknown**>(database.put()));
    CoTaskMemFree(connection);if(FAILED(hr))return finish(hr);
    hr=database->Initialize();if(FAILED(hr))return finish(hr);
    Com<IDBCreateSession> sessions;
    hr=database->QueryInterface(IID_PPV_ARGS(sessions.put()));if(FAILED(hr))return finish(hr);
    Com<IDBCreateCommand> session;
    hr=sessions->CreateSession(nullptr,IID_IDBCreateCommand,reinterpret_cast<IUnknown**>(session.put()));if(FAILED(hr))return finish(hr);
    Com<ICommandText> command;
    hr=session->CreateCommand(nullptr,IID_ICommandText,reinterpret_cast<IUnknown**>(command.put()));if(FAILED(hr))return finish(hr);
    // Provider timeout is advisory; the UI never waits for this worker.
    Com<ICommandProperties> props;
    if(SUCCEEDED(command->QueryInterface(IID_PPV_ARGS(props.put())))){
        DBPROP prop{};prop.dwPropertyID=DBPROP_COMMANDTIMEOUT;prop.dwOptions=DBPROPOPTIONS_OPTIONAL;
        prop.colid=DB_NULLID;prop.vValue.vt=VT_I4;prop.vValue.lVal=2;
        DBPROPSET set{&prop,1,DBPROPSET_ROWSET};props->SetProperties(1,&set);
    }
    auto sql=file_query(query);
    hr=command->SetCommandText(DBGUID_DEFAULT,sql.c_str());if(FAILED(hr))return finish(hr);
    if(gen.load()!=expected)return finish(S_OK);
    Com<IRowset> rows;
    hr=command->Execute(nullptr,IID_IRowset,nullptr,nullptr,reinterpret_cast<IUnknown**>(rows.put()));
    if(FAILED(hr))return finish(hr);
    Com<IAccessor> accessor;
    hr=rows->QueryInterface(IID_PPV_ARGS(accessor.put()));if(FAILED(hr))return finish(hr);
    struct Column {DBSTATUS status;DBLENGTH length;wchar_t text[32768];};
    struct Row {Column name,path;};
    auto row=std::make_unique<Row>();
    DBBINDING bindings[2]{};
    for(int i=0;i<2;++i){
        auto& b=bindings[i];size_t offset=i?offsetof(Row,path):offsetof(Row,name);
        b.iOrdinal=i+1;b.obStatus=offset+offsetof(Column,status);b.obLength=offset+offsetof(Column,length);b.obValue=offset+offsetof(Column,text);
        b.dwPart=DBPART_STATUS|DBPART_LENGTH|DBPART_VALUE;b.dwMemOwner=DBMEMOWNER_CLIENTOWNED;
        b.eParamIO=DBPARAMIO_NOTPARAM;b.cbMaxLen=sizeof(Column::text);b.wType=DBTYPE_WSTR;
    }
    HACCESSOR handle{};DBBINDSTATUS statuses[2]{};
    hr=accessor->CreateAccessor(DBACCESSOR_ROWDATA,2,bindings,sizeof(Row),&handle,statuses);
    if(FAILED(hr))return finish(hr);
    out.items.reserve(MaxResults);
    while(gen.load()==expected && out.items.size()<MaxResults){
        DBCOUNTITEM count=0;HROW* batch=nullptr;
        HRESULT next=rows->GetNextRows(DB_NULL_HCHAPTER,0,16,&count,&batch);
        if(FAILED(next)){hr=next;break;}
        for(DBCOUNTITEM i=0;i<count;++i){
            if(gen.load()!=expected)break;
            *row={};
            if(SUCCEEDED(rows->GetData(batch[i],handle,row.get())) && row->name.status==DBSTATUS_S_OK && row->path.status==DBSTATUS_S_OK){
                std::wstring name(row->name.text),path(row->path.text);
                if(!path.empty() && is_absolute_path(path))out.items.push_back({name,path,{},Kind::File});
            }
            if(out.items.size()>=MaxResults)break;
        }
        if(count)rows->ReleaseRows(count,batch,nullptr,nullptr,nullptr);
        CoTaskMemFree(batch);
        if(next==DB_S_ENDOFROWSET || !count)break;
    }
    accessor->ReleaseAccessor(handle,nullptr);
    return finish(hr);
}
}
