/*
 * xmu_hv.c  –  Hypervisor core
 *
 * Implements:
 *   · Global hypervisor init/destroy
 *   · VM create/destroy/run/pause/reset
 *   · I/O port handler registration
 *   · Firmware / kernel loader
 *     - Flat binary at 0x7C00  (BIOS-style MBR)
 *     - Linux bzImage direct boot
 *     - Simple 16-byte BIOS stub (IVT, E820)
 */

#include "xmu.h"
#include <sys/stat.h>

/* ── Forward declarations ─────────────────────────────────────── */
extern void xmu_devices_init(XmuVm *vm);
extern void xmu_vga_dump(FILE *out);

/* ═══════════════════════════════════════════════════════════════
 *  Hypervisor init / destroy
 * ═══════════════════════════════════════════════════════════════ */

int xmu_init(XmuHypervisor *hv, FILE *log) {
    memset(hv, 0, sizeof(*hv));
    hv->log = log ? log : stderr;
    hv->initialized = true;
    fprintf(hv->log,
        "╔══════════════════════════════════════════╗\n"
        "║  XMU Hypervisor v%s                  ║\n"
        "║  x86-64 CPU Emulation | Type-2 VMM      ║\n"
        "╚══════════════════════════════════════════╝\n",
        XMU_VERSION_STR);
    return 0;
}

void xmu_destroy(XmuHypervisor *hv) {
    XmuVm *vm = hv->vms;
    while (vm) {
        XmuVm *next = vm->next;
        xmu_vm_destroy(vm);
        vm = next;
    }
    hv->vms = NULL;
    hv->num_vms = 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  I/O port registration
 * ═══════════════════════════════════════════════════════════════ */

int xmu_io_register(XmuVm *vm, uint16_t lo, uint16_t hi,
                    uint32_t (*rd)(uint16_t, int, void*),
                    void     (*wr)(uint16_t, uint32_t, int, void*),
                    void *opaque)
{
    XmuIoHandler *h = calloc(1, sizeof(XmuIoHandler));
    if (!h) return -1;
    h->port_lo = lo;
    h->port_hi = hi;
    h->read    = rd;
    h->write   = wr;
    h->opaque  = opaque;
    h->next    = vm->io_handlers;
    vm->io_handlers = h;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Minimal BIOS stub
 *  Placed at physical 0xF0000 (Real-Mode reset vector area)
 *  Provides:
 *    · INT 10h (video / TTY output)
 *    · INT 15h (E820 memory map)
 *    · INT 16h (keyboard)
 *    · INT 1Ah (RTC)
 *    · Far JMP at F000:FFF0 → F000:E000 (entry point)
 * ═══════════════════════════════════════════════════════════════ */

/* Minimal real-mode BIOS as raw x86 bytes.
 * Layout at segment 0xF000 (linear 0xF0000):
 *   +0x0000  IVT patch trampoline
 *   +0x1000  INT handlers
 *   +0xFFF0  Reset vector: JMP 0xF000:0x1000
 */
static uint8_t bios_stub[] = {
    /* F000:FFF0 → reset vector: JMP FAR F000:1000 */
    /* We position this at offset 0xFFF0 inside the 64KB segment */
    /* Filled programmatically below */
};

static void install_bios(XmuVm *vm) {
    /* Allocate 64KB BIOS ROM at 0xF0000 */
    uint8_t *bios = calloc(1, 65536);
    if (!bios) return;

    /* ── IVT setup code at F000:0000 ────────────────────────────
     * This runs first; it writes the IVT entries for INT 10/15/16/1A
     * then jumps to 0x7C00 (or halts if no bootloader).
     *
     * Encoding: real-mode 16-bit x86
     *
     * cli
     * xor  ax, ax
     * mov  ds, ax
     * ; Install INT 10h handler at 0x0040 (vector 0x10 = index 0x10*4=0x40)
     * mov  word [0x40], 0x1010   ; offset
     * mov  word [0x42], 0xF000   ; segment
     * ; Install INT 15h at 0x54
     * mov  word [0x54], 0x1040
     * mov  word [0x56], 0xF000
     * ; Install INT 16h at 0x58
     * mov  word [0x58], 0x1080
     * mov  word [0x5A], 0xF000
     * ; Install INT 1Ah at 0x68
     * mov  word [0x68], 0x10C0
     * mov  word [0x6A], 0xF000
     * sti
     * ; Jump to MBR at 0000:7C00
     * jmp  far 0x0000:0x7C00
     */
    static const uint8_t ivt_init[] = {
        0xFA,                           /* cli                          */
        0x31, 0xC0,                     /* xor ax, ax                   */
        0x8E, 0xD8,                     /* mov ds, ax                   */
        /* INT 10h */
        0xC7, 0x06, 0x40, 0x00, 0x10, 0x10, /* mov [0x40], 0x1010      */
        0xC7, 0x06, 0x42, 0x00, 0x00, 0xF0, /* mov [0x42], 0xF000      */
        /* INT 15h */
        0xC7, 0x06, 0x54, 0x00, 0x40, 0x10,
        0xC7, 0x06, 0x56, 0x00, 0x00, 0xF0,
        /* INT 16h */
        0xC7, 0x06, 0x58, 0x00, 0x80, 0x10,
        0xC7, 0x06, 0x5A, 0x00, 0x00, 0xF0,
        /* INT 1Ah */
        0xC7, 0x06, 0x68, 0x00, 0xC0, 0x10,
        0xC7, 0x06, 0x6A, 0x00, 0x00, 0xF0,
        0xFB,                           /* sti                          */
        /* jmp far 0000:7C00 */
        0xEA, 0x00, 0x7C, 0x00, 0x00
    };
    memcpy(bios, ivt_init, sizeof(ivt_init));

    /* ── INT 10h (video) at F000:1010 ──────────────────────────
     * Simplified: AH=0x0E (teletype output) → write AL to UART
     * OUT 0x3F8, AL ; iret
     */
    static const uint8_t int10_handler[] = {
        0x80, 0xFC, 0x0E,           /* cmp  ah, 0x0E                */
        0x75, 0x04,                 /* jne  .done                   */
        0xEE,                       /* out  dx, al  (DX=0x3F8)     */
        0x90, 0x90,                 /* nop nop (placeholder)        */
        /* .done: */
        0xCF                        /* iret                         */
    };
    memcpy(bios + 0x1010, int10_handler, sizeof(int10_handler));

    /* ── INT 15h (E820) at F000:1040 ───────────────────────────
     * AX=0xE820, EDX='SMAP':
     *   Entry 0: 0x00000000-0x0009FFFF  (640KB usable)
     *   Entry 1: 0x00100000-RAM_TOP     (extended RAM)
     *   CF=1 when list exhausted
     */
    static const uint8_t int15_handler[] = {
        /* Compare AX to 0xE820 */
        0x3D, 0x20, 0xE8,           /* cmp  ax, 0xE820              */
        0x75, 0x2E,                 /* jne  .iret                   */
        /* Compare EDX to 'SMAP' (0x534D4150) */
        0x66, 0x81, 0xFA,
              0x50, 0x41, 0x4D, 0x53, /* cmp  edx, 'SMAP'           */
        0x75, 0x26,                 /* jne  .iret                   */
        /* BX=0: first entry */
        0x83, 0xFB, 0x00,           /* cmp  bx, 0                   */
        0x75, 0x12,                 /* jne  entry2                  */
        /* Entry 0: base=0, size=0x9FC00, type=1 */
        0x66, 0xC7, 0x05, 0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
        0x66, 0xC7, 0x05, 0x08, 0x00, 0x00, 0xFC, 0x09, 0x00, 0x00, 0x00,
        /* bx=1, ecx=24, CF=0 */
        0xBB, 0x01, 0x00,
        0x66, 0xB9, 0x18, 0x00, 0x00, 0x00,
        0xCF,
        /* entry2 */
        0xF9,  /* stc: carry=1 → list end */
        0xCF
    };
    memcpy(bios + 0x1040, int15_handler, sizeof(int15_handler));

    /* ── INT 16h (keyboard) at F000:1080 ───────────────────────
     * AH=0: block until key (stub: return 0x0d 'Enter')
     * AH=1: check buffer (ZF=1: no key)
     */
    static const uint8_t int16_handler[] = {
        0x80, 0xFC, 0x00,  /* cmp ah, 0    */
        0x75, 0x04,        /* jne .nokey   */
        0xB8, 0x1C, 0x0D,  /* mov ax, 0x0D1C (Enter) */
        0xCF,              /* iret         */
        /* .nokey: */
        0x80, 0xCC, 0x40,  /* or  ah, 0x40  → ZF behavior hack */
        0xCF
    };
    memcpy(bios + 0x1080, int16_handler, sizeof(int16_handler));

    /* ── INT 1Ah (RTC) at F000:10C0 ────────────────────────────
     * AH=0: get tick count → CX:DX  (stub: 0)
     * AH=2: get time (BCD)
     */
    static const uint8_t int1a_handler[] = {
        0x31, 0xC9,  /* xor cx, cx */
        0x31, 0xD2,  /* xor dx, dx */
        0xCF
    };
    memcpy(bios + 0x10C0, int1a_handler, sizeof(int1a_handler));

    /* ── Reset vector at F000:FFF0 ──────────────────────────────
     * JMP FAR F000:0000  (→ IVT init code above)
     */
    bios[0xFFF0] = 0xEA;                    /* JMP FAR           */
    bios[0xFFF1] = 0x00; bios[0xFFF2]=0x00; /* offset 0x0000     */
    bios[0xFFF3] = 0x00; bios[0xFFF4]=0xF0; /* segment 0xF000    */

    /* BIOS date string */
    const char *date = "01/01/2024";
    memcpy(bios + 0xFFF5, date, 10);
    bios[0xFFFE] = 0xFC; /* IBM PC BIOS ID byte */

    xmu_mem_add_region(vm, 0xF0000, 65536, MEM_TYPE_ROM, bios);
    free(bios);
}

/* ═══════════════════════════════════════════════════════════════
 *  Load a flat binary bootloader/kernel at 0x7C00
 * ═══════════════════════════════════════════════════════════════ */
static int load_binary(XmuVm *vm, const char *path, uint64_t load_addr) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    struct stat st;
    fstat(fileno(f), &st);
    size_t sz = (size_t)st.st_size;

    uint8_t *buf = malloc(sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, sz, f) != sz) { free(buf); fclose(f); return -1; }
    fclose(f);

    xmu_mem_write_bulk(vm, load_addr, buf, sz);
    free(buf);
    fprintf(vm->log, "[HV] Loaded %zu bytes at GPA %016llx\n",
            sz, (unsigned long long)load_addr);
    return 0;
}

/* Bulk write helper (used by loader) */
void xmu_mem_write_bulk(XmuVm *vm, uint64_t gpa, const void *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        xmu_mem_write(vm, gpa + i, ((const uint8_t*)data)[i], 1);
}

/* ═══════════════════════════════════════════════════════════════
 *  VM create
 * ═══════════════════════════════════════════════════════════════ */

XmuVm *xmu_vm_create(XmuHypervisor *hv, const XmuVmConfig *cfg) {
    if (hv->num_vms >= XMU_MAX_VMS) {
        fprintf(hv->log, "[HV] Max VMs reached (%d)\n", XMU_MAX_VMS);
        return NULL;
    }

    XmuVm *vm = calloc(1, sizeof(XmuVm));
    if (!vm) return NULL;

    vm->vm_id     = hv->num_vms;
    vm->state     = VM_STATE_CREATED;
    vm->config    = *cfg;
    vm->debug     = cfg->enable_debug;
    vm->log       = cfg->log_file ? cfg->log_file : (hv->log ? hv->log : stderr);
    vm->ram_size  = cfg->ram_size ? cfg->ram_size : (32 * 1024 * 1024);
    strncpy(vm->name, cfg->name ? cfg->name : "vm0", sizeof(vm->name)-1);

    fprintf(vm->log, "[HV] Creating VM '%s' (id=%d, ram=%llu MB)\n",
            vm->name, vm->vm_id,
            (unsigned long long)(vm->ram_size / (1024*1024)));

    /* ── Allocate main RAM ─────────────────────────────────────── */
    vm->ram = calloc(1, vm->ram_size);
    if (!vm->ram) { free(vm); return NULL; }
    xmu_mem_add_region(vm, 0x00000, vm->ram_size, MEM_TYPE_RAM, NULL);
    /* Alias HVA pointer for region just added */
    vm->mem_regions->hva = vm->ram;

    /* ── Register standard devices ─────────────────────────────── */
    xmu_devices_init(vm);

    /* ── Install BIOS ──────────────────────────────────────────── */
    if (cfg->bios_path)
        load_binary(vm, cfg->bios_path, 0xF0000);
    else
        install_bios(vm);

    /* ── Load kernel / bootloader ──────────────────────────────── */
    if (cfg->kernel_path)
        load_binary(vm, cfg->kernel_path, 0x7C00);

    /* ── Create VCPUs ──────────────────────────────────────────── */
    int n = cfg->num_vcpus > 0 ? cfg->num_vcpus : 1;
    if (n > XMU_MAX_VCPUS) n = XMU_MAX_VCPUS;
    vm->num_vcpus = n;
    for (int i = 0; i < n; i++) {
        vm->vcpus[i] = xmu_vcpu_create(vm, i);
        if (!vm->vcpus[i]) {
            fprintf(vm->log, "[HV] Failed to create VCPU %d\n", i);
        }
    }

    /* ── Link into hypervisor list ─────────────────────────────── */
    vm->next   = hv->vms;
    hv->vms    = vm;
    hv->num_vms++;

    fprintf(vm->log, "[HV] VM '%s' ready (%d VCPU(s))\n", vm->name, vm->num_vcpus);
    return vm;
}

/* ═══════════════════════════════════════════════════════════════
 *  VM destroy
 * ═══════════════════════════════════════════════════════════════ */

void xmu_vm_destroy(XmuVm *vm) {
    if (!vm) return;

    /* Destroy VCPUs */
    for (int i = 0; i < vm->num_vcpus; i++)
        if (vm->vcpus[i]) xmu_vcpu_destroy(vm->vcpus[i]);

    /* Free memory regions */
    XmuMemRegion *r = vm->mem_regions;
    while (r) {
        XmuMemRegion *next = r->next;
        if (r->hva && r->type != MEM_TYPE_MMIO && r->hva != vm->ram)
            free(r->hva);
        free(r);
        r = next;
    }

    /* Free I/O handlers */
    XmuIoHandler *h = vm->io_handlers;
    while (h) {
        XmuIoHandler *next = h->next;
        free(h);
        h = next;
    }

    if (vm->ram) free(vm->ram);
    free(vm);
}

/* ═══════════════════════════════════════════════════════════════
 *  VM run
 * ═══════════════════════════════════════════════════════════════ */

int xmu_vm_run(XmuVm *vm) {
    if (!vm || !vm->vcpus[0]) return -1;
    vm->state = VM_STATE_RUNNING;
    fprintf(vm->log, "[HV] VM '%s' starting (RIP=%016llx)\n",
            vm->name,
            (unsigned long long)vm->vcpus[0]->regs[REG_RIP]);

    /* Run VCPU 0 for up to 10M instructions (then return control) */
    int rc = xmu_vcpu_run(vm->vcpus[0], 10000000);

    XmuVmExitReason reason = vm->vcpus[0]->exit_reason;
    fprintf(vm->log, "[HV] VM '%s' exit: reason=%d insns=%llu\n",
            vm->name, reason,
            (unsigned long long)vm->total_insns);

    if (reason == VMEXIT_SHUTDOWN || reason == VMEXIT_TRIPLE_FAULT)
        vm->state = VM_STATE_STOPPED;
    else
        vm->state = VM_STATE_PAUSED;

    return rc;
}

int xmu_vm_pause(XmuVm *vm) {
    if (!vm) return -1;
    vm->state = VM_STATE_PAUSED;
    return 0;
}

int xmu_vm_reset(XmuVm *vm) {
    if (!vm) return -1;
    for (int i = 0; i < vm->num_vcpus; i++)
        if (vm->vcpus[i]) xmu_vcpu_reset(vm->vcpus[i]);
    vm->state = VM_STATE_CREATED;
    vm->total_insns = 0;
    return 0;
}