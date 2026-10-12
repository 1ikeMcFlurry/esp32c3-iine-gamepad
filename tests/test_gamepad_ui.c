#include "gamepad_ui.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void peer(gamepad_ui_t *ui,unsigned id,bool candidate,bool saved)
{
    gamepad_event_t e={.type=GAMEPAD_PEER,.peer={.candidate=candidate,.saved=saved,.seen=true,.rssi=-45}};
    e.peer.address[0]=id;
    snprintf(e.peer.name,sizeof(e.peer.name),"Controller %u",id);
    gamepad_ui_event(ui,&e,100);
}
static void test_selection(void)
{
    gamepad_ui_t ui;
    gamepad_ui_init(&ui);
    unsigned indices[GAMEPAD_MAX_PEERS];
    assert(gamepad_ui_visible(&ui,indices)==0);
    assert(gamepad_ui_key(&ui,2,false).command==GP_CMD_REFRESH);
    peer(&ui,1,false,false);
    peer(&ui,2,true,false);
    peer(&ui,3,true,true);
    assert(gamepad_ui_visible(&ui,indices)==2 && indices[0]==1);
    ui_action_t action=gamepad_ui_key(&ui,2,false);
    assert(action.send && action.command==GP_CMD_CONNECT && action.peer.address[0]==2);
    assert(ui.page==UI_CONNECTING);
    action=gamepad_ui_key(&ui,2,true);
    assert(action.send && action.command==GP_CMD_DISCONNECT && ui.page==UI_DEVICES);
    gamepad_ui_key(&ui,1,false);
    action=gamepad_ui_key(&ui,2,true);
    assert(!action.send && ui.page==UI_FORGET);
    action=gamepad_ui_key(&ui,2,true);
    assert(!action.send && ui.page==UI_DEVICES); /* Cancel cannot forget. */
    gamepad_ui_key(&ui,2,true);
    action=gamepad_ui_key(&ui,2,false);
    assert(action.send && action.command==GP_CMD_FORGET && action.peer.address[0]==3);
    gamepad_event_t e={.type=GAMEPAD_FORGOTTEN,.peer=action.peer};
    gamepad_ui_event(&ui,&e,200);
    assert(!ui.peers[2].saved);
    ui.selected=3;
    gamepad_ui_key(&ui,2,false);
    assert(ui.show_all && gamepad_ui_visible(&ui,indices)==3);
    gamepad_ui_key(&ui,0,false);
    assert(ui.selected==4);
    e.type=GAMEPAD_SCAN_CLEAR;
    gamepad_ui_event(&ui,&e,200);
    assert(ui.count==0 && ui.selected==0);
    for(unsigned i=0;i<GAMEPAD_MAX_PEERS;++i) peer(&ui,i+1,false,false);
    peer(&ui,30,true,false);
    assert(ui.count==GAMEPAD_MAX_PEERS && ui.peers[0].address[0]==30);
    peer(&ui,30,true,true);
    assert(ui.count==GAMEPAD_MAX_PEERS && ui.peers[0].saved);
}
static void test_input(void)
{
    gamepad_ui_t ui;
    gamepad_ui_init(&ui);
    gamepad_event_t e={.type=GAMEPAD_LAYOUT,.field={.usage_page=1,.usage=0x30,.logical_min=INT32_MIN,.logical_max=INT32_MAX}};
    gamepad_ui_event(&ui,&e,0);
    e.type=GAMEPAD_INPUT;
    e.decoded=true;
    e.state.axes_present=1;
    e.state.axes[0]=INT32_MIN;
    gamepad_ui_event(&ui,&e,0);
    assert(gamepad_ui_axis(&ui,0,30)==-30);
    ui.input.axes[0]=INT32_MAX;
    assert(gamepad_ui_axis(&ui,0,30)==30);
    ui.input.axes[0]=0;
    assert(abs(gamepad_ui_axis(&ui,0,30))<=1);
    assert(gamepad_ui_axis(&ui,1,30)==0);
    ui.hat_min=1; ui.hat_max=8;
    ui.input.hat=1; assert(gamepad_ui_dpad(&ui)==1);
    ui.input.hat=2; assert(gamepad_ui_dpad(&ui)==5);
    ui.input.hat=8; assert(gamepad_ui_dpad(&ui)==9);
    ui.input.hat=-1; assert(gamepad_ui_dpad(&ui)==0);
    ui.hat_min=0; ui.hat_max=3; ui.input.hat=2;
    assert(gamepad_ui_dpad(&ui)==2);
    e.type=GAMEPAD_LINK;e.link=GP_SCANNING;
    gamepad_ui_event(&ui,&e,0);
    assert(ui.reports==0 && ui.input.axes_present==0 && ui.input.buttons==0 && ui.input.hat==-1);
}
static void test_render(void)
{
    gamepad_ui_t ui;
    gamepad_ui_init(&ui);
    uint16_t *whole=malloc(240*320*2),*stripe=malloc((240*16+2)*2);
    assert(whole && stripe);
    peer(&ui,1,true,true);
    memcpy(ui.active.name,"Invalid \xF0\x9F",11); /* Truncated UTF-8 must not overread. */
    for(unsigned page=0;page<14;++page) {
        ui.page=page<4?page:UI_LIVE;
        ui.vendor=page>=4;
        ui.protocol_stage=page>=4?page-4:0;
        ui.decoded=page>=12;
        ui.raw_view=page==13;
        gamepad_ui_render(&ui,1234,whole,0,320);
        for(int y=0;y<320;y+=16) {
            stripe[0]=stripe[240*16+1]=0xABCD;
            gamepad_ui_render(&ui,1234,stripe+1,y,16);
            assert(stripe[0]==0xABCD && stripe[240*16+1]==0xABCD);
            assert(!memcmp(whole+y*240,stripe+1,240*16*2));
        }
    }
    free(whole); free(stripe);
}
static void test_private(void)
{
    gamepad_ui_t ui; gamepad_ui_init(&ui);
    gamepad_event_t e={.type=GAMEPAD_PROTOCOL,.protocol_stage=IINE_WAITING,.packets=3};
    gamepad_ui_event(&ui,&e,100);
    assert(ui.page==UI_LIVE && ui.vendor && ui.packets==3);
    e.type=GAMEPAD_INPUT; e.length=2; e.raw[0]=0x80;e.raw[1]=0;
    gamepad_ui_event(&ui,&e,200);
    e.raw[1]=1;gamepad_ui_event(&ui,&e,300);
    assert(ui.changed_ms[0]==200 && ui.changed_ms[1]==300 && ui.reports==2);
    assert(!ui.decoded);
    e.decoded=true;e.state.buttons=1;
    gamepad_ui_event(&ui,&e,310);
    assert(ui.button_pressed_ms[0]==310);
    e.state.buttons=0;gamepad_ui_event(&ui,&e,320);
    assert(ui.button_pressed_ms[0]==310 && ui.input.buttons==0);
    assert(!gamepad_ui_key(&ui,2,false).send && ui.raw_view);
    gamepad_ui_key(&ui,2,false);assert(!ui.raw_view);
    e.type=GAMEPAD_LINK;e.link=GP_SCANNING;
    gamepad_ui_event(&ui,&e,400);
    assert(!ui.vendor && !ui.reports && !ui.packets && !ui.changed_ms[0]);
    assert(!ui.button_pressed_ms[0] && !ui.raw_view);
}
int main(void)
{
    test_selection(); test_input(); test_render(); test_private();
    puts("UI tests passed: selection, forget/cancel, capacity, axes, hat, disconnect, stripe bounds");
}
