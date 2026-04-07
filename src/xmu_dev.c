/*
 * xmu_dev.c  –  Device emulation
 *
 * Emulates:
 *   · Intel 8259A Dual PIC (master+slave, IRQ 0-15)
 *   · Intel 8253/8254 PIT  (channels 0-2, modes 0-3)
 *   · NS 16550A UART       (COM1-COM4 with FIFOs)
 *   · Simple VGA text-mode stub (MMIO 0xB8000)
 *   · CMOS/RTC stub
 */

#include "xmu.h"
#include <time.h>

/* ═══════════════════════════════════════════════════════════════
 *  8259A PIC
 * ═══════════════════════════════════════════════════════════════ */

void xmu_pic_init(XmuPic *pic, bool slave, int base_vec) {
    memset(pic, 0, sizeof(*pic));
    pic->is_slave    = slave;
    pic->base_vector = base_vec;
    pic->imr         = 0xFF;  /* all IRQs masked by default */
}

/* Get the highest-priority pending (unmasked) IRQ, -1 if none */
static int pic_pending_irq(const XmuPic *pic) {
    uint8_t pending = pic->irr & ~pic->imr;
    if (!pending) return -1;
    for (int i = 0; i < 8; i++)
        if (pending & (1 << i)) return i;
    return -1;
}

/* Raise an IRQ line */
void xmu_pic_raise_irq(XmuPic *pic, int irq) {
    pic->irr |= (1 << irq);
}

/* Lower an IRQ line */
void xmu_pic_lower_irq(XmuPic *pic, int irq) {
    pic->irr &= ~(1 << irq);
}

/* Get interrupt vector if any pending (for CPU interrupt check) */
int xmu_pic_get_vector(XmuPic *master, XmuPic *slave) {
    int irq = pic_pending_irq(master);
    if (irq < 0) return -1;
    /* IRQ 2 is cascade from slave */
    if (irq == 2) {
        int sirq = pic_pending_irq(slave);
        if (sirq >= 0) {
            slave->isr |= (1 << sirq);
            slave->irr &= ~(1 << sirq);
            return slave->base_vector + sirq;
        }
    }
    master->isr |= (1 << irq);
    master->irr &= ~(1 << irq);
    return master->base_vector + irq;
}

/* PIC I/O read */
static uint32_t pic_read(XmuPic *pic, uint16_t port, int size) {
    (void)size;
    if (port & 1) return pic->imr;
    /* ISR/IRR depending on last OCW3 */
    return pic->irr;
}

/* PIC I/O write */
static void pic_write(XmuPic *pic, uint16_t port, uint8_t val) {
    if (!(port & 1)) {
        /* Command port (OCW2/3 or ICW1) */
        if (val & 0x10) {
            /* ICW1 - init sequence */
            pic->init_state = 1;
            pic->irr = 0; pic->isr = 0;
        } else if (val == 0x20 || val == 0x60) {
            /* Non-specific EOI */
            for (int i = 0; i < 8; i++)
                if (pic->isr & (1<<i)) { pic->isr &= ~(1<<i); break; }
        }
    } else {
        /* Data port: ICW2/3/4 or OCW1 */
        switch (pic->init_state) {
        case 1: pic->base_vector = val & 0xF8; pic->init_state = 2; break;
        case 2: pic->init_state = (val & 0x01) ? 3 : 0; break;
        case 3: pic->auto_eoi = !!(val & 2); pic->init_state = 0; break;
        case 0: pic->imr = val; break;
        }
    }
}

/* ── PIC I/O port callbacks registered with VM ───────────────── */
static uint32_t pic_master_read_cb(uint16_t port, int sz, void *op) {
    return pic_read((XmuPic*)op, port, sz);
}
static void pic_master_write_cb(uint16_t port, uint32_t val, int sz, void *op) {
    (void)sz; pic_write((XmuPic*)op, port, (uint8_t)val);
}
static uint32_t pic_slave_read_cb(uint16_t port, int sz, void *op) {
    return pic_read((XmuPic*)op, port, sz);
}
static void pic_slave_write_cb(uint16_t port, uint32_t val, int sz, void *op) {
    (void)sz; pic_write((XmuPic*)op, port, (uint8_t)val);
}

/* ═══════════════════════════════════════════════════════════════
 *  8253/8254 PIT
 * ═══════════════════════════════════════════════════════════════ */

#define PIT_FREQ_HZ 1193182ULL

void xmu_pit_init(XmuPit *pit) {
    memset(pit, 0, sizeof(*pit));
    pit->freq_hz = PIT_FREQ_HZ;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pit->base_time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    /* Default: all channels mode 0, count=0 */
    for (int i = 0; i < 3; i++) {
        pit->ch[i].mode        = 0;
        pit->ch[i].reload      = 0;
        pit->ch[i].count       = 0;
        pit->ch[i].access_mode = 3; /* lo/hi */
        pit->ch[i].read_low    = true;
        pit->ch[i].rw_state    = 0;
    }
}

static uint16_t pit_read_count(XmuPit *pit, int ch_idx) {
    XmuPitChannel *ch = &pit->ch[ch_idx];
    if (ch->latch_active) {
        uint16_t v = ch->latched_val;
        ch->latch_active = false;
        return v;
    }
    /* Compute elapsed ticks */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    uint64_t elapsed_ns = now_ns - pit->base_time_ns;
    uint64_t elapsed_ticks = (elapsed_ns * PIT_FREQ_HZ) / 1000000000ULL;
    uint16_t rel = ch->reload ? (uint16_t)(elapsed_ticks % ch->reload) : 0;
    return ch->reload - rel;
}

static uint32_t pit_read_cb(uint16_t port, int sz, void *op) {
    (void)sz;
    XmuPit *pit = (XmuPit*)op;
    int ch_idx = port & 3;
    if (ch_idx > 2) return 0xFF; /* port 0x43 = control, read-back */
    XmuPitChannel *ch = &pit->ch[ch_idx];
    uint16_t count = pit_read_count(pit, ch_idx);
    if (ch->access_mode == 3) {
        /* lo/hi byte alternation */
        if (ch->rw_state == 0) { ch->rw_state = 1; return count & 0xFF; }
        else                   { ch->rw_state = 0; return (count >> 8) & 0xFF; }
    } else if (ch->access_mode == 1) {
        return count & 0xFF;
    } else {
        return (count >> 8) & 0xFF;
    }
}

static void pit_write_cb(uint16_t port, uint32_t val8, int sz, void *op) {
    (void)sz;
    XmuPit *pit = (XmuPit*)op;
    uint8_t val = (uint8_t)val8;
    if ((port & 3) == 3) {
        /* Mode/Command register */
        int ch_idx   = (val >> 6) & 3;
        int access   = (val >> 4) & 3;
        int mode     = (val >> 1) & 7;
        if (ch_idx == 3) return; /* read-back command, skip */
        XmuPitChannel *ch = &pit->ch[ch_idx];
        if (access == 0) {
            /* Latch command */
            ch->latch_active = true;
            ch->latched_val  = pit_read_count(pit, ch_idx);
        } else {
            ch->access_mode = access;
            ch->mode        = mode;
            ch->rw_state    = 0;
            ch->reload      = 0;
            ch->latch_active= false;
        }
        return;
    }
    /* Data write */
    int ch_idx = port & 3;
    XmuPitChannel *ch = &pit->ch[ch_idx];
    if (ch->access_mode == 3) {
        if (ch->rw_state == 0) {
            ch->reload = (ch->reload & 0xFF00) | val;
            ch->rw_state = 1;
        } else {
            ch->reload = (ch->reload & 0x00FF) | ((uint16_t)val << 8);
            ch->rw_state = 0;
            /* Reset base time on channel 0 reload */
            if (ch_idx == 0) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                pit->base_time_ns = (uint64_t)ts.tv_sec*1000000000ULL + ts.tv_nsec;
            }
        }
    } else if (ch->access_mode == 1) {
        ch->reload = (ch->reload & 0xFF00) | val;
    } else {
        ch->reload = (ch->reload & 0x00FF) | ((uint16_t)val << 8);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  NS 16550A UART
 * ═══════════════════════════════════════════════════════════════ */

/* LSR bits */
#define LSR_DR    0x01   /* Data Ready          */
#define LSR_OE    0x02   /* Overrun Error       */
#define LSR_THRE  0x20   /* THR Empty           */
#define LSR_TEMT  0x40   /* Transmitter Empty   */

void xmu_uart_init(XmuUart *uart, int irq,
                   void (*tx)(uint8_t, void*), void *opaque) {
    memset(uart, 0, sizeof(*uart));
    uart->irq     = irq;
    uart->lsr     = LSR_THRE | LSR_TEMT;  /* TX empty at init */
    uart->iir     = 0x01;                  /* No interrupt pending */
    uart->tx_byte = tx;
    uart->opaque  = opaque;
    uart->dll     = 12;  /* 9600 baud divisor (default) */
}

void xmu_uart_rx_char(XmuUart *uart, uint8_t ch) {
    if (uart->rx_count < UART_FIFO_SIZE) {
        uart->rx_fifo[uart->rx_tail] = ch;
        uart->rx_tail = (uart->rx_tail + 1) % UART_FIFO_SIZE;
        uart->rx_count++;
        uart->lsr |= LSR_DR;
    } else {
        uart->lsr |= LSR_OE;
    }
}

static uint32_t uart_read_cb(uint16_t port, int sz, void *op) {
    (void)sz;
    XmuUart *uart = (XmuUart*)op;
    int reg = port & 7;

    if (uart->dlab && (reg == 0)) return uart->dll & 0xFF;
    if (uart->dlab && (reg == 1)) return (uart->dll >> 8) & 0xFF;

    switch (reg) {
    case 0: { /* RBR */
        if (uart->rx_count > 0) {
            uint8_t c = uart->rx_fifo[uart->rx_head];
            uart->rx_head = (uart->rx_head + 1) % UART_FIFO_SIZE;
            uart->rx_count--;
            if (uart->rx_count == 0) uart->lsr &= ~LSR_DR;
            return c;
        }
        return 0;
    }
    case 1: return uart->ier;
    case 2: return uart->iir;
    case 3: return uart->lcr;
    case 4: return uart->mcr;
    case 5: return uart->lsr;
    case 6: return uart->msr;
    case 7: return uart->scr;
    }
    return 0xFF;
}

static void uart_write_cb(uint16_t port, uint32_t val8, int sz, void *op) {
    (void)sz;
    XmuUart *uart = (XmuUart*)op;
    uint8_t val = (uint8_t)val8;
    int reg = port & 7;

    if (uart->dlab && reg == 0) { uart->dll = (uart->dll & 0xFF00) | val; return; }
    if (uart->dlab && reg == 1) { uart->dll = (uart->dll & 0x00FF) | ((uint16_t)val<<8); return; }

    switch (reg) {
    case 0: /* THR - transmit */
        if (uart->tx_byte) uart->tx_byte(val, uart->opaque);
        uart->lsr |= (LSR_THRE | LSR_TEMT);
        break;
    case 1: uart->ier = val; break;
    case 2: uart->fcr = val; break;
    case 3:
        uart->lcr  = val;
        uart->dlab = !!(val & 0x80);
        break;
    case 4: uart->mcr = val; break;
    case 7: uart->scr = val; break;
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  CMOS / RTC stub  (ports 0x70–0x71)
 * ═══════════════════════════════════════════════════════════════ */
static uint8_t cmos_index = 0;

static uint32_t cmos_read_cb(uint16_t port, int sz, void *op) {
    (void)sz; (void)op;
    if (port == 0x71) {
        time_t t = time(NULL);
        struct tm *tm = gmtime(&t);
        switch (cmos_index) {
        case 0x00: return tm->tm_sec;
        case 0x02: return tm->tm_min;
        case 0x04: return tm->tm_hour;
        case 0x06: return tm->tm_wday + 1;
        case 0x07: return tm->tm_mday;
        case 0x08: return tm->tm_mon + 1;
        case 0x09: return tm->tm_year % 100;
        case 0x0A: return 0x26;  /* status A: UIP=0, 32kHz oscillator */
        case 0x0B: return 0x02;  /* status B: 24h, binary mode */
        case 0x0D: return 0x80;  /* status D: valid RAM */
        case 0x14: return 0x00;  /* equipment byte */
        case 0x15: return 0x00;  /* base memory low */
        case 0x16: return 0x02;  /* base memory high (640 KB = 0x0280) */
        case 0x17: return 0x00;  /* extended memory low */
        case 0x18: return 0x00;  /* extended memory high */
        case 0x32: return (tm->tm_year + 1900) / 100;
        default:   return 0xFF;
        }
    }
    return 0xFF;
}

static void cmos_write_cb(uint16_t port, uint32_t val, int sz, void *op) {
    (void)sz; (void)op;
    if (port == 0x70) cmos_index = (uint8_t)val & 0x7F;
}

/* ═══════════════════════════════════════════════════════════════
 *  VGA text-mode stub (MMIO 0xB8000)
 * ═══════════════════════════════════════════════════════════════ */
#define VGA_TEXT_BASE  0xB8000ULL
#define VGA_TEXT_SIZE  0x8000ULL   /* 32 KB covers text+attr cells */

#define VGA_COLS 80
#define VGA_ROWS 25

static uint8_t vga_text_buf[VGA_TEXT_SIZE];
static int     vga_cursor_row = 0, vga_cursor_col = 0;

static uint64_t vga_mmio_read(XmuMemRegion *r, uint64_t addr, int size) {
    (void)r;
    uint64_t off = addr - VGA_TEXT_BASE;
    if (off + size > VGA_TEXT_SIZE) return ~0ULL;
    uint64_t v = 0;
    memcpy(&v, vga_text_buf + off, size);
    return v;
}

static void vga_mmio_write(XmuMemRegion *r, uint64_t addr, uint64_t val, int size) {
    (void)r;
    uint64_t off = addr - VGA_TEXT_BASE;
    if (off + size > VGA_TEXT_SIZE) return;
    memcpy(vga_text_buf + off, &val, size);
}

/* VGA port stub */
static uint8_t vga_crtc_idx = 0;
static uint8_t vga_crtc_regs[256] = {0};

static uint32_t vga_port_read(uint16_t port, int sz, void *op) {
    (void)sz; (void)op;
    if (port == 0x3D5) return vga_crtc_regs[vga_crtc_idx];
    if (port == 0x3DA) return 0x08; /* VBlank */
    return 0xFF;
}
static void vga_port_write(uint16_t port, uint32_t val, int sz, void *op) {
    (void)sz; (void)op;
    if (port == 0x3D4) { vga_crtc_idx = (uint8_t)val; return; }
    if (port == 0x3D5) {
        vga_crtc_regs[vga_crtc_idx] = (uint8_t)val;
        if (vga_crtc_idx == 0x0F) /* cursor low */
            vga_cursor_col = vga_crtc_regs[0x0F] % VGA_COLS;
        if (vga_crtc_idx == 0x0E) /* cursor high */
            vga_cursor_row = vga_crtc_regs[0x0E] / VGA_COLS;
    }
}

/* Dump VGA text buffer to host terminal */
void xmu_vga_dump(FILE *out) {
    fprintf(out, "\n┌─── VGA Text (80x25) ");
    for (int i = 0; i < 58; i++) fputc('─', out);
    fprintf(out, "┐\n");
    for (int row = 0; row < VGA_ROWS; row++) {
        fprintf(out, "│");
        for (int col = 0; col < VGA_COLS; col++) {
            uint8_t ch  = vga_text_buf[(row * VGA_COLS + col) * 2];
            if (ch < 0x20 || ch > 0x7E) ch = ' ';
            fputc(ch, out);
        }
        fprintf(out, "│\n");
    }
    fprintf(out, "└");
    for (int i = 0; i < 80; i++) fputc('─', out);
    fprintf(out, "┘\n");
}

/* ═══════════════════════════════════════════════════════════════
 *  Device registration helper
 * ═══════════════════════════════════════════════════════════════ */

/* Register all standard PC devices with a VM */
void xmu_devices_init(XmuVm *vm) {
    /* PIC master: ports 0x20-0x21 */
    xmu_pic_init(&vm->pic_master, false, 0x08);
    xmu_io_register(vm, 0x20, 0x21,
                    pic_master_read_cb, pic_master_write_cb, &vm->pic_master);

    /* PIC slave: ports 0xA0-0xA1 */
    xmu_pic_init(&vm->pic_slave, true, 0x70);
    xmu_io_register(vm, 0xA0, 0xA1,
                    pic_slave_read_cb, pic_slave_write_cb, &vm->pic_slave);

    /* PIT: ports 0x40-0x43 */
    xmu_pit_init(&vm->pit);
    xmu_io_register(vm, 0x40, 0x43,
                    pit_read_cb, pit_write_cb, &vm->pit);

    /* UART COM1: ports 0x3F8-0x3FF */
    xmu_uart_init(&vm->uart[0], 4,
                  (void(*)(uint8_t,void*))fputc, stdout);
    xmu_io_register(vm, 0x3F8, 0x3FF,
                    uart_read_cb, uart_write_cb, &vm->uart[0]);

    /* UART COM2: ports 0x2F8-0x2FF */
    xmu_uart_init(&vm->uart[1], 3, NULL, NULL);
    xmu_io_register(vm, 0x2F8, 0x2FF,
                    uart_read_cb, uart_write_cb, &vm->uart[1]);

    /* CMOS/RTC: ports 0x70-0x71 */
    xmu_io_register(vm, 0x70, 0x71, cmos_read_cb, cmos_write_cb, NULL);

    /* VGA ports: 0x3B0-0x3DF */
    xmu_io_register(vm, 0x3B0, 0x3DF, vga_port_read, vga_port_write, NULL);

    /* VGA MMIO: 0xB8000-0xBFFFF */
    XmuMemRegion *vga_r = calloc(1, sizeof(XmuMemRegion));
    if (vga_r) {
        vga_r->gpa       = VGA_TEXT_BASE;
        vga_r->size      = VGA_TEXT_SIZE;
        vga_r->type      = MEM_TYPE_MMIO;
        vga_r->writable  = true;
        vga_r->mmio_read  = vga_mmio_read;
        vga_r->mmio_write = vga_mmio_write;
        vga_r->next = vm->mem_regions;
        vm->mem_regions = vga_r;
    }

    /* Fill VGA buffer with blank cells (space + grey-on-black) */
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vga_text_buf[i * 2]     = ' ';
        vga_text_buf[i * 2 + 1] = 0x07;  /* light grey on black */
    }
}