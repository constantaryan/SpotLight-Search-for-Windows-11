"""Send synthetic Win+Space chords through the real keyboard hook."""
import ctypes as c, subprocess,time,pathlib,json
from ui_test import u,k,metrics
root=pathlib.Path(__file__).resolve().parents[1]
assert not u.FindWindowW("Spotlight.Window.1",None),"Quit existing launcher first"
proc=subprocess.Popen([str(root/"build/release/Spotlight.exe"),"--background"])
u.keybd_event.argtypes=[c.c_ubyte,c.c_ubyte,c.c_ulong,c.c_size_t]
def key(v,up=False):u.keybd_event(v,0,2 if up else 0,0);time.sleep(.08)
try:
    time.sleep(3);h=u.FindWindowW("Spotlight.Window.1",None);assert h
    for win in [0x5b,0x5c]:
        assert not u.IsWindowVisible(h)
        key(win);key(32);assert u.IsWindowVisible(h),"Chord did not show launcher"
        key(32);assert u.IsWindowVisible(h),"Repeat toggled launcher"
        key(32,True);key(win,True);time.sleep(.3)
        assert u.IsWindowVisible(h),"Windows release stole focus / opened Start"
        key(win);key(32);assert not u.IsWindowVisible(h),"Chord did not hide launcher"
        key(win,True);key(32,True)
    handle=k.OpenProcess(0x1000|0x10,False,proc.pid)
    before=metrics(handle);time.sleep(60);after=metrics(handle);k.CloseHandle(handle)
    result=dict(shortcut="Left and right Windows+Space",repeat="passed",release_order="passed",idle_seconds=60,before=before,after=after)
    (root/"build/shortcut-metrics.json").write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
finally:
    for v in [32,0x5b,0x5c]:u.keybd_event(v,0,2,0)
    if 'h' in locals() and h:u.PostMessageW(h,0x111,19,0)
    proc.wait(timeout=5)
