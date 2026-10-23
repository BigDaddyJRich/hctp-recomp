// Verified CChar code +0x30; CCharProp copies tagCharacterGameProp at +0x20.
// CAM's editable copy must see the same defaults as the character instance.
#include "hctp_media_defaults.h"
using MediaDefault = hctp_media_defaults::MediaDefault;
std::map<std::pair<unsigned,int>,MediaDefault> g_mediaDefaults;
PS2Runtime::RecompiledFunction g_originalMediaPropCtor=nullptr;
PS2Runtime::RecompiledFunction g_originalMediaEntranceCtor=nullptr;

bool applyMediaDefaults(uint8_t *ram, uint32_t record, uint32_t code)
{
    if (code >= 8600u || record > 0x2000000u - 0x170u) return false;
    const unsigned id=code/100u;
    const int attire=int(code/10u%10u);
    auto chosen=g_mediaDefaults.find({id,attire});
    if(chosen==g_mediaDefaults.end())chosen=g_mediaDefaults.find({id,-1});
    if(chosen==g_mediaDefaults.end())return false;
    return hctp_media_defaults::apply(ram+record,ram+0x4c0f54u+id*0x170u,chosen->second);
}

void mediaDefaultPropertyCtor(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    const uint32_t self=uint32_t(_mm_cvtsi128_si32(ctx->r[4]))&0x1ffffffu;
    const uint32_t character=uint32_t(_mm_cvtsi128_si32(ctx->r[5]))&0x1ffffffu;
    g_originalMediaPropCtor(ram,ctx,runtime);
    if (!self || self>0x2000000u-0x190u || !character || character>0x2000000u-0x34u) return;
    uint32_t code;std::memcpy(&code,ram+character+0x30,4);
    applyMediaDefaults(ram,self+0x20u,code);
}

void mediaDefaultEntranceCtor(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    const uint32_t self=uint32_t(_mm_cvtsi128_si32(ctx->r[4]))&0x1ffffffu;
    g_originalMediaEntranceCtor(ram,ctx,runtime);
    if (!self || self>0x2000000u-0x2a4u) return;
    // g_pChar[0] is the selected CAM character. The original entrance constructor
    // retains it while removing the other preview actors. No save/template edit.
    uint32_t character=0,code=0;
    std::memcpy(&character,ram+0x526ea0u,4);
    if (!character || character>0x2000000u-0x34u) {
        std::fprintf(stderr,"[hctp] CAM media: no selected character (ptr %08x)%c",character,10);
        return;
    }
    std::memcpy(&code,ram+character+0x30u,4);
    // CMovesMenuBase copies tagMovesData to this+0xb4. Its character property is
    // at +0x14: Draw reads video at this+0x226 and music at this+0x227.
    const unsigned id=code/100u;
    const int attire=int(code/10u%10u);
    const auto found=g_mediaDefaults.find({id,attire});
    const auto fallback=g_mediaDefaults.find({id,-1});
    const bool changed=applyMediaDefaults(ram,self+0xc8u,code);
    std::fprintf(stderr,"[hctp] CAM media cast %u selected %08x configured %d changed %d music %u video %u stock %u/%u%c",
                 code,character,int(found!=g_mediaDefaults.end() || fallback!=g_mediaDefaults.end()),int(changed),
                 unsigned(ram[self+0x227u]),unsigned(ram[self+0x226u]),
                 code<8600u ? unsigned(ram[0x4c0f54u+id*0x170u+0x15fu]) : 0u,
                 code<8600u ? unsigned(ram[0x4c0f54u+id*0x170u+0x15eu]) : 0u,10);
}

void installMediaDefaults(PS2Runtime &runtime)
{
    std::ifstream cfg("MEDIA_DEFAULTS.txt");
    for(std::string line;std::getline(cfg,line);)
    {
        int id,attire,music,video;
        if(line.empty() || line[0]=='#')continue;
        if(std::sscanf(line.c_str(),"%d %d %d %d",&id,&attire,&music,&video)!=4 ||
           id<0 || id>=86 || attire < -1 || attire>9 || music < -1 || music>255 || video < -1 || video>255)
        {
            g_mediaDefaults.clear();std::fprintf(stderr,"[hctp] invalid MEDIA_DEFAULTS; disabled\n");return;
        }
        g_mediaDefaults[{unsigned(id),attire}]={music,video};
    }
    if(!g_mediaDefaults.empty())
    {
        g_originalMediaPropCtor=runtime.lookupFunction(0x1cd1d0u);
        g_originalMediaEntranceCtor=runtime.lookupFunction(0x352640u);
        if(!g_originalMediaPropCtor || !g_originalMediaEntranceCtor ||
           !runtime.replaceFunction(0x1cd1d0u,mediaDefaultPropertyCtor) ||
           !runtime.replaceFunction(0x352640u,mediaDefaultEntranceCtor))
            g_mediaDefaults.clear();
    }
    std::fprintf(stderr,"[hctp] media defaults: %zu wrestler/attire entries; saved custom choices preserved\n",g_mediaDefaults.size());
}
