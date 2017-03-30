#include "../fscftl.h"
#include "ppa-ops.h"
#include "../writecache/wcb-mngr.h"

/*********************************************************************
 * How to use submit_bio to submit PPA command
 * form a rqd
 * 		1.fullfill field of rqd.xx
 *		2.callback fn(ctx)
 * 		3.ppalist
 * 		4.metadata
 * 		5.form a bio, the R/W data should be in bio->bvecpage
 * 		6.call core io handle fn
 *
 *********************************************************************/

void get_ppa_each_region(geo_ppa *ppa, u8 *ch, u8 *sec, 
                                u8 *pl, u8 *lun, u16 *pg, u16 *blk) 
{
    *ch = ppa->nand.ch;
    *sec = ppa->nand.sec;
    *pl = ppa->nand.pl;
    *lun = ppa->nand.lun;
    *pg = ppa->nand.pg;
    *blk = ppa->nand.blk;
}


void set_ppa_nand_addr(geo_ppa *ppa, u8 ch, u8 sec, 
                            u8 pl, u8 lun, u16 pg, u16 blk)
{
        ppa->nand.ch  = ch;
        ppa->nand.sec = sec;
        ppa->nand.pl  = pl;
        ppa->nand.lun = lun;
        ppa->nand.pg  = pg;
        ppa->nand.blk = blk;
}

void ppa_step(geo_ppa *ppa, bool mode)
{
        if (mode == CH_INCRS) {
                ppa->ppa++;
        } else {
                bool carry;
                u8 ch, sec, pl, lun;
                u16 pg, blk;

                get_ppa_each_region(ppa, &ch, &sec, &pl, &lun, &pg, &blk);

        	INCRE_BOUNDED(sec, EP_BITS, carry);
        	IF_CARRY_THEN_INCRE_BOUNDED(carry, pl, PL_BITS);
        	IF_CARRY_THEN_INCRE_BOUNDED(carry, ch, CH_BITS);
        	IF_CARRY_THEN_INCRE_BOUNDED(carry, lun, LN_BITS);
        	IF_CARRY_THEN_INCRE_BOUNDED(carry, pg, PG_BITS);
        	IF_CARRY_THEN_INCRE_BOUNDED(carry, blk,BL_BITS);

                set_ppa_nand_addr(ppa, ch, sec, pl, lun, pg, blk);
        }
}

u32 pos_to_ppa(struct wcb_lun_entity *entity, bool mode)
{
        if (mode == CH_INCRS) {
	        return entity->baddr.ppa + entity->pos;
        } else {
                u8 ch, sec, pl;
                geo_ppa ppa;
                geo_pos temp;
                temp.all = entity->pos;
                sec = temp.bits.sec;
                pl  = temp.bits.pl;
                ch  = temp.bits.ch;

                ppa.ppa = entity->baddr.ppa; // blk pg lun
                ppa.nand.ch = ch;
                ppa.nand.sec = sec;
                ppa.nand.pl = pl;

                return ppa.ppa;
        }
}

static void print_ppa_cqe(struct nvme_ppa_command *cmd, u64 result, int status)
{
	printk("New PPA command CQE complete\n");
	
	switch (cmd->opcode) {
	case NVM_OP_ESPPA:
		printk("  ersppa");
		break;
	case NVM_OP_WRPPA:
		printk("  wrppa");
		break;
	case NVM_OP_RDPPA:
		printk("  rdppa");
		break;
	case NVM_OP_WRRAW:
		printk("  wrpparaw");
		break;
	case NVM_OP_RDRAW:
		printk("  rdpparaw");
		break;
	default:
		printk("  opcode invalid!!!");
	}

	printk("  nr_ppas:%d  control:%x\n", (cmd->nlb+1), cmd->control);
	printk("  status:0x%x result.u64:0x%llx\n", status, result);
	printk("\n");
}

/*
 * Returns 0 on success.  If the result is negative, it's a Linux error code;
 * if the result is positive, it's an NVM Express status code
 */
static void nvme_ppa_completion(struct request *req, int error)
{
	//void *ctx = req->end_io_data;
	int status = error;						/* No phase tag */
	u64 result = nvme_req(req)->result.u64; /* 64bit completion btmap */
	struct nvme_command *cmd = nvme_req(req)->cmd;	/* original sqe */

	// goto free/release some source
	print_ppa_cqe((struct nvme_ppa_command *)cmd, result, status);

	kfree(nvme_req(req)->cmd);
	blk_mq_free_request(req);
}

static void nvm_end_io_sync(struct nvm_rq *rqd)
{
	struct completion *waiting = rqd->private;

	complete(waiting);
}

int nvm_ersppa_sync(struct nvm_exdev *dev, struct ppa_addr *ppas, int nr_ppas)
{
        int ret = 0;
        struct nvm_rq rqd;

        DECLARE_COMPLETION_ONSTACK(wait);

        memset(&rqd, 0, sizeof(struct nvm_rq));

        // 1.fullfill field of rqd.xx
        rqd.opcode = NVM_OP_ERASE;
        rqd.flags = NVM_IO_SNGL_ACCESS;

        // 2.callback fn(ctx)
        rqd.end_io = nvm_end_io_sync;
        rqd.private = &wait;

        // 3.ppalist
        ret = set_rqd_nand_ppalist(dev, &rqd, ppas, nr_ppas);
        if (ret)
                return ret;

        // 4.metadata

        // 5.form a bio

        // 6.call core io handle fn
        ret = dev->ops->submit_io(dev, &rqd);  //nvm_submit_ppa
        if (ret) {
                pr_err("erase I/O submission falied: %d\n", ret);
                goto free_ppa_list;
        }

        wait_for_completion_io(&wait);

free_ppa_list:
        free_rqd_nand_ppalist(dev, &rqd);
        return ret;
}

int nvm_rdpparaw_sync(struct nvm_exdev *exdev, 
                             struct physical_address *ppa, int nr_ppas, 
                             u16 ctrl, void *databuf, void *metabuf)
{
	int i;
	u64 *ppalist = NULL;
	dma_addr_t dma_ppalist, dma_meta;
	struct nvme_ppa_command ppa_cmd;
	struct request_queue *q = exdev->bns->queue;

	memset(&ppa_cmd, 0x00, sizeof(ppa_cmd));

	// set ppalist
	if (nr_ppas == 1) {
		ppa_cmd.ppalist = cpu_to_le64(ppa[0].ppa); 
	} else {
		ppalist = nvm_exdev_dma_pool_alloc(exdev, &dma_ppalist);
		if (!ppalist) {
			pr_err("nvm: failed to allocate dma memory\n");
			return -ENOMEM;
		}

		for (i = 0; i < nr_ppas; i++)
			ppalist[i] = (u64)ppa[i].ppa;

		ppa_cmd.ppalist = cpu_to_le64(dma_ppalist); 
	}

	// set metadata
	dma_meta = dma_map_single(&exdev->pdev->dev, metabuf, 
                                  nr_ppas * NAND_RAW_SIZE, DMA_FROM_DEVICE);
	
	ppa_cmd.opcode = NVM_OP_RDRAW;
	ppa_cmd.nsid = exdev->bns->ns_id;
	ppa_cmd.metadata = cpu_to_le64(dma_meta);
	ppa_cmd.nlb = cpu_to_le16(nr_ppas - 1);
	ppa_cmd.control = cpu_to_le16(ctrl);

	nvme_submit_sync_cmd(q, (struct nvme_command *)&ppa_cmd,
                             databuf, nr_ppas * CFG_NAND_EP_SIZE);

	dma_unmap_single(&exdev->pdev->dev, dma_meta, 
					nr_ppas * NAND_RAW_SIZE, DMA_FROM_DEVICE);

	if (nr_ppas > 1)
		nvm_exdev_dma_pool_free(exdev, ppalist, dma_ppalist);

	return 0;
}

/////////////////////////////////////////////////////////////////
// async Mode use dma_coherent
int nvm_rdpparaw(struct nvm_exdev *exdev, dmappa ppa, int nr_ppas, 
				u16 ctrl, void *databuf, dma_addr_t meta_dma)
{	
	struct nvme_ppa_command *ppa_cmd;
	
	ppa_cmd = kzalloc(sizeof(struct nvme_ppa_command), GFP_KERNEL);
	if (!ppa_cmd)
		return -ENOMEM;

	ppa_cmd->opcode = NVM_OP_RDRAW;
	ppa_cmd->nsid = exdev->bns->ns_id;
	ppa_cmd->metadata = cpu_to_le64(meta_dma);
	ppa_cmd->ppalist = ppa.dma_ppa_list;
	ppa_cmd->nlb = cpu_to_le16(nr_ppas - 1);
	ppa_cmd->control = cpu_to_le16(ctrl);

	nvme_submit_ppa_cmd(exdev, ppa_cmd, databuf, 
                   nr_ppas * CFG_NAND_EP_SIZE, nvme_ppa_completion, NULL);

	// in IRQ callkack completion to release

	return 0;
}

void erspps_smoke_test(struct nvm_exdev *dev)
{
	struct ppa_addr nandppa[2];

	nandppa[0].ppa = 0x3;
	nandppa[0].ppa = 0x4;

	printk("%s\n", __FUNCTION__);
	
	nvm_ersppa_sync(dev, &nandppa[0], 1);
	nvm_ersppa_sync(dev, &nandppa[1], 1);
}

void rdpparawsync_smoke_test(struct nvm_exdev * exdev)
{
	struct physical_address ppa[2];
	void *databuf, *metabuf;
	int *tmp;

	ppa[0].ppa = 0x100;	
	ppa[1].ppa = 0x103;

	printk("%s\n", __FUNCTION__);

	databuf = kzalloc((CFG_NAND_EP_SIZE + NAND_RAW_SIZE) * 2, GFP_KERNEL);
	metabuf = databuf + CFG_NAND_EP_SIZE * 2;

	nvm_rdpparaw_sync(exdev, ppa, 2, NVM_IO_SNGL_ACCESS, databuf, metabuf);

	tmp = (int *)databuf;
	printk("0x%x 0x%x 0x%x 0x%x\n", tmp[0], tmp[1], tmp[2], tmp[3]);

	kfree(databuf);
}

void rdpparraw_smoke_test(struct nvm_exdev * exdev)
{
	//g_ppalist_buf[0] = 0x13;
	int nr_ppas=1;
	dma_addr_t dma_meta;
	dmappa ppa;
	void *databuf = wcb_entity_base_data(0);
	ppa.nandppa = 0x12;

	dma_meta = wcb_entity_base_metadma(0);

	nvm_rdpparaw(exdev, ppa, nr_ppas, 0, databuf, dma_meta);
}

void run_testcase(struct nvm_exdev *exdev)
{
	rdpparraw_smoke_test(exdev);

	//erspps_smoke_test(exdev);
	//rdpparawsync_smoke_test(exdev);

	return;
}

int erase_rblk_wait(struct nvm_exdev *exdev, u16 blk)
{
        int ret = 0;
        u16 nlb = 0, ch, lun;
        u16 plmode;
        u16 nr_ppas = CFG_NAND_CHANNEL_NUM*CFG_NAND_LUN_NUM;
        dma_addr_t dma_ppalist;
        u64 *ppalist;
        geo_ppa ppa;
	struct nvme_ppa_command *ppa_cmd;
	
	ppa_cmd = kzalloc(sizeof(struct nvme_ppa_command), GFP_KERNEL);
	if (!ppa_cmd)
		return -ENOMEM;

        if (CFG_NAND_PLANE_NUM == 4) {
                plmode = NVM_IO_QUAD_ACCESS;
        } else if (CFG_NAND_PLANE_NUM == 2) {
                plmode = NVM_IO_DUAL_ACCESS;
        }

        ppalist = nvm_exdev_dma_pool_alloc(exdev, &dma_ppalist);
        if (!ppalist) {
                pr_err("nvm: failed to allocate dma memory\n");
                ret = -ENOMEM;
                goto free_cmd;
        }

        for (lun = 0; lun < CFG_NAND_LUN_NUM; lun++) {
                for (ch = 0; ch < CFG_NAND_CHANNEL_NUM; ch++) {
                        set_ppa_nand_addr(&ppa, ch, 0, 0, lun, 0, blk);
                        ppalist[nlb++] = ppa.ppa;
                }
        }

	ppa_cmd->opcode = NVM_OP_ERASE;
	ppa_cmd->nsid = exdev->bns->ns_id;
	ppa_cmd->nlb = cpu_to_le16(nr_ppas - 1);
        ppa_cmd->ppalist = cpu_to_le64(dma_ppalist);
	ppa_cmd->control = cpu_to_le16(plmode);

        nvme_submit_ppa_cmd_sync(exdev, ppa_cmd, NULL, 0);

        nvm_exdev_dma_pool_free(exdev, ppalist, dma_ppalist);
free_cmd:
        kfree(ppa_cmd);
	return ret;
}

#if 0
///////////////////BackEnd Ioctl sync//////////////////
// nvme-cli extend
// nvme-cli fscftl passthru
static void user_passthru_cmd_completion(struct request *rq, int error)
{
	struct completion *waiting = rq->end_io_data;

	complete(waiting);
}

static int submit_user_passthru_cmd(struct nvm_exdev *dev,
				struct nvme_ppa_command *cmd,
				void __user *ubuf, unsigned int bufflen,
				void __user *meta_buf, unsigned int meta_len,
				void __user *ppa_buf, unsigned int ppa_len,
				u32 *result, u64 *status, unsigned int timeout)
{
	bool write = nvme_is_write((struct nvme_command *)cmd);
        struct request_queue *q = dev->bns->queue;
	struct gendisk *disk = dev->bns->disk;
	struct request *rq;
	struct bio *bio = NULL;
	__le64 *ppa_list = NULL;
	dma_addr_t ppa_dma;
	__le64 *metadata = NULL;
	dma_addr_t metadata_dma;
	DECLARE_COMPLETION_ONSTACK(wait);
	int ret;

	rq = nvme_alloc_request(q, (struct nvme_command *)cmd, 0, NVME_QID_ANY);
	if (IS_ERR(rq)) {
		ret = -ENOMEM;
		goto err_cmd;
	}

	rq->timeout = timeout ? timeout : ADMIN_TIMEOUT;

	rq->cmd_flags &= ~REQ_FAILFAST_DRIVER;
	rq->end_io_data = &wait;

	if (ppa_buf && ppa_len) {
                ppa_list = nvm_exdev_dma_pool_alloc(dev, &ppa_dma);
		if (!ppa_list) {
			ret = -ENOMEM;
			goto err_rq;
		}
		if (copy_from_user(ppa_list, (void __user *)ppa_buf,
						sizeof(u64) * (ppa_len + 1))) {
			ret = -EFAULT;
			goto err_ppa;
		}
		cmd->ppalist = cpu_to_le64(ppa_dma);
	} else {
		cmd->ppalist = cpu_to_le64((uintptr_t)ppa_buf);
	}

	if (ubuf && bufflen) {
		ret = blk_rq_map_user(q, rq, NULL, ubuf, bufflen, GFP_KERNEL);
		if (ret)
			goto err_ppa;
		bio = rq->bio;

		if (meta_buf && meta_len) {
			metadata = nvm_exdev_dma_pool_alloc(dev, &metadata_dma);
			if (!metadata) {
				ret = -ENOMEM;
				goto err_map;
			}

			if (write) {
				if (copy_from_user(metadata,
						(void __user *)meta_buf,
						meta_len)) {
					ret = -EFAULT;
					goto err_meta;
				}
			}
			cmd->metadata = cpu_to_le64(metadata_dma);
		}

		if (!disk)
			goto submit;

		bio->bi_bdev = bdget_disk(disk, 0);
		if (!bio->bi_bdev) {
			ret = -ENODEV;
			goto err_meta;
		}
	}

submit:
	blk_execute_rq_nowait(q, NULL, rq, 0, user_passthru_cmd_completion);

	wait_for_completion_io(&wait);

	ret = nvme_error_status(rq->errors);
	if (result)
		*result = rq->errors & 0x7ff;
	if (status)
		*status = le64_to_cpu(nvme_req(rq)->result.u64);

	if (metadata && !ret && !write) {
		if (copy_to_user(meta_buf, (void *)metadata, meta_len))
			ret = -EFAULT;
	}
err_meta:
	if (meta_buf && meta_len)
		nvm_exdev_dma_pool_free(dev, metadata, metadata_dma);
err_map:
	if (bio) {
		if (disk && bio->bi_bdev)
			bdput(bio->bi_bdev);
		blk_rq_unmap_user(bio);
	}
err_ppa:
	if (ppa_buf && ppa_len)
		nvm_exdev_dma_pool_free(dev, ppa_list, ppa_dma);
err_rq:
	blk_mq_free_request(rq);
err_cmd:
	return ret;
}

static int nvme_issue_user_cmd(struct nvm_exdev *dev,
				      struct nvm_user_vio __user *uvio)
{
	int ret;
	struct nvm_user_vio vio;
        struct nvme_ppa_command c;

	if (copy_from_user(&vio, uvio, sizeof(vio)))
		return -EFAULT;
	if (vio.flags)
		return -EINVAL;

	memset(&c, 0, sizeof(c));
	c.opcode = vio.opcode;
	c.nsid = cpu_to_le32(dev->bns->ns_id);
	c.control = cpu_to_le16(vio.control);
	c.nlb = cpu_to_le16(vio.nppas);
        c.dsmgmt = cpu_to_le32(vio.rsvd3[0]);

	ret = submit_user_passthru_cmd(dev, &c,
                     (void __user *)(uintptr_t)vio.addr, (vio.nppas + 1) * EXP_PPA_SIZE,
                     (void __user *)(uintptr_t)vio.metadata, vio.metadata_len,
		     (void __user *)(uintptr_t)vio.ppa_list, vio.nppas,
		     &vio.result, &vio.status, 0);

	if (ret && copy_to_user(uvio, &vio, sizeof(vio)))
		return -EFAULT;

	return ret;
}
#endif
