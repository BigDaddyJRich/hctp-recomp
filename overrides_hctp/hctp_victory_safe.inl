// Mapped preview IDs MUST bypass the stock 86-byte stack lookup.
void convertDoramockFromMoves(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t p=uint32_t(_mm_cvtsi128_si32(ctx->r[4]));
    if (p>0x02000000u-4u)
    {
        std::fprintf(stderr,"[victory] invalid guest reference; preview rejected\n");
        ctx->pc=uint32_t(_mm_cvtsi128_si32(ctx->r[31]));
        return;
    }
    int32_t n;std::memcpy(&n,rdram+p,4);
    static const char *test=std::getenv("HCTP_VICTORY_TEST");
    static const int32_t orphans[]={6202,6208,6220,6223,6226,6236,6240,6243,6258,6261,6262,6263,6264,
        6265,6266,6267,6268,6269,6270,6271,6272,6273,6274,6275,6284,6285,6286,6287,6288,6289,
        6290,6291,6292,6293,6294,6295,6296,6297,6298,6299,6300,6301,6314,6315,6316,6317,6318,
        6319,6320,6321,6322,6323,6328,6329,6331,6332,6333,6334,6335,6336,6337,6338,6339,6340,
        6341,6342,6344,6345,6346,6347,6348,6349,6350};
    static size_t cycle=0;
    auto it=g_victoryEvent.find(uint32_t(n));
    int32_t event=test?(std::strcmp(test,"cycle")==0?orphans[cycle++%(sizeof(orphans)/sizeof(orphans[0]))]:std::atoi(test)):
        it!=g_victoryEvent.end()?int32_t(it->second):-1;
    if (event<0 && n>=0 && n<86)
    {
        g_origConvertDoramock(rdram,ctx,runtime);
        return;
    }
    if (event<6200 || event>6455)
    {
        std::fprintf(stderr,"[victory] unsupported list %d / event %d; using safe default\n",n,event);
        event=6259;
    }
    std::memcpy(rdram+p,&event,4);
    std::fprintf(stderr,"[victory] list %d -> event %d (stock lookup bypassed)\n",n,event);
    ctx->pc=uint32_t(_mm_cvtsi128_si32(ctx->r[31]));
}
