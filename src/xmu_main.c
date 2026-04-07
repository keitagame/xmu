/*
 * xmu_main.c  –  XMU command-line entry point
 *
 * Usage:
 *   xmu [options]
 *   xmu --demo          Run built-in demo guest
 *   xmu --kernel FILE   Boot a flat binary at 0x7C00
 *   xmu --debug         Enable verbose debug output
 *   xmu --mem MB        Set guest RAM size (default: 32)
 *   xmu --shell         Interactive hypervisor shell
 */

#include "xmu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

/* Forward declarations from xmu_hv.c */
extern void xmu_mem_write_bulk(XmuVm *vm, uint64_t gpa, const void *data, size_t len);
extern void xmu_vga_dump(FILE *out);

/* ═══════════════════════════════════════════════════════════════
 *  Built-in Demo Guest Programs
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Demo 1: Real-mode "Hello from XMU!" via INT 10h
 * Runs at 0x7C00 (classic MBR load address)
 *
 * Generated from:
 *   cli
 *   mov  dx, 0x3F8       ; COM1 base
 *   mov  si, msg
 * .loop:
 *   lodsb
 *   test al, al
 *   jz   .done
 *   out  dx, al
 *   jmp  .loop
 * .done:
 *   hlt
 *
 * msg: db "Hello from XMU Hypervisor!", 13, 10, 0
 */
static const uint8_t demo_hello[] = {
    /* cli */
    0xFA,
    /* mov dx, 0x3F8 */
    0xBA, 0xF8, 0x03,
    /* mov si, 0x7C0C (offset to msg within segment) */
    0xBE, 0x0C, 0x7C,
    /* .loop: lodsb */
    0xAC,
    /* test al, al */
    0x84, 0xC0,
    /* jz .done (+4) */
    0x74, 0x04,
    /* out dx, al */
    0xEE,
    /* jmp .loop (-7) */
    0xEB, 0xF8,
    /* .done: hlt */
    0xF4,
    /* msg: "Hello from XMU Hypervisor!\r\n\0" */
    'H','e','l','l','o',' ','f','r','o','m',' ',
    'X','M','U',' ','H','y','p','e','r','v','i','s','o','r','!',
    '\r', '\n', 0x00
};

/*
 * Demo 2: Protected-mode setup + long-mode entry + back to UART print
 * (64-bit "Hello World" without OS)
 *
 * Steps:
 *  1. Real mode: enable A20, load GDT, enter protected mode
 *  2. 32-bit pmode: set up identity-mapped page tables
 *  3. Enable PAE, set CR3, set EFER.LME, enable PG → 64-bit
 *  4. 64-bit: print via port 0x3F8
 */
static const uint8_t demo_longmode[] = {
    /* ── Real mode start at 7C00 ─────────────────────────── */
    0xFA,                           /* cli                           */
    /* Load GDT (at 0x7E00) */
    0x0F, 0x01, 0x16, 0xF6, 0x7D,  /* lgdt [0x7DF6]                 */
    /* Enable protected mode: CR0 |= PE */
    0x0F, 0x20, 0xC0,               /* mov eax, cr0                  */
    0x0C, 0x01,                     /* or  al, 1                     */
    0x0F, 0x22, 0xC0,               /* mov cr0, eax                  */
    /* Far jump to flush pipeline and load CS with code32 selector */
    0xEA, 0x20, 0x7C, 0x08, 0x00,  /* jmp 0x0008:0x7C20             */
    /* ── Pad to 0x7C20 (32-bit protected mode entry) ─────── */
    0x90, 0x90,                     /* nop nop                       */
    /* 0x7C20: Set up segments */
    0x66, 0xB8, 0x10, 0x00,         /* mov ax, 0x10 (data32)         */
    0x8E, 0xD8,                     /* mov ds, ax                    */
    0x8E, 0xC0,                     /* mov es, ax                    */
    0x8E, 0xD0,                     /* mov ss, ax                    */
    /* Set up stack at 0x90000 */
    0x66, 0xBC, 0x00, 0x00, 0x09, 0x00, /* mov esp, 0x90000          */
    /* Build simple identity page tables at 0x1000 */
    /* PML4 at 0x1000, PDPT at 0x2000, PD at 0x3000 */
    /* Clear pages */
    0x66, 0x31, 0xC0,               /* xor eax, eax                  */
    0x66, 0xBF, 0x00, 0x10, 0x00, 0x00, /* mov edi, 0x1000           */
    0x66, 0xB9, 0x00, 0x30, 0x00, 0x00, /* mov ecx, 0x3000 (bytes/4) */
    0xF3, 0x66, 0xAB,               /* rep stosd                     */
    /* PML4[0] → PDPT at 0x2000 | P|W */
    0x66, 0xC7, 0x05, 0x00, 0x10, 0x00, 0x00, 0x03, 0x20, 0x00, 0x00,
    /* PDPT[0] → PD at 0x3000 | P|W */
    0x66, 0xC7, 0x05, 0x00, 0x20, 0x00, 0x00, 0x03, 0x30, 0x00, 0x00,
    /* PD[0] → 2MB page at 0x000000 | P|W|PS */
    0x66, 0xC7, 0x05, 0x00, 0x30, 0x00, 0x00, 0x83, 0x00, 0x00, 0x00,
    /* PD[1] → 2MB page at 0x200000 | P|W|PS */
    0x66, 0xC7, 0x05, 0x08, 0x30, 0x00, 0x00, 0x83, 0x00, 0x20, 0x00,
    /* CR3 = 0x1000 */
    0x66, 0xB8, 0x00, 0x10, 0x00, 0x00,
    0x0F, 0x22, 0xD8,               /* mov cr3, eax                  */
    /* Enable PAE: CR4 |= PAE(5) */
    0x0F, 0x20, 0xE0,               /* mov eax, cr4                  */
    0x66, 0x0D, 0x20, 0x00, 0x00, 0x00, /* or eax, 0x20              */
    0x0F, 0x22, 0xE0,               /* mov cr4, eax                  */
    /* EFER.LME: WRMSR ECX=0xC0000080 */
    0x66, 0xB9, 0x80, 0x00, 0x00, 0xC0, /* mov ecx, 0xC0000080       */
    0x0F, 0x32,                     /* rdmsr                         */
    0x66, 0x0D, 0x00, 0x01, 0x00, 0x00, /* or eax, 0x100 (LME)       */
    0x0F, 0x30,                     /* wrmsr                         */
    /* Enable paging: CR0 |= PG */
    0x0F, 0x20, 0xC0,               /* mov eax, cr0                  */
    0x66, 0x0D, 0x00, 0x00, 0x00, 0x80, /* or eax, 0x80000000 (PG)   */
    0x0F, 0x22, 0xC0,               /* mov cr0, eax                  */
    /* Far jump to 64-bit code */
    0xEA, 0xC0, 0x7C, 0x18, 0x00,  /* jmp 0x0018:0x7CC0             */

    /* ── Pad to align ────────────────────────────────────── */
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,

    /* 0x7CC0: 64-bit code: print "Long Mode!\r\n" to UART */
    /* mov rdx, 0x3F8 */
    0x48, 0xBA, 0xF8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* lea rsi, [rip + msg_offset] */
    0x48, 0x8D, 0x35, 0x0E, 0x00, 0x00, 0x00, /* rip+14 */
    /* .loop64: */
    0xAC,          /* lodsb               */
    0x84, 0xC0,    /* test al, al         */
    0x74, 0x04,    /* jz .done64          */
    0xEE,          /* out dx, al          */
    0xEB, 0xF8,    /* jmp .loop64         */
    /* .done64: */
    0xF4,          /* hlt                 */
    /* msg: */
    'L','o','n','g',' ','M','o','d','e',' ','E','n','t','e','r','e','d','!',
    '\r', '\n', 0x00
};

/* GDT for demo_longmode: */
/* 0x00: null, 0x08: code32 (DPL0 CS), 0x10: data32, 0x18: code64 */
static const uint8_t demo_gdt[] = {
    /* Null */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* Code32: base=0, limit=0xFFFFFFFF, DPL=0, C/E=1, 32-bit */
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xCF, 0x00,
    /* Data32: base=0, limit=0xFFFFFFFF, DPL=0, W=1 */
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xCF, 0x00,
    /* Code64: base=0, limit=0, DPL=0, L=1 (64-bit code) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x9A, 0x20, 0x00,
};

/* GDT descriptor (6 bytes: limit, base) stored at 0x7DF6 */
static uint8_t demo_gdtr[6];

/* ═══════════════════════════════════════════════════════════════
 *  Interactive hypervisor shell
 * ═══════════════════════════════════════════════════════════════ */

static void print_banner(void) {
    printf(
        "\n"
        "  ╔╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╦╗\n"
        "  ║                                   ║\n"
        "  ║   ██╗  ██╗███╗   ███╗██╗   ██╗   ║\n"
        "  ║   ╚██╗██╔╝████╗ ████║██║   ██║   ║\n"
        "  ║    ╚███╔╝ ██╔████╔██║██║   ██║   ║\n"
        "  ║    ██╔██╗ ██║╚██╔╝██║██║   ██║   ║\n"
        "  ║   ██╔╝ ██╗██║ ╚═╝ ██║╚██████╔╝   ║\n"
        "  ║   ╚═╝  ╚═╝╚═╝     ╚═╝ ╚═════╝    ║\n"
        "  ║                                   ║\n"
        "  ║   X Machine Unit Hypervisor       ║\n"
        "  ║   v" XMU_VERSION_STR " · x86-64 CPU Emulation  ║\n"
        "  ║                                   ║\n"
        "  ╚╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╩╝\n\n"
    );
}

static void print_help(void) {
    printf(
        "Commands:\n"
        "  run          Run VM until exit\n"
        "  step [N]     Step N instructions (default: 1)\n"
        "  regs         Dump VCPU registers\n"
        "  mem <GPA>    Dump 64 bytes at guest physical address\n"
        "  vga          Show VGA text screen\n"
        "  reset        Reset VM\n"
        "  info         VM information\n"
        "  help         This help\n"
        "  quit         Exit XMU\n\n"
    );
}

static void run_shell(XmuVm *vm) {
    char line[256];
    printf("\nXMU Shell ready. Type 'help' for commands.\n\n");

    while (1) {
        printf("xmu(%s)> ", vm->name);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        /* trim newline */
        line[strcspn(line, "\n")] = 0;

        if (!strcmp(line, "quit") || !strcmp(line, "q") || !strcmp(line, "exit")) {
            printf("Goodbye.\n");
            break;
        } else if (!strcmp(line, "help") || !strcmp(line, "h")) {
            print_help();
        } else if (!strcmp(line, "run") || !strcmp(line, "r")) {
            printf("[Shell] Running VM...\n");
            xmu_vm_run(vm);
            printf("[Shell] VM exited (reason=%d)\n",
                   vm->vcpus[0]->exit_reason);
        } else if (!strncmp(line, "step", 4)) {
            uint64_t n = 1;
            sscanf(line + 4, " %llu", (unsigned long long*)&n);
            vm->vcpus[0]->exit_reason = VMEXIT_NONE;
            vm->vcpus[0]->halted = false;
            xmu_vcpu_run(vm->vcpus[0], n);
            xmu_vcpu_dump(vm->vcpus[0], stdout);
        } else if (!strcmp(line, "regs")) {
            xmu_vcpu_dump(vm->vcpus[0], stdout);
        } else if (!strncmp(line, "mem", 3)) {
            uint64_t gpa = 0;
            sscanf(line + 3, " %llx", (unsigned long long*)&gpa);
            xmu_mem_dump(vm, gpa, 64, stdout);
        } else if (!strcmp(line, "vga")) {
            xmu_vga_dump(stdout);
        } else if (!strcmp(line, "reset")) {
            xmu_vm_reset(vm);
            printf("[Shell] VM reset.\n");
        } else if (!strcmp(line, "info")) {
            printf("VM: %s  state=%d  vcpus=%d  ram=%llu MB\n",
                   vm->name, vm->state, vm->num_vcpus,
                   (unsigned long long)(vm->ram_size / 1024 / 1024));
            printf("Total instructions executed: %llu\n",
                   (unsigned long long)vm->total_insns);
        } else if (strlen(line) > 0) {
            printf("Unknown command '%s'. Type 'help'.\n", line);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Signal handler
 * ═══════════════════════════════════════════════════════════════ */

static volatile int g_sigint = 0;
static void sigint_handler(int s) { (void)s; g_sigint = 1; }

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    print_banner();

    /* ── Parse arguments ─────────────────────────────────────── */
    bool     do_demo    = false;
    bool     do_demo2   = false;
    bool     do_shell   = false;
    bool     do_debug   = false;
    bool     do_run     = false;
    const char *kernel  = NULL;
    const char *bios    = NULL;
    uint64_t ram_mb     = 32;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--demo"))        do_demo  = true;
        else if (!strcmp(argv[i], "--demo2"))  do_demo2 = true;
        else if (!strcmp(argv[i], "--shell"))  do_shell = true;
        else if (!strcmp(argv[i], "--debug"))  do_debug = true;
        else if (!strcmp(argv[i], "--run"))    do_run   = true;
        else if (!strcmp(argv[i], "--kernel") && i+1 < argc)
            kernel = argv[++i];
        else if (!strcmp(argv[i], "--bios") && i+1 < argc)
            bios = argv[++i];
        else if (!strcmp(argv[i], "--mem") && i+1 < argc)
            ram_mb = strtoull(argv[++i], NULL, 10);
        else {
            printf("Usage: xmu [--demo|--demo2] [--shell] [--debug]\n"
                   "           [--kernel FILE] [--bios FILE] [--mem MB]\n"
                   "           [--run]\n");
            return 1;
        }
    }

    /* Default: run demo if no args */
    if (!do_demo && !do_demo2 && !kernel && !do_shell)
        do_demo = true;

    /* ── Set up hypervisor ───────────────────────────────────── */
    signal(SIGINT, sigint_handler);

    XmuHypervisor hv;
    xmu_init(&hv, stdout);

    /* ── Create VM ───────────────────────────────────────────── */
    XmuVmConfig cfg = {
        .name         = "xmu-vm0",
        .ram_size     = ram_mb * 1024 * 1024,
        .num_vcpus    = 1,
        .bios_path    = bios,
        .kernel_path  = kernel,
        .enable_debug = do_debug,
        .log_file     = stdout,
    };
    XmuVm *vm = xmu_vm_create(&hv, &cfg);
    if (!vm) {
        fprintf(stderr, "Failed to create VM\n");
        return 1;
    }

    /* ── Load demo guest ─────────────────────────────────────── */
    if (do_demo) {
        printf("[Main] Loading demo1 (real-mode UART hello)\n");
        xmu_mem_write_bulk(vm, 0x7C00, demo_hello, sizeof(demo_hello));
    } else if (do_demo2) {
        printf("[Main] Loading demo2 (long-mode transition)\n");

        /* Install GDT at 0x7E00 */
        xmu_mem_write_bulk(vm, 0x7E00, demo_gdt, sizeof(demo_gdt));

        /* GDT descriptor (limit=31, base=0x7E00) at 0x7DF6 */
        uint16_t gdt_limit = sizeof(demo_gdt) - 1;
        uint32_t gdt_base  = 0x7E00;
        memcpy(demo_gdtr + 0, &gdt_limit, 2);
        memcpy(demo_gdtr + 2, &gdt_base,  4);
        xmu_mem_write_bulk(vm, 0x7DF6, demo_gdtr, 6);

        xmu_mem_write_bulk(vm, 0x7C00, demo_longmode, sizeof(demo_longmode));
    }

    /* ── Run or shell ────────────────────────────────────────── */
    if (do_shell) {
        run_shell(vm);
    } else if (do_run || do_demo || do_demo2 || kernel) {
        printf("\n[Main] Starting VM execution...\n");
        printf("─────────────────────────────────────────\n");

        clock_t t0 = clock();
        int rc = xmu_vm_run(vm);
        clock_t t1 = clock();
        double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;

        printf("\n─────────────────────────────────────────\n");
        printf("[Main] Execution complete\n");
        printf("  Instructions: %llu\n", (unsigned long long)vm->total_insns);
        printf("  Time:         %.3f s\n", elapsed);
        if (elapsed > 0)
            printf("  Speed:        %.1f MIPS\n",
                   (double)vm->total_insns / elapsed / 1e6);
        printf("  Exit reason:  %d\n", vm->vcpus[0]->exit_reason);
        printf("\n");

        /* Dump final CPU state */
        xmu_vcpu_dump(vm->vcpus[0], stdout);

        if (do_demo2)
            xmu_vga_dump(stdout);
    }

    /* ── Cleanup ─────────────────────────────────────────────── */
    xmu_destroy(&hv);
    printf("\n[XMU] Hypervisor shutdown. Goodbye.\n");
    return 0;
}