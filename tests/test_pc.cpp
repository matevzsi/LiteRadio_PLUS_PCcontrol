#include <cassert>
#include <cstdint>
#include <cstring>
extern "C" {
#include "pc_control.h"
}
static void put16(uint8_t *p,uint16_t v) { p[0]=v; p[1]=v>>8; }
void test_pc() {
    uint16_t physical[8]={1500,1500,988,1500,988,2012,988,988};
    uint16_t manual[8]={1500,1500,988,1500,988,988,988,988},out[8];
    uint8_t r[64]={ 'P','C',1,1,0,0,PC_DEFAULT_MASK,0,100,0 };
    for(unsigned i=0;i<8;++i) put16(r+10+2*i,1500);
    put16(r+14,1750); put16(r+16,1250); put16(r+20,2000);
    pc_control_disconnect();
    pc_control_step(physical,manual,out,0,1);
    assert(pcControl.state==PC_MANUAL && out[2]==1000 && out[4]==2000 && out[5]==1500);
    for(unsigned v=988;v<=2012;++v) {
        physical[2]=v;
        pc_control_step(physical,manual,out,0,1);
        unsigned clamped=v<1000?1000:v>2000?2000:v;
        int error=2*((int)out[5]-1500)-((int)clamped-1000);
        assert(error>=-1 && error<=1); // integer rounding is at most half a unit
    }
    physical[2]=1500; pc_control_step(physical,manual,out,0,1);
    assert(out[5]==1750); // center stick -> 50% lift
    physical[2]=988;
    assert(!pc_control_receive(r,64,0)); // physical permission required
    physical[6]=2012;
    pc_control_step(physical,manual,out,0,1);
    assert(pcControl.state==PC_FAILSAFE && pcControl.permit);
    assert(pc_control_receive(r,64,10));
    pc_control_step(physical,manual,out,109,1);
    assert(pcControl.state==PC_ACTIVE && out[2]==1500 && out[3]==1250 && out[5]==2000);
    assert(out[4]==2000); // SB cannot be overridden
    pcControl.mask &= ~(1U<<5);
    pc_control_step(physical,manual,out,109,1);
    assert(out[5]==1500); // unselected lift stays on the remapped manual stick
    pcControl.mask |= 1U<<5;
    pcControl.channel[5]=1000;
    pc_control_step(physical,manual,out,109,1);
    assert(out[5]==1000); // PC -100% lift remains -100%, without a second mix
    pcControl.channel[5]=2000;
    assert(!pc_control_receive(r,64,100)); // duplicate does not refresh watchdog
    put16(r+4,65535); assert(!pc_control_receive(r,64,100)); // backwards
    pc_control_step(physical,manual,out,110,1);
    assert(pcControl.state==PC_FAILSAFE && out[2]==1000 && out[3]==1500 && out[5]==1000);
    put16(r+4,1); assert(!pc_control_receive(r,64,111));
    pc_control_step(physical,manual,out,112,1);
    assert(!pc_control_receive(r,64,112)); // timeout stays latched
    physical[6]=988; pc_control_step(physical,manual,out,113,1);
    assert(pcControl.state==PC_MANUAL);
    physical[6]=2012; pc_control_step(physical,manual,out,114,1);
    put16(r+4,65535); assert(pc_control_receive(r,64,0xfffffff0));
    put16(r+4,0); assert(pc_control_receive(r,64,0xfffffff1)); // wrap
    pc_control_step(physical,manual,out,0x20,1);
    assert(pcControl.state==PC_ACTIVE); // tick wrap
    pc_control_step(physical,manual,out,0x55,1);
    assert(pcControl.state==PC_FAILSAFE);
    physical[6]=988; pc_control_step(physical,manual,out,0,1);
    physical[6]=2012; pc_control_step(physical,manual,out,1,1);
    for(unsigned size=0;size<64;++size) assert(!pc_control_receive(r,size,2));
    r[6]|=0x10; assert(!pc_control_receive(r,64,2)); r[6]=PC_DEFAULT_MASK;
    r[63]=1; assert(!pc_control_receive(r,64,2)); r[63]=0;
    put16(r+8,49); assert(!pc_control_receive(r,64,2)); put16(r+8,251); assert(!pc_control_receive(r,64,2));
    put16(r+8,100);
    for(unsigned i=0;i<8;++i) {
        uint16_t old=r[10+2*i]|r[11+2*i]<<8;
        put16(r+10+2*i,987); assert(!pc_control_receive(r,64,2));
        put16(r+10+2*i,2013); assert(!pc_control_receive(r,64,2));
        put16(r+10+2*i,old);
    }
    assert(pc_control_receive(r,64,2));
    pc_control_step(physical,manual,out,3,0);
    assert(pcControl.state==PC_FAILSAFE && !pcControl.permit);
    pc_control_step(physical,manual,out,4,1);
    assert(!pc_control_receive(r,64,4)); // USB reconnect alone cannot re-arm
    physical[6]=988; pc_control_step(physical,manual,out,5,1);
    physical[6]=2012; pc_control_step(physical,manual,out,6,1);
    assert(pc_control_receive(r,64,6));
    r[3]=0; assert(pc_control_receive(r,64,7));
    pc_control_step(physical,manual,out,8,1); r[3]=1;
    assert(!pc_control_receive(r,64,8)); // STOP latches until switch cycled
    assert(pc_control_mix(1000,200,-100)==1000);
    assert(pc_control_mix(1500,200,-100)==1000);
    assert(pc_control_mix(2000,200,-100)==2000);
    assert(pc_control_mix(2012,200,-100)==2000);
    for(unsigned v=988;v<=2012;++v)
        assert(pc_control_mix(v,100,0)==(v<1000?1000:v>2000?2000:v));
}
