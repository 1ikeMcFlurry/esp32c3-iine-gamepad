#include "gamepad_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void save(const gamepad_ui_t *ui,const char *directory,const char *name)
{
    char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    FILE *f=fopen(path,"wb");if(!f) exit(1);
    fprintf(f,"P6\n240 320\n255\n");
    uint16_t stripe[240*16];
    for(int y=0;y<320;y+=16) {
        gamepad_ui_render(ui,1500,stripe,y,16);
        for(unsigned i=0;i<240*16;++i) {
            unsigned p=stripe[i];
            unsigned char rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};
            fwrite(rgb,1,3,f);
        }
    }
    fclose(f);
}
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    gamepad_ui_t ui;gamepad_ui_init(&ui);
    save(&ui,argv[1],"empty");
    gamepad_event_t e={.type=GAMEPAD_PEER,.peer={.name="IINE GAMEPAD",.rssi=-43,.candidate=true,.seen=true}};
    e.peer.address[0]=0xA1;e.peer.address[1]=0x24;e.peer.address[2]=0x8B;
    gamepad_ui_event(&ui,&e,1000);
    e.peer.address[0]=0xB2;e.peer.saved=true;e.peer.rssi=-61;strcpy(e.peer.name,"Xbox Wireless Controller");
    gamepad_ui_event(&ui,&e,1000);
    save(&ui,argv[1],"devices");
    gamepad_ui_key(&ui,2,false);ui.link=GP_PAIRING;
    save(&ui,argv[1],"pairing");
    ui.page=UI_LIVE;ui.link=GP_READY;ui.decoded=true;ui.reports=128;
    ui.mapped_buttons=0xFFFF;ui.input.buttons=(1u<<0)|(1u<<3)|(1u<<7)|(1u<<12);
    ui.mapped_axes=ui.input.axes_present=63;
    for(unsigned i=0;i<6;++i) {ui.minimum[i]=0;ui.maximum[i]=65535;ui.input.axes[i]=(i*10000+40000)%65536;}
    ui.hat_min=1;ui.hat_max=8;ui.input.hat=2;
    save(&ui,argv[1],"live");
    ui.decoded=false;ui.raw_length=32;
    for(unsigned i=0;i<32;++i) ui.raw[i]=i*7;
    save(&ui,argv[1],"raw");
    ui.page=UI_DEVICES;strcpy(ui.error,"no BLE HID service");
    save(&ui,argv[1],"error");
    ui.page=UI_FORGET;
    save(&ui,argv[1],"forget");
    ui.page=UI_LIVE;ui.vendor=true;ui.protocol_stage=IINE_WAITING;
    ui.reports=0;ui.raw_length=0;ui.packets=3;strcpy(ui.active.name,"IINE GAMEPAD");
    save(&ui,argv[1],"private_wait");
    ui.protocol_stage=IINE_RECEIVING;ui.reports=21;ui.raw_length=15;ui.packets=154;
    ui.decoded=false;
    for(unsigned i=0;i<15;++i) {ui.raw[i]=i*11;ui.changed_ms[i]=i%3?100:1400;}
    save(&ui,argv[1],"private_raw");
    ui.protocol_stage=IINE_UNKNOWN;ui.reports=0;ui.raw_length=0;ui.packets=0;
    save(&ui,argv[1],"private_unknown");
    ui.protocol_stage=IINE_RECEIVING;ui.decoded=true;ui.raw_view=false;ui.reports=432;
    ui.input.buttons=(1u<<0)|(1u<<6)|(1u<<9);ui.input.dpad=5;
    for(unsigned i=0;i<6;++i) {ui.minimum[i]=0;ui.maximum[i]=255;ui.input.axes[i]=128;}
    ui.input.axes[0]=40;ui.input.axes[1]=64;ui.input.axes[3]=220;ui.input.axes[4]=160;
    ui.button_pressed_ms[1]=1400;
    save(&ui,argv[1],"private_controls");
}
