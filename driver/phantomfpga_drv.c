// SPDX-License-Identifier: GPL-2.0
/*
 * PhantomFPGA PCIe Driver v3.0
 *
 * A Linux kernel driver for the PhantomFPGA virtual PCI device.
 * This skeleton provides the structure - you fill in the TODOs!
 *
 * v3.0 streams pre-built data frames via scatter-gather DMA.
 * Build a driver, stream frames over TCP, display them on the host.
 *
 * Learning objectives:
 *   - PCI device probing and BAR mapping
 *   - Scatter-gather DMA with descriptor rings
 *   - Coherent DMA buffer management
 *   - MSI-X interrupt handling with coalescing
 *   - Character device file operations
 *   - IOCTL interface design
 *   - Memory mapping with mmap()
 *   - CRC32 validation
 *
 * Copyright (C) 2026 PhantomFPGA Project
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/crc32.h>
#include <linux/delay.h>

#include "phantomfpga_regs.h"
#include "phantomfpga_uapi.h"

/* Module metadata */
MODULE_AUTHOR("Me");
MODULE_DESCRIPTION("PhantomFPGA v3.0 Frame Streaming Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("3.0");

/* Driver constants */
#define DRIVER_NAME             "phantomfpga"
#define PHANTOMFPGA_MAX_DEVICES 4

/* Buffer size for frames (frame size + completion writeback) */
#define PHANTOMFPGA_BUFFER_SIZE (PHANTOMFPGA_FRAME_SIZE + PHANTOMFPGA_COMPL_SIZE)

/* ------------------------------------------------------------------------ */
/* Descriptor Buffer Entry                                                  */
/* One of these per descriptor in the ring                                  */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_buffer - Per-descriptor buffer tracking
 *
 * Each descriptor needs a DMA buffer for the device to write frame data.
 * We track both the virtual address (for driver/userspace) and DMA address
 * (for the device).
 */
struct phantomfpga_buffer {
	void *vaddr;              /* Kernel virtual address */
	dma_addr_t dma_addr;      /* DMA address for device */
	size_t size;              /* Buffer size in bytes */
};

/* ------------------------------------------------------------------------ */
/* Device Private Data Structure                                            */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_dev - Per-device private data
 *
 * This structure holds all state for a single PhantomFPGA device.
 * One instance is allocated per PCI device in probe().
 *
 * v3.0 changes: Simplified for fixed-size frame streaming.
 * No more variable packet sizes or header profiles.
 */
struct phantomfpga_dev {
	/* PCI device reference */
	struct pci_dev *pdev;

	/* BAR0 register mapping */
	void __iomem *regs;         /* Kernel virtual address of BAR0 */
	resource_size_t regs_start; /* Physical address of BAR0 */
	resource_size_t regs_len;   /* Length of BAR0 region */

	/* Descriptor ring (SG-DMA) */
	struct phantomfpga_sg_desc *desc_ring;  /* Descriptor ring virtual addr */
	dma_addr_t desc_ring_dma;               /* Descriptor ring DMA addr */
	u32 desc_count;                         /* Number of descriptors */

	/* Per-descriptor buffers */
	struct phantomfpga_buffer *buffers;     /* Array of buffer tracking */
	size_t buffer_size;                     /* Size of each buffer */

	/* Contiguous DMA buffer pool for mmap */
	void *buffer_pool;                      /* Kernel virtual address of all buffers */
	dma_addr_t buffer_pool_dma;             /* DMA address of buffer pool */
	size_t buffer_pool_size;                /* Total size of buffer pool */

	/* Configuration */
	u32 frame_rate;             /* Frames per second (1-60) */
	u16 irq_coalesce_count;     /* IRQ after N completions */
	u16 irq_coalesce_timeout;   /* IRQ timeout in microseconds */
	bool configured;            /* Has SET_CFG been called? */
	bool streaming;             /* Is device currently streaming? */

	/* Ring indices (driver-side shadow) */
	u32 desc_head;              /* Head: driver writes to submit */
	u32 desc_tail;              /* Tail: device updates on completion */
	u32 shadow_tail;            /* Completion pointer (set by IRQ handler) */
	u32 consumer;               /* Consumer pointer (advanced by read/ioctl) */

	/* Statistics (driver-side) */
	u64 frames_consumed;
	u64 bytes_consumed;
	u32 irq_count;
	u32 crc_errors;

	/* Synchronization */
	spinlock_t lock;            /* Protects indices and state */
	struct mutex ioctl_lock;    /* Serializes ioctl operations */
	wait_queue_head_t wait_queue; /* For poll/blocking read */

	/* Character device */
	struct cdev cdev;
	dev_t devno;
	struct device *dev;         /* sysfs device */
	int minor;

	/* MSI-X vectors */
	int num_vectors;
	int irq_complete;           /* IRQ number for completion vector */
	int irq_error;              /* IRQ number for error vector */
	int irq_no_desc;            /* IRQ number for no-descriptor vector */
};

/* ------------------------------------------------------------------------ */
/* Global Driver State                                                      */
/* ------------------------------------------------------------------------ */

static struct class *phantomfpga_class;
static dev_t phantomfpga_devno;
static DEFINE_IDA(phantomfpga_ida);  /* Minor number allocator */

/* PCI device ID table */
static const struct pci_device_id phantomfpga_pci_ids[] = {
	{ PCI_DEVICE(PHANTOMFPGA_VENDOR_ID, PHANTOMFPGA_DEVICE_ID) },
	{ 0, }  /* Terminator */
};
MODULE_DEVICE_TABLE(pci, phantomfpga_pci_ids);

/* ------------------------------------------------------------------------ */
/* Register Access Helpers                                                  */
/* These helpers provide type-safe register access                          */
/* ------------------------------------------------------------------------ */

static inline u32 pfpga_read32(struct phantomfpga_dev *pfdev, u32 offset)
{
	return ioread32(pfdev->regs + offset);
}

static inline void pfpga_write32(struct phantomfpga_dev *pfdev, u32 offset, u32 val)
{
	iowrite32(val, pfdev->regs + offset);
}

/* ------------------------------------------------------------------------ */
/* CRC32 Verification                                                       */
/* Trust but verify - validate frame CRCs                                   */
/* ------------------------------------------------------------------------ */

/*
 * Compute CRC32 for a data buffer.
 *
 * Uses the Linux kernel's crc32_le() function with the IEEE 802.3 polynomial.
 * The device uses the same polynomial, so results should match.
 */
static inline u32 pfpga_compute_crc32(const void *data, size_t len)
{
	return crc32_le(~0, data, len) ^ ~0;
}

/*
 * Validate CRC32 of a received frame.
 * Returns true if CRC matches, false otherwise.
 */
static inline bool pfpga_validate_frame_crc(const void *frame)
{
	u32 computed_crc = pfpga_compute_crc32(frame, PHANTOMFPGA_FRAME_CRC_OFFSET);
	u32 stored_crc = le32_to_cpu(*phantomfpga_frame_crc_ptr((void *)frame));
	return computed_crc == stored_crc;
}

static int pfpga_alloc_descriptors(struct phantomfpga_dev *pfdev,
				   u32 desc_count, size_t buffer_size);
/* ------------------------------------------------------------------------ */
/* Hardware Operations - SG-DMA for Frame Streaming                         */
/* ------------------------------------------------------------------------ */

/*
 * Configure the descriptor ring address in the device.
 * Called after descriptor ring allocation.
 */
static void __maybe_unused pfpga_configure_desc_ring(struct phantomfpga_dev *pfdev)
{
	/*
	 * Program the 64-bit DMA address of the descriptor ring into
	 * two 32-bit hardware registers.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_RING_LO,
		      lower_32_bits(pfdev->desc_ring_dma));
	pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_RING_HI,
		      upper_32_bits(pfdev->desc_ring_dma));

	/*
	 * Tell the device how many descriptors exist in the ring.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_RING_SIZE,
		      pfdev->desc_count);

	/*
	 * Reset driver-side ring tracking before descriptors are submitted.
	 */
	pfdev->desc_head = 0;
	pfdev->desc_tail = 0;
	pfdev->shadow_tail = 0;
	pfdev->consumer = 0;

	/*
	 * Hardware head is writable by the driver.
	 * Hardware tail is read-only/device-owned, so we do not write DESC_TAIL.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_HEAD, 0);
}

/*
 * Apply frame streaming configuration to device registers.
 * Called from SET_CFG ioctl after validation.
 */
static void __maybe_unused pfpga_apply_config(struct phantomfpga_dev *pfdev)
{
	u32 irq_coalesce;

	/*
	 * The frame rate register controls how many frames per second
	 * the PhantomFPGA device should generate.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_FRAME_RATE, pfdev->frame_rate);

	/*
	 * The IRQ coalescing register packs two values into one 32-bit register:
	 *   - irq_coalesce_count
	 *   - irq_coalesce_timeout
	 *
	 * The helper knows the correct bit layout, so we do not shift manually here.
	 */
	irq_coalesce = phantomfpga_irq_coalesce_pack(
		pfdev->irq_coalesce_count,
		pfdev->irq_coalesce_timeout);

	pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_COALESCE, irq_coalesce);

	/*
	 * Enable all PhantomFPGA interrupt sources in the IRQ mask register.
	 * This prepares the device for later interrupt-driven streaming.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_MASK, PHANTOMFPGA_IRQ_ALL);
}

/*
 * Submit descriptors to the device.
 *
 * After populating descriptors with buffer addresses, write the new
 * head index to tell the device how many are available.
 */
static void __maybe_unused pfpga_submit_descriptors(struct phantomfpga_dev *pfdev, u32 count)
{
	if (!pfdev->desc_count || !count)
		return;

	/*
	 * Make sure all descriptor memory writes are visible before
	 * we notify the device by updating the hardware head register.
	 */
	wmb();

	/*
	 * Descriptor count is validated as a power of two during SET_CFG,
	 * so this mask wraps the ring index efficiently.
	 */
	pfdev->desc_head =
		(pfdev->desc_head + count) & (pfdev->desc_count - 1);

	/*
	 * Tell the PhantomFPGA device where the new producer/head index is.
	 * After this write, hardware can start consuming descriptors.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_HEAD, pfdev->desc_head);
}

/*
 * Initialize all descriptors with buffer addresses.
 * Called once after buffer allocation.
 */
static void __maybe_unused pfpga_init_descriptors(struct phantomfpga_dev *pfdev)
{
	u32 i;

	if (!pfdev->desc_ring || !pfdev->buffers || !pfdev->desc_count)
		return;

	for (i = 0; i < pfdev->desc_count; i++) {
		/*
		 * control starts at 0 because this descriptor is free/available.
		 * The device will later set PHANTOMFPGA_DESC_CTRL_COMPLETED
		 * when it finishes writing a frame into this buffer.
		 */
		pfdev->desc_ring[i].control = cpu_to_le32(0);

		/*
		 * length tells the device how many bytes it may write into
		 * this descriptor's destination buffer.
		 */
		pfdev->desc_ring[i].length =
			cpu_to_le32(pfdev->buffer_size);

		/*
		 * dst_addr is the DMA/bus address of buffer i.
		 * The FPGA device uses this address, not the kernel virtual address.
		 */
		pfdev->desc_ring[i].dst_addr =
			cpu_to_le64(pfdev->buffers[i].dma_addr);

		/*
		 * We use hardware ring mode through DESC_HEAD/DESC_TAIL,
		 * so descriptors do not need linked-list chaining.
		 */
		pfdev->desc_ring[i].next_desc = cpu_to_le64(0);

		/*
		 * Reserved field must be zero for clean hardware/device ABI behavior.
		 */
		pfdev->desc_ring[i].reserved = cpu_to_le64(0);
	}

	/*
	 * Submit all but one descriptor.
	 * Leaving one slot empty is the classic ring-buffer trick that lets
	 * head == tail mean empty instead of being ambiguous with full.
	 */
	pfpga_submit_descriptors(pfdev, pfdev->desc_count - 1);
}

/*
 * Start frame streaming.
 */
static int pfpga_start_streaming(struct phantomfpga_dev *pfdev)
{
	if (!pfdev->configured)
		return -EINVAL;

	if (pfdev->streaming)
		return -EBUSY;

	/*
	 * Reset the driver's ring indexes before starting a fresh stream.
	 * desc_head is where the driver submits descriptors.
	 * desc_tail/shadow_tail track what the device completed.
	 * consumer tracks what userspace consumed.
	 */
	pfdev->desc_head = 0;
	pfdev->desc_tail = 0;
	pfdev->shadow_tail = 0;
	pfdev->consumer = 0;

	/*
	 * Reset the hardware-visible descriptor head.
	 * The tail register is device-owned/read-only from the driver's view,
	 * so we reset the driver's local tail state and let hardware update
	 * PHANTOMFPGA_REG_DESC_TAIL as descriptors complete.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_HEAD, 0);

	/*
	 * Rebuild the descriptor ring and submit available descriptors.
	 * pfpga_init_descriptors() clears descriptor control fields,
	 * fills buffer addresses, and submits desc_count - 1 descriptors.
	 */
	pfpga_init_descriptors(pfdev);

	/*
	 * Clear old pending interrupt status before enabling streaming.
	 * PHANTOMFPGA_REG_IRQ_STATUS is W1C, meaning writing 1 clears bits.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS, PHANTOMFPGA_IRQ_ALL);

	/*
	 * Start the device and enable global PhantomFPGA interrupts.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_CTRL,
		      PHANTOMFPGA_CTRL_RUN | PHANTOMFPGA_CTRL_IRQ_EN);

	pfdev->streaming = true;

	return 0;
}
/*
 * Stop frame streaming.
 */
static int pfpga_stop_streaming(struct phantomfpga_dev *pfdev)
{
	u32 ctrl;

	/*
	 * Read the current control register so we only modify the RUN bit
	 * and preserve any other control bits that may already be set.
	 */
	ctrl = pfpga_read32(pfdev, PHANTOMFPGA_REG_CTRL);

	/*
	 * Clear the RUN bit. This tells the PhantomFPGA device to stop
	 * producing frames.
	 */
	ctrl &= ~PHANTOMFPGA_CTRL_RUN;

	/*
	 * Write the updated control value back to hardware.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_CTRL, ctrl);

	/*
	 * Update driver state so future SET_CFG calls are allowed again.
	 */
	pfdev->streaming = false;

	/*
	 * Wake readers or poll waiters. If userspace is blocked waiting
	 * for frames, it should re-check streaming state and exit cleanly.
	 */
	wake_up_interruptible(&pfdev->wait_queue);

	return 0;
}

/*
 * Perform soft reset of the device.
 */
static void pfpga_soft_reset(struct phantomfpga_dev *pfdev)
{
	/*
	 * Ask the PhantomFPGA device to reset its internal state.
	 * QEMU handles this by clearing CTRL/STATUS/IRQ state, descriptor
	 * head/tail indices, frame sequence state, and device statistics.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_CTRL, PHANTOMFPGA_CTRL_RESET);

	/*
	 * The reset bit is self-clearing. Give the device a short moment to
	 * complete the reset before the driver continues programming registers.
	 */
	udelay(10);

	/*
	 * Keep the driver's software view in sync with the freshly-reset
	 * device state.
	 */
	pfdev->streaming = false;
	pfdev->desc_head = 0;
	pfdev->desc_tail = 0;
	pfdev->shadow_tail = 0;
	pfdev->consumer = 0;
}

/* ------------------------------------------------------------------------ */
/* Interrupt Handlers                                                       */
/* The device is trying to tell you something                               */
/* ------------------------------------------------------------------------ */

/*
 * MSI-X interrupt handler for completion (vector 0).
 *
 * Called when descriptors complete (IRQ coalescing thresholds met).
 * The driver should process completed descriptors and wake up waiters.
 */
static irqreturn_t __maybe_unused pfpga_irq_complete(int irq, void *data)
{
	struct phantomfpga_dev *pfdev = data;
	unsigned long flags;
	u32 irq_status;
	u32 tail;

	(void)irq;

	irq_status = pfpga_read32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS);

	if (!(irq_status & PHANTOMFPGA_IRQ_COMPLETE))
		return IRQ_NONE;

	/*
	 * Clear the completion interrupt. IRQ_STATUS is write-1-to-clear.
	 */
	pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS,
		      PHANTOMFPGA_IRQ_COMPLETE);

	/*
	 * Read the device completion pointer. This tells us how far the
	 * hardware has completed descriptors.
	 */
	tail = pfpga_read32(pfdev, PHANTOMFPGA_REG_DESC_TAIL) &
	       (pfdev->desc_count - 1);

	spin_lock_irqsave(&pfdev->lock, flags);
	pfdev->shadow_tail = tail;
	pfdev->desc_tail = tail;
	pfdev->irq_count++;
	spin_unlock_irqrestore(&pfdev->lock, flags);

	/*
	 * Wake userspace readers/pollers. pfpga_read() and pfpga_poll()
	 * can now see consumer != shadow_tail.
	 */
	wake_up_interruptible(&pfdev->wait_queue);

	return IRQ_HANDLED;
}

/*
 * MSI-X interrupt handler for errors (vector 1).
 *
 * Called on error conditions (DMA error, device error).
 */
static irqreturn_t __maybe_unused pfpga_irq_error(int irq, void *data)
{
	struct phantomfpga_dev *pfdev = data;
	u32 irq_status;

	(void)irq;

	irq_status = pfpga_read32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS);

	if (!(irq_status & PHANTOMFPGA_IRQ_ERROR))
		return IRQ_NONE;

	pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS,
		      PHANTOMFPGA_IRQ_ERROR);

	dev_warn(&pfdev->pdev->dev,
		 "error interrupt: status=0x%08x\n", irq_status);

	wake_up_interruptible(&pfdev->wait_queue);

	return IRQ_HANDLED;
}

/*
 * MSI-X interrupt handler for no-descriptor condition (vector 2).
 *
 * Called when device has frames to send but no descriptors available.
 * This means backpressure - consumer isn't keeping up with frame rate.
 */
static irqreturn_t __maybe_unused pfpga_irq_no_desc(int irq, void *data)
{
	struct phantomfpga_dev *pfdev = data;
	u32 irq_status;

	(void)irq;

	irq_status = pfpga_read32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS);

	if (!(irq_status & PHANTOMFPGA_IRQ_NO_DESC))
		return IRQ_NONE;

	pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS,
		      PHANTOMFPGA_IRQ_NO_DESC);

	dev_dbg(&pfdev->pdev->dev, "no descriptors available\n");

	wake_up_interruptible(&pfdev->wait_queue);

	return IRQ_HANDLED;
}


/* ------------------------------------------------------------------------ */
/* File Operations                                                          */
/* The gateway between userspace dreams and kernel reality                  */
/* ------------------------------------------------------------------------ */

/*
 * Open the device file.
 */
static int pfpga_open(struct inode *inode, struct file *file)
{
	struct phantomfpga_dev *pfdev;

	pfdev = container_of(inode->i_cdev, struct phantomfpga_dev, cdev);
	file->private_data = pfdev;

	dev_dbg(&pfdev->pdev->dev, "device opened\n");
	return 0;
}

/*
 * Close the device file.
 */
static int pfpga_release(struct inode *inode, struct file *file)
{
	struct phantomfpga_dev *pfdev = file->private_data;

	/*
	 * Closing the device file does not automatically stop streaming.
	 * Streaming is controlled explicitly through PHANTOMFPGA_IOCTL_START
	 * and PHANTOMFPGA_IOCTL_STOP, while release only drops this file handle.
	 */

	dev_dbg(&pfdev->pdev->dev, "device closed\n");
	return 0;
}

/*
 * Read frames from the device.
 *
 * In SG-DMA mode, this reads completed frame data from descriptor buffers
 * and copies to userspace.
 */
static ssize_t pfpga_read(struct file *file, char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	unsigned long flags;
	u32 head, tail, pending;
	struct phantomfpga_sg_desc *desc;
	struct phantomfpga_completion *compl;
	void *buffer;
	size_t to_copy;
	int ret;

	/*
	 * Read one completed frame from the SG-DMA ring.
	 *
	 * The hardware DESC_TAIL register tells the driver how far the device
	 * has completed descriptors. The driver-side consumer index tells us
	 * which descriptor userspace should receive next.
	 *
	 * After copying the frame to userspace, the descriptor is cleared,
	 * consumer is advanced, software statistics are updated, and one
	 * descriptor is submitted back to the device so the stream can continue.
	 */
	if (!pfdev->streaming)
		return -EINVAL;

	if (!pfdev->desc_ring || !pfdev->buffers || !pfdev->desc_count)
		return -EINVAL;

	if (count < PHANTOMFPGA_FRAME_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&pfdev->lock, flags);
	head = pfdev->consumer;
	tail = pfpga_read32(pfdev, PHANTOMFPGA_REG_DESC_TAIL) &
	       (pfdev->desc_count - 1);
	pfdev->shadow_tail = tail;
	pending = (tail - head) & (pfdev->desc_count - 1);
	spin_unlock_irqrestore(&pfdev->lock, flags);

	desc = &pfdev->desc_ring[head];


	/*
	 * Normal case:
	 *   head != tail, so pending > 0 and at least one descriptor should be
	 *   ready for userspace.
	 *
	 * Wrapped/full-ring case:
	 *   head == tail can look empty, but if the descriptor at head has the
	 *   COMPLETED bit set, the device has completed a full ring before
	 *   userspace consumed anything. In that case, read() must still consume
	 *   the descriptor instead of returning -EAGAIN.
	 */
	if (!pending &&
	    !(le32_to_cpu(desc->control) & PHANTOMFPGA_DESC_CTRL_COMPLETED))
		return -EAGAIN;

	if (pending &&
	    !(le32_to_cpu(desc->control) & PHANTOMFPGA_DESC_CTRL_COMPLETED)) {
		dev_dbg(&pfdev->pdev->dev,
			"read: DESC_TAIL indicates desc %u complete, control=0x%08x\n",
			head, le32_to_cpu(desc->control));
	}

	buffer = pfdev->buffers[head].vaddr;
	compl = phantomfpga_completion_ptr(buffer, pfdev->buffer_size);

	if (le32_to_cpu(compl->status) != PHANTOMFPGA_COMPL_OK)
		dev_warn(&pfdev->pdev->dev,
			 "descriptor %u completed with status %u\n",
			 head, le32_to_cpu(compl->status));

	to_copy = le32_to_cpu(compl->actual_length);
	if (!to_copy || to_copy > PHANTOMFPGA_FRAME_SIZE)
		to_copy = PHANTOMFPGA_FRAME_SIZE;

	if (copy_to_user(buf, buffer, to_copy))
		return -EFAULT;

	if (!pfpga_validate_frame_crc(buffer))
		pfdev->crc_errors++;

	/*
	 * Mark this descriptor reusable by clearing the device-completed bit.
	 */
	desc->control = cpu_to_le32(0);

	spin_lock_irqsave(&pfdev->lock, flags);
	pfdev->consumer = (head + 1) & (pfdev->desc_count - 1);
	pfdev->frames_consumed++;
	pfdev->bytes_consumed += to_copy;
	spin_unlock_irqrestore(&pfdev->lock, flags);

	/*
	 * Give one descriptor back to the device so streaming can continue.
	 */
	pfpga_submit_descriptors(pfdev, 1);

	ret = to_copy;
	return ret;
}

/*
 * Write to the device - not supported.
 * The device produces frames, it doesn't consume them.
 */
static ssize_t pfpga_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	return -EPERM;
}

/*
 * Poll for readable data.
 */
static __poll_t pfpga_poll(struct file *file, poll_table *wait)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	__poll_t mask = 0;
	unsigned long flags;
	u32 head, tail;

	/*
	 * Register this file with the driver's wait queue. Completion and
	 * no-descriptor IRQ handlers wake this queue when device state changes.
	 */
	poll_wait(file, &pfdev->wait_queue, wait);

	if (!pfdev->streaming) {
		mask |= EPOLLHUP;
		return mask;
	}

	if (!pfdev->desc_count)
		return mask;

	/*
	 * Refresh the driver's view of the hardware completion pointer.
	 * If consumer != tail, at least one completed descriptor is ready
	 * for userspace to read.
	 */
	spin_lock_irqsave(&pfdev->lock, flags);
	head = pfdev->consumer;
	tail = pfpga_read32(pfdev, PHANTOMFPGA_REG_DESC_TAIL) &
	       (pfdev->desc_count - 1);
	pfdev->shadow_tail = tail;
	spin_unlock_irqrestore(&pfdev->lock, flags);

	/*
	 * If consumer != tail, at least one completed descriptor is waiting
	 * for userspace to read.
	 */
	/*
	 * If consumer != tail, at least one completed descriptor is waiting
	 * for userspace to read.
	 *
	 * Also check the descriptor's COMPLETED bit directly. This matters when
	 * the hardware tail wraps around and becomes equal to the consumer index:
	 *
	 *   head == tail
	 *
	 * can mean "empty", but it can also happen after the device completed a
	 * full ring before userspace consumed anything. In that case, the current
	 * descriptor still has COMPLETED set, so poll() must report readable data.
	 */
	if (head != tail ||
	    (pfdev->desc_ring &&
	     (le32_to_cpu(pfdev->desc_ring[head].control) &
	      PHANTOMFPGA_DESC_CTRL_COMPLETED)))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

/*
 * Memory map descriptor buffers to userspace.
 *
 * This allows zero-copy access - userspace reads frames directly
 * from the DMA buffers without kernel copying.
 */
static int pfpga_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	int ret;

	dev_dbg(&pfdev->pdev->dev, "mmap request: size=%zu offset=%lu\n",
		size, offset);

	if (!pfdev->configured)
		return -EINVAL;

	if (!pfdev->buffer_pool)
		return -ENOMEM;

	if (offset != 0)
		return -EINVAL;

	if (size != pfdev->buffer_pool_size)
		return -EINVAL;

	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);

	ret = dma_mmap_coherent(&pfdev->pdev->dev,
				vma,
				pfdev->buffer_pool,
				pfdev->buffer_pool_dma,
				pfdev->buffer_pool_size);
	if (ret)
		dev_err(&pfdev->pdev->dev, "dma_mmap_coherent failed: %d\n", ret);

	return ret;
}

/*
 * IOCTL handler - the control center for device configuration.
 */
static long pfpga_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	void __user *argp = (void __user *)arg;
	int ret = 0;

	/*
	 * The v3.0 ioctl interface provides:
	 *   - PHANTOMFPGA_IOCTL_SET_CFG: Configure frame rate, descriptors
	 *   - PHANTOMFPGA_IOCTL_GET_CFG: Read current configuration
	 *   - PHANTOMFPGA_IOCTL_START: Start frame streaming
	 *   - PHANTOMFPGA_IOCTL_STOP: Stop frame streaming
	 *   - PHANTOMFPGA_IOCTL_GET_STATS: Get statistics
	 *   - PHANTOMFPGA_IOCTL_RESET_STATS: Reset driver statistics
	 *   - PHANTOMFPGA_IOCTL_GET_BUFFER_INFO: Get mmap info
	 *   - PHANTOMFPGA_IOCTL_CONSUME_FRAME: Mark frame consumed (mmap mode)
	 *   - PHANTOMFPGA_IOCTL_SET_FAULT: Configure fault injection
	 */

	if (_IOC_TYPE(cmd) != PHANTOMFPGA_IOC_MAGIC)
		return -ENOTTY;

	if (_IOC_NR(cmd) > PHANTOMFPGA_IOC_MAXNR)
		return -ENOTTY;

	mutex_lock(&pfdev->ioctl_lock);

	switch (cmd) {
	case PHANTOMFPGA_IOCTL_SET_CFG:
		{
			struct phantomfpga_config cfg;

				/*
				 * Configure frame streaming from userspace.
				 *
				 * This path validates the requested descriptor count and
				 * frame rate, allocates/reallocates SG-DMA resources, stores
				 * the runtime configuration, programs device registers, and
				 * initializes/submits the descriptor ring.
				 */
			if (pfdev->streaming) {
				ret = -EBUSY;
				break;
			}

			if (copy_from_user(&cfg, argp, sizeof(cfg))) {
				ret = -EFAULT;
				break;
			}

			if (cfg.desc_count < PHANTOMFPGA_MIN_DESC_COUNT ||
			    cfg.desc_count > PHANTOMFPGA_MAX_DESC_COUNT ||
			    (cfg.desc_count & (cfg.desc_count - 1))) {
				ret = -EINVAL;
				break;
			}

			if (cfg.frame_rate < PHANTOMFPGA_MIN_FRAME_RATE ||
			    cfg.frame_rate > PHANTOMFPGA_MAX_FRAME_RATE) {
				ret = -EINVAL;
				break;
			}

			ret = pfpga_alloc_descriptors(pfdev, cfg.desc_count,
						      PHANTOMFPGA_BUFFER_SIZE);
			if (ret)
				break;

			pfdev->frame_rate = cfg.frame_rate;
			pfdev->irq_coalesce_count = cfg.irq_coalesce_count;
			pfdev->irq_coalesce_timeout = cfg.irq_coalesce_timeout;

			/*
			 * Start each new userspace configuration from a clean
			 * device state. This clears stale hardware DESC_TAIL,
			 * frame sequence state, pending IRQ status, and device
			 * statistics before the ring/config registers are
			 * programmed again.
			 */
			pfpga_soft_reset(pfdev);

			pfpga_apply_config(pfdev);
			pfpga_configure_desc_ring(pfdev);
			pfpga_init_descriptors(pfdev);

			pfdev->configured = true;
			ret = 0;
		}
		break;

	case PHANTOMFPGA_IOCTL_GET_CFG:
		{
			struct phantomfpga_config cfg = {
				.desc_count = pfdev->desc_count,
				.frame_rate = pfdev->frame_rate,
				.irq_coalesce_count = pfdev->irq_coalesce_count,
				.irq_coalesce_timeout = pfdev->irq_coalesce_timeout,
			};

			if (copy_to_user(argp, &cfg, sizeof(cfg)))
				ret = -EFAULT;
		}
		break;

	case PHANTOMFPGA_IOCTL_START:
		ret = pfpga_start_streaming(pfdev);
		break;

	case PHANTOMFPGA_IOCTL_STOP:
		ret = pfpga_stop_streaming(pfdev);
		break;

	case PHANTOMFPGA_IOCTL_GET_STATS:
		{
			struct phantomfpga_stats stats;
			unsigned long flags;

			/*
			* Return combined driver/device statistics.
			*
			* Driver-owned counters are copied under the spinlock.
			* Device-owned counters are read from MMIO registers.
			* The completed stats structure is then copied to userspace.
			*/

			memset(&stats, 0, sizeof(stats));

			spin_lock_irqsave(&pfdev->lock, flags);
			stats.frames_consumed = pfdev->frames_consumed;
			stats.bytes_consumed = pfdev->bytes_consumed;
			stats.irq_count = pfdev->irq_count;
			stats.crc_errors = pfdev->crc_errors;
			spin_unlock_irqrestore(&pfdev->lock, flags);

			/* Read device stats */
			stats.frames_produced = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_FRAMES_TX);
			stats.frames_dropped = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_FRAMES_DROP);
			stats.desc_completed = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_DESC_COMPL);
			stats.errors = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_ERRORS);
			stats.current_frame = pfpga_read32(pfdev, PHANTOMFPGA_REG_CURRENT_FRAME);
			stats.status = pfpga_read32(pfdev, PHANTOMFPGA_REG_STATUS);

			if (copy_to_user(argp, &stats, sizeof(stats)))
				ret = -EFAULT;
		}
		break;

	case PHANTOMFPGA_IOCTL_RESET_STATS:
		{
			unsigned long flags;

			spin_lock_irqsave(&pfdev->lock, flags);
			pfdev->frames_consumed = 0;
			pfdev->bytes_consumed = 0;
			pfdev->irq_count = 0;
			pfdev->crc_errors = 0;
			spin_unlock_irqrestore(&pfdev->lock, flags);
		}
		break;

	case PHANTOMFPGA_IOCTL_GET_BUFFER_INFO:
		{
			struct phantomfpga_buffer_info info;

			if (!pfdev->configured) {
				ret = -EINVAL;
				break;
			}

			memset(&info, 0, sizeof(info));
			info.buffer_size = pfdev->buffer_size;
			info.buffer_count = pfdev->desc_count;
			info.total_size = pfdev->buffer_size * pfdev->desc_count;
			info.frame_size = PHANTOMFPGA_FRAME_SIZE;

			if (copy_to_user(argp, &info, sizeof(info)))
				ret = -EFAULT;
		}
		break;

	case PHANTOMFPGA_IOCTL_CONSUME_FRAME:
		{
			struct phantomfpga_sg_desc *desc;
			struct phantomfpga_completion *compl;
			unsigned long flags;
			size_t consumed_len;
			u32 head, tail, pending;

			if (!pfdev->streaming) {
				ret = -EINVAL;
				break;
			}

			if (!pfdev->desc_ring || !pfdev->buffers || !pfdev->desc_count) {
				ret = -EINVAL;
				break;
			}

			spin_lock_irqsave(&pfdev->lock, flags);
			head = pfdev->consumer;
			tail = pfpga_read32(pfdev, PHANTOMFPGA_REG_DESC_TAIL) &
			       (pfdev->desc_count - 1);
			pfdev->shadow_tail = tail;
			pending = (tail - head) & (pfdev->desc_count - 1);
			spin_unlock_irqrestore(&pfdev->lock, flags);

			desc = &pfdev->desc_ring[head];

			if (!pending &&
			    !(le32_to_cpu(desc->control) & PHANTOMFPGA_DESC_CTRL_COMPLETED)) {
				ret = -EAGAIN;
				break;
			}

			if (pending &&
			    !(le32_to_cpu(desc->control) & PHANTOMFPGA_DESC_CTRL_COMPLETED)) {
				dev_dbg(&pfdev->pdev->dev,
					"consume: DESC_TAIL indicates desc %u complete, control=0x%08x\n",
					head, le32_to_cpu(desc->control));
			}

			compl = phantomfpga_completion_ptr(pfdev->buffers[head].vaddr,
							   pfdev->buffer_size);

			if (le32_to_cpu(compl->status) != PHANTOMFPGA_COMPL_OK)
				dev_warn(&pfdev->pdev->dev,
					 "descriptor %u completed with status %u\n",
					 head, le32_to_cpu(compl->status));

			consumed_len = le32_to_cpu(compl->actual_length);
			if (!consumed_len || consumed_len > PHANTOMFPGA_FRAME_SIZE)
				consumed_len = PHANTOMFPGA_FRAME_SIZE;

			desc->control = cpu_to_le32(0);

			spin_lock_irqsave(&pfdev->lock, flags);
			pfdev->consumer = (head + 1) & (pfdev->desc_count - 1);
			pfdev->frames_consumed++;
			pfdev->bytes_consumed += consumed_len;
			spin_unlock_irqrestore(&pfdev->lock, flags);

			pfpga_submit_descriptors(pfdev, 1);

			ret = 0;
		}
		break;

	case PHANTOMFPGA_IOCTL_SET_FAULT:
		{
			struct phantomfpga_fault_cfg fault;

			if (copy_from_user(&fault, argp, sizeof(fault))) {
				ret = -EFAULT;
				break;
			}

			/*
			 * Configure QEMU/device-side fault injection.
			 *
			 * inject_flags selects which faults are active. Mask the value
			 * before writing so userspace cannot set undefined fault bits.
			 *
			 * fault_rate controls how often a fault is injected:
			 *   0 = disabled by the device helper
			 *   N = approximately 1 in N frames affected
			 */
			pfpga_write32(pfdev, PHANTOMFPGA_REG_FAULT_INJECT,
				      fault.inject_flags & PHANTOMFPGA_FAULT_ALL);
			pfpga_write32(pfdev, PHANTOMFPGA_REG_FAULT_RATE,
				      fault.fault_rate);

			ret = 0;
		}
		break;

	default:
		ret = -ENOTTY;
	}

	mutex_unlock(&pfdev->ioctl_lock);
	return ret;
}

/* File operations structure */
static const struct file_operations phantomfpga_fops = {
	.owner          = THIS_MODULE,
	.open           = pfpga_open,
	.release        = pfpga_release,
	.read           = pfpga_read,
	.write          = pfpga_write,
	.poll           = pfpga_poll,
	.mmap           = pfpga_mmap,
	.unlocked_ioctl = pfpga_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ------------------------------------------------------------------------ */
/* PCI Device Initialization                                                */
/* ------------------------------------------------------------------------ */

/*
 * Setup MSI-X interrupts.
 */
static int pfpga_setup_msix(struct phantomfpga_dev *pfdev)
{
	struct pci_dev *pdev = pfdev->pdev;
	int ret;

	pfdev->num_vectors = 0;
	pfdev->irq_complete = -1;
	pfdev->irq_error = -1;
	pfdev->irq_no_desc = -1;

	/*
	 * Prefer the full v3.0 MSI-X layout: one vector for completion,
	 * one for errors, and one for no-descriptor/backpressure events.
	 */
	ret = pci_alloc_irq_vectors(pdev,
				    PHANTOMFPGA_MSIX_VECTORS,
				    PHANTOMFPGA_MSIX_VECTORS,
				    PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_warn(&pdev->dev,
			 "failed to allocate MSI-X vectors (%d), trying MSI/legacy\n",
			 ret);

		/*
		 * Fallback: use one interrupt vector if the platform does not
		 * provide all MSI-X vectors.
		 */
		ret = pci_alloc_irq_vectors(pdev, 1, 1,
					    PCI_IRQ_MSI | PCI_IRQ_LEGACY);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"failed to allocate interrupt vector: %d\n",
				ret);
			return ret;
		}
	}

	pfdev->num_vectors = ret;

	pfdev->irq_complete = pci_irq_vector(pdev,
					     PHANTOMFPGA_MSIX_VEC_COMPLETE);
	if (pfdev->irq_complete < 0) {
		ret = pfdev->irq_complete;
		goto err_free_vectors;
	}

	ret = request_irq(pfdev->irq_complete, pfpga_irq_complete,
			  0, DRIVER_NAME "-complete", pfdev);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to request completion IRQ: %d\n", ret);
		goto err_free_vectors;
	}

	if (pfdev->num_vectors > PHANTOMFPGA_MSIX_VEC_ERROR) {
		pfdev->irq_error = pci_irq_vector(pdev,
						  PHANTOMFPGA_MSIX_VEC_ERROR);
		if (pfdev->irq_error < 0) {
			ret = pfdev->irq_error;
			goto err_free_complete;
		}

		ret = request_irq(pfdev->irq_error, pfpga_irq_error,
				  0, DRIVER_NAME "-error", pfdev);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to request error IRQ: %d\n", ret);
			goto err_free_complete;
		}
	}

	if (pfdev->num_vectors > PHANTOMFPGA_MSIX_VEC_NO_DESC) {
		pfdev->irq_no_desc = pci_irq_vector(pdev,
						    PHANTOMFPGA_MSIX_VEC_NO_DESC);
		if (pfdev->irq_no_desc < 0) {
			ret = pfdev->irq_no_desc;
			goto err_free_error;
		}

		ret = request_irq(pfdev->irq_no_desc, pfpga_irq_no_desc,
				  0, DRIVER_NAME "-no-desc", pfdev);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to request no-desc IRQ: %d\n", ret);
			goto err_free_error;
		}
	}

	dev_info(&pdev->dev, "interrupts enabled with %d vector(s)\n",
		 pfdev->num_vectors);

	return 0;

err_free_error:
	if (pfdev->irq_error >= 0) {
		free_irq(pfdev->irq_error, pfdev);
		pfdev->irq_error = -1;
	}

err_free_complete:
	if (pfdev->irq_complete >= 0) {
		free_irq(pfdev->irq_complete, pfdev);
		pfdev->irq_complete = -1;
	}

err_free_vectors:
	pci_free_irq_vectors(pdev);
	pfdev->num_vectors = 0;
	pfdev->irq_complete = -1;
	pfdev->irq_error = -1;
	pfdev->irq_no_desc = -1;

	return ret;
}

/*
 * Cleanup MSI-X interrupts.
 */
static void pfpga_teardown_msix(struct phantomfpga_dev *pfdev)
{
	if (pfdev->irq_no_desc >= 0) {
		free_irq(pfdev->irq_no_desc, pfdev);
		pfdev->irq_no_desc = -1;
	}

	if (pfdev->irq_error >= 0) {
		free_irq(pfdev->irq_error, pfdev);
		pfdev->irq_error = -1;
	}

	if (pfdev->irq_complete >= 0) {
		free_irq(pfdev->irq_complete, pfdev);
		pfdev->irq_complete = -1;
	}

	if (pfdev->num_vectors > 0) {
		pci_free_irq_vectors(pfdev->pdev);
		pfdev->num_vectors = 0;
	}
}

/*
 * Free SG-DMA resources.
 *
 * The current implementation uses one coherent descriptor ring and one
 * contiguous coherent DMA buffer pool. Each pfdev->buffers[i] entry points
 * into that pool, so the pool is freed once. The fallback branch keeps
 * compatibility with the older per-buffer allocation style.
 *
 * After freeing memory, all descriptor, buffer, and ring-tracking state is
 * reset so the next configuration starts from a clean software state.
 */
static void pfpga_free_descriptors(struct phantomfpga_dev *pfdev)
{
	u32 i;

	/*
	 * New path: one contiguous DMA buffer pool.
	 * Each pfdev->buffers[i].vaddr points inside this pool, so we must
	 * free the pool once, not each buffer separately.
	 */
	if (pfdev->buffer_pool) {
		dma_free_coherent(&pfdev->pdev->dev,
				  pfdev->buffer_pool_size,
				  pfdev->buffer_pool,
				  pfdev->buffer_pool_dma);

		pfdev->buffer_pool = NULL;
		pfdev->buffer_pool_dma = 0;
		pfdev->buffer_pool_size = 0;
	} else if (pfdev->buffers) {
		/*
		 * Fallback path: older style where each descriptor had its own
		 * separate DMA allocation.
		 */
		for (i = 0; i < pfdev->desc_count; i++) {
			if (pfdev->buffers[i].vaddr) {
				dma_free_coherent(&pfdev->pdev->dev,
						  pfdev->buffers[i].size,
						  pfdev->buffers[i].vaddr,
						  pfdev->buffers[i].dma_addr);
			}
		}
	}

	kfree(pfdev->buffers);
	pfdev->buffers = NULL;

	if (pfdev->desc_ring) {
		size_t ring_size = pfdev->desc_count * sizeof(*pfdev->desc_ring);

		dma_free_coherent(&pfdev->pdev->dev,
				  ring_size,
				  pfdev->desc_ring,
				  pfdev->desc_ring_dma);

		pfdev->desc_ring = NULL;
		pfdev->desc_ring_dma = 0;
	}

	pfdev->desc_count = 0;
	pfdev->buffer_size = 0;
	pfdev->desc_head = 0;
	pfdev->desc_tail = 0;
	pfdev->shadow_tail = 0;
	pfdev->consumer = 0;
	pfdev->configured = false;
}


/*
 * Allocate SG-DMA resources.
 *
 * The descriptor ring is allocated as coherent DMA memory because both the
 * CPU and the PhantomFPGA device access it. Frame storage is allocated as one
 * contiguous coherent DMA pool, then split into per-descriptor slices through
 * pfdev->buffers[i].
 *
 * This layout keeps the mmap path simple: userspace can map one contiguous
 * pool, while the device still receives one DMA destination address per
 * descriptor.
 */
static int pfpga_alloc_descriptors(struct phantomfpga_dev *pfdev,
				   u32 desc_count, size_t buffer_size)
{
	size_t ring_size;
	size_t pool_size;
	u32 i;

	/* Free old DMA resources before allocating a new configuration */
	pfpga_free_descriptors(pfdev);

	ring_size = desc_count * sizeof(*pfdev->desc_ring);
	pool_size = desc_count * buffer_size;

	/* Allocate coherent DMA memory for the descriptor ring */
	pfdev->desc_ring = dma_alloc_coherent(&pfdev->pdev->dev,
					      ring_size,
					      &pfdev->desc_ring_dma,
					      GFP_KERNEL);
	if (!pfdev->desc_ring)
		return -ENOMEM;

	memset(pfdev->desc_ring, 0, ring_size);

	/*
	 * Store these now so pfpga_free_descriptors() can clean up correctly
	 * if a later allocation fails.
	 */
	pfdev->desc_count = desc_count;
	pfdev->buffer_size = buffer_size;
	pfdev->buffer_pool_size = pool_size;

	/* Allocate one contiguous coherent DMA buffer pool for mmap */
	pfdev->buffer_pool = dma_alloc_coherent(&pfdev->pdev->dev,
						pool_size,
						&pfdev->buffer_pool_dma,
						GFP_KERNEL);
	if (!pfdev->buffer_pool)
		goto err_free;

	memset(pfdev->buffer_pool, 0, pool_size);

	/* Allocate the driver-side buffer tracking array */
	pfdev->buffers = kcalloc(desc_count, sizeof(*pfdev->buffers), GFP_KERNEL);
	if (!pfdev->buffers)
		goto err_free;

	/*
	 * Each buffer entry points to its slice inside the contiguous pool.
	 * The device still gets one DMA address per descriptor.
	 */
	for (i = 0; i < desc_count; i++) {
		pfdev->buffers[i].vaddr =
			(u8 *)pfdev->buffer_pool + (i * buffer_size);
		pfdev->buffers[i].dma_addr =
			pfdev->buffer_pool_dma + (i * buffer_size);
		pfdev->buffers[i].size = buffer_size;
	}

	dev_info(&pfdev->pdev->dev,
		 "allocated %u descriptors, buffer_size=%zu, pool_size=%zu, ring_dma=%pad, pool_dma=%pad\n",
		 desc_count, buffer_size, pool_size,
		 &pfdev->desc_ring_dma, &pfdev->buffer_pool_dma);

	return 0;

err_free:
	pfpga_free_descriptors(pfdev);
	return -ENOMEM;
}
/*
 * Create character device.
 */
static int pfpga_create_cdev(struct phantomfpga_dev *pfdev)
{
	int minor;
	int ret;

	minor = ida_alloc_max(&phantomfpga_ida, PHANTOMFPGA_MAX_DEVICES - 1,
			      GFP_KERNEL);
	if (minor < 0)
		return minor;

	pfdev->minor = minor;
	pfdev->devno = MKDEV(MAJOR(phantomfpga_devno), minor);

	cdev_init(&pfdev->cdev, &phantomfpga_fops);
	pfdev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&pfdev->cdev, pfdev->devno, 1);
	if (ret)
		goto err_ida;

	pfdev->dev = device_create(phantomfpga_class, &pfdev->pdev->dev,
				   pfdev->devno, pfdev, DRIVER_NAME "%d", minor);
	if (IS_ERR(pfdev->dev)) {
		ret = PTR_ERR(pfdev->dev);
		goto err_cdev;
	}

	dev_info(&pfdev->pdev->dev, "created /dev/%s%d\n", DRIVER_NAME, minor);
	return 0;

err_cdev:
	cdev_del(&pfdev->cdev);
err_ida:
	ida_free(&phantomfpga_ida, minor);
	return ret;
}

/*
 * Destroy character device.
 */
static void pfpga_destroy_cdev(struct phantomfpga_dev *pfdev)
{
	device_destroy(phantomfpga_class, pfdev->devno);
	cdev_del(&pfdev->cdev);
	ida_free(&phantomfpga_ida, pfdev->minor);
}

/*
 * PCI probe function - called when kernel finds matching device.
 */
static int phantomfpga_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct phantomfpga_dev *pfdev;
	u32 dev_id, dev_ver;
	int ret;

	dev_info(&pdev->dev, "probing PhantomFPGA v3.0 device\n");

	/* Allocate device private data */
	pfdev = kzalloc(sizeof(*pfdev), GFP_KERNEL);
	if (!pfdev)
		return -ENOMEM;

	pfdev->pdev = pdev;
	spin_lock_init(&pfdev->lock);
	mutex_init(&pfdev->ioctl_lock);
	init_waitqueue_head(&pfdev->wait_queue);

	pci_set_drvdata(pdev, pfdev);

	/* Enable PCI device */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable PCI device\n");
		goto err_free;
	}

	/* Request BAR0 region */
	ret = pci_request_region(pdev, 0, DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "failed to request BAR0\n");
		goto err_disable;
	}

	/* Map BAR0 */
	pfdev->regs_start = pci_resource_start(pdev, 0);
	pfdev->regs_len = pci_resource_len(pdev, 0);
	pfdev->regs = pci_iomap(pdev, 0, pfdev->regs_len);
	if (!pfdev->regs) {
		dev_err(&pdev->dev, "failed to map BAR0\n");
		ret = -ENOMEM;
		goto err_release;
	}

	dev_info(&pdev->dev, "BAR0 mapped: phys=0x%llx len=%llu virt=%p\n",
		 (unsigned long long)pfdev->regs_start,
		 (unsigned long long)pfdev->regs_len,
		 pfdev->regs);

	/* Verify device identity */
	dev_id = pfpga_read32(pfdev, PHANTOMFPGA_REG_DEV_ID);
	if (dev_id != PHANTOMFPGA_DEV_ID_VAL) {
		dev_err(&pdev->dev, "unexpected device ID: 0x%08x (expected 0x%08x)\n",
			dev_id, PHANTOMFPGA_DEV_ID_VAL);
		ret = -ENODEV;
		goto err_unmap;
	}

	/* Check version */
	dev_ver = pfpga_read32(pfdev, PHANTOMFPGA_REG_DEV_VER);
	dev_info(&pdev->dev, "device ID: 0x%08x version: 0x%08x\n", dev_id, dev_ver);

	if (dev_ver < PHANTOMFPGA_DEV_VER) {
		dev_warn(&pdev->dev, "device version older than driver expects, "
			 "things might get interesting\n");
	}

	/* Enable bus mastering for DMA */
	pci_set_master(pdev);

	/* Set DMA mask - try 64-bit, fall back to 32-bit */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "failed to set DMA mask\n");
			goto err_unmap;
		}
		dev_info(&pdev->dev, "using 32-bit DMA\n");
	} else {
		dev_info(&pdev->dev, "using 64-bit DMA\n");
	}

	/* Setup MSI-X interrupts */
	ret = pfpga_setup_msix(pfdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to setup MSI-X: %d\n", ret);
		goto err_unmap;
	}

	/* Allocate default descriptors and buffers */
	ret = pfpga_alloc_descriptors(pfdev,
				      PHANTOMFPGA_DEFAULT_DESC_COUNT,
				      PHANTOMFPGA_BUFFER_SIZE);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate descriptors: %d\n", ret);
		goto err_msix;
	}

	/* Perform soft reset */
	pfpga_soft_reset(pfdev);

	/* Create character device */
	ret = pfpga_create_cdev(pfdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to create char device: %d\n", ret);
		goto err_desc;
	}

	/* Set default configuration (not yet applied) */
	pfdev->desc_count = PHANTOMFPGA_DEFAULT_DESC_COUNT;
	pfdev->frame_rate = PHANTOMFPGA_DEFAULT_FRAME_RATE;
	pfdev->irq_coalesce_count = PHANTOMFPGA_DEFAULT_IRQ_COUNT;
	pfdev->irq_coalesce_timeout = PHANTOMFPGA_DEFAULT_IRQ_TIMEOUT;
	pfdev->buffer_size = PHANTOMFPGA_BUFFER_SIZE;
	pfdev->configured = false;
	pfdev->streaming = false;

	dev_info(&pdev->dev, "PhantomFPGA v3.0 driver loaded\n");
	return 0;

err_desc:
	pfpga_free_descriptors(pfdev);
err_msix:
	pfpga_teardown_msix(pfdev);
err_unmap:
	pci_iounmap(pdev, pfdev->regs);
err_release:
	pci_release_region(pdev, 0);
err_disable:
	pci_disable_device(pdev);
err_free:
	kfree(pfdev);
	return ret;
}

/*
 * PCI remove function - called when device is removed or driver unloaded.
 */
static void phantomfpga_remove(struct pci_dev *pdev)
{
	struct phantomfpga_dev *pfdev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "removing PhantomFPGA device\n");

	/* Stop streaming if active */
	if (pfdev->streaming) {
		mutex_lock(&pfdev->ioctl_lock);
		pfpga_stop_streaming(pfdev);
		mutex_unlock(&pfdev->ioctl_lock);
	}

	/* Destroy character device */
	pfpga_destroy_cdev(pfdev);

	/* Free descriptors and buffers */
	pfpga_free_descriptors(pfdev);

	/* Release interrupts */
	pfpga_teardown_msix(pfdev);

	/* Unmap and release BAR0 */
	if (pfdev->regs) {
		pci_iounmap(pdev, pfdev->regs);
		pfdev->regs = NULL;
	}
	pci_release_region(pdev, 0);

	/* Disable device */
	pci_disable_device(pdev);

	/* Free private data */
	kfree(pfdev);

	dev_info(&pdev->dev, "PhantomFPGA driver unloaded\n");
}

/* PCI driver structure */
static struct pci_driver phantomfpga_pci_driver = {
	.name     = DRIVER_NAME,
	.id_table = phantomfpga_pci_ids,
	.probe    = phantomfpga_probe,
	.remove   = phantomfpga_remove,
};

/* ------------------------------------------------------------------------ */
/* Module Init/Exit                                                         */
/* ------------------------------------------------------------------------ */

static int __init phantomfpga_init(void)
{
	int ret;

	pr_info("PhantomFPGA v3.0 driver initializing\n");

	ret = alloc_chrdev_region(&phantomfpga_devno, 0, PHANTOMFPGA_MAX_DEVICES,
				  DRIVER_NAME);
	if (ret) {
		pr_err("failed to allocate chrdev region: %d\n", ret);
		return ret;
	}

	phantomfpga_class = class_create(DRIVER_NAME);
	if (IS_ERR(phantomfpga_class)) {
		ret = PTR_ERR(phantomfpga_class);
		pr_err("failed to create device class: %d\n", ret);
		goto err_chrdev;
	}

	ret = pci_register_driver(&phantomfpga_pci_driver);
	if (ret) {
		pr_err("failed to register PCI driver: %d\n", ret);
		goto err_class;
	}

	pr_info("PhantomFPGA v3.0 driver initialized (major=%d)\n",
		MAJOR(phantomfpga_devno));
	return 0;

err_class:
	class_destroy(phantomfpga_class);
err_chrdev:
	unregister_chrdev_region(phantomfpga_devno, PHANTOMFPGA_MAX_DEVICES);
	return ret;
}

static void __exit phantomfpga_exit(void)
{
	pr_info("PhantomFPGA v3.0 driver exiting\n");

	pci_unregister_driver(&phantomfpga_pci_driver);
	class_destroy(phantomfpga_class);
	unregister_chrdev_region(phantomfpga_devno, PHANTOMFPGA_MAX_DEVICES);

	pr_info("PhantomFPGA v3.0 driver unloaded\n");
}

module_init(phantomfpga_init);
module_exit(phantomfpga_exit);
