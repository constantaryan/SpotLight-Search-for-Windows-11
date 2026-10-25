#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#define N 5000
static wchar_t (*names)[64];
static volatile unsigned long checksum=0;
static double now_ms(void){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return 1000.0*(double)t.QuadPart/(double)f.QuadPart;}
static int score(const wchar_t* name,const wchar_t* q){
    size_t n=wcslen(name),m=wcslen(q),i,k=0,first=0,last=0;
    const wchar_t* p;
    if(!m)return 1;
    if(wcscmp(name,q)==0)return 10000;
    if(wcsncmp(name,q,m)==0)return 8000-(int)n;
    p=wcsstr(name,q);if(p)return 5000-(int)(p-name);
    for(i=0;i<n && k<m;i++)if(name[i]==q[k]){if(!k)first=i;last=i;k++;}
    return k==m?2000-(int)(last-first):0;
}
static int compare(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR command,int show){
    const wchar_t* queries[]={L"application",L"editor",L"0420",L"ae",L"no match"};
    WNDCLASSW cls={0};HWND panel,edit,list;PROCESS_MEMORY_COUNTERS_EX hidden={0},visible={0};double times[100];int i,j,k;wchar_t output[32768];FILE* file;
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_LISTVIEW_CLASSES};
    (void)previous;(void)command;(void)show;
    InitCommonControlsEx(&controls);
    cls.lpfnWndProc=proc;cls.hInstance=instance;cls.lpszClassName=L"SpotlightBench";RegisterClassW(&cls);
    panel=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Spotlight benchmark",WS_POPUP,0,0,700,435,NULL,NULL,instance,NULL);
    edit=CreateWindowExW(0,L"EDIT",L"Search",WS_CHILD|WS_VISIBLE,24,22,650,42,panel,NULL,instance,NULL);
    list=CreateWindowExW(0,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|LVS_REPORT,16,83,668,300,panel,NULL,instance,NULL);
    (void)edit;(void)list;
    RegisterHotKey(panel,1,MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_NOREPEAT,VK_F11);
    names=(wchar_t (*)[64])calloc(N,sizeof(*names));
    for(i=0;i<N;i++)swprintf(names[i],64,L"application %04d editor",i);
    hidden.cb=sizeof(hidden);GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&hidden,sizeof(hidden));
    ShowWindow(panel,SW_SHOWNOACTIVATE);UpdateWindow(panel);
    visible.cb=sizeof(visible);GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&visible,sizeof(visible));
    ShowWindow(panel,SW_HIDE);
    for(i=0;i<100;i++){
        int best[10]={0};double start=now_ms();
        for(j=0;j<N;j++){
            int s=score(names[j],queries[i%5]);
            for(k=0;k<10;k++)if(s>best[k]){int t=best[k];best[k]=s;s=t;}
        }
        for(k=0;k<10;k++)checksum+=(unsigned long)best[k];
        times[i]=now_ms()-start;
    }
    qsort(times,100,sizeof(double),compare);
    GetModuleFileNameW(NULL,output,32700);wcscat(output,L".json");file=_wfopen(output,L"w");
    if(file){fprintf(file,"{\"hidden_private\":%llu,\"visible_private\":%llu,\"hidden_ws\":%llu,\"visible_ws\":%llu,\"rank_p50_ms\":%.6f,\"rank_p95_ms\":%.6f,\"checksum\":%lu}\n",
        (unsigned long long)hidden.PrivateUsage,(unsigned long long)visible.PrivateUsage,(unsigned long long)hidden.WorkingSetSize,(unsigned long long)visible.WorkingSetSize,times[50],times[95],checksum);fclose(file);}
    free(names);UnregisterHotKey(panel,1);DestroyWindow(panel);return 0;
}
