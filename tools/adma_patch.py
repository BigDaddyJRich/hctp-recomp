# AutoDMA input: copy the transfer's L/R blocks from IOP RAM at DMA COMPLETION, not at the kick.
# Movie (Sofdec ADX via CRI rna) audio: right channel exactly 8192 samples behind + pops on the right: the driver
# kicks the transfer, then writes (part of) the block; the copy at the kick took stale data.
import sys
root = sys.argv[1]  # F:/hctp-newbase/PS2Recomp/ps2xIOP/src/emulator

def patch(path, pairs):
    p = root + '/' + path
    s = open(p, encoding='utf-8', newline='').read()
    CR, LF = chr(13), chr(10)
    crlf = (CR + LF) in s
    s = s.replace(CR + LF, LF)
    for old, new in pairs:
        if old not in s:
            raise SystemExit('NOT FOUND in ' + path + ': ' + old[:70])
        s = s.replace(old, new, 1)
    if crlf:
        s = s.replace(LF, CR + LF)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched', path)

patch('core/iop_memory.h', [
    ("            std::vector<int16_t> pcm; // AutoDMA: the stereo frames (L, R interleaved, 48 kHz) the SPU2 plays\n",
     "            std::vector<int16_t> pcm; // AutoDMA: the stereo frames (L, R interleaved, 48 kHz) the SPU2 plays\n"
     "            uint32_t pcmSource = 0;   // AutoDMA read at completion: the L/R blocks in IOP RAM (see autoDmaFrames)\n"
     "            uint32_t pcmBytes = 0;\n"),
    ("        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;\n",
     "        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;\n"
     "        // AutoDMA blocks (512 bytes L, 512 bytes R each) at source -> 48 kHz stereo frames (L, R interleaved)\n"
     "        [[nodiscard]] std::vector<int16_t> autoDmaFrames(uint32_t source, uint32_t bytes) const;\n"),
])

patch('core/iop_memory.cpp', [
    ("""            if (bytes != 0u && source < RamSize && bytes <= RamSize - source)
            {
                constexpr uint32_t kBlockSamples = 256u; // 512 bytes per channel per block
                start.pcm.resize(static_cast<size_t>(bytes / 2u));
                size_t out = 0u;
                for (uint32_t block = 0u; block < bytes; block += 0x400u)
                {
                    const uint8_t *left = m_ram.data() + source + block;
                    const uint8_t *right = left + 0x200u;
                    for (uint32_t i = 0u; i < kBlockSamples; ++i)
                    {
                        int16_t l, r;
                        std::memcpy(&l, left + i * 2u, sizeof(l));
                        std::memcpy(&r, right + i * 2u, sizeof(r));
                        start.pcm[out++] = l;
                        start.pcm[out++] = r;
                    }
                }
                start.delayCycles""",
     """            if (bytes != 0u && source < RamSize && bytes <= RamSize - source)
            {
                // The SPU2 reads AutoDMA data over the transfer, not at the kick: read it at completion
                // (iop_emulator), so a block the driver finishes writing after kicking is not taken stale (CRI rna
                // movie audio: the right channel came 8192 samples late, with pops). VENDETTA_ADMA_AT_KICK=1 = old.
                static const bool atKick = std::getenv("VENDETTA_ADMA_AT_KICK") != nullptr;
                if (atKick)
                    start.pcm = autoDmaFrames(source, bytes);
                else
                {
                    start.pcmSource = source;
                    start.pcmBytes = bytes;
                }
                start.delayCycles"""),
    ("""    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {""",
     """    std::vector<int16_t> IopMemory::autoDmaFrames(uint32_t source, uint32_t bytes) const
    {
        constexpr uint32_t kBlockSamples = 256u; // 512 bytes per channel per block
        std::vector<int16_t> pcm;
        if (bytes == 0u || source >= RamSize || bytes > RamSize - source)
            return pcm;
        pcm.resize(static_cast<size_t>(bytes / 2u));
        size_t out = 0u;
        for (uint32_t block = 0u; block + 0x400u <= bytes; block += 0x400u)
        {
            const uint8_t *left = m_ram.data() + source + block;
            const uint8_t *right = left + 0x200u;
            for (uint32_t i = 0u; i < kBlockSamples; ++i)
            {
                int16_t l, r;
                std::memcpy(&l, left + i * 2u, sizeof(l));
                std::memcpy(&r, right + i * 2u, sizeof(r));
                pcm[out++] = l;
                pcm[out++] = r;
            }
        }
        pcm.resize(out);
        return pcm;
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {"""),
])

patch('iop_emulator.cpp', [
    ("""                if (!dma->pcm.empty())
                    memory.spu2().queueInput(dma->core, dma->pcm.data(), dma->pcm.size() / 2u);""",
     """                if (!dma->pcm.empty())
                    memory.spu2().queueInput(dma->core, dma->pcm.data(), dma->pcm.size() / 2u);
                if (dma->pcmBytes != 0u)
                    pendingAutoDma[dma->irq] = {dma->core, dma->pcmSource, dma->pcmBytes};"""),
    ("""                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);""",
     """                completed.push_back(it->first);
                // AutoDMA: the transfer's data as it stands at completion (before the refill this interrupt asks for)
                if (const auto adma = pendingAutoDma.find(it->first); adma != pendingAutoDma.end())
                {
                    const std::vector<int16_t> frames = memory.autoDmaFrames(adma->second.source, adma->second.bytes);
                    if (!frames.empty())
                        memory.spu2().queueInput(adma->second.core, frames.data(), frames.size() / 2u);
                    pendingAutoDma.erase(adma);
                }
                it = pendingDmaInterrupts.erase(it);"""),
    ("""            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();""",
     """            pendingDmaInterrupts.clear();
            pendingAutoDma.clear();
            pendingGuestCallbacks.clear();"""),
    ("""        std::map<int, uint64_t> pendingDmaInterrupts;""",
     """        std::map<int, uint64_t> pendingDmaInterrupts;
        struct PendingAutoDma
        {
            int core = 0;
            uint32_t source = 0;
            uint32_t bytes = 0;
        };
        std::map<int, PendingAutoDma> pendingAutoDma; // irq -> the AutoDMA transfer read at its completion"""),
])
