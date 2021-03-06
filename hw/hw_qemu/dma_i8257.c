/*
 * QEMU DMA emulation
 *
 * Copyright (c) 2003-2004 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdio.h>
#include "interface.h"
#include "qemu_common.h"

#define DEBUG_DMA

#define dolog(...) fprintf (stderr, "dma: " __VA_ARGS__)
#ifdef DEBUG_DMA
#define lwarn(...) fprintf (stderr, "dma: " __VA_ARGS__)
#define linfo(...) fprintf (stderr, "dma: " __VA_ARGS__)
#define ldebug(...) fprintf (stderr, "dma: " __VA_ARGS__)
#else
#define lwarn(...)
#define linfo(...)
#define ldebug(...)
#endif

struct dma_regs {
    int now[2];
    uint16_t base[2];
    uint8_t mode;
    uint8_t page;
    uint8_t pageh;
    uint8_t dack;
    uint8_t eop;
    DMA_transfer_handler transfer_handler;
    void *opaque;
};

#define ADDR 0
#define COUNT 1

static struct dma_cont {
    uint8_t status;
    uint8_t command;
    uint8_t mask;
    uint8_t flip_flop;
    int dshift;
    struct dma_regs regs[4];
} dma_controllers[2];
static int nb_used_dma_cont;

enum {
    CMD_MEMORY_TO_MEMORY = 0x01,
    CMD_FIXED_ADDRESS    = 0x02,
    CMD_BLOCK_CONTROLLER = 0x04,
    CMD_COMPRESSED_TIME  = 0x08,
    CMD_CYCLIC_PRIORITY  = 0x10,
    CMD_EXTENDED_WRITE   = 0x20,
    CMD_LOW_DREQ         = 0x40,
    CMD_LOW_DACK         = 0x80,
    CMD_NOT_SUPPORTED    = CMD_MEMORY_TO_MEMORY | CMD_FIXED_ADDRESS
    | CMD_COMPRESSED_TIME | CMD_CYCLIC_PRIORITY | CMD_EXTENDED_WRITE
    | CMD_LOW_DREQ | CMD_LOW_DACK

};

static void DMA_run (void);

static int channels[8] = {-1, 2, 3, 1, -1, -1, -1, 0};

void write_page (void *opaque, uint32_t nport, uint32_t data)
{
    struct dma_cont *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        dolog ("invalid channel %#x %#x\n", nport, data);
        return;
    }
    d->regs[ichan].page = data;
}

void write_pageh (void *opaque, uint32_t nport, uint32_t data)
{
    struct dma_cont *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        dolog ("invalid channel %#x %#x\n", nport, data);
        return;
    }
    d->regs[ichan].pageh = data;
}

uint32_t read_page (void *opaque, uint32_t nport)
{
    struct dma_cont *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        dolog ("invalid channel read %#x\n", nport);
        return 0;
    }
    return d->regs[ichan].page;
}

uint32_t read_pageh (void *opaque, uint32_t nport)
{
    struct dma_cont *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        dolog ("invalid channel read %#x\n", nport);
        return 0;
    }
    return d->regs[ichan].pageh;
}

static inline void init_chan (struct dma_cont *d, int ichan)
{
    struct dma_regs *r;

    r = d->regs + ichan;
    r->now[ADDR] = r->base[ADDR] << d->dshift;
    r->now[COUNT] = 0;
}

static inline int getff (struct dma_cont *d)
{
    int ff;

    ff = d->flip_flop;
    d->flip_flop = !ff;
    return ff;
}

uint32_t read_chan (void *opaque, uint32_t nport)
{
    struct dma_cont *d = opaque;
    int ichan, nreg, iport, ff, val, dir;
    struct dma_regs *r;

    iport = (nport >> d->dshift) & 0x0f;
    ichan = iport >> 1;
    nreg = iport & 1;
    r = d->regs + ichan;

    dir = ((r->mode >> 5) & 1) ? -1 : 1;
    ff = getff (d);
    if (nreg)
        val = (r->base[COUNT] << d->dshift) - r->now[COUNT];
    else
        val = r->now[ADDR] + r->now[COUNT] * dir;

    ldebug ("read_chan %#x -> %d\n", iport, val);
    return (val >> (d->dshift + (ff << 3))) & 0xff;
}

void write_chan (void *opaque, uint32_t nport, uint32_t data)
{
    struct dma_cont *d = opaque;
    int iport, ichan, nreg;
    struct dma_regs *r;

    fprintf(stderr, "write_chan %x %x\n", nport, data);
    iport = (nport >> d->dshift) & 0x0f;
    ichan = iport >> 1;
    nreg = iport & 1;
    r = d->regs + ichan;
    if (getff (d)) {
        r->base[nreg] = (r->base[nreg] & 0xff) | ((data << 8) & 0xff00);
        init_chan (d, ichan);
    } else {
        r->base[nreg] = (r->base[nreg] & 0xff00) | (data & 0xff);
    }
}

void write_cont (void *opaque, uint32_t nport, uint32_t data)
{
    struct dma_cont *d = opaque;
    int iport, ichan = 0;

    iport = (nport >> d->dshift) & 0x0f;
    fprintf(stderr, "write_cont %x %x\n", iport, data);
    switch (iport) {
    case 0x08:                  /* command */
        fprintf(stderr, "DMA %d command: %x\n", d->dshift, data);
        if ((data != 0) && (data & CMD_NOT_SUPPORTED)) {
            dolog ("command %#x not supported\n", data);
            return;
        }
        d->command = data;
        break;

    case 0x09:
        ichan = data & 3;
        if (data & 4) {
            d->status |= 1 << (ichan + 4);
        }
        else {
            d->status &= ~(1 << (ichan + 4));
        }
        d->status &= ~(1 << ichan);
        DMA_run();
        break;

    case 0x0a:                  /* single mask */
        if (data & 4)
            d->mask |= 1 << (data & 3);
        else
            d->mask &= ~(1 << (data & 3));
        DMA_run();
        break;

    case 0x0b:                  /* mode */
        {
            ichan = data & 3;
#ifdef DEBUG_DMA
            {
                int op, ai, dir, opmode;
                op = (data >> 2) & 3;
                ai = (data >> 4) & 1;
                dir = (data >> 5) & 1;
                opmode = (data >> 6) & 3;

                linfo ("ichan %d, op %d, ai %d, dir %d, opmode %d\n",
                       ichan, op, ai, dir, opmode);
            }
#endif
            d->regs[ichan].mode = data;
            break;
        }

    case 0x0c:                  /* clear flip flop */
        fprintf(stderr, "DMA %d clear flip flop\n", d->dshift);
        d->flip_flop = 0;
        break;

    case 0x0d:                  /* reset */
        fprintf(stderr, "DMA %d reset\n", d->dshift);
        d->flip_flop = 0;
        d->mask = ~0;
        d->status = 0;
        d->command = 0;
        break;

    case 0x0e:                  /* clear mask for all channels */
        fprintf(stderr, "DMA %d clear mask for all chan\n", d->dshift);
        d->mask = 0;
        DMA_run();
        break;

    case 0x0f:                  /* write mask for all channels */
        fprintf(stderr, "DMA %d write mask %x for all chan\n", d->dshift, data);
        d->mask = data;
        DMA_run();
        break;

    default:
        dolog ("unknown iport %#x\n", iport);
        break;
    }

#ifdef DEBUG_DMA
    if (0xc != iport) {
        linfo ("write_cont: nport %#06x, ichan % 2d, val %#06x\n",
               nport, ichan, data);
    }
#endif
}

uint32_t read_cont (void *opaque, uint32_t nport)
{
    struct dma_cont *d = opaque;
    int iport, val;

    iport = (nport >> d->dshift) & 0x0f;
    switch (iport) {
    case 0x08:                  /* status */
        val = d->status;
        d->status &= 0xf0;
        break;
    case 0x0f:                  /* mask */
        val = d->mask;
        break;
    default:
        val = 0;
        break;
    }

    ldebug ("read_cont: nport %#06x, iport %#04x val %#x\n", nport, iport, val);
    return val;
}

int DMA_get_channel_mode (int nchan)
{
    return dma_controllers[nchan > 3].regs[nchan & 3].mode;
}

void DMA_hold_DREQ (int nchan)
{
    int ncont, ichan;

    ncont = nchan > 3;
    ichan = nchan & 3;
    linfo ("held cont=%d chan=%d\n", ncont, ichan);
    dma_controllers[ncont].status |= 1 << (ichan + 4);
    DMA_run();
}

void DMA_release_DREQ (int nchan)
{
    int ncont, ichan;

    ncont = nchan > 3;
    ichan = nchan & 3;
    linfo ("released cont=%d chan=%d\n", ncont, ichan);
    dma_controllers[ncont].status &= ~(1 << (ichan + 4));
    DMA_run();
}

static void channel_run (int ncont, int ichan)
{
    int n;
    struct dma_regs *r = &dma_controllers[ncont].regs[ichan];
#ifdef DEBUG_DMA
    int dir, opmode;

    dir = (r->mode >> 5) & 1;
    opmode = (r->mode >> 6) & 3;

    if (dir) {
        dolog ("DMA in address decrement mode\n");
    }
    if (opmode != 1) {
        dolog ("DMA not in single mode select %#x\n", opmode);
    }
#endif

    r = dma_controllers[ncont].regs + ichan;
    n = r->transfer_handler (r->opaque, ichan + (ncont << 2),
                             r->now[COUNT], (r->base[COUNT] + 1) << ncont);
    r->now[COUNT] = n;
    ldebug ("dma_pos %d size %d\n", n, (r->base[COUNT] + 1) << ncont);
}

static QEMUBH *dma_bh;

static void DMA_run (void)
{
    struct dma_cont *d;
    int icont, ichan;
    int rearm = 0;

    d = dma_controllers;

    for (icont = 0; icont < 2; icont++, d++) {
        for (ichan = 0; ichan < 4; ichan++) {
            int mask;

            mask = 1 << ichan;

            if ((0 == (d->mask & mask)) && (0 != (d->status & (mask << 4)))) {
                channel_run (icont, ichan);
                rearm = 1;
            }
        }
    }

    if (rearm)
        qemu_bh_schedule_idle(dma_bh);
}

void DMA_run_bh(void *unused)
{
    DMA_run();
}

void DMA_register_channel (int nchan,
                           DMA_transfer_handler transfer_handler,
                           void *opaque)
{
    struct dma_regs *r;
    int ichan, ncont;

    ncont = nchan > 3;
    ichan = nchan & 3;

    r = dma_controllers[ncont].regs + ichan;
    r->transfer_handler = transfer_handler;
    r->opaque = opaque;
}

int DMA_read_memory (int nchan, void *buf, int pos, int len)
{
    struct dma_regs *r = &dma_controllers[nchan > 3].regs[nchan & 3];
    target_phys_addr_t addr = ((r->pageh & 0x7f) << 24) | (r->page << 16) | r->now[ADDR];

    if (r->mode & 0x20) {
        int i;
        uint8_t *p = buf;

        cpu_physical_memory_read (addr - pos - len, buf, len);
        /* What about 16bit transfers? */
        for (i = 0; i < len >> 1; i++) {
            uint8_t b = p[len - i - 1];
            p[i] = b;
        }
    }
    else
        cpu_physical_memory_read (addr + pos, buf, len);

    return len;
}

int DMA_write_memory (int nchan, void *buf, int pos, int len)
{
    struct dma_regs *r = &dma_controllers[nchan > 3].regs[nchan & 3];
    target_phys_addr_t addr = ((r->pageh & 0x7f) << 24) | (r->page << 16) | r->now[ADDR];

    if (r->mode & 0x20) {
        int i;
        uint8_t *p = buf;

        cpu_physical_memory_write (addr - pos - len, buf, len);
        /* What about 16bit transfers? */
        for (i = 0; i < len; i++) {
            uint8_t b = p[len - i - 1];
            p[i] = b;
        }
    }
    else
        cpu_physical_memory_write (addr + pos, buf, len);

    return len;
}

/* request the emulator to transfer a new DMA memory block ASAP */
/*
void DMA_schedule(int nchan)
{
    CPUState *env = cpu_single_env;
    if (env)
        cpu_interrupt(env, CPU_INTERRUPT_EXIT);
}
*/

static void dma_reset(void *opaque)
{
    struct dma_cont *d = opaque;
    write_cont (d, (0x0d << d->dshift), 0);
}

static int dma_phony_handler (void *opaque, int nchan, int dma_pos, int dma_len)
{
    dolog ("unregistered DMA channel used nchan=%d dma_pos=%d dma_len=%d\n",
           nchan, dma_pos, dma_len);
    return dma_pos;
}

/*
static void dma_save (QEMUFile *f, void *opaque)
{
    struct dma_cont *d = opaque;
    int i;

    // qemu_put_8s (f, &d->status); 
    qemu_put_8s (f, &d->command);
    qemu_put_8s (f, &d->mask);
    qemu_put_8s (f, &d->flip_flop);
    qemu_put_be32 (f, d->dshift);

    for (i = 0; i < 4; ++i) {
        struct dma_regs *r = &d->regs[i];
        qemu_put_be32 (f, r->now[0]);
        qemu_put_be32 (f, r->now[1]);
        qemu_put_be16s (f, &r->base[0]);
        qemu_put_be16s (f, &r->base[1]);
        qemu_put_8s (f, &r->mode);
        qemu_put_8s (f, &r->page);
        qemu_put_8s (f, &r->pageh);
        qemu_put_8s (f, &r->dack);
        qemu_put_8s (f, &r->eop);
    }
}

static int dma_load (QEMUFile *f, void *opaque, int version_id)
{
    struct dma_cont *d = opaque;
    int i;

    if (version_id != 1)
        return -EINVAL;

    // qemu_get_8s (f, &d->status); 
    qemu_get_8s (f, &d->command);
    qemu_get_8s (f, &d->mask);
    qemu_get_8s (f, &d->flip_flop);
    d->dshift=qemu_get_be32 (f);

    for (i = 0; i < 4; ++i) {
        struct dma_regs *r = &d->regs[i];
        r->now[0]=qemu_get_be32 (f);
        r->now[1]=qemu_get_be32 (f);
        qemu_get_be16s (f, &r->base[0]);
        qemu_get_be16s (f, &r->base[1]);
        qemu_get_8s (f, &r->mode);
        qemu_get_8s (f, &r->page);
        qemu_get_8s (f, &r->pageh);
        qemu_get_8s (f, &r->dack);
        qemu_get_8s (f, &r->eop);
    }

    DMA_run();

    return 0;
}
*/

/* dshift = 0: 8 bit DMA, 1 = 16 bit DMA */
struct dma_cont* DMA_c_init(int dshift)
{
    int i;
    struct dma_cont *d;
    
    if (nb_used_dma_cont >= ARRAY_SIZE(dma_controllers)) {
        fprintf(stderr, "Out of DMA controllers.\n");
        exit(1);
    }

    d = &dma_controllers[nb_used_dma_cont++];
    fprintf(stderr, "%p\n", d);

    d->dshift = dshift;
    dma_reset(d);
    for (i = 0; i < ARRAY_SIZE (d->regs); ++i) {
        d->regs[i].transfer_handler = dma_phony_handler;
    }

    // Register bottom-half callback routine when both controllers are created.
    if (nb_used_dma_cont == ARRAY_SIZE(dma_controllers))
    	dma_bh = qemu_bh_new(DMA_run_bh, NULL);

    return d;
}
