/*
 * xmu_mem.c  –  Guest physical memory management
 *
 * Implements:
 *   · Physical memory region registration (RAM / ROM / MMIO)
 *   · GPA → HVA lookup with caching
 *   · 1/2/4/8-byte guest memory reads and writes
 *   · Memory hex-dump utility
 */

#include "xmu.h"

/* ── Find a region covering [gpa, gpa+size) ──────────────────── */
XmuMemRegion *xmu_mem_find_region(XmuVm *vm, uint64_t gpa) {
    for (XmuMemRegion *r = vm->mem_regions; r; r = r->next) {
        if (gpa >= r->gpa && gpa < r->gpa + r->size)
            return r;
    }
    return NULL;
}

/* ── Add a memory region ──────────────────────────────────────── */
int xmu_mem_add_region(XmuVm *vm, uint64_t gpa, uint64_t size,
                       XmuMemType type, const void *data)
{
    XmuMemRegion *r = calloc(1, sizeof(XmuMemRegion));
    if (!r) return -1;

    r->gpa  = gpa;
    r->size = size;
    r->type = type;

    if (type == MEM_TYPE_RAM || type == MEM_TYPE_ROM) {
        r->hva = calloc(1, size);
        if (!r->hva) { free(r); return -1; }
        if (data) memcpy(r->hva, data, size);
        r->writable   = (type == MEM_TYPE_RAM);
        r->executable = true;
    }

    /* Prepend to list */
    r->next = vm->mem_regions;
    vm->mem_regions = r;
    return 0;
}

/* ── Remove a memory region ───────────────────────────────────── */
int xmu_mem_remove_region(XmuVm *vm, uint64_t gpa) {
    XmuMemRegion **pp = &vm->mem_regions;
    while (*pp) {
        if ((*pp)->gpa == gpa) {
            XmuMemRegion *r = *pp;
            *pp = r->next;
            if (r->hva) free(r->hva);
            free(r);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

/* ── Translate GPA → HVA (NULL if MMIO or unmapped) ──────────── */
uint8_t *xmu_mem_lookup(XmuVm *vm, uint64_t gpa, int size, bool write) {
    XmuMemRegion *r = xmu_mem_find_region(vm, gpa);
    if (!r) return NULL;
    if (r->type == MEM_TYPE_MMIO) return NULL;
    if (write && !r->writable) return NULL;
    uint64_t off = gpa - r->gpa;
    if (off + (uint64_t)size > r->size) return NULL;
    return r->hva + off;
}

/* ── Generic guest physical memory read (1/2/4/8 bytes) ──────── */
uint64_t xmu_mem_read(XmuVm *vm, uint64_t gpa, int size) {
    XmuMemRegion *r = xmu_mem_find_region(vm, gpa);
    if (!r) {
        if (vm->debug)
            fprintf(vm->log, "[MEM] Read from unmapped GPA %016llx (size=%d)\n",
                    (unsigned long long)gpa, size);
        return ~0ULL;
    }

    if (r->type == MEM_TYPE_MMIO) {
        if (r->mmio_read) return r->mmio_read(r, gpa, size);
        return ~0ULL;
    }

    uint64_t off = gpa - r->gpa;
    uint8_t *p   = r->hva + off;

    switch (size) {
    case 1: return *p;
    case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
    case 8: { uint64_t v; memcpy(&v, p, 8); return v; }
    default: return 0;
    }
}

/* ── Generic guest physical memory write ─────────────────────── */
void xmu_mem_write(XmuVm *vm, uint64_t gpa, uint64_t val, int size) {
    XmuMemRegion *r = xmu_mem_find_region(vm, gpa);
    if (!r) {
        if (vm->debug)
            fprintf(vm->log, "[MEM] Write to unmapped GPA %016llx val=%016llx (size=%d)\n",
                    (unsigned long long)gpa,
                    (unsigned long long)val, size);
        return;
    }

    if (r->type == MEM_TYPE_MMIO) {
        if (r->mmio_write) r->mmio_write(r, gpa, val, size);
        return;
    }

    if (!r->writable) {
        if (vm->debug)
            fprintf(vm->log, "[MEM] Write to ROM GPA %016llx ignored\n",
                    (unsigned long long)gpa);
        return;
    }

    uint64_t off = gpa - r->gpa;
    uint8_t *p   = r->hva + off;

    switch (size) {
    case 1: *p = (uint8_t)val; break;
    case 2: { uint16_t v=(uint16_t)val; memcpy(p,&v,2); break; }
    case 4: { uint32_t v=(uint32_t)val; memcpy(p,&v,4); break; }
    case 8: { memcpy(p,&val,8); break; }
    }
}

/* ── Memory hex dump ──────────────────────────────────────────── */
void xmu_mem_dump(XmuVm *vm, uint64_t gpa, int len, FILE *out) {
    fprintf(out, "Memory dump GPA=%016llx len=%d\n",
            (unsigned long long)gpa, len);
    for (int i = 0; i < len; i++) {
        if (i % 16 == 0)
            fprintf(out, "  %016llx: ", (unsigned long long)(gpa + i));
        uint8_t b = (uint8_t)xmu_mem_read(vm, gpa + i, 1);
        fprintf(out, "%02x ", b);
        if (i % 16 == 15) {
            /* ASCII column */
            fprintf(out, " |");
            for (int j = i - 15; j <= i; j++) {
                uint8_t c = (uint8_t)xmu_mem_read(vm, gpa + j, 1);
                fputc((c >= 0x20 && c < 0x7F) ? c : '.', out);
            }
            fprintf(out, "|\n");
        }
    }
    if (len % 16) fputc('\n', out);
}