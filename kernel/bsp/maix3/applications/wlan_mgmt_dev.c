#include <rthw.h>
#include <rtthread.h>

#include "rtdef.h"
#include <lwp.h>
#include <lwp_user_mm.h>

#include <netdev_ipaddr.h>
#include <netdev.h>

#include <wlan_mgnt.h>

#define DBG_TAG "WlanMgmtDev"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

// basic
#define IOCTRL_WM_GET_AUTORECONNECT 0x00
#define IOCTRL_WM_SET_AUTORECONNECT 0x01
#define IOCTRL_WM_GET_POWERSAVE 0x02
#define IOCTRL_WM_SET_POWERSAVE 0x03
#define IOCTRL_WM_REGISET_EVT_HDL 0x04
#define IOCTRL_WM_UNREGISET_EVT_HDL 0x05

// sta
#define IOCTRL_WM_STA_CONNECT 0x10
#define IOCTRL_WM_STA_DISCONNECT 0x11
#define IOCTRL_WM_STA_IS_CONNECTED 0x12
#define IOCTRL_WM_STA_GET_MAC 0x13
#define IOCTRL_WM_STA_SET_MAC 0x14
#define IOCTRL_WM_STA_GET_INFO 0x15
#define IOCTRL_WM_STA_GET_RSSI 0x16
#define IOCTRL_WM_STA_SCAN 0x17

// ap
#define IOCTRL_WM_AP_START  0x20
#define IOCTRL_WM_AP_STOP  0x21
#define IOCTRL_WM_AP_IS_ACTIVE  0x22
#define IOCTRL_WM_AP_GET_STA_INFO  0x23
#define IOCTRL_WM_AP_DEAUTH_STA  0x24

// network util
#define IOCTRL_NET_IFCONFIG 0x100

struct rt_wlan_mgmt_device
{
    struct rt_device device;
    struct rt_mutex lock;
};

struct rt_wlan_mgmt_device_cmd_handle
{
    int cmd;
    rt_err_t (*func)(struct rt_wlan_mgmt_device *mgmt_dev, void *args);
};

/*****************************************************************************/
static struct rt_wlan_mgmt_device wlan_mgmt_device;

struct rt_wlan_connect_config {
    int use_info;
    union {
        rt_wlan_ssid_t ssid;
        struct rt_wlan_info info;
    };
    rt_wlan_key_t key;
};

static rt_err_t _wlan_mgmt_dev_cmd_sta_connect(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    rt_err_t err = RT_EOK;
    struct rt_wlan_connect_config config;

    lwp_get_from_user(&config, args, sizeof(struct rt_wlan_connect_config));

    if(config.use_info) {
        err = rt_wlan_connect_adv(&config.info, config.key.val);
    } else {
        err = rt_wlan_connect(config.ssid.val, config.key.val);
    }

    return err;
}

static rt_err_t _wlan_mgmt_dev_cmd_sta_disconnect(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    return rt_wlan_disconnect();
}

static rt_err_t _wlan_mgmt_dev_cmd_sta_isconnected(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    int status = (int)rt_wlan_is_connected();

    lwp_put_to_user(args, &status, sizeof(int));

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_dev_cmd_sta_get_rssi(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    int rssi = rt_wlan_get_rssi();

    lwp_put_to_user(args, &rssi, sizeof(int));

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_dev_cmd_sta_scan(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    struct rt_wlan_scan_result *result = (struct rt_wlan_scan_result *)args;
    struct rt_wlan_scan_result *scan_result = RT_NULL;

    int32_t result_num;
    struct rt_wlan_info info;

    lwp_get_from_user(&result_num, &result->num, sizeof(int32_t));
    lwp_get_from_user(&info, &result->info[0], sizeof(struct rt_wlan_info));

    /* clean scan result */
    rt_wlan_scan_result_clean();
    /* scan ap info */
    scan_result = rt_wlan_scan_with_info(&info);

    if(scan_result) {
        if(scan_result->num > result_num) {
            LOG_W("Scan result count(%d) bigger than user requset(%d)\n", scan_result->num, result_num);
            scan_result->num = result_num;
        }

        lwp_put_to_user(&result->num, &scan_result->num, sizeof(int32_t));

        for(int i = 0; i< scan_result->num; i++) {
            lwp_put_to_user(&result->info[i], &scan_result->info[i], sizeof(struct rt_wlan_info));
        }
        rt_wlan_scan_result_clean();

        return RT_EOK;
    }

    LOG_E("Can't scan ap\n");

    return -1;
}

static rt_err_t _wlan_mgmt_dev_cmd_ap_start(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    rt_err_t err = RT_EOK;
    struct rt_wlan_connect_config config;

    lwp_get_from_user(&config, args, sizeof(struct rt_wlan_connect_config));

    if(config.use_info) {
        err = rt_wlan_start_ap_adv(&config.info, config.key.val);
    } else {
        err = rt_wlan_start_ap(config.ssid.val, config.key.val);
    }

    return err;
}

static rt_err_t _wlan_mgmt_dev_cmd_ap_stop(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    return rt_wlan_ap_stop();
}

static rt_err_t _wlan_mgmt_dev_cmd_ap_isactive(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    int active = (int)rt_wlan_ap_is_active();
    lwp_put_to_user(args, &active, sizeof(int));

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_dev_cmd_ap_deauth_sta(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    uint8_t mac[6];

    lwp_get_from_user(mac, args, sizeof(mac));

    return rt_wlan_ap_deauth_sta(mac);
}

static rt_err_t _wlan_mgmt_dev_cmd_net_ifconfig(struct rt_wlan_mgmt_device *mgmt_dev, void *args)
{
    struct ifconfig {
        uint16_t net_if;            /* 0: sta, 1: ap, 2:... */
        uint16_t set;               /* 0:get, 1:set */
        ip_addr_t ip;               /* IP address */
        ip_addr_t gw;               /* gateway */
        ip_addr_t netmask;          /* subnet mask */
        ip_addr_t dns;              /* DNS server */
    };
    struct ifconfig ifconfig;
    struct ifconfig *out = (struct ifconfig *)args;

    struct rt_wlan_device *wlan_dev = NULL;
    struct netdev *netdev = NULL;

    lwp_get_from_user(&ifconfig, args, sizeof(struct ifconfig));

    if(0x00 == ifconfig.net_if) { /* wlan station */
        wlan_dev = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
        if(NULL == wlan_dev) {
            LOG_E("Can't find netif %s\n", RT_WLAN_DEVICE_STA_NAME);
            return -RT_ERROR;
        }
        netdev = wlan_dev->netdev;
    } else if(0x01 == ifconfig.net_if) { /* wlan ap */
        wlan_dev = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_AP_NAME);
        if(NULL == wlan_dev) {
            LOG_E("Can't find netif %s\n", RT_WLAN_DEVICE_STA_NAME);
            return -RT_ERROR;
        }
        netdev = wlan_dev->netdev;
    } else {
        LOG_E("Unsupport netif %d\n", ifconfig.net_if);
        return -RT_ERROR;
    }

    if(NULL == netdev) {
        LOG_E("Can't find netif %d\n", ifconfig.net_if);
        return -RT_ERROR;
    }

    if(0x00 == ifconfig.set) { /* get */
        lwp_put_to_user(&out->ip, &netdev->ip_addr, sizeof(ip_addr_t));
        lwp_put_to_user(&out->gw, &netdev->gw, sizeof(ip_addr_t));
        lwp_put_to_user(&out->netmask, &netdev->netmask, sizeof(ip_addr_t));
        lwp_put_to_user(&out->dns, &netdev->dns_servers[0], sizeof(ip_addr_t));
    } else { /* set */
        if(0x00 != netdev_set_ipaddr(netdev, &ifconfig.ip)) {
            LOG_E("Set itf %s ip failed\n", netdev->name);
            return -RT_ERROR;
        }
        if(0x00 != netdev_set_gw(netdev, &ifconfig.gw)) {
            LOG_E("Set itf %s gw failed\n", netdev->name);
            return -RT_ERROR;
        }
        if(0x00 != netdev_set_netmask(netdev, &ifconfig.netmask)) {
            LOG_E("Set itf %s netmask failed\n", netdev->name);
            return -RT_ERROR;
        }
        if(0x00 != netdev_set_dns_server(netdev, 1, &ifconfig.dns)) {
            LOG_E("Set itf %s dns failed\n", netdev->name);
            return -RT_ERROR;
        }
    }

    return RT_EOK;
}

static struct rt_wlan_mgmt_device_cmd_handle cmd_handles[] = {
    // basic

    // station
    {
        .cmd = IOCTRL_WM_STA_CONNECT,
        .func = _wlan_mgmt_dev_cmd_sta_connect,
    },
    {
        .cmd = IOCTRL_WM_STA_DISCONNECT,
        .func = _wlan_mgmt_dev_cmd_sta_disconnect,
    },
    {
        .cmd = IOCTRL_WM_STA_IS_CONNECTED,
        .func = _wlan_mgmt_dev_cmd_sta_isconnected,
    },
    {
        .cmd = IOCTRL_WM_STA_GET_RSSI,
        .func = _wlan_mgmt_dev_cmd_sta_get_rssi,
    },
    {
        .cmd = IOCTRL_WM_STA_SCAN,
        .func = _wlan_mgmt_dev_cmd_sta_scan,
    },

    // ap
    {
        .cmd = IOCTRL_WM_AP_START,
        .func = _wlan_mgmt_dev_cmd_ap_start,
    },
    {
        .cmd = IOCTRL_WM_AP_STOP,
        .func = _wlan_mgmt_dev_cmd_ap_stop,
    },
    {
        .cmd = IOCTRL_WM_AP_IS_ACTIVE,
        .func = _wlan_mgmt_dev_cmd_ap_isactive,
    },
    {
        .cmd = IOCTRL_WM_AP_DEAUTH_STA,
        .func = _wlan_mgmt_dev_cmd_ap_deauth_sta,
    },

    // network
    {
        .cmd = IOCTRL_NET_IFCONFIG,
        .func = _wlan_mgmt_dev_cmd_net_ifconfig,
    },
};

static rt_err_t _wlan_mgmt_dev_control(rt_device_t dev, int cmd, void *args)
{
    struct rt_wlan_mgmt_device *mgmt_dev = (struct rt_wlan_mgmt_device *)dev;
    const size_t handle_count = sizeof(cmd_handles) / sizeof(cmd_handles[0]);

    for(size_t i = 0; i < handle_count; i++) {
        if(cmd == cmd_handles[i].cmd) {
            return cmd_handles[i].func(mgmt_dev, args);
        }
    }

    LOG_E("Unsupport cmd 0x%x\n", cmd);

    return -RT_ERROR;
}

static rt_err_t  _wlan_mgmt_dev_init(rt_device_t dev)
{
    (void)dev;

    rt_wlan_config_autoreconnect(RT_TRUE);

    return (rt_err_t)rt_wlan_init();
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops wlan_mgmt_dev_ops =
{
    _wlan_mgmt_dev_init,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    _wlan_mgmt_dev_control
};
#endif

static int wlan_mgmt_dev_init(void)
{
    rt_err_t err = RT_EOK;
    static rt_int8_t _init_flag = 0;

    /* Execute only once */
    if (_init_flag == 0) {
        rt_memset(&wlan_mgmt_device, 0, sizeof(struct rt_wlan_mgmt_device));

#ifdef RT_USING_DEVICE_OPS
        wlan_mgmt_device.device.ops = &wlan_mgmt_dev_ops;
#else
        wlan_mgmt_device.device.init       = _wlan_mgmt_dev_init;
        wlan_mgmt_device.device.open       = RT_NULL;
        wlan_mgmt_device.device.close      = RT_NULL;
        wlan_mgmt_device.device.read       = RT_NULL;
        wlan_mgmt_device.device.write      = RT_NULL;
        wlan_mgmt_device.device.control    = _rt_wlan_dev_control;
#endif
        wlan_mgmt_device.device.user_data = RT_NULL;
        wlan_mgmt_device.device.type = RT_Device_Class_Miscellaneous;

        rt_mutex_init(&wlan_mgmt_device.lock, "wlan_mgmt_dev", RT_IPC_FLAG_FIFO);

        if(RT_EOK != (err = rt_device_register(&wlan_mgmt_device.device, "wlan_mgmt", RT_DEVICE_FLAG_RDWR))) {
            LOG_E("wlan_mgmt_device register failed, %d\n", errno);
        }
    }
    return 0;
}
INIT_APP_EXPORT(wlan_mgmt_dev_init);