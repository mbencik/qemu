/*
 * QEMU remote port memory master.
 *
 * Copyright (c) 2014 Xilinx Inc
 * Written by Edgar E. Iglesias <edgar.iglesias@xilinx.com>
 *
 * This code is licensed under the GNU GPL.
 */

#include "qemu/osdep.h"
#include "sysemu/sysemu.h"
#include "qemu/log.h"
#include "qapi/qmp/qerror.h"
#include "qapi/error.h"
#include "hw/sysbus.h"

#include "hw/remote-port-proto.h"
#include "hw/remote-port-device.h"

#include "hw/fdt_generic_util.h"

#ifndef REMOTE_PORT_ERR_DEBUG
#define REMOTE_PORT_DEBUG_LEVEL 0
#else
#define REMOTE_PORT_DEBUG_LEVEL 1
#endif

#define DB_PRINT_L(level, ...) do { \
    if (REMOTE_PORT_DEBUG_LEVEL > level) { \
        fprintf(stderr,  ": %s: ", __func__); \
        fprintf(stderr, ## __VA_ARGS__); \
    } \
} while (0);

#define TYPE_REMOTE_PORT_MEMORY_MASTER "remote-port-memory-master"
#define REMOTE_PORT_MEMORY_MASTER(obj) \
        OBJECT_CHECK(RemotePortMemoryMaster, (obj), \
                     TYPE_REMOTE_PORT_MEMORY_MASTER)

#define REMOTE_PORT_MEMORY_MASTER_PARENT_CLASS \
    object_class_get_parent( \
            object_class_by_name(TYPE_REMOTE_PORT_MEMORY_MASTER))

typedef struct RemotePortMemoryMaster RemotePortMemoryMaster;

typedef struct RemotePortMap {
    RemotePortMemoryMaster *parent;
    MemoryRegion iomem;
    uint64_t offset;
} RemotePortMap;

struct RemotePortMemoryMaster {
    /* private */
    SysBusDevice parent;

    RemotePortMap *mmaps;

    /* public */
    uint32_t rp_dev;
    bool relative;
    struct RemotePort *rp;
    struct rp_peer_state *peer;
};

static uint64_t rp_io_read(void *opaque, hwaddr addr, unsigned size,
                           MemTxAttrs attr)
{
    RemotePortMap *map = opaque;
    RemotePortMemoryMaster *s = map->parent;
    struct rp_pkt_busaccess_ext_base pkt;
    struct rp_encode_busaccess_in in = {0};
    uint64_t value = 0;
    int64_t rclk;
    uint8_t *data;
    int len;
    int i;
    RemotePortDynPkt rsp;

    DB_PRINT_L(1, "\n");
    addr += s->relative ? 0 : map->offset;

    in.cmd = RP_CMD_read;
    in.id = rp_new_id(s->rp);
    in.dev = s->rp_dev;
    in.clk = rp_normalized_vmclk(s->rp);
    in.master_id = attr.master_id;
    in.addr = addr;
    in.attr |= attr.secure ? RP_BUS_ATTR_SECURE : 0;
    in.size = size;
    in.stream_width = size;
    len = rp_encode_busaccess(s->peer, &pkt, &in);

    rp_rsp_mutex_lock(s->rp);
    rp_write(s->rp, (void *) &pkt, len);

    rsp = rp_wait_resp(s->rp);
    /* We dont support out of order answers yet.  */
    assert(rsp.pkt->hdr.id == be32_to_cpu(pkt.hdr.id));

    data = rp_busaccess_rx_dataptr(s->peer, &rsp.pkt->busaccess_ext_base);
    for (i = 0; i < size; i++) {
        value |= data[i] << (i *8);
    }
    rclk = rsp.pkt->busaccess_ext_base.timestamp;
    rp_dpkt_invalidate(&rsp);
    rp_rsp_mutex_unlock(s->rp);
    rp_sync_vmclock(s->rp, in.clk, rclk);

    /* Reads are sync-points, roll the sync timer.  */
    rp_restart_sync_timer(s->rp);
    rp_leave_iothread(s->rp);
    DB_PRINT_L(0, "addr: %" HWADDR_PRIx " data: %" PRIx64 "\n", addr, value);
    return value;
}

static void rp_io_write(void *opaque, hwaddr addr, uint64_t value,
                        unsigned size, MemTxAttrs attr)
{
    RemotePortMap *map = opaque;
    RemotePortMemoryMaster *s = map->parent;
    int64_t rclk;
    RemotePortDynPkt rsp;

    struct  {
        struct rp_pkt_busaccess_ext_base pkt;
        uint8_t reserved[8];
    } pay;
    uint8_t *data = rp_busaccess_tx_dataptr(s->peer, &pay.pkt);
    struct rp_encode_busaccess_in in = {0};
    int i;
    int len;

    DB_PRINT_L(0, "addr: %" HWADDR_PRIx " data: %" PRIx64 "\n", addr, value);

    for (i = 0; i < 8; i++) {
        data[i] = value >> (i * 8);
    }

    assert(size <= 8);
    addr += s->relative ? 0 : map->offset;

    in.cmd = RP_CMD_write;
    in.id = rp_new_id(s->rp);
    in.dev = s->rp_dev;
    in.clk = rp_normalized_vmclk(s->rp);
    in.master_id = attr.master_id;
    in.addr = addr;
    in.attr |= attr.secure ? RP_BUS_ATTR_SECURE : 0;
    in.size = size;
    in.stream_width = size;
    len = rp_encode_busaccess(s->peer, &pay.pkt, &in);

    rp_rsp_mutex_lock(s->rp);

    rp_write(s->rp, (void *) &pay, len + size);

    rsp = rp_wait_resp(s->rp);

    /* We dont support out of order answers yet.  */
    assert(rsp.pkt->hdr.id == be32_to_cpu(pay.pkt.hdr.id));
    rclk = rsp.pkt->busaccess.timestamp;
    rp_dpkt_invalidate(&rsp);
    rp_rsp_mutex_unlock(s->rp);
    rp_sync_vmclock(s->rp, in.clk, rclk);
    /* Reads are sync-points, roll the sync timer.  */
    rp_restart_sync_timer(s->rp);
    rp_leave_iothread(s->rp);
    DB_PRINT_L(1, "\n");
}

static void rp_io_access(MemoryTransaction *tr)
{
    MemTxAttrs attr = tr->attr;
    void *opaque = tr->opaque;
    hwaddr addr = tr->addr;
    unsigned size = tr->size;
    uint64_t value = tr->data.u64;;
    bool is_write = tr->rw;

    if (is_write) {
        rp_io_write(opaque, addr, value, size, attr);
    } else {
        tr->data.u64 = rp_io_read(opaque, addr, size, attr);
    }
}

static const MemoryRegionOps rp_ops = {
    .access = rp_io_access,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rp_memory_master_realize(DeviceState *dev, Error **errp)
{
    RemotePortMemoryMaster *s = REMOTE_PORT_MEMORY_MASTER(dev);

    assert(s->rp);
    s->peer = rp_get_peer(s->rp);
}

static void rp_memory_master_init(Object *obj)
{
    RemotePortMemoryMaster *rpms = REMOTE_PORT_MEMORY_MASTER(obj);
    object_property_add_link(obj, "rp-adaptor0", "remote-port",
                             (Object **)&rpms->rp,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_UNREF_ON_RELEASE,
                             &error_abort);
}

static bool rp_parse_reg(FDTGenericMMap *obj, FDTGenericRegPropInfo reg,
                         Error **errp)
{
    RemotePortMemoryMaster *s = REMOTE_PORT_MEMORY_MASTER(obj);
    FDTGenericMMapClass *parent_fmc = 
        FDT_GENERIC_MMAP_CLASS(REMOTE_PORT_MEMORY_MASTER_PARENT_CLASS);
    int i;

    s->mmaps = g_new0(typeof(*s->mmaps), reg.n);
    for (i = 0; i < reg.n; ++i) {
        char *name = g_strdup_printf("rp-%d", i);

        s->mmaps[i].offset = reg.a[i];
        memory_region_init_io(&s->mmaps[i].iomem, OBJECT(obj), &rp_ops,
                              &s->mmaps[i], name, reg.s[i]);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmaps[i].iomem);
        s->mmaps[i].parent = s;
        g_free(name);
    }

    return parent_fmc ? parent_fmc->parse_reg(obj, reg, errp) : false;
}

static Property rp_properties[] = {
    DEFINE_PROP_UINT32("rp-chan0", RemotePortMemoryMaster, rp_dev, 0),
    DEFINE_PROP_BOOL("relative", RemotePortMemoryMaster, relative, false),
    DEFINE_PROP_END_OF_LIST()
};

static void rp_memory_master_class_init(ObjectClass *oc, void *data)
{
    FDTGenericMMapClass *fmc = FDT_GENERIC_MMAP_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->props = rp_properties;
    dc->realize = rp_memory_master_realize;
    fmc->parse_reg = rp_parse_reg;
}

static const TypeInfo rp_info = {
    .name          = TYPE_REMOTE_PORT_MEMORY_MASTER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RemotePortMemoryMaster),
    .instance_init = rp_memory_master_init,
    .class_init    = rp_memory_master_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { TYPE_FDT_GENERIC_MMAP },
        { TYPE_REMOTE_PORT_DEVICE },
        { },
    },
};

static void rp_register_types(void)
{
    type_register_static(&rp_info);
}

type_init(rp_register_types)
