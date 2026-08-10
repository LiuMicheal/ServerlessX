#include "./kernel_helper.h"

#include <linux/kallsyms.h>
#include <linux/completion.h>
#include <net/net_namespace.h>

ib_sa_comp_mask
path_rec_service_id(void)
{
  return IB_SA_PATH_REC_SERVICE_ID;
}

ib_sa_comp_mask
path_rec_dgid(void)
{
  return IB_SA_PATH_REC_DGID;
}

ib_sa_comp_mask
path_rec_sgid(void)
{
  return IB_SA_PATH_REC_SGID;
}

ib_sa_comp_mask
path_rec_numb_path(void)
{
  return IB_SA_PATH_REC_NUMB_PATH;
}

#if defined(BASE_INBOX_RDMA_5_14)
struct bd_resolve_context {
  struct completion done;
  int status;
};

static void
bd_resolve_complete(int status, struct sockaddr *src_addr,
                    struct rdma_dev_addr *addr, void *context)
{
  struct bd_resolve_context *ctx = context;

  ctx->status = status;
  complete(&ctx->done);
}

const struct ib_gid_attr *
bd_rdma_get_gid_attr(struct ib_device *device, unsigned int port_num,
                     int gid_index)
{
  const struct ib_gid_attr *attr;

  attr = rdma_get_gid_attr(device, port_num, gid_index);
  return IS_ERR(attr) ? NULL : attr;
}

void
bd_rdma_put_gid_attr(const struct ib_gid_attr *attr)
{
  if (attr)
    rdma_put_gid_attr(attr);
}

int
bd_resolve_roce_path(struct ib_device *device, unsigned int port_num,
                     int gid_index, const union ib_gid *dgid,
                     __be64 service_id, struct sa_path_rec *path)
{
  const struct ib_gid_attr *attr;
  struct rdma_dev_addr dev_addr = {};
  struct bd_resolve_context ctx;
  struct ib_port_attr port_attr;
  union {
    struct sockaddr addr;
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
  } src_addr, dst_addr;
  u16 pkey;
  int ret;

  attr = rdma_get_gid_attr(device, port_num, gid_index);
  if (IS_ERR(attr))
    return PTR_ERR(attr);
  if (attr->gid_type != IB_GID_TYPE_ROCE_UDP_ENCAP) {
    ret = -EAFNOSUPPORT;
    goto out_put_attr;
  }

  rdma_gid2ip(&src_addr.addr, &attr->gid);
  rdma_gid2ip(&dst_addr.addr, dgid);
  if (src_addr.addr.sa_family != dst_addr.addr.sa_family) {
    ret = -EINVAL;
    goto out_put_attr;
  }

  dev_addr.net = &init_net;
  dev_addr.sgid_attr = attr;
  init_completion(&ctx.done);
  ret = rdma_resolve_ip(&src_addr.addr, &dst_addr.addr, &dev_addr, 1000,
                        bd_resolve_complete, true, &ctx);
  if (ret)
    goto out_put_attr;
  wait_for_completion(&ctx.done);
  ret = ctx.status;
  if (ret)
    goto out_put_attr;

  ret = ib_query_port(device, port_num, &port_attr);
  if (ret)
    goto out_put_attr;
  ret = ib_get_cached_pkey(device, port_num, 0, &pkey);
  if (ret)
    goto out_put_attr;

  memset(path, 0, sizeof(*path));
  path->dgid = *dgid;
  path->sgid = attr->gid;
  path->service_id = service_id;
  path->hop_limit = dev_addr.hoplimit;
  path->reversible = 1;
  path->numb_path = 1;
  path->pkey = cpu_to_be16(pkey);
  path->mtu_selector = IB_SA_EQ;
  path->mtu = port_attr.active_mtu;
  path->rec_type = SA_PATH_REC_TYPE_ROCE_V2;
  path->roce.route_resolved = true;
  sa_path_set_dmac(path, dev_addr.dst_dev_addr);
  ret = 0;

out_put_attr:
  rdma_put_gid_attr(attr);
  return ret;
}
#endif

#if defined(BASE_MLNX_OFED_LINUX_5_4_1_0_3_0) ||                               \
  defined(BASE_MLNX_OFED_LINUX_4_9_3_1_5_0) || defined(BASE_INBOX_RDMA_5_14)
int
bd_ib_post_send(struct ib_qp* qp,
                struct ib_send_wr* send_wr,
                struct ib_send_wr** bad_send_wr)
{
  return ib_post_send(qp, (const struct ib_send_wr *)send_wr, (const struct ib_send_wr **)bad_send_wr);
}

#else 
int
bd_ib_post_send(struct ib_qp* qp,
                struct ib_send_wr* send_wr,
                struct ib_send_wr** bad_send_wr)
{
  return ib_post_send(qp, send_wr, bad_send_wr);
}
#endif

#if defined(BASE_MLNX_OFED_LINUX_5_4_1_0_3_0) ||                              \
  defined(BASE_MLNX_OFED_LINUX_4_9_3_1_5_0) || defined(BASE_INBOX_RDMA_5_14)
int
bd_ib_post_recv(struct ib_qp* qp,
                struct ib_recv_wr* wr,
                struct ib_recv_wr** bad_send_wr)
{
  return ib_post_recv(qp, wr, (const struct ib_recv_wr **) bad_send_wr);
}

int
bd_ib_post_srq_recv(struct ib_srq* qp,
                struct ib_recv_wr* wr,
                struct ib_recv_wr** bad_send_wr)
{
  return ib_post_srq_recv(qp, wr, (const struct ib_recv_wr **) bad_send_wr);
}

#else
int
bd_ib_post_recv(struct ib_qp* qp,
                struct ib_recv_wr* wr,
                struct ib_recv_wr** bad_send_wr)
{
  return ib_post_recv(qp, wr, bad_send_wr);
}

int
bd_ib_post_srq_recv(struct ib_srq* qp,
                struct ib_recv_wr* wr,
                struct ib_recv_wr** bad_send_wr)
{
    return ib_post_srq_recv(qp, wr, bad_send_wr);
}
#endif

int
bd_ib_poll_cq(struct ib_cq* cq, int num_entries, struct ib_wc* wc)
{
  return ib_poll_cq(cq, num_entries, wc);
}

void
bd_rdma_ah_set_dlid(struct rdma_ah_attr* attr, unsigned int dlid)
{
  rdma_ah_set_dlid(attr, dlid);
}

void
bd_set_recv_wr_id(struct ib_recv_wr* wr, unsigned long long wr_id)
{
  wr->wr_id = wr_id;
}

int
bd_get_recv_wr_id(struct ib_recv_wr* wr)
{
  return wr->wr_id;
}

unsigned long long
bd_get_wc_wr_id(struct ib_wc* wc)
{
  return wc->wr_id;
}

int
gfp_highuser(void)
{
  return GFP_HIGHUSER;
}

unsigned int
bd_page_size(void)
{
  return PAGE_SIZE;
}

unsigned int
dma_from_device(void)
{
  return DMA_FROM_DEVICE;
}

int
bd_ib_dma_map_sg(struct ib_device* dev,
                 struct scatterlist* sg,
                 int nents,
                 enum dma_data_direction direction)
{
  return ib_dma_map_sg(dev, sg, nents, direction);
}

void
bd_sg_set_page(struct scatterlist* sg,
               struct page* page,
               unsigned int len,
               unsigned int offset)
{
  return sg_set_page(sg, page, len, offset);
}

struct page*
bd_alloc_pages(int mask, int page_order)
{
  return alloc_pages(mask, page_order);
}
void
bd_get_page(struct page* page)
{
    get_page(page);
}

void
bd_free_pages(struct page* page, unsigned int order)
{
  __free_pages(page, order);
}

void*
bd_page_address(const struct page* page)
{
  return page_address(page);
}

struct page*
bd_virt_to_page(void* kaddr)
{
  return virt_to_page(kaddr);
}

long
ptr_is_err(const void* ptr)
{
  return IS_ERR(ptr);
}

#if defined(BASE_MLNX_OFED_LINUX_5_4_1_0_3_0) ||                               \
  defined(BASE_MLNX_OFED_LINUX_4_9_3_1_5_0) || defined(BASE_INBOX_RDMA_5_14)
struct ib_cq*
bd_ib_create_cq(struct ib_device* device,
                ib_comp_handler comp_handler,
                void (*event_handler)(struct ib_event*, void*),
                void* cq_context,
                const struct ib_cq_init_attr* cq_attr)
{
  return __ib_create_cq(device, comp_handler, event_handler, cq_context, cq_attr, "rust-kernel-rdma-base");
}
#endif
