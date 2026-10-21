// Host-side imported media: stock AFS/PAC layouts remain untouched.
std::map<std::pair<char, uint32_t>, std::pair<uint16_t, uint16_t>> g_hostRoutes;
std::map<std::pair<char, uint32_t>, std::string> g_hostMedia;
PS2Runtime::RecompiledFunction g_hostOrigMusic = nullptr, g_hostOrigVideo = nullptr;

const std::string *hostMediaPath(char kind, uint32_t partition, uint32_t entry)
{
    if (partition > 65535 || entry > 65535) return nullptr;
    auto it = g_hostMedia.find({kind, (partition << 16) | entry});
    return it == g_hostMedia.end() ? nullptr : &it->second;
}

bool routeHostMedia(char kind, uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t id = uint32_t(_mm_cvtsi128_si32(ctx->r[5]));
    const uint32_t dest = uint32_t(_mm_cvtsi128_si32(ctx->r[4]));
    auto it = g_hostRoutes.find({kind, id});
    if (it == g_hostRoutes.end() || dest > 0x2000000 - 4) return false;
    std::memcpy(rdram + dest, &it->second.first, 2);
    std::memcpy(rdram + dest + 2, &it->second.second, 2);
    returnValue(ctx, 0);
    return true;
}

void hostMusicIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!routeHostMedia('M', rdram, ctx)) g_hostOrigMusic(rdram, ctx, runtime);
}
void hostVideoIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!routeHostMedia('V', rdram, ctx)) g_hostOrigVideo(rdram, ctx, runtime);
}

void installHostMedia(PS2Runtime &runtime)
{
    std::ifstream file("HOST_MEDIA.txt");
    char kind; uint32_t part, entry; std::string path;
    while (file >> kind >> part >> entry >> path)
    {
        if ((kind != 'M' && kind != 'V') || part > 65535 || entry > 65535 ||
            path.rfind("MODS/MEDIA/", 0) != 0 || path.find("..") != std::string::npos ||
            path.find(':') != std::string::npos || path.find('\\') != std::string::npos)
        {
            g_hostMedia.clear();
            std::fprintf(stderr, "[hctp] invalid HOST_MEDIA row; import disabled\n");
            return;
        }
        g_hostMedia[{kind, (part << 16) | entry}] = path;
    }
    if (g_hostMedia.empty()) return;
    std::ifstream routes("MEDIA_ROUTES.txt");
    uint32_t id;
    while (routes >> kind >> id >> part >> entry)
    {
        if (id > 255 || !hostMediaPath(kind, part, entry))
        {
            g_hostRoutes.clear(); g_hostMedia.clear();
            std::fprintf(stderr, "[hctp] missing imported media route; import disabled\n");
            return;
        }
        g_hostRoutes[{kind,id}] = {uint16_t(part),uint16_t(entry)};
    }
    g_hostOrigMusic = runtime.lookupFunction(0x1b0490u);
    g_hostOrigVideo = runtime.lookupFunction(0x1b05b0u);
    if (!g_hostOrigMusic || !g_hostOrigVideo)
    {
        g_hostRoutes.clear(); g_hostMedia.clear();
        return;
    }
    runtime.replaceFunction(0x1b0490u, hostMusicIndex);
    runtime.replaceFunction(0x1b05b0u, hostVideoIndex);
    std::fprintf(stderr, "[hctp] host media: %zu files, %zu added routes; stock archives unchanged\n",
                 g_hostMedia.size(), g_hostRoutes.size());
}
