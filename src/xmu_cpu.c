/*
 * xmu_cpu.c  –  x86-64 CPU emulation core
 *
 * Implements:
 *   · Instruction fetch / decode / execute loop
 *   · Real mode, Protected mode, Long mode (64-bit)
 *   · Full GPR, segment, control register set
 *   · Interrupts, exceptions, IRET
 *   · CPUID leaf emulation
 *   · MSR read/write
 */

#include "xmu.h"
#include <time.h>

/* ── Helper macros ────────────────────────────────────────────── */
#define RIP   (vcpu->regs[REG_RIP])
#define RSP   (vcpu->regs[REG_RSP])
#define RFLAGS (vcpu->regs[REG_RFLAGS])
#define RAX   (vcpu->regs[REG_RAX])
#define RBX   (vcpu->regs[REG_RBX])
#define RCX   (vcpu->regs[REG_RCX])
#define RDX   (vcpu->regs[REG_RDX])
#define RSI   (vcpu->regs[REG_RSI])
#define RDI   (vcpu->regs[REG_RDI])
#define RBP   (vcpu->regs[REG_RBP])
#define R8    (vcpu->regs[REG_R8])
#define R9    (vcpu->regs[REG_R9])
#define R10   (vcpu->regs[REG_R10])
#define R11   (vcpu->regs[REG_R11])
#define R12   (vcpu->regs[REG_R12])
#define R13   (vcpu->regs[REG_R13])
#define R14   (vcpu->regs[REG_R14])
#define R15   (vcpu->regs[REG_R15])
#define CR0   (vcpu->regs[REG_CR0])
#define CR2   (vcpu->regs[REG_CR2])
#define CR3   (vcpu->regs[REG_CR3])
#define CR4   (vcpu->regs[REG_CR4])
#define EFER  (vcpu->regs[REG_EFER])

#define IS_REAL_MODE()   (!(CR0 & CR0_PE))
#define IS_LONG_MODE()   (EFER & EFER_LMA)
#define IS_PROT_MODE()   ((CR0 & CR0_PE) && !(EFER & EFER_LMA))
#define PAGING_ENABLED() (CR0 & CR0_PG)

/* Set/clear RFLAGS bits */
#define SET_ZF(v)  do { if(!(v)) RFLAGS|=RFLAG_ZF; else RFLAGS&=~RFLAG_ZF; } while(0)
#define SET_SF(v,s) do { if((v)>>(s-1)&1) RFLAGS|=RFLAG_SF; else RFLAGS&=~RFLAG_SF; } while(0)
#define SET_CF(v)  do { if(v) RFLAGS|=RFLAG_CF; else RFLAGS&=~RFLAG_CF; } while(0)
#define SET_OF(v)  do { if(v) RFLAGS|=RFLAG_OF; else RFLAGS&=~RFLAG_OF; } while(0)
#define GET_ZF()   (!!(RFLAGS & RFLAG_ZF))
#define GET_SF()   (!!(RFLAGS & RFLAG_SF))
#define GET_CF()   (!!(RFLAGS & RFLAG_CF))
#define GET_OF()   (!!(RFLAGS & RFLAG_OF))

/* Parity */
static inline bool parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return !(v & 1);
}

/* Sign-extend helpers */
static inline int64_t sext8 (uint8_t  v) { return (int64_t)(int8_t)v;  }
static inline int64_t sext16(uint16_t v) { return (int64_t)(int16_t)v; }
static inline int64_t sext32(uint32_t v) { return (int64_t)(int32_t)v; }

/* ── Virtual address translation ──────────────────────────────── */

/* Walk a 4-level page table (CR3 → PML4 → PDPT → PD → PT) */
static uint64_t virt_to_phys(XmuVcpu *vcpu, uint64_t va, bool write, bool *fault) {
    XmuVm *vm = vcpu->vm;
    *fault = false;

    if (!PAGING_ENABLED())
        return va;

    uint64_t pml4_idx = (va >> 39) & 0x1FF;
    uint64_t pdpt_idx = (va >> 30) & 0x1FF;
    uint64_t pd_idx   = (va >> 21) & 0x1FF;
    uint64_t pt_idx   = (va >> 12) & 0x1FF;
    uint64_t offset   = va & 0xFFF;

    uint64_t cr3 = CR3 & PTE_ADDR_MASK;

    /* PML4 */
    uint64_t pml4e = xmu_mem_read(vm, cr3 + pml4_idx * 8, 8);
    if (!(pml4e & PTE_PRESENT)) { *fault = true; return 0; }

    /* PDPT */
    uint64_t pdpt_base = pml4e & PTE_ADDR_MASK;
    uint64_t pdpte = xmu_mem_read(vm, pdpt_base + pdpt_idx * 8, 8);
    if (!(pdpte & PTE_PRESENT)) { *fault = true; return 0; }
    /* 1 GB huge page */
    if (pdpte & PTE_HUGE)
        return (pdpte & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFF);

    /* PD */
    uint64_t pd_base = pdpte & PTE_ADDR_MASK;
    uint64_t pde = xmu_mem_read(vm, pd_base + pd_idx * 8, 8);
    if (!(pde & PTE_PRESENT)) { *fault = true; return 0; }
    /* 2 MB huge page */
    if (pde & PTE_HUGE)
        return (pde & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFF);

    /* PT */
    uint64_t pt_base = pde & PTE_ADDR_MASK;
    uint64_t pte = xmu_mem_read(vm, pt_base + pt_idx * 8, 8);
    if (!(pte & PTE_PRESENT)) { *fault = true; return 0; }
    if (write && !(pte & PTE_WRITABLE)) { *fault = true; return 0; }

    return (pte & PTE_ADDR_MASK) | offset;
}

/* ── Guest memory access (with translation + fault injection) ─── */
static uint64_t vcpu_read_mem(XmuVcpu *vcpu, uint64_t va, int size) {
    bool fault = false;
    uint64_t pa = virt_to_phys(vcpu, va, false, &fault);
    if (fault) {
        vcpu->regs[REG_CR2] = va;
        xmu_vcpu_inject_exception(vcpu, EXC_PF, true,
            0x00 /* not-present read */);
        return 0;
    }
    return xmu_mem_read(vcpu->vm, pa, size);
}

static void vcpu_write_mem(XmuVcpu *vcpu, uint64_t va, uint64_t val, int size) {
    bool fault = false;
    uint64_t pa = virt_to_phys(vcpu, va, true, &fault);
    if (fault) {
        vcpu->regs[REG_CR2] = va;
        xmu_vcpu_inject_exception(vcpu, EXC_PF, true,
            0x02 /* not-present write */);
        return;
    }
    xmu_mem_write(vcpu->vm, pa, val, size);
}

/* ── Instruction fetch buffer ─────────────────────────────────── */
static void insn_refill(XmuVcpu *vcpu) {
    for (int i = 0; i < 16; i++) {
        uint8_t *p = xmu_mem_lookup(vcpu->vm, RIP + i, 1, false);
        vcpu->insn_buf[i] = p ? *p : 0xF4; /* HLT if unmapped */
    }
    vcpu->insn_fetch_addr = RIP;
    vcpu->insn_len = 0;
}

static inline uint8_t fetch8(XmuVcpu *vcpu) {
    if (RIP != vcpu->insn_fetch_addr + vcpu->insn_len)
        insn_refill(vcpu);
    uint8_t b = vcpu->insn_buf[vcpu->insn_len++];
    RIP++;
    return b;
}
static inline uint16_t fetch16(XmuVcpu *vcpu) {
    uint8_t lo = fetch8(vcpu), hi = fetch8(vcpu);
    return (uint16_t)lo | ((uint16_t)hi << 8);
}
static inline uint32_t fetch32(XmuVcpu *vcpu) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)fetch8(vcpu) << (i * 8);
    return v;
}
static inline uint64_t fetch64(XmuVcpu *vcpu) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)fetch8(vcpu) << (i * 8);
    return v;
}

/* ── ModRM / SIB decode ───────────────────────────────────────── */
typedef struct {
    int  mod, reg, rm;
    int  sib_base, sib_idx, sib_scale;
    bool has_sib;
    int64_t disp;
    int     disp_size;
} ModRM;

static uint64_t *gpr_ptr(XmuVcpu *vcpu, int r) {
    static const XmuRegId map[] = {
        REG_RAX, REG_RCX, REG_RDX, REG_RBX,
        REG_RSP, REG_RBP, REG_RSI, REG_RDI,
        REG_R8,  REG_R9,  REG_R10, REG_R11,
        REG_R12, REG_R13, REG_R14, REG_R15
    };
    return &vcpu->regs[map[r & 15]];
}

static ModRM decode_modrm(XmuVcpu *vcpu, bool rex_b, bool rex_x) {
    ModRM m = {0};
    uint8_t byte = fetch8(vcpu);
    m.mod = (byte >> 6) & 3;
    m.reg = (byte >> 3) & 7;
    m.rm  = byte & 7;
    if (rex_b) m.rm  |= 8;

    m.has_sib = (m.mod != 3 && m.rm == 4);
    if (m.has_sib) {
        uint8_t sib = fetch8(vcpu);
        m.sib_scale = (sib >> 6) & 3;
        m.sib_idx   = ((sib >> 3) & 7) | (rex_x ? 8 : 0);
        m.sib_base  = (sib & 7)        | (rex_b ? 8 : 0);
    }

    if (m.mod == 1) {
        m.disp = sext8(fetch8(vcpu));
        m.disp_size = 1;
    } else if (m.mod == 2 || (m.mod == 0 && m.rm == 5)) {
        m.disp = sext32(fetch32(vcpu));
        m.disp_size = 4;
    }
    return m;
}

/* Compute effective address from ModRM */
static uint64_t modrm_ea(XmuVcpu *vcpu, const ModRM *m) {
    if (m->mod == 3) return 0;  /* register operand */

    uint64_t ea = m->disp;
    if (m->mod == 0 && m->rm == 5)
        return RIP + ea;  /* RIP-relative */

    if (m->has_sib) {
        if (m->sib_base != 5 || m->mod != 0)
            ea += *gpr_ptr(vcpu, m->sib_base);
        if (m->sib_idx != 4)
            ea += *gpr_ptr(vcpu, m->sib_idx) << m->sib_scale;
    } else {
        ea += *gpr_ptr(vcpu, m->rm);
    }
    return ea;
}

/* Read/write operand via ModRM */
static uint64_t modrm_read(XmuVcpu *vcpu, const ModRM *m, int size) {
    if (m->mod == 3) {
        uint64_t v = *gpr_ptr(vcpu, m->rm);
        switch (size) {
            case 1: return v & 0xFF;
            case 2: return v & 0xFFFF;
            case 4: return v & 0xFFFFFFFF;
            default: return v;
        }
    }
    return vcpu_read_mem(vcpu, modrm_ea(vcpu, m), size);
}

static void modrm_write(XmuVcpu *vcpu, const ModRM *m, uint64_t val, int size) {
    if (m->mod == 3) {
        uint64_t *r = gpr_ptr(vcpu, m->rm);
        switch (size) {
            case 1: *r = (*r & ~0xFFULL)     | (val & 0xFF);       break;
            case 2: *r = (*r & ~0xFFFFULL)   | (val & 0xFFFF);     break;
            case 4: *r =                        (val & 0xFFFFFFFF); break; /* zero-extend */
            default:*r = val;                                        break;
        }
        return;
    }
    vcpu_write_mem(vcpu, modrm_ea(vcpu, m), val, size);
}

/* ── ALU helpers ──────────────────────────────────────────────── */
static uint64_t alu_add(XmuVcpu *vcpu, uint64_t a, uint64_t b, int bits) {
    uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    uint64_t r = (a + b) & mask;
    RFLAGS &= ~(RFLAG_CF|RFLAG_ZF|RFLAG_SF|RFLAG_OF|RFLAG_PF|RFLAG_AF);
    if ((a + b) > mask) RFLAGS |= RFLAG_CF;
    if (!r) RFLAGS |= RFLAG_ZF;
    if (r >> (bits - 1)) RFLAGS |= RFLAG_SF;
    if (parity8(r & 0xFF)) RFLAGS |= RFLAG_PF;
    /* Overflow: same-sign operands, different-sign result */
    uint64_t sign_bit = 1ULL << (bits - 1);
    if (!((a ^ b) & sign_bit) && ((r ^ a) & sign_bit)) RFLAGS |= RFLAG_OF;
    return r;
}

static uint64_t alu_sub(XmuVcpu *vcpu, uint64_t a, uint64_t b, int bits) {
    uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    uint64_t r = (a - b) & mask;
    RFLAGS &= ~(RFLAG_CF|RFLAG_ZF|RFLAG_SF|RFLAG_OF|RFLAG_PF|RFLAG_AF);
    if (a < b) RFLAGS |= RFLAG_CF;
    if (!r) RFLAGS |= RFLAG_ZF;
    if (r >> (bits - 1)) RFLAGS |= RFLAG_SF;
    if (parity8(r & 0xFF)) RFLAGS |= RFLAG_PF;
    uint64_t sign_bit = 1ULL << (bits - 1);
    if (((a ^ b) & sign_bit) && ((r ^ a) & sign_bit)) RFLAGS |= RFLAG_OF;
    return r;
}

static uint64_t alu_and(XmuVcpu *vcpu, uint64_t a, uint64_t b, int bits) {
    uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    uint64_t r = (a & b) & mask;
    RFLAGS &= ~(RFLAG_CF|RFLAG_OF|RFLAG_ZF|RFLAG_SF|RFLAG_PF);
    if (!r) RFLAGS |= RFLAG_ZF;
    if (r >> (bits - 1)) RFLAGS |= RFLAG_SF;
    if (parity8(r & 0xFF)) RFLAGS |= RFLAG_PF;
    return r;
}

static uint64_t alu_or(XmuVcpu *vcpu, uint64_t a, uint64_t b, int bits) {
    uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    uint64_t r = (a | b) & mask;
    RFLAGS &= ~(RFLAG_CF|RFLAG_OF|RFLAG_ZF|RFLAG_SF|RFLAG_PF);
    if (!r) RFLAGS |= RFLAG_ZF;
    if (r >> (bits - 1)) RFLAGS |= RFLAG_SF;
    if (parity8(r & 0xFF)) RFLAGS |= RFLAG_PF;
    return r;
}

static uint64_t alu_xor(XmuVcpu *vcpu, uint64_t a, uint64_t b, int bits) {
    uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    uint64_t r = (a ^ b) & mask;
    RFLAGS &= ~(RFLAG_CF|RFLAG_OF|RFLAG_ZF|RFLAG_SF|RFLAG_PF);
    if (!r) RFLAGS |= RFLAG_ZF;
    if (r >> (bits - 1)) RFLAGS |= RFLAG_SF;
    if (parity8(r & 0xFF)) RFLAGS |= RFLAG_PF;
    return r;
}

/* ── Stack helpers ────────────────────────────────────────────── */
static void stack_push(XmuVcpu *vcpu, uint64_t val, int size) {
    RSP -= size;
    vcpu_write_mem(vcpu, RSP, val, size);
}

static uint64_t stack_pop(XmuVcpu *vcpu, int size) {
    uint64_t v = vcpu_read_mem(vcpu, RSP, size);
    RSP += size;
    return v;
}

/* ── Exception/interrupt delivery ────────────────────────────── */
void xmu_vcpu_inject_exception(XmuVcpu *vcpu, int vector,
                                bool has_err, uint32_t err_code)
{
    XmuVm *vm = vcpu->vm;
    if (vm->debug)
        fprintf(vm->log, "[VCPU%d] Exception #%d err=%u at RIP=%016llx\n",
                vcpu->vcpu_id, vector, err_code, (unsigned long long)RIP);

    if (IS_LONG_MODE()) {
        /* Read IDT entry */
        uint64_t idt_entry_addr = vcpu->idtr_base + (uint64_t)vector * 16;
        uint64_t lo = xmu_mem_read(vm, idt_entry_addr,     8);
        uint64_t hi = xmu_mem_read(vm, idt_entry_addr + 8, 8);

        uint64_t handler =
            ((lo >> 0)  & 0xFFFF)       |
            ((lo >> 32) & 0xFFFF0000ULL)|
            (hi << 32);

        /* Push interrupt frame */
        stack_push(vcpu, vcpu->seg_ss.selector, 8);
        stack_push(vcpu, RSP + 8,               8); /* original RSP */
        stack_push(vcpu, RFLAGS,                8);
        stack_push(vcpu, vcpu->seg_cs.selector, 8);
        stack_push(vcpu, RIP,                   8);
        if (has_err)
            stack_push(vcpu, err_code, 8);

        RFLAGS &= ~(RFLAG_IF | RFLAG_TF | RFLAG_RF | RFLAG_NT);
        RIP = handler;
    } else {
        /* Real mode IVT */
        uint32_t ivt_entry = xmu_mem_read(vm, (uint64_t)vector * 4, 4);
        stack_push(vcpu, RFLAGS & 0xFFFF, 2);
        stack_push(vcpu, vcpu->seg_cs.selector, 2);
        stack_push(vcpu, RIP & 0xFFFF, 2);
        RFLAGS &= ~(RFLAG_IF | RFLAG_TF);
        RIP = ivt_entry & 0xFFFF;
        vcpu->seg_cs.selector = (ivt_entry >> 16) & 0xFFFF;
        vcpu->seg_cs.base     = vcpu->seg_cs.selector << 4;
    }

    vcpu->insn_fetch_addr = ~0ULL; /* invalidate fetch cache */
}

void xmu_vcpu_inject_irq(XmuVcpu *vcpu, int irq) {
    if (!(RFLAGS & RFLAG_IF)) return;  /* interrupts masked */
    vcpu->int_pending = true;
    vcpu->int_vector  = irq;
}

/* ── CPUID emulation ──────────────────────────────────────────── */
static void emulate_cpuid(XmuVcpu *vcpu) {
    uint32_t leaf = (uint32_t)RAX;
    uint32_t sub  = (uint32_t)RCX;
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

    switch (leaf) {
    case 0:   /* Max leaf + vendor */
        eax = 7;
        /* "XMUhyper" */
        ebx = 0x756d5858; /* XMUh → 'XMUh' little-endian reversed */
        ecx = 0x65707279; /* yper */
        edx = 0x00726f73; /* sor  */
        break;
    case 1:   /* Family/model/features */
        eax = 0x000906E9; /* Kaby Lake */
        ebx = (vcpu->vcpu_id << 24) | (1 << 16); /* APIC ID, logical CPUs=1 */
        ecx = (1 << 0)   /* SSE3     */
            | (1 << 9)   /* SSSE3    */
            | (1 << 23)  /* POPCNT   */
            | (1 << 30); /* RDRAND   */
        edx = (1 << 0)   /* FPU      */
            | (1 << 4)   /* TSC      */
            | (1 << 5)   /* MSR      */
            | (1 << 6)   /* PAE      */
            | (1 << 8)   /* CX8      */
            | (1 << 11)  /* SEP      */
            | (1 << 15)  /* CMOV     */
            | (1 << 23)  /* MMX      */
            | (1 << 24)  /* FXSR     */
            | (1 << 25)  /* SSE      */
            | (1 << 26)  /* SSE2     */;
        break;
    case 7:   /* Structured extended features */
        if (sub == 0) {
            eax = 0;
            ebx = (1 << 0) /* FSGSBASE */;
        }
        break;
    case 0x80000000: eax = 0x80000004; break; /* max ext leaf */
    case 0x80000001:
        edx = (1 << 11) /* SYSCALL */
            | (1 << 20) /* NX      */
            | (1 << 29) /* LM      */;
        break;
    case 0x80000002: /* brand string part 1 */
        eax = 0x204d5558; ebx = 0x70797268;
        ecx = 0x69737265; edx = 0x20726f6f;
        break;
    case 0x80000003: eax = 0x20303132; ebx=0; ecx=0; edx=0; break;
    case 0x80000004: eax=0; ebx=0; ecx=0; edx=0; break;
    default: break;
    }

    RAX = eax; RBX = ebx; RCX = ecx; RDX = edx;
}

/* ── MSR emulation ────────────────────────────────────────────── */
#define MSR_TSC        0x10
#define MSR_APICBASE   0x1B
#define MSR_EFER       0xC0000080
#define MSR_STAR       0xC0000081
#define MSR_LSTAR      0xC0000082
#define MSR_FMASK      0xC0000084
#define MSR_FSBASE     0xC0000100
#define MSR_GSBASE     0xC0000101

static uint64_t msr_read(XmuVcpu *vcpu, uint32_t msr) {
    switch (msr) {
    case MSR_TSC:     { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                        return (uint64_t)ts.tv_sec * 3000000000ULL + ts.tv_nsec * 3; }
    case MSR_EFER:    return EFER;
    case MSR_APICBASE:return 0xFEE00900; /* APIC at default addr, BSP */
    case MSR_FSBASE:  return vcpu->seg_fs.base;
    case MSR_GSBASE:  return vcpu->seg_gs.base;
    default:
        if (msr < 0x300) return vcpu->vm->msrs[msr];
        return 0;
    }
}

static void msr_write(XmuVcpu *vcpu, uint32_t msr, uint64_t val) {
    switch (msr) {
    case MSR_EFER:
        EFER = val;
        /* Activate long mode if LME set and paging enabled */
        if ((val & EFER_LME) && PAGING_ENABLED())
            EFER |= EFER_LMA;
        break;
    case MSR_FSBASE: vcpu->seg_fs.base = val; break;
    case MSR_GSBASE: vcpu->seg_gs.base = val; break;
    default:
        if (msr < 0x300) vcpu->vm->msrs[msr] = val;
        break;
    }
}

/* ── I/O port access ──────────────────────────────────────────── */
static uint32_t vcpu_in(XmuVcpu *vcpu, uint16_t port, int size) {
    XmuVm *vm = vcpu->vm;
    vcpu->exit_reason = VMEXIT_IO;
    for (XmuIoHandler *h = vm->io_handlers; h; h = h->next) {
        if (port >= h->port_lo && port <= h->port_hi && h->read)
            return h->read(port, size, h->opaque);
    }
    return 0xFFFFFFFF;
}

static void vcpu_out(XmuVcpu *vcpu, uint16_t port, uint32_t val, int size) {
    XmuVm *vm = vcpu->vm;
    vcpu->exit_reason = VMEXIT_IO;
    for (XmuIoHandler *h = vm->io_handlers; h; h = h->next) {
        if (port >= h->port_lo && port <= h->port_hi && h->write) {
            h->write(port, val, size, h->opaque);
            return;
        }
    }
}

/* ── Main instruction decode/execute ─────────────────────────── */

/* Execute one instruction; returns false on fatal error */
static bool execute_one(XmuVcpu *vcpu) {
    /* Pending interrupt check */
    if (vcpu->int_pending && (RFLAGS & RFLAG_IF)) {
        int vec = vcpu->int_vector;
        vcpu->int_pending = false;
        xmu_vcpu_inject_exception(vcpu, vec, false, 0);
        return true;
    }

    /* Prefix state */
    bool rex_w = false, rex_r = false, rex_x = false, rex_b = false;
    bool prefix_66 = false, prefix_67 = false;
    bool prefix_f2 = false, prefix_f3 = false;
    bool prefix_lock = false;
    int  opsize = IS_LONG_MODE() ? 8 : 4;

    insn_refill(vcpu);
    uint64_t insn_start = RIP;

    /* Decode prefixes */
    uint8_t op = 0;
    for (;;) {
        op = fetch8(vcpu);
        if (op >= 0x40 && op <= 0x4F && IS_LONG_MODE()) {
            /* REX prefix */
            rex_w = !!(op & 8);
            rex_r = !!(op & 4);
            rex_x = !!(op & 2);
            rex_b = !!(op & 1);
            if (rex_w) opsize = 8;
            continue;
        }
        if (op == 0x66) { prefix_66 = true; opsize = (opsize==4)?2:4; continue; }
        if (op == 0x67) { prefix_67 = true; continue; }
        if (op == 0xF0) { prefix_lock = true; continue; }
        if (op == 0xF2) { prefix_f2 = true; continue; }
        if (op == 0xF3) { prefix_f3 = true; continue; }
        break;
    }

    /* Determine operand size in 32-bit default */
    if (!IS_LONG_MODE() && !rex_w)
        opsize = prefix_66 ? 2 : 4;

    vcpu->insn_count++;

    /* ── Opcode dispatch ───────────────────────────────────────── */
    switch (op) {

    /* ── NOP ───────────────────────────────────────────────────── */
    case 0x90: /* NOP / XCHG EAX,EAX */
        break;

    /* ── MOV r8, imm8 ─────────────────────────────────────────── */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        int r = (op & 7) | (rex_b ? 8 : 0);
        uint8_t imm = fetch8(vcpu);
        uint64_t *reg = gpr_ptr(vcpu, r);
        *reg = (*reg & ~0xFFULL) | imm;
        break;
    }

    /* ── MOV r16/32/64, imm ───────────────────────────────────── */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int r = (op & 7) | (rex_b ? 8 : 0);
        uint64_t imm = (opsize==8) ? fetch64(vcpu) :
                       (opsize==4) ? fetch32(vcpu) : fetch16(vcpu);
        *gpr_ptr(vcpu, r) = imm;
        break;
    }

    /* ── MOV r/m, r  (8-bit) ──────────────────────────────────── */
    case 0x88: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t v = *gpr_ptr(vcpu, m.reg | (rex_r?8:0)) & 0xFF;
        modrm_write(vcpu, &m, v, 1);
        break;
    }
    /* ── MOV r/m, r  (16/32/64-bit) ──────────────────────────── */
    case 0x89: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t v = *gpr_ptr(vcpu, m.reg | (rex_r?8:0));
        modrm_write(vcpu, &m, v, opsize);
        break;
    }
    /* ── MOV r, r/m  (8-bit) ──────────────────────────────────── */
    case 0x8A: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint8_t v = (uint8_t)modrm_read(vcpu, &m, 1);
        uint64_t *dst = gpr_ptr(vcpu, m.reg | (rex_r?8:0));
        *dst = (*dst & ~0xFFULL) | v;
        break;
    }
    /* ── MOV r, r/m  (16/32/64-bit) ──────────────────────────── */
    case 0x8B: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t v = modrm_read(vcpu, &m, opsize);
        *gpr_ptr(vcpu, m.reg | (rex_r?8:0)) = v;
        break;
    }

    /* ── MOV r/m, imm8 ────────────────────────────────────────── */
    case 0xC6: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint8_t imm = fetch8(vcpu);
        modrm_write(vcpu, &m, imm, 1);
        break;
    }
    /* ── MOV r/m, imm16/32/64 ─────────────────────────────────── */
    case 0xC7: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t imm = (opsize==8) ? (uint64_t)(int64_t)(int32_t)fetch32(vcpu) :
                       (opsize==4) ? fetch32(vcpu) : fetch16(vcpu);
        modrm_write(vcpu, &m, imm, opsize);
        break;
    }

    /* ── PUSH r64 ─────────────────────────────────────────────── */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        stack_push(vcpu, *gpr_ptr(vcpu, (op&7)|(rex_b?8:0)), opsize);
        break;

    /* ── POP r64 ──────────────────────────────────────────────── */
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        *gpr_ptr(vcpu, (op&7)|(rex_b?8:0)) = stack_pop(vcpu, opsize);
        break;

    /* ── PUSH imm8 ────────────────────────────────────────────── */
    case 0x6A:
        stack_push(vcpu, (uint64_t)(int64_t)sext8(fetch8(vcpu)), opsize);
        break;

    /* ── PUSH imm32 ───────────────────────────────────────────── */
    case 0x68:
        stack_push(vcpu, (uint64_t)(int64_t)(int32_t)fetch32(vcpu), opsize);
        break;

    /* ── ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m, imm8 (sign ext) ─── */
    case 0x83: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        int64_t imm = sext8(fetch8(vcpu));
        uint64_t a = modrm_read(vcpu, &m, opsize);
        uint64_t r;
        switch (m.reg) {
        case 0: r = alu_add(vcpu, a, (uint64_t)imm, opsize*8); modrm_write(vcpu,&m,r,opsize); break;
        case 1: r = alu_or (vcpu, a, (uint64_t)imm, opsize*8); modrm_write(vcpu,&m,r,opsize); break;
        case 4: r = alu_and(vcpu, a, (uint64_t)imm, opsize*8); modrm_write(vcpu,&m,r,opsize); break;
        case 5: r = alu_sub(vcpu, a, (uint64_t)imm, opsize*8); modrm_write(vcpu,&m,r,opsize); break;
        case 6: r = alu_xor(vcpu, a, (uint64_t)imm, opsize*8); modrm_write(vcpu,&m,r,opsize); break;
        case 7: alu_sub(vcpu, a, (uint64_t)imm, opsize*8); break; /* CMP */
        default: xmu_vcpu_inject_exception(vcpu, EXC_UD, false, 0); break;
        }
        break;
    }

    /* ── ADD r/m8, r8 ─────────────────────────────────────────── */
    case 0x00: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t a = modrm_read(vcpu,&m,1);
        uint64_t b = *gpr_ptr(vcpu, m.reg|(rex_r?8:0)) & 0xFF;
        modrm_write(vcpu,&m, alu_add(vcpu,a,b,8), 1);
        break;
    }
    /* ── ADD r/m, r ───────────────────────────────────────────── */
    case 0x01: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t a = modrm_read(vcpu,&m,opsize);
        uint64_t b = *gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        modrm_write(vcpu,&m, alu_add(vcpu,a,b,opsize*8), opsize);
        break;
    }
    /* ── ADD r, r/m ───────────────────────────────────────────── */
    case 0x03: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t *dst = gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        uint64_t b = modrm_read(vcpu,&m,opsize);
        *dst = alu_add(vcpu, *dst, b, opsize*8);
        break;
    }

    /* ── SUB r/m, r ───────────────────────────────────────────── */
    case 0x29: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t a = modrm_read(vcpu,&m,opsize);
        uint64_t b = *gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        modrm_write(vcpu,&m, alu_sub(vcpu,a,b,opsize*8), opsize);
        break;
    }
    /* ── SUB r, r/m ───────────────────────────────────────────── */
    case 0x2B: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t *dst = gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        uint64_t b = modrm_read(vcpu,&m,opsize);
        *dst = alu_sub(vcpu, *dst, b, opsize*8);
        break;
    }

    /* ── AND r, r/m ───────────────────────────────────────────── */
    case 0x23: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t *dst = gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        uint64_t b = modrm_read(vcpu,&m,opsize);
        *dst = alu_and(vcpu, *dst, b, opsize*8);
        break;
    }
    /* ── OR r, r/m ────────────────────────────────────────────── */
    case 0x0B: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t *dst = gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        uint64_t b = modrm_read(vcpu,&m,opsize);
        *dst = alu_or(vcpu, *dst, b, opsize*8);
        break;
    }
    /* ── XOR r, r/m ───────────────────────────────────────────── */
    case 0x33: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t *dst = gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        uint64_t b = modrm_read(vcpu,&m,opsize);
        *dst = alu_xor(vcpu, *dst, b, opsize*8);
        break;
    }
    /* ── XOR r/m, r ───────────────────────────────────────────── */
    case 0x31: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t a = modrm_read(vcpu,&m,opsize);
        uint64_t b = *gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        modrm_write(vcpu,&m, alu_xor(vcpu,a,b,opsize*8), opsize);
        break;
    }

    /* ── CMP r/m, r ───────────────────────────────────────────── */
    case 0x39: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t a = modrm_read(vcpu,&m,opsize);
        uint64_t b = *gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        alu_sub(vcpu, a, b, opsize*8);
        break;
    }
    /* ── CMP r, r/m ───────────────────────────────────────────── */
    case 0x3B: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t a = *gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        uint64_t b = modrm_read(vcpu,&m,opsize);
        alu_sub(vcpu, a, b, opsize*8);
        break;
    }
    /* ── TEST r/m, r ──────────────────────────────────────────── */
    case 0x85: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t a = modrm_read(vcpu,&m,opsize);
        uint64_t b = *gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        alu_and(vcpu, a, b, opsize*8);
        break;
    }

    /* ── INC/DEC r32/64 ───────────────────────────────────────── */
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        if (!IS_LONG_MODE()) {
            int r = op & 7;
            uint64_t *rp = gpr_ptr(vcpu, r);
            *rp = alu_add(vcpu, *rp, 1, opsize*8);
        }
        /* In 64-bit mode these are REX prefixes, already handled above */
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        if (!IS_LONG_MODE()) {
            int r = op & 7;
            uint64_t *rp = gpr_ptr(vcpu, r);
            *rp = alu_sub(vcpu, *rp, 1, opsize*8);
        }
        break;

    /* ── LEA r, m ─────────────────────────────────────────────── */
    case 0x8D: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        *gpr_ptr(vcpu, m.reg|(rex_r?8:0)) = modrm_ea(vcpu, &m);
        break;
    }

    /* ── XCHG r, r/m ──────────────────────────────────────────── */
    case 0x87: {
        ModRM m = decode_modrm(vcpu, rex_b, rex_x);
        uint64_t *r = gpr_ptr(vcpu, m.reg|(rex_r?8:0));
        uint64_t a = *r;
        uint64_t b = modrm_read(vcpu, &m, opsize);
        *r = b;
        modrm_write(vcpu, &m, a, opsize);
        break;
    }

    /* ── SHORT jumps ──────────────────────────────────────────── */
    case 0xEB: { int8_t rel = (int8_t)fetch8(vcpu); RIP += rel; break; }
    case 0x70: { int8_t r=(int8_t)fetch8(vcpu); if( GET_OF()) RIP+=r; break; } /* JO  */
    case 0x71: { int8_t r=(int8_t)fetch8(vcpu); if(!GET_OF()) RIP+=r; break; } /* JNO */
    case 0x72: { int8_t r=(int8_t)fetch8(vcpu); if( GET_CF()) RIP+=r; break; } /* JC  */
    case 0x73: { int8_t r=(int8_t)fetch8(vcpu); if(!GET_CF()) RIP+=r; break; } /* JNC */
    case 0x74: { int8_t r=(int8_t)fetch8(vcpu); if( GET_ZF()) RIP+=r; break; } /* JZ  */
    case 0x75: { int8_t r=(int8_t)fetch8(vcpu); if(!GET_ZF()) RIP+=r; break; } /* JNZ */
    case 0x78: { int8_t r=(int8_t)fetch8(vcpu); if( GET_SF()) RIP+=r; break; } /* JS  */
    case 0x79: { int8_t r=(int8_t)fetch8(vcpu); if(!GET_SF()) RIP+=r; break; } /* JNS */
    case 0x7C: { int8_t r=(int8_t)fetch8(vcpu);
                 if(GET_SF()!=GET_OF()) { RIP+=r; } break; }  /* JL  */
    case 0x7D: { int8_t r=(int8_t)fetch8(vcpu);
                 if(GET_SF()==GET_OF()) { RIP+=r; } break; }  /* JGE */
    case 0x7E: { int8_t r=(int8_t)fetch8(vcpu);
                 if(GET_ZF()||(GET_SF()!=GET_OF())) { RIP+=r; } break; } /* JLE */
    case 0x7F: { int8_t r=(int8_t)fetch8(vcpu);
                 if(!GET_ZF()&&(GET_SF()==GET_OF())) { RIP+=r; } break; }/* JG  */

    /* ── NEAR jump ────────────────────────────────────────────── */
    case 0xE9: { int32_t rel=(int32_t)fetch32(vcpu); RIP+=rel; break; }

    /* ── CALL near ────────────────────────────────────────────── */
    case 0xE8: {
        int32_t rel = (int32_t)fetch32(vcpu);
        stack_push(vcpu, RIP, opsize);
        RIP += rel;
        break;
    }

    /* ── RET near ─────────────────────────────────────────────── */
    case 0xC3: RIP = stack_pop(vcpu, opsize); break;
    case 0xC2: { uint16_t n=fetch16(vcpu); RIP=stack_pop(vcpu,opsize); RSP+=n; break; }

    /* ── HLT ──────────────────────────────────────────────────── */
    case 0xF4:
        vcpu->halted = true;
        vcpu->exit_reason = VMEXIT_HLT;
        break;

    /* ── CLI / STI ────────────────────────────────────────────── */
    case 0xFA: RFLAGS &= ~RFLAG_IF; break;
    case 0xFB: RFLAGS |=  RFLAG_IF; break;

    /* ── CLD / STD ────────────────────────────────────────────── */
    case 0xFC: RFLAGS &= ~RFLAG_DF; break;
    case 0xFD: RFLAGS |=  RFLAG_DF; break;

    /* ── IN / OUT ─────────────────────────────────────────────── */
    case 0xE4: { uint8_t  port=fetch8(vcpu); RAX=(RAX&~0xFF)|vcpu_in(vcpu,port,1); break;}
    case 0xE5: { uint8_t  port=fetch8(vcpu); RAX=(RAX&~0xFFFFULL)|vcpu_in(vcpu,port,2); break;}
    case 0xEC: { uint16_t port=(uint16_t)RDX; RAX=(RAX&~0xFF)|vcpu_in(vcpu,port,1); break;}
    case 0xED: { uint16_t port=(uint16_t)RDX; RAX=(RAX&~0xFFFFULL)|vcpu_in(vcpu,port,2); break;}
    case 0xE6: { uint8_t  port=fetch8(vcpu); vcpu_out(vcpu,port,(uint32_t)RAX,1); break;}
    case 0xE7: { uint8_t  port=fetch8(vcpu); vcpu_out(vcpu,port,(uint32_t)RAX,2); break;}
    case 0xEE: { uint16_t port=(uint16_t)RDX; vcpu_out(vcpu,port,(uint32_t)RAX,1); break;}
    case 0xEF: { uint16_t port=(uint16_t)RDX; vcpu_out(vcpu,port,(uint32_t)RAX,2); break;}

    /* ── INT n ────────────────────────────────────────────────── */
    case 0xCD: {
        uint8_t vec = fetch8(vcpu);
        xmu_vcpu_inject_exception(vcpu, vec, false, 0);
        break;
    }

    /* ── IRET ─────────────────────────────────────────────────── */
    case 0xCF:
        RIP    = stack_pop(vcpu, opsize);
        vcpu->seg_cs.selector = (uint16_t)stack_pop(vcpu, opsize);
        RFLAGS = stack_pop(vcpu, opsize);
        if (IS_LONG_MODE()) {
            RSP = stack_pop(vcpu, 8);
            vcpu->seg_ss.selector = (uint16_t)stack_pop(vcpu, 8);
        }
        break;

    /* ── 2-byte opcodes (0F xx) ───────────────────────────────── */
    case 0x0F: {
        uint8_t op2 = fetch8(vcpu);
        switch (op2) {
        /* ── CPUID ──────────────────────────────────────────── */
        case 0xA2:
            emulate_cpuid(vcpu);
            vcpu->exit_reason = VMEXIT_CPUID;
            break;
        /* ── RDMSR ──────────────────────────────────────────── */
        case 0x32: {
            uint32_t msr = (uint32_t)RCX;
            uint64_t v = msr_read(vcpu, msr);
            RAX = v & 0xFFFFFFFF;
            RDX = v >> 32;
            vcpu->exit_reason = VMEXIT_MSR_READ;
            break;
        }
        /* ── WRMSR ──────────────────────────────────────────── */
        case 0x30: {
            uint32_t msr = (uint32_t)RCX;
            uint64_t v = ((uint64_t)(uint32_t)RDX << 32) | (uint32_t)RAX;
            msr_write(vcpu, msr, v);
            vcpu->exit_reason = VMEXIT_MSR_WRITE;
            break;
        }
        /* ── RDTSC ──────────────────────────────────────────── */
        case 0x31: {
            uint64_t tsc = msr_read(vcpu, MSR_TSC);
            RAX = tsc & 0xFFFFFFFF;
            RDX = tsc >> 32;
            break;
        }
        /* ── MOV CR → r64 ───────────────────────────────────── */
        case 0x20: {
            ModRM m = decode_modrm(vcpu, rex_b, rex_x);
            static const XmuRegId crmap[] = {REG_CR0,REG_RIP,REG_CR2,REG_CR3,
                                              REG_CR4,REG_RIP,REG_RIP,REG_RIP};
            *gpr_ptr(vcpu, m.rm|(rex_b?8:0)) = vcpu->regs[crmap[m.reg&7]];
            break;
        }
        /* ── MOV r64 → CR ───────────────────────────────────── */
        case 0x22: {
            ModRM m = decode_modrm(vcpu, rex_b, rex_x);
            static const XmuRegId crmap[] = {REG_CR0,REG_RIP,REG_CR2,REG_CR3,
                                              REG_CR4,REG_RIP,REG_RIP,REG_RIP};
            vcpu->regs[crmap[m.reg&7]] = *gpr_ptr(vcpu, m.rm|(rex_b?8:0));
            /* Enable long mode when EFER.LME + PG */
            if ((m.reg&7)==0 && (CR0&CR0_PG) && (EFER&EFER_LME))
                EFER |= EFER_LMA;
            break;
        }
        /* ── LGDT / LIDT (0F 01 /2, /3) ────────────────────── */
        case 0x01: {
            ModRM m = decode_modrm(vcpu, rex_b, rex_x);
            uint64_t ea = modrm_ea(vcpu, &m);
            uint16_t lim = (uint16_t)vcpu_read_mem(vcpu, ea,   2);
            uint64_t base= vcpu_read_mem(vcpu, ea+2, 8);
            if (m.reg == 2) {
                vcpu->gdtr_base  = base; vcpu->gdtr_limit = lim;
            } else if (m.reg == 3) {
                vcpu->idtr_base  = base; vcpu->idtr_limit = lim;
            }
            break;
        }
        /* ── SYSCALL ────────────────────────────────────────── */
        case 0x05:
            R11   = RFLAGS;
            RCX   = RIP;
            RFLAGS &= ~(RFLAG_IF|RFLAG_TF|RFLAG_DF);
            RIP   = msr_read(vcpu, MSR_LSTAR);
            break;
        /* ── SYSRET ─────────────────────────────────────────── */
        case 0x07:
            RIP    = RCX;
            RFLAGS = R11;
            break;
        /* ── Near conditional jumps (0F 8x) ─────────────────── */
        case 0x84: { int32_t r=(int32_t)fetch32(vcpu); if( GET_ZF()) RIP+=r; break; }
        case 0x85: { int32_t r=(int32_t)fetch32(vcpu); if(!GET_ZF()) RIP+=r; break; }
        case 0x8C: { int32_t r=(int32_t)fetch32(vcpu); if(GET_SF()!=GET_OF()) { RIP+=r; } break; }
        case 0x8D: { int32_t r=(int32_t)fetch32(vcpu); if(GET_SF()==GET_OF()) { RIP+=r; } break; }
        case 0x8E: { int32_t r=(int32_t)fetch32(vcpu); if(GET_ZF()||(GET_SF()!=GET_OF())) { RIP+=r; } break; }
        case 0x8F: { int32_t r=(int32_t)fetch32(vcpu); if(!GET_ZF()&&(GET_SF()==GET_OF())) { RIP+=r; } break; }
        /* ── MOVZX r, r/m8 ──────────────────────────────────── */
        case 0xB6: {
            ModRM m = decode_modrm(vcpu, rex_b, rex_x);
            *gpr_ptr(vcpu, m.reg|(rex_r?8:0)) = modrm_read(vcpu, &m, 1);
            break;
        }
        /* ── MOVZX r, r/m16 ─────────────────────────────────── */
        case 0xB7: {
            ModRM m = decode_modrm(vcpu, rex_b, rex_x);
            *gpr_ptr(vcpu, m.reg|(rex_r?8:0)) = modrm_read(vcpu, &m, 2);
            break;
        }
        /* ── MOVSX r, r/m8 ──────────────────────────────────── */
        case 0xBE: {
            ModRM m = decode_modrm(vcpu, rex_b, rex_x);
            *gpr_ptr(vcpu, m.reg|(rex_r?8:0)) =
                (uint64_t)(int64_t)(int8_t)modrm_read(vcpu, &m, 1);
            break;
        }
        /* ── BSF r, r/m ─────────────────────────────────────── */
        case 0xBC: {
            ModRM m = decode_modrm(vcpu, rex_b, rex_x);
            uint64_t v = modrm_read(vcpu, &m, opsize);
            uint64_t *dst = gpr_ptr(vcpu, m.reg|(rex_r?8:0));
            if (!v) { RFLAGS |= RFLAG_ZF; }
            else {
                RFLAGS &= ~RFLAG_ZF;
                int i=0; while(!((v>>i)&1)) i++;
                *dst = i;
            }
            break;
        }
        /* ── PAUSE (F3 90) / other ──────────────────────────── */
        case 0x90: vcpu->exit_reason = VMEXIT_PAUSE; break;
        default:
            if (vcpu->vm->debug)
                fprintf(vcpu->vm->log, "[VCPU%d] Unknown 2-byte opcode 0F %02X at %016llx\n",
                        vcpu->vcpu_id, op2,
                        (unsigned long long)(insn_start));
            xmu_vcpu_inject_exception(vcpu, EXC_UD, false, 0);
            break;
        }
        break;
    } /* end 0F */

    /* ── Unhandled: inject #UD ────────────────────────────────── */
    default:
        if (vcpu->vm->debug)
            fprintf(vcpu->vm->log,
                    "[VCPU%d] Unhandled opcode %02X at RIP=%016llx\n",
                    vcpu->vcpu_id, op,
                    (unsigned long long)insn_start);
        xmu_vcpu_inject_exception(vcpu, EXC_UD, false, 0);
        break;
    } /* end switch(op) */

    return true;
}

/* ── VCPU public API ──────────────────────────────────────────── */

XmuVcpu *xmu_vcpu_create(XmuVm *vm, int id) {
    XmuVcpu *v = calloc(1, sizeof(XmuVcpu));
    if (!v) return NULL;
    v->vm      = vm;
    v->vcpu_id = id;
    xmu_vcpu_reset(v);
    return v;
}

void xmu_vcpu_destroy(XmuVcpu *v) { free(v); }

void xmu_vcpu_reset(XmuVcpu *vcpu) {
    memset(vcpu->regs, 0, sizeof(vcpu->regs));
    /* Real-mode reset vector: CS=0xF000, RIP=0xFFF0 */
    RIP = 0xFFF0;
    vcpu->seg_cs.selector = 0xF000;
    vcpu->seg_cs.base     = 0xFFFF0000;
    vcpu->seg_cs.limit    = 0xFFFF;
    vcpu->seg_ds.limit    = 0xFFFF;
    vcpu->seg_es.limit    = 0xFFFF;
    vcpu->seg_ss.limit    = 0xFFFF;
    vcpu->seg_fs.limit    = 0xFFFF;
    vcpu->seg_gs.limit    = 0xFFFF;
    RFLAGS = 0x0002;  /* reserved bit always 1 */
    CR0    = 0x60000010;
    vcpu->gdtr_limit = 0xFFFF;
    vcpu->idtr_limit = 0xFFFF;
    vcpu->halted     = false;
    vcpu->exit_reason = VMEXIT_NONE;
    vcpu->insn_fetch_addr = ~0ULL;
}

int xmu_vcpu_run(XmuVcpu *vcpu, uint64_t max_insns) {
    uint64_t n = 0;
    vcpu->exit_reason = VMEXIT_NONE;

    while (!vcpu->halted && vcpu->exit_reason == VMEXIT_NONE) {
        if (max_insns && n >= max_insns) break;
        if (!execute_one(vcpu)) return -1;
        n++;
    }
    vcpu->vm->total_insns += n;
    return 0;
}

/* ── Utility ──────────────────────────────────────────────────── */
static const char *reg_names[] = {
    "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
    "r8","r9","r10","r11","r12","r13","r14","r15",
    "rip","rflags","cs","ds","es","fs","gs","ss",
    "cr0","cr2","cr3","cr4","efer"
};
const char *xmu_reg_name(XmuRegId r) {
    if (r < REG_COUNT) return reg_names[r];
    return "??";
}

void xmu_vcpu_dump(const XmuVcpu *v, FILE *out) {
    fprintf(out,
        "──── VCPU %d ────────────────────────────────\n"
        " RIP=%016llx  RFLAGS=%016llx\n"
        " RAX=%016llx  RBX=%016llx\n"
        " RCX=%016llx  RDX=%016llx\n"
        " RSI=%016llx  RDI=%016llx\n"
        " RSP=%016llx  RBP=%016llx\n"
        " R8 =%016llx  R9 =%016llx\n"
        " R10=%016llx  R11=%016llx\n"
        " R12=%016llx  R13=%016llx\n"
        " R14=%016llx  R15=%016llx\n"
        " CR0=%016llx  CR3=%016llx  CR4=%016llx\n"
        " EFER=%016llx\n"
        " CS=%04x DS=%04x SS=%04x  (%llu insns)\n",
        v->vcpu_id,
        (unsigned long long)v->regs[REG_RIP],
        (unsigned long long)v->regs[REG_RFLAGS],
        (unsigned long long)v->regs[REG_RAX],
        (unsigned long long)v->regs[REG_RBX],
        (unsigned long long)v->regs[REG_RCX],
        (unsigned long long)v->regs[REG_RDX],
        (unsigned long long)v->regs[REG_RSI],
        (unsigned long long)v->regs[REG_RDI],
        (unsigned long long)v->regs[REG_RSP],
        (unsigned long long)v->regs[REG_RBP],
        (unsigned long long)v->regs[REG_R8],
        (unsigned long long)v->regs[REG_R9],
        (unsigned long long)v->regs[REG_R10],
        (unsigned long long)v->regs[REG_R11],
        (unsigned long long)v->regs[REG_R12],
        (unsigned long long)v->regs[REG_R13],
        (unsigned long long)v->regs[REG_R14],
        (unsigned long long)v->regs[REG_R15],
        (unsigned long long)v->regs[REG_CR0],
        (unsigned long long)v->regs[REG_CR3],
        (unsigned long long)v->regs[REG_CR4],
        (unsigned long long)v->regs[REG_EFER],
        v->seg_cs.selector, v->seg_ds.selector, v->seg_ss.selector,
        (unsigned long long)v->insn_count);
}