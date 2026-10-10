// Def Jam Vendetta (USA, SLUS-20639) — IOP routines run natively (ps2x/iop/iop_native_hooks.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// EA's software mixer in SNDDRV.IRX (loaded at 0x24200): the leaf routine 0x2A3FC-0x2A5E4, over half of the IOP's
// instructions. Two outer passes of two inner ones; pass p = outer * 2 + inner takes an input offset (in samples) and
// an accumulator-table index from two 8-byte tables (0x2EDE4 / 0x2EDD4, copied to its stack frame), the accumulator
// buffer *(0x2FC20 + index * 4) + outer * 0x400, and for 256 samples writes
//   out[k] = clamp(in[k] + (acc[k] >> 7), -32768, 32767), acc[k] = 0
// to output + p * 0x200 (input $a0, output $a1). Run natively it leaves memory and registers as the interpreted routine
// does and charges the IOP the same number of instructions (they depend only on which samples clamp).
// VENDETTA_IOP_NATIVE=0 interprets it again; VENDETTA_IOP_NATIVE_VERIFY=1 interprets it and checks the native result.
#include "ps2_runtime.h"
#include "ps2x/iop/iop_native_hooks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vendetta_iop
{
    constexpr uint32_t kMixer = 0x2A3FCu;
    constexpr uint32_t kMixerBytes = 0x1E8u;              // to 0x2A5E4: the last instruction is the delay slot of jr $ra
    constexpr uint64_t kMixerHash = 0x49ECC60E9D3A8047ull; // FNV-1a of those bytes as relocated at 0x24200
    constexpr uint32_t kInputOffsets = 0x2EDE4u;           // 4 halfwords: each pass's offset into the input, in samples
    constexpr uint32_t kAccIndices = 0x2EDD4u;             // 4 halfwords: each pass's entry in the accumulator table
    constexpr uint32_t kAccTable = 0x2FC20u;               // the accumulator buffers' addresses
    constexpr uint32_t kSamples = 256u;

    enum Reg : uint32_t
    {
        V0 = 2, V1 = 3, A0 = 4, A1 = 5, A2 = 6, A3 = 7, T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, T5 = 13, T6 = 14,
        T7 = 15, S0 = 16, T9 = 25, SP = 29
    };
    constexpr uint32_t kResultRegs[] = {V0, V1, A0, A1, A2, A3, T0, T1, T2, T3, T4, T5, T6, T7, S0, T9, SP};

    struct Mix
    {
        uint32_t outAddr[4] = {};
        uint32_t accAddr[4] = {};
        uint16_t out[4][kSamples] = {};
        uint32_t regs[32] = {};
        uint32_t stackAddr = 0;
        uint8_t stack[20] = {};
        uint32_t instructions = 0;
    };

    bool overlaps(uint32_t a, uint32_t aSize, uint32_t b, uint32_t bSize)
    {
        const uint32_t pa = a & 0x1FFFFFFFu, pb = b & 0x1FFFFFFFu;
        return pa < pb + bSize && pb < pa + aSize;
    }

    // Is [address, address + size) IOP RAM?
    bool inRam(const ps2x::iop::NativeCall &call, uint32_t address, uint32_t size)
    {
        uint8_t probe = 0;
        return call.readRam(address, &probe, 1) && call.readRam(address + size - 1u, &probe, 1) &&
               (address & 0x1FFFFFFFu) + size > (address & 0x1FFFFFFFu);
    }

    // The routine's effect. All addresses are worked out and checked first, so nothing is written unless the whole call
    // is done natively. commit = write each pass to IOP RAM before the next reads (exact even when a later pass reads
    // what an earlier one wrote); without it (the verify mode's prediction, which writes nothing) such a call is
    // refused. false = not handled (the IOP interprets the routine).
    bool runMixer(ps2x::iop::NativeCall &call, Mix &mix, bool commit)
    {
        const uint32_t input = call.reg(A0), output = call.reg(A1), sp = call.reg(SP);
        uint8_t tables[16];
        int16_t inputOffsets[4], accIndices[4];
        uint32_t accIndexWord = 0;
        if (!call.readRam(kInputOffsets, tables, 8) || !call.readRam(kAccIndices, tables + 8, 8))
            return false;
        std::memcpy(inputOffsets, tables, 8);
        std::memcpy(accIndices, tables + 8, 8);
        std::memcpy(&accIndexWord, tables + 8, 4);

        uint32_t in[4], acc[4], out[4];
        for (uint32_t pass = 0; pass < 4u; ++pass)
        {
            const uint32_t outer = pass / 2u;
            in[pass] = input + (static_cast<uint32_t>(static_cast<int32_t>(inputOffsets[pass])) << 1u);
            uint32_t accPointer = 0u;
            if (!call.readRam(kAccTable + (static_cast<uint32_t>(static_cast<int32_t>(accIndices[pass])) << 2u), &accPointer, 4))
                return false;
            acc[pass] = accPointer + (outer << 10u);
            out[pass] = output + pass * 0x200u;
            if (!inRam(call, in[pass], kSamples * 2u) || !inRam(call, acc[pass], kSamples * 4u) ||
                !inRam(call, out[pass], kSamples * 2u))
                return false;
            // within a pass the routine interleaves its reads and writes sample by sample: no overlap allowed
            if (overlaps(out[pass], kSamples * 2u, in[pass], kSamples * 2u) || overlaps(out[pass], kSamples * 2u, acc[pass], kSamples * 4u) ||
                overlaps(in[pass], kSamples * 2u, acc[pass], kSamples * 4u))
                return false;
            for (uint32_t earlier = 0; !commit && earlier < pass; ++earlier)
                if (overlaps(in[pass], kSamples * 2u, out[earlier], kSamples * 2u) ||
                    overlaps(in[pass], kSamples * 2u, acc[earlier], kSamples * 4u) ||
                    overlaps(acc[pass], kSamples * 4u, out[earlier], kSamples * 2u) ||
                    overlaps(acc[pass], kSamples * 4u, acc[earlier], kSamples * 4u))
                    return false;
        }
        mix.stackAddr = sp - 0x18u;
        if (!inRam(call, mix.stackAddr, sizeof(mix.stack)))
            return false;

        // the prologue's stack frame: the two tables, then $s0 at +0x10
        std::memcpy(mix.stack, tables, 16);
        const uint32_t s0 = call.reg(S0);
        std::memcpy(mix.stack + 16, &s0, 4);
        if (commit)
            call.writeRam(mix.stackAddr, mix.stack, sizeof(mix.stack));

        uint32_t instructions = 26u + 3u; // prologue, epilogue
        uint32_t lastSum = 0u;
        for (uint32_t pass = 0; pass < 4u; ++pass)
        {
            if (pass % 2u == 0u)
                instructions += 1u + 4u;          // the outer pass's head and its end test
            instructions += 17u + 4u + 64u * 8u;  // the pass's setup and end test; each group of 4 samples' loop tail
            int16_t samples[kSamples];
            int32_t accumulators[kSamples];
            (void)call.readRam(in[pass], samples, sizeof(samples)); // in RAM: checked above
            (void)call.readRam(acc[pass], accumulators, sizeof(accumulators));
            for (uint32_t k = 0; k < kSamples; ++k)
            {
                const int32_t sum = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(samples[k])) +
                                                         static_cast<uint32_t>(accumulators[k] >> 7));
                uint32_t cost = (k & 3u) == 0u ? 10u : 11u;
                if (sum < -0x8000)
                {
                    mix.out[pass][k] = 0x8000u;
                    --cost;
                }
                else if (sum > 0x7FFF)
                {
                    mix.out[pass][k] = 0x7FFFu;
                    ++cost;
                }
                else
                {
                    mix.out[pass][k] = static_cast<uint16_t>(sum);
                }
                instructions += cost;
                lastSum = static_cast<uint32_t>(sum);
            }
            mix.outAddr[pass] = out[pass];
            mix.accAddr[pass] = acc[pass];
            if (commit)
            {
                static const int32_t zeros[kSamples] = {};
                call.writeRam(out[pass], mix.out[pass], sizeof(mix.out[pass]));
                call.writeRam(acc[pass], zeros, sizeof(zeros));
            }
        }
        // the registers as the routine leaves them
        mix.regs[V0] = 0u; // the last loop test
        mix.regs[V1] = lastSum;
        mix.regs[A0] = acc[3] + 0x400u;
        mix.regs[A1] = output + 0x800u;
        mix.regs[A2] = 0x200u;
        mix.regs[A3] = output + 0x800u;
        mix.regs[T0] = in[3] + 0x200u;
        mix.regs[T1] = 0x7FFFu;
        mix.regs[T2] = 2u;
        mix.regs[T3] = acc[3];
        mix.regs[T4] = 2u;
        mix.regs[T5] = 0xFFFF8000u;
        mix.regs[T6] = input;
        mix.regs[T7] = accIndexWord; // the second table copy's first word
        mix.regs[S0] = s0;           // restored from the frame
        mix.regs[T9] = kAccIndices;
        mix.regs[SP] = sp;
        mix.instructions = instructions;
        return true;
    }

    uint32_t mixerRun(ps2x::iop::NativeCall &call)
    {
        static Mix mix; // not on the stack: ~2 KB of samples
        if (!runMixer(call, mix, true))
            return 0u;
        for (uint32_t reg : kResultRegs)
            call.setReg(reg, mix.regs[reg]);
        return mix.instructions;
    }

    uint32_t mixerCheck(ps2x::iop::NativeCall &call, bool before, uint64_t instructions)
    {
        static Mix predicted;
        static bool valid = false;
        static uint32_t skipped = 0u, reports = 0u;
        if (before)
        {
            valid = runMixer(call, predicted, false);
            if (!valid && ++skipped <= 4u)
                std::fprintf(stderr, "[iop-native] verify: a mixer call not predicted (overlapping buffers)%c", 10);
            return 0u;
        }
        if (!valid)
            return 0u;
        uint32_t differences = 0u;
        for (uint32_t pass = 0; pass < 4u; ++pass)
        {
            uint16_t out[kSamples];
            int32_t acc[kSamples];
            if (!call.readRam(predicted.outAddr[pass], out, sizeof(out)) || !call.readRam(predicted.accAddr[pass], acc, sizeof(acc)))
                return 1u;
            differences += std::memcmp(out, predicted.out[pass], sizeof(out)) != 0 ? 1u : 0u;
            for (uint32_t k = 0; k < kSamples; ++k)
                differences += acc[k] != 0 ? 1u : 0u;
        }
        uint8_t stack[sizeof(predicted.stack)];
        if (call.readRam(predicted.stackAddr, stack, sizeof(stack)) && std::memcmp(stack, predicted.stack, sizeof(stack)) != 0)
            ++differences;
        for (uint32_t reg : kResultRegs)
            if (call.reg(reg) != predicted.regs[reg])
            {
                ++differences;
                if (reports++ < 8u)
                    std::fprintf(stderr, "[iop-native] verify: $%u = 0x%08x, predicted 0x%08x%c", reg, call.reg(reg), predicted.regs[reg], 10);
            }
        if (instructions != predicted.instructions)
        {
            ++differences;
            if (reports++ < 8u)
                std::fprintf(stderr, "[iop-native] verify: %llu instructions, predicted %u%c",
                             static_cast<unsigned long long>(instructions), predicted.instructions, 10);
        }
        valid = false;
        return differences;
    }
}

void installVendettaIop()
{
    const char *native = std::getenv("VENDETTA_IOP_NATIVE");
    if (native && *native == '0')
        return;
    ps2x::iop::NativeHook hook;
    hook.name = "SNDDRV's mixer";
    hook.address = vendetta_iop::kMixer;
    hook.codeBytes = vendetta_iop::kMixerBytes;
    hook.codeHash = vendetta_iop::kMixerHash;
    hook.run = vendetta_iop::mixerRun;
    hook.check = vendetta_iop::mixerCheck;
    ps2x::iop::registerNativeHook(hook);
}
