// Headless query helper for adjmk's analysed Ghidra project of Def Jam Vendetta (SLUS_206.39).
// Usage (after the program has been imported + analysed):
//   C:\Tools\ghidra-headless.bat C:\Tools\ghidra-projects adjmk -process SLUS_206.39 -noanalysis ^
//       -scriptPath <repo>\tools\ghidra -postScript AdjmkQuery.java <query> [<query> ...]
// Queries:
//   info                 program summary (language, image base, function count, memory blocks)
//   x:<addr>             every reference TO <addr>   (x:<from>-<to> = any address in the range)
//   d:<addr>             decompile the function containing <addr>
//   f:<addr>             name/bounds of the function containing <addr>
//   c:<addr>             callers and callees of the function containing <addr>
//   s:<value>            scan all instructions for a scalar operand == <value>; report fn + instr
//                        (s:<value>@<lo>-<hi> restricts the scan to that vaddr range)
// Addresses are hex virtual addresses (0x optional). Output goes to stdout between ADJMK markers.
//@category adjmk

import java.util.*;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class AdjmkQuery extends GhidraScript {

    private Address addr(String s) {
        s = s.trim();
        if (s.startsWith("0x") || s.startsWith("0X")) s = s.substring(2);
        return toAddr(Long.parseLong(s, 16));
    }

    private String fn(Address a) {
        Function f = getFunctionContaining(a);
        return f == null ? "(no function)" : f.getName() + " @ " + f.getEntryPoint();
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        println("ADJMK-BEGIN");
        for (String q : args) {
            println("ADJMK-QUERY " + q);
            if (q.equals("info")) {
                println("  name        " + currentProgram.getName());
                println("  language    " + currentProgram.getLanguageID() + " / " + currentProgram.getCompilerSpec().getCompilerSpecID());
                println("  image base  " + currentProgram.getImageBase());
                println("  functions   " + currentProgram.getFunctionManager().getFunctionCount());
                for (MemoryBlock b : currentProgram.getMemory().getBlocks())
                    println("  block       " + b.getName() + "  " + b.getStart() + "-" + b.getEnd()
                        + (b.isInitialized() ? "" : "  (uninitialised)") + (b.isExecute() ? "  x" : ""));
            }
            else if (q.startsWith("x:")) {
                String[] r = q.substring(2).split("-");
                Address lo = addr(r[0]), hi = r.length > 1 ? addr(r[1]) : lo;
                int n = 0;
                Map<String, List<String>> byFunc = new TreeMap<>();
                // walk the addresses that ARE reference destinations, then list who points at each
                AddressIterator dests = currentProgram.getReferenceManager().getReferenceDestinationIterator(lo, true);
                while (dests.hasNext()) {
                    Address to = dests.next();
                    if (to.compareTo(hi) > 0) break;
                    ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(to);
                    while (it.hasNext()) {
                        Reference ref = it.next();
                        n++;
                        byFunc.computeIfAbsent(fn(ref.getFromAddress()), k -> new ArrayList<>())
                              .add(ref.getFromAddress() + "->" + ref.getToAddress() + " " + ref.getReferenceType());
                    }
                }
                println("  " + n + " references in " + byFunc.size() + " functions");
                for (Map.Entry<String, List<String>> e : byFunc.entrySet())
                    println("  " + e.getKey() + "  :  " + String.join(", ", e.getValue()));
            }
            else if (q.startsWith("f:")) {
                Function f = getFunctionContaining(addr(q.substring(2)));
                println(f == null ? "  (no function)" : "  " + f.getName() + "  " + f.getBody().getMinAddress()
                    + "-" + f.getBody().getMaxAddress() + "  callers=" + f.getCallingFunctions(monitor).size()
                    + " callees=" + f.getCalledFunctions(monitor).size());
            }
            else if (q.startsWith("c:")) {
                Function f = getFunctionContaining(addr(q.substring(2)));
                if (f == null) { println("  (no function)"); continue; }
                println("  " + f.getName() + "  " + f.getBody().getMinAddress() + "-" + f.getBody().getMaxAddress());
                for (Function g : new TreeSet<>(Comparator.comparing(Function::getEntryPoint)) {{ addAll(f.getCallingFunctions(monitor)); }})
                    println("  called by  " + g.getName() + "  (" + g.getBody().getNumAddresses() + " bytes)");
                for (Function g : new TreeSet<>(Comparator.comparing(Function::getEntryPoint)) {{ addAll(f.getCalledFunctions(monitor)); }})
                    println("  calls      " + g.getName() + "  (" + g.getBody().getNumAddresses() + " bytes)");
            }
            else if (q.startsWith("d:") || q.startsWith("D:")) {
                // D:<addr> = create the function there first (raw binaries such as an IOP RAM dump have none)
                Function f = getFunctionContaining(addr(q.substring(2)));
                if (f == null && q.startsWith("D:")) {
                    disassemble(addr(q.substring(2)));
                    f = createFunction(addr(q.substring(2)), null);
                }
                if (f == null) { println("  (no function)"); continue; }
                DecompInterface dec = new DecompInterface();
                dec.openProgram(currentProgram);
                DecompileResults res = dec.decompileFunction(f, 120, monitor);
                println("  // " + f.getName() + " @ " + f.getEntryPoint());
                if (res != null && res.decompileCompleted())
                    for (String line : res.getDecompiledFunction().getC().split("\n")) println("  " + line);
                else
                    println("  decompile failed: " + (res == null ? "null" : res.getErrorMessage()));
                dec.dispose();
            }
            else if (q.startsWith("s:")) {
                String body = q.substring(2);
                long lo = 0, hi = -1L;
                int at = body.indexOf('@');
                if (at >= 0) {
                    String[] r = body.substring(at + 1).split("-");
                    lo = addr(r[0]).getOffset();
                    hi = (r.length > 1 ? addr(r[1]) : addr(r[0])).getOffset();
                    body = body.substring(0, at);
                }
                long val = Long.parseLong(body.trim().replaceFirst("^0[xX]", ""), 16);
                int n = 0;
                Map<String, List<String>> byFunc = new TreeMap<>();
                var instrs = currentProgram.getListing().getInstructions(true);
                while (instrs.hasNext()) {
                    ghidra.program.model.listing.Instruction ins = instrs.next();
                    long off = ins.getMinAddress().getOffset();
                    if (hi >= 0 && (off < lo || off > hi)) continue;
                    boolean hit = false;
                    for (int op = 0; op < ins.getNumOperands() && !hit; op++)
                        for (Object o : ins.getOpObjects(op))
                            if (o instanceof ghidra.program.model.scalar.Scalar
                                && ((ghidra.program.model.scalar.Scalar) o).getUnsignedValue() == val) { hit = true; break; }
                    if (hit) {
                        n++;
                        byFunc.computeIfAbsent(fn(ins.getMinAddress()), k -> new ArrayList<>())
                              .add(ins.getMinAddress() + "  " + ins.toString());
                    }
                }
                println("  " + n + " instructions in " + byFunc.size() + " functions use scalar 0x" + Long.toHexString(val));
                for (Map.Entry<String, List<String>> e : byFunc.entrySet())
                    println("  " + e.getKey() + "  :  " + String.join(" | ", e.getValue()));
            }
            else if (q.startsWith("l:")) {
                // Disassembly listing over a plain address range: l:<from>[-<to>]
                String[] r = q.substring(2).split("-");
                Address lo = addr(r[0]), hi = r.length > 1 ? addr(r[1]) : lo.add(0x200);
                InstructionIterator it = currentProgram.getListing().getInstructions(lo, true);
                int n = 0;
                while (it.hasNext() && n++ < 4000) {
                    Instruction ins = it.next();
                    if (ins.getMinAddress().compareTo(hi) > 0) break;
                    println("  " + ins.getMinAddress() + "  " + ins.toString());
                }
                if (n == 0) println("  (no disassembled instructions in range — may be undefined bytes)");
            }
            else if (q.startsWith("b:")) {
                // Hex dump of a named memory block's raw bytes (e.g. a VU .DVP.overlay): b:<blockName>
                String name = q.substring(2);
                MemoryBlock blk = null;
                for (MemoryBlock b : currentProgram.getMemory().getBlocks())
                    if (b.getName().equals(name) || b.getName().contains(name)) { blk = b; break; }
                if (blk == null) { println("  (no block matching " + name + ")"); continue; }
                int len = (int) Math.min(blk.getSize(), 0x2000);
                byte[] buf = new byte[len];
                try { currentProgram.getMemory().getBytes(blk.getStart(), buf); }
                catch (Exception e) { println("  (cannot read bytes: " + e.getMessage() + ")"); continue; }
                println("  block " + blk.getName() + "  size=" + blk.getSize());
                StringBuilder hex = new StringBuilder();
                for (int i = 0; i < len; i++) {
                    hex.append(String.format("%02x", buf[i] & 0xff));
                    if ((i & 0xf) == 0xf) { println("  " + String.format("%04x", i - 15) + "  " + hex); hex.setLength(0); }
                }
                if (hex.length() > 0) println("  " + hex);
            }
            else if (q.startsWith("vu:")) {
                // Disassembly listing of a named memory block (e.g. an overlay): vu:<blockName>
                String name = q.substring(3);
                MemoryBlock blk = null;
                for (MemoryBlock b : currentProgram.getMemory().getBlocks())
                    if (b.getName().equals(name) || b.getName().contains(name)) { blk = b; break; }
                if (blk == null) { println("  (no block matching " + name + ")"); continue; }
                println("  block " + blk.getName() + "  " + blk.getStart() + "-" + blk.getEnd());
                InstructionIterator it = currentProgram.getListing().getInstructions(blk.getStart(), true);
                int n = 0;
                while (it.hasNext() && n++ < 4000) {
                    Instruction ins = it.next();
                    if (ins.getMinAddress().compareTo(blk.getEnd()) > 0) break;
                    println("  " + ins.getMinAddress() + "  " + ins.toString());
                }
                if (n == 0) println("  (no disassembled instructions — bytes may be undefined; try running Disassemble in the GUI)");
            }
            else println("  unknown query");
        }
        println("ADJMK-END");
    }
}
