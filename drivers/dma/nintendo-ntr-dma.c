// #include <linux/bitfield.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "dmaengine.h"
#include "virt-dma.h"

#define NTR_DMA_NUM_CHAN	4

#define REG_DMA_SAD	0x00
#define REG_DMA_DAD	0x04
#define REG_DMA_CNT	0x08

#define REG_DMA_BASE(n)	((n) * 12)
#define REG_DMA_FILL(n)	(((n) * 4) + 48)

#define DMA_CNT_DST_INCREMENT	(0u << 21)
#define DMA_CNT_DST_DECREMENT	(1u << 21)
#define DMA_CNT_DST_FIXED	(2u << 21)
#define DMA_CNT_DST_INC_RELOAD	(3u << 21)

#define DMA_CNT_SRC_INCREMENT	(0u << 23)
#define DMA_CNT_SRC_DECREMENT	(1u << 23)
#define DMA_CNT_SRC_FIXED	(2u << 23)

#define DMA_CNT_16BIT		(0u << 26)
#define DMA_CNT_32BIT		(1u << 26)

#define DMA_CNT_IRQ_ENABLE	BIT(30)
#define DMA_CNT_START_BUSY	BIT(31)

#define DMA_CNT_MAX_NUM_WORDS	0x200000
#define DMA_CNT_NUM_WORDS(n)	((n) & (DMA_CNT_MAX_NUM_WORDS - 1))

struct ntr_dma;
struct ntr_dma_tx;

struct ntr_dma_chan {
	void __iomem *io;
	void __iomem *fill;
	dma_cookie_t cookie;
	struct virt_dma_chan vchan;
	struct ntr_dma_tx *current_tx;
};

struct ntr_dma {
	struct dma_device dma;
	struct ntr_dma_chan chan[NTR_DMA_NUM_CHAN];
};

struct ntr_dma_tx {
	struct virt_dma_desc vdesc;
	u32 sad;
	u32 dad;
	u32 cnt;
	u32 fill;
};

static inline struct ntr_dma *to_ntr_dma(struct dma_device *dma)
{
	return container_of(dma, struct ntr_dma, dma);
}

static inline struct ntr_dma_chan *to_ntr_dma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct ntr_dma_chan, vchan.chan);
}

static inline struct ntr_dma_tx *to_ntr_dma_tx(struct virt_dma_desc *desc)
{
	return container_of(desc, struct ntr_dma_tx, vdesc);
}

static u32 ntr_dma_chan_cnt(struct ntr_dma_chan *chan)
{
	return ioread32(chan->io + REG_DMA_CNT);
}

static void ntr_dma_chan_tx(struct ntr_dma_chan *chan, struct ntr_dma_tx *tx)
{
	void __iomem *io = chan->io;
	iowrite32(tx->fill, chan->fill);
	iowrite32(tx->sad, io + REG_DMA_SAD);
	iowrite32(tx->dad, io + REG_DMA_DAD);
	iowrite32(tx->cnt, io + REG_DMA_CNT);
	/* "After changing the Enable bit from 0 to 1, wait 2 clock
	 *  cycles before accessing any DMA related registers." */
	nop();
	nop();
}

static void ntr_dma_tx_free(struct virt_dma_desc *desc)
{
	kfree(to_ntr_dma_tx(desc));
}

static struct dma_async_tx_descriptor *ntr_dma_prep_xfer(struct ntr_dma_chan *chan,
			dma_addr_t dest, dma_addr_t src, size_t len,
			u32 fill, u32 cnt, unsigned long flags)
{
	struct ntr_dma_tx *tx;

	/**
	 * The DMA controller has a limited number of _words_ it
	 * can transfer, leading to different _lengths_ depending
	 * on the bus width.
	 *
	 * A "zero length" transfer actually means "max length" to
	 * the hardware, so we need to special case this check.
	 */
	if (!len) {
		pr_err("zero len request ignored");
		return NULL;
	}

	if (!((dest | src | len) & 3)) {
		cnt |= DMA_CNT_32BIT;
		len >>= 2;
	} else if (!((dest | src | len) & 1)) {
		cnt |= DMA_CNT_16BIT;
		len >>= 1;
	} else {
		pr_err("unaligned transfer requested: %px -> %px, %zu bytes\n",
			(void*)src, (void*)dest, len);
		return NULL;
	}

	if (len > DMA_CNT_MAX_NUM_WORDS) {
		pr_err("transfer length %zu exceeds the maximum %zu\n",
			len, DMA_CNT_MAX_NUM_WORDS);
		return NULL;
	} else if (len == DMA_CNT_MAX_NUM_WORDS) {
		len = 0; /* special case for the max transfer length */
	}

	tx = kzalloc(sizeof(*tx), GFP_NOWAIT);
	if (!tx)
		return NULL;

	tx->sad = src;
	tx->dad = dest;
	tx->cnt = cnt | DMA_CNT_IRQ_ENABLE | DMA_CNT_START_BUSY |
		  DMA_CNT_NUM_WORDS(len);
	tx->fill = fill;
	return vchan_tx_prep(&chan->vchan, &tx->vdesc, flags);
}

static struct dma_async_tx_descriptor *ntr_dma_prep_memcpy(struct dma_chan *chan,
		dma_addr_t dest, dma_addr_t src, size_t len, unsigned long flags)
{
	return ntr_dma_prep_xfer(to_ntr_dma_chan(chan),
		dest, src, len, 0,
		DMA_CNT_SRC_INCREMENT | DMA_CNT_DST_INCREMENT,
		flags
	);
}

static struct dma_async_tx_descriptor *ntr_dma_prep_memset(struct dma_chan *chan,
		dma_addr_t dest, int value, size_t len, unsigned long flags)
{
	return ntr_dma_prep_xfer(to_ntr_dma_chan(chan),
		dest, (u32)(to_ntr_dma_chan(chan)->fill), len, (u32)value,
		DMA_CNT_SRC_INCREMENT | DMA_CNT_DST_INCREMENT,
		flags
	);
}

static int ntr_dma_terminate_all(struct dma_chan *chan)
{
	struct ntr_dma_chan *ntr_chan = to_ntr_dma_chan(chan);
	unsigned long flags;
	LIST_HEAD(list);

	spin_lock_irqsave(&ntr_chan->vchan.lock, flags);
	iowrite32(0, ntr_chan->io + REG_DMA_CNT);	/* stop the active transfer */

	if (ntr_chan->current_tx) {
		vchan_terminate_vdesc(&ntr_chan->current_tx->vdesc);
		ntr_chan->current_tx = NULL;
	}

	vchan_get_all_descriptors(&ntr_chan->vchan, &list);
	list_splice_tail(&list, &ntr_chan->vchan.desc_terminated);
	spin_unlock_irqrestore(&ntr_chan->vchan.lock, flags);
	return 0;
}

static void ntr_dma_synchronize(struct dma_chan *chan)
{
	struct ntr_dma_chan *ntr_chan = to_ntr_dma_chan(chan);
	vchan_synchronize(&ntr_chan->vchan);
}

static enum dma_status ntr_dma_tx_status(struct dma_chan *chan,
					 dma_cookie_t cookie,
					 struct dma_tx_state *state)
{
	struct ntr_dma_chan *ntr_chan = to_ntr_dma_chan(chan);
	unsigned long flags;

	enum dma_status status = dma_cookie_status(chan, cookie, state);

	spin_lock_irqsave(&ntr_chan->vchan.lock, flags);
	if (cookie == ntr_chan->cookie) {
		/* the current DMA transfer is either in progress or done */
		status = (ntr_dma_chan_cnt(ntr_chan) & DMA_CNT_START_BUSY) ?
			DMA_IN_PROGRESS : DMA_COMPLETE;
	} else if (vchan_find_desc(&ntr_chan->vchan, cookie)) {
		status = DMA_IN_PROGRESS;
	} else {
		status = DMA_ERROR;
	}
	spin_unlock_irqrestore(&ntr_chan->vchan.lock, flags);

	return status;
}

static void ntr_dma_chan_start_next(struct ntr_dma_chan *ntr_chan)
{
	struct virt_dma_desc *next_desc = vchan_next_desc(&ntr_chan->vchan);
	if (!next_desc) {
		ntr_chan->current_tx = NULL;
		return;
	}

	list_del(&next_desc->node);
	ntr_chan->cookie = next_desc->tx.cookie;
	ntr_chan->current_tx = to_ntr_dma_tx(next_desc);
	ntr_dma_chan_tx(ntr_chan, ntr_chan->current_tx);
}

static void ntr_dma_issue_pending(struct dma_chan *chan)
{
	struct ntr_dma_chan *ntr_chan = to_ntr_dma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&ntr_chan->vchan.lock, flags);
	if (vchan_issue_pending(&ntr_chan->vchan) && !ntr_chan->current_tx)
		ntr_dma_chan_start_next(ntr_chan);
	spin_unlock_irqrestore(&ntr_chan->vchan.lock, flags);
}

static void ntr_dma_release(struct dma_device *dev)
{
	/* make sure all the channels are stopped */
	struct ntr_dma *dma = to_ntr_dma(dev);
	for (unsigned i = 0; i < NTR_DMA_NUM_CHAN; i++) {
		if (!(ntr_dma_chan_cnt(&dma->chan[i]) & DMA_CNT_START_BUSY))
			continue;
		pr_warn("DMA channel %u is still active, force stopping it\n", i);
		iowrite32(0, dma->chan[i].io + REG_DMA_CNT);
	}
}

static irqreturn_t ntr_dma_chan_irq(int irq, void *dev_data)
{
	struct ntr_dma_chan *ntr_chan = dev_data;

	if (ntr_dma_chan_cnt(ntr_chan) & DMA_CNT_START_BUSY)
		return IRQ_NONE;

	spin_lock(&ntr_chan->vchan.lock);
	vchan_cookie_complete(&ntr_chan->current_tx->vdesc);
	ntr_dma_chan_start_next(ntr_chan);
	spin_unlock(&ntr_chan->vchan.lock);
	return IRQ_HANDLED;
}

static int ntr_dma_alloc_chan_resources(struct dma_chan *chan)
{
	return 0;
}

static void ntr_dma_free_chan_resources(struct dma_chan *chan)
{
	struct ntr_dma_chan *ntr_chan = to_ntr_dma_chan(chan);
	/* force stop the channel */
	iowrite32(0, ntr_chan->io + REG_DMA_CNT);
	vchan_free_chan_resources(&ntr_chan->vchan);
}

static int ntr_dma_probe(struct platform_device *pdev)
{
	int err = 0;
	void __iomem *io;
	struct ntr_dma *dma;

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

	dma->dma.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				   BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	dma->dma.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				   BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	dma->dma.directions = BIT(DMA_MEM_TO_MEM);
	dma->dma.descriptor_reuse = true;
	dma->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;
	dma->dma.device_alloc_chan_resources = ntr_dma_alloc_chan_resources;
	dma->dma.device_free_chan_resources = ntr_dma_free_chan_resources;
	dma->dma.device_terminate_all = ntr_dma_terminate_all;
	dma->dma.device_synchronize = ntr_dma_synchronize;
	dma->dma.device_tx_status = ntr_dma_tx_status;
	dma->dma.device_issue_pending = ntr_dma_issue_pending;
	dma->dma.device_release = ntr_dma_release;

	dma->dma.device_prep_dma_memcpy = ntr_dma_prep_memcpy;
	dma->dma.copy_align = DMAENGINE_ALIGN_2_BYTES;
	dma_cap_set(DMA_MEMCPY, dma->dma.cap_mask);

	dma->dma.device_prep_dma_memset = ntr_dma_prep_memset;
	dma->dma.fill_align = DMAENGINE_ALIGN_2_BYTES;
	dma_cap_set(DMA_MEMSET, dma->dma.cap_mask);
	INIT_LIST_HEAD(&dma->dma.channels);

	for (unsigned i = 0; i < NTR_DMA_NUM_CHAN; i++) {
		int irq;
		struct ntr_dma_chan *ntr_chan = &dma->chan[i];

		ntr_chan->io = io + REG_DMA_BASE(i);
		ntr_chan->fill = io + REG_DMA_FILL(i);
		ntr_chan->current_tx = NULL;
		ntr_chan->vchan.desc_free = ntr_dma_tx_free;

		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return dev_err_probe(&pdev->dev, irq,
				"failed to get channel %d irq\n", i);

		err = devm_request_irq(&pdev->dev, irq,
			ntr_dma_chan_irq, 0,
			dev_name(&pdev->dev), ntr_chan);
		if (err)
			return dev_err_probe(&pdev->dev, err,
				"failed to register the channel %d irq handler\n", i);

		vchan_init(&ntr_chan->vchan, &dma->dma);
	}

	platform_set_drvdata(pdev, &dma->dma);

	err = dma_async_device_register(&dma->dma);
	if (err)
		return dev_err_probe(&pdev->dev, err, "failed to register DMA device\n");
	return err;
}

static void ntr_dma_remove(struct platform_device *pdev)
{
	struct dma_device *dma = platform_get_drvdata(pdev);
	dma_async_device_unregister(dma);
}

static const struct of_device_id ntr_dma_of_match[] = {
	{ .compatible = "nintendo,ntr-dma" },
	{}
};
MODULE_DEVICE_TABLE(of, ntr_dma_of_match);

static struct platform_driver ntr_dma_driver = {
	.probe	= ntr_dma_probe,
	.remove	= ntr_dma_remove,

	.driver = {
		.name = "ntr-dma",
		.of_match_table = of_match_ptr(ntr_dma_of_match),
	},
};
module_platform_driver(ntr_dma_driver);
