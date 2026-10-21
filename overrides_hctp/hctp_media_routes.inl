// Imported media routes. Data is local to the isolated game's working directory.
// Music and video IDs have separate namespaces in MOVES/LIST.
std::map<std::pair<char, uint32_t>, std::pair<uint16_t, uint16_t>> g_mediaRoutes;
PS2Runtime::RecompiledFunction g_origMediaMusic = nullptr, g_origMediaVideo = nullptr;
PS2Runtime::RecompiledFunction g_origMediaAfsEntry = nullptr;
std::map<uint32_t, uint32_t> g_mediaAfsCounts;

void importedAfsEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // AddAFSEnt allocates 0x11c + 2*capacity bytes; CRI populates this from
    // the archive's actual count. A larger AFS without a larger work area
    // overwrites the next game-heap block, even with native music playback.
    const uint32_t part = uint32_t(_mm_cvtsi128_si32(ctx->r[4]));
    const uint32_t capacity = uint32_t(_mm_cvtsi128_si32(ctx->r[6]));
    auto it = g_mediaAfsCounts.find(part);
    if (it != g_mediaAfsCounts.end() && it->second > capacity)
    {
        ctx->r[6] = _mm_set_epi64x(0, it->second);
        std::fprintf(stderr, "[hctp] AFS partition %u work capacity %u -> %u\n", part, capacity, it->second);
    }
    g_origMediaAfsEntry(rdram, ctx, runtime);
}



bool importedMediaRoute(char kind, uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t id = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
    auto it = g_mediaRoutes.find({kind, id});
    if (it == g_mediaRoutes.end())
        return false;
    const uint32_t out = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
    if (out > 0x02000000u - 4u)
        return false;
    std::memcpy(rdram + out, &it->second.first, 2);
    std::memcpy(rdram + out + 2, &it->second.second, 2);
    static unsigned logged = 0;
    if (logged++ < 16)
        std::fprintf(stderr, "[hctp] imported media %c %u -> partition %u entry %u\n", kind, id,
                     unsigned(it->second.first), unsigned(it->second.second));
    returnValue(ctx, 0);
    return true;
}

void importedMusicIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!importedMediaRoute('M', rdram, ctx))
        g_origMediaMusic(rdram, ctx, runtime);
}

void importedVideoIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!importedMediaRoute('V', rdram, ctx))
        g_origMediaVideo(rdram, ctx, runtime);
}

void installImportedMedia(PS2Runtime &runtime)
{
    std::ifstream file("MEDIA_ROUTES.txt");
    char kind;
    uint32_t id, partition, entry;
    while (file >> kind >> id >> partition >> entry)
    {
        if ((kind != 'M' && kind != 'V') || id > 255 || entry > 32767 ||
            (kind == 'M' && partition != 4) || (kind == 'V' && partition != 7))
        {
            std::fprintf(stderr, "[hctp] invalid imported media route; import disabled\n");
            g_mediaRoutes.clear();
            return;
        }
        g_mediaRoutes[{kind, id}] = {uint16_t(partition), uint16_t(entry)};
    }
    if (g_mediaRoutes.empty())
        return;
    g_origMediaMusic = runtime.lookupFunction(0x1b0490u);
    g_origMediaVideo = runtime.lookupFunction(0x1b05b0u);
    if (!g_origMediaMusic || !g_origMediaVideo)
    {
        std::fprintf(stderr, "[hctp] imported media hooks unavailable\n");
        return;
    }
    runtime.replaceFunction(0x1b0490u, importedMusicIndex);
    runtime.replaceFunction(0x1b05b0u, importedVideoIndex);
    const char *archives[] = {"BGM/BGM000.AFS", "BGM/BGM001.AFS", "BGM/BGM002.AFS", "BGM/BGM003.AFS", "BGM/BGM004.AFS",
        "MOVIE/NMV000.AFS", "MOVIE/NMV001.AFS", "MOVIE/NMV002.AFS", "MOVIE/NMV003.AFS", "MOVIE/LOGOS.AFS"};
    for (uint32_t part = 0; part < 10; ++part)
    {
        std::ifstream afs(archives[part], std::ios::binary);
        uint8_t header[8] = {};
        if (!afs.read(reinterpret_cast<char *>(header), 8) || std::memcmp(header, "AFS\0", 4)) continue;
        uint32_t count; std::memcpy(&count, header + 4, 4);
        if (count > 0 && count <= 4096) g_mediaAfsCounts[part] = count;
    }
    g_origMediaAfsEntry = runtime.lookupFunction(0x15a3b0u);
    if (g_origMediaAfsEntry) runtime.replaceFunction(0x15a3b0u, importedAfsEntry);
    std::fprintf(stderr, "[hctp] imported media routes: %zu\n", g_mediaRoutes.size());
}
