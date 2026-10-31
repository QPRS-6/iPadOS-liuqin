#!/usr/bin/env python3
"""Decode an Adreno a7xx PM4 stream (type 4 / type 7) from a list of dwords."""
import sys

OP = {
 72:"CP_ME_INIT", 16:"CP_NOP", 28:"CP_PREEMPT_ENABLE", 30:"CP_PREEMPT_TOKEN",
 63:"CP_INDIRECT_BUFFER", 87:"CP_INDIRECT_BUFFER_CHAIN", 55:"CP_INDIRECT_BUFFER_PFD",
 38:"CP_WAIT_FOR_IDLE", 60:"CP_WAIT_REG_MEM", 82:"CP_WAIT_REG_EQ", 83:"CP_WAIT_REG_GTE",
 92:"CP_WAIT_UNTIL_READ", 93:"CP_WAIT_IB_PFD_COMPLETE", 33:"CP_REG_RMW", 47:"CP_SET_BIN_DATA5",
 62:"CP_REG_TO_MEM", 61:"CP_MEM_WRITE", 79:"CP_MEM_WRITE_CNTR", 68:"CP_COND_EXEC",
 69:"CP_COND_WRITE5", 70:"CP_EVENT_WRITE7", 88:"CP_EVENT_WRITE_SHD", 89:"CP_EVENT_WRITE_CFL",
 91:"CP_EVENT_WRITE_ZPD", 49:"CP_RUN_OPENCL", 34:"CP_DRAW_INDX", 54:"CP_DRAW_INDX_2",
 52:"CP_DRAW_INDX_BIN", 53:"CP_DRAW_INDX_2_BIN", 35:"CP_VIZ_QUERY", 37:"CP_SET_STATE",
 45:"CP_SET_CONSTANT", 39:"CP_IM_LOAD", 43:"CP_IM_LOAD_IMMEDIATE", 59:"CP_INVALIDATE_STATE",
 74:"CP_SET_SHADER_BASES/REG_TO_SCRATCH", 80:"CP_SET_BIN_MASK/START_BIN", 81:"CP_SET_BIN_SELECT/END_BIN",
 94:"CP_CONTEXT_UPDATE", 64:"CP_INTERRUPT", 95:"CP_SET_PROTECTED_MODE", 111:"CP_BOOTSTRAP_UCODE",
 48:"CP_LOAD_STATE4", 58:"CP_COND_INDIRECT_BUFFER_PFE", 50:"CP_COND_INDIRECT_BUFFER_PFD",
 76:"CP_SET_BIN/SCRATCH_WRITE", 113:"CP_TEST_TWO_MEMS", 120:"CP_REG_WR_NO_CTXT",
 17:"CP_RECORD_PFP_TIMESTAMP", 102:"CP_SET_SECURE_MODE", 19:"CP_WAIT_FOR_ME",
 67:"CP_SET_DRAW_STATE", 56:"CP_DRAW_INDX_OFFSET", 40:"CP_DRAW_INDIRECT", 41:"CP_DRAW_INDX_INDIRECT",
 42:"CP_DRAW_INDIRECT_MULTI", 36:"CP_DRAW_AUTO", 25:"CP_DRAW_PRED_ENABLE_GLOBAL",
 26:"CP_DRAW_PRED_ENABLE_LOCAL", 78:"CP_DRAW_PRED_SET", 116:"CP_WIDE_REG_WRITE/REG_TO_MEM_OFFSET_MEM",
 77:"CP_SCRATCH_TO_REG", 18:"CP_WAIT_MEM_WRITES", 71:"CP_COND_REG_EXEC", 66:"CP_MEM_TO_REG",
 65:"CP_EXEC_CS_INDIRECT", 51:"CP_EXEC_CS", 101:"CP_SET_MARKER", 86:"CP_SET_PSEUDO_REG",
 29:"CP_SKIP_IB2_ENABLE_GLOBAL", 35:"CP_SKIP_IB2_ENABLE_LOCAL",
 53:"CP_SET_SUBDRAW_SIZE", 98:"CP_WHERE_AM_I", 100:"CP_SET_VISIBILITY_OVERRIDE",
 105:"CP_PREEMPT_ENABLE_GLOBAL", 106:"CP_PREEMPT_ENABLE_LOCAL", 107:"CP_CONTEXT_SWITCH_YIELD",
 108:"CP_SET_RENDER_MODE/SCOPE_CNTL", 110:"CP_COMPUTE_CHECKPOINT", 115:"CP_MEM_TO_MEM",
 44:"CP_BLIT", 57:"CP_REG_TEST", 99:"CP_SET_MODE", 50:"CP_LOAD_STATE6_GEOM",
 52:"CP_LOAD_STATE6_FRAG", 54:"CP_LOAD_STATE6", 23:"CP_THREAD_CONTROL/IN_IB_PREFETCH_END",
 31:"IN_SUBBLK_PREFETCH/CP_RESET_CONTEXT_STATE", 32:"IN_INSTR_PREFETCH", 73:"IN_CONST_PREFETCH/MEM_TO_SCRATCH",
 85:"IN_INCR_UPDT_STATE/CP_SET_AMBLE", 84:"CP_CONTEXT_SWITCH", 109:"CP_REG_WRITE",
 20:"CP_WAIT_TIMESTAMP", 21:"CP_GLOBAL_TIMESTAMP", 22:"CP_LOCAL_TIMESTAMP",
 24:"CP_RESOURCE_LIST", 27:"CP_BV_BR_COUNT_OPS", 39:"CP_SKIP_IB_MODE", 88:"CP_MEMORY_MAP_UPDATE",
 89:"CP_BARRIER", 127:"CP_FIXED_STRIDE_DRAW_TABLE", 114:"CP_REG_TO_MEM_OFFSET_REG",
 117:"CP_MEMCPY", 46:"CP_SET_BIN_DATA5_OFFSET", 112:"CP_WAIT_TWO_REGS", 93:"CP_NON_CONTEXT_REG_BUNCH",
 0x3f:"CP_INDIRECT_BUFFER",
}

def parity(v):
    return (0x9669 >> (0xf & (v ^ (v>>4) ^ (v>>8) ^ (v>>12) ^ (v>>16) ^ (v>>20) ^ (v>>24) ^ (v>>28)))) & 1

MARKER = {1:"RM6_DIRECT_RENDER",2:"RM6_BIN_VISIBILITY",3:"RM6_BIN_DIRECT",4:"RM6_BIN_RENDER_START",
          5:"RM6_BIN_END_OF_DRAWS",6:"RM6_BIN_RESOLVE",7:"RM6_BIN_RENDER_END",8:"RM6_COMPUTE",
          12:"RM6_BLIT2DSCALE",13:"RM6_IB1LIST_START",14:"RM6_IB1LIST_END",15:"RM7_BIN_VISIBILITY_END",
          32:"RM8_DEPTH_PASS_START",33:"RM8_DEPTH_PASS_END"}

EVENT = {0x01:"DUMMY",0x04:"CACHE_FLUSH_TS",0x05:"CONTEXT_DONE",0x07:"HLSQ_FLUSH",0x09:"WRITE_PRIMITIVE_COUNTS",
         0x0b:"START_PRIMITIVE_CTRS",0x0c:"STOP_PRIMITIVE_CTRS",0x0d:"START_FRAGMENT_CTRS",0x0e:"STOP_FRAGMENT_CTRS",
         0x0f:"START_COMPUTE_CTRS",0x10:"STOP_COMPUTE_CTRS",0x11:"FLUSH_SO_0",0x12:"FLUSH_SO_1",0x13:"FLUSH_SO_2",
         0x14:"FLUSH_SO_3",0x15:"ZPASS_DONE",0x16:"RB_DONE_TS",0x18:"CCU_INVALIDATE_DEPTH",
         0x19:"CCU_INVALIDATE_COLOR",0x1a:"CCU_RESOLVE_CLEAN",0x1c:"CCU_FLUSH_DEPTH",0x1d:"CCU_FLUSH_COLOR",
         0x1e:"CCU_RESOLVE",0x1f:"CCU_END_RESOLVE_GROUP",0x20:"CCU_CLEAN_DEPTH",0x21:"CCU_CLEAN_COLOR",
         0x24:"LRZ_FLIP_BUFFER",0x25:"LRZ_CLEAR",0x26:"LRZ_CACHE_FLUSH",0x27:"BLIT_OP_FILL_2D",
         0x28:"LRZ_CACHE_INVALIDATE",0x29:"LRZ_Q_CACHE_INVALIDATE",0x2a:"BLIT_OP_SCALE_2D",0x2b:"CONTEXT_DONE_2D",
         0x2c:"VSC_BINNING_START",0x2d:"VSC_BINNING_END",0x30:"CACHE_RESET",0x31:"CACHE_CLEAN",
         0x32:"CACHE_FLUSH7",0x33:"CACHE_INVALIDATE7",0x35:"SUBPASS_FENCE",0x3f:"DEBUG_LABEL"}

def decode(words, show_payload=True):
    i = 0
    out = []
    n = len(words)
    while i < n:
        w = words[i]
        t = (w >> 28) & 0xf
        if t == 7 and (w & 0x0F000000) == 0:
            op = (w >> 16) & 0x7f
            size = w & 0x3fff
            name = OP.get(op, "unknown(0x%02x)" % op)
            pay = words[i+1:i+1+size]
            extra = ""
            if op == 0x46 and pay:
                ev = pay[0] & 0x3f
                extra = " event=%s(0x%x) raw=0x%08x" % (EVENT.get(ev, "?"), ev, pay[0])
                if len(pay) > 1:
                    extra += " addr=0x%08x%08x" % (pay[2] if len(pay) > 2 else 0, pay[1])
            elif op == 0x65 and pay:
                mode = (pay[0] >> 8) & 1
                extra = " marker=%s raw=0x%08x" % (MARKER.get(pay[0] & 0x1f, "?") if mode == 0 else "IFPC", pay[0])
            elif op == 0x47 and pay:
                m = (pay[0] >> 28) & 0xf
                extra = " condreg mode=%d dw=%d raw=0x%08x,0x%08x" % (m, (pay[-1] & 0xffffff) if len(pay)>1 else 0, pay[0], pay[1] if len(pay)>1 else 0)
            elif op == 0x86 and pay:
                extra = " pseudo=%d addr=0x%08x%08x" % (pay[0] & 0x7ff, pay[2] if len(pay)>2 else 0, pay[1] if len(pay)>1 else 0)
            elif op == 0x56 and pay:
                extra = " subdraw=%d" % (pay[0] & 0xffff)
            elif op == 0x35 and pay:
                extra = " subdrawsize=%d" % (pay[0] & 0xffff)
            elif op == 0x63 and pay:
                extra = " set_mode=%d" % pay[0]
            out.append("  %4d: %-26s sz=%-3d %s%s" % (i, name, size, " ".join("%08x" % p for p in pay) if show_payload else "", extra))
            i += 1 + size
        elif t == 4:
            off = (w >> 8) & 0x7ffff
            size = w & 0x7f
            pay = words[i+1:i+1+size]
            out.append("  %4d: reg 0x%05x = %s" % (i, off, " ".join("%08x" % p for p in pay)))
            i += 1 + size
        else:
            out.append("  %4d: BAD HEADER 0x%08x (type %d) -- stream desync" % (i, w, t))
            i += 1
    return out

if __name__ == "__main__":
    path = sys.argv[1]
    words = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        for tok in line.split():
            words.append(int(tok, 16))
    print("total dwords: %d" % len(words))
    for line in decode(words):
        print(line)
