"""Explicit end-to-end browser action check. Opens one Bing search for vs code."""
import ctypes as c,subprocess,time,pathlib,os
from ui_test import u,set_text
root=pathlib.Path(__file__).resolve().parents[1]
assert not u.FindWindowW("Spotlight.Window.1",None),"Quit the launcher before this test"
history=pathlib.Path(os.environ['LOCALAPPDATA'])/'Spotlight/history.txt'
before=history.read_bytes() if history.exists() else b''
proc=subprocess.Popen([str(root/'build/release/Spotlight.exe'),'--background'])
try:
    time.sleep(3);h=u.FindWindowW('Spotlight.Window.1',None);assert h
    u.GetLayeredWindowAttributes.argtypes=[c.c_void_p,c.POINTER(c.c_ulong),c.POINTER(c.c_ubyte),c.POINTER(c.c_ulong)]
    color=c.c_ulong();alpha=c.c_ubyte();flags=c.c_ulong()
    uniform=u.GetLayeredWindowAttributes(h,c.byref(color),c.byref(alpha),c.byref(flags))
    assert not uniform or alpha.value==255,'Foreground must not use uniform window fading'
    u.SendMessageW(h,0x8005,0,0);edit=u.GetDlgItem(h,100);listing=u.GetDlgItem(h,101)
    set_text(edit,'readme')
    for _ in range(8):u.SendMessageW(edit,0x100,0x28,0)
    time.sleep(3)
    count=u.SendMessageW(listing,0x1004,0,0)
    assert count>0 and u.SendMessageW(listing,0x100C,-1,2)==count-1,'Web selection not retained after file results'
    set_text(edit,'vs code')
    for _ in range(8):u.SendMessageW(edit,0x100,0x28,0)
    u.SendMessageW(edit,0x100,0x0D,0);time.sleep(3)
    assert not u.IsWindowVisible(h),'Browser launch reported an error'
    after=history.read_bytes() if history.exists() else b''
    assert before==after,'Web query changed launch history'
    print('PASS: foreground has no uniform fading; web selection survives async results; browser launch accepted; no query history written.')
finally:
    if 'h' in locals() and h:u.PostMessageW(h,0x111,19,0)
    proc.wait(timeout=5)
