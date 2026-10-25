"""Exercise the native UI and measure this process; does not launch search results."""
import ctypes as c
from ctypes import wintypes as w
import subprocess, time, json, pathlib, argparse
u=c.WinDLL("user32",use_last_error=True);k=c.WinDLL("kernel32",use_last_error=True);p=c.WinDLL("psapi",use_last_error=True)
u.FindWindowW.argtypes=[w.LPCWSTR,w.LPCWSTR];u.FindWindowW.restype=w.HWND
u.GetDlgItem.argtypes=[w.HWND,c.c_int];u.GetDlgItem.restype=w.HWND
u.SendMessageW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM];u.SendMessageW.restype=c.c_ssize_t
u.IsWindowVisible.argtypes=[w.HWND];u.IsWindowVisible.restype=w.BOOL
u.PostMessageW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM]
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.CloseHandle.argtypes=[w.HANDLE]
class Memory(c.Structure):
    _fields_=[("cb",w.DWORD),("faults",w.DWORD)]+[(x,c.c_size_t) for x in ["peak_ws","ws","peak_paged","paged","peak_nonpaged","nonpaged","pagefile","peak_pagefile","private"]]
class Io(c.Structure):
    _fields_=[(x,c.c_ulonglong) for x in ["read_ops","write_ops","other_ops","read_bytes","write_bytes","other_bytes"]]
p.GetProcessMemoryInfo.argtypes=[w.HANDLE,c.POINTER(Memory),w.DWORD]
k.GetProcessIoCounters.argtypes=[w.HANDLE,c.POINTER(Io)]
k.GetProcessTimes.argtypes=[w.HANDLE,c.POINTER(w.FILETIME),c.POINTER(w.FILETIME),c.POINTER(w.FILETIME),c.POINTER(w.FILETIME)]
u.GetGuiResources.argtypes=[w.HANDLE,w.DWORD];u.GetGuiResources.restype=w.DWORD
k.GetProcessHandleCount.argtypes=[w.HANDLE,c.POINTER(w.DWORD)]
def metrics(handle):
    m=Memory();m.cb=c.sizeof(m);assert p.GetProcessMemoryInfo(handle,c.byref(m),m.cb)
    io=Io();k.GetProcessIoCounters(handle,c.byref(io))
    times=[w.FILETIME() for _ in range(4)];k.GetProcessTimes(handle,*[c.byref(x) for x in times])
    cpu=sum((x.dwHighDateTime<<32)|x.dwLowDateTime for x in times[2:])/1e7
    handles=w.DWORD();k.GetProcessHandleCount(handle,c.byref(handles))
    return dict(private=m.private,working_set=m.ws,cpu_seconds=cpu,handles=handles.value,gdi=u.GetGuiResources(handle,0),user=u.GetGuiResources(handle,1),write_bytes=io.write_bytes,write_ops=io.write_ops)
def set_text(edit,text):
    buf=c.create_unicode_buffer(text)
    u.SendMessageW(edit,0xC,0,c.cast(buf,c.c_void_p).value)
def percentile(values,f):
    return sorted(values)[min(len(values)-1,int(len(values)*f))]
def main():
    a=argparse.ArgumentParser();a.add_argument("--idle",type=int,default=600);a.add_argument("--launch-test",action="store_true");a.add_argument("--cycles",type=int,default=1000);args=a.parse_args()
    assert args.cycles>=1000
    root=pathlib.Path(__file__).resolve().parents[1];exe=root/"build/release/Spotlight.exe"
    assert not u.FindWindowW("Spotlight.Window.1",None),"Quit the existing launcher before this test."
    proc=subprocess.Popen([str(exe),"--background"],creationflags=subprocess.CREATE_NO_WINDOW)
    handle=k.OpenProcess(0x1000|0x10,False,proc.pid)
    try:
        for _ in range(100):
            window=u.FindWindowW("Spotlight.Window.1",None)
            if window:break
            assert proc.poll() is None,"Launcher exited during startup"
            time.sleep(.1)
        assert window,"Native window not created"
        edit=u.GetDlgItem(window,100);listing=u.GetDlgItem(window,101)
        assert edit and listing
        time.sleep(3)
        u.SendMessageW(window,0x8005,0,0)
        assert u.IsWindowVisible(window)
        time.sleep(1)
        initial_count=u.SendMessageW(listing,0x1004,0,0)
        assert 0<initial_count<=8,initial_count
        # A second executable activates the existing process; it must not hide it.
        subprocess.run([str(exe)],check=True,timeout=5)
        time.sleep(.2);assert u.IsWindowVisible(window),"Second instance hid the panel"
        # A real focus/outside click can dismiss the app during the wait. Retry
        # that interrupted setup; still fail any incorrect selection while visible.
        for attempt in range(3):
            if not u.IsWindowVisible(window):u.SendMessageW(window,0x8005,0,0)
            set_text(edit,"a")
            if u.SendMessageW(listing,0x1004,0,0)<3:break
            u.SendMessageW(edit,0x100,0x28,0);u.SendMessageW(edit,0x100,0x28,0)
            time.sleep(3)
            if not u.IsWindowVisible(window):continue
            selected=u.SendMessageW(listing,0x100C,-1,2)
            assert selected==2,f"Selection changed while visible: {selected}"
            break
        else:raise InterruptedError("Desktop focus repeatedly changed during the selection check")
        set_text(edit,"spotlight_no_matching_file_3d937")
        time.sleep(3)
        assert u.SendMessageW(listing,0x1004,0,0)==1,"Miss should leave only the explicit web action"
        set_text(edit,"C:\\")
        assert u.SendMessageW(listing,0x1004,0,0)>=1,"Explicit path missing"
        if args.launch_test:
            fixture=root/"build/release/launch_fixture.exe"
            marker=pathlib.Path(str(fixture)+".launched")
            marker.unlink(missing_ok=True)
            set_text(edit,str(fixture))
            u.SendMessageW(edit,0x100,0x0D,0)
            for _ in range(50):
                if marker.exists():break
                time.sleep(.1)
            assert marker.read_text()=="ok","Shell launch did not execute the fixture"
            time.sleep(.5)
            u.SendMessageW(window,0x8005,0,0)
        set_text(edit,"");time.sleep(.2)
        u.SendMessageW(window,0x10,0,0);time.sleep(.2)
        before=metrics(handle)
        times=[];midpoint=None
        for i in range(args.cycles):
            start=time.perf_counter()
            u.SendMessageW(window,0x8005,0,0)
            set_text(edit,["code","notepad","zzzzzz","\u0939\u093f\u0928\u094d\u0926\u0940",""][i%5])
            assert u.SendMessageW(listing,0x1004,0,0)<=8
            u.SendMessageW(window,0x10,0,0)
            times.append((time.perf_counter()-start)*1000)
            if i%100==0:time.sleep(.03)
            if i==999:midpoint=metrics(handle);print("After 1000 cycles: "+json.dumps(midpoint),flush=True)
        time.sleep(2);after=metrics(handle)
        print(json.dumps({"cycles":args.cycles,"before":before,"after":after,"cycle_p95_ms":percentile(times,.95)}),flush=True)
        # Idle sampling is passive; no timers or messages are injected into the app.
        idle_before=metrics(handle);start=time.monotonic()
        while time.monotonic()-start<args.idle:
            time.sleep(min(30,args.idle-(time.monotonic()-start)))
            assert proc.poll() is None
            print("Idle observation: %ds"%int(time.monotonic()-start),flush=True)
        idle_after=metrics(handle);elapsed=time.monotonic()-start
        import os
        result=dict(cycles=args.cycles,midpoint=midpoint,initial_results=initial_count,before=before,after=after,
            cycle_p50_ms=percentile(times,.5),cycle_p95_ms=percentile(times,.95),
            idle_seconds=elapsed,idle_before=idle_before,idle_after=idle_after,
            idle_cpu_percent=(idle_after["cpu_seconds"]-idle_before["cpu_seconds"])/elapsed/(os.cpu_count() or 1)*100,
            idle_write_bytes=idle_after["write_bytes"]-idle_before["write_bytes"])
        output=root/"build/ui-metrics.json";output.write_text(json.dumps(result,indent=2))
        print(json.dumps(result),flush=True)
        assert after["gdi"]<=before["gdi"]+2,"GDI leak"
        assert after["user"]<=before["user"]+2,"USER object leak"
        assert after["private"]<before["private"]+3*1024*1024,"Unbounded interaction memory growth"
    finally:
        # Send the normal Quit command only to the test-owned process.
        if 'window' in locals() and window:u.PostMessageW(window,0x111,19,0)
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.terminate()
        if handle:k.CloseHandle(handle)
if __name__=="__main__":main()


