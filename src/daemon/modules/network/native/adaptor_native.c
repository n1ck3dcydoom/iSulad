/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2020. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: zhangxiaoyu
 * Create: 2020-10-31
 * Description: provide network config functions
 ********************************************************************************/

#include "adaptor_native.h"

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "utils.h"
#include "path.h"
#include "error.h"
#include "err_msg.h"
#include "linked_list.h"
#include "isulad_config.h"
#include "isula_libutils/log.h"
#include "utils_network.h"
#include "network_tools.h"
#include "cni_operate.h"

#define MAX_BRIDGE_ID 1024

#define NETWOKR_DRIVER_BRIDGE "bridge"
#define NETWOKR_DRIVER_MACVLAN "macvlan"

struct subnet_scope {
    char *begin;
    char *end;
};
/* Reserved IPv4 address ranges for private networks */
const struct subnet_scope g_private_networks[] = {
    /* Class C network 192.168.0.0/16 */
    { "192.168.0.0/24", "192.168.255.0/24" },
    /* Class B network 172.16.0.0/12 */
    { "172.16.0.0/24", "172.31.255.0/24" },
    /* Class A network 10.0.0.0/8 */
    { "10.0.0.0/24", "10.255.255.0/24" },
};

typedef struct native_newtork_t {
    // network conflist
    struct cni_network_list_conf *conflist;

    // containers linked to network
    struct linked_list containers_list;

    pthread_rwlock_t rwlock;
} native_network;

typedef struct native_store_t {
    // string -> ptr(native_newtork)
    map_t *name_to_network;

    size_t network_len;

    char *conf_dir;

    char **bin_paths;
    size_t bin_paths_len;

    pthread_rwlock_t rwlock;
} native_store;

static native_store g_store = { 0 };

enum lock_type { SHARED = 0, EXCLUSIVE };
static inline bool native_store_lock(enum lock_type type)
{
    int nret = 0;

    if (type == SHARED) {
        nret = pthread_rwlock_rdlock(&g_store.rwlock);
    } else {
        nret = pthread_rwlock_wrlock(&g_store.rwlock);
    }
    if (nret != 0) {
        errno = nret;
        SYSERROR("Lock network list failed");
        return false;
    }

    return true;
}

static inline void native_store_unlock()
{
    int nret = 0;

    nret = pthread_rwlock_unlock(&g_store.rwlock);
    if (nret != 0) {
        errno = nret;
        SYSERROR("Unlock network list failed");
    }
}

static inline void native_network_lock(enum lock_type type, native_network *network)
{
    int nret = 0;

    if (type == SHARED) {
        nret = pthread_rwlock_rdlock(&network->rwlock);
    } else {
        nret = pthread_rwlock_wrlock(&network->rwlock);
    }
    if (nret != 0) {
        errno = nret;
        SYSERROR("Lock network list failed");
    }
}

static inline void native_network_unlock(native_network *network)
{
    int nret = 0;

    nret = pthread_rwlock_unlock(&network->rwlock);
    if (nret != 0) {
        errno = nret;
        SYSERROR("Unlock network list failed");
    }
}

struct plugin_op {
    const char *plugin;
    cni_net_conf * (*op)(const network_create_request *request);
};

static cni_net_conf *conf_bridge_plugin(const network_create_request *request);
static cni_net_conf *conf_portmap_plugin(const network_create_request *request);
static cni_net_conf *conf_firewall_plugin(const network_create_request *request);
static cni_net_conf *conf_dnsname_plugin(const network_create_request *request);

static const struct plugin_op g_bridge_plugin = {
    .plugin = "bridge",
    .op = conf_bridge_plugin,
};

static const struct plugin_op g_portmap_plugin = {
    .plugin = "portmap",
    .op = conf_portmap_plugin,
};

static const struct plugin_op g_firewall_plugin = {
    .plugin = "firewall",
    .op = conf_firewall_plugin,
};

static const struct plugin_op g_dnsname_plugin = {
    .plugin = "dnsname",
    .op = conf_dnsname_plugin,
};

#define BRIDGE_DRIVER_PLUGINS_LEN 4
static const struct plugin_op *g_bridge_driver_plugins[] = { &g_bridge_plugin, &g_portmap_plugin, &g_firewall_plugin, &g_dnsname_plugin};

struct net_driver_ops {
    cni_net_conf_list * (*conf)(const network_create_request *request, string_array *missing);
    int (*check)(const network_create_request *request);
    int (*remove)(cni_net_conf_list *list);
};

static cni_net_conf_list *conf_bridge(const network_create_request *request, string_array *missing);
static int check_bridge(const network_create_request *request);
static int remove_bridge(cni_net_conf_list *list);

static const struct net_driver_ops g_bridge_ops = {
    .conf = conf_bridge,
    .check = check_bridge,
    .remove = remove_bridge,
};

static const struct net_driver_ops g_macvlan_ops = {
    .conf = NULL,
    .check = NULL,
    .remove = NULL,
};

struct net_driver {
    const char *driver;
    const struct net_driver_ops *ops;
};

static const struct net_driver g_drivers[] = {
    {
        .driver = NETWOKR_DRIVER_BRIDGE,
        .ops = &g_bridge_ops,
    },
    {
        .driver = NETWOKR_DRIVER_MACVLAN,
        .ops = &g_macvlan_ops,
    },
};

static const size_t g_numnets = sizeof(g_drivers) / sizeof(struct net_driver);

static const struct net_driver *get_ops_by_driver(const char *driver)
{
    size_t i;

    if (driver == NULL) {
        // default bridge driver
        return &g_drivers[0];
    }

    for (i = 0; i < g_numnets; i++) {
        if (strcmp(driver, g_drivers[i].driver) == 0) {
            return &g_drivers[i];
        }
    }

    WARN("Do not support network driver: %s", driver);
    return NULL;
}

static void native_network_free(native_network *network)
{
    struct linked_list *it = NULL;
    struct linked_list *next = NULL;

    if (network == NULL) {
        return;
    }

    free_cni_network_list_conf(network->conflist);
    network->conflist = NULL;

    linked_list_for_each_safe(it, &(network->containers_list), next) {
        char *cont_id = (char *)it->elem;
        linked_list_del(it);
        free(cont_id);
        free(it);
        it = NULL;
    }

    pthread_rwlock_destroy(&(network->rwlock));

    free(network);
}

static void native_network_kvfree(void *key, void *value)
{
    native_network *conf = (native_network *)value;
    native_network_free(conf);
    free(key);
}

void native_destory()
{
    (void)native_store_lock(EXCLUSIVE);

    if (g_store.name_to_network != NULL) {
        map_free(g_store.name_to_network);
    }
    g_store.name_to_network = NULL;
    g_store.network_len = 0;

    free(g_store.conf_dir);
    g_store.conf_dir = NULL;

    util_free_array_by_len(g_store.bin_paths, g_store.bin_paths_len);
    g_store.bin_paths = NULL;
    g_store.bin_paths_len = 0;
}

static bool is_native_config_file(const char *filename)
{
    if (filename == NULL) {
        return false;
    }

    return strncmp(ISULAD_CNI_NETWORK_CONF_FILE_PRE, filename, strlen(ISULAD_CNI_NETWORK_CONF_FILE_PRE)) == 0;
}

static native_network *native_network_init(struct cni_network_list_conf **conflist)
{
    native_network *network = NULL;

    network = (native_network *)util_common_calloc_s(sizeof(native_network));
    if (network == NULL) {
        ERROR("Out of memory");
        return NULL;
    }

    if (pthread_rwlock_init(&(network->rwlock), NULL) != 0) {
        ERROR("init lock for native network failed");
        goto err_out;
    }

    linked_list_init(&(network->containers_list));
    network->conflist = *conflist;
    *conflist = NULL;

    return network;

err_out:
    free(network);
    return NULL;
}

static int native_store_add_network(struct cni_network_list_conf **conflist)
{
    native_network *network = NULL;

    network = native_network_init(conflist);
    if (network == NULL) {
        ERROR("Failed to init native network");
        return -1;
    }

    if (!map_replace(g_store.name_to_network, (void *)network->conflist->list->name, network)) {
        ERROR("add net failed: %s", network->conflist->list->name);
        native_network_free(network);
        return -1;
    }
    g_store.network_len++;

    return 0;
}

static int load_store_map()
{
    int ret = 0;
    int pos = 0;
    size_t i;
    size_t tmp_len = 0;
    struct cni_network_list_conf **tmp = NULL;
    char message[MAX_BUFFER_SIZE] = { 0 };

    ret = get_net_conflist_from_dir(&tmp, &tmp_len, is_native_config_file);
    if (ret != 0) {
        ERROR("Failed to load net conflist from dir, maybe the net files count is above %d", MAX_NETWORK_CONFIG_FILE_COUNT);
        return -1;
    }

    if (tmp_len == 0) {
        WARN("No native network config list found");
        goto out;
    }

    for (i = 0; i < tmp_len; i++) {
        const char *name = NULL;

        if (tmp[i] == NULL || tmp[i]->list == NULL) {
            continue;
        }

        name = tmp[i]->list->name;
        if (map_search(g_store.name_to_network, (void *)name) != NULL) {
            INFO("Ignore network: %s, because already exist", name);
            continue;
        }

        ret = native_store_add_network(&(tmp[i]));
        if (ret != 0) {
            ERROR("Failed to add network to native store");
            goto out;
        }

        if (strlen(name) + 1 < MAX_BUFFER_SIZE - pos) {
            sprintf(message + pos, "%s,", name);
            pos += strlen(name) + 1;
        }
    }

    if (pos > 0) {
        message[pos - 1] = '\0';
    }
    INFO("Loaded native network conflist file successfully, [ %s ]", message);

out:
    for (i = 0; i < tmp_len; i++) {
        free_cni_network_list_conf(tmp[i]);
        tmp[i] = NULL;
    }
    free(tmp);

    return ret;
}

int native_init(const char *conf_dir, const char **bin_paths, const size_t bin_paths_len)
{
    int ret = 0;

    if (pthread_rwlock_init(&(g_store.rwlock), NULL) != 0) {
        ERROR("init lock for native store failed");
        return -1;
    }

    g_store.name_to_network = map_new(MAP_STR_PTR, MAP_DEFAULT_CMP_FUNC, native_network_kvfree);
    if (g_store.name_to_network == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    ret = load_store_map();
    if (ret != 0) {
        ERROR("Failed to load native store map from dir");
        goto out;
    }

    g_store.conf_dir = util_strdup_s(conf_dir);
    if (util_dup_array_of_strings(bin_paths, bin_paths_len, &g_store.bin_paths, &g_store.bin_paths_len) != 0) {
        ERROR("Failed to dup bin path");
        ret = -1;
        goto out;
    }

out:
    if (ret != 0) {
        native_destory();
        DEBUG("Native adaptor init failed");
    } else {
        DEBUG("Native adaptor init success");
    }

    return ret;
}

bool native_ready()
{
    bool ret = false;

    if (!native_store_lock(SHARED)) {
        return false;
    }

    ret = g_store.network_len > 0;

    native_store_unlock();

    return ret;
}

typedef int (*get_config_callback)(const cni_net_conf_list *list, string_array *array);

static int get_config_net_name(const cni_net_conf_list *list, string_array *array)
{
    if (list->name == NULL) {
        return 0;
    }

    return util_append_string_array(array, list->name);
}

static int get_config_bridge_name(const cni_net_conf_list *list, string_array *array)
{
    size_t i;
    int nret = 0;
    cni_net_conf *plugin = NULL;

    if (list->plugins == NULL) {
        return 0;
    }
    for (i = 0; i < list->plugins_len; i++) {
        plugin = list->plugins[i];
        if (plugin == NULL || strcmp(plugin->type, NETWOKR_DRIVER_BRIDGE) != 0 || plugin->bridge == NULL) {
            continue;
        }
        nret = util_append_string_array(array, plugin->bridge);
        if (nret != 0) {
            return -1;
        }
    }

    return 0;
}

static int get_config_subnet(const cni_net_conf_list *list, string_array *array)
{
    size_t i;
    int nret = 0;
    bool condition = false;
    cni_net_conf *plugin = NULL;

    if (list->plugins == NULL) {
        return 0;
    }

    for (i = 0; i < list->plugins_len; i++) {
        plugin = list->plugins[i];
        condition = plugin == NULL || plugin->ipam == NULL || plugin->ipam->ranges == NULL ||
                    plugin->ipam->ranges_len == 0 || plugin->ipam->ranges[0] == NULL ||
                    plugin->ipam->ranges_item_lens == NULL || plugin->ipam->ranges_item_lens[0] == 0 ||
                    plugin->ipam->ranges[0][0] == NULL || plugin->ipam->ranges[0][0]->subnet == NULL;
        if (condition) {
            continue;
        }
        nret = util_append_string_array(array, plugin->ipam->ranges[0][0]->subnet);
        if (nret != 0) {
            return -1;
        }
    }

    return 0;
}

static int get_cni_config(get_config_callback cb, string_array **array)
{
    int ret = 0;
    map_itor *itor = NULL;
    string_array *tmp = NULL;

    if (!native_store_lock(SHARED)) {
        return -1;
    }

    tmp = util_common_calloc_s(sizeof(string_array));
    if (tmp == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    if (g_store.network_len == 0) {
        *array = tmp;
        tmp = NULL;
        goto out;
    }

    itor = map_itor_new(g_store.name_to_network);
    if (itor == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    for (; map_itor_valid(itor); map_itor_next(itor)) {
        native_network *network = map_itor_value(itor);

        ret = cb(network->conflist->list, tmp);
        if (ret != 0) {
            goto out;
        }
    }
    *array = tmp;
    tmp = NULL;

out:
    map_itor_free(itor);
    native_store_unlock();
    util_free_string_array(tmp);
    return ret;
}

static int get_interface_name(string_array **interface_names)
{
    int ret = 0;
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    string_array *tmp = NULL;

    ret = getifaddrs(&ifaddr);
    if (ret != 0) {
        ERROR("Failed to get if addr");
        return ret;
    }

    tmp = util_common_calloc_s(sizeof(string_array));
    if (tmp == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        // one AF_PACKET address per interface
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_PACKET) {
            continue;
        }
        ret = util_append_string_array(tmp, ifa->ifa_name);
        if (ret != 0) {
            goto out;
        }
    }
    *interface_names = tmp;
    tmp = NULL;

out:
    util_free_string_array(tmp);
    freeifaddrs(ifaddr);
    return ret;
}

static int get_host_net_ip(string_array **host_net_ip)
{
    int ret = 0;
    char ipaddr[INET6_ADDRSTRLEN] = { 0 };
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    string_array *tmp = NULL;

    ret = getifaddrs(&ifaddr);
    if (ret != 0) {
        ERROR("Failed to get if addr");
        return ret;
    }

    tmp = util_common_calloc_s(sizeof(string_array));
    if (tmp == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }

        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (inet_ntop(AF_INET, &(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr), ipaddr, INET_ADDRSTRLEN) ==
                NULL) {
                ERROR("Failed to get ipv4 addr");
                ret = ECOMM;
                goto out;
            }
            ret = util_append_string_array(tmp, ipaddr);
            if (ret != 0) {
                goto out;
            }
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            if (inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr), ipaddr, INET6_ADDRSTRLEN) ==
                NULL) {
                ERROR("Failed to ipv6 addr");
                ret = ECOMM;
                goto out;
            }
            ret = util_append_string_array(tmp, ipaddr);
            if (ret != 0) {
                goto out;
            }
        }
    }

    *host_net_ip = tmp;
    tmp = NULL;

out:
    util_free_string_array(tmp);
    freeifaddrs(ifaddr);
    return ret;
}

/*
 * RETURN VALUE:
 * 0        : net not conflict
 * 1        : net conflict
 * others   : error
 */
static int net_conflict(const struct ipnet *net, const struct ipnet *ipnet)
{
    int ret = 0;
    size_t i = 0;
    uint8_t *first_net = NULL;
    uint8_t *first_ipnet = NULL;

    if (net == NULL || ipnet == NULL) {
        return 0;
    }

    if (net->ip_len != ipnet->ip_len || net->ip_mask_len != ipnet->ip_mask_len) {
        return 0;
    }

    first_net = util_smart_calloc_s(sizeof(uint8_t), net->ip_len);
    if (first_net == NULL) {
        ERROR("Out of memory");
        return -1;
    }
    first_ipnet = util_smart_calloc_s(sizeof(uint8_t), ipnet->ip_len);
    if (first_ipnet == NULL) {
        free(first_net);
        ERROR("Out of memory");
        return -1;
    }

    for (i = 0; i < ipnet->ip_len; i++) {
        first_net[i] = net->ip[i] & net->ip_mask[i];
        first_ipnet[i] = ipnet->ip[i] & ipnet->ip_mask[i];
    }

    if (util_net_contain_ip(net, first_ipnet, ipnet->ip_len, true) ||
        util_net_contain_ip(ipnet, first_net, net->ip_len, true)) {
        ret = 1;
    }

    free(first_net);
    free(first_ipnet);
    return ret;
}

/*
 * RETURN VALUE:
 * 0        : subnet available
 * 1        : subnet not avaliable
 * others   : error
 */
static int check_subnet_available(const char *subnet, const string_array *subnets, const string_array *host_ip)
{
    int ret = 0;
    size_t i = 0;
    uint8_t *ip = NULL;
    size_t ip_len = 0;
    struct ipnet *net = NULL;
    struct ipnet *tmp = NULL;

    ret = util_parse_ipnet_from_str(subnet, &net);
    if (ret != 0 || net == NULL) {
        ERROR("Parse CIDR %s failed", subnet);
        return -1;
    }

    for (i = 0; i < subnets->len; i++) {
        ret = util_parse_ipnet_from_str(subnets->items[i], &tmp);
        if (ret != 0 || tmp == NULL) {
            WARN("Parse CIDR %s failed", subnets->items[i]);
            ret = 0;
            continue;
        }
        ret = net_conflict(tmp, net);
        if (ret != 0) {
            goto out;
        }
        util_free_ipnet(tmp);
        tmp = NULL;
    }

    for (i = 0; i < host_ip->len; i++) {
        ret = util_parse_ip_from_str(host_ip->items[i], &ip, &ip_len);
        if (ret != 0 || ip == NULL || ip_len == 0) {
            WARN("Parse host IP %s failed", host_ip->items[i]);
            ret = 0;
            continue;
        }
        if (util_net_contain_ip(net, ip, ip_len, true)) {
            ret = 1;
            goto out;
        }
        free(ip);
        ip = NULL;
        ip_len = 0;
    }

out:
    free(ip);
    util_free_ipnet(net);
    util_free_ipnet(tmp);
    return ret;
}

static int check_bridge(const network_create_request *request)
{
    int ret = 0;
    string_array *net_names = NULL;
    string_array *subnets = NULL;
    string_array *host_ip = NULL;

    if (request->name != NULL) {
        // check name conflict
        ret = get_cni_config(get_config_net_name, &net_names);
        if (ret != 0) {
            goto out;
        }
        if (util_string_array_contain(net_names, request->name)) {
            isulad_set_error_message("Network name \"%s\" has been used", request->name);
            ret = EINVALIDARGS;
            goto out;
        }
    }

    if (request->subnet == NULL) {
        goto out;
    }

    ret = get_cni_config(get_config_subnet, &subnets);
    if (ret != 0) {
        goto out;
    }

    ret = get_host_net_ip(&host_ip);
    if (ret != 0) {
        goto out;
    }

    ret = check_subnet_available(request->subnet, subnets, host_ip);
    if (ret == 1) {
        isulad_set_error_message("Subnet \"%s\" conflict with CNI config or host network", request->subnet);
        ret = EINVALIDARGS;
    }

out:
    util_free_string_array(net_names);
    util_free_string_array(subnets);
    util_free_string_array(host_ip);
    return ret;
}

static char *find_bridge_name()
{
    int nret = 0;
    int i = 0;
    char *num = NULL;
    char *name = NULL;
    string_array *net_names = NULL;
    string_array *bridge_names = NULL;
    string_array *host_net_names = NULL;
    const char *bridge_name_prefix = "isula-br";

    nret = get_cni_config(get_config_net_name, &net_names);
    if (nret != 0) {
        return NULL;
    }

    nret = get_cni_config(get_config_bridge_name, &bridge_names);
    if (nret != 0) {
        goto out;
    }

    nret = get_interface_name(&host_net_names);
    if (nret != 0) {
        goto out;
    }

    for (i = 0; i < MAX_BRIDGE_ID; i++) {
        free(name);
        name = NULL;
        free(num);
        num = NULL;

        num = util_int_to_string(i);
        if (num == NULL) {
            goto out;
        }
        name = util_string_append(num, bridge_name_prefix);
        if (name == NULL) {
            goto out;
        }
        if (net_names != NULL && util_string_array_contain(net_names, name)) {
            continue;
        }
        if (bridge_names != NULL && util_string_array_contain(bridge_names, name)) {
            continue;
        }
        if (host_net_names != NULL && util_string_array_contain(host_net_names, name)) {
            continue;
        }
        goto out;
    }
    free(name);
    name = NULL;
    isulad_set_error_message("Too many network bridges");

out:
    free(num);
    util_free_string_array(net_names);
    util_free_string_array(bridge_names);
    util_free_string_array(host_net_names);
    return name;
}

static char *find_private_network(char *subnet)
{
    int nret = 0;
    int i = 0;
    uint32_t ip = 0;
    uint32_t mask = 0;
    struct ipnet *ipnet = NULL;
    char *nx = NULL;
    size_t len = sizeof(g_private_networks) / sizeof(g_private_networks[0]);

    if (subnet == NULL) {
        return util_strdup_s(g_private_networks[0].begin);
    }

    for (i = 0; i < len - 1; i++) {
        if (strcmp(subnet, g_private_networks[i].end) == 0) {
            return util_strdup_s(g_private_networks[i + 1].begin);
        }
    }

    nret = util_parse_ipnet_from_str(subnet, &ipnet);
    if (nret != 0 || ipnet == NULL) {
        ERROR("Parse IP %s failed", subnet);
        return NULL;
    }
    for (i = 0; i < ipnet->ip_len; i++) {
        ip <<= 8;
        mask <<= 8;
        ip += (uint32_t)(ipnet->ip[i]);
        mask += (uint32_t)(ipnet->ip_mask[i]);
    }
    mask = ~mask + 1;
    ip += mask;
    mask = 0xff;
    for (i = ipnet->ip_len - 1; i >= 0; i--) {
        ipnet->ip[i] = (uint8_t)(ip & mask);
        ip >>= 8;
    }

    nx = util_ipnet_to_string(ipnet);
    util_free_ipnet(ipnet);

    return nx;
}

static char *find_subnet()
{
    int nret = 0;
    char *subnet = NULL;
    string_array *config_subnet = NULL;
    string_array *host_ip = NULL;

    size_t len = sizeof(g_private_networks) / sizeof(g_private_networks[0]);
    const char *end = g_private_networks[len - 1].end;

    nret = get_cni_config(get_config_subnet, &config_subnet);
    if (nret != 0) {
        return NULL;
    }

    nret = get_host_net_ip(&host_ip);
    if (nret != 0) {
        goto out;
    }

    do {
        char *nx_subnet = find_private_network(subnet);
        if (nx_subnet == NULL) {
            free(subnet);
            subnet = NULL;
            goto out;
        } else {
            free(subnet);
            subnet = nx_subnet;
        }

        nret = check_subnet_available(subnet, config_subnet, host_ip);
        if (nret == 0) {
            goto out;
        }
        if (nret == 1) {
            continue;
        }
        // error
        free(subnet);
        subnet = NULL;
        goto out;
    } while (strcmp(subnet, end) != 0);

    free(subnet);
    subnet = NULL;
    isulad_set_error_message("Cannot find avaliable subnet by default");

out:
    util_free_string_array(config_subnet);
    util_free_string_array(host_ip);
    return subnet;
}

static char *find_gateway(const char *subnet)
{
    int nret = 0;
    size_t i;
    uint8_t *first_ip = NULL;
    char *gateway = NULL;
    struct ipnet *ipnet = NULL;

    nret = util_parse_ipnet_from_str(subnet, &ipnet);
    if (nret != 0 || ipnet == NULL) {
        ERROR("Parse IP %s failed", subnet);
        return NULL;
    }

    first_ip = util_smart_calloc_s(sizeof(uint8_t), ipnet->ip_len);
    if (first_ip == NULL) {
        ERROR("Out of memory");
        goto out;
    }

    if (ipnet->ip_mask[ipnet->ip_mask_len - 1] == 0xff) {
        isulad_set_error_message("No avaliable gateway in %s", subnet);
        goto out;
    }

    for (i = 0; i < ipnet->ip_len; i++) {
        first_ip[i] = ipnet->ip[i] & ipnet->ip_mask[i];
    }
    first_ip[ipnet->ip_len - 1] = first_ip[ipnet->ip_len - 1] | 0x01;
    gateway = util_ip_to_string(first_ip, ipnet->ip_len);

out:
    util_free_ipnet(ipnet);
    free(first_ip);
    return gateway;
}

static cni_net_conf_ipam *conf_bridge_plugin_ipam(const network_create_request *request)
{
    struct ipnet *ipnet = NULL;
    cni_net_conf_ipam *ipam = NULL;

    ipam = util_common_calloc_s(sizeof(cni_net_conf_ipam));
    if (ipam == NULL) {
        ERROR("Out of memory");
        return NULL;
    }

    ipam->type = util_strdup_s("host-local");
    ipam->routes = util_common_calloc_s(sizeof(cni_network_route *));
    if (ipam->routes == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }
    ipam->routes[0] = util_common_calloc_s(sizeof(cni_network_route));
    if (ipam->routes[0] == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }
    ipam->routes_len++;
    ipam->routes[0]->dst = util_strdup_s("0.0.0.0/0");

    ipam->ranges =
        (cni_net_conf_ipam_ranges_element ***)util_common_calloc_s(sizeof(cni_net_conf_ipam_ranges_element **));
    if (ipam->ranges == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }
    ipam->ranges_item_lens = (size_t *)util_common_calloc_s(sizeof(size_t));
    if (ipam->ranges_item_lens == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }

    ipam->ranges[0] =
        (cni_net_conf_ipam_ranges_element **)util_common_calloc_s(sizeof(cni_net_conf_ipam_ranges_element *));
    if (ipam->ranges[0] == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }
    ipam->ranges_len++;
    ipam->ranges[0][0] =
        (cni_net_conf_ipam_ranges_element *)util_common_calloc_s(sizeof(cni_net_conf_ipam_ranges_element));
    if (ipam->ranges[0][0] == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }
    (ipam->ranges_item_lens)[0]++;

    if (request->subnet != NULL) {
        // reduce subnet, e.g. 192.168.2.5/16 -> 192.168.0.0/16
        if (util_parse_ipnet_from_str(request->subnet, &ipnet) != 0) {
            ERROR("Failed to parse cide subnet %s", request->subnet);
            goto err_out;
        }

        if (util_reduce_ip_by_mask(ipnet) != 0) {
            ERROR("Failed to reduce ip by mask");
            goto err_out;
        }

        ipam->ranges[0][0]->subnet = util_ipnet_to_string(ipnet);
        if (ipam->ranges[0][0]->subnet == NULL) {
            ERROR("Failed to convert ipnet to string");
            goto err_out;
        }

        if (strcmp(request->subnet, ipam->ranges[0][0]->subnet) != 0) {
            DEBUG("reduce subnet \"%s\" to \"%s\"", request->subnet, ipam->ranges[0][0]->subnet);
        }
    } else {
        ipam->ranges[0][0]->subnet = find_subnet();
        if (ipam->ranges[0][0]->subnet == NULL) {
            ERROR("Failed to find available subnet");
            goto err_out;
        }
    }

    if (request->gateway != NULL) {
        ipam->ranges[0][0]->gateway = util_strdup_s(request->gateway);
    } else {
        ipam->ranges[0][0]->gateway = find_gateway(ipam->ranges[0][0]->subnet);
        if (ipam->ranges[0][0]->gateway == NULL) {
            ERROR("Failed to find gateway");
            goto err_out;
        }
    }

    util_free_ipnet(ipnet);
    return ipam;

err_out:
    free_cni_net_conf_ipam(ipam);
    util_free_ipnet(ipnet);
    return NULL;
}

static cni_net_conf *conf_bridge_plugin(const network_create_request *request)
{
    cni_net_conf *plugin = NULL;

    plugin = util_common_calloc_s(sizeof(cni_net_conf));
    if (plugin == NULL) {
        ERROR("Out of memory");
        return NULL;
    }

    plugin->type = util_strdup_s(NETWOKR_DRIVER_BRIDGE);
    plugin->bridge = find_bridge_name();
    if (plugin->bridge == NULL) {
        ERROR("Failed to find avaliable bridge name");
        goto err_out;
    }

    if (request->internal) {
        plugin->is_gateway = false;
        plugin->ip_masq = false;
    } else {
        plugin->is_gateway = true;
        plugin->ip_masq = true;
    }
    plugin->hairpin_mode = true;

    plugin->ipam = conf_bridge_plugin_ipam(request);
    if (plugin->ipam == NULL) {
        ERROR("Failed to config bridge ipam");
        goto err_out;
    }

    return plugin;

err_out:
    free_cni_net_conf(plugin);
    return NULL;
}

static cni_net_conf *conf_portmap_plugin(const network_create_request *request)
{
    cni_net_conf *plugin = NULL;

    plugin = util_common_calloc_s(sizeof(cni_net_conf));
    if (plugin == NULL) {
        ERROR("Out of memory");
        return NULL;
    }

    plugin->type = util_strdup_s("portmap");
    plugin->capabilities = util_common_calloc_s(sizeof(json_map_string_bool));
    if (plugin->capabilities == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }
    plugin->capabilities->keys = util_common_calloc_s(sizeof(char *));
    if (plugin->capabilities->keys == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }
    plugin->capabilities->keys[0] = util_strdup_s("portMappings");
    plugin->capabilities->values = util_common_calloc_s(sizeof(bool));
    if (plugin->capabilities->values == NULL) {
        free(plugin->capabilities->keys[0]);
        ERROR("Out of memory");
        goto err_out;
    }
    plugin->capabilities->values[0] = true;
    plugin->capabilities->len++;

    return plugin;

err_out:
    free_cni_net_conf(plugin);
    return NULL;
}

static cni_net_conf *conf_firewall_plugin(const network_create_request *request)
{
    cni_net_conf *plugin = NULL;

    plugin = util_common_calloc_s(sizeof(cni_net_conf));
    if (plugin == NULL) {
        ERROR("Out of memory");
        return NULL;
    }
    plugin->type = util_strdup_s("firewall");

    return plugin;
}

static cni_net_conf *conf_dnsname_plugin(const network_create_request *request)
{
    cni_net_conf *plugin = NULL;

    plugin = (cni_net_conf *)util_common_calloc_s(sizeof(cni_net_conf));
    if (plugin == NULL) {
        ERROR("Out of memory");
        return NULL;
    }
    plugin->type = util_strdup_s("dnsname");
    plugin->domain_name = util_strdup_s(ISULAD_DNS_DOMAIN_NAME);

    plugin->capabilities = util_common_calloc_s(sizeof(json_map_string_bool));
    if (plugin->capabilities == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }
    plugin->capabilities->keys = util_common_calloc_s(sizeof(char *));
    if (plugin->capabilities->keys == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }
    plugin->capabilities->keys[0] = util_strdup_s("aliases");
    plugin->capabilities->values = util_common_calloc_s(sizeof(bool));
    if (plugin->capabilities->values == NULL) {
        free(plugin->capabilities->keys[0]);
        ERROR("Out of memory");
        goto err_out;
    }
    plugin->capabilities->values[0] = true;
    plugin->capabilities->len++;

    return plugin;

err_out:
    free_cni_net_conf(plugin);
    return NULL;
}

static bool cni_bin_detect(const char *cni)
{
    size_t i;
    char *path = NULL;

    for (i = 0; i < g_store.bin_paths_len; i++) {
        path = util_path_join(g_store.bin_paths[i], cni);
        if (path == NULL) {
            ERROR("Failed to join path %s and %s", g_store.bin_paths[i], cni);
            return false;
        }

        if (util_file_exists(path)) {
            free(path);
            return true;
        }

        free(path);
        path = NULL;
    }

    return false;
}

static cni_net_conf_list *conf_bridge(const network_create_request *request, string_array *missing)
{
    size_t i;
    cni_net_conf_list *list = NULL;

    list = (cni_net_conf_list *)util_common_calloc_s(sizeof(cni_net_conf_list));
    if (list == NULL) {
        ERROR("Out of memory");
        return NULL;
    }

    list->plugins = (cni_net_conf **)util_smart_calloc_s(sizeof(cni_net_conf *), BRIDGE_DRIVER_PLUGINS_LEN);
    if (list->plugins == NULL) {
        ERROR("Out of memory");
        goto err_out;
    }

    for (i = 0; i < BRIDGE_DRIVER_PLUGINS_LEN; i++) {
        cni_net_conf *plugin = NULL;

        if (!cni_bin_detect(g_bridge_driver_plugins[i]->plugin)) {
            // skip conf dnsname if dnsname plugin not exist
            if (strcmp(g_bridge_driver_plugins[i]->plugin, g_dnsname_plugin.plugin) == 0) {
                continue;
            }

            if (util_append_string_array(missing, g_bridge_driver_plugins[i]->plugin) != 0) {
                ERROR("Failed to append string %s to array", g_bridge_driver_plugins[i]->plugin);
                goto err_out;
            }
        }

        plugin = g_bridge_driver_plugins[i]->op(request);
        if (plugin == NULL) {
            ERROR("Failed to config %s plugin", g_bridge_driver_plugins[i]->plugin);
            goto err_out;
        }
        list->plugins[i] = plugin;
        list->plugins_len++;
    }

    list->cni_version = util_strdup_s(CURRENT_VERSION);
    if (request->name != NULL) {
        list->name = util_strdup_s(request->name);
    } else {
        // consider first plugin (bridge) ifname as conflist name
        list->name = util_strdup_s(list->plugins[0]->bridge);
    }

    return list;

err_out:
    free_cni_net_conf_list(list);
    return NULL;
}

static int create_conflist_file(struct cni_network_list_conf *conflist)
{
    int ret = 0;
    int nret = 0;
    char conflist_file[PATH_MAX] = { 0x00 };
    char *conflist_json = NULL;
    parser_error err = NULL;

    if (!util_dir_exists(g_store.conf_dir)) {
        ret = util_mkdir_p(g_store.conf_dir, CONFIG_DIRECTORY_MODE);
        if (ret != 0) {
            ERROR("Failed to create network config directory %s", g_store.conf_dir);
            isulad_set_error_message("Failed to create network config directory %s", g_store.conf_dir);
            goto out;
        }
    }

    nret = snprintf(conflist_file, sizeof(conflist_file), "%s/%s%s.conflist", g_store.conf_dir,
                    ISULAD_CNI_NETWORK_CONF_FILE_PRE, conflist->list->name);
    if ((size_t)nret >= sizeof(conflist_file) || nret < 0) {
        ERROR("Failed to snprintf conflist_file");
        ret = -1;
        goto out;
    }

    conflist_json = cni_net_conf_list_generate_json(conflist->list, NULL, &err);
    if (conflist_json == NULL) {
        ERROR("Failed to generate conf list json: %s", err);
        ret = -1;
        goto out;
    }

    if (util_file_exists(conflist_file)) {
        ERROR("File %s exist", conflist_file);
        isulad_set_error_message("File %s exist", conflist_file);
        ret = -1;
        goto out;
    }

    if (util_atomic_write_file(conflist_file, conflist_json, strlen(conflist_json), CONFIG_FILE_MODE, true) != 0) {
        ERROR("Failed write %s", conflist_file);
        isulad_set_error_message("Failed write %s", conflist_file);
        ret = -1;
        goto out;
    }

    conflist->bytes = util_strdup_s(conflist_json);

out:
    free(conflist_json);
    free(err);
    return ret;
}

static int set_missing_plugin_err_msg(const string_array *missing)
{
    int ret = 0;
    char *cni_str = NULL;
    char *path_str = NULL;

    if (missing->items == NULL) {
        return ret;
    }

    cni_str = util_string_join(",", (const char **)missing->items, missing->len);
    if (cni_str == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    path_str = util_string_join(",", (const char **)g_store.bin_paths, g_store.bin_paths_len);
    if (path_str == NULL) {
        ERROR("Out of memory");
        free(cni_str);
        ret = -1;
        goto out;
    }

    isulad_set_error_message("WARN:cannot find cni plugin \"%s\" in dir \"%s\"", cni_str, path_str);
    free(cni_str);
    free(path_str);

out:
    return ret;
}

int native_config_create(const network_create_request *request, char **name, uint32_t *cc)
{
    int ret = 0;
    string_array *missing = NULL;
    struct cni_network_list_conf *conflist = NULL;
    const struct net_driver *pnet = NULL;

    if (request == NULL || name == NULL || cc == NULL) {
        ERROR("Invalid arguments");
        return -1;
    }

    EVENT("Event: {Object: network, Type: creating, Target: %s}", request->name);

    pnet = get_ops_by_driver(request->driver);
    if (pnet == NULL || strcmp(pnet->driver, NETWOKR_DRIVER_BRIDGE) != 0) {
        ERROR("Cannot support driver %s", request->driver);
        isulad_set_error_message("Cannot support driver: %s", request->driver);
        *cc = ISULAD_ERR_INPUT;
        return -1;
    }

    if (pnet->ops->check == NULL || pnet->ops->conf == NULL) {
        ERROR("net type: %s unsupport ops", pnet->driver);
        return -1;
    }

    ret = pnet->ops->check(request);
    if (ret != 0) {
        ERROR("Failed to check %s", pnet->driver);
        *cc = ISULAD_ERR_INPUT;
        return -1;
    }

    missing = (string_array *)util_common_calloc_s(sizeof(string_array));
    if (missing == NULL) {
        ERROR("Out of memory");
        *cc = ISULAD_ERR_EXEC;
        ret = -1;
        goto out;
    }

    conflist = (struct cni_network_list_conf *)util_common_calloc_s(sizeof(struct cni_network_list_conf));
    if (conflist == NULL) {
        ERROR("Out of memory");
        *cc = ISULAD_ERR_EXEC;
        ret = -1;
        goto out;
    }

    conflist->list = pnet->ops->conf(request, missing);
    if (conflist->list == NULL) {
        ERROR("Failed to conf %s", pnet->driver);
        *cc = ISULAD_ERR_EXEC;
        ret = -1;
        goto out;
    }
    *name = util_strdup_s(conflist->list->name);

    ret = create_conflist_file(conflist);
    if (ret != 0) {
        ERROR("Failed to create conflist file");
        *cc = ISULAD_ERR_EXEC;
        goto out;
    }

    if (!native_store_lock(EXCLUSIVE)) {
        *cc = ISULAD_ERR_EXEC;
        ret = -1;
        goto out;
    }

    ret = native_store_add_network(&conflist);
    native_store_unlock();
    if (ret != 0) {
        ERROR("Failed to add network to native store");
        *cc = ISULAD_ERR_EXEC;
        goto out;
    }

    ret = set_missing_plugin_err_msg(missing);
    if (ret != 0) {
        ERROR("Failed to set missing plugin err msg");
        *cc = ISULAD_ERR_EXEC;
        goto out;
    }

    EVENT("Event: {Object: network, Type: created, Target: %s}", *name);

out:
    free_cni_network_list_conf(conflist);
    util_free_string_array(missing);

    return ret;
}

int native_config_inspect(const char *name, char **network_json)
{
    int ret = 0;
    map_itor *itor = NULL;

    if (name == NULL || network_json == NULL) {
        ERROR("Invalid arguments");
        return -1;
    }

    EVENT("Event: {Object: network, Type: inspecting, Target: %s}", name);

    if (!native_store_lock(SHARED)) {
        return -1;
    }

    if (g_store.network_len == 0) {
        ret = -1;
        goto out;
    }

    itor = map_itor_new(g_store.name_to_network);
    if (itor == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    for (; map_itor_valid(itor); map_itor_next(itor)) {
        native_network *network = map_itor_value(itor);

        if (network->conflist->list->name == NULL || strcmp(network->conflist->list->name, name) != 0) {
            continue;
        }
        *network_json = util_strdup_s(network->conflist->bytes);

        // TODO: inspect the linked containers ip info

        EVENT("Event: {Object: network, Type: inspected, Target: %s}", name);
        goto out;
    }

    ret = -1;

out:
    map_itor_free(itor);
    native_store_unlock();
    if (ret != 0) {
        isulad_set_error_message("No such network %s", name);
    }
    return ret;
}

static bool network_info_match_filter(const cni_net_conf_list *list, const struct filters_args *filters)
{
    size_t i;
    size_t len = list->plugins_len;

    if (!filters_args_match(filters, "name", list->name)) {
        return false;
    }

    for (i = 0; i < len; i++) {
        if (filters_args_match(filters, "plugin", list->plugins[i]->type)) {
            return true;
        }
    }

    return false;
}

static network_network_info *get_network_info(const cni_net_conf_list *list)
{
    size_t i;
    int nret = 0;
    network_network_info *net_info = NULL;

    net_info = (network_network_info *)util_common_calloc_s(sizeof(network_network_info));
    if (net_info == NULL) {
        ERROR("Out of memory");
        return NULL;
    }

    net_info->name = util_strdup_s(list->name);
    net_info->version = util_strdup_s(list->cni_version);
    net_info->plugins_len = 0;
    if (list->plugins_len == 0) {
        return net_info;
    }

    for (i = 0; i < list->plugins_len; i++) {
        if (list->plugins[i]->type == NULL) {
            continue;
        }
        nret = util_array_append(&net_info->plugins, list->plugins[i]->type);
        if (nret != 0) {
            ERROR("Failed to append network plugins array");
            goto err_out;
        }
        net_info->plugins_len++;
    }
    return net_info;

err_out:
    free_network_network_info(net_info);
    return NULL;
}

static void free_network_info_arr(network_network_info **networks, size_t len)
{
    size_t i;

    if (networks == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        free_network_network_info(networks[i]);
    }
    free(networks);
}

int native_config_list(const struct filters_args *filters, network_network_info ***networks, size_t *networks_len)
{
    int ret = 0;
    size_t old_size, new_size;
    network_network_info **nets = NULL;
    size_t nets_len = 0;
    network_network_info *net_info = NULL;
    map_itor *itor = NULL;

    if (networks == NULL || networks_len == NULL) {
        ERROR("Invalid arguments");
        return -1;
    }

    EVENT("Event: {Object: network, Type: listing}");

    if (!native_store_lock(SHARED)) {
        return -1;
    }

    if (g_store.network_len == 0) {
        goto out;
    }

    nets = (network_network_info **)util_smart_calloc_s(sizeof(network_network_info *), g_store.network_len);
    if (nets == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    itor = map_itor_new(g_store.name_to_network);
    if (itor == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    for (; map_itor_valid(itor); map_itor_next(itor)) {
        native_network *network = map_itor_value(itor);

        if (filters != NULL && !network_info_match_filter(network->conflist->list, filters)) {
            continue;
        }
        net_info = get_network_info(network->conflist->list);
        if (net_info == NULL) {
            ret = -1;
            goto out;
        }
        nets[nets_len] = net_info;
        net_info = NULL;
        nets_len++;
    }

    if (g_store.network_len != nets_len) {
        if (nets_len == 0) {
            goto out;
        }

        old_size = g_store.network_len * sizeof(network_network_info *);
        new_size = nets_len * sizeof(network_network_info *);
        ret = util_mem_realloc((void **)&nets, new_size, nets, old_size);
        if (ret != 0) {
            ERROR("Out of memory");
            goto out;
        }
    }
    *networks = nets;
    nets = NULL;
    *networks_len = nets_len;
    nets_len = 0;

    EVENT("Event: {Object: network, Type: listed}");

out:
    map_itor_free(itor);
    native_store_unlock();
    free_network_info_arr(nets, nets_len);
    return ret;
}

static native_network *get_network_by_name(const char *name)
{
    char *json = NULL;
    map_itor *itor = NULL;
    native_network *network = NULL;

    // native store lock by caller function
    if (g_store.network_len == 0) {
        isulad_set_error_message("Cannot find network %s", name);
        goto out;
    }

    itor = map_itor_new(g_store.name_to_network);
    if (itor == NULL) {
        ERROR("Out of memory");
        goto out;
    }

    for (; map_itor_valid(itor); map_itor_next(itor)) {
        network = map_itor_value(itor);

        if (network->conflist->list->name == NULL || strcmp(network->conflist->list->name, name) != 0) {
            continue;
        }
        break;
    }

    if (!map_itor_valid(itor)) {
        isulad_set_error_message("Cannot find network %s", name);
        network = NULL;
        goto out;
    }

out:
    free(json);
    map_itor_free(itor);
    return network;
}

bool has_connected_container(native_network *network)
{
    bool ret = true;
    int pos = 0;
    struct linked_list *it = NULL;
    struct linked_list *next = NULL;
    char message[MAX_BUFFER_SIZE] = { 0 };

    native_network_lock(SHARED, network);

    if (linked_list_empty(&network->containers_list)) {
        ret = false;
        goto out;
    }

    linked_list_for_each_safe(it, &(network->containers_list), next) {
        char *cont_id = (char *)it->elem;
        if (strlen(cont_id) + 1 >= MAX_BUFFER_SIZE - pos) {
            break;
        }
        sprintf(message + pos, "%s,", cont_id);
        pos += strlen(cont_id) + 1;
    }

    if (pos > 0) {
        message[pos - 1] = '\0';
    }

    INFO("network %s has connected containers [ %s ]", network->conflist->list->name, message);

out:
    native_network_unlock(network);
    return ret;
}

static const struct net_driver *get_ops_by_conflist(cni_net_conf_list *conflist)
{
    size_t i;
    const struct net_driver *pnet = NULL;

    for (i = 0; i < conflist->plugins_len; i++) {
        if (conflist->plugins[i] == NULL || conflist->plugins[i]->type == NULL) {
            continue;
        }

        pnet = get_ops_by_driver(conflist->plugins[i]->type);
        if (pnet != NULL) {
            return pnet;
        }
    }

    return NULL;
}

static void run_delete_device(void *args)
{
    char **tmp_args = (char **)args;
    const size_t CMD_ARGS_NUM = 4;

    if (util_array_len((const char **)tmp_args) != (size_t)CMD_ARGS_NUM) {
        ERROR("delete device need four args");
        exit(1);
    }

    execvp(tmp_args[0], tmp_args);
}

static int remove_interface(const char *ifa)
{
    int ret = 0;
    size_t i = 0;
    const size_t args_len = 5;
    char **args = NULL;
    string_array *interfaces = NULL;
    char *stdout_msg = NULL;
    char *stderr_msg = NULL;

    ret = get_interface_name(&interfaces);
    if (ret != 0) {
        ERROR("Failed to get interface names");
        return -1;
    }

    if (interfaces->len == 0) {
        return 0;
    }

    if (!util_string_array_contain(interfaces, ifa)) {
        goto out;
    }

    args = (char **)util_smart_calloc_s(sizeof(char *), args_len);
    if (args == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    args[i++] = util_strdup_s("ip");
    args[i++] = util_strdup_s("link");
    args[i++] = util_strdup_s("delete");
    args[i] = util_strdup_s(ifa);

    if (!util_exec_cmd(run_delete_device, args, NULL, &stdout_msg, &stderr_msg)) {
        ERROR("Unexpected command output %s with error: %s", stdout_msg, stderr_msg);
        ret = -1;
    }

out:
    util_free_string_array(interfaces);
    util_free_array(args);
    free(stdout_msg);
    free(stderr_msg);
    return ret;
}

static int remove_bridge(cni_net_conf_list *list)
{
    size_t i;
    cni_net_conf *plugin = NULL;

    for (i = 0; i < list->plugins_len; i++) {
        plugin = list->plugins[i];
        if (plugin == NULL || strcmp(plugin->type, NETWOKR_DRIVER_BRIDGE) != 0 || plugin->bridge == NULL) {
            continue;
        }
        break;
    }

    if (i == list->plugins_len) {
        return 0;
    }

    if (remove_interface(plugin->bridge) != 0) {
        ERROR("Failed to remove interface %s", plugin->bridge);
        return -1;
    }

    return 0;
}

static char *get_file_path_by_name(const char *name)
{
    int nret = 0;
    char *path = NULL;
    char conflist_file[PATH_MAX] = { 0x00 };

    nret = snprintf(conflist_file, sizeof(conflist_file), "%s/%s%s.conflist", g_store.conf_dir,
                    ISULAD_CNI_NETWORK_CONF_FILE_PRE, name);
    if ((size_t)nret >= sizeof(conflist_file) || nret < 0) {
        ERROR("Failed to snprintf conflist_file");
        goto out;
    }
    path = util_strdup_s(conflist_file);

out:
    return path;
}

int native_config_remove(const char *name, char **res_name)
{
    int ret = 0;
    int get_err = 0;
    char *path = NULL;
    const struct net_driver *pnet = NULL;
    native_network *network = NULL;

    if (name == NULL || res_name == NULL) {
        ERROR("Invalid arguments");
        return -1;
    }

    EVENT("Event: {Object: network, Type: removing, Target: %s}", name);

    if (!native_store_lock(EXCLUSIVE)) {
        return -1;
    }

    network = get_network_by_name(name);
    if (network == NULL) {
        ERROR("Failed to get network by name");
        ret = -1;
        goto out;
    }

    if (has_connected_container(network)) {
        isulad_set_error_message("network %s has connected containers", network->conflist->list->name);
        ret = -1;
        goto out;
    }

    DAEMON_CLEAR_ERRMSG();
    pnet = get_ops_by_conflist(network->conflist->list);
    if (pnet != NULL) {
        if (pnet->ops->remove == NULL) {
            WARN("net type: %s unsupport remove", pnet->driver);
            isulad_append_error_message("net type: %s unsupport remove. ", pnet->driver);
        } else if (pnet->ops->remove(network->conflist->list) != 0) {
            WARN("Failed to remove %s interface", pnet->driver);
            isulad_append_error_message("Failed to remove %s interface. ", pnet->driver);
        }
    }

    path = get_file_path_by_name(network->conflist->list->name);
    if (path == NULL) {
        WARN("Failed to get %s file path", network->conflist->list->name);
        isulad_append_error_message("Failed to get %s file path. ", network->conflist->list->name);
    } else if (!util_force_remove_file(path, &get_err)) {
        errno = get_err;
        SYSWARN("Failed to delete %s.", path);
        isulad_append_error_message("Failed to delete %s.", path);
    }

    if (!map_remove(g_store.name_to_network, (void *)network->conflist->list->name)) {
        WARN("remove network failed: %s", network->conflist->list->name);
        isulad_append_error_message("remove network failed: %s. ", network->conflist->list->name);
    } else {
        g_store.network_len--;
        network = NULL;
    }

    *res_name = util_strdup_s(name);

    EVENT("Event: {Object: network, Type: removed, Target: %s}", name);

out:
    native_store_unlock();
    free(path);
    return ret;
}

static int do_native_append_cni_result(const char *name, const char *interface, const struct cni_opt_result *cni_result,
                                       network_api_result_list *list)
{
    struct network_api_result *work = NULL;

    if (cni_result == NULL) {
        INFO("get empty result from network: %s", name);
        return 0;
    }

    work = network_parse_to_api_result(name, interface, cni_result);
    if (work == NULL) {
        return -1;
    }

    if (network_api_result_list_append(work, list)) {
        return 0;
    }

    free_network_api_result(work);
    return -1;
}

static int do_foreach_network_op(const network_api_conf *conf, bool ignore_nofound, cni_op_t op,
                                 network_api_result_list *list)
{
    int ret = 0;
    size_t i;
    struct cni_manager manager = { 0 };
    struct cni_opt_result *cni_result = NULL;
    bool use_annotations = false;

    // Step1, build cni manager config
    manager.id = conf->pod_id;
    manager.netns_path = conf->netns_path;
    manager.cni_args = conf->args;

    // Step 2, foreach operator for all network plane
    for (i = 0; i < conf->extral_nets_len; i++) {
        native_network *network = NULL;

        if (conf->extral_nets[i] == NULL || conf->extral_nets[i]->name == NULL ||
            conf->extral_nets[i]->interface == NULL) {
            WARN("empty config, just ignore net idx: %zu", i);
            continue;
        }
        network = map_search(g_store.name_to_network, (void *)conf->extral_nets[i]->name);
        if (network == NULL) {
            ERROR("Cannot found net: %s", conf->extral_nets[i]->name);
            // do best to detach network plane of container
            if (ignore_nofound) {
                continue;
            }
            isulad_set_error_message("Cannot found net: %s", conf->extral_nets[i]->name);
            ret = -1;
            goto out;
        }
        // use conf interface
        manager.ifname = conf->extral_nets[i]->interface;

        // external configurations(portmappings, iprange, bandwith and so on) for mult-networks
        // should work for only one:
        // for first network is a good choice.
        if (!use_annotations) {
            manager.annotations = conf->annotations;
            use_annotations = true;
        } else {
            manager.annotations = NULL;
        }

        // clear cni result
        free_cni_opt_result(cni_result);
        cni_result = NULL;

        if (op(&manager, network->conflist, &cni_result) != 0) {
            ERROR("Do op on net: %s failed", conf->extral_nets[i]->name);
            ret = -1;
            goto out;
        }
        EVENT("Event: {Object: network %s, Target: %s}", conf->extral_nets[i]->name, conf->pod_id);

        if (do_native_append_cni_result(conf->extral_nets[i]->name, conf->extral_nets[i]->interface, cni_result, list) != 0) {
            isulad_set_error_message("parse cni result for net: '%s' failed", conf->extral_nets[i]->name);
            ERROR("parse cni result for net: '%s' failed", conf->extral_nets[i]->name);
            ret = -1;
            goto out;
        }
    }

out:
    free_cni_opt_result(cni_result);
    return ret;
}

static int do_add_container_list(const char *network_name, const char *cont_id)
{
    struct linked_list *it = NULL;
    native_network *network = NULL;

    network = map_search(g_store.name_to_network, (void *)network_name);
    if (network == NULL) {
        ERROR("Cannot found net: %s", network_name);
        return -1;
    }

    it = (struct linked_list *)util_common_calloc_s(sizeof(struct linked_list));
    if (it == NULL) {
        ERROR("Failed to malloc for linked_list");
        return -1;
    }

    linked_list_init(it);
    linked_list_add_elem(it, util_strdup_s(cont_id));

    native_network_lock(EXCLUSIVE, network);

    linked_list_add_tail(&(network->containers_list), it);
    it = NULL;

    native_network_unlock(network);

    return 0;
}

int native_attach_networks(const network_api_conf *conf, network_api_result_list *result)
{
    int ret = 0;
    size_t i = 0;

    if (conf == NULL) {
        ERROR("Invalid argument");
        return -1;
    }
    if (!native_store_lock(SHARED)) {
        return -1;
    }

    if (g_store.network_len == 0) {
        ERROR("Not found native networks");
        goto unlock;
    }

    // first, attach to loopback network
    ret = attach_loopback(conf->pod_id, conf->netns_path);
    if (ret != 0) {
        ERROR("Attach to loop net failed");
        goto unlock;
    }

    ret = do_foreach_network_op(conf, false, attach_network_plane, result);
    if (ret != 0) {
        ERROR("Attach network plane failed");
        goto unlock;
    }

    for (i = 0; i < conf->extral_nets_len; i++) {
        ret = do_add_container_list(conf->extral_nets[i]->name, conf->pod_id);
        if (ret != 0) {
            ERROR("Failed to add container %s to network %s list", conf->pod_id, conf->extral_nets[i]->name);
            goto unlock;
        }
    }

unlock:
    native_store_unlock();
    return ret;
}

static void do_remove_container_list(const char *network_name, const char *cont_id)
{
    bool not_find = true;
    struct linked_list *it = NULL;
    struct linked_list *next = NULL;
    native_network *network = NULL;

    network = map_search(g_store.name_to_network, (void *)network_name);
    if (network == NULL) {
        ERROR("Cannot found net: %s", network_name);
        return;
    }

    native_network_lock(EXCLUSIVE, network);

    linked_list_for_each_safe(it, &(network->containers_list), next) {
        if (strcmp((char *)it->elem, cont_id) == 0) {
            char *id = (char *)it->elem;
            linked_list_del(it);
            free(id);
            free(it);
            it = NULL;
            not_find = false;
            break;
        }
    }

    if (not_find) {
        ERROR("Cannot find container %s in network %s", cont_id, network_name);
    }

    native_network_unlock(network);
}

int native_detach_networks(const network_api_conf *conf, network_api_result_list *result)
{
    int ret = 0;
    size_t i = 0;

    if (conf == NULL) {
        ERROR("Invalid argument");
        return -1;
    }

    if (!native_store_lock(SHARED)) {
        return -1;
    }

    if (g_store.network_len == 0) {
        ERROR("Not found native networks");
        ret = -1;
        goto unlock;
    }

    // first, detach to loopback network
    ret = detach_loopback(conf->pod_id, conf->netns_path);
    if (ret != 0) {
        ERROR("Deatch to loop net failed");
        goto unlock;
    }

    ret = do_foreach_network_op(conf, true, detach_network_plane, result);
    if (ret != 0) {
        goto unlock;
    }

unlock:
    for (i = 0; i < conf->extral_nets_len; i++) {
        do_remove_container_list(conf->extral_nets[i]->name, conf->pod_id);
    }

    native_store_unlock();
    return ret;
}

bool native_network_exist(const char *name)
{
    bool ret = false;

    if (name == NULL) {
        ERROR("Invalid argument");
        return false;
    }

    if (!native_store_lock(SHARED)) {
        return false;
    }

    if (map_search(g_store.name_to_network, (void *)name) != NULL) {
        ret = true;
    }

    native_store_unlock();
    return ret;
}

int native_network_add_container_list(const char *network_name, const char *cont_id)
{
    int ret = 0;

    if (network_name == NULL || cont_id == NULL) {
        ERROR("Invalid argument");
        return -1;
    }
    if (!native_store_lock(SHARED)) {
        return -1;
    }

    ret = do_add_container_list(network_name, cont_id);

    native_store_unlock();
    return ret;
}
