/* Friend markers drawn with Webbed's own sprites: the journal's map pin and the UI arrow. */
#include "marker_ids.h"

typedef struct { double x, y; int valid; } MapPoint;
static Event drawBook;
static MapPoint ownRoom, peerRoom;
static unsigned lastMapOwn=~0u,lastMapPeer=~0u;
static int mapVisible;
/* BGR, as GameMaker stores colours: green for the host, blue for the guest; the game's own pin is red. */
static const int hostColour=0x5de19b, guestColour=0xe0b452;

static int peer_live(void){
    return player && bus->status==CONNECTED && incoming.active && GetTickCount()-incoming.heartbeat<1800;
}
static int peer_colour(void){return bus->role==HOST?guestColour:hostColour;}
static int soft_colour(int c,double keep){
    int out=0;
    for(int shift=0;shift<24;shift+=8)out|=(255-(int)((255-((c>>shift)&255))*keep))<<shift;
    return out;
}
static MapPoint map_point(void *book,unsigned room){
    Val map=*field(book,V_map_rooms);
    if(!isnum(map))return (MapPoint){0};
    Val name=FN(book,room_get_name,num(room));
    if((name.kind&255)!=1)return (MapPoint){0};
    Val entry=FN(book,ds_map_find_value,map,name);
    drop(&name);
    if(!isnum(entry))return (MapPoint){0};
    GameString key;
    Val x=FN(book,ds_map_find_value,entry,str(&key,"x"));
    Val y=FN(book,ds_map_find_value,entry,str(&key,"y"));
    return (MapPoint){value(x),value(y),isnum(x)&&isnum(y)};
}
static void sprite(void *p,int spr,int frame,double x,double y,double scale,double angle,int colour,double alpha){
    FN(p,draw_sprite_ext,num(spr),num(frame),num(x),num(y),num(scale),num(scale),num(angle),num(colour),num(alpha));
}
/* The game's UI arrow, tinted towards the friend's colour and pointing along (dx,dy). */
static void arrow(void *p,double x,double y,double dx,double dy,double size,int color){
    double angle=dir(dx,dy),scale=size/32;
    FN(p,gpu_set_fog,num(1),num(color),num(0),num(0));
    sprite(p,SPR_sUIArrow,0,x,y,scale,angle,0xffffff,1);
    FN(p,gpu_set_fog,num(0),num(0),num(0),num(1));
    sprite(p,SPR_sUIArrow,0,x,y,scale,angle,0xffffff,.3);
}
/* The journal's own map pin, recoloured: a flat silhouette in the friend's colour with the
   original shading laid over it, plus the pin's shadow. */
static void friend_pin(void *p,double x,double y){
    sprite(p,SPR_sMapMarker,0,x,y,1,0,0xffffff,1);
    FN(p,gpu_set_fog,num(1),num(peer_colour()),num(0),num(0));
    sprite(p,SPR_sMapMarker,1,x,y,1,0,0xffffff,1);
    FN(p,gpu_set_fog,num(0),num(0),num(0),num(1));
    sprite(p,SPR_sMapMarker,1,x,y,1,0,0xffffff,.25);
}
static void on_book_draw(void *self,void *other){
    drawBook(self,other);
    mapVisible=0;
    if(!peer_live()){lastMapOwn=lastMapPeer=~0u;return;}
    if(lastMapOwn!=bus->room || lastMapPeer!=incoming.room){
        ownRoom=map_point(self,bus->room);peerRoom=map_point(self,incoming.room);
        lastMapOwn=bus->room;lastMapPeer=incoming.room;
    }
    if(!read_field(self,V_open) || read_field(self,V_page)!=0 || fabs(read_field(self,V_turn_page))>.05)return;
    mapVisible=1;
    if(!peerRoom.valid)return;
    double w=read_field(self,V_page_width),h=read_field(self,V_page_height);
    double guiWidth=value(FN0(self,display_get_gui_width)),guiHeight=value(FN0(self,display_get_gui_height));
    double left=(guiWidth-w)*.5,top=(guiHeight-h)*(2.0/3.0)+read_field(self,V_y_offset);
    /* Same room: stand the friend's pin beside the game's own instead of on top of it. */
    double offset=(ownRoom.valid && hypot(ownRoom.x-peerRoom.x,ownRoom.y-peerRoom.y)<8)?9:0;
    Val alpha=FN0(self,draw_get_alpha);FN(self,draw_set_alpha,num(1));
    friend_pin(self,left+peerRoom.x+offset,top+peerRoom.y);
    FN(self,draw_set_alpha,alpha);
}

static void edge_marker(void *self){
    if(!peer_live() || mapVisible || exists(self,subId))return;
    double w=value(FN0(self,display_get_gui_width)),h=value(FN0(self,display_get_gui_height));
    double dx=0,dy=0;
    if(incoming.room==bus->room){
        Val camera=FN(self,view_get_camera,num(0));
        double cw=value(FN(self,camera_get_view_width,camera)),ch=value(FN(self,camera_get_view_height,camera));
        if(cw<=0 || ch<=0)return;
        dx=(incoming.x-value(FN(self,camera_get_view_x,camera)))/cw*w-w*.5;
        dy=(incoming.y-value(FN(self,camera_get_view_y,camera)))/ch*h-h*.5;
        if(fabs(dx)<=w*.5 && fabs(dy)<=h*.5)return;
    }else{
        if(lastMapOwn!=bus->room || lastMapPeer!=incoming.room || !ownRoom.valid || !peerRoom.valid)return;
        dx=peerRoom.x-ownRoom.x;dy=peerRoom.y-ownRoom.y;
    }
    double len=hypot(dx,dy),margin=18;
    if(len<.01 || w<=2*margin || h<=2*margin)return;
    double k=1/fmax(fabs(dx)/(w*.5-margin),fabs(dy)/(h*.5-margin));
    double x=w*.5+k*dx,y=h*.5+k*dy;
    double pulse=1+.08*sin(GetTickCount()/170.0);
    Val alpha=FN0(self,draw_get_alpha);FN(self,draw_set_alpha,num(1));
    arrow(self,x,y,dx/len,dy/len,30*pulse,peer_colour());
    FN(self,draw_set_alpha,alpha);
}
