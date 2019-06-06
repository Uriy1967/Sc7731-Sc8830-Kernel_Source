/*
 * sound/soc/sprd/dai/sprd-pcm.c
 *
 * SpreadTrum DMA for the pcm stream.
 *
 * Copyright (C) 2012 SpreadTrum Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "sprd-asoc-debug.h"
#define pr_fmt(fmt) pr_sprd_fmt(" PCM ")""fmt

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/stat.h>
#include <linux/of.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include <soc/sprd/dma.h>
#include <soc/sprd/dma_reg.h>
#include <soc/sprd/sprd-audio.h>

#include "sprd-asoc-common.h"
#include "sprd-pcm.h"
#include "vaudio.h"
#include <soc/sprd/i2s.h>
#include "dfm.h"

#ifndef DMA_LINKLIST_CFG_NODE_SIZE
#define DMA_LINKLIST_CFG_NODE_SIZE (sizeof(sprd_dma_desc))
#endif
#define SPRD_PCM_CHANNEL_MAX 2

struct dma_cb_data_t {
	struct snd_pcm_substream *substream;
	/* 0: left channel, 1: right channel */
	int dma_chn_idx;
};

struct sprd_runtime_data {
	int dma_addr_offset;
	struct sprd_pcm_dma_params *params;
	int uid_cid_map[2];
	int int_pos_update[2];
	int burst_len;
	int hw_chan;
	int dma_pos_pre[2];
	int interleaved;
	int irq_called;
#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
	int buffer_in_iram;
#endif
#ifdef CONFIG_SND_VERBOSE_PROCFS
	struct snd_info_entry *proc_info_entry;
#endif
	unsigned int g_cfg_indx[SPRD_PCM_CHANNEL_MAX];
	unsigned int g_cfg_max;
	sprd_dma_desc * *dma_desc[SPRD_PCM_CHANNEL_MAX];
	struct dma_cb_data_t g_dma_cb_data[SPRD_PCM_CHANNEL_MAX];
	sprd_dma_desc *dma_cfg_array;
};

#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
#define SPRD_AUDIO_DMA_NODE_SIZE (1024)
#endif

static const struct snd_pcm_hardware sprd_pcm_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
	    SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_NONINTERLEAVED |
	    SNDRV_PCM_INFO_INTERLEAVED |
	    SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME |
	    SNDRV_PCM_INFO_NO_PERIOD_WAKEUP,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	/* 16bits, stereo-2-channels */
	.period_bytes_min = VBC_FIFO_FRAME_NUM * 4,
	/* non limit */
	.period_bytes_max = VBC_FIFO_FRAME_NUM * 4 * 100,
	.periods_min = 1,
	/* non limit */
	.periods_max = PAGE_SIZE / DMA_LINKLIST_CFG_NODE_SIZE,
	.buffer_bytes_max = VBC_BUFFER_BYTES_MAX,
};

static const struct snd_pcm_hardware sprd_i2s_pcm_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
	    SNDRV_PCM_INFO_MMAP_VALID |
	    SNDRV_PCM_INFO_INTERLEAVED |
	    SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	/* 16bits, stereo-2-channels */
	.period_bytes_min = 8 * 2,
	/* non limit */
	.period_bytes_max = 32 * 2 * 100,
	.periods_min = 1,
	/* non limit */
	.periods_max = PAGE_SIZE / DMA_LINKLIST_CFG_NODE_SIZE,
	.buffer_bytes_max = I2S_BUFFER_BYTES_MAX,
};

atomic_t lightsleep_refcnt;

int sprd_lightsleep_disable(const char *id, int disalbe)
__attribute__ ((weak, alias("__sprd_lightsleep_disable")));

static int __sprd_lightsleep_disable(const char *id, int disable)
{
	sp_asoc_pr_dbg("NO lightsleep control function %d\n", disable);
	return 0;
}

static inline int sprd_is_vaudio(struct snd_soc_dai *cpu_dai)
{
	return ((cpu_dai->driver->id == VAUDIO_MAGIC_ID)
		|| (cpu_dai->driver->id == VAUDIO_MAGIC_ID + 1));
}

static inline int sprd_is_i2s(struct snd_soc_dai *cpu_dai)
{
	return (cpu_dai->driver->id == I2S_MAGIC_ID);
}

static inline int sprd_is_dfm(struct snd_soc_dai *cpu_dai)
{
	return (cpu_dai->driver->id == DFM_MAGIC_ID);
}

static inline const char *sprd_dai_pcm_name(struct snd_soc_dai *cpu_dai)
{
	if (sprd_is_i2s(cpu_dai))
		return "I2S";
	else if (sprd_is_vaudio(cpu_dai))
		return "VAUDIO";
	else if (sprd_is_dfm(cpu_dai))
		return "DFM";

	return "VBC";
}

#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
#ifdef CONFIG_SND_SOC_SPRD_IRAM_BACKUP
static char *s_mem_for_iram;
#endif
static char *s_iram_remap_base;

static int sprd_buffer_iram_backup(void)
{
#ifdef CONFIG_SND_SOC_SPRD_IRAM_BACKUP
	void __iomem *iram_start;
	sp_asoc_pr_dbg("%s 0x%x\n", __func__, (int)s_mem_for_iram);
#endif
	if (!s_iram_remap_base) {
		s_iram_remap_base =
		    ioremap_nocache(SPRD_IRAM_ALL_PHYS, SPRD_IRAM_ALL_SIZE);
	}
#ifdef CONFIG_SND_SOC_SPRD_IRAM_BACKUP
	if (!s_mem_for_iram) {
		s_mem_for_iram = kzalloc(SPRD_IRAM_ALL_SIZE, GFP_KERNEL);
	} else {
		sp_asoc_pr_dbg("IRAM is Backup, Be Careful use IRAM!\n");
		return 0;
	}
	if (!s_mem_for_iram) {
		pr_err("ERR:IRAM Backup Error!\n");
		return -ENOMEM;
	}
	iram_start = (void __iomem *)(s_iram_remap_base);
	memcpy_fromio(s_mem_for_iram, iram_start, SPRD_IRAM_ALL_SIZE);
#endif
	return 0;
}

static int sprd_buffer_iram_restore(void)
{
#ifdef CONFIG_SND_SOC_SPRD_IRAM_BACKUP
	void __iomem *iram_start;
	sp_asoc_pr_dbg("%s 0x%x\n", __func__, (int)s_mem_for_iram);
	if (!s_mem_for_iram) {
		pr_err("ERR:IRAM not Backup\n");
		return 0;
	}
	iram_start = (void __iomem *)(s_iram_remap_base);
	memcpy_toio(iram_start, s_mem_for_iram, SPRD_IRAM_ALL_SIZE);
	kfree(s_mem_for_iram);
	s_mem_for_iram = 0;
#endif
	return 0;
}
#endif

static inline int sprd_pcm_is_interleaved(struct snd_pcm_runtime *runtime)
{
	return (runtime->access == SNDRV_PCM_ACCESS_RW_INTERLEAVED ||
		runtime->access == SNDRV_PCM_ACCESS_MMAP_INTERLEAVED);
}

#define PCM_DIR_NAME(stream) (stream == SNDRV_PCM_STREAM_PLAYBACK ?\
"Playback" : "Captrue")

static int sprd_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *srtd = substream->private_data;
	struct sprd_runtime_data *rtd;
	struct i2s_config *config;
	int burst_len;
	int hw_chan;
	int ret;

	sp_asoc_pr_info("%s Open %s\n", sprd_dai_pcm_name(srtd->cpu_dai),
			PCM_DIR_NAME(substream->stream));
	if (sprd_is_i2s(srtd->cpu_dai)) {
		snd_soc_set_runtime_hwparams(substream, &sprd_i2s_pcm_hardware);
		config = srtd->cpu_dai->ac97_pdata;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			burst_len = I2S_FIFO_DEPTH - config->tx_watermark;
		else
			burst_len = config->rx_watermark;
		burst_len <<= config->byte_per_chan;
		hw_chan = 1;
	} else {
		snd_soc_set_runtime_hwparams(substream, &sprd_pcm_hardware);
		burst_len = (VBC_FIFO_FRAME_NUM * 4);
		hw_chan = 2;
	}
	/*
	 * For mysterious reasons (and despite what the manual says)
	 * playback samples are lost if the DMA count is not a multiple
	 * of the DMA burst size.  Let's add a rule to enforce that.
	 */
	ret = snd_pcm_hw_constraint_step(runtime, 0,
					 SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					 burst_len);
	if (ret)
		goto out;

	ret = snd_pcm_hw_constraint_step(runtime, 0,
					 SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
					 burst_len);
	if (ret)
		goto out;

	ret = snd_pcm_hw_constraint_integer(runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		goto out;

	ret = -ENOMEM;
	rtd = kzalloc(sizeof(*rtd), GFP_KERNEL);
	if (!rtd)
		goto out;
	if (sprd_is_dfm(srtd->cpu_dai) || sprd_is_vaudio(srtd->cpu_dai)) {
		runtime->private_data = rtd;
		ret = 0;
		goto out;
	}
#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
	if (sprd_is_i2s(srtd->cpu_dai)
	    || !((substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		 && 0 == sprd_buffer_iram_backup())) {
#endif
		if (atomic_inc_return(&lightsleep_refcnt) == 1)
			sprd_lightsleep_disable("audio", 1);
#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
	} else {
		runtime->hw.periods_max =
		    SPRD_AUDIO_DMA_NODE_SIZE / DMA_LINKLIST_CFG_NODE_SIZE;
		runtime->hw.buffer_bytes_max =
		    SPRD_IRAM_ALL_SIZE - (2 * SPRD_AUDIO_DMA_NODE_SIZE);
		rtd->buffer_in_iram = 1;
	}
#endif

	rtd->uid_cid_map[0] = rtd->uid_cid_map[1] = -1;

	rtd->burst_len = burst_len;
	rtd->hw_chan = hw_chan;

	runtime->private_data = rtd;
	ret = 0;
	goto out;

out:
	return ret;
}

static int sprd_pcm_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct sprd_runtime_data *rtd = runtime->private_data;
	struct snd_soc_pcm_runtime *srtd = substream->private_data;

	sp_asoc_pr_info("%s Close %s\n", sprd_dai_pcm_name(srtd->cpu_dai),
			PCM_DIR_NAME(substream->stream));

	if (sprd_is_dfm(srtd->cpu_dai) || sprd_is_vaudio(srtd->cpu_dai)) {
		kfree(rtd);
		return 0;
	}
#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
	if (rtd->buffer_in_iram)
		sprd_buffer_iram_restore();
	else {
#endif
		if (!atomic_dec_return(&lightsleep_refcnt))
			sprd_lightsleep_disable("audio", 0);
#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
	}
#endif

	kfree(rtd);

	return 0;
}

int dma_cfg_hw(struct sprd_runtime_data *rtd, int dma_chn_idx,
	       sprd_dma_desc *dma_desc)
{
	if (dma_chn_idx > SPRD_PCM_CHANNEL_MAX) {
		pr_err("wrong channl count %d\n", dma_chn_idx);
		return -1;
	}
	sci_dma_config((u32) (rtd->uid_cid_map[dma_chn_idx]),
		       dma_desc, 1, NULL);

	return 0;
}

static void sprd_pcm_dma_irq_ch(void *dma_ch, void *dev_id)
{
	struct dma_cb_data_t *cb_data = dev_id;
	int dma_chan_idx = cb_data->dma_chn_idx;
	struct snd_pcm_substream *substream = cb_data->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct sprd_runtime_data *rtd = runtime->private_data;
	struct sci_dma_handle *dma_handle = dma_ch;
	struct sprd_pcm_dma_params *dma = rtd->params;
	int i = 0;
	u32 cfg_idx = 0;

	if (!rtd->irq_called) {
		rtd->irq_called = 1;
		sp_asoc_pr_info("IRQ CALL\n");
	}

	if (dma_handle->dma_done == TRANS_DONE) {
		if (rtd->g_cfg_indx[dma_chan_idx] == rtd->g_cfg_max)
			rtd->g_cfg_indx[dma_chan_idx] = 0;
		cfg_idx = rtd->g_cfg_indx[dma_chan_idx];
		dma_cfg_hw(rtd, dma_chan_idx,
			   rtd->dma_desc[dma_chan_idx][cfg_idx]);
		sci_dma_register_irqhandle(rtd->uid_cid_map[dma_chan_idx],
					   TRANS_BLOCK_DONE,
					   sprd_pcm_dma_irq_ch,
					   &(rtd->g_dma_cb_data[dma_chan_idx]));
		rtd->g_cfg_indx[dma_chan_idx]++;
		sci_dma_start(rtd->uid_cid_map[dma_chan_idx],
		      dma->channels[dma_chan_idx]);
	}

	if (rtd->hw_chan == 1)
		goto irq_fast;

	for (i = 0; i < 2; i++) {
		if (dma_handle->dma_chn == rtd->uid_cid_map[i]) {
			rtd->int_pos_update[i] = 1;
			if (rtd->uid_cid_map[1 - i] >= 0) {
				if (rtd->int_pos_update[1 - i])
					goto irq_ready;
			} else
				goto irq_ready;
		}
	}
	goto irq_ret;
irq_ready:
	rtd->int_pos_update[0] = 0;
	rtd->int_pos_update[1] = 0;
irq_fast:

	snd_pcm_period_elapsed(substream);
irq_ret:
	return;
}

/*
 * proc interface
 */

#ifdef CONFIG_SND_VERBOSE_PROCFS
static void sprd_pcm_proc_dump_reg(int id, struct snd_info_buffer *buffer)
{
	u32 reg, base_reg;
	u32 offset = sci_dma_dump_reg(id, &base_reg);
	for (reg = base_reg; reg < base_reg + offset; reg += 0x10) {
		snd_iprintf(buffer, "0x%04x | 0x%08x 0x%08x 0x%08x 0x%08x\n",
			    (unsigned int)(reg - base_reg),
			    __raw_readl((void __iomem *)(reg + 0x00)),
			    __raw_readl((void __iomem *)(reg + 0x04)),
			    __raw_readl((void __iomem *)(reg + 0x08)),
			    __raw_readl((void __iomem *)(reg + 0x0C))
		    );
	}
}

static void sprd_pcm_proc_read(struct snd_info_entry *entry,
			       struct snd_info_buffer *buffer)
{
	struct snd_pcm_substream *substream = entry->private_data;
	struct sprd_runtime_data *rtd = substream->runtime->private_data;
	int i;

	for (i = 0; i < rtd->hw_chan; i++) {
		if (rtd->uid_cid_map[i] >= 0) {
			snd_iprintf(buffer, "Channel%d Config\n",
				    rtd->uid_cid_map[i]);
			sprd_pcm_proc_dump_reg(rtd->uid_cid_map[i], buffer);
		}
	}
}

static void sprd_pcm_proc_init(struct snd_pcm_substream *substream)
{
	struct snd_info_entry *entry;
	struct snd_pcm_str *pstr = substream->pstr;
	struct snd_pcm *pcm = pstr->pcm;
	struct sprd_runtime_data *rtd = substream->runtime->private_data;

	entry = snd_info_create_card_entry(pcm->card, "DMA",
				pstr->proc_root);
	if (entry != NULL) {
		snd_info_set_text_ops(entry, substream, sprd_pcm_proc_read);
		if (snd_info_register(entry) < 0) {
			snd_info_free_entry(entry);
			entry = NULL;
		}
	}
	rtd->proc_info_entry = entry;
}

static void sprd_pcm_proc_done(struct snd_pcm_substream *substream)
{
	struct sprd_runtime_data *rtd = substream->runtime->private_data;
	snd_info_free_entry(rtd->proc_info_entry);
	rtd->proc_info_entry = NULL;
}
#else /* !CONFIG_SND_VERBOSE_PROCFS */
static inline void sprd_pcm_proc_init(struct snd_pcm_substream *substream)
{
}

static void sprd_pcm_proc_done(struct snd_pcm_substream *substream)
{
}
#endif

static int sprd_pcm_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct sprd_runtime_data *rtd = runtime->private_data;
	struct snd_soc_pcm_runtime *srtd = substream->private_data;
	struct sprd_pcm_dma_params *dma;
	size_t totsize = params_buffer_bytes(params);
	size_t period = params_period_bytes(params);
	size_t periods = params_periods(params);
	dma_addr_t dma_buff_phys[2];	/*, next_desc_phys[2]; */
	struct i2s_config *config = NULL;
	int ret = 0;
	int i, j = 0;
	int used_chan_count;
	int chan_id;
	int test_idx = 0;

	sp_asoc_pr_dbg("%s\n", __func__);

	dma = snd_soc_dai_get_dma_data(srtd->cpu_dai, substream);
	if (!dma)
		goto no_dma;

	used_chan_count = params_channels(params);

	sp_asoc_pr_info("chan=%d totsize=%d period=%d\n", used_chan_count,
			totsize, period);
	memset(&(rtd->g_dma_cb_data), 0, sizeof(rtd->g_dma_cb_data));
	for (i = 0; i < used_chan_count; i++)
		rtd->g_cfg_indx[i] = 0;
	rtd->g_cfg_max = 0;
	if (sprd_is_i2s(srtd->cpu_dai)) {
		config = srtd->cpu_dai->ac97_pdata;
		used_chan_count = rtd->hw_chan;
	} else {
		rtd->interleaved = (used_chan_count == 2)
		    && sprd_pcm_is_interleaved(runtime);
		if (rtd->interleaved) {
			sp_asoc_pr_dbg("Interleaved Access\n");
			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
				dma->desc.src_step = 4;
			else
				dma->desc.des_step = 4;

		} else {
			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
				dma->desc.src_step = 2;
			else
				dma->desc.des_step = 2;
		}
	}

	/* this may get called several times by oss emulation
	 * with different params */
	if (rtd->params == NULL) {
		rtd->params = dma;
		for (i = 0; i < used_chan_count; i++) {
			chan_id = sci_dma_request(dma->name, FULL_DMA_CHN);
			if (chan_id < 0) {
				pr_err("ERR:PCM Request DMA Error %d\n",
				       dma->channels[i]);
				for (i--; i >= 0; i--) {
					sci_dma_free(rtd->uid_cid_map[i]);
					rtd->uid_cid_map[i] = -1;
					rtd->params = NULL;
				}
				goto hw_param_err;
			}
			rtd->uid_cid_map[i] = chan_id;
			pr_info("Chan%d DMA ID=%d\n",
				rtd->uid_cid_map[i], rtd->params->channels[i]);
		}
	}

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);

	runtime->dma_bytes = totsize;

	rtd->dma_cfg_array =
	    kzalloc(used_chan_count * periods * sizeof(sprd_dma_desc),
		    GFP_KERNEL);
	if (!rtd->dma_cfg_array) {
		pr_err("%s dma_cfg_array alloc failed\n", __func__);
		goto hw_param_err;
	}
	for (i = 0; i < used_chan_count; i++) {
		rtd->dma_desc[i] =
		    kzalloc(periods * sizeof(sprd_dma_desc *), GFP_KERNEL);
		if (!rtd->dma_desc[i]) {
			pr_err
			    ("%s rtd->dma_desc[%d], periods=%d alloc failed\n",
			     __func__, i, periods);
			goto hw_param_err;
		}
	}

	for (i = 0; i < used_chan_count; i++) {
		for (j = 0; j < periods; j++) {
			rtd->dma_desc[i][j] =
			    rtd->dma_cfg_array + i * periods + j;
		}
	}
	dma_buff_phys[0] = runtime->dma_addr;
	rtd->dma_addr_offset = (totsize / used_chan_count);
	if (sprd_pcm_is_interleaved(runtime))
		rtd->dma_addr_offset = 2;
	dma_buff_phys[1] = runtime->dma_addr + rtd->dma_addr_offset;
	j = 0;
	do {
		for (i = 0; i < used_chan_count; i++) {
			rtd->dma_desc[i][j]->datawidth = dma->desc.datawidth;
			if (sprd_is_i2s(srtd->cpu_dai)) {
				if (substream->stream ==
				    SNDRV_PCM_STREAM_PLAYBACK) {
					rtd->dma_desc[i][j]->fragmens_len =
					    (I2S_FIFO_DEPTH -
					     config->tx_watermark) *
					    rtd->dma_desc[i][j]->datawidth;
				} else {
					rtd->dma_desc[i][j]->fragmens_len =
					    config->rx_watermark *
					    rtd->dma_desc[i][j]->datawidth;
				}
			} else {
				rtd->dma_desc[i][j]->fragmens_len =
				    dma->desc.fragmens_len;
			}
			rtd->dma_desc[i][j]->block_len =
			    period / used_chan_count;
			rtd->dma_desc[i][j]->transcation_len =
			    rtd->dma_desc[i][j]->block_len;
			rtd->dma_desc[i][j]->req_mode = FRAG_REQ_MODE;
			rtd->dma_desc[i][j]->src_step = dma->desc.src_step;
			rtd->dma_desc[i][j]->des_step = dma->desc.des_step;
			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
				rtd->dma_desc[i][j]->src_addr =
				    dma_buff_phys[i];
				rtd->dma_desc[i][j]->des_addr =
				    dma->dev_paddr[i];
			} else {
				rtd->dma_desc[i][j]->src_addr =
				    dma->dev_paddr[i];
				rtd->dma_desc[i][j]->des_addr =
				    dma_buff_phys[i];
			}
			dma_buff_phys[i] += rtd->dma_desc[i][j]->block_len;
			if (rtd->interleaved)
				dma_buff_phys[i] +=
				    rtd->dma_desc[i][j]->block_len;
		}

		if (period > totsize)
			period = totsize;
		j++;
	} while (totsize -= period);

	pr_info("Node Size:%d\n", j);
	rtd->g_cfg_max = j;

	for (i = 0; i < used_chan_count; i++) {
		rtd->g_dma_cb_data[i].dma_chn_idx = i;
		rtd->g_dma_cb_data[i].substream = substream;
		dma_cfg_hw(rtd, i,
			   rtd->dma_desc[i][rtd->g_cfg_indx[i]]);
		sci_dma_register_irqhandle(rtd->uid_cid_map[i],
					   TRANS_BLOCK_DONE,
					   sprd_pcm_dma_irq_ch,
					   &(rtd->g_dma_cb_data[i]));
		pr_info("Register IRQ for DMA Chan ID %d\n",
			rtd->uid_cid_map[i]);
		rtd->g_cfg_indx[i]++;
	}
	for (i = 0; i < used_chan_count; i++) {
		for (test_idx = 0; test_idx < j; test_idx++) {
			pr_info("%s dma_desc[%d][%d]: datawidth %d, src_addr %#x, des_addr %#x, fragmens_len %#x, block_len %#x, src_step %#x, des_step %#x, req_mode %#x, transn_len %#x, src_frag_step %#x, dst_frag_step %#x, wrap_ptr %#x, wrap_to %#x, src_blk_step %#x, dst_blk_step %#x, linklist_ptr %#x, is_end %#x, uid_cid_map[%d]\n",
			     __func__, i, test_idx,
			     rtd->dma_desc[i][test_idx]->datawidth,
			     rtd->dma_desc[i][test_idx]->src_addr,
			     rtd->dma_desc[i][test_idx]->des_addr,
			     rtd->dma_desc[i][test_idx]->fragmens_len,
			     rtd->dma_desc[i][test_idx]->block_len,
			     rtd->dma_desc[i][test_idx]->src_step,
			     rtd->dma_desc[i][test_idx]->des_step,
			     rtd->dma_desc[i][test_idx]->req_mode,
			     rtd->dma_desc[i][test_idx]->transcation_len,
			     rtd->dma_desc[i][test_idx]->src_frag_step,
			     rtd->dma_desc[i][test_idx]->dst_frag_step,
			     rtd->dma_desc[i][test_idx]->wrap_ptr,
			     rtd->dma_desc[i][test_idx]->wrap_to,
			     rtd->dma_desc[i][test_idx]->src_blk_step,
			     rtd->dma_desc[i][test_idx]->dst_blk_step,
			     rtd->dma_desc[i][test_idx]->linklist_ptr,
			     rtd->dma_desc[i][test_idx]->is_end,
			     rtd->uid_cid_map[i]
			    );
		}
		pr_info("dma_cb_data[%d]: %d\n", i,
			rtd->g_dma_cb_data[i].dma_chn_idx);
	}

	sprd_pcm_proc_init(substream);

	goto ok_go_out;

no_dma:
	sp_asoc_pr_dbg("no dma\n");
	rtd->params = NULL;
	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);
	runtime->dma_bytes = totsize;
	return ret;
hw_param_err:
	sp_asoc_pr_dbg("hw_param_err\n");
	for (i = 0; i < used_chan_count; i++) {
		kfree(rtd->dma_desc[i]);
		rtd->dma_desc[i] = NULL;
	}
	kfree(rtd->dma_cfg_array);
	rtd->dma_cfg_array = NULL;
ok_go_out:
	sp_asoc_pr_dbg("return %i\n", ret);

	return ret;
}

static int sprd_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct sprd_runtime_data *rtd = substream->runtime->private_data;
	struct sprd_pcm_dma_params *dma = rtd->params;
	int i;

	snd_pcm_set_runtime_buffer(substream, NULL);

	if (dma) {
		for (i = 0; i < rtd->hw_chan; i++) {
			if (rtd->uid_cid_map[i] >= 0) {
				sci_dma_free(rtd->uid_cid_map[i]);
				rtd->uid_cid_map[i] = -1;
			}
		}
		rtd->params = NULL;
		/* globle var clear */
	}

	for (i = 0; i < rtd->hw_chan; i++) {
		kfree(rtd->dma_desc[i]);
		rtd->dma_desc[i] = NULL;
	}
	kfree(rtd->dma_cfg_array);
	rtd->dma_cfg_array = NULL;
	sprd_pcm_proc_done(substream);

	return 0;
}

static int sprd_pcm_prepare(struct snd_pcm_substream *substream)
{
	return 0;
}

static int sprd_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct sprd_runtime_data *rtd = substream->runtime->private_data;
	struct sprd_pcm_dma_params *dma = rtd->params;
	int ret = 0;
	int i;

	if (!dma) {
		sp_asoc_pr_dbg("no trigger");
		return 0;
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		for (i = 0; i < rtd->hw_chan; i++) {
			if (rtd->uid_cid_map[i] >= 0)
				sci_dma_start(rtd->uid_cid_map[i],
					      dma->channels[i]);
		}
		pr_info("%s S\n", __func__);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		for (i = 0; i < rtd->hw_chan; i++) {
			if (rtd->uid_cid_map[i] >= 0) {
				sci_dma_stop(rtd->uid_cid_map[i],
					     dma->channels[i]);
			}
		}
		rtd->irq_called = 0;
		pr_info("%s E\n", __func__);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static snd_pcm_uframes_t sprd_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct sprd_runtime_data *rtd = runtime->private_data;
	snd_pcm_uframes_t x;
	int now_pointer;
	int bytes_of_pointer = 0;
	int shift = 1;
	int sel_max = 0;
	struct snd_soc_pcm_runtime *srtd = substream->private_data;
	if (sprd_is_dfm(srtd->cpu_dai) || sprd_is_vaudio(srtd->cpu_dai))
		return 0;

	if (rtd->interleaved)
		shift = 0;

	if (rtd->uid_cid_map[0] >= 0) {
		now_pointer = sprd_pcm_dma_get_addr(rtd->uid_cid_map[0],
						    substream) -
		    runtime->dma_addr;
		bytes_of_pointer = now_pointer;
	}
	if (rtd->uid_cid_map[1] >= 0) {
		now_pointer = sprd_pcm_dma_get_addr(rtd->uid_cid_map[1],
						    substream) -
		    runtime->dma_addr - rtd->dma_addr_offset;
		if (!bytes_of_pointer) {
			bytes_of_pointer = now_pointer;
		} else {
			sel_max = (bytes_of_pointer < rtd->dma_pos_pre[0]);
			sel_max ^= (now_pointer < rtd->dma_pos_pre[1]);
			rtd->dma_pos_pre[0] = bytes_of_pointer;
			rtd->dma_pos_pre[1] = now_pointer;
			if (sel_max) {
				bytes_of_pointer =
				    max(bytes_of_pointer, now_pointer) << shift;
			} else {
				bytes_of_pointer =
				    min(bytes_of_pointer, now_pointer) << shift;
			}
		}
	}

	x = bytes_to_frames(runtime, bytes_of_pointer);

	if (x == runtime->buffer_size)
		x = 0;

	return x;
}

static int sprd_pcm_mmap(struct snd_pcm_substream *substream,
			 struct vm_area_struct *vma)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *srtd = substream->private_data;

	if (sprd_is_dfm(srtd->cpu_dai) || sprd_is_vaudio(srtd->cpu_dai)) {
		sp_asoc_pr_dbg("no mmap");
		return 0;
	}
#ifndef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
	return dma_mmap_writecombine(substream->pcm->card->dev, vma,
				     runtime->dma_area,
				     runtime->dma_addr, runtime->dma_bytes);
#else
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start,
			       runtime->dma_addr >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start, vma->vm_page_prot);
#endif
}

static struct snd_pcm_ops sprd_pcm_ops = {
	.open = sprd_pcm_open,
	.close = sprd_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = sprd_pcm_hw_params,
	.hw_free = sprd_pcm_hw_free,
	.prepare = sprd_pcm_prepare,
	.trigger = sprd_pcm_trigger,
	.pointer = sprd_pcm_pointer,
	.mmap = sprd_pcm_mmap,
};

static int sprd_pcm_preallocate_dma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_soc_pcm_runtime *rtd = pcm->private_data;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	size_t size = AUDIO_BUFFER_BYTES_MAX;
	buf->dev.type = SNDRV_DMA_TYPE_DEV;
	buf->dev.dev = pcm->card->dev;
	if (sprd_is_i2s(rtd->cpu_dai))
		size = I2S_BUFFER_BYTES_MAX;
	else
		size = VBC_BUFFER_BYTES_MAX;
#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
	if (sprd_is_i2s(rtd->cpu_dai)
	    || !((substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		 && 0 == sprd_buffer_iram_backup())) {
#endif
		buf->private_data = NULL;
		buf->area = dma_alloc_coherent(pcm->card->dev, size,
					       &buf->addr, GFP_KERNEL);
#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
	} else {
		buf->private_data = buf;
		buf->area = (void *)(s_iram_remap_base);
		buf->addr = SPRD_IRAM_ALL_PHYS;
		size = SPRD_IRAM_ALL_SIZE - (2 * SPRD_AUDIO_DMA_NODE_SIZE);
	}
#endif
	if (!buf->area)
		return -ENOMEM;
	buf->bytes = size;
	return 0;
}

static u64 sprd_pcm_dmamask = DMA_BIT_MASK(32);
static struct snd_dma_buffer *save_p_buf;
#define VBC_CHAN (2)
static struct snd_dma_buffer *save_c_buf[VBC_CHAN] = { 0 };

static int sprd_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	struct snd_pcm *pcm = rtd->pcm;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_pcm_substream *substream;
	int ret = 0;

	sp_asoc_pr_dbg("%s %s\n", __func__, sprd_dai_pcm_name(cpu_dai));

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &sprd_pcm_dmamask;
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);

	substream = pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	if (substream) {
		struct snd_dma_buffer *buf = &substream->dma_buffer;
		if (sprd_is_i2s(cpu_dai) || !save_p_buf) {
			ret = sprd_pcm_preallocate_dma_buffer(pcm,
					SNDRV_PCM_STREAM_PLAYBACK);
			if (ret)
				goto out;
			if (!sprd_is_i2s(cpu_dai))
				save_p_buf = buf;
			sp_asoc_pr_dbg("Playback alloc memery\n");
		} else {
			memcpy(buf, save_p_buf, sizeof(*buf));
			sp_asoc_pr_dbg("Playback share memery\n");
		}
	}

	substream = pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
	if (substream) {
		int id = cpu_dai->driver->id;
		struct snd_dma_buffer *buf = &substream->dma_buffer;
		if (sprd_is_vaudio(cpu_dai))
			id -= VAUDIO_MAGIC_ID;
		if (sprd_is_i2s(cpu_dai) || !save_c_buf[id]) {
			ret = sprd_pcm_preallocate_dma_buffer(pcm,
					SNDRV_PCM_STREAM_CAPTURE);
			if (ret)
				goto out;
			if (!sprd_is_i2s(cpu_dai))
				save_c_buf[id] = buf;
			sp_asoc_pr_dbg("Capture alloc memery %d\n", id);
		} else {
			memcpy(buf, save_c_buf[id], sizeof(*buf));
			sp_asoc_pr_dbg("Capture share memery %d\n", id);
		}
	}
out:
	sp_asoc_pr_dbg("return %i\n", ret);
	return ret;
}

static void sprd_pcm_free_dma_buffers(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *buf;
	int stream;
	int i;

	sp_asoc_pr_dbg("%s\n", __func__);

	for (stream = 0; stream < 2; stream++) {
		substream = pcm->streams[stream].substream;
		if (!substream)
			continue;
		buf = &substream->dma_buffer;
		if (!buf->area)
			continue;
#ifdef CONFIG_SND_SOC_SPRD_AUDIO_BUFFER_USE_IRAM
		if (buf->private_data)
			sprd_buffer_iram_restore();
		else
#endif
			dma_free_coherent(pcm->card->dev, buf->bytes,
					  buf->area, buf->addr);
		buf->area = NULL;
		if (buf == save_p_buf)
			save_p_buf = 0;
		for (i = 0; i < VBC_CHAN; i++) {
			if (buf == save_c_buf[i])
				save_c_buf[i] = 0;
		}
	}
}

static struct snd_soc_platform_driver sprd_soc_platform = {
	.ops = &sprd_pcm_ops,
	.pcm_new = sprd_pcm_new,
	.pcm_free = sprd_pcm_free_dma_buffers,
};

static int sprd_soc_platform_probe(struct platform_device *pdev)
{
	return snd_soc_register_platform(&pdev->dev, &sprd_soc_platform);
}

static int sprd_soc_platform_remove(struct platform_device *pdev)
{
	snd_soc_unregister_platform(&pdev->dev);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id sprd_pcm_of_match[] = {
	{.compatible = "sprd,sprd-pcm",},
	{},
};

MODULE_DEVICE_TABLE(of, sprd_pcm_of_match);
#endif

static struct platform_driver sprd_pcm_driver = {
	.driver = {
		   .name = "sprd-pcm-audio",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(sprd_pcm_of_match),
		   },

	.probe = sprd_soc_platform_probe,
	.remove = sprd_soc_platform_remove,
};

static int __init sprd_pcm_driver_init(void)
{
	return platform_driver_register(&sprd_pcm_driver);
}

late_initcall(sprd_pcm_driver_init);

MODULE_DESCRIPTION("SPRD ASoC PCM DMA");
MODULE_AUTHOR("Ken Kuang <ken.kuang@spreadtrum.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sprd-audio");
