// #include <linux/bitfield.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "dmaengine.h"
#include "virt-dma.h"

#define TWL_NDMA_NUM_CHAN	4

/** Global configuration register */
#define REG_NDMA_GCNT	0x00

#define NDMA_GCNT_CYCLE_SELECTION(n)	(((n) & 0xf) << 16)

#define NDMA_GCNT_PRIORITY_NUMERICAL	(0)
#define NDMA_GCNT_PRIORITY_ROUNDROBIN	BIT(31)

/** Per-channel configuration registers */
#define REG_NDMA(n)	(((n) * 0x1c) + 4)
#define REG_NDMA_SAD	0x00
#define REG_NDMA_DAD	0x04
#define REG_NDMA_TCNT	0x08
#define REG_NDMA_WCNT	0x0c
#define REG_NDMA_BCNT	0x10
#define REG_NDMA_FILL	0x14
#define REG_NDMA_CNT	0x18

/** Maximum Total Transfer length (including repeats) */
#define NDMA_TCNT_MAX	0x10000000

/** Maximum Logical Block length */
#define NDMA_WCNT_MAX	0x1000000

/** Block Transfer Timing/Interval configuration */
#define NDMA_BCNT_INTERVAL(x)	((x) & 0xffff)
#define NDMA_BCNT_PRESCALE_1	(0u << 16)
#define NDMA_BCNT_PRESCALE_4	(1u << 16)
#define NDMA_BCNT_PRESCALE_16	(2u << 16)
#define NDMA_BCNT_PRESCALE_64	(3u << 16)

/** Control fields */
#define NDMA_CNT_DST_INCREMENT	(0u << 10)
#define NDMA_CNT_DST_DECREMENT	(1u << 10)
#define NDMA_CNT_DST_FIXED	(2u << 10)
#define NDMA_CNT_DST_RELOAD	BIT(12)

#define NDMA_CNT_SRC_INCREMENT	(0u << 13)
#define NDMA_CNT_SRC_DECREMENT	(1u << 13)
#define NDMA_CNT_SRC_FIXED	(2u << 13)
#define NDMA_CNT_SRC_FILL	(3u << 13)
#define NDMA_CNT_SRC_RELOAD	BIT(15)

#define NDMA_CNT_BLK_WORDS(w)	(((w) & 0xf) << 16)

#define NDMA_CNT_MODE_IMMEDIATE	BIT(28)

#define NDMA_CNT_REPEAT_TOTAL	(0)
#define NDMA_CNT_REPEAT_FOREVER	BIT(29)

#define NDMA_CNT_IRQ_ENABLE	BIT(30)
#define NDMA_CNT_START_BUSY	BIT(31)


struct twl_ndma;
struct twl_ndma_tx;

struct twl_ndma_chan {
	void __iomem *io;
	dma_cookie_t cookie;
	struct virt_dma_chan vchan;
	struct twl_ndma_tx *current_tx;
};

struct twl_ndma {
	struct dma_device dma;
	struct twl_ndma_chan chan[TWL_NDMA_NUM_CHAN];
};

struct twl_ndma_tx {
	struct virt_dma_desc vdesc;
	u32 sad;
	u32 dad;
	u32 tcnt;
	u32 wcnt;
	u32 bcnt;
	u32 fill;
	u32 cnt;
};

static inline struct twl_ndma *to_twl_ndma(struct dma_device *dma)
{
	return container_of(dma, struct twl_ndma, dma);
}

static inline struct twl_ndma_chan *to_twl_ndma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct twl_ndma_chan, vchan.chan);
}

static inline struct twl_ndma_tx *to_twl_ndma_tx(struct virt_dma_desc *desc)
{
	return container_of(desc, struct twl_ndma_tx, vdesc);
}

static u32 twl_ndma_chan_cnt(struct twl_ndma_chan *chan)
{
	return ioread32(chan->io + REG_NDMA_CNT);
}

static void twl_ndma_chan_tx(struct twl_ndma_chan *chan, struct twl_ndma_tx *tx)
{
	void __iomem *io = chan->io;
	iowrite32(tx->sad,  io + REG_NDMA_SAD);
	iowrite32(tx->dad,  io + REG_NDMA_DAD);
	iowrite32(tx->tcnt, io + REG_NDMA_TCNT);
	iowrite32(tx->wcnt, io + REG_NDMA_WCNT);
	iowrite32(tx->bcnt, io + REG_NDMA_BCNT);
	iowrite32(tx->fill, io + REG_NDMA_FILL);
	iowrite32(tx->cnt,  io + REG_NDMA_CNT);
}

static void twl_ndma_tx_free(struct virt_dma_desc *desc)
{
	kfree(to_twl_ndma_tx(desc));
}

static struct dma_async_tx_descriptor *twl_ndma_prep_xfer(struct twl_ndma_chan *chan,
			dma_addr_t dest, dma_addr_t src, size_t len,
			u32 fill, u32 cnt, unsigned long flags)
{
	struct twl_ndma_tx *tx;

	if (!len || ((dest | src | len) & 3)) {
		pr_err("align check failed %px %px %zu\n", (void*)dest, (void*)src, len);
		return NULL;
	}

	if (len > NDMA_WCNT_MAX) {
		pr_err("transfer length %zu exceeds the maximum %zu\n",
			len, NDMA_WCNT_MAX);
		return NULL;
	} else if (len == NDMA_WCNT_MAX) {
		len = 0; /* special case for the max transfer length */
	}

	tx = kzalloc(sizeof(*tx), GFP_NOWAIT);
	if (!tx) {
		pr_err("mem alloc failed\n");
		return NULL;
	}

	tx->sad = src;
	tx->dad = dest;
	tx->tcnt = 0;
	tx->wcnt = len >> 2;
	tx->bcnt = 0;
	tx->fill = fill;
	tx->cnt = cnt | NDMA_CNT_BLK_WORDS(0) | NDMA_CNT_MODE_IMMEDIATE |
		  NDMA_CNT_IRQ_ENABLE | NDMA_CNT_START_BUSY;
	return vchan_tx_prep(&chan->vchan, &tx->vdesc, flags);
}

static struct dma_async_tx_descriptor *twl_ndma_prep_memcpy(struct dma_chan *chan,
		dma_addr_t dest, dma_addr_t src, size_t len, unsigned long flags)
{
	return twl_ndma_prep_xfer(to_twl_ndma_chan(chan),
		dest, src, len, 0,
		NDMA_CNT_SRC_INCREMENT | NDMA_CNT_DST_INCREMENT,
		flags
	);
}

static struct dma_async_tx_descriptor *twl_ndma_prep_memset(struct dma_chan *chan,
		dma_addr_t dest, int value, size_t len, unsigned long flags)
{
	return twl_ndma_prep_xfer(to_twl_ndma_chan(chan),
		dest, 0, len, (u32)value,
		NDMA_CNT_SRC_FILL | NDMA_CNT_DST_INCREMENT,
		flags
	);
}

static int twl_ndma_terminate_all(struct dma_chan *chan)
{
	struct twl_ndma_chan *ndma_chan = to_twl_ndma_chan(chan);
	unsigned long flags;
	LIST_HEAD(list);

	spin_lock_irqsave(&ndma_chan->vchan.lock, flags);
	iowrite32(0, ndma_chan->io + REG_NDMA_CNT);	/* stop the active transfer */

	if (ndma_chan->current_tx) {
		vchan_terminate_vdesc(&ndma_chan->current_tx->vdesc);
		ndma_chan->current_tx = NULL;
	}

	vchan_get_all_descriptors(&ndma_chan->vchan, &list);
	list_splice_tail(&list, &ndma_chan->vchan.desc_terminated);
	spin_unlock_irqrestore(&ndma_chan->vchan.lock, flags);
	return 0;
}

static void twl_ndma_synchronize(struct dma_chan *chan)
{
	struct twl_ndma_chan *ndma_chan = to_twl_ndma_chan(chan);
	vchan_synchronize(&ndma_chan->vchan);
}

static enum dma_status twl_ndma_tx_status(struct dma_chan *chan,
					 dma_cookie_t cookie,
					 struct dma_tx_state *state)
{
	struct twl_ndma_chan *ndma_chan = to_twl_ndma_chan(chan);
	unsigned long flags;

	enum dma_status status = dma_cookie_status(chan, cookie, state);

	spin_lock_irqsave(&ndma_chan->vchan.lock, flags);
	if (cookie == ndma_chan->cookie) {
		/* the current DMA transfer is either in progress or done */
		status = (twl_ndma_chan_cnt(ndma_chan) & NDMA_CNT_START_BUSY) ?
			DMA_IN_PROGRESS : DMA_COMPLETE;
	} else if (vchan_find_desc(&ndma_chan->vchan, cookie)) {
		status = DMA_IN_PROGRESS;
	} else {
		status = DMA_ERROR;
	}
	spin_unlock_irqrestore(&ndma_chan->vchan.lock, flags);

	return status;
}

static void twl_ndma_chan_start_next(struct twl_ndma_chan *ndma_chan)
{
	struct virt_dma_desc *next_desc = vchan_next_desc(&ndma_chan->vchan);
	if (!next_desc) {
		ndma_chan->current_tx = NULL;
		return;
	}

	list_del(&next_desc->node);
	ndma_chan->cookie = next_desc->tx.cookie;
	ndma_chan->current_tx = to_twl_ndma_tx(next_desc);
	twl_ndma_chan_tx(ndma_chan, ndma_chan->current_tx);
}

static void twl_ndma_issue_pending(struct dma_chan *chan)
{
	struct twl_ndma_chan *ndma_chan = to_twl_ndma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&ndma_chan->vchan.lock, flags);
	if (vchan_issue_pending(&ndma_chan->vchan) && !ndma_chan->current_tx)
		twl_ndma_chan_start_next(ndma_chan);
	spin_unlock_irqrestore(&ndma_chan->vchan.lock, flags);
}

static void twl_ndma_release(struct dma_device *dev)
{
	/* make sure all the channels are stopped */
	struct twl_ndma *dma = to_twl_ndma(dev);
	for (unsigned i = 0; i < TWL_NDMA_NUM_CHAN; i++) {
		if (!(twl_ndma_chan_cnt(&dma->chan[i]) & NDMA_CNT_START_BUSY))
			continue;
		pr_warn("DMA channel %u is still active, force stopping it\n", i);
		iowrite32(0, dma->chan[i].io + REG_NDMA_CNT);
	}
}

static irqreturn_t twl_ndma_chan_irq(int irq, void *dev_data)
{
	struct twl_ndma_chan *ndma_chan = dev_data;

	if (twl_ndma_chan_cnt(ndma_chan) & NDMA_CNT_START_BUSY)
		return IRQ_NONE;

	spin_lock(&ndma_chan->vchan.lock);
	vchan_cookie_complete(&ndma_chan->current_tx->vdesc);
	twl_ndma_chan_start_next(ndma_chan);
	spin_unlock(&ndma_chan->vchan.lock);
	return IRQ_HANDLED;
}

static int twl_ndma_alloc_chan_resources(struct dma_chan *chan)
{
	return 0;
}

static void twl_ndma_free_chan_resources(struct dma_chan *chan)
{
	struct twl_ndma_chan *ndma_chan = to_twl_ndma_chan(chan);
	/* force stop the channel */
	iowrite32(0, ndma_chan->io + REG_NDMA_CNT);
	vchan_free_chan_resources(&ndma_chan->vchan);
}

static int twl_ndma_probe(struct platform_device *pdev)
{
	int err = 0;
	void __iomem *io;
	struct twl_ndma *dma;

	io = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(io))
		return dev_err_probe(&pdev->dev, PTR_ERR(io),
			"failed to acquire IO resource\n");

	dma = devm_kzalloc(&pdev->dev, sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return dev_err_probe(&pdev->dev, -ENOMEM,
			"failed to allocate state memory\n");

	dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));

	/* set up the main DMA structure */
	dma->dma.dev = &pdev->dev;

	dma->dma.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	dma->dma.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	dma->dma.directions = BIT(DMA_MEM_TO_MEM);
	dma->dma.descriptor_reuse = true;
	dma->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;
	dma->dma.device_alloc_chan_resources = twl_ndma_alloc_chan_resources;
	dma->dma.device_free_chan_resources = twl_ndma_free_chan_resources;
	dma->dma.device_terminate_all = twl_ndma_terminate_all;
	dma->dma.device_synchronize = twl_ndma_synchronize;
	dma->dma.device_tx_status = twl_ndma_tx_status;
	dma->dma.device_issue_pending = twl_ndma_issue_pending;
	dma->dma.device_release = twl_ndma_release;

	dma->dma.device_prep_dma_memcpy = twl_ndma_prep_memcpy;
	dma->dma.copy_align = DMAENGINE_ALIGN_4_BYTES;
	dma_cap_set(DMA_MEMCPY, dma->dma.cap_mask);

	dma->dma.device_prep_dma_memset = twl_ndma_prep_memset;
	dma->dma.fill_align = DMAENGINE_ALIGN_4_BYTES;
	dma_cap_set(DMA_MEMSET, dma->dma.cap_mask);
	INIT_LIST_HEAD(&dma->dma.channels);

	/** Global startup */
	iowrite32(NDMA_GCNT_PRIORITY_NUMERICAL, io + REG_NDMA_GCNT);

	for (unsigned i = 0; i < TWL_NDMA_NUM_CHAN; i++) {
		int irq;
		struct twl_ndma_chan *ndma_chan = &dma->chan[i];

		ndma_chan->io = io + REG_NDMA(i);
		ndma_chan->current_tx = NULL;
		ndma_chan->vchan.desc_free = twl_ndma_tx_free;

		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return dev_err_probe(&pdev->dev, irq,
				"failed to get channel %d irq\n", i);

		err = devm_request_irq(&pdev->dev, irq,
			twl_ndma_chan_irq, 0,
			dev_name(&pdev->dev), ndma_chan);
		if (err)
			return dev_err_probe(&pdev->dev, err,
				"failed to register the channel %d irq handler\n", i);

		vchan_init(&ndma_chan->vchan, &dma->dma);
	}

	platform_set_drvdata(pdev, &dma->dma);

	err = dma_async_device_register(&dma->dma);
	if (err)
		return dev_err_probe(&pdev->dev, err, "failed to register DMA device\n");
	return err;
}

static void twl_ndma_remove(struct platform_device *pdev)
{
	struct dma_device *dma = platform_get_drvdata(pdev);
	dma_async_device_unregister(dma);
}

static const struct of_device_id twl_ndma_of_match[] = {
	{ .compatible = "nintendo,twl-ndma" },
	{}
};
MODULE_DEVICE_TABLE(of, twl_ndma_of_match);

static struct platform_driver twl_ndma_driver = {
	.probe	= twl_ndma_probe,
	.remove	= twl_ndma_remove,

	.driver = {
		.name = "twl-ndma",
		.of_match_table = of_match_ptr(twl_ndma_of_match),
	},
};
module_platform_driver(twl_ndma_driver);
