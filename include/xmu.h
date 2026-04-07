#ifndef XMU_H
#define XMU_H

/* Avoid conflict with sys/ucontext.h REG_* names */
#ifdef _GNU_SOURCE
#undef _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 *  XMU - X Machine Unit Hypervisor  v1.0.0
 * ═══════════════════════════════════════════════════════════════ */

#define XMU_VERSION_STR   "1.0.0"
#define XMU_PAGE_SIZE     4096
#define XMU_MAX_VMS       16
#define XMU_MAX_VCPUS     8
#define XMU_MAX_RAM       (256ULL * 1024 * 1024)

/* ── CPU register table indices ───────────────────────────────── */
/* Use XMU_ prefix to avoid conflict with ucontext REG_* macros   */
#define XMU_RAX     0
#define XMU_RCX     1
#define XMU_RDX     2
#define XMU_RBX     3
#define XMU_RSP     4
#define XMU_RBP     5
#define XMU_RSI     6
#define XMU_RDI     7
#define XMU_R8      8
#define XMU_R9      9
#define XMU_R10     10
#define XMU_R11     11
#define XMU_R12     12
#define XMU_R13     13
#define XMU_R14     14
#define XMU_R15     15
#define XMU_RIP     16
#define XMU_RFLAGS  17
#define XMU_CS      18
#define XMU_DS      19
#define XMU_ES      20
#define XMU_FS      21
#define XMU_GS      22
#define XMU_SS      23
#define XMU_CR0     24
#define XMU_CR2     25
#define XMU_CR3     26
#define XMU_CR4     27
#define XMU_EFER    28
#define XMU_NREGS   29

typedef int XmuRegId;

/* Short aliases used throughout the source */
#define REG_RAX    XMU_RAX
#define REG_RCX    XMU_RCX
#define REG_RDX    XMU_RDX
#define REG_RBX    XMU_RBX
#define REG_RSP    XMU_RSP
#define REG_RBP    XMU_RBP
#define REG_RSI    XMU_RSI
#define REG_RDI    XMU_RDI
#define REG_R8     XMU_R8
#define REG_R9     XMU_R9
#define REG_R10    XMU_R10
#define REG_R11    XMU_R11
#define REG_R12    XMU_R12
#define REG_R13    XMU_R13
#define REG_R14    XMU_R14
#define REG_R15    XMU_R15
#define REG_RIP    XMU_RIP
#define REG_RFLAGS XMU_RFLAGS
#define REG_CS     XMU_CS
#define REG_DS     XMU_DS
#define REG_ES     XMU_ES
#define REG_FS     XMU_FS
#define REG_GS     XMU_GS
#define REG_SS     XMU_SS
#define REG_CR0    XMU_CR0
#define REG_CR2    XMU_CR2
#define REG_CR3    XMU_CR3
#define REG_CR4    XMU_CR4
#define REG_EFER   XMU_EFER
#define REG_COUNT  XMU_NREGS

/* ── RFLAGS bits ──────────────────────────────────────────────── */
#define RFLAG_CF   (1ULL << 0)
#define RFLAG_PF   (1ULL << 2)
#define RFLAG_AF   (1ULL << 4)
#define RFLAG_ZF   (1ULL << 6)
#define RFLAG_SF   (1ULL << 7)
#define RFLAG_TF   (1ULL << 8)
#define RFLAG_IF   (1ULL << 9)
#define RFLAG_DF   (1ULL << 10)
#define RFLAG_OF   (1ULL << 11)
#define RFLAG_IOPL (3ULL << 12)
#define RFLAG_NT   (1ULL << 14)
#define RFLAG_RF   (1ULL << 16)
#define RFLAG_ID   (1ULL << 21)

/* ── CR0 bits ─────────────────────────────────────────────────── */
#define CR0_PE   (1ULL << 0)
#define CR0_MP   (1ULL << 1)
#define CR0_EM   (1ULL << 2)
#define CR0_WP   (1ULL << 16)
#define CR0_PG   (1ULL << 31)

/* ── EFER bits ────────────────────────────────────────────────── */
#define EFER_SCE  (1ULL << 0)
#define EFER_LME  (1ULL << 8)
#define EFER_LMA  (1ULL << 10)
#define EFER_NXE  (1ULL << 11)

/* ── Page table bits ──────────────────────────────────────────── */
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_ACCESSED (1ULL << 5)
#define PTE_DIRTY    (1ULL << 6)
#define PTE_HUGE     (1ULL << 7)
#define PTE_NX       (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* ── Exception vectors ────────────────────────────────────────── */
#define EXC_DE   0
#define EXC_DB   1
#define EXC_NMI  2
#define EXC_BP   3
#define EXC_OF   4
#define EXC_BR   5
#define EXC_UD   6
#define EXC_NM   7
#define EXC_DF   8
#define EXC_TS   10
#define EXC_NP   11
#define EXC_SS   12
#define EXC_GP   13
#define EXC_PF   14
#define EXC_MF   16
#define EXC_AC   17
#define EXC_MC   18
#define EXC_XM   19

/* ── VM exit reasons ──────────────────────────────────────────── */
typedef enum {
    VMEXIT_NONE = 0,
    VMEXIT_HLT,
    VMEXIT_CPUID,
    VMEXIT_IO,
    VMEXIT_MMIO,
    VMEXIT_MSR_READ,
    VMEXIT_MSR_WRITE,
    VMEXIT_INTERRUPT,
    VMEXIT_EXCEPTION,
    VMEXIT_TRIPLE_FAULT,
    VMEXIT_SHUTDOWN,
    VMEXIT_PAUSE,
    VMEXIT_INVALID_OPCODE,
} XmuVmExitReason;

/* ── Segment register ─────────────────────────────────────────── */
typedef struct {
    uint16_t selector;
    uint64_t base;
    uint32_t limit;
    uint32_t access;
} XmuSegment;

/* ── VCPU ─────────────────────────────────────────────────────── */
typedef struct XmuVcpu {
    uint64_t regs[XMU_NREGS];

    XmuSegment seg_cs, seg_ds, seg_es;
    XmuSegment seg_fs, seg_gs, seg_ss;

    uint64_t gdtr_base;  uint32_t gdtr_limit;
    uint64_t idtr_base;  uint32_t idtr_limit;
    uint64_t ldtr_base;  uint16_t ldtr_sel;
    uint64_t tr_base;    uint16_t tr_sel;

    bool     int_pending;
    uint8_t  int_vector;
    bool     nmi_pending;
    bool     halted;

    XmuVmExitReason exit_reason;
    uint64_t        exit_qual;
    uint64_t        exit_info1;
    uint64_t        exit_info2;

    uint64_t insn_fetch_addr;
    uint8_t  insn_buf[16];
    int      insn_len;

    int      vcpu_id;
    struct XmuVm *vm;

    uint64_t insn_count;
} XmuVcpu;

/* ── Memory region ────────────────────────────────────────────── */
typedef enum {
    MEM_TYPE_RAM = 0,
    MEM_TYPE_ROM,
    MEM_TYPE_MMIO,
    MEM_TYPE_RESERVED,
} XmuMemType;

typedef struct XmuMemRegion {
    uint64_t    gpa;
    uint64_t    size;
    uint8_t    *hva;
    XmuMemType  type;
    bool        writable;
    bool        executable;
    uint64_t (*mmio_read)(struct XmuMemRegion*, uint64_t addr, int size);
    void     (*mmio_write)(struct XmuMemRegion*, uint64_t addr, uint64_t val, int size);
    struct XmuMemRegion *next;
} XmuMemRegion;

/* ── I/O handler ──────────────────────────────────────────────── */
typedef struct XmuIoHandler {
    uint16_t port_lo, port_hi;
    uint32_t (*read) (uint16_t port, int size, void *opaque);
    void     (*write)(uint16_t port, uint32_t val, int size, void *opaque);
    void    *opaque;
    struct XmuIoHandler *next;
} XmuIoHandler;

/* ── 8259A PIC ────────────────────────────────────────────────── */
typedef struct {
    uint8_t irr, isr, imr;
    uint8_t icw1, icw2, icw3, icw4;
    int     init_state;
    bool    auto_eoi;
    int     base_vector;
    bool    is_slave;
} XmuPic;

/* ── 8253 PIT ─────────────────────────────────────────────────── */
typedef struct {
    uint8_t  mode;
    uint16_t reload, count;
    bool     gate, output;
    bool     latch_active;
    uint16_t latched_val;
    bool     read_low;
    uint8_t  access_mode;
    uint8_t  rw_state;
} XmuPitChannel;

typedef struct {
    XmuPitChannel ch[3];
    uint64_t      base_time_ns;
    uint64_t      freq_hz;
} XmuPit;

/* ── 16550A UART ──────────────────────────────────────────────── */
#define UART_FIFO_SIZE 64
typedef struct {
    uint8_t  rbr, thr, ier, iir, fcr, lcr, mcr, lsr, msr, scr;
    uint16_t dll, dlh;
    uint8_t  rx_fifo[UART_FIFO_SIZE];
    int      rx_head, rx_tail, rx_count;
    uint8_t  tx_fifo[UART_FIFO_SIZE];
    int      tx_head, tx_tail, tx_count;
    bool     dlab;
    int      irq;
    void   (*tx_byte)(uint8_t ch, void *opaque);
    void    *opaque;
} XmuUart;

/* ── VM config ────────────────────────────────────────────────── */
typedef struct {
    const char *name;
    uint64_t    ram_size;
    int         num_vcpus;
    const char *bios_path;
    const char *kernel_path;
    const char *initrd_path;
    const char *cmdline;
    bool        enable_kvm;
    bool        enable_debug;
    FILE       *log_file;
} XmuVmConfig;

/* ── VM state ─────────────────────────────────────────────────── */
typedef enum {
    VM_STATE_CREATED = 0,
    VM_STATE_RUNNING,
    VM_STATE_PAUSED,
    VM_STATE_STOPPED,
    VM_STATE_ERROR,
} XmuVmState;

typedef struct XmuVm {
    int          vm_id;
    char         name[64];
    XmuVmState   state;

    XmuVcpu     *vcpus[XMU_MAX_VCPUS];
    int          num_vcpus;

    uint8_t     *ram;
    uint64_t     ram_size;
    XmuMemRegion *mem_regions;
    XmuIoHandler *io_handlers;

    XmuPic       pic_master, pic_slave;
    XmuPit       pit;
    XmuUart      uart[4];
    uint64_t     msrs[0x300];

    XmuVmConfig  config;
    bool         debug;
    FILE        *log;

    uint64_t     total_insns;
    uint64_t     run_time_ns;

    struct XmuVm *next;
} XmuVm;

/* ── Hypervisor ───────────────────────────────────────────────── */
typedef struct {
    XmuVm  *vms;
    int     num_vms;
    bool    initialized;
    FILE   *log;
} XmuHypervisor;

/* ═══ Public API ═══════════════════════════════════════════════ */
int         xmu_init(XmuHypervisor *hv, FILE *log);
void        xmu_destroy(XmuHypervisor *hv);

XmuVm      *xmu_vm_create(XmuHypervisor *hv, const XmuVmConfig *cfg);
void        xmu_vm_destroy(XmuVm *vm);
int         xmu_vm_run(XmuVm *vm);
int         xmu_vm_pause(XmuVm *vm);
int         xmu_vm_reset(XmuVm *vm);

XmuVcpu    *xmu_vcpu_create(XmuVm *vm, int id);
void        xmu_vcpu_destroy(XmuVcpu *vcpu);
int         xmu_vcpu_run(XmuVcpu *vcpu, uint64_t max_insns);
void        xmu_vcpu_reset(XmuVcpu *vcpu);

int         xmu_mem_add_region(XmuVm *vm, uint64_t gpa, uint64_t size,
                               XmuMemType type, const void *data);
int         xmu_mem_remove_region(XmuVm *vm, uint64_t gpa);
uint8_t    *xmu_mem_lookup(XmuVm *vm, uint64_t gpa, int size, bool write);
uint64_t    xmu_mem_read(XmuVm *vm, uint64_t gpa, int size);
void        xmu_mem_write(XmuVm *vm, uint64_t gpa, uint64_t val, int size);
void        xmu_mem_write_bulk(XmuVm *vm, uint64_t gpa, const void *data, size_t len);

int         xmu_io_register(XmuVm *vm, uint16_t lo, uint16_t hi,
                            uint32_t (*rd)(uint16_t, int, void*),
                            void     (*wr)(uint16_t, uint32_t, int, void*),
                            void *opaque);

void        xmu_vcpu_inject_irq(XmuVcpu *vcpu, int irq);
void        xmu_vcpu_inject_exception(XmuVcpu *vcpu, int vector,
                                      bool has_err, uint32_t err_code);

void        xmu_pic_init(XmuPic *pic, bool slave, int base_vec);
void        xmu_pit_init(XmuPit *pit);
void        xmu_uart_init(XmuUart *uart, int irq,
                          void (*tx)(uint8_t, void*), void *opaque);
void        xmu_uart_rx_char(XmuUart *uart, uint8_t ch);

XmuMemRegion *xmu_mem_find_region(XmuVm *vm, uint64_t gpa);

const char *xmu_reg_name(XmuRegId r);
void        xmu_vcpu_dump(const XmuVcpu *vcpu, FILE *out);
void        xmu_mem_dump(XmuVm *vm, uint64_t gpa, int len, FILE *out);

#endif /* XMU_H */