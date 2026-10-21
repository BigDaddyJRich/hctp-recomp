// Verified CChar code +0x30; CCharProp copies tagCharacterGameProp at +0x20.
// Only the per-character instance is changed, never the save/template records.
struct MediaDefault { int music=-1, video=-1; };
std::map<std::pair<unsigned,int>,MediaDefault> g_mediaDefaults;
PS2Runtime::RecompiledFunction g_originalMediaPropCtor=nullptr;

void mediaDefaultPropertyCtor(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    const uint32_t self=uint32_t(_mm_cvtsi128_si32(ctx->r[4]))&0x1ffffffu;
    const uint32_t character=uint32_t(_mm_cvtsi128_si32(ctx->r[5]))&0x1ffffffu;
    g_originalMediaPropCtor(ram,ctx,runtime);
    if (!self || self>0x2000000u-0x190u || !character || character>0x2000000u-0x34u) return;
    uint32_t code;std::memcpy(&code,ram+character+0x30,4);
    if (code>=8600u) return; // no CAWs, props, or expanded roster assumptions
    const unsigned id=code/100u;
    const int attire=int(code/10u%10u);
    auto chosen=g_mediaDefaults.find({id,attire});
    if(chosen==g_mediaDefaults.end())chosen=g_mediaDefaults.find({id,-1});
    if(chosen==g_mediaDefaults.end())return;
    const uint32_t base=0x4c0f54u+id*0x170u;
    const auto &m=chosen->second;
    // Saved custom choices that differ from the original template take priority.
    // Default-only substitutions do not make Create-a-Moveset choices unusable.
    if(m.music>=0 && ram[self+0x17f]==ram[base+0x15f])ram[self+0x17f]=uint8_t(m.music);
    if(m.video>=0 && ram[self+0x17e]==ram[base+0x15e])ram[self+0x17e]=uint8_t(m.video);
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
        if(!g_originalMediaPropCtor || !runtime.replaceFunction(0x1cd1d0u,mediaDefaultPropertyCtor))
            g_mediaDefaults.clear();
    }
    std::fprintf(stderr,"[hctp] media defaults: %zu wrestler/attire entries; saved custom choices preserved\n",g_mediaDefaults.size());
}
