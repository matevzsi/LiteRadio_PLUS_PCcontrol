"""Exercise the actual keyboard handlers using fake Tk, time and HID client."""
import argparse
import ast
from pathlib import Path
import sys
from types import SimpleNamespace


class Radio:
    battery = status = None
    status_received_at = battery_received_at = None
    def __init__(self): self.controls=[]; self.closed=False
    def send_control(self, **values): self.controls.append(values)
    def read_telemetry(self): return None
    def close(self): self.closed=True


class Label:
    def __init__(self,*a,**kw): pass
    def pack(self,**kw): pass


class Canvas(Label):
    def create_text(self,*a,**kw): return 1
    def create_rectangle(self,*a,**kw): return 2
    def coords(self,*a): pass
    def itemconfigure(self,*a,**kw): pass


class Variable:
    def __init__(self,value=''): self.value=value
    def set(self,value): self.value=value


class Window:
    def __init__(self): self.handlers={}; self.pending={}; self.serial=0; self.closed=False
    def configure(self,**kw): pass
    def title(self,*a): pass
    def geometry(self,*a): pass
    def resizable(self,*a): pass
    def bind(self,event,fn): self.handlers[event]=fn
    def protocol(self,event,fn): self.handlers[event]=fn
    def after(self,ms,fn):
        self.serial+=1; self.pending[self.serial]=fn; return self.serial
    def after_cancel(self,ident): self.pending.pop(ident,None)
    def destroy(self): self.closed=True
    def press(self,key): self.handlers['<KeyPress>'](SimpleNamespace(keysym=key))
    def release(self,key): self.handlers['<KeyRelease>'](SimpleNamespace(keysym=key))
    def tick(self):
        ident=next(k for k,v in self.pending.items() if v.__name__=='tick')
        now[0]+=.04
        self.pending.pop(ident)()
    def mainloop(self):
        self.tick()
        assert radio.controls[-1]==dict(thrust=-100,steer=0,lift=-100)
        self.press('w');self.press('d');self.press('space')
        for _ in range(20): self.tick()
        assert radio.controls[-1]==dict(thrust=-40,steer=50,lift=-30)
        first=radio.controls[1]
        assert abs(first['thrust']-(-100+120*.04))<1e-8
        assert abs(first['steer']-225*.04)<1e-8
        assert abs(first['lift']-(-100+150*.04))<1e-8
        # Auto-repeat must not toggle lift a second time.
        self.press('space');self.release('space');self.press('space');self.tick()
        assert radio.controls[-1]['lift']==-30
        self.press('a') # opposing A/D center
        for _ in range(8): self.tick()
        assert radio.controls[-1]['steer']==0
        self.handlers['<FocusOut>'](None)
        assert radio.controls[-1]==dict(thrust=-100,steer=0,lift=-100)
        self.tick()
        assert radio.controls[-1]==dict(thrust=-100,steer=0,lift=-100)
        self.press('escape')
        assert self.closed and radio.closed


radio,root,now=Radio(),Window(),[0.0]
tk=SimpleNamespace(Tk=lambda:root,Label=Label,Canvas=Canvas,StringVar=Variable,Event=object)
scope=dict(argparse=argparse,time=SimpleNamespace(monotonic=lambda:now[0]),
           tk=tk,LiteRadioClient=lambda **kw:radio,__name__='keyboard_test')
source=Path(__file__).resolve().parents[1]/'tools/keyboard_control.py'
tree=ast.parse(source.read_text())
tree.body=[n for n in tree.body if not isinstance(n,(ast.Import,ast.ImportFrom))]
exec(compile(tree,str(source),'exec'),scope)
sys.argv=['keyboard_control.py']
scope['main']()
print('PASS: keyboard targets, slew rates, opposing keys, repeat handling, focus reset and close')

class Bar:
    def set(self,fraction,text,color=None): self.value=(fraction,text,color)
bars={name:Bar() for name in ['signal','battery','link','arm']}
update=scope['update_telemetry']
update(bars,radio,10,3.0,4.35)
assert 'No vehicle' in bars['battery'].value[1]
assert 'Waiting' in bars['link'].value[1]
radio.status=dict(radio_powered=True,link_state=2,rssi_dbm=-80,lq=95)
radio.battery=dict(voltage_v=4.1,consumed_mah=35)
radio.status_received_at=radio.battery_received_at=10
update(bars,radio,10.1,3.0,4.35)
assert bars['signal'].value[:2]==(.5,'-80 dBm')
assert bars['link'].value==(.95,'Connected | LQ 95%','#39cc91')
assert '4.10 V' in bars['battery'].value[1] and bars['battery'].value[0]>0
radio.status_received_at=16
update(bars,radio,16,3.0,4.35)
assert 'stale' in bars['battery'].value[1] and bars['battery'].value[0]==0
radio.status['link_state']=0
update(bars,radio,16,3.0,4.35)
assert bars['link'].value==(0,'RF disconnected','#ef6461')
update(bars,radio,20,3.0,4.35)
assert bars['link'].value==(0,'USB status stale','#93a0b2')
print('PASS: telemetry bars distinguish live, missing, stale and disconnected data')

radio.status_received_at=20
radio.status['radio_powered']=True
radio.status['arm_command']=True
update(bars,radio,20,3.0,4.35)
assert bars['arm'].value==(1,'ARM requested','#ef6461')
radio.status['arm_command']=False
update(bars,radio,20,3.0,4.35)
assert bars['arm'].value==(0,'DISARM requested','#39cc91')
update(bars,radio,22,3.0,4.35)
assert bars['arm'].value==(0,'Unknown / unavailable','#93a0b2')
print('PASS: arm command display and stale-state handling')
