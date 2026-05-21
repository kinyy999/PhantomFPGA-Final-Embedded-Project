/*
 * QTest Unit Tests for PhantomFPGA Device v3.0
 *
 * Tests the PhantomFPGA virtual FPGA device's register behavior,
 * scatter-gather DMA configuration, and frame streaming.
 *
 * v3.0 streams pre-built data frames via scatter-gather DMA.
 * Build a driver, stream frames over TCP, display them on the host.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"
#include "libqos/malloc-pc.h"
#include "hw/pci/pci_regs.h"
#include "qemu/module.h"

/* ------------------------------------------------------------------------ */
/* Device Constants (mirrored from phantomfpga.h)                           */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_VENDOR_ID       0x0DAD
#define PHANTOMFPGA_DEVICE_ID       0xF00D

#define PHANTOMFPGA_DEV_ID_VAL      0xF00DFACE
#define PHANTOMFPGA_DEV_VER         0x00030000  /* v3.0.0 */

/* Frame constants - fixed, no configuration needed */
#define FRAME_SIZE                  5120        /* Bytes per frame */
#define FRAME_COUNT                 250         /* Total frames (10 sec @ 25fps) */
#define FRAME_MAGIC                 0xF00DFACE

/* Register offsets - simplified for v3.0 */
#define REG_DEV_ID              0x000
#define REG_DEV_VER             0x004
#define REG_CTRL                0x008
#define REG_STATUS              0x00C

/* Frame Configuration */
#define REG_FRAME_SIZE          0x010   /* R   - Frame size (5120) */
#define REG_FRAME_COUNT         0x014   /* R   - Total frames (250) */
#define REG_FRAME_RATE          0x018   /* R/W - Frames per second (1-60) */
#define REG_CURRENT_FRAME       0x01C   /* R   - Current frame index */

/* Descriptor Ring Configuration */
#define REG_DESC_RING_LO        0x020
#define REG_DESC_RING_HI        0x024
#define REG_DESC_RING_SIZE      0x028
#define REG_DESC_HEAD           0x02C
#define REG_DESC_TAIL           0x030

/* Interrupt Configuration */
#define REG_IRQ_STATUS          0x034
#define REG_IRQ_MASK            0x038
#define REG_IRQ_COALESCE        0x03C

/* Statistics */
#define REG_STAT_FRAMES_TX      0x040   /* Frames transmitted */
#define REG_STAT_FRAMES_DROP    0x044   /* Frames dropped (backpressure) */
#define REG_STAT_BYTES_LO       0x048
#define REG_STAT_BYTES_HI       0x04C
#define REG_STAT_DESC_COMPL     0x050
#define REG_STAT_ERRORS         0x054

/* Fault Injection */
#define REG_FAULT_INJECT        0x058
#define REG_FAULT_RATE          0x05C

/* Control register bits */
#define CTRL_RUN                (1 << 0)
#define CTRL_RESET              (1 << 1)
#define CTRL_IRQ_EN             (1 << 2)

/* Status register bits */
#define STATUS_RUNNING          (1 << 0)
#define STATUS_DESC_EMPTY       (1 << 1)
#define STATUS_ERROR            (1 << 2)

/* IRQ bits */
#define IRQ_COMPLETE            (1 << 0)
#define IRQ_ERROR               (1 << 1)
#define IRQ_NO_DESC             (1 << 2)
#define IRQ_ALL_BITS            (IRQ_COMPLETE | IRQ_ERROR | IRQ_NO_DESC)

/* Fault injection bits - simplified for frames */
#define FAULT_DROP_FRAME        (1 << 0)
#define FAULT_CORRUPT_CRC       (1 << 1)
#define FAULT_CORRUPT_DATA      (1 << 2)
#define FAULT_SKIP_SEQUENCE     (1 << 3)
#define FAULT_ALL_BITS          0x0F

/* Default values */
#define DEFAULT_FRAME_RATE      25      /* 25 fps */
#define DEFAULT_DESC_RING_SIZE  256
#define DEFAULT_IRQ_COALESCE    ((40000 << 16) | 8)  /* 8 frames or 40ms */
#define DEFAULT_FAULT_RATE      1000

/* Limits */
#define MIN_FRAME_RATE          1
#define MAX_FRAME_RATE          60
#define MIN_DESC_RING_SIZE      4
#define MAX_DESC_RING_SIZE      4096

/* Descriptor structure (must match device) */
typedef struct {
    uint32_t control;
    uint32_t length;
    uint64_t dst_addr;
    uint64_t next_desc;
    uint64_t reserved;
} __attribute__((packed)) TestSGDesc;

#define DESC_SIZE           32
#define DESC_CTRL_COMPLETED (1 << 0)
#define DESC_CTRL_EOP       (1 << 1)
#define DESC_CTRL_SOP       (1 << 2)

/* Completion structure (must match device) */
typedef struct {
    uint32_t status;
    uint32_t actual_length;
    uint64_t timestamp;
} __attribute__((packed)) TestCompletion;

#define COMPL_SIZE          16
#define COMPL_STATUS_OK     0

/* ------------------------------------------------------------------------ */
/* Test Fixture                                                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    QTestState *qts;
    QPCIBus *pcibus;
    QPCIDevice *dev;
    QPCIBar bar0;
    uint64_t bar0_addr;
    QGuestAllocator alloc;
} PhantomFPGATestState;

static uint32_t reg_read(PhantomFPGATestState *s, uint32_t offset)
{
    return qpci_io_readl(s->dev, s->bar0, offset);
}

static void reg_write(PhantomFPGATestState *s, uint32_t offset, uint32_t val)
{
    qpci_io_writel(s->dev, s->bar0, offset, val);
}

static PhantomFPGATestState *phantomfpga_test_start(void)
{
    PhantomFPGATestState *s;

    s = g_new0(PhantomFPGATestState, 1);

    /* Start QEMU with PhantomFPGA device */
    s->qts = qtest_init("-device phantomfpga");

    /* Initialize guest memory allocator for DMA buffers */
    pc_alloc_init(&s->alloc, s->qts, ALLOC_NO_FLAGS);

    /* Get PCI bus */
    s->pcibus = qpci_new_pc(s->qts, NULL);
    g_assert(s->pcibus != NULL);

    /* Find our device */
    s->dev = qpci_device_find(s->pcibus, QPCI_DEVFN(0x04, 0));
    if (!s->dev) {
        for (int slot = 0; slot < 32; slot++) {
            s->dev = qpci_device_find(s->pcibus, QPCI_DEVFN(slot, 0));
            if (s->dev) {
                uint16_t vendor = qpci_config_readw(s->dev, PCI_VENDOR_ID);
                uint16_t device = qpci_config_readw(s->dev, PCI_DEVICE_ID);
                if (vendor == PHANTOMFPGA_VENDOR_ID &&
                    device == PHANTOMFPGA_DEVICE_ID) {
                    break;
                }
                g_free(s->dev);
                s->dev = NULL;
            }
        }
    }
    g_assert(s->dev != NULL);

    /* Enable device and bus mastering for DMA */
    qpci_device_enable(s->dev);

    /* Map BAR0 */
    s->bar0 = qpci_iomap(s->dev, 0, &s->bar0_addr);

    return s;
}

static void phantomfpga_test_stop(PhantomFPGATestState *s)
{
    qpci_iounmap(s->dev, s->bar0);
    g_free(s->dev);
    qpci_free_pc(s->pcibus);
    alloc_destroy(&s->alloc);
    qtest_quit(s->qts);
    g_free(s);
}

/* ------------------------------------------------------------------------ */
/* Identification Register Tests                                            */
/* ------------------------------------------------------------------------ */

static void test_dev_id(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_DEV_ID);
    g_assert_cmpuint(val, ==, PHANTOMFPGA_DEV_ID_VAL);

    phantomfpga_test_stop(s);
}

static void test_dev_ver(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_DEV_VER);
    g_assert_cmpuint(val, ==, PHANTOMFPGA_DEV_VER);

    phantomfpga_test_stop(s);
}

static void test_dev_id_readonly(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    reg_write(s, REG_DEV_ID, 0xDEADBEEF);
    val = reg_read(s, REG_DEV_ID);
    g_assert_cmpuint(val, ==, PHANTOMFPGA_DEV_ID_VAL);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Control Register Tests                                                   */
/* ------------------------------------------------------------------------ */

static void test_ctrl_write_read(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_CTRL, CTRL_IRQ_EN);
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, CTRL_IRQ_EN);

    reg_write(s, REG_CTRL, CTRL_RUN | CTRL_IRQ_EN);
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, CTRL_RUN | CTRL_IRQ_EN);

    reg_write(s, REG_CTRL, CTRL_IRQ_EN);
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, CTRL_IRQ_EN);

    phantomfpga_test_stop(s);
}

static void test_ctrl_reset(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Configure non-default values */
    reg_write(s, REG_FRAME_RATE, 30);
    reg_write(s, REG_CTRL, CTRL_IRQ_EN);

    /* Trigger reset */
    reg_write(s, REG_CTRL, CTRL_RESET);

    /* CTRL should be 0 (RESET bit self-clears) */
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, 0);

    /* Configuration should be back to defaults */
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FRAME_RATE);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Frame Configuration Tests                                                */
/* Fixed frame size and count - simplified from v2.0                        */
/* ------------------------------------------------------------------------ */

/*
 * Test FRAME_SIZE register is fixed at 5120 and read-only
 */
static void test_frame_size(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Should always be FRAME_SIZE */
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, FRAME_SIZE);

    /* Try to write (should be ignored - read only) */
    reg_write(s, REG_FRAME_SIZE, 9999);
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, FRAME_SIZE);

    phantomfpga_test_stop(s);
}

/*
 * Test FRAME_COUNT register is fixed at 250 and read-only
 */
static void test_frame_count(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Should always be FRAME_COUNT */
    val = reg_read(s, REG_FRAME_COUNT);
    g_assert_cmpuint(val, ==, FRAME_COUNT);

    /* Try to write (should be ignored - read only) */
    reg_write(s, REG_FRAME_COUNT, 9999);
    val = reg_read(s, REG_FRAME_COUNT);
    g_assert_cmpuint(val, ==, FRAME_COUNT);

    phantomfpga_test_stop(s);
}

/*
 * Test FRAME_RATE register write/read
 */
static void test_frame_rate(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FRAME_RATE);

    /* Set valid value */
    reg_write(s, REG_FRAME_RATE, 30);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, 30);

    /* Minimum */
    reg_write(s, REG_FRAME_RATE, MIN_FRAME_RATE);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, MIN_FRAME_RATE);

    /* Maximum */
    reg_write(s, REG_FRAME_RATE, MAX_FRAME_RATE);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, MAX_FRAME_RATE);

    phantomfpga_test_stop(s);
}

/*
 * Test FRAME_RATE clamping
 */
static void test_frame_rate_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Value below minimum */
    reg_write(s, REG_FRAME_RATE, 0);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, MIN_FRAME_RATE);

    /* Value above maximum */
    reg_write(s, REG_FRAME_RATE, MAX_FRAME_RATE + 100);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, MAX_FRAME_RATE);

    phantomfpga_test_stop(s);
}

/*
 * Test CURRENT_FRAME register is initially 0 and read-only
 */
static void test_current_frame(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially 0 */
    val = reg_read(s, REG_CURRENT_FRAME);
    g_assert_cmpuint(val, ==, 0);

    /* Try to write (should be ignored - read only) */
    reg_write(s, REG_CURRENT_FRAME, 42);
    val = reg_read(s, REG_CURRENT_FRAME);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Descriptor Ring Configuration Tests                                      */
/* ------------------------------------------------------------------------ */

static void test_desc_ring_addr(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t lo, hi;

    lo = reg_read(s, REG_DESC_RING_LO);
    hi = reg_read(s, REG_DESC_RING_HI);
    g_assert_cmpuint(lo, ==, 0);
    g_assert_cmpuint(hi, ==, 0);

    reg_write(s, REG_DESC_RING_LO, 0xDEADBEEF);
    lo = reg_read(s, REG_DESC_RING_LO);
    g_assert_cmpuint(lo, ==, 0xDEADBEEF);

    reg_write(s, REG_DESC_RING_HI, 0xCAFEBABE);
    hi = reg_read(s, REG_DESC_RING_HI);
    g_assert_cmpuint(hi, ==, 0xCAFEBABE);

    lo = reg_read(s, REG_DESC_RING_LO);
    hi = reg_read(s, REG_DESC_RING_HI);
    g_assert_cmpuint(lo, ==, 0xDEADBEEF);
    g_assert_cmpuint(hi, ==, 0xCAFEBABE);

    phantomfpga_test_stop(s);
}

static void test_desc_ring_size(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_DESC_RING_SIZE);

    reg_write(s, REG_DESC_RING_SIZE, 128);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, 128);

    /* Non-power-of-2 should be rounded down */
    reg_write(s, REG_DESC_RING_SIZE, 100);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, 64);

    reg_write(s, REG_DESC_RING_SIZE, 200);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, 128);

    phantomfpga_test_stop(s);
}

static void test_desc_ring_size_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    reg_write(s, REG_DESC_RING_SIZE, 1);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, MIN_DESC_RING_SIZE);

    reg_write(s, REG_DESC_RING_SIZE, MAX_DESC_RING_SIZE + 1000);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, MAX_DESC_RING_SIZE);

    phantomfpga_test_stop(s);
}

static void test_desc_head(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_DESC_HEAD);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_DESC_HEAD, 10);
    val = reg_read(s, REG_DESC_HEAD);
    g_assert_cmpuint(val, ==, 10);

    phantomfpga_test_stop(s);
}

static void test_desc_head_wrapping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val, ring_size;

    ring_size = reg_read(s, REG_DESC_RING_SIZE);

    reg_write(s, REG_DESC_HEAD, ring_size + 5);
    val = reg_read(s, REG_DESC_HEAD);
    g_assert_cmpuint(val, ==, 5);

    phantomfpga_test_stop(s);
}

static void test_desc_tail(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_DESC_TAIL);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_DESC_TAIL, 42);
    val = reg_read(s, REG_DESC_TAIL);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* IRQ Register Tests                                                       */
/* ------------------------------------------------------------------------ */

static void test_irq_mask(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_IRQ_MASK, IRQ_COMPLETE);
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, IRQ_COMPLETE);

    reg_write(s, REG_IRQ_MASK, IRQ_ALL_BITS);
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, IRQ_ALL_BITS);

    reg_write(s, REG_IRQ_MASK, 0xFFFFFFFF);
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, IRQ_ALL_BITS);

    phantomfpga_test_stop(s);
}

static void test_irq_status_initial(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_IRQ_STATUS);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

static void test_irq_status_w1c(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    reg_write(s, REG_IRQ_STATUS, IRQ_COMPLETE);
    val = reg_read(s, REG_IRQ_STATUS);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

static void test_irq_coalesce(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_IRQ_COALESCE);
    g_assert_cmpuint(val, ==, DEFAULT_IRQ_COALESCE);

    /* Set custom value: 16 frames, 20ms timeout */
    reg_write(s, REG_IRQ_COALESCE, (20000 << 16) | 16);
    val = reg_read(s, REG_IRQ_COALESCE);
    g_assert_cmpuint(val, ==, (20000 << 16) | 16);

    reg_write(s, REG_IRQ_COALESCE, 0xFFFFFFFF);
    val = reg_read(s, REG_IRQ_COALESCE);
    g_assert_cmpuint(val, ==, 0xFFFFFFFF);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Statistics Register Tests                                                */
/* ------------------------------------------------------------------------ */

static void test_stats_initial(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_STAT_FRAMES_TX);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_FRAMES_DROP);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_BYTES_LO);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_BYTES_HI);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_ERRORS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_DESC_COMPL);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

static void test_stats_readonly(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    reg_write(s, REG_STAT_FRAMES_TX, 999);
    val = reg_read(s, REG_STAT_FRAMES_TX);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_STAT_FRAMES_DROP, 999);
    val = reg_read(s, REG_STAT_FRAMES_DROP);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_STAT_ERRORS, 999);
    val = reg_read(s, REG_STAT_ERRORS);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_STAT_DESC_COMPL, 999);
    val = reg_read(s, REG_STAT_DESC_COMPL);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Fault Injection Tests                                                    */
/* Simplified for frame-based streaming                                     */
/* ------------------------------------------------------------------------ */

static void test_fault_inject(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_FAULT_INJECT, FAULT_DROP_FRAME);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, FAULT_DROP_FRAME);

    reg_write(s, REG_FAULT_INJECT, FAULT_CORRUPT_CRC | FAULT_CORRUPT_DATA);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, FAULT_CORRUPT_CRC | FAULT_CORRUPT_DATA);

    reg_write(s, REG_FAULT_INJECT, FAULT_ALL_BITS);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, FAULT_ALL_BITS);

    reg_write(s, REG_FAULT_INJECT, 0xFFFFFFFF);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, FAULT_ALL_BITS);

    phantomfpga_test_stop(s);
}

static void test_fault_rate(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FAULT_RATE);

    /* Set aggressive rate (10% of frames) */
    reg_write(s, REG_FAULT_RATE, 10);
    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, 10);

    /* Set conservative rate (0.01% of frames) */
    reg_write(s, REG_FAULT_RATE, 10000);
    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, 10000);

    /* Set to 0 (effectively disables faults) */
    reg_write(s, REG_FAULT_RATE, 0);
    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Status Register Tests                                                    */
/* ------------------------------------------------------------------------ */

static void test_status_initial(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_STATUS, 0xFF);
    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

static void test_status_running(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, 0);

    reg_write(s, REG_CTRL, CTRL_RUN);

    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, STATUS_RUNNING);

    reg_write(s, REG_CTRL, 0);

    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, 0);

    phantomfpga_test_stop(s);
}

static void test_status_desc_empty(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    reg_write(s, REG_DESC_RING_LO, 0x10000000);
    reg_write(s, REG_DESC_RING_HI, 0);
    reg_write(s, REG_DESC_RING_SIZE, 256);

    reg_write(s, REG_CTRL, CTRL_RUN);

    qtest_clock_step_next(s->qts);

    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_DESC_EMPTY, ==, STATUS_DESC_EMPTY);

    reg_write(s, REG_CTRL, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Frame Production Tests                                                   */
/* ------------------------------------------------------------------------ */

static void test_frame_production(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t tail_before, tail_after;
    uint32_t stat_frames_before, stat_frames_after;
    const int num_descs = 16;
    const uint32_t buf_size = FRAME_SIZE + COMPL_SIZE;
    uint64_t desc_ring_addr;
    uint64_t buf_addrs[16];
    TestSGDesc desc;
    int i;

    /* Allocate descriptor ring in guest memory */
    desc_ring_addr = guest_alloc(&s->alloc, num_descs * DESC_SIZE);

    /* Allocate frame buffers and initialize descriptors */
    for (i = 0; i < num_descs; i++) {
        buf_addrs[i] = guest_alloc(&s->alloc, buf_size);

        memset(&desc, 0, sizeof(desc));
        desc.control = 0;
        desc.length = buf_size;
        desc.dst_addr = buf_addrs[i];
        desc.next_desc = 0;
        desc.reserved = 0;

        qtest_memwrite(s->qts, desc_ring_addr + i * DESC_SIZE,
                       &desc, sizeof(desc));
    }

    /* Configure descriptor ring */
    reg_write(s, REG_DESC_RING_LO, (uint32_t)(desc_ring_addr & 0xFFFFFFFF));
    reg_write(s, REG_DESC_RING_HI, (uint32_t)(desc_ring_addr >> 32));
    reg_write(s, REG_DESC_RING_SIZE, num_descs);

    /* Submit descriptors */
    reg_write(s, REG_DESC_HEAD, 8);

    /* Set high frame rate for fast testing */
    reg_write(s, REG_FRAME_RATE, 60);

    tail_before = reg_read(s, REG_DESC_TAIL);
    stat_frames_before = reg_read(s, REG_STAT_FRAMES_TX);

    /* Start the device */
    reg_write(s, REG_CTRL, CTRL_RUN);

    /* Advance the clock to produce frames */
    qtest_clock_step(s->qts, 200 * 1000 * 1000);  /* 200ms */

    tail_after = reg_read(s, REG_DESC_TAIL);
    stat_frames_after = reg_read(s, REG_STAT_FRAMES_TX);

    /* Stop the device */
    reg_write(s, REG_CTRL, 0);

    /* Descriptor tail should have advanced */
    g_assert_cmpuint(tail_after, >, tail_before);

    /* Frame counter should have increased */
    g_assert_cmpuint(stat_frames_after, >, stat_frames_before);

    /* Verify first descriptor was marked completed */
    qtest_memread(s->qts, desc_ring_addr, &desc, sizeof(desc));
    g_assert_cmpuint(desc.control & DESC_CTRL_COMPLETED, ==, DESC_CTRL_COMPLETED);

    /* Clean up */
    for (i = 0; i < num_descs; i++) {
        guest_free(&s->alloc, buf_addrs[i]);
    }
    guest_free(&s->alloc, desc_ring_addr);

    phantomfpga_test_stop(s);
}

static void test_no_frames_without_descriptors(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t tail_before, tail_after;
    const int num_descs = 16;
    uint64_t desc_ring_addr;

    desc_ring_addr = guest_alloc(&s->alloc, num_descs * DESC_SIZE);

    reg_write(s, REG_DESC_RING_LO, (uint32_t)(desc_ring_addr & 0xFFFFFFFF));
    reg_write(s, REG_DESC_RING_HI, (uint32_t)(desc_ring_addr >> 32));
    reg_write(s, REG_DESC_RING_SIZE, num_descs);
    /* HEAD is 0, TAIL is 0 - no descriptors available */

    reg_write(s, REG_FRAME_RATE, 60);

    tail_before = reg_read(s, REG_DESC_TAIL);

    reg_write(s, REG_CTRL, CTRL_RUN);

    qtest_clock_step(s->qts, 100 * 1000 * 1000);

    tail_after = reg_read(s, REG_DESC_TAIL);

    /* Tail should NOT have advanced (no descriptors submitted) */
    g_assert_cmpuint(tail_after, ==, tail_before);

    /* DESC_EMPTY status should be set */
    uint32_t status = reg_read(s, REG_STATUS);
    g_assert_cmpuint(status & STATUS_DESC_EMPTY, ==, STATUS_DESC_EMPTY);

    /* Frames should have been dropped */
    uint32_t dropped = reg_read(s, REG_STAT_FRAMES_DROP);
    g_assert_cmpuint(dropped, >, 0);

    reg_write(s, REG_CTRL, CTRL_RESET);

    guest_free(&s->alloc, desc_ring_addr);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Reset Behavior Tests                                                     */
/* ------------------------------------------------------------------------ */

static void test_full_reset(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Configure everything to non-default values */
    reg_write(s, REG_FRAME_RATE, 30);
    reg_write(s, REG_DESC_RING_LO, 0xAAAAAAAA);
    reg_write(s, REG_DESC_RING_HI, 0xBBBBBBBB);
    reg_write(s, REG_DESC_RING_SIZE, 512);
    reg_write(s, REG_DESC_HEAD, 50);
    reg_write(s, REG_IRQ_MASK, IRQ_ALL_BITS);
    reg_write(s, REG_IRQ_COALESCE, (2000 << 16) | 64);
    reg_write(s, REG_FAULT_INJECT, FAULT_ALL_BITS);
    reg_write(s, REG_FAULT_RATE, 10);
    reg_write(s, REG_CTRL, CTRL_IRQ_EN);

    /* Trigger reset */
    reg_write(s, REG_CTRL, CTRL_RESET);

    /* Everything should be back to defaults */
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FRAME_RATE);

    val = reg_read(s, REG_DESC_RING_LO);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_DESC_RING_HI);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_DESC_RING_SIZE);

    val = reg_read(s, REG_DESC_HEAD);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_DESC_TAIL);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_IRQ_STATUS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_IRQ_COALESCE);
    g_assert_cmpuint(val, ==, DEFAULT_IRQ_COALESCE);

    val = reg_read(s, REG_STAT_FRAMES_TX);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_FRAMES_DROP);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_ERRORS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_DESC_COMPL);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FAULT_RATE);

    /* Read-only frame registers should be unchanged */
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, FRAME_SIZE);

    val = reg_read(s, REG_FRAME_COUNT);
    g_assert_cmpuint(val, ==, FRAME_COUNT);

    phantomfpga_test_stop(s);
}

static void test_reset_stops_device(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    reg_write(s, REG_DESC_RING_LO, 0x10000000);
    reg_write(s, REG_DESC_RING_SIZE, 256);
    reg_write(s, REG_DESC_HEAD, 10);
    reg_write(s, REG_CTRL, CTRL_RUN);

    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, STATUS_RUNNING);

    reg_write(s, REG_CTRL, CTRL_RESET);

    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* PCI Configuration Tests                                                  */
/* ------------------------------------------------------------------------ */

static void test_pci_ids(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint16_t vendor, device;

    vendor = qpci_config_readw(s->dev, PCI_VENDOR_ID);
    device = qpci_config_readw(s->dev, PCI_DEVICE_ID);

    g_assert_cmpuint(vendor, ==, PHANTOMFPGA_VENDOR_ID);
    g_assert_cmpuint(device, ==, PHANTOMFPGA_DEVICE_ID);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Test Registration                                                        */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    /* Identification tests */
    qtest_add_func("/phantomfpga/dev_id", test_dev_id);
    qtest_add_func("/phantomfpga/dev_ver", test_dev_ver);
    qtest_add_func("/phantomfpga/dev_id_readonly", test_dev_id_readonly);
    qtest_add_func("/phantomfpga/pci_ids", test_pci_ids);

    /* Control register tests */
    qtest_add_func("/phantomfpga/ctrl_write_read", test_ctrl_write_read);
    qtest_add_func("/phantomfpga/ctrl_reset", test_ctrl_reset);

    /* Frame configuration tests */
    qtest_add_func("/phantomfpga/frame_size", test_frame_size);
    qtest_add_func("/phantomfpga/frame_count", test_frame_count);
    qtest_add_func("/phantomfpga/frame_rate", test_frame_rate);
    qtest_add_func("/phantomfpga/frame_rate_clamping", test_frame_rate_clamping);
    qtest_add_func("/phantomfpga/current_frame", test_current_frame);

    /* Descriptor ring configuration tests */
    qtest_add_func("/phantomfpga/desc_ring_addr", test_desc_ring_addr);
    qtest_add_func("/phantomfpga/desc_ring_size", test_desc_ring_size);
    qtest_add_func("/phantomfpga/desc_ring_size_clamping", test_desc_ring_size_clamping);
    qtest_add_func("/phantomfpga/desc_head", test_desc_head);
    qtest_add_func("/phantomfpga/desc_head_wrapping", test_desc_head_wrapping);
    qtest_add_func("/phantomfpga/desc_tail", test_desc_tail);

    /* IRQ tests */
    qtest_add_func("/phantomfpga/irq_mask", test_irq_mask);
    qtest_add_func("/phantomfpga/irq_status_initial", test_irq_status_initial);
    qtest_add_func("/phantomfpga/irq_status_w1c", test_irq_status_w1c);
    qtest_add_func("/phantomfpga/irq_coalesce", test_irq_coalesce);

    /* Statistics tests */
    qtest_add_func("/phantomfpga/stats_initial", test_stats_initial);
    qtest_add_func("/phantomfpga/stats_readonly", test_stats_readonly);

    /* Fault injection tests */
    qtest_add_func("/phantomfpga/fault_inject", test_fault_inject);
    qtest_add_func("/phantomfpga/fault_rate", test_fault_rate);

    /* Status tests */
    qtest_add_func("/phantomfpga/status_initial", test_status_initial);
    qtest_add_func("/phantomfpga/status_running", test_status_running);
    qtest_add_func("/phantomfpga/status_desc_empty", test_status_desc_empty);

    /* Frame production tests */
    qtest_add_func("/phantomfpga/frame_production", test_frame_production);
    qtest_add_func("/phantomfpga/no_frames_without_descriptors",
                   test_no_frames_without_descriptors);

    /* Reset tests */
    qtest_add_func("/phantomfpga/full_reset", test_full_reset);
    qtest_add_func("/phantomfpga/reset_stops_device", test_reset_stops_device);

    return g_test_run();
}
