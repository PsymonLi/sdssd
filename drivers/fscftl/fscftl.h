#ifndef FSCFTL_H_
#define FSCFTL_H_

#include "../../include/linux/lightnvm.h"
#include "../nvme/host/nvme.h"
#include "hwcfg/cfg/flash_cfg.h"
#include "bootblk/bootblk_mngr.h"

//don't define fix value, should read from HW register
#define MAX_PPA_PER_CMD		64   // due to cqe 64 bit

/* Bootblk+SYSTBL+BB+GC+SpecialPPA(XOR FistPage Ftllog) */
//#define OP_CAPACITY		(CAPACITY - USER_CAPACITY)
/* User data Byte */
#define USER_CAPACITY   ((CAPACITY*8)/11)

#define MAX_USER_LBA	(USER_CAPACITY/CFG_NAND_EP_SIZE)

#define NAND_RAW_SIZE 		304
#define NAND_META_SIZE 		16

// io opcode
enum {
	NVM_OP_WRPPA		= 0x91,
	NVM_OP_RDPPA		= 0x92,
	NVM_OP_ESPPA		= 0x90,
	NVM_OP_WRRAW		= 0x95,
	NVM_OP_RDRAW		= 0x96,
};

// ppa sqe
struct nvme_ppa_command {
	__u8			opcode;
	__u8			flags;
	__u16			command_id;
	__le32			nsid;
	__u64			rsvd2;
	__le64			metadata;
	__le64			prp1;
	__le64			prp2;
	__le64			ppalist;
	__le16			nlb;
	__le16			control;
	__le32			dsmgmt;
	__le64			resv;
};

/* extern fn */
int nvm_create_exns(struct nvm_exdev *exdev);
void nvm_delete_exns(struct nvm_exdev *exdev);

void free_rqd_nand_ppalist(struct nvm_exdev * dev, struct nvm_rq *rqd);
int set_rqd_nand_ppalist(struct nvm_exdev *dev, struct nvm_rq *rqd, 
						 struct ppa_addr *ppas, int nr_ppas);

int nvm_exdev_setup_pool(struct nvm_exdev *dev, char *name);
void nvm_exdev_release_pool(struct nvm_exdev *dev);
void *nvm_exdev_dma_pool_alloc(struct nvm_exdev *dev, dma_addr_t *dma_handle);
void nvm_exdev_dma_pool_free(struct nvm_exdev *dev, void *vaddr, 
							 dma_addr_t dma_handle);

int nvme_submit_ppa_cmd(struct nvm_exdev *dev, struct nvme_ppa_command *cmd,
						void *buffer, unsigned bufflen, 
						rq_end_io_fn *done, void *ctx);

#endif
