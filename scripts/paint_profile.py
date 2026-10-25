"""Development-only stage timing; requires SL_PROFILE_PAINT build."""
import subprocess,time,pathlib,json
from ui_test import u,set_text
root=pathlib.Path(__file__).resolve().parents[1];assert not u.FindWindowW('Spotlight.Window.1',None)
proc=subprocess.Popen([str(root/'build/release/Spotlight.exe'),'--background'])
try:
    time.sleep(3);h=u.FindWindowW('Spotlight.Window.1',None);assert h
    u.SendMessageW(h,0x8005,0,0);edit=u.GetDlgItem(h,100);set_text(edit,'code');time.sleep(2)
    u.SendMessageW(h,0x8029,0,0)
    for _ in range(40):u.SendMessageW(h,0x802a,0,0)
    result={'frames':u.SendMessageW(h,0x8028,0,0),'stages_ms':{name:u.SendMessageW(h,0x8028,i+1,0)/1000 for i,name in enumerate(['clear','chrome','input','rows','backdrop','present'])}}
    assert result['frames']>=40,result
    u.SendMessageW(h,0x10,0,0);u.SendMessageW(h,0x8029,0,0)
    for i in range(20):
        u.SendMessageW(h,0x8005,0,0);set_text(edit,['code','notepad','zzzzzz',''][i%4]);u.SendMessageW(h,0x10,0,0)
    result['cycles']=20;result['cycle_frames']=u.SendMessageW(h,0x8028,0,0)
    result['cycle_stages_ms']={name:u.SendMessageW(h,0x8028,i+1,0)/1000 for i,name in enumerate(['clear','chrome','input','rows','backdrop','present'])}
    print(json.dumps(result));(root/'build/paint-profile.json').write_text(json.dumps(result,indent=2))
finally:
    if 'h' in locals() and h:u.PostMessageW(h,0x111,19,0)
    proc.wait(timeout=5)
