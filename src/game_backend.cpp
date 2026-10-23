#include "game_backend.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace vt::ios {
namespace {
using Obj=void*;using Klass=void*;using Field=void*;using Method=const void*;
struct Vec3 {float x=0,y=0,z=0;};
struct Quat {float x=0,y=0,z=0,w=1;};
struct Bounds {Vec3 center{},extents{};};
struct Rect {float x=0,y=0,w=0,h=0;};
struct Ray {Vec3 origin{},direction{};};
struct RaycastHit {Vec3 point{},normal{};std::uint32_t faceId=0;float distance=0,uvX=0,uvY=0;int collider=0;std::uint8_t pad[16]{};};

bool valid(Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::fabs(p.x)<100000.f&&std::fabs(p.y)<100000.f&&std::fabs(p.z)<100000.f;}
bool validBounds(const Bounds&b){
    return valid(b.center)&&valid(b.extents)&&b.extents.x>=0.f&&b.extents.y>=0.f&&b.extents.z>=0.f&&
        b.extents.x<=100000.f&&b.extents.y<=100000.f&&b.extents.z<=100000.f;
}
float distXZ(Vec3 a,Vec3 b){return std::hypot(a.x-b.x,a.z-b.z);}
float dist3D(Vec3 a,Vec3 b){return std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z));}

bool safeReadMemory(const void*src,void*dst,std::size_t size){
    if(!src||!dst||size==0||size>0x1000)return false;
    auto addr=reinterpret_cast<std::uintptr_t>(src);
    if(addr<0x10000)return false;
#if defined(__APPLE__)
    vm_size_t outSize=0;
    if(vm_read_overwrite(mach_task_self(),static_cast<vm_address_t>(addr),static_cast<vm_size_t>(size),
        reinterpret_cast<vm_address_t>(dst),&outSize)!=KERN_SUCCESS||outSize!=size)return false;
    return true;
#else
    std::memcpy(dst,src,size);
    return true;
#endif
}

template<class T>
bool safeReadAt(const void*base,std::size_t offset,T&out){
    if(!base)return false;
    auto addr=reinterpret_cast<std::uintptr_t>(base);
    if(addr<0x10000)return false;
    return safeReadMemory(reinterpret_cast<const char*>(base)+offset,&out,sizeof(T));
}

template<class T>
bool safeWriteAt(void*base,std::size_t offset,const T&val){
    if(!base)return false;
    T probe{};
    if(!safeReadAt(base,offset,probe))return false;
    std::memcpy(reinterpret_cast<char*>(base)+offset,&val,sizeof(T));
    return true;
}

bool iequals(const char*a,const char*b){
    if(!a||!b)return false;
    while(*a&&*b){
        if(std::tolower(static_cast<unsigned char>(*a))!=std::tolower(static_cast<unsigned char>(*b)))return false;
        ++a;++b;
    }
    return *a==*b;
}

struct API {
    void*(*domain_get)()=nullptr;
    void**(*domain_get_assemblies)(void*,std::size_t*)=nullptr;
    void*(*assembly_get_image)(void*)=nullptr;
    const char*(*image_get_name)(void*)=nullptr;
    Klass(*class_from_name)(void*,const char*,const char*)=nullptr;
    Klass(*class_get_parent)(Klass)=nullptr;
    const char*(*class_get_name)(Klass)=nullptr;
    const char*(*class_get_namespace)(Klass)=nullptr;
    Method(*class_get_method_from_name)(Klass,const char*,int)=nullptr;
    Field(*class_get_field_from_name)(Klass,const char*)=nullptr;
    const void*(*field_get_type)(Field)=nullptr;
    int(*type_get_type)(const void*)=nullptr;
    void(*field_get_value)(Obj,Field,void*)=nullptr;
    void(*field_static_get_value)(Field,void*)=nullptr;
    Obj(*runtime_invoke)(Method,Obj,void**,Obj*)=nullptr;
    Klass(*object_get_class)(Obj)=nullptr;
    void*(*object_unbox)(Obj)=nullptr;
    std::uintptr_t(*gchandle_new)(Obj,bool)=nullptr;
    Obj(*gchandle_get_target)(std::uintptr_t)=nullptr;
    void(*gchandle_free)(std::uintptr_t)=nullptr;
    Obj(*array_new)(Klass,std::size_t)=nullptr;
    std::size_t(*array_length)(Obj)=nullptr;
    std::uint32_t(*array_object_header_size)()=nullptr;
    void*(*thread_current)()=nullptr;
    void*(*thread_attach)(void*)=nullptr;
    void(*thread_detach)(void*)=nullptr;
    // Optional IL2CPP helpers (resolved when present in UnityFramework)
    void(*field_set_value)(Obj,Field,void*)=nullptr;
    std::size_t(*field_get_offset)(Field)=nullptr;
    Field(*class_get_fields)(Klass,void**)=nullptr;
    const char*(*field_get_name)(Field)=nullptr;
    const void*(*class_get_type)(Klass)=nullptr;
    Obj(*type_get_object)(const void*)=nullptr;
    Obj(*string_new)(const char*)=nullptr;
    bool hasNativeLayout=false;

    explicit API(const GameBackend::Resolver& r){
#define LOAD(n) n=reinterpret_cast<decltype(n)>(r("il2cpp_" #n));if(!n)throw std::runtime_error("Missing il2cpp_" #n)
        LOAD(domain_get);LOAD(domain_get_assemblies);LOAD(assembly_get_image);LOAD(image_get_name);
        LOAD(class_from_name);LOAD(class_get_parent);LOAD(class_get_name);LOAD(class_get_namespace);
        LOAD(class_get_method_from_name);LOAD(class_get_field_from_name);LOAD(field_get_type);LOAD(type_get_type);
        LOAD(field_get_value);LOAD(field_static_get_value);LOAD(runtime_invoke);
        LOAD(object_get_class);LOAD(object_unbox);LOAD(gchandle_new);LOAD(gchandle_get_target);LOAD(gchandle_free);
        LOAD(array_new);LOAD(array_length);LOAD(array_object_header_size);
        LOAD(thread_current);LOAD(thread_attach);LOAD(thread_detach);
#undef LOAD
#define OPT(n) n=reinterpret_cast<decltype(n)>(r("il2cpp_" #n))
        OPT(field_set_value);OPT(field_get_offset);OPT(class_get_fields);OPT(field_get_name);
        OPT(class_get_type);OPT(type_get_object);OPT(string_new);
#undef OPT
        hasNativeLayout=(field_get_offset!=nullptr&&class_get_type!=nullptr);
    }
};

struct Root {
    API* a=nullptr;std::uintptr_t h=0;
    Root()=default;Root(API&api,Obj o):a(&api),h(o?api.gchandle_new(o,false):0){if(o&&!h)throw std::runtime_error("GC handle failed");}
    Root(const Root&)=delete;Root&operator=(const Root&)=delete;
    Root(Root&&other)noexcept:a(other.a),h(other.h){other.a=nullptr;other.h=0;}
    Root&operator=(Root&&other)noexcept{if(this!=&other){reset();a=other.a;h=other.h;other.a=nullptr;other.h=0;}return *this;}
    ~Root(){reset();}
    Obj get()const{return h?a->gchandle_get_target(h):nullptr;}
    void reset(){if(h&&a){a->gchandle_free(h);h=0;}}
    void set(API&api,Obj o){auto next=o?api.gchandle_new(o,false):0;if(o&&!next)throw std::runtime_error("GC handle failed");reset();a=&api;h=next;}
};

struct WorldMarker {
    Vec3 anchor{};
    std::string color="green";
    std::string label;
    std::string key;
    int stage=-1;
    bool selected=false;
    bool showRing=true;
    bool showDistance=true;
};

struct RouteNode {
    Vec3 anchor{};
    std::string key;
    bool drop=false;
    int stage=0;
    bool goal=false;
};

struct ProjectedPt {
    float x=0,y=0,depth=0;
    bool ok=false;
};

bool isMiniControlName(const std::string&name){
    static const std::set<std::string> kMini={
        "ActorBattlePlayer","ActorCatchUpPlayer","ActorDuelPlayer","ActorFashionShowPlayer",
        "ActorInfiniteFloorPlayer","ActorKMGPlayer","ActorObbyPlayer","ActorPingPongPlayer",
        "ActorSmashOutPlayer","ActorSpeedUpPlayer","ActorTreasureHuntPlayer","ActorMiniGameControl"
    };
    return kMini.contains(name);
}

int parseDigits(const std::string&s,std::size_t pos,std::size_t len){
    if(pos+len>s.size())return -1;
    int v=0;
    for(std::size_t i=0;i<len;++i){
        if(!std::isdigit(static_cast<unsigned char>(s[pos+i])))return -1;
        v=v*10+(s[pos+i]-'0');
    }
    return v;
}

struct HoleLayout {
    std::string stageName;
    int stage=0;
    int limit=0;
    std::string variant;
};

bool parseHolePath(const std::vector<std::string>&path,HoleLayout&out){
    // MapRoot / OutProp / Hide_Stage / Stage_01_7_Root / Stage_Board_01_a / Stamp / DropBoard_stage01_Stamp_1
    if(path.size()<7)return false;
    const std::size_t n=path.size();
    if(path[n-5]!="Hide_Stage"||path[n-2]!="Stamp")return false;
    const auto&sRoot=path[n-4];
    const auto&sBoard=path[n-3];
    const auto&sStamp=path[n-1];
    if(sRoot.size()!=15||sRoot.compare(0,6,"Stage_")!=0||sRoot.compare(9,6,"_Root")!=0)return false;
    int st=parseDigits(sRoot,6,2);
    int lim=parseDigits(sRoot,9,1);
    if(st<1||st>7||(lim!=1&&lim!=3&&lim!=5&&lim!=7))return false;
    if(sBoard.size()!=16||sBoard.compare(0,12,"Stage_Board_")!=0||sBoard[14]!='_')return false;
    if(parseDigits(sBoard,12,2)!=st)return false;
    char var=sBoard[15];
    if(var<'a'||var>'e')return false;
    if(sStamp.compare(0,15,"DropBoard_stage")!=0||sStamp.find("_Stamp_")==std::string::npos)return false;
    out={sRoot,st,lim,sBoard};
    return true;
}

bool parseHoleBoardPath(const std::vector<std::string>&path,int&stageOut,std::string&variantOut){
    // MapRoot / OutProp / Hide_Stage / Stage_01_7_Root / Stage_Board_01_a / Board / DropBoard_stage01_a
    if(path.size()<7)return false;
    const std::size_t n=path.size();
    if(path[n-5]!="Hide_Stage"||path[n-2]!="Board")return false;
    const auto&sBoard=path[n-3];
    if(sBoard.size()!=16||sBoard.compare(0,12,"Stage_Board_")!=0||sBoard[14]!='_')return false;
    int st=parseDigits(sBoard,12,2);
    char var=sBoard[15];
    if(st<1||st>7||var<'a'||var>'e')return false;
    stageOut=st;variantOut=sBoard;
    return true;
}

struct DoorChoice {
    std::string kind; // "door", "wall", "column"
    std::string group; // "door1", "wall1".."wall4", "entrance-column"
    std::string side; // "L","C","R" or "through","wall"
};

bool parseDoorIdentity(const std::vector<std::string>&path,DoorChoice&out){
    // MapRoot / Environment / HideObject / Door_Root / door1_L
    if(path.size()<5)return false;
    const std::size_t n=path.size();
    if(path[n-3]!="HideObject")return false;
    const auto&folder=path[n-2];
    const auto&leaf=path[n-1];
    if(folder=="Door_Root"){
        auto us=leaf.rfind('_');
        if(us==std::string::npos||us+2!=leaf.size()||leaf.compare(0,4,"door")!=0)return false;
        char s=leaf[us+1];
        if(s!='L'&&s!='C'&&s!='R')return false;
        out={"door",leaf.substr(0,us),std::string(1,s)};
        return true;
    }
    if(folder=="Obstacle_Root"&&(leaf=="plir_through"||leaf=="plir_wall")){
        out={"column","entrance-column",leaf=="plir_through"?"through":"wall"};
        return true;
    }
    if(folder=="Wall_Root"&&leaf.compare(0,21,"TwistRoad_Random_wall")==0){
        auto us=leaf.rfind('_');
        if(us!=std::string::npos&&us+2==leaf.size()&&(leaf[us+1]=='L'||leaf[us+1]=='R')){
            std::string num=leaf.substr(21,us-21);
            if(!num.empty()){
                out={"wall","wall"+num,std::string(1,leaf[us+1])};
                return true;
            }
        }
    }
    return false;
}

bool parseRoadVariant(const std::vector<std::string>&path,std::string&nameOut){
    // MapRoot / Environment / Stage_Board / Stage_02 / Hide_Stage / Road_01_Root / Road_01..27
    if(path.size()<7)return false;
    const std::size_t n=path.size();
    if(path[n-2]!="Road_01_Root")return false;
    const auto&leaf=path[n-1];
    if(leaf.size()==7&&leaf.compare(0,5,"Road_")==0){
        int num=parseDigits(leaf,5,2);
        if(num>=1&&num<=27){nameOut=leaf;return true;}
    }
    return false;
}

int parseRoadLaneRow(const std::vector<std::string>&path){
    for(const auto&part:path){
        if(part.compare(0,10,"Road_line_")==0||part.compare(0,10,"road_line_")==0){
            if(part.size()==11&&part[10]>='1'&&part[10]<='3')return part[10]-'0';
            if(part.size()==12&&part[10]=='0'&&part[11]>='1'&&part[11]<='3')return part[11]-'0';
        }
    }
    return 0;
}

bool parseMazeVariant(const std::vector<std::string>&path,std::string&variantOut){
    // ... / RandomMaze_Root / MazeCreate_S_06_R / maze_stage_06 / maze_stage_06_collider
    for(std::size_t i=0;i+3<path.size();++i){
        if(path[i]=="RandomMaze_Root"){
            const auto&v=path[i+1];
            if(v.size()==17&&v.compare(0,13,"MazeCreate_S_")==0){
                int num=parseDigits(v,13,2);
                char dir=v[16];
                if(num>=1&&num<=22&&(dir=='R'||dir=='T'||dir=='B'||dir=='L')){
                    variantOut=v;return true;
                }
            }
        }
    }
    return false;
}

bool mazeSegmentHitsBox(Vec3 a,Vec3 b,const std::array<float,4>&box){
    float lo=0.f,hi=1.f;
    for(int axis=0;axis<2;++axis){
        float ak=axis==0?a.x:a.z,bk=axis==0?b.x:b.z;
        float delta=bk-ak,mn=box[axis],mx=box[axis+2];
        if(std::fabs(delta)<1e-9f){
            if(ak<mn||ak>mx)return false;
        }else{
            float t0=(mn-ak)/delta,t1=(mx-ak)/delta;
            lo=std::max(lo,std::min(t0,t1));
            hi=std::min(hi,std::max(t0,t1));
            if(lo>hi)return false;
        }
    }
    return true;
}

Vec3 rotateScaleTranslate(Vec3 point,Vec3 pos,Vec3 scale,Quat q){
    float x=point.x*scale.x,y=point.y*scale.y,z=point.z*scale.z;
    float tx=2.f*(q.y*z-q.z*y),ty=2.f*(q.z*x-q.x*z),tz=2.f*(q.x*y-q.y*x);
    return {
        pos.x+x+q.w*tx+q.y*tz-q.z*ty,
        pos.y+y+q.w*ty+q.z*tx-q.x*tz,
        pos.z+z+q.w*tz+q.x*ty-q.y*tx
    };
}
} // namespace

bool runtimeRequested(const Model&m){return m.enabled[Esp]||m.enabled[Line]||m.enabled[SpamJump];}

struct GameBackend::State {
    API a;void* ownThread=nullptr;
    std::map<std::string,Klass> classes;
    std::map<std::tuple<Klass,std::string,int>,Method> methods;
    std::map<std::pair<Klass,std::string>,Field> fields;
    Root actorRoot,controlRoot,miniGameRoot;
    std::string actorUID;
    double nextJump=0,lastJump=-1,lastTick=-1;
    double settleUntil=0,retryAfter=0;
    bool idle=true,needsSettle=false;
    std::uint64_t jumpRequests=0;
    std::vector<EspItem> points;
    std::vector<RouteSegment> routeSegments;
    std::vector<WorldMarker> worldMarkers;
    std::vector<RouteNode> routeNodes;
    int currentRouteStage=0;
    double espScanTime=-1,lastMiniSearchTime=-1;
    std::string status="Waiting for IL2CPP",controllerName,currentMapName,espSummary;
    int currentMapId=0;

    // Map 120015 / 120006 / 120018 / 120041 caches
    std::string cachedRoundKey;
    struct HoleTileRec {Root comp;HoleLayout layout;};
    struct HoleBoardRec {Root col;Root variantGo;int stage=0;std::string variant;};
    std::vector<HoleTileRec> holeTiles;
    std::vector<HoleBoardRec> holeBoards;
    std::vector<Root> holeFinalColliders;
    std::map<std::string,Vec3> holeSavedAnchors;
    bool holeStructureReady=false;

    struct MazeState {
        std::string key;
        std::string activeVariant;
        Root meshCol;
        Bounds meshBounds{};
        float step=0.5f,x0=0,z0=0;
        int width=0,height=0;
        std::vector<float> heights; // NaN = null
        bool gridComplete=false;
        std::vector<Root> boxColliders;
        std::vector<std::vector<std::string>> boxPaths;
        std::string routeSignature;
        std::vector<RouteNode> solvedNodes;
        bool routeReady=false;
        std::string routeReason="Đang quét mê cung";
    } maze;

    explicit State(Resolver r):a(r){}
    ~State(){clearCaches();controlRoot.reset();actorRoot.reset();if(ownThread)a.thread_detach(ownThread);}

    void clearCaches(){
        holeTiles.clear();holeBoards.clear();holeFinalColliders.clear();holeSavedAnchors.clear();
        holeStructureReady=false;cachedRoundKey.clear();
        maze=MazeState{};
        miniGameRoot.reset();
        worldMarkers.clear();routeNodes.clear();
    }

    Klass tryKlass(const char*image,const char*ns,const char*name){
        std::string key=std::string(image?image:"*")+":"+ns+":"+name;
        if(auto it=classes.find(key);it!=classes.end())return it->second;
        auto domain=a.domain_get();if(!domain)return nullptr;
        if(!a.thread_current()&&!ownThread){ownThread=a.thread_attach(domain);}
        std::size_t count=0;auto list=a.domain_get_assemblies(domain,&count);
        if(!list||count>4096)return nullptr;
        for(std::size_t i=0;i<count;++i){
            auto im=a.assembly_get_image(list[i]);const char*n=a.image_get_name(im);
            if(!n)continue;
            if(!image||!*image||std::strcmp(n,image)==0){
                auto c=a.class_from_name(im,ns,name);
                if(c){classes[key]=c;return c;}
            }
        }
        return nullptr;
    }

    Klass klass(const char*image,const char*ns,const char*name){
        auto domain=a.domain_get();if(!domain)throw std::runtime_error("Waiting for IL2CPP domain");
        if(!a.thread_current()&&!ownThread){ownThread=a.thread_attach(domain);if(!ownThread)throw std::runtime_error("IL2CPP attach failed");}
        std::size_t count=0;auto list=a.domain_get_assemblies(domain,&count);
        if(!list||count>4096)throw std::runtime_error("Waiting for assemblies");
        if(auto c=tryKlass(image,ns,name))return c;
        throw std::runtime_error(std::string("Class unavailable: ")+name);
    }

    Klass game(const char*name){return klass("Assembly-CSharp.dll","",name);}
    Klass tryGame(const char*name){return tryKlass("Assembly-CSharp.dll","",name);}
    Klass unity(const char*name){
        if(auto c=tryKlass("UnityEngine.CoreModule.dll","UnityEngine",name))return c;
        if(auto c=tryKlass("UnityEngine.PhysicsModule.dll","UnityEngine",name))return c;
        if(auto c=tryKlass(nullptr,"UnityEngine",name))return c;
        throw std::runtime_error(std::string("Unity class unavailable: ")+name);
    }

    Method tryMethod(Klass c,const char*name,int argc){
        if(!c)return nullptr;
        auto key=std::make_tuple(c,std::string(name),argc);
        if(auto i=methods.find(key);i!=methods.end())return i->second;
        for(auto k=c;k;k=a.class_get_parent(k)){
            auto m=a.class_get_method_from_name(k,name,argc);
            if(m){methods[key]=m;return m;}
        }
        return nullptr;
    }

    Method method(Klass c,const char*name,int argc){
        if(auto m=tryMethod(c,name,argc))return m;
        throw std::runtime_error(std::string("Method unavailable: ")+name);
    }

    Field tryField(Klass c,const char*name){
        if(!c)return nullptr;
        auto key=std::make_pair(c,std::string(name));
        if(auto i=fields.find(key);i!=fields.end())return i->second;
        for(auto k=c;k;k=a.class_get_parent(k)){
            auto f=a.class_get_field_from_name(k,name);
            if(f){fields[key]=f;return f;}
        }
        if(a.class_get_fields&&a.field_get_name){
            for(auto k=c;k;k=a.class_get_parent(k)){
                void*iter=nullptr;
                while(Field f=a.class_get_fields(k,&iter)){
                    const char*fn=a.field_get_name(f);
                    if(fn&&iequals(fn,name)){fields[key]=f;return f;}
                }
            }
        }
        return nullptr;
    }

    Field field(Klass c,const char*name){
        if(auto f=tryField(c,name))return f;
        throw std::runtime_error(std::string("Field unavailable: ")+name);
    }

    Field findFieldAny(Klass c,std::initializer_list<const char*>names){
        for(const char*n:names)if(auto f=tryField(c,n))return f;
        return nullptr;
    }

    bool inheritsFrom(Klass c,const char*targetName){
        for(auto k=c;k;k=a.class_get_parent(k)){
            const char*n=a.class_get_name(k);
            if(n&&std::strcmp(n,targetName)==0)return true;
        }
        return false;
    }

    std::string classLabel(Obj o){
        if(!o)return {};
        Klass k=a.object_get_class(o);
        if(!k)return {};
        const char*n=a.class_get_name(k);
        return n?std::string(n):std::string();
    }

    Obj invoke(Method m,Obj o,void**args=nullptr){
        Obj ex=nullptr;auto r=a.runtime_invoke(m,o,args,&ex);
        if(ex)throw std::runtime_error("Managed call raised an exception");
        return r;
    }

    Obj call(Obj o,const char*n,int argc=0,void**args=nullptr){
        if(!o)throw std::runtime_error("Waiting for object");
        return invoke(method(a.object_get_class(o),n,argc),o,args);
    }

    template<class T>T unbox(Obj o){
        if(!o)throw std::runtime_error("Missing boxed return value");
        auto p=a.object_unbox(o);if(!p)throw std::runtime_error("Cannot unbox result");
        T v{};std::memcpy(&v,p,sizeof(v));return v;
    }

    template<class T>T read(Obj o,Field f,int expected=0){
        if(!o||!f)throw std::runtime_error("Null field owner");
        if(expected&&a.type_get_type(a.field_get_type(f))!=expected)throw std::runtime_error("Field type changed");
        T v{};a.field_get_value(o,f,&v);return v;
    }

    template<class T>bool writeField(Obj o,Field f,T value){
        if(!o||!f)return false;
        if(a.field_set_value){a.field_set_value(o,f,&value);return true;}
        if(a.field_get_offset){
            std::size_t off=a.field_get_offset(f);
            return safeWriteAt(o,off,value);
        }
        return false;
    }

    template<class T>bool readFieldOrOffset(Obj o,std::initializer_list<const char*>names,std::size_t offset,T&out){
        if(!o)return false;
        Klass k=a.object_get_class(o);
        if(Field f=findFieldAny(k,names)){
            a.field_get_value(o,f,&out);
            return true;
        }
        if(a.hasNativeLayout&&offset>0)return safeReadAt(o,offset,out);
        return false;
    }

    Obj ref(Obj o,const char*n){return read<Obj>(o,field(a.object_get_class(o),n));}

    Obj findRefByClass(Obj o,const char*targetClassName,std::initializer_list<const char*>names,std::size_t fallbackOffset){
        if(!o)return nullptr;
        Klass k=a.object_get_class(o);
        if(Field f=findFieldAny(k,names)){
            Obj val=nullptr;a.field_get_value(o,f,&val);
            if(val&&classLabel(val)==targetClassName)return val;
        }
        if(a.class_get_fields){
            for(auto cur=k;cur;cur=a.class_get_parent(cur)){
                void*iter=nullptr;
                while(Field f=a.class_get_fields(cur,&iter)){
                    int t=a.type_get_type(a.field_get_type(f));
                    if(t==0x12||t==0x1c){
                        Obj val=nullptr;a.field_get_value(o,f,&val);
                        if(val&&classLabel(val)==targetClassName)return val;
                    }
                }
            }
        }
        if(a.hasNativeLayout&&fallbackOffset>0){
            Obj val=nullptr;
            if(safeReadAt(o,fallbackOffset,val)&&val&&classLabel(val)==targetClassName)return val;
        }
        return nullptr;
    }

    bool alive(Obj o){
        if(!o)return false;
        void*args[]={o};
        return unbox<bool>(invoke(method(unity("Object"),"op_Implicit",1),nullptr,args));
    }

    std::string objectName(Obj o){
        if(!o||!alive(o))return {};
        Method m=tryMethod(unity("Object"),"get_name",0);
        if(!m)return {};
        Obj str=invoke(m,o);
        if(!str)return {};
        std::int32_t len=0;
        if(!safeReadAt(str,0x10,len)||len<=0||len>256)return {};
        std::string out;out.reserve(std::size_t(len));
        for(int i=0;i<len;++i){
            std::uint16_t ch=0;
            if(!safeReadAt(str,0x14+std::size_t(i)*2,ch))break;
            if(ch<0x80)out.push_back(static_cast<char>(ch));
            else if(ch<0x800){
                out.push_back(static_cast<char>(0xC0|(ch>>6)));
                out.push_back(static_cast<char>(0x80|(ch&0x3F)));
            }else{
                out.push_back(static_cast<char>(0xE0|(ch>>12)));
                out.push_back(static_cast<char>(0x80|((ch>>6)&0x3F)));
                out.push_back(static_cast<char>(0x80|(ch&0x3F)));
            }
        }
        return out;
    }

    Obj componentGO(Obj comp){
        if(!comp||!alive(comp))return nullptr;
        return call(comp,"get_gameObject");
    }

    Obj getTransform(Obj goOrComp){
        if(!goOrComp||!alive(goOrComp))return nullptr;
        return call(goOrComp,"get_transform");
    }

    Obj getParent(Obj tr){
        if(!tr||!alive(tr))return nullptr;
        return call(tr,"get_parent");
    }

    bool activeGO(Obj go){
        if(!go||!alive(go))return false;
        if(auto m=tryMethod(a.object_get_class(go),"get_activeInHierarchy",0))return unbox<bool>(invoke(m,go));
        if(auto m=tryMethod(a.object_get_class(go),"get_activeSelf",0))return unbox<bool>(invoke(m,go));
        return false;
    }

    bool colliderEnabled(Obj col){
        if(!col||!alive(col))return false;
        return unbox<bool>(call(col,"get_enabled"));
    }

    bool colliderTrigger(Obj col){
        if(!col||!alive(col))return false;
        return unbox<bool>(call(col,"get_isTrigger"));
    }

    bool solidCollider(Obj col){
        if(!col||!alive(col))return false;
        Obj go=componentGO(col);
        return activeGO(go)&&colliderEnabled(col)&&!colliderTrigger(col);
    }

    Bounds boundsOfCollider(Obj col){
        if(!col||!alive(col))throw std::runtime_error("Collider unavailable");
        Bounds b=unbox<Bounds>(call(col,"get_bounds"));
        if(!validBounds(b))throw std::runtime_error("Invalid collider bounds");
        return b;
    }

    Vec3 worldPos(Obj tr){
        if(!tr||!alive(tr))throw std::runtime_error("Transform unavailable");
        Vec3 v=unbox<Vec3>(call(tr,"get_position"));
        if(!valid(v))throw std::runtime_error("Invalid transform position");
        return v;
    }

    std::vector<std::string> hierarchyNames(Obj tr){
        std::vector<std::string> names;
        std::set<Obj> seen;
        Obj cur=tr;
        for(int i=0;i<16&&cur&&alive(cur);++i){
            if(seen.contains(cur))break;
            seen.insert(cur);
            Obj go=componentGO(cur);
            names.push_back(objectName(go));
            cur=getParent(cur);
        }
        std::reverse(names.begin(),names.end());
        return names;
    }

    Obj topAncestorGO(Obj compOrGo){
        Obj tr=getTransform(compOrGo);
        if(!tr)return nullptr;
        std::set<Obj> seen;
        for(int i=0;i<32;++i){
            if(seen.contains(tr))break;
            seen.insert(tr);
            Obj p=getParent(tr);
            if(!p||!alive(p))break;
            tr=p;
        }
        return componentGO(tr);
    }

    Obj systemTypeFor(Klass k){
        if(!k||!a.class_get_type||!a.type_get_object)return nullptr;
        const void*t=a.class_get_type(k);
        return t?a.type_get_object(t):nullptr;
    }

    std::vector<Obj> readObjArray(Obj arr,std::size_t maxItems=4096){
        std::vector<Obj> out;
        if(!arr)return out;
        std::size_t n=a.array_length(arr);
        if(n==0||n>maxItems)return out;
        auto head=a.array_object_header_size();
        if(head<16||head>128)return out;
        out.reserve(n);
        for(std::size_t i=0;i<n;++i){
            Obj item=nullptr;
            std::memcpy(&item,reinterpret_cast<const char*>(arr)+head+i*sizeof(Obj),sizeof(Obj));
            if(item)out.push_back(item);
        }
        return out;
    }

    std::vector<Obj> readManagedList(Obj listObj,std::size_t maxItems=2048){
        std::vector<Obj> out;
        if(!listObj)return out;
        Obj itemsArr=nullptr;
        std::int32_t size=0;
        Klass lk=a.object_get_class(listObj);
        if(Field fItems=tryField(lk,"_items"))a.field_get_value(listObj,fItems,&itemsArr);
        else if(a.hasNativeLayout)safeReadAt(listObj,0x10,itemsArr);
        if(Field fSize=tryField(lk,"_size"))a.field_get_value(listObj,fSize,&size);
        else if(a.hasNativeLayout)safeReadAt(listObj,0x18,size);
        if(!itemsArr||size<=0||std::size_t(size)>maxItems)return out;
        std::size_t cap=a.array_length(itemsArr);
        if(cap<std::size_t(size))return out;
        auto head=a.array_object_header_size();
        out.reserve(std::size_t(size));
        for(std::int32_t i=0;i<size;++i){
            Obj item=nullptr;
            std::memcpy(&item,reinterpret_cast<const char*>(itemsArr)+head+std::size_t(i)*sizeof(Obj),sizeof(Obj));
            if(item)out.push_back(item);
        }
        return out;
    }

    std::vector<Obj> getComponentsInChildren(Obj rootGo,Klass targetKlass){
        std::vector<Obj> out;
        if(!rootGo||!alive(rootGo)||!targetKlass)return out;
        Obj sysType=systemTypeFor(targetKlass);
        if(!sysType)return out;
        // 1. Try GameObject.GetComponentsInternal(Type, bool, bool, bool, bool, object)
        if(Method m=tryMethod(unity("GameObject"),"GetComponentsInternal",6)){
            bool useSearchType=true,recursive=true,includeInactive=true,reverse=false;
            Obj resultList=nullptr;
            void*args[]={sysType,&useSearchType,&recursive,&includeInactive,&reverse,resultList};
            try{
                Obj arr=invoke(m,rootGo,args);
                if(arr)return readObjArray(arr,4096);
            }catch(...){}
        }
        // 2. Try Component.GetComponentsInChildren(Type, bool) on Transform
        Obj tr=getTransform(rootGo);
        if(tr&&alive(tr)){
            if(Method m=tryMethod(unity("Component"),"GetComponentsInChildren",2)){
                bool includeInactive=true;
                void*args[]={sysType,&includeInactive};
                try{
                    Obj arr=invoke(m,tr,args);
                    if(arr)return readObjArray(arr,4096);
                }catch(...){}
            }
        }
        return out;
    }

    bool raycastSingleCollider(Obj col,Vec3 origin,Vec3 dir,float maxDist,RaycastHit&hitOut){
        if(!col||!alive(col))return false;
        Method m=tryMethod(unity("Collider"),"Raycast",3);
        if(!m)return false;
        Ray ray{origin,dir};
        RaycastHit hit{};
        void*args[]={&ray,&hit,&maxDist};
        try{
            bool hasHit=unbox<bool>(invoke(m,col,args));
            if(hasHit&&valid(hit.point)){hitOut=hit;return true;}
        }catch(...){}
        return false;
    }

    bool stableBoardBlocks(Vec3 anchor,Obj boardCol,const Bounds&b){
        if(std::fabs(anchor.x-b.center.x)>b.extents.x||std::fabs(anchor.z-b.center.z)>b.extents.z||
           std::fabs(anchor.y-b.center.y)>b.extents.y+1.f)return false;
        float topY=b.center.y+b.extents.y+0.25f,dist=b.extents.y*2.f+0.5f;
        RaycastHit hit{};
        return raycastSingleCollider(boardCol,{anchor.x,topY,anchor.z},{0.f,-1.f,0.f},dist,hit);
    }

    Obj actorSystem(){Obj o=nullptr;a.field_static_get_value(field(game("ActorSystem"),"Self"),&o);return o;}
    Obj actor(){auto o=actorSystem();return o?call(o,"get_MyActorCharacter"):nullptr;}

    Obj currentControl(Obj actorObj){
        auto control=ref(actorObj,"actorControl");if(!control)return nullptr;
        if(ref(control,"actorBase")!=actorObj)throw std::runtime_error("Local controller link changed");
        auto klass=a.object_get_class(control);bool supported=false;
        for(auto c=klass;c;c=a.class_get_parent(c)){
            const char*n=a.class_get_name(c);const char*ns=a.class_get_namespace(c);
            if(n&&ns&&!*ns&&std::strcmp(n,"ActorControl")==0){supported=true;break;}
        }
        if(!supported)throw std::runtime_error("Local controller is not an ActorControl");
        if(read<bool>(control,field(klass,"_terminate"),2))return nullptr;
        // ActorControl is a plain System.Object (IActorControl), NOT a UnityEngine.Object.
        // Never call UnityEngine.Object.op_Implicit(control); validate via actorBase + _terminate.
        controllerName=a.class_get_name(klass);return control;
    }

    void resetFocus(){controlRoot.reset();actorRoot.reset();actorUID.clear();nextJump=0;lastJump=-1;}

    // Ported AutoJump (HOLD_JUMP R5) from Windows agent.js:
    // Checks goal/death/stun gates so AutoJump stops at finish line (IsGoalIn / KMG states 51..54)
    // and temporarily lifts _canJump around OnJump() then restores it immediately.
    bool checkJumpBlocked(Obj control,Klass klass,std::string&reasonOut){
        const bool isMini=isMiniControlName(controllerName)||inheritsFrom(klass,"ActorMiniGameControl");
        if(isMini){
            std::uint8_t goal=0;
            if(!readFieldOrOffset<std::uint8_t>(control,{"<IsGoalIn>k__BackingField","_isGoalIn","isGoalIn"},0x2B2,goal)){
                if(Method mGoal=tryMethod(klass,"get_IsGoalIn",0)){
                    try{goal=unbox<bool>(invoke(mGoal,control))?1:0;}catch(...){}
                }
            }
            if(goal==1){reasonOut="Spam nhảy: đã về đích • dừng nhảy";return true;}

            std::uint8_t death=0;
            if(!readFieldOrOffset<std::uint8_t>(control,{"<IsDeathInGame>k__BackingField","_isDeathInGame","isDeathInGame"},0x2B0,death)){
                if(Method mDeath=tryMethod(klass,"get_IsDeathInGame",0)){
                    try{death=unbox<bool>(invoke(mDeath,control))?1:0;}catch(...){}
                }
            }
            if(controllerName=="ActorKMGPlayer"){
                std::uint8_t die=0,watch=0,canMove=1,canJump=1;
                std::int16_t kmgState=0;
                std::int32_t maxHp=0,curHp=1;
                readFieldOrOffset<std::uint8_t>(control,{"isDie","_isDie"},0x402,die);
                readFieldOrOffset<std::uint8_t>(control,{"isWatchMode","_isWatchMode","<IsWatchMode>k__BackingField"},0x380,watch);
                readFieldOrOffset<std::int16_t>(control,{"_state","state","curState"},0x400,kmgState);
                readFieldOrOffset<std::int32_t>(control,{"_maxHp","maxHp","<MaxHp>k__BackingField"},0x354,maxHp);
                readFieldOrOffset<std::int32_t>(control,{"_currentHp","currentHp","<CurrentHp>k__BackingField"},0x358,curHp);
                readFieldOrOffset<std::uint8_t>(control,{"<IsCanMove>k__BackingField","_canMove","canMove"},0x2A8,canMove);
                readFieldOrOffset<std::uint8_t>(control,{"<IsCanJump>k__BackingField","_canJump","canJump"},0x2A9,canJump);
                if(kmgState>=51&&kmgState<=54){reasonOut="Spam nhảy: đã về đích • dừng nhảy";return true;}
                if(watch==1){reasonOut="Spam nhảy: đang chế độ xem";return true;}
                if(die==1||(kmgState>=8&&kmgState<=13)||kmgState==50||(maxHp>0&&curHp<=0)){
                    reasonOut="Spam nhảy: chờ nhân vật hồi sinh";return true;
                }
                const bool healthOk=(maxHp<=0||curHp>0);
                const bool locomotion=(kmgState>=0&&kmgState<=3)&&(canMove==1||canJump==1);
                const bool deathHistoryOnly=(death==1&&!die&&!watch&&healthOk&&locomotion);
                if(death==1&&!deathHistoryOnly){reasonOut="Spam nhảy: chờ nhân vật hồi sinh";return true;}
            }else if(death==1){
                reasonOut="Spam nhảy: chờ nhân vật hồi sinh";return true;
            }
        }else if(controllerName=="ActorDefaultControlPlayer"&&a.hasNativeLayout){
            std::int32_t stun=0;
            std::uint8_t r0=0,r1=0,r2=0;
            safeReadAt(control,0x2E4,stun);
            safeReadAt(control,0x304,r0);
            safeReadAt(control,0x318,r1);
            safeReadAt(control,0x319,r2);
            if(stun!=0||r0==1||r1==1||r2==1){
                reasonOut="Spam nhảy: nhân vật đang choáng/hồi sinh";return true;
            }
        }
        return false;
    }

    void jumpTick(Obj actorObj,double time,double intervalMs,std::string&jumpNoteOut,bool jumpPressed){
        if(!std::isfinite(intervalMs)||intervalMs<0||intervalMs>1000)throw std::runtime_error("Invalid spam interval");
        auto control=currentControl(actorObj);
        if(!control){controlRoot.reset();nextJump=0;lastJump=-1;return;}
        if(control!=controlRoot.get()){controlRoot.set(a,control);nextJump=0;lastJump=-1;}
        auto klass=a.object_get_class(control);
        std::string blockedReason;
        if(checkJumpBlocked(control,klass,blockedReason)){
            jumpNoteOut=blockedReason;
            return;
        }
        if(!jumpPressed){
            nextJump=0;
            jumpNoteOut="Spam nhảy: BẬT • Đè nút Nhảy để spam";
            return;
        }
        if(time<nextJump||time==lastJump){
            jumpNoteOut="Spam nhảy: ĐANG NHẢY • Giữ nút để tiếp tục";
            return;
        }

        // Scoped _canJump / IsCanJump permission lift around OnJump() (ported from Windows AutoJump R5)
        const bool isMini=isMiniControlName(controllerName)||inheritsFrom(klass,"ActorMiniGameControl");
        const std::size_t fallbackOffset=isMini?0x2A9:(controllerName=="ActorDefaultControlPlayer"?0x4E1:0);
        Field canJumpField=findFieldAny(klass,{"_canJump","<IsCanJump>k__BackingField","canJump","_isCanJump","isCanJump"});
        std::uint8_t origCanJump=1;
        bool liftedField=false,liftedRaw=false;

        if(canJumpField){
            a.field_get_value(control,canJumpField,&origCanJump);
            if(origCanJump==0){
                std::uint8_t one=1;
                liftedField=writeField(control,canJumpField,one);
            }
        }
        if(!liftedField&&a.hasNativeLayout&&fallbackOffset>0){
            if(safeReadAt(control,fallbackOffset,origCanJump)&&origCanJump==0){
                std::uint8_t one=1;
                liftedRaw=safeWriteAt(control,fallbackOffset,one);
            }
        }

        struct RestoreGuard {
            State* s;Obj ctrl;Field f;std::size_t off;std::uint8_t orig;bool byField,byRaw;
            ~RestoreGuard(){
                if(byField)s->writeField(ctrl,f,orig);
                else if(byRaw)safeWriteAt(ctrl,off,orig);
            }
        } restoreGuard{this,control,canJumpField,fallbackOffset,origCanJump,liftedField,liftedRaw};

        call(control,"OnJump");
        ++jumpRequests;lastJump=time;nextJump=time+intervalMs/1000.;
        jumpNoteOut="Spam nhảy: ĐANG NHẢY • Giữ nút để tiếp tục";
    }

    Vec3 position(Obj actorObj){
        auto tr=ref(actorObj,"baseTransfrom");
        if(!alive(tr))throw std::runtime_error("Transform unavailable");
        return worldPos(tr);
    }

    bool isCandidateMiniGame(Obj cand){
        if(!cand||!alive(cand))return false;
        std::string label=classLabel(cand);
        if(label.empty())return false;
        if(label=="MiniGameKMGTwistRoad"||label=="MiniGameKMGFogMaze"||
           label=="MiniGameKMGRealBoard"||label=="MiniGameKMGDuel"||
           label=="MiniGameKMGGreenRedLight"||label=="MiniGameKMGCrossyRoad"||
           label=="MiniGameKMGRunAway"||label=="MiniGameKMGMingle"||
           label=="MiniGameKMGStealCrown"){
            return true;
        }
        if(label.compare(0,8,"MiniGame")==0){
            std::int32_t id=0;
            if(safeReadAt(cand,0x110,id)&&id>=100000&&id<=999999)return true;
        }
        return false;
    }

    Obj findMiniGame(double time,Obj actorObj){
        Obj cached=miniGameRoot.get();
        if(cached&&alive(cached)&&isCandidateMiniGame(cached))return cached;
        miniGameRoot.reset();
        if(lastMiniSearchTime>=0&&time-lastMiniSearchTime<0.25)return nullptr;
        lastMiniSearchTime=time;

        // 1. Scan player control pointers (ActorKMGPlayer)
        if(a.hasNativeLayout&&actorObj&&alive(actorObj)){
            Obj ctrl=currentControl(actorObj);
            if(ctrl){
                for(std::size_t off=0x18;off<=0x480;off+=8){
                    Obj cand=nullptr;
                    if(safeReadAt(ctrl,off,cand)&&cand&&cand!=ctrl&&cand!=actorObj){
                        if(isCandidateMiniGame(cand)){
                            miniGameRoot.set(a,cand);
                            return cand;
                        }
                    }
                }
            }
        }

        // 2. Check static fields across candidate minigame classes
        static const char* minigameClassNames[]={
            "MiniGameKMG","MiniGame","MiniGameKMGTwistRoad","MiniGameKMGFogMaze",
            "MiniGameKMGRealBoard","MiniGameKMGDuel","MiniGameKMGGreenRedLight",
            "MiniGameKMGCrossyRoad","MiniGameKMGMingle","MiniGameKMGRunAway",
            "MiniGameUnit","MiniGameKMGUnit","MiniGameManager","MiniGameSystem",
            "PlayGameManager","GameManager","StageManager"
        };
        for(const char* clsName : minigameClassNames){
            Klass k=tryGame(clsName);
            if(!k)continue;
            // 2A. Named static fields
            for(const char* fName : {"Instance","Self","_instance","current","currentMiniGame","_currentMiniGame","game","miniGame","<Instance>k__BackingField","<Self>k__BackingField"}){
                if(Field f=tryField(k,fName)){
                    Obj cand=nullptr;
                    try{a.field_static_get_value(f,&cand);}catch(...){cand=nullptr;}
                    if(isCandidateMiniGame(cand)){
                        miniGameRoot.set(a,cand);
                        return cand;
                    }
                }
            }
            // 2B. Direct static_fields inspection (+0xB8)
            void* sf=nullptr;
            if(!a.hasNativeLayout) continue;
            if(safeReadAt(k,0xB8,sf)&&sf){
                for(std::size_t off=0;off<=0x40;off+=8){
                    Obj cand=nullptr;
                    if(safeReadAt(sf,off,cand)&&cand){
                        if(isCandidateMiniGame(cand)){
                            miniGameRoot.set(a,cand);
                            return cand;
                        }
                    }
                }
            }
        }

        // 3. Fallback: System.Type.GetType + UnityEngine.Object.FindObjectOfType
        Klass typeKlass=klass("mscorlib.dll","System","Type");
        Method getTypeMethod=tryMethod(typeKlass,"GetType",1);
        Method findObjMethod=tryMethod(unity("Object"),"FindObjectOfType",1);
        if(getTypeMethod&&findObjMethod&&a.string_new){
            for(const char* tname : {
                "MiniGameKMGTwistRoad, Assembly-CSharp",
                "MiniGameKMGFogMaze, Assembly-CSharp",
                "MiniGameKMGRealBoard, Assembly-CSharp",
                "MiniGameKMGDuel, Assembly-CSharp",
                "MiniGameKMGGreenRedLight, Assembly-CSharp",
                "MiniGameKMGCrossyRoad, Assembly-CSharp",
                "MiniGameKMG, Assembly-CSharp"
            }){
                Obj str=a.string_new(tname);
                if(!str)continue;
                void* args1[]={str};
                try{
                    Obj sysType=invoke(getTypeMethod,nullptr,args1);
                    if(sysType){
                        void* args2[]={sysType};
                        Obj found=invoke(findObjMethod,nullptr,args2);
                        if(isCandidateMiniGame(found)){
                            miniGameRoot.set(a,found);
                            return found;
                        }
                    }
                }catch(...){}
            }
        }

        return nullptr;
    }

    int getMiniGameId(Obj gameObj){
        if(!gameObj)return 0;
        std::int32_t id=0;
        if(readFieldOrOffset<std::int32_t>(gameObj,{"_miniGameId","miniGameId","_gameId","gameId","_id","id","<MiniGameId>k__BackingField"},0x110,id)){
            if(id>=100000&&id<=999999)return id;
        }
        if(a.class_get_fields){
            for(Klass k=a.object_get_class(gameObj);k;k=a.class_get_parent(k)){
                void*iter=nullptr;
                while(Field f=a.class_get_fields(k,&iter)){
                    int t=a.type_get_type(a.field_get_type(f));
                    if(t==0x08||t==0x09){ // I4 or U4
                        std::int32_t v=0;a.field_get_value(gameObj,f,&v);
                        if(v>=120000&&v<=129999)return v;
                    }
                }
            }
        }
        return id;
    }

    int getBattleState(Obj gameObj){
        std::uint8_t st=5;
        if(readFieldOrOffset<std::uint8_t>(gameObj,{"_battleState","battleState","_curState","curState","<BattleState>k__BackingField"},0x6C,st)){
            if(st<=15)return st;
        }
        return 5;
    }

    Obj getHideObjectManager(Obj gameObj){
        if(Obj m=findRefByClass(gameObj,"HideObjectManager",{"_hideObjectManager","hideObjectManager","m_HideObjectManager"},0x140)){
            if(alive(m))return m;
        }
        if(Klass homKlass=tryGame("HideObjectManager")){
            auto list=getComponentsInChildren(topAncestorGO(gameObj),homKlass);
            if(!list.empty()&&alive(list[0]))return list[0];
        }
        return nullptr;
    }

    bool tileSurfaceAnchor(Obj stampObj,Vec3&outAnchor){
        // Check renderer (+0x30) then collider (+0x40)
        Obj ren=findRefByClass(stampObj,"MeshRenderer",{"_renderer","renderer","meshRenderer"},0x30);
        if(!ren)ren=findRefByClass(stampObj,"Renderer",{"_renderer","renderer"},0x30);
        if(ren&&alive(ren)){
            try{
                Bounds b=unbox<Bounds>(call(ren,"get_bounds"));
                if(validBounds(b)&&b.extents.x>1e-5f&&b.extents.z>1e-5f){
                    outAnchor={b.center.x,b.center.y+b.extents.y,b.center.z};
                    return true;
                }
            }catch(...){}
        }
        Obj col=nullptr;
        readFieldOrOffset<Obj>(stampObj,{"_collider","collider","boxCollider"},0x40,col);
        if(col&&alive(col)){
            try{
                Bounds b=boundsOfCollider(col);
                if(b.extents.x>1e-5f&&b.extents.z>1e-5f){
                    outAnchor={b.center.x,b.center.y+b.extents.y,b.center.z};
                    return true;
                }
            }catch(...){}
        }
        return false;
    }

    bool inactiveHoleBounds(Obj col,Obj tileObj,Bounds&outBounds){
        if(!col||!alive(col))return false;
        std::string type=classLabel(col);
        Bounds localB{};
        if(type=="BoxCollider"){
            localB.center=unbox<Vec3>(call(col,"get_center"));
            Vec3 sz=unbox<Vec3>(call(col,"get_size"));
            localB.extents={sz.x*0.5f,sz.y*0.5f,sz.z*0.5f};
        }else if(type=="MeshCollider"){
            Obj mesh=call(col,"get_sharedMesh");
            if(!mesh||!alive(mesh))return false;
            localB=unbox<Bounds>(call(mesh,"get_bounds"));
        }else return false;

        Obj tr=getTransform(componentGO(col));
        if(!tr||!alive(tr))return false;

        auto transformCorners=[&](auto&&xformFn,Bounds&res)->bool{
            Vec3 lo{1e30f,1e30f,1e30f},hi{-1e30f,-1e30f,-1e30f};
            for(int dx:{-1,1})for(int dy:{-1,1})for(int dz:{-1,1}){
                Vec3 corner{
                    localB.center.x+float(dx)*localB.extents.x,
                    localB.center.y+float(dy)*localB.extents.y,
                    localB.center.z+float(dz)*localB.extents.z
                };
                Vec3 wp=xformFn(corner);
                if(!valid(wp))return false;
                lo.x=std::min(lo.x,wp.x);lo.y=std::min(lo.y,wp.y);lo.z=std::min(lo.z,wp.z);
                hi.x=std::max(hi.x,wp.x);hi.y=std::max(hi.y,wp.y);hi.z=std::max(hi.z,wp.z);
            }
            res.center={(lo.x+hi.x)*0.5f,(lo.y+hi.y)*0.5f,(lo.z+hi.z)*0.5f};
            res.extents={(hi.x-lo.x)*0.5f,(hi.y-lo.y)*0.5f,(hi.z-lo.z)*0.5f};
            return validBounds(res);
        };

        Bounds current{};
        if(transformCorners([&](Vec3 pt){
            void*args[]={&pt};
            return unbox<Vec3>(call(tr,"TransformPoint",1,args));
        },current)&&current.extents.x>0.f&&current.extents.z>0.f){
            outBounds=current;return true;
        }

        // Recover shrunk cover geometry from saved originPos (+0xAC) and originScale (+0xB8)
        if(!tileObj||!a.hasNativeLayout)return false;
        std::int32_t state=0;std::uint8_t tween=0;
        if(!readFieldOrOffset<std::int32_t>(tileObj,{"_state","state"},0x58,state)||state!=2)return false;
        if(!safeReadAt(tileObj,0x90,tween)||!tween)return false;
        Vec3 origPos{},origScale{};
        if(!safeReadAt(tileObj,0xAC,origPos)||!safeReadAt(tileObj,0xB8,origScale))return false;
        if(!valid(origPos)||!valid(origScale)||origScale.x<=0.f||origScale.y<=0.f||origScale.z<=0.f)return false;
        Quat q=unbox<Quat>(call(tr,"get_localRotation"));
        Obj parent=getParent(tr);
        if(transformCorners([&](Vec3 pt){
            Vec3 lp=rotateScaleTranslate(pt,origPos,origScale,q);
            if(!parent||!alive(parent))return lp;
            void*args[]={&lp};
            return unbox<Vec3>(call(parent,"TransformPoint",1,args));
        },outBounds)&&outBounds.extents.x>0.f&&outBounds.extents.z>0.f){
            return true;
        }
        return false;
    }

    // 1. Map 120018 (Kính Thật/Giả) & MiniGameKMGRealBoard (Ván Thật/Giả)
    void collectRealBoardOrGlass(Obj gameObj,bool isGlass){
        Obj manager=getHideObjectManager(gameObj);
        if(!manager){espSummary="Kính/Ván: chờ HideObjectManager";return;}
        Obj boardRoot=isGlass?manager:nullptr;
        if(!isGlass)readFieldOrOffset<Obj>(manager,{"_boardRoot","boardRoot"},0xA8,boardRoot);
        if(!boardRoot||!alive(boardRoot))boardRoot=manager;
        Obj rootGo=componentGO(boardRoot);
        Klass stampKlass=tryGame("HideStampObjectData");
        if(!stampKlass||!rootGo){espSummary="Kính/Ván: chờ dữ liệu ô";return;}

        auto stamps=getComponentsInChildren(rootGo,stampKlass);
        if(stamps.empty()){
            Obj stampList=nullptr;
            readFieldOrOffset<Obj>(manager,{"hideStampObjectDataList","_hideStampObjectDataList","stampObjectList"},0x40,stampList);
            stamps=readManagedList(stampList,2048);
        }

        std::set<Obj> seenColliders;
        int greenCount=0,redCount=0;
        for(Obj obj:stamps){
            if(!obj||!alive(obj))continue;
            std::int32_t state=-1;
            if(!readFieldOrOffset<std::int32_t>(obj,{"_state","state","curState"},0x58,state))continue;
            if(state<0||state>5)continue;

            // Check stableGlassSurface first for glass map 120018 when state == 5
            if(isGlass&&state==5){
                Obj stableGo=nullptr;
                readFieldOrOffset<Obj>(obj,{"stableObject","_stableObject"},0x48,stableGo);
                if(stableGo&&alive(stableGo)&&activeGO(stableGo)){
                    auto cols=getComponentsInChildren(stableGo,unity("Collider"));
                    Obj bestCol=nullptr;float bestArea=0.f;Vec3 bestAnchor{};
                    for(Obj c:cols){
                        if(!solidCollider(c))continue;
                        try{
                            Bounds b=boundsOfCollider(c);
                            if(b.extents.x<=1e-5f||b.extents.z<=1e-5f||b.extents.y>std::min(b.extents.x,b.extents.z))continue;
                            float area=b.extents.x*b.extents.z;
                            if(area>bestArea){bestArea=area;bestCol=c;bestAnchor={b.center.x,b.center.y+b.extents.y,b.center.z};}
                        }catch(...){}
                    }
                    if(bestCol&&!seenColliders.contains(bestCol)){
                        seenColliders.insert(bestCol);
                        worldMarkers.push_back({bestAnchor,"green","",std::to_string(reinterpret_cast<std::uintptr_t>(bestCol)),-1,false,true,false});
                        ++greenCount;
                        continue;
                    }
                }
            }

            Obj go=componentGO(obj);
            if(!activeGO(go))continue;
            Obj col=nullptr;
            readFieldOrOffset<Obj>(obj,{"_collider","collider","boxCollider"},0x40,col);
            if(!col||!alive(col)||!activeGO(componentGO(col)))continue;
            if(seenColliders.contains(col))continue;

            bool solid=(state==5)?solidCollider(col):false;
            std::string color=(state==5&&solid)?"green":"red";
            Vec3 anchor{};
            if(!tileSurfaceAnchor(obj,anchor))continue;
            seenColliders.insert(col);
            worldMarkers.push_back({anchor,color,"",std::to_string(reinterpret_cast<std::uintptr_t>(col)),-1,false,true,false});
            if(color=="green")++greenCount;else ++redCount;
        }
        std::sort(worldMarkers.begin(),worldMarkers.end(),[](const WorldMarker&a,const WorldMarker&b){return a.key<b.key;});
        espSummary=std::string(isGlass?"Kính 120018":"Ván Thật/Giả")+": "+std::to_string(greenCount)+" xanh, "+std::to_string(redCount)+" đỏ";
    }

    // 2. Map 120026 (Obby Race *3 Vàng / VCNV02)
    void collectRoad120026(Obj gameObj,int battleState){
        Obj manager=getHideObjectManager(gameObj);
        if(!manager){espSummary="120026: chờ HideObjectManager";return;}
        Obj hideList=nullptr;
        readFieldOrOffset<Obj>(manager,{"hideGameObjectList","_hideGameObjectList"},0x38,hideList);
        auto objects=readManagedList(hideList,2048);
        if(objects.empty()){espSummary="120026: chờ danh sách đường";return;}

        struct RoadVar {Obj go;std::string name;std::vector<std::string> path;bool active;};
        std::vector<RoadVar> vars;
        for(Obj go:objects){
            if(!go||!alive(go))continue;
            auto path=hierarchyNames(getTransform(go));
            std::string name;
            if(parseRoadVariant(path,name))vars.push_back({go,name,path,activeGO(go)});
        }
        if(battleState!=5||vars.size()!=27){
            espSummary="120026: chờ bắt đầu lượt chọn đường";return;
        }
        const RoadVar*selected=nullptr;
        for(const auto&v:vars){
            if(v.active){if(selected){selected=nullptr;break;}selected=&v;}
        }
        if(!selected){espSummary="120026: chờ kích hoạt 1 phương án đường";return;}

        auto cols=getComponentsInChildren(selected->go,unity("Collider"));
        std::map<int,std::vector<std::pair<Obj,Bounds>>> lanes;
        for(Obj c:cols){
            if(!solidCollider(c))continue;
            std::string t=classLabel(c);
            if(t!="BoxCollider"&&t!="MeshCollider")continue;
            auto path=hierarchyNames(getTransform(componentGO(c)));
            int row=parseRoadLaneRow(path);
            if(row<1||row>3)continue;
            try{
                Bounds b=boundsOfCollider(c);
                if(b.extents.x<0.1f||b.extents.z<0.1f||b.extents.y>std::min(b.extents.x,b.extents.z))continue;
                lanes[row].push_back({c,b});
            }catch(...){}
        }

        int verifiedRows=0;
        for(auto&[row,candidates]:lanes){
            std::sort(candidates.begin(),candidates.end(),[](const auto&a,const auto&b){
                return a.second.extents.x*a.second.extents.z>b.second.extents.x*b.second.extents.z;
            });
            for(std::size_t i=0;i<std::min<std::size_t>(candidates.size(),4);++i){
                Obj c=candidates[i].first;
                const Bounds&b=candidates[i].second;
                std::vector<Vec3> samples;
                bool ok=true;
                for(float frac:{-0.6f,0.f,0.6f}){
                    Vec3 req{b.center.x,b.center.y+b.extents.y,b.center.z+frac*b.extents.z};
                    RaycastHit hit{};
                    if(raycastSingleCollider(c,{req.x,b.center.y+b.extents.y+0.25f,req.z},{0.f,-1.f,0.f},b.extents.y*2.f+0.5f,hit)){
                        if(hit.normal.y<0.7f||std::fabs(hit.point.y-b.center.y)>b.extents.y+0.05f){ok=false;break;}
                        samples.push_back({hit.point.x,hit.point.y+0.04f,hit.point.z});
                    }else{
                        samples.push_back({req.x,req.y+0.04f,req.z});
                    }
                }
                if(ok&&samples.size()==3){
                    ++verifiedRows;
                    for(const Vec3&pt:samples){
                        worldMarkers.push_back({pt,"yellow","H"+std::to_string(row),"road:"+std::to_string(row),row,true,true,true});
                    }
                    break;
                }
            }
        }
        espSummary="120026 ("+selected->name+"): "+std::to_string(verifiedRows)+"/3 hàng sàn";
    }

    // 3. Map 120006 (Map Cổng YES / NO - Map 3 Cửa + Line)
    void collectDoors120006(Obj gameObj,int battleState,Vec3 playerPos){
        Obj manager=getHideObjectManager(gameObj);
        if(!manager){espSummary="120006: chờ HideObjectManager";return;}
        Obj hideList=nullptr;
        readFieldOrOffset<Obj>(manager,{"hideGameObjectList","_hideGameObjectList"},0x38,hideList);
        auto objects=readManagedList(hideList,2048);
        if(objects.empty()){espSummary="120006: chờ danh sách cửa";return;}

        struct DoorRow {
            DoorChoice choice;
            Vec3 anchor{};
            bool active=false,parentActive=false,known=false,solid=false,suppress=false;
            int colliderCount=0;
            std::string color="yellow";
        };
        std::map<std::string,std::vector<DoorRow>> doorGroups;
        std::map<std::string,std::vector<DoorRow>> wallColGroups;

        for(Obj go:objects){
            if(!go||!alive(go))continue;
            Obj tr=getTransform(go);
            if(!tr)continue;
            auto path=hierarchyNames(tr);
            DoorChoice ch{};
            if(!parseDoorIdentity(path,ch))continue;
            DoorRow row{};
            row.choice=ch;
            try{row.anchor=worldPos(tr);}catch(...){continue;}
            Obj parentTr=getParent(tr);
            row.parentActive=parentTr&&alive(parentTr)&&activeGO(componentGO(parentTr));
            row.active=activeGO(go);
            auto cols=getComponentsInChildren(go,unity("Collider"));
            row.colliderCount=int(cols.size());
            row.solid=false;
            for(Obj c:cols)if(solidCollider(c))row.solid=true;
            row.known=row.parentActive&&row.colliderCount>0;
            if(ch.kind=="door")doorGroups[ch.group].push_back(row);
            else wallColGroups[ch.group].push_back(row);
        }

        const bool playing=(battleState==5);
        std::vector<DoorRow> allDoors,allWallCols;

        for(auto&[grp,rows]:doorGroups){
            std::set<std::string> sides;
            bool allParent=true;
            for(const auto&r:rows){sides.insert(r.choice.side);if(!r.parentActive)allParent=false;}
            const bool complete=(rows.size()==3&&sides.size()==3&&allParent);
            int inactiveCount=0;bool collidersAgree=true;
            for(const auto&r:rows){
                if(!r.active)++inactiveCount;
                if(r.colliderCount>0&&r.solid!=r.active)collidersAgree=false;
            }
            if(playing&&complete&&inactiveCount==1&&collidersAgree){
                for(auto&r:rows){r.known=true;r.solid=r.active;r.color=r.active?"red":"green";}
            }else{
                bool allKnown=complete;int openCount=0;
                for(const auto&r:rows){if(!r.known)allKnown=false;else if(!r.solid)++openCount;}
                for(auto&r:rows)r.color=(playing&&allKnown&&openCount<=1)?(r.solid?"red":"green"):"yellow";
            }
            for(const auto&r:rows)allDoors.push_back(r);
        }

        for(auto&[grp,rows]:wallColGroups){
            std::set<std::string> sides;bool allParent=true;int activeCount=0;
            for(const auto&r:rows){
                sides.insert(r.choice.side);
                if(!r.parentActive)allParent=false;
                if(r.active)++activeCount;
            }
            const bool paired=(playing&&rows.size()==2&&sides.size()==2&&allParent&&activeCount==1);
            if(paired){
                if(rows[0].choice.kind=="column"){
                    for(auto&r:rows){
                        if(!r.active)r.suppress=true;
                        else{
                            bool pass=(r.choice.side=="through");
                            r.color=pass?"green":"red";
                            r.solid=!pass;
                        }
                    }
                }else{
                    for(auto&r:rows){
                        r.color=r.active?"red":"green";
                        r.solid=r.active;
                    }
                }
            }else{
                for(auto&r:rows){
                    if(r.known)r.color=r.solid?"red":"green";
                    else r.color="yellow";
                }
            }
            for(const auto&r:rows)allWallCols.push_back(r);
        }

        int yesCount=0,noCount=0;
        auto emitDoorMarker=[&](const DoorRow&r){
            if(r.suppress)return;
            std::string lbl=r.color=="green"?"YES":(r.color=="red"?"NO":"CHƯA RÕ");
            if(r.color=="green")++yesCount;else if(r.color=="red")++noCount;
            worldMarkers.push_back({r.anchor,r.color,lbl,r.choice.group+":"+r.choice.side,0,r.color=="green",true,true});
        };
        for(const auto&r:allDoors)emitDoorMarker(r);
        for(const auto&r:allWallCols)emitDoorMarker(r);

        // Build Line (planDoorGuideRoute)
        struct Gate {std::string key;Vec3 center{},anchor{};bool passable=false;};
        std::vector<Gate> gates;
        auto makeGate=[&](const std::string&key,const std::vector<DoorRow>&group,std::size_t expectedSize)->bool{
            if(group.size()!=expectedSize)return false;
            Vec3 c{0,0,0};
            const DoorRow*openRow=nullptr;int openCnt=0;
            for(const auto&r:group){
                c.x+=r.anchor.x;c.y+=r.anchor.y;c.z+=r.anchor.z;
                if(r.color=="green"&&!r.solid){openRow=&r;++openCnt;}
            }
            c.x/=float(group.size());c.y/=float(group.size());c.z/=float(group.size());
            bool pass=(openCnt==1&&openRow!=nullptr);
            gates.push_back({key,c,pass?openRow->anchor:c,pass});
            return true;
        };

        bool gatesOk=playing&&valid(playerPos);
        for(int n=1;n<=4&&gatesOk;++n){
            std::string k="wall"+std::to_string(n);
            gatesOk=makeGate(k,wallColGroups[k],2);
        }
        if(gatesOk){
            const auto&cols=wallColGroups["entrance-column"];
            if(cols.size()==2){
                Vec3 c{(cols[0].anchor.x+cols[1].anchor.x)*0.5f,(cols[0].anchor.y+cols[1].anchor.y)*0.5f,(cols[0].anchor.z+cols[1].anchor.z)*0.5f};
                const DoorRow*act=cols[0].active?&cols[0]:(cols[1].active?&cols[1]:nullptr);
                bool pass=(act&&act->color=="green"&&!act->solid);
                gates.push_back({"entrance-column",c,act?act->anchor:c,pass});
            }else gatesOk=false;
        }
        if(gatesOk)gatesOk=makeGate("door1",doorGroups["door1"],3);

        if(gatesOk&&gates.size()==6){
            float dx=gates.back().center.x-gates.front().center.x;
            float dz=gates.back().center.z-gates.front().center.z;
            bool useX=std::fabs(dx)>std::fabs(dz);
            float delta=useX?dx:dz;
            float sign=delta>0.f?1.f:(delta<0.f?-1.f:0.f);
            if(sign!=0.f&&std::max(std::fabs(dx),std::fabs(dz))>=5.f){
                auto coord=[&](Vec3 p){return sign*(useX?p.x:p.z);};
                std::sort(gates.begin(),gates.end(),[&](const Gate&a,const Gate&b){return coord(a.center)<coord(b.center);});
                float progress=coord(playerPos);
                routeNodes.push_back({{playerPos.x,playerPos.y+0.05f,playerPos.z},"@player",false,0,false});
                auto appendNode=[&](const Gate&g,float offset,const char*suffix){
                    Vec3 a=g.anchor;
                    if(useX)a.x+=sign*offset;else a.z+=sign*offset;
                    a.y+=0.2f;
                    if(distXZ(routeNodes.back().anchor,a)>0.05f){
                        routeNodes.push_back({a,g.key+":"+suffix,false,0,false});
                    }
                };
                constexpr float approach=2.f;
                for(const auto&g:gates){
                    if(progress>coord(g.center)+approach)continue;
                    if(!g.passable){
                        if(progress<coord(g.center)-approach)appendNode(g,-approach,"stop");
                        break;
                    }
                    if(progress<coord(g.anchor)-approach)appendNode(g,-approach,"approach");
                    if(progress<=coord(g.anchor))appendNode(g,0.f,"pass");
                    appendNode(g,approach,"exit");
                }
            }
        }
        currentRouteStage=0;
        espSummary="120006 (Cổng YES/NO): "+std::to_string(yesCount)+" YES, "+std::to_string(noCount)+" NO";
    }

    // 4. Map 120015 (Map Tìm Lỗ + Line)
    void collectHoles120015(Obj gameObj,int battleState,Vec3 playerPos){
        Obj manager=getHideObjectManager(gameObj);
        if(!manager){espSummary="120015: chờ HideObjectManager";return;}
        Obj stampList=nullptr;
        readFieldOrOffset<Obj>(manager,{"hideStampObjectDataList","_hideStampObjectDataList","stampObjectList"},0x40,stampList);
        if(!stampList){espSummary="120015: chờ danh sách lỗ";return;}

        std::string roundKey=std::to_string(reinterpret_cast<std::uintptr_t>(gameObj))+":"+
            std::to_string(reinterpret_cast<std::uintptr_t>(manager))+":"+
            std::to_string(reinterpret_cast<std::uintptr_t>(stampList));
        if(cachedRoundKey!=roundKey||!holeStructureReady){
            holeTiles.clear();holeBoards.clear();holeFinalColliders.clear();holeSavedAnchors.clear();
            cachedRoundKey=roundKey;
            Obj rootGo=topAncestorGO(gameObj);
            if(Klass stampKlass=tryGame("HideStampObjectData")){
                for(Obj s:getComponentsInChildren(rootGo,stampKlass)){
                    if(!s||!alive(s))continue;
                    auto path=hierarchyNames(getTransform(componentGO(s)));
                    HoleLayout layout{};
                    if(parseHolePath(path,layout)){
                        HoleTileRec rec{};rec.comp.set(a,s);rec.layout=layout;
                        holeTiles.push_back(std::move(rec));
                    }
                }
            }
            for(Obj c:getComponentsInChildren(rootGo,unity("Collider"))){
                if(!c||!alive(c))continue;
                Obj tr=getTransform(componentGO(c));
                auto path=hierarchyNames(tr);
                int st=0;std::string var;
                if(parseHoleBoardPath(path,st,var)){
                    Obj p1=getParent(tr),p2=p1?getParent(p1):nullptr;
                    Obj varGo=p2?componentGO(p2):componentGO(c);
                    if(varGo&&alive(varGo)){
                        HoleBoardRec br{};br.col.set(a,c);br.variantGo.set(a,varGo);br.stage=st;br.variant=var;
                        holeBoards.push_back(std::move(br));
                    }
                }else if(path.size()>=3&&path[0]=="MapRoot"&&path[1]=="OutProp"&&path[2]=="Final_Board"){
                    Root r(a,c);holeFinalColliders.push_back(std::move(r));
                }
            }
            holeStructureReady=!holeTiles.empty()&&!holeBoards.empty();
        }
        if(!holeStructureReady){espSummary="120015: đang quét cấu trúc 7 tầng";return;}

        auto registeredVec=readManagedList(stampList,2048);
        std::set<Obj> registry(registeredVec.begin(),registeredVec.end());

        struct LiveBoard {Obj col;Bounds bounds{};float y=0;int stage=0;std::string variant;bool active=false;};
        std::vector<LiveBoard> boards;
        std::map<int,std::string> activeVariantByStage;
        std::map<int,float> stageY;
        for(const auto&br:holeBoards){
            Obj col=br.col.get(),varGo=br.variantGo.get();
            if(!col||!alive(col)||!varGo||!alive(varGo))continue;
            try{
                Bounds b=boundsOfCollider(col);
                Vec3 pos=worldPos(getTransform(componentGO(col)));
                bool act=activeGO(varGo)&&solidCollider(col);
                float y=(act&&b.extents.x>0.f&&b.extents.z>0.f)?(b.center.y+b.extents.y):(pos.y+1.f);
                boards.push_back({col,b,y,br.stage,br.variant,act});
                if(act){activeVariantByStage[br.stage]=br.variant;stageY[br.stage]=y;}
            }catch(...){}
        }

        struct HoleCand {std::string key;Vec3 anchor{};int stage=0;};
        std::map<int,std::vector<HoleCand>> holesByStage;
        for(const auto&tr:holeTiles){
            Obj obj=tr.comp.get();
            if(!obj||!alive(obj)||!registry.contains(obj))continue;
            int st=tr.layout.stage;
            auto itVar=activeVariantByStage.find(st);
            if(itVar==activeVariantByStage.end()||itVar->second!=tr.layout.variant)continue;
            std::int32_t state=-1,maxStep=-1;
            if(!readFieldOrOffset<std::int32_t>(obj,{"_state","state","curState"},0x58,state))continue;
            if(!readFieldOrOffset<std::int32_t>(obj,{"maxStep","_maxStep"},0xE0,maxStep)||maxStep!=0)continue;
            Obj col=nullptr;
            readFieldOrOffset<Obj>(obj,{"_collider","collider","boxCollider"},0x40,col);
            if(!col||!alive(col))continue;
            bool solid=solidCollider(col);
            Bounds b{};
            bool gotBounds=false;
            try{
                b=boundsOfCollider(col);
                gotBounds=(b.extents.x>0.f&&b.extents.z>0.f);
            }catch(...){}
            if(!gotBounds)gotBounds=inactiveHoleBounds(col,obj,b);

            std::string key=std::to_string(reinterpret_cast<std::uintptr_t>(obj));
            Vec3 anchor{};
            bool hasAnchor=false;
            if(gotBounds&&b.extents.x>0.f&&b.extents.z>0.f&&b.extents.y<=std::min(b.extents.x,b.extents.z)){
                anchor={b.center.x,b.center.y+b.extents.y,b.center.z};
                holeSavedAnchors[key]=anchor;
                hasAnchor=true;
            }else if(auto it=holeSavedAnchors.find(key);it!=holeSavedAnchors.end()){
                anchor=it->second;hasAnchor=true;
            }
            if(!hasAnchor)continue;
            bool eligible=(state==0&&solid)||((state==1||state==2||state==3)&&!solid);
            if(!eligible)continue;
            if(std::fabs(anchor.y-stageY[st])>2.5f)continue;

            bool blocked=false;
            for(const auto&bd:boards){
                if(bd.active&&bd.stage==st&&stableBoardBlocks(anchor,bd.col,bd.bounds)){blocked=true;break;}
            }
            if(blocked)continue;
            holesByStage[st].push_back({key,anchor,st});
        }

        // Goal position (Final_Board at +0x1D8 or holeFinalColliders)
        Vec3 goalPos{};bool hasGoal=false;
        Obj endpointGo=nullptr;
        readFieldOrOffset<Obj>(gameObj,{"_endPointGo","endPointGo","_goalObject","goalObject"},0x1D8,endpointGo);
        if(endpointGo&&alive(endpointGo)&&activeGO(endpointGo)){
            try{goalPos=worldPos(getTransform(endpointGo));hasGoal=valid(goalPos);}catch(...){}
        }
        if(!hasGoal){
            for(const auto&fc:holeFinalColliders){
                Obj c=fc.get();
                if(!c||!alive(c)||!solidCollider(c))continue;
                try{
                    Bounds b=boundsOfCollider(c);
                    if(b.extents.x>0.f&&b.extents.z>0.f){
                        goalPos={b.center.x,b.center.y+b.extents.y,b.center.z};
                        hasGoal=valid(goalPos);break;
                    }
                }catch(...){}
            }
        }

        // Backward dynamic programming route (planHoleRoute)
        std::vector<int> remainingStages;
        for(int st=1;st<=7;++st){
            if(stageY.contains(st)&&stageY[st]<=playerPos.y+0.25f&&!holesByStage[st].empty()){
                remainingStages.push_back(st);
            }else if(stageY.contains(st)&&stageY[st]<=playerPos.y+0.25f&&holesByStage[st].empty()){
                break; // Stop contiguous prefix at first unverified floor
            }
        }

        std::set<std::string> selectedKeys;
        if(battleState==5&&valid(playerPos)&&!remainingStages.empty()){
            struct DpOption {float cost=0;Vec3 anchor{};std::vector<HoleCand> path;};
            std::vector<DpOption> next;
            const bool includeGoal=(hasGoal&&remainingStages.back()==7);
            if(includeGoal){
                next.push_back({0.f,goalPos,{}});
            }else{
                int lastSt=remainingStages.back();
                for(const auto&h:holesByStage[lastSt])next.push_back({0.f,h.anchor,{h}});
            }
            int startIdx=includeGoal?int(remainingStages.size())-1:int(remainingStages.size())-2;
            for(int i=startIdx;i>=0;--i){
                int st=remainingStages[std::size_t(i)];
                std::vector<DpOption> cur;
                for(const auto&h:holesByStage[st]){
                    const DpOption*best=nullptr;float bestCost=1e30f;
                    for(const auto&tail:next){
                        float c=distXZ(h.anchor,tail.anchor)+tail.cost;
                        if(!best||c<bestCost){bestCost=c;best=&tail;}
                    }
                    if(best){
                        std::vector<HoleCand> p={h};
                        p.insert(p.end(),best->path.begin(),best->path.end());
                        cur.push_back({bestCost,h.anchor,std::move(p)});
                    }
                }
                next=std::move(cur);
            }
            const DpOption*bestOverall=nullptr;float bestTotal=1e30f;
            for(const auto&opt:next){
                float c=distXZ(playerPos,opt.anchor)+opt.cost;
                if(!bestOverall||c<bestTotal){bestTotal=c;bestOverall=&opt;}
            }
            if(bestOverall){
                constexpr float lift=0.035f;
                currentRouteStage=remainingStages.front();
                routeNodes.push_back({{playerPos.x,playerPos.y+lift,playerPos.z},"@feet",false,currentRouteStage,false});
                Vec3 prev=playerPos;
                for(const auto&h:bestOverall->path){
                    selectedKeys.insert(h.key);
                    float surfY=h.anchor.y+lift;
                    routeNodes.push_back({{prev.x,surfY,prev.z},"@land:"+std::to_string(h.stage),true,h.stage,false});
                    routeNodes.push_back({{h.anchor.x,surfY,h.anchor.z},h.key,false,h.stage,false});
                    prev=h.anchor;
                }
                if(includeGoal){
                    float goalY=goalPos.y+lift;
                    routeNodes.push_back({{prev.x,goalY,prev.z},"@land:goal",true,7,false});
                    routeNodes.push_back({{goalPos.x,goalY,goalPos.z},"@goal",false,7,true});
                }
            }
        }

        int totalHoles=0;
        for(int st=1;st<=7;++st){
            for(const auto&h:holesByStage[st]){
                ++totalHoles;
                bool sel=selectedKeys.contains(h.key);
                worldMarkers.push_back({h.anchor,"green",sel?("T"+std::to_string(st)):"",h.key,st,sel,true,true});
            }
        }
        if(hasGoal){
            worldMarkers.push_back({goalPos,"yellow","ĐÍCH","@goal",8,true,true,true});
        }
        espSummary="120015 (Map Lỗ): "+std::to_string(totalHoles)+" lỗ mở"+(routeNodes.size()>1?" • đã nối Line":"");
    }

    // 5. Map 120041 (MiniGameKMGFogMaze - Đích Mê Cung + Line A*8)
    void collectMaze120041(Obj gameObj,int battleState,Vec3 playerPos){
        Obj fowManager=findRefByClass(gameObj,"FowManager",{"_fowManager","fowManager","FowManager"},0x1D8);
        Obj endpointGo=nullptr;
        readFieldOrOffset<Obj>(gameObj,{"EndPointGo","_endPointGo","endPointGo"},0x1E0,endpointGo);
        if(!endpointGo||!alive(endpointGo)){
            espSummary="120041: chờ cổng đích mê cung";return;
        }
        Vec3 goalPos{};
        try{goalPos=worldPos(getTransform(endpointGo));}catch(...){return;}
        if(!valid(goalPos))return;

        // Always emit ESP Đích Mê Cung marker!
        worldMarkers.push_back({goalPos,"yellow","ĐÍCH","@maze_goal",0,true,true,true});

        Vec3 startPos=playerPos;
        if(fowManager&&alive(fowManager)){
            Obj startTr=nullptr;
            readFieldOrOffset<Obj>(fowManager,{"StartingPoint","_startingPoint","startingPoint"},0x78,startTr);
            if(startTr&&alive(startTr)){
                try{Vec3 sp=worldPos(startTr);if(valid(sp))startPos=sp;}catch(...){}
            }
        }

        if(battleState!=3&&battleState!=4&&battleState!=5){
            espSummary="120041 (Mê Cung): đã hiện ĐÍCH • chờ bắt đầu lượt";
            return;
        }

        std::string mapKey=std::to_string(reinterpret_cast<std::uintptr_t>(gameObj));
        if(maze.key!=mapKey||!maze.meshCol.get()||!alive(maze.meshCol.get())){
            maze=MazeState{};maze.key=mapKey;
            Obj rootGo=topAncestorGO(gameObj);
            for(Obj c:getComponentsInChildren(rootGo,unity("Collider"))){
                if(!c||!alive(c))continue;
                std::string t=classLabel(c);
                auto path=hierarchyNames(getTransform(componentGO(c)));
                std::string var;
                if(t=="MeshCollider"&&parseMazeVariant(path,var)&&solidCollider(c)){
                    maze.meshCol.set(a,c);
                    maze.activeVariant=var;
                    try{
                        maze.meshBounds=boundsOfCollider(c);
                        maze.step=0.5f;
                        maze.x0=maze.meshBounds.center.x-maze.meshBounds.extents.x;
                        maze.z0=maze.meshBounds.center.z-maze.meshBounds.extents.z;
                        maze.width=int(std::ceil(2.f*maze.meshBounds.extents.x/maze.step))+1;
                        maze.height=int(std::ceil(2.f*maze.meshBounds.extents.z/maze.step))+1;
                        if(maze.width>1&&maze.height>1&&maze.width*maze.height<=20000){
                            maze.heights.clear();
                            maze.heights.reserve(std::size_t(maze.width*maze.height));
                        }
                    }catch(...){}
                }else if(t=="BoxCollider"){
                    Root r(a,c);
                    maze.boxColliders.push_back(std::move(r));
                    maze.boxPaths.push_back(std::move(path));
                }
            }
        }

        Obj meshCol=maze.meshCol.get();
        if(!meshCol||!alive(meshCol)||maze.width<=1||maze.height<=1){
            espSummary="120041 (Mê Cung): đã hiện ĐÍCH • chờ biến thể mê cung";
            return;
        }

        // Cooperative height-grid raycast sampling (up to 384 cells per tick)
        const std::size_t totalCells=std::size_t(maze.width*maze.height);
        if(!maze.gridComplete){
            for(int step=0;step<384&&maze.heights.size()<totalCells;++step){
                std::size_t k=maze.heights.size();
                float x=maze.x0+float(k%std::size_t(maze.width))*maze.step;
                float z=maze.z0+float(k/std::size_t(maze.width))*maze.step;
                float top=maze.meshBounds.center.y+maze.meshBounds.extents.y+0.5f;
                float range=maze.meshBounds.extents.y*2.f+1.f;
                RaycastHit hit{};
                if(raycastSingleCollider(meshCol,{x,top,z},{0.f,-1.f,0.f},range,hit)){
                    maze.heights.push_back(std::round(hit.point.y*1000.f)/1000.f);
                }else{
                    maze.heights.push_back(std::numeric_limits<float>::quiet_NaN());
                }
            }
            maze.gridComplete=(maze.heights.size()==totalCells);
            if(!maze.gridComplete){
                espSummary="120041 ("+maze.activeVariant+"): đang dò lưới "+
                    std::to_string(maze.heights.size())+"/"+std::to_string(totalCells);
                return;
            }
        }

        // Collect live solid BoxColliders for active variant and common gates
        Bounds floorB{Vec3{0,startPos.y-0.5f,0},Vec3{50.f,0.5f,50.f}};
        std::vector<Bounds> solidBoxes;
        for(std::size_t i=0;i<maze.boxColliders.size();++i){
            Obj c=maze.boxColliders[i].get();
            if(!c||!alive(c)||!solidCollider(c))continue;
            const auto&p=maze.boxPaths[i];
            auto itRoot=std::find(p.begin(),p.end(),"RandomMaze_Root");
            if(itRoot!=p.end()&&(itRoot+1)!=p.end()&&*(itRoot+1)!=maze.activeVariant)continue;
            try{
                Bounds b=boundsOfCollider(c);
                if(p.size()==4&&p[0]=="MapRoot"&&p[1]=="Environment"&&p[2]=="Default_board"&&p[3]=="Board")floorB=b;
                solidBoxes.push_back(b);
            }catch(...){}
        }

        std::string sig=maze.activeVariant+":"+std::to_string(battleState)+":"+std::to_string(solidBoxes.size());
        if(!maze.routeReady||maze.routeSignature!=sig){
            maze.routeSignature=sig;
            maze.solvedNodes.clear();
            maze.routeReady=false;

            const float floorY=floorB.center.y+floorB.extents.y;
            const float pad=0.25f,bodyH=1.05f,step=maze.step;
            const float x0=std::min({maze.x0,startPos.x-2.f,goalPos.x-2.f});
            const float z0=std::min({maze.z0,startPos.z-2.f,goalPos.z-2.f});
            const float xmax=std::max({maze.x0+float(maze.width-1)*maze.step,startPos.x+2.f,goalPos.x+2.f});
            const float zmax=std::max({maze.z0+float(maze.height-1)*maze.step,startPos.z+2.f,goalPos.z+2.f});
            const int w=int(std::ceil((xmax-x0)/step))+1;
            const int h=int(std::ceil((zmax-z0)/step))+1;
            if(w>2&&h>2&&w*h<=40000){
                std::vector<std::uint8_t> blocked(std::size_t(w*h),0);
                std::vector<std::array<float,4>> obstacles;
                auto stampRect=[&](float ax,float az,float bx,float bz){
                    obstacles.push_back({ax,az,bx,bz});
                    int loX=std::max(0,int(std::ceil((ax-x0)/step)));
                    int hiX=std::min(w-1,int(std::floor((bx-x0)/step)));
                    int loZ=std::max(0,int(std::ceil((az-z0)/step)));
                    int hiZ=std::min(h-1,int(std::floor((bz-z0)/step)));
                    for(int zz=loZ;zz<=hiZ;++zz)for(int xx=loX;xx<=hiX;++xx)blocked[std::size_t(zz*w+xx)]=1;
                };
                for(std::size_t k=0;k<maze.heights.size();++k){
                    float hy=maze.heights[k];
                    if(!std::isfinite(hy)||hy<=floorY+0.2f)continue;
                    float wx=maze.x0+float(k%std::size_t(maze.width))*maze.step;
                    float wz=maze.z0+float(k/std::size_t(maze.width))*maze.step;
                    float r=pad+maze.step*0.5f;
                    stampRect(wx-r,wz-r,wx+r,wz+r);
                }
                for(const Bounds&b:solidBoxes){
                    if(b.center.y+b.extents.y>floorY+0.2f&&b.center.y-b.extents.y<floorY+bodyH-0.1f){
                        stampRect(b.center.x-b.extents.x-pad,b.center.z-b.extents.z-pad,
                                  b.center.x+b.extents.x+pad,b.center.z+b.extents.z+pad);
                    }
                }
                auto cellPt=[&](int idx)->Vec3{return {x0+float(idx%w)*step,floorY+0.04f,z0+float(idx/w)*step};};
                auto nearestFree=[&](Vec3 p)->int{
                    int cx=int(std::round((p.x-x0)/step)),cz=int(std::round((p.z-z0)/step));
                    int best=-1;float bestD=1e30f;
                    for(int r=0;r<=6&&best<0;++r){
                        for(int dz=-r;dz<=r;++dz)for(int dx=-r;dx<=r;++dx){
                            int xx=cx+dx,zz=cz+dz;
                            if(xx<0||xx>=w||zz<0||zz>=h)continue;
                            int idx=zz*w+xx;
                            if(!blocked[std::size_t(idx)]){
                                float d=distXZ(p,cellPt(idx));
                                if(d<bestD){bestD=d;best=idx;}
                            }
                        }
                    }
                    return best;
                };
                int sIdx=nearestFree(startPos),gIdx=nearestFree(goalPos);
                if(sIdx>=0&&gIdx>=0){
                    std::vector<float> dist(std::size_t(w*h),1e30f);
                    std::vector<int> prev(std::size_t(w*h),-1);
                    std::vector<std::uint8_t> closed(std::size_t(w*h),0);
                    using QItem=std::pair<float,int>;
                    std::priority_queue<QItem,std::vector<QItem>,std::greater<QItem>> pq;
                    dist[std::size_t(sIdx)]=0.f;prev[std::size_t(sIdx)]=sIdx;
                    pq.push({0.f,sIdx});
                    int gx=gIdx%w,gz=gIdx/w;
                    while(!pq.empty()){
                        auto[f,u]=pq.top();pq.pop();
                        if(closed[std::size_t(u)])continue;
                        closed[std::size_t(u)]=1;
                        if(u==gIdx)break;
                        int ux=u%w,uz=u/w;
                        float du=dist[std::size_t(u)];
                        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx){
                            if(!dx&&!dz)continue;
                            int nx=ux+dx,nz=uz+dz;
                            if(nx<0||nx>=w||nz<0||nz>=h)continue;
                            int v=nz*w+nx;
                            if(blocked[std::size_t(v)]||closed[std::size_t(v)])continue;
                            if(dx&&dz&&(blocked[std::size_t(uz*w+nx)]||blocked[std::size_t(nz*w+ux)]))continue;
                            float nd=du+((dx&&dz)?1.41421356f:1.f);
                            if(nd+1e-6f<dist[std::size_t(v)]){
                                dist[std::size_t(v)]=nd;prev[std::size_t(v)]=u;
                                float hCost=std::hypot(float(nx-gx),float(nz-gz));
                                pq.push({nd+hCost,v});
                            }
                        }
                    }
                    if(prev[std::size_t(gIdx)]>=0){
                        std::vector<Vec3> chain;
                        for(int cur=gIdx;;cur=prev[std::size_t(cur)]){
                            chain.push_back(cellPt(cur));
                            if(cur==sIdx||prev[std::size_t(cur)]==cur)break;
                        }
                        std::reverse(chain.begin(),chain.end());
                        auto segClear=[&](Vec3 a,Vec3 b){
                            for(const auto&obs:obstacles)if(mazeSegmentHitsBox(a,b,obs))return false;
                            return true;
                        };
                        std::vector<Vec3> smooth={chain.front()};
                        std::size_t at=0;
                        while(at+1<chain.size()){
                            std::size_t next=at+1;
                            for(std::size_t j=chain.size()-1;j>at+1;--j){
                                if(segClear(chain[at],chain[j])){next=j;break;}
                            }
                            smooth.push_back(chain[next]);
                            at=next;
                        }
                        for(std::size_t i=0;i<smooth.size();++i){
                            maze.solvedNodes.push_back({smooth[i],"maze:"+std::to_string(i),false,0,i+1==smooth.size()});
                        }
                        maze.routeReady=(maze.solvedNodes.size()>=2);
                    }
                }
            }
        }

        if(maze.routeReady){
            routeNodes=maze.solvedNodes;
            currentRouteStage=0;
            espSummary="120041 ("+maze.activeVariant+"): đã hiện ĐÍCH + Line mê cung";
        }else{
            espSummary="120041 ("+maze.activeVariant+"): đã hiện ĐÍCH • đang tìm đường";
        }
    }

    void projectCurrentWorldToScreen(Vec3 playerPos){
        points.clear();routeSegments.clear();
        if(worldMarkers.empty()&&routeNodes.empty())return;
        auto cam=invoke(method(unity("Camera"),"get_main",0),nullptr);
        if(!alive(cam)){needsSettle=true;return;}
        Root camRoot(a,cam);
        Rect viewport=unbox<Rect>(call(cam,"get_rect"));
        if(!std::isfinite(viewport.w)||!std::isfinite(viewport.h)||viewport.w<=0.f||viewport.h<=0.f)return;

        auto project=[&](Vec3 p)->ProjectedPt{
            void*v[]={&p};
            Vec3 vp=unbox<Vec3>(call(cam,"WorldToViewportPoint",1,v));
            if(!valid(vp))return {};
            return {viewport.x+vp.x*viewport.w,1.f-(viewport.y+vp.y*viewport.h),vp.z,true};
        };

        // Project world markers with 12-point rings (capped at 96 rings per frame just like Windows agent.js)
        int ringCount=0;
        for(const auto&m:worldMarkers){
            if(!valid(m.anchor)||m.key=="@feet")continue;
            auto p=project(m.anchor);
            if(!p.ok||p.depth<=0.01f||p.x<0.f||p.x>1.f||p.y<0.f||p.y>1.f)continue;
            EspItem item{};
            item.x=p.x;item.y=p.y;item.headY=p.y;
            item.color=m.color;item.selected=m.selected;
            item.distance=valid(playerPos)?std::round(dist3D(m.anchor,playerPos)*10.f)/10.f:0.f;
            if(m.showDistance&&std::isfinite(item.distance)&&item.distance>=0.f&&!m.label.empty()){
                char buf[96];
                if(item.distance<10.f)std::snprintf(buf,sizeof(buf),"%s • %.1fm",m.label.c_str(),double(item.distance));
                else std::snprintf(buf,sizeof(buf),"%s • %.0fm",m.label.c_str(),double(item.distance));
                item.label=buf;
            }else{
                item.label=m.label;
            }
            if(m.showRing&&ringCount<96){
                constexpr float radius=0.8f;
                bool ringOk=true;
                for(int i=0;i<12;++i){
                    float angle=float(i)*(3.1415926535f/6.f);
                    Vec3 rp{m.anchor.x+radius*std::cos(angle),m.anchor.y+0.035f,m.anchor.z+radius*std::sin(angle)};
                    auto qp=project(rp);
                    if(!qp.ok||qp.depth<=0.01f||std::fabs(qp.x)>8.f||std::fabs(qp.y)>8.f){ringOk=false;break;}
                    item.ring[std::size_t(i)]={qp.x,qp.y};
                }
                if(ringOk){item.hasRing=true;++ringCount;}
            }
            points.push_back(std::move(item));
        }

        // Project route segments with near-plane clipping and viewport Liang-Barsky clipping (from agent.js projectRouteSegments)
        if(routeNodes.size()>=2){
            std::vector<ProjectedPt> pts(routeNodes.size());
            for(std::size_t i=0;i<routeNodes.size();++i)pts[i]=project(routeNodes[i].anchor);
            constexpr float nearZ=0.01f;
            for(std::size_t i=1;i<routeNodes.size();++i){
                ProjectedPt aPt=pts[i-1],bPt=pts[i];
                if(!aPt.ok||!bPt.ok)continue;
                if(aPt.depth<nearZ&&bPt.depth<nearZ)continue;
                if(aPt.depth<nearZ||bPt.depth<nearZ){
                    float t=(nearZ-aPt.depth)/(bPt.depth-aPt.depth);
                    Vec3 wa=routeNodes[i-1].anchor,wb=routeNodes[i].anchor;
                    Vec3 cutW{wa.x+t*(wb.x-wa.x),wa.y+t*(wb.y-wa.y),wa.z+t*(wb.z-wa.z)};
                    ProjectedPt cut=project(cutW);
                    if(!cut.ok||cut.depth<=0.f)continue;
                    if(aPt.depth<nearZ)aPt=cut;else bPt=cut;
                }
                float dx=bPt.x-aPt.x,dy=bPt.y-aPt.y;
                float lo=0.f,hi=1.f;bool ok=true;
                const std::pair<float,float> edges[4]={{-dx,aPt.x},{dx,1.f-aPt.x},{-dy,aPt.y},{dy,1.f-aPt.y}};
                for(auto[p,q]:edges){
                    if(std::fabs(p)<1e-12f){if(q<0.f)ok=false;continue;}
                    float t=q/p;
                    if(p<0.f)lo=std::max(lo,t);else hi=std::min(hi,t);
                }
                if(!ok||lo>hi)continue;
                auto clamp01=[](float v){return std::clamp(v,0.f,1.f);};
                RouteSegment seg{};
                seg.a={clamp01(aPt.x+lo*dx),clamp01(aPt.y+lo*dy)};
                seg.b={clamp01(aPt.x+hi*dx),clamp01(aPt.y+hi*dy)};
                seg.drop=routeNodes[i].drop;
                seg.currentStage=(routeNodes[i].stage==currentRouteStage);
                routeSegments.push_back(seg);
            }
        }
    }

    void collectPlazaPlayers(Obj actorObj,Vec3 playerPos){
        auto sys=actorSystem();
        if(!sys)return;
        Obj dict=nullptr;
        Method m=tryMethod(a.object_get_class(sys),"get_OtherActorCharacter",0);
        if(m){
            try{dict=invoke(m,sys);}catch(...){dict=nullptr;}
        }
        if(!dict&&a.hasNativeLayout){
            safeReadAt(sys,0x70,dict);
        }
        if(!dict)return;
        Obj entriesArr=nullptr;
        std::int32_t count=0;
        if(Field f=tryField(a.object_get_class(dict),"_entries"))a.field_get_value(dict,f,&entriesArr);
        else if(a.hasNativeLayout)safeReadAt(dict,0x18,entriesArr);
        if(Field f=tryField(a.object_get_class(dict),"_count"))a.field_get_value(dict,f,&count);
        else if(a.hasNativeLayout)safeReadAt(dict,0x20,count);

        if(!entriesArr||count<=0||count>1024)return;
        auto head=a.array_object_header_size();
        for(std::int32_t i=0;i<count&&i<128;++i){
            Obj otherActor=nullptr;
            std::int64_t uid=0;
            const char* entryPtr=reinterpret_cast<const char*>(entriesArr)+head+std::size_t(i)*24;
            if(!safeReadAt(entryPtr,8,uid)||!safeReadAt(entryPtr,16,otherActor))continue;
            if(!otherActor||!alive(otherActor)||otherActor==actorObj)continue;
            try{
                Obj tr=ref(otherActor,"baseTransfrom");
                if(!tr||!alive(tr))continue;
                Vec3 opos=worldPos(tr);
                float dist=std::hypot(opos.x-playerPos.x,opos.z-playerPos.z);
                if(dist>350.f)continue;
                WorldMarker wm{};
                wm.anchor=opos;
                wm.anchor.y+=1.2f;
                wm.color="yellow";
                wm.showRing=false;
                wm.showDistance=true;
                wm.label="Player";
                worldMarkers.push_back(wm);
            }catch(...){}
        }
    }

    void espTick(Obj actorObj,double time){
        Vec3 playerPos{};
        try{playerPos=position(actorObj);}catch(...){return;}
        if(time-espScanTime>=0.05){
            espScanTime=time;
            worldMarkers.clear();routeNodes.clear();
            Obj gameObj=findMiniGame(time,actorObj);
            if(!gameObj||!alive(gameObj)){
                currentMapName.clear();currentMapId=0;
                collectPlazaPlayers(actorObj,playerPos);
                if(worldMarkers.empty()){
                    espSummary="Đang ở sảnh ("+(controllerName.empty()?"ActorDefaultControlPlayer":controllerName)+") • Vào minigame (Tìm Lỗ, Cổng YES/NO, Mê Cung, Kính, Obby) để hiện vòng & Line";
                }else{
                    espSummary="ESP Sảnh ("+controllerName+"): "+std::to_string(worldMarkers.size())+" người chơi • Vào minigame sẽ tự hiện vòng & Line";
                }
                projectCurrentWorldToScreen(playerPos);
                return;
            }
            currentMapName=classLabel(gameObj);
            currentMapId=getMiniGameId(gameObj);
            int battleState=getBattleState(gameObj);

            if(currentMapName=="MiniGameKMGRealBoard"||(currentMapName=="MiniGameKMGTwistRoad"&&currentMapId==120018)){
                collectRealBoardOrGlass(gameObj,currentMapId==120018&&currentMapName=="MiniGameKMGTwistRoad");
            }else if(currentMapName=="MiniGameKMGTwistRoad"&&currentMapId==120026){
                collectRoad120026(gameObj,battleState);
            }else if(currentMapName=="MiniGameKMGTwistRoad"&&currentMapId==120006){
                collectDoors120006(gameObj,battleState,playerPos);
            }else if(currentMapName=="MiniGameKMGTwistRoad"&&currentMapId==120015){
                collectHoles120015(gameObj,battleState,playerPos);
            }else if(currentMapName=="MiniGameKMGFogMaze"||currentMapId==120041){
                collectMaze120041(gameObj,battleState,playerPos);
            }else{
                espSummary="Map hiện tại ("+currentMapName+" #"+std::to_string(currentMapId)+"): chờ map hỗ trợ";
            }
        }
        projectCurrentWorldToScreen(playerPos);
    }
};

GameBackend::GameBackend(Resolver r):state(std::make_unique<State>(std::move(r))){}
GameBackend::~GameBackend()=default;
const std::vector<EspItem>&GameBackend::esp()const{return state->points;}
const std::vector<RouteSegment>&GameBackend::route()const{return state->routeSegments;}
std::string GameBackend::diagnostic()const{
    auto&s=*state;
    return s.status+"\nController: "+(s.controllerName.empty()?"none":s.controllerName)+
        "\nMiniGame: "+(s.currentMapName.empty()?"none":s.currentMapName+" ("+std::to_string(s.currentMapId)+")")+
        "\nJump requests sent (not confirmed jumps): "+std::to_string(s.jumpRequests)+
        "\nESP markers: "+std::to_string(s.points.size())+" | Line segments: "+std::to_string(s.routeSegments.size())+
        "\nRuntime gate: "+(s.idle?"idle (no game calls)":"active");
}
void GameBackend::stop(Model&m){
    auto&s=*state;m.enabled.fill(false);s.points.clear();s.routeSegments.clear();
    s.clearCaches();s.resetFocus();s.espScanTime=-1;
    s.settleUntil=0;s.retryAfter=0;s.idle=true;
    m.status="Đã tắt ESP, Line và Spam nhảy";s.status=m.status;
}
void GameBackend::suspend(double time){
    auto&s=*state;s.points.clear();s.routeSegments.clear();
    s.clearCaches();s.resetFocus();s.espScanTime=-1;s.retryAfter=0;
    s.needsSettle=true;
    s.settleUntil=std::isfinite(time)?time+.75:0;
}
void GameBackend::toggle(Model&m,int id){
    if(id<0||id>=FeatureCount||!m.available[id]||!m.connected||!m.controlsAllowed)return;
    bool on=!m.enabled[id];m.enabled[id]=on;
    if(id==SpamJump){state->nextJump=0;state->lastJump=-1;if(!on)state->controlRoot.reset();}
    if(id==Esp&&!on)state->points.clear();
    if(id==Line&&!on)state->routeSegments.clear();
    state->settleUntil=0;
    const char*label=id==Esp?"ESP":(id==Line?"Line":"Spam nhảy");
    m.toast=std::string(label)+(on?" • BẬT":" • TẮT");m.toastUntil=m.time+1.;
}
void GameBackend::tick(Model&m,double time){
    auto&s=*state;
    try{
        if(!m.connected||!m.controlsAllowed){
            if(!s.idle)stop(m);
            else{m.status="Chờ kết nối game";s.status=m.status;}
            return;
        }
        if(!std::isfinite(time)||time<0){s.points.clear();s.routeSegments.clear();m.status="Thời gian cập nhật không hợp lệ";return;}
        if(time<s.lastTick){s.nextJump=0;s.lastJump=-1;s.espScanTime=-1;s.settleUntil=0;s.retryAfter=0;}
        s.lastTick=time;
        if(!runtimeRequested(m)){
            s.points.clear();s.routeSegments.clear();
            if(!s.idle){s.clearCaches();s.resetFocus();s.espScanTime=-1;s.idle=true;m.player.clear();}
            s.status="Chờ bật ESP, Line hoặc Spam nhảy";m.status=s.status;
            return;
        }
        if(!m.license.valid){
            s.points.clear();s.routeSegments.clear();
            s.status="Chờ kích hoạt Key bản quyền (45.131.65.244:30019)";
            m.status=s.status;
            return;
        }
        s.idle=false;
        if(time<s.retryAfter){m.status=s.status;return;}
        auto actorObj=s.actor();
        if(!s.alive(actorObj)){
            s.points.clear();s.routeSegments.clear();s.clearCaches();s.resetFocus();m.player.clear();
            s.needsSettle=true;
            s.status="Chờ nhân vật local";m.status=s.status;return;
        }
        auto uid=std::to_string(s.read<std::int64_t>(actorObj,s.field(s.a.object_get_class(actorObj),"<SUID>k__BackingField"),0x0a));
        if(actorObj!=s.actorRoot.get()||uid!=s.actorUID){
            const bool hadActor=s.actorRoot.get()!=nullptr;
            s.clearCaches();s.resetFocus();s.actorRoot.set(s.a,actorObj);s.actorUID=uid;
            if(hadActor||s.needsSettle){s.settleUntil=time+.75;s.needsSettle=false;}
        }
        m.player=uid;
        if(time<s.settleUntil){s.points.clear();s.routeSegments.clear();s.status="Đang chờ cảnh ổn định";m.status=s.status;return;}
        std::string detail;
        auto control=s.currentControl(actorObj);(void)control;
        if(m.enabled[SpamJump]){
            try{s.jumpTick(actorObj,time,m.jumpIntervalMs,detail,m.jumpPressed);}
            catch(const std::exception&e){s.nextJump=time+.25;detail=std::string("Spam nhảy: ")+e.what();}
        }else s.controlRoot.reset();
        if(m.enabled[Esp]||m.enabled[Line]){
            try{
                s.espTick(actorObj,time);
                if(!m.enabled[Esp])s.points.clear();
                if(!m.enabled[Line])s.routeSegments.clear();
                if(detail.empty()&&!s.espSummary.empty())detail=s.espSummary;
            }catch(const std::exception&e){
                s.points.clear();s.routeSegments.clear();s.needsSettle=true;
                if(!detail.empty())detail+=" | ";
                detail+=std::string("ESP/Line: ")+e.what();
            }
        }else{
            s.points.clear();s.routeSegments.clear();
        }
        s.retryAfter=0;
        m.status=detail.empty()?"ESP Minigame / Line / Tự nhảy đang hoạt động":detail;s.status=m.status;
    }catch(const std::exception&e){
        s.points.clear();s.routeSegments.clear();s.clearCaches();s.resetFocus();m.player.clear();
        s.needsSettle=true;
        s.status=std::string("Chờ cảnh: ")+e.what();m.status=s.status;
        s.retryAfter=std::isfinite(time)?time+.75:0;
    }catch(...){
        s.points.clear();s.routeSegments.clear();s.clearCaches();s.resetFocus();m.player.clear();
        s.needsSettle=true;
        s.status="Backend lỗi không xác định";m.status=s.status;
        s.retryAfter=std::isfinite(time)?time+.75:0;
    }
}
}
