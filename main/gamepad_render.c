#include "gamepad_ui.h"
#include <stdio.h>
#include <string.h>
#include "ui_font.inc"

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))
#define BG RGB(243,246,250)
#define INK RGB(23,39,62)
#define MUTED RGB(88,107,131)
#define BLUE RGB(27,91,221)
#define PALE RGB(222,234,252)
#define LINE RGB(209,220,233)
#define GREEN RGB(0,119,90)
#define RED RGB(177,49,42)
#define WHITE 0xffff

typedef struct { uint16_t *pixels; int y, height; } canvas_t;
static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }
static void dot(canvas_t *c, int x, int y, uint16_t color)
{
    if (x >= 0 && x < 240 && y >= c->y && y < c->y + c->height)
        c->pixels[(y-c->y)*240+x] = color;
}
static void rect(canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    int end_y = min(y+h, c->y+c->height), end_x = min(x+w,240);
    for (int yy = max(y,c->y); yy < end_y; ++yy)
        for (int xx = max(x,0); xx < end_x; ++xx) dot(c,xx,yy,color);
}
static void circle(canvas_t *c, int x, int y, int r, uint16_t color)
{
    for (int yy=max(y-r,c->y); yy<=min(y+r,c->y+c->height-1); ++yy)
        for (int xx=max(x-r,0); xx<=min(x+r,239); ++xx)
            if ((xx-x)*(xx-x)+(yy-y)*(yy-y)<=r*r) dot(c,xx,yy,color);
}
static void roundrect(canvas_t *c,int x,int y,int w,int h,int r,uint16_t color)
{
    rect(c,x+r,y,w-2*r,h,color);
    rect(c,x,y+r,w,h-2*r,color);
    circle(c,x+r,y+r,r,color); circle(c,x+w-r-1,y+r,r,color);
    circle(c,x+r,y+h-r-1,r,color); circle(c,x+w-r-1,y+h-r-1,r,color);
}
static uint32_t codepoint(const char **p)
{
    const unsigned char *s=(const unsigned char *)*p;
    uint32_t value=*s++;
    unsigned n=0;
    if (value >= 0xc2 && value < 0xe0) { value &= 31; n=1; }
    else if(value>=0xe0 && value<0xf0) { value &= 15; n=2; }
    else if(value>=0xf0 && value<0xf5) { value &= 7; n=3; }
    else if(value>=0x80) value='?';
    for(unsigned i=0;i<n;++i) {
        if ((*s & 0xc0)!=0x80) { value='?'; break; }
        value=(value<<6)|(*s++ & 63);
    }
    *p=(const char *)s;
    return value;
}
static const font_glyph_t *glyph(uint32_t value, unsigned font)
{
    unsigned lo=0,hi=FONT_GLYPH_COUNT;
    while(lo<hi) { unsigned mid=(lo+hi)/2; if(font_codepoints[mid]<value) lo=mid+1; else hi=mid; }
    if(lo==FONT_GLYPH_COUNT || font_codepoints[lo]!=value) lo='?'-32;
    return &font_glyphs[font][lo];
}
static uint16_t blend(uint16_t a,uint16_t b,unsigned alpha)
{
    unsigned r=(((a>>11)&31)*(3-alpha)+((b>>11)&31)*alpha)/3;
    unsigned g=(((a>>5)&63)*(3-alpha)+((b>>5)&63)*alpha)/3;
    unsigned bl=((a&31)*(3-alpha)+(b&31)*alpha)/3;
    return (r<<11)|(g<<5)|bl;
}
static int text_width(const char *s,unsigned font)
{
    int w=0;
    while(*s) w+=glyph(codepoint(&s),font)->width;
    return w;
}
static void text(canvas_t *c,int x,int y,const char *s,unsigned font,uint16_t color,int limit)
{
    int end=x+limit;
    while(*s) {
        const font_glyph_t *g=glyph(codepoint(&s),font);
        if(x+g->width>end) break;
        for(int yy=max(0,c->y-y);yy<min(font_heights[font],c->y+c->height-y);++yy)
            for(int xx=0;xx<g->width;++xx) {
                unsigned bit=(yy*g->width+xx)*2;
                unsigned alpha=(font_bitmap[g->offset+bit/8]>>(6-bit%8))&3;
                int px=x+xx,py=y+yy;
                if(alpha && px>=0 && px<240 && py>=c->y && py<c->y+c->height) {
                    uint16_t *dest=&c->pixels[(py-c->y)*240+px];
                    *dest=blend(*dest,color,alpha);
                }
            }
        x+=g->width;
    }
}
static void center(canvas_t *c,int x,int y,const char *s,unsigned font,uint16_t color)
{ text(c,x-text_width(s,font)/2,y,s,font,color,240); }
static void clipped_name(canvas_t *c,int x,int y,const char *s,uint16_t color,int width)
{
    bool truncated=text_width(s,1)>width;
    text(c,x,y,s,1,color,width-(truncated?16:0));
    if(truncated) text(c,x+width-15,y,"..",1,color,15);
}
static void header(canvas_t *c,const char *title,const char *badge,uint16_t accent)
{
    text(c,16,15,title,3,INK,170);
    roundrect(c,174,19,50,23,7,accent);
    center(c,199,22,badge,0,WHITE);
}
static void footer(canvas_t *c,const char *line1,const char *line2)
{
    rect(c,16,280,208,1,LINE);
    center(c,120,286,line1,0,INK);
    if(line2) center(c,120,301,line2,0,MUTED);
}
static void devices(canvas_t *c,const gamepad_ui_t *ui,uint32_t now)
{
    bool no_hid=strstr(ui->error,"no BLE HID")!=NULL;
    header(c,"选择手柄",no_hid?"不支持":ui->error[0]?"重试":"搜索中",ui->error[0]?RED:BLUE);
    text(c,17,46,"IINE L1241  /  v0.3.1",0,MUTED,207);
    roundrect(c,14,69,212,47,7,ui->error[0]?RGB(255,230,224):PALE);
    text(c,23,74,no_hid?"当前未提供手柄输入服务":ui->error[0]?"连接失败，请重进配对模式":"手柄：配对 + A 长按3秒",1,ui->error[0]?RED:INK,195);
    if(ui->error[0]) {
        const char *message=no_hid?"可尝试配对+A；串口有服务诊断":
                            strstr(ui->error,"passkey")?"需要密码，请切换手柄模式":"重新选择设备即可重试";
        text(c,23,94,message,0,RED,195);
    } else text(c,23,94,"选择 IINE GAMEPAD 进行适配",0,MUTED,195);
    unsigned indices[GAMEPAD_MAX_PEERS],n=gamepad_ui_visible(ui,indices),rows=n+2;
    unsigned selected=ui->selected<rows?ui->selected:0;
    unsigned first=selected>=4?selected-3:0;
    int top=123;
    if(!n) {
        center(c,120,128,"等待手柄出现",2,INK);
        center(c,120,155,"也可切换查看所有蓝牙设备",0,MUTED);
        top=181;
    }
    for(unsigned row=first;row<rows && row<first+4;++row) {
        int y=top+(int)(row-first)*38;
        bool focus=row==selected;
        roundrect(c,14,y,212,35,6,focus?BLUE:WHITE);
        uint16_t fg=focus?WHITE:INK,secondary=focus?WHITE:MUTED;
        if(row<n) {
            unsigned idx=indices[row];
            const gamepad_peer_t *p=&ui->peers[idx];
            clipped_name(c,23,y+1,p->name,fg,190);
            char detail[100];
            bool online=p->seen && (uint32_t)(now-ui->seen_ms[idx])<15000;
            if(online) snprintf(detail,sizeof(detail),"%02X:%02X:%02X  %d dBm  %s",p->address[2],p->address[1],p->address[0],p->rssi,p->saved?"已配对":"可选择");
            else snprintf(detail,sizeof(detail),"%02X:%02X:%02X  %s",p->address[2],p->address[1],p->address[0],p->saved?"已保存 / 唤醒后重连":"未再发现 / 可重试");
            text(c,23,y+19,detail,0,secondary,192);
        } else text(c,24,y+8,row==n?"刷新设备列表":ui->show_all?"只看手柄设备":"查看所有蓝牙设备",1,fg,191);
    }
    footer(c,"工牌：上下选择 · 确定连接","已配对设备：长按确定可取消配对");
}
static void connecting(canvas_t *c,const gamepad_ui_t *ui,uint32_t now)
{
    header(c,"连接手柄","配对中",BLUE);
    clipped_name(c,17,50,ui->active.name,MUTED,207);
    const char *steps[]={"连接蓝牙设备","配对并建立加密","读取手柄输入服务","进入实时操作界面"};
    unsigned step=ui->link==GP_PAIRING?1:ui->link==GP_DISCOVERING?2:0;
    for(unsigned i=0;i<4;++i) {
        int y=99+i*39;
        if(i<3) rect(c,32,y+12,2,39,LINE);
        circle(c,33,y+11,11,i<=step?BLUE:LINE);
        char num[4]; snprintf(num,sizeof(num),"%u",i+1);
        center(c,33,y+3,num,0,i<=step?WHITE:MUTED);
        text(c,55,y+1,steps[i],1,i<=step?INK:MUTED,175);
        if(i==step) circle(c,217,y+12,3+(now/350)%3,BLUE);
    }
    footer(c,"请保持手柄在配对模式","长按工牌确定键：取消并返回");
}
static void live(canvas_t *c,const gamepad_ui_t *ui)
{
    header(c,"手柄测试","已连接",GREEN);
    char label[70];
    snprintf(label,sizeof(label),"输入 #%lu  %s",(unsigned long)ui->reports,ui->reports?(ui->decoded?"实时显示":"原始数据"):"等待操作");
    text(c,17,47,label,0,MUTED,207);
    if(ui->reports && !ui->decoded) {
        roundrect(c,14,75,212,189,8,WHITE);
        text(c,25,89,"已收到未识别的输入",2,RED,190);
        text(c,25,118,"请切换手柄模式后重新连接",0,MUTED,190);
        snprintf(label,sizeof(label),"Report %u / %u bytes",ui->report_id,ui->raw_length);
        text(c,25,147,label,1,INK,190);
        for(unsigned line=0;line<4;++line) {
            char hex[32]={0};
            for(unsigned i=0;i<8 && line*8+i<ui->raw_length;++i)
                snprintf(hex+i*3,sizeof(hex)-i*3,"%02X ",ui->raw[line*8+i]);
            text(c,25,174+line*20,hex,0,BLUE,190);
        }
    } else {
        circle(c,65,111,37,LINE); circle(c,65,111,35,WHITE);
        rect(c,36,111,59,1,LINE); rect(c,65,82,1,59,LINE);
        int dx=gamepad_ui_axis(ui,0,27),dy=gamepad_ui_axis(ui,1,27);
        circle(c,65+dx,111+dy,7,BLUE);
        center(c,65,153,"摇杆 X / Y",0,MUTED);
        uint8_t pad=gamepad_ui_dpad(ui);
        roundrect(c,160,75,25,23,4,(pad&1)?BLUE:LINE);
        roundrect(c,160,128,25,23,4,(pad&2)?BLUE:LINE);
        roundrect(c,187,101,25,24,4,(pad&4)?BLUE:LINE);
        roundrect(c,133,101,25,24,4,(pad&8)?BLUE:LINE);
        circle(c,172,113,8,WHITE);
        center(c,172,153,"方向键",0,MUTED);
        const char *axes[]={"X","Y","Z","Rx","Ry","Rz"};
        for(unsigned a=0;a<6;++a) {
            int x=17+a*35;
            bool present=ui->input.axes_present&(1u<<a);
            center(c,x+15,173,axes[a],0,MUTED);
            rect(c,x,191,30,5,LINE);
            if(present) { int level=gamepad_ui_axis(ui,a,14)+14; rect(c,x,191,level+1,5,BLUE); }
        }
        text(c,17,203,"按键",0,MUTED,50);
        text(c,118,203,"按下即高亮",0,MUTED,108);
        for(unsigned b=0;b<32;++b) {
            int x=16+(b%8)*26,y=219+(b/8)*15;
            bool down=ui->input.buttons&(1u<<b),mapped=ui->mapped_buttons&(1u<<b);
            roundrect(c,x,y,24,14,3,down?BLUE:mapped?WHITE:LINE);
            snprintf(label,sizeof(label),"%u",b+1);
            center(c,x+12,y-1,label,0,down?WHITE:mapped?INK:MUTED);
        }
    }
    footer(c,"长按工牌确定键：断开并返回", "按键编号对应 HID 定义");
}

static uint16_t button_color(bool down,uint32_t pressed,uint32_t now)
{
    return down?BLUE:pressed && (uint32_t)(now-pressed)<600?PALE:WHITE;
}
static void private_button(canvas_t *c,const gamepad_ui_t *ui,uint32_t now,
                           unsigned index,const char *label,int x,int y,int w,int h)
{
    bool down=ui->input.buttons&(1u<<index);
    roundrect(c,x,y,w,h,4,button_color(down,ui->button_pressed_ms[index],now));
    center(c,x+w/2,y+(h-15)/2,label,0,down?WHITE:INK);
}
static void private_controls(canvas_t *c,const gamepad_ui_t *ui,uint32_t now)
{
    header(c,"手柄实时测试","已连接",GREEN);
    char label[64];
    snprintf(label,sizeof(label),"L1241   操作数据 %lu",(unsigned long)ui->reports);
    text(c,17,47,label,0,MUTED,207);
    for(unsigned side=0;side<2;++side) {
        int x=side?176:64; unsigned axis=side?3:0;
        circle(c,x,105,31,LINE);circle(c,x,105,29,WHITE);
        rect(c,x-25,105,51,1,LINE);rect(c,x,80,1,51,LINE);
        circle(c,x+gamepad_ui_axis(ui,axis,23),105+gamepad_ui_axis(ui,axis+1,23),6,BLUE);
        snprintf(label,sizeof(label),"%s %ld / %ld",side?"右":"左",(long)ui->input.axes[axis],(long)ui->input.axes[axis+1]);
        center(c,x,141,label,0,MUTED);
    }
    const char *shoulders[]={"L","R","ZL","ZR"};
    for(unsigned i=0;i<4;++i) private_button(c,ui,now,i+4,shoulders[i],16+i*53,164,49,21);
    static const int px[]={54,54,78,30},py[]={191,239,215,215};
    static const char *directions[]={"上","下","右","左"};
    for(unsigned i=0;i<4;++i) {
        bool down=ui->input.dpad&(1u<<i);
        roundrect(c,px[i],py[i],22,21,4,button_color(down,ui->dpad_pressed_ms[i],now));
        center(c,px[i]+11,py[i]+3,directions[i],0,down?WHITE:INK);
    }
    circle(c,65,225,6,LINE);
    static const int bx[]={200,175,175,150},by[]={224,248,200,224};
    static const char *face[]={"A","B","X","Y"};
    for(unsigned i=0;i<4;++i) {
        bool down=ui->input.buttons&(1u<<i);
        circle(c,bx[i],by[i],12,LINE);
        circle(c,bx[i],by[i],11,button_color(down,ui->button_pressed_ms[i],now));
        center(c,bx[i],by[i]-8,face[i],1,down?WHITE:INK);
    }
    const char *small[]={"-","+","L3","R3"};
    for(unsigned i=0;i<4;++i) private_button(c,ui,now,i+8,small[i],16+i*53,263,49,15);
    footer(c,"短按看数据 · 长按断开返回","蓝色为按住 · 浅蓝为刚按过");
}

static void private_live(canvas_t *c,const gamepad_ui_t *ui,uint32_t now)
{
    if(ui->decoded && !ui->raw_view) {private_controls(c,ui,now);return;}
    bool received=ui->protocol_stage==IINE_RECEIVING;
    bool unavailable=ui->protocol_stage==IINE_UNKNOWN || ui->protocol_stage==IINE_NO_INPUT;
    header(c,"IINE 适配测试",received?"有数据":unavailable?"待适配":"已连接",received?GREEN:unavailable?RED:BLUE);
    clipped_name(c,17,46,ui->active.name,MUTED,207);
    const char *title="正在识别私有协议",*hint="请保持手柄开启";
    switch(ui->protocol_stage) {
    case IINE_CAPABILITIES: title="协议已识别，查询能力"; break;
    case IINE_STARTING: title="正在开启按键测试"; break;
    case IINE_WAITING: title="等待手柄操作数据"; hint="请按键、移动摇杆观察变化"; break;
    case IINE_RECEIVING: title="已收到实时数据"; hint=ui->decoded?"短按确定键返回操作界面":"字节变化会亮起，映射待确认"; break;
    case IINE_NO_INPUT: title="已连接，尚无输入数据"; hint="按键后仍无变化则需继续适配"; break;
    case IINE_UNKNOWN: title="私有协议暂未识别"; hint="尚未开启测试，可返回重试"; break;
    default: break;
    }
    roundrect(c,14,72,212,49,7,unavailable?RGB(255,230,224):PALE);
    text(c,23,78,title,1,unavailable?RED:INK,195);
    text(c,23,100,hint,0,MUTED,195);
    char label[80];
    snprintf(label,sizeof(label),"接收 %lu  校验失败 %lu",(unsigned long)ui->packets,(unsigned long)ui->bad_packets);
    text(c,17,130,label,0,MUTED,207);
    snprintf(label,sizeof(label),"数据变化 %lu  /  %u bytes",(unsigned long)ui->reports,ui->raw_length);
    text(c,17,149,label,0,MUTED,207);
    for(unsigned i=0;i<15;++i) {
        int x=15+(i%3)*71,y=175+(i/3)*18;
        bool present=i<ui->raw_length;
        bool changed=present && ui->changed_ms[i] && (uint32_t)(now-ui->changed_ms[i])<700;
        roundrect(c,x,y,67,16,3,changed?BLUE:WHITE);
        if(present) snprintf(label,sizeof(label),"%02u: %02X",i,ui->raw[i]);
        else snprintf(label,sizeof(label),"%02u: --",i);
        center(c,x+33,y,label,0,changed?WHITE:present?INK:MUTED);
    }
    footer(c,"短按切换显示 · 长按断开返回","当前显示原始输入字节");
}

void gamepad_ui_render(const gamepad_ui_t *ui,uint32_t now,uint16_t *pixels,int y,int height)
{
    if(y<0 || height<=0 || y+height>320) return;
    canvas_t c={pixels,y,height};
    rect(&c,0,0,240,320,BG);
    if(ui->page==UI_DEVICES) devices(&c,ui,now);
    else if(ui->page==UI_CONNECTING) connecting(&c,ui,now);
    else if(ui->page==UI_LIVE) {
        if(ui->vendor) private_live(&c,ui,now);
        else live(&c,ui);
    }
    else {
        header(&c,"取消配对","确认",RED);
        clipped_name(&c,17,67,ui->active.name,INK,207);
        text(&c,20,120,"移除这台设备的配对记录？",1,INK,205);
        text(&c,20,153,"下次连接需要重新配对。",1,MUTED,205);
        roundrect(&c,20,204,200,42,7,RED);
        center(&c,120,214,"短按确定：移除配对",1,WHITE);
        footer(&c,"长按确定：保留配对并返回",NULL);
    }
}
