#![windows_subsystem = "windows"]
use std::{ptr::null_mut,mem::{size_of,zeroed}};
use windows_sys::Win32::{Foundation::*, UI::{WindowsAndMessaging::*,Controls::*,Input::KeyboardAndMouse::*}, Graphics::Gdi::UpdateWindow, System::{LibraryLoader::*,Threading::*,ProcessStatus::*,Performance::*}};
fn wide(s:&str)->Vec<u16>{s.encode_utf16().chain(Some(0)).collect()}
unsafe extern "system" fn proc(h:HWND,m:u32,w:WPARAM,l:LPARAM)->LRESULT {DefWindowProcW(h,m,w,l)}
fn score(name:&[u16],q:&[u16])->i32{
    if q.is_empty(){return 1} if name==q{return 10000}
    if name.starts_with(q){return 8000-name.len() as i32}
    if let Some(p)=name.windows(q.len()).position(|x|x==q){return 5000-p as i32}
    let(mut k,mut first,mut last)=(0,0,0);
    for(i,&c)in name.iter().enumerate(){if k==q.len(){break} if c==q[k]{if k==0{first=i}last=i;k+=1;}}
    if k==q.len(){2000-(last-first) as i32}else{0}
}
unsafe fn now()->f64{let(mut t,mut f)=(0,0);QueryPerformanceCounter(&mut t);QueryPerformanceFrequency(&mut f);1000.0*t as f64/f as f64}
fn main(){unsafe{
    let controls=INITCOMMONCONTROLSEX{dwSize:size_of::<INITCOMMONCONTROLSEX>() as u32,dwICC:ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    let instance=GetModuleHandleW(std::ptr::null());let class=wide("SpotlightBenchRust");
    let mut cls:WNDCLASSW=zeroed();cls.lpfnWndProc=Some(proc);cls.hInstance=instance;cls.lpszClassName=class.as_ptr();RegisterClassW(&cls);
    let panel=CreateWindowExW(WS_EX_TOOLWINDOW,class.as_ptr(),wide("Spotlight benchmark").as_ptr(),WS_POPUP,0,0,700,435,null_mut(),null_mut(),instance,std::ptr::null());
    CreateWindowExW(0,wide("EDIT").as_ptr(),wide("Search").as_ptr(),WS_CHILD|WS_VISIBLE,24,22,650,42,panel,null_mut(),instance,std::ptr::null());
    CreateWindowExW(0,wide("SysListView32").as_ptr(),wide("").as_ptr(),WS_CHILD|WS_VISIBLE|LVS_REPORT,16,83,668,300,panel,null_mut(),instance,std::ptr::null());
    RegisterHotKey(panel,1,MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_NOREPEAT,VK_F11 as u32);
    let mut names=vec![[0u16;64];5000];
    for(i,name)in names.iter_mut().enumerate(){
        let s=format!("application {:04} editor",i);
        for(j,c)in s.encode_utf16().enumerate(){name[j]=c;}
    }
    let mut hidden:PROCESS_MEMORY_COUNTERS_EX=zeroed();hidden.cb=size_of::<PROCESS_MEMORY_COUNTERS_EX>() as u32;
    GetProcessMemoryInfo(GetCurrentProcess(),&mut hidden as *mut _ as *mut PROCESS_MEMORY_COUNTERS,hidden.cb);
    ShowWindow(panel,SW_SHOWNOACTIVATE);UpdateWindow(panel);
    let mut visible:PROCESS_MEMORY_COUNTERS_EX=zeroed();visible.cb=size_of::<PROCESS_MEMORY_COUNTERS_EX>() as u32;
    GetProcessMemoryInfo(GetCurrentProcess(),&mut visible as *mut _ as *mut PROCESS_MEMORY_COUNTERS,visible.cb);
    ShowWindow(panel,SW_HIDE);
    let queries:Vec<Vec<u16>>=["application","editor","0420","ae","no match"].iter().map(|s|s.encode_utf16().collect()).collect();
    let mut times=Vec::with_capacity(100);let mut checksum=0u32;
    for i in 0..100{
        let mut best=[0;10];let start=now();
        for name in &names{
            let n=name.iter().position(|&c|c==0).unwrap_or(64);
            let mut s=score(&name[..n],&queries[i%5]);
            for b in &mut best{if s>*b{std::mem::swap(&mut s,b);}}
        }
        for b in best{checksum=checksum.wrapping_add(b as u32)}
        times.push(now()-start);
    }
    std::hint::black_box(checksum);times.sort_by(|a,b|a.total_cmp(b));
    let output=std::env::current_exe().unwrap().as_os_str().to_string_lossy().to_string()+".json";
    std::fs::write(output,format!("{{\"hidden_private\":{},\"visible_private\":{},\"hidden_ws\":{},\"visible_ws\":{},\"rank_p50_ms\":{},\"rank_p95_ms\":{},\"checksum\":{}}}\n",hidden.PrivateUsage,visible.PrivateUsage,hidden.WorkingSetSize,visible.WorkingSetSize,times[50],times[95],checksum)).unwrap();
    UnregisterHotKey(panel,1);DestroyWindow(panel);
}}
