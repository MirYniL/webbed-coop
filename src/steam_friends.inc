/* Webbed ships SteamFriends015. Use its flat public API and the game's existing callback pump. */
#include <ctype.h>
typedef unsigned long long FriendID;
typedef struct { void **methods; unsigned char flags; int callback; } JoinCallback;
typedef struct { FriendID friendID; char connect[256]; } JoinRequest;
static void *steamFriends;
static int (*friendCount)(void *,int);
static FriendID (*friendByIndex)(void *,int,int);
static const char *(*presence)(void *,FriendID,const char *);
static void (*requestPresence)(void *,FriendID);
static unsigned char (*setPresence)(void *,const char *,const char *);
static char pendingSteamInvite[192];
static DWORD discoveryStarted;
static int published, launchChecked;
static int valid_invite(const char *code){
    size_t n=strnlen(code,190);
    if(n<9 || n>=150 || strncmp(code,"WB3/",4)!=0)return 0;
    for(size_t i=4;i<n;i++)
        if(!(isalnum((unsigned char)code[i]) || code[i]=='/' || code[i]=='-' || code[i]=='_'))return 0;
    return 1;
}
static void queue_invite(const char *line){
    const char prefix[]="+webbedcoop ";
    if(!strncmp(line,prefix,sizeof(prefix)-1) && valid_invite(line+sizeof(prefix)-1))
        snprintf(pendingSteamInvite,sizeof(pendingSteamInvite),"%s",line+sizeof(prefix)-1);
}
static void __thiscall joined(JoinCallback *cb,JoinRequest *event){
    (void)cb;
    char line[257];memcpy(line,event->connect,256);line[256]=0;queue_invite(line);
}
static void __thiscall joined_result(JoinCallback *cb,JoinRequest *event,unsigned char failed,FriendID call){
    (void)call;if(!failed)joined(cb,event);
}
static int __thiscall joined_size(JoinCallback *cb){(void)cb;return sizeof(JoinRequest);}
/* CCallbackBase vtable as MSVC lays it out: the two Run overloads come in reverse
   declaration order, so Run(void*, bool, SteamAPICall_t) is slot 0 and Run(void*) slot 1. */
static void *joinMethods[]={(void*)joined_result,(void*)joined,(void*)joined_size};
static JoinCallback joinCallback={joinMethods,0,337};
static void steam_setup(void){
    if(!launchChecked){
        launchChecked=1;
        const wchar_t *arg=wcsstr(GetCommandLineW(),L"+webbedcoop ");
        if(arg){char value[160]={0};if(swscanf(arg+12,L"%149hs",value)==1 && valid_invite(value))
            snprintf(pendingSteamInvite,sizeof(pendingSteamInvite),"%s",value);}
    }
    if(steamFriends || (menuTick>1 && menuTick%120!=0))return;
    HMODULE dll=GetModuleHandleW(L"steam_api.dll");if(!dll)return;
    void *(*client)(void)=(void*)GetProcAddress(dll,"SteamClient");
    int (*user)(void)=(void*)GetProcAddress(dll,"SteamAPI_GetHSteamUser");
    int (*pipe)(void)=(void*)GetProcAddress(dll,"SteamAPI_GetHSteamPipe");
    void *(*friends)(void*,int,int,const char*)=(void*)GetProcAddress(dll,"SteamAPI_ISteamClient_GetISteamFriends");
    if(!client || !user || !pipe || !friends || !user() || !pipe())return;
    void *cl=client();if(!cl)return;
    friendCount=(void*)GetProcAddress(dll,"SteamAPI_ISteamFriends_GetFriendCount");
    friendByIndex=(void*)GetProcAddress(dll,"SteamAPI_ISteamFriends_GetFriendByIndex");
    presence=(void*)GetProcAddress(dll,"SteamAPI_ISteamFriends_GetFriendRichPresence");
    requestPresence=(void*)GetProcAddress(dll,"SteamAPI_ISteamFriends_RequestFriendRichPresence");
    setPresence=(void*)GetProcAddress(dll,"SteamAPI_ISteamFriends_SetRichPresence");
    void (*reg)(JoinCallback*,int)=(void*)GetProcAddress(dll,"SteamAPI_RegisterCallback");
    if(!friendCount||!friendByIndex||!presence||!requestPresence||!setPresence||!reg)return;
    steamFriends=friends(cl,user(),pipe(),"SteamFriends015");
    if(steamFriends)reg(&joinCallback,337);
}
static int find_friend_world(char *out,int refresh){
    if(!steamFriends)return 0;
    int n=friendCount(steamFriends,4),matches=0;
    for(int i=0;i<n;i++){
        FriendID id=friendByIndex(steamFriends,i,4);
        if(refresh)requestPresence(steamFriends,id);
        const char *s=presence(steamFriends,id,"webbed_coop_world");
        if(s && valid_invite(s)){snprintf(out,192,"%s",s);matches++;}
    }
    return matches;
}
static void announce_world(void){
    if(!steamFriends)return;
    int advertise=bus->role==HOST && bus->status==LISTENING && valid_invite(bus->invite) && !bus->disconnect;
    if(advertise==published)return;
    if(advertise){
        char text[230];snprintf(text,sizeof(text),"+webbedcoop %s",bus->invite);
        if(setPresence(steamFriends,"webbed_coop_world",bus->invite) && setPresence(steamFriends,"connect",text))published=1;
    }else{setPresence(steamFriends,"webbed_coop_world",NULL);setPresence(steamFriends,"connect",NULL);published=0;}
}
