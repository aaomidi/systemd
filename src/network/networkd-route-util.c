/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/rtnetlink.h>

#include "alloc-util.h"
#include "networkd-address.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-route-util.h"
#include "networkd-route.h"
#include "parse-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "sysctl-util.h"

#define ROUTES_DEFAULT_MAX_PER_FAMILY 4096U

unsigned routes_max(void) {
        static thread_local unsigned cached = 0;
        _cleanup_free_ char *s4 = NULL, *s6 = NULL;
        unsigned val4 = ROUTES_DEFAULT_MAX_PER_FAMILY, val6 = ROUTES_DEFAULT_MAX_PER_FAMILY;

        if (cached > 0)
                return cached;

        if (sysctl_read_ip_property(AF_INET, NULL, "route/max_size", &s4) >= 0)
                if (safe_atou(s4, &val4) >= 0 && val4 == 2147483647U)
                        /* This is the default "no limit" value in the kernel */
                        val4 = ROUTES_DEFAULT_MAX_PER_FAMILY;

        if (sysctl_read_ip_property(AF_INET6, NULL, "route/max_size", &s6) >= 0)
                (void) safe_atou(s6, &val6);

        cached = MAX(ROUTES_DEFAULT_MAX_PER_FAMILY, val4) +
                 MAX(ROUTES_DEFAULT_MAX_PER_FAMILY, val6);
        return cached;
}

static Route *link_find_default_gateway(Link *link, int family, Route *gw) {
        Route *route;

        assert(link);

        SET_FOREACH(route, link->routes) {
                if (!route_exists(route))
                        continue;
                if (family != AF_UNSPEC && route->family != family)
                        continue;
                if (route->dst_prefixlen != 0)
                        continue;
                if (route->src_prefixlen != 0)
                        continue;
                if (route->table != RT_TABLE_MAIN)
                        continue;
                if (route->type != RTN_UNICAST)
                        continue;
                if (route->scope != RT_SCOPE_UNIVERSE)
                        continue;
                if (!in_addr_is_set(route->gw_family, &route->gw))
                        continue;
                if (gw) {
                        if (route->gw_weight > gw->gw_weight)
                                continue;
                        if (route->priority >= gw->priority)
                                continue;
                }
                gw = route;
        }

        return gw;
}

int manager_find_uplink(Manager *m, int family, Link *exclude, Link **ret) {
        Route *gw = NULL;
        Link *link;

        assert(m);
        assert(IN_SET(family, AF_UNSPEC, AF_INET, AF_INET6));

        /* Looks for a suitable "uplink", via black magic: an interface that is up and where the
         * default route with the highest priority points to. */

        HASHMAP_FOREACH(link, m->links_by_index) {
                if (link == exclude)
                        continue;

                if (link->state != LINK_STATE_CONFIGURED)
                        continue;

                gw = link_find_default_gateway(link, family, gw);
        }

        if (!gw)
                return -ENOENT;

        if (ret) {
                assert(gw->link);
                *ret = gw->link;
        }

        return 0;
}

static bool link_address_is_reachable(Link *link, int family, const union in_addr_union *address) {
        Route *route;
        Address *a;

        assert(link);
        assert(link->manager);
        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(address);

        SET_FOREACH(route, link->routes) {
                if (!route_exists(route))
                        continue;
                if (route->family != family)
                        continue;
                if (!in_addr_is_set(route->family, &route->dst))
                        continue;
                if (in_addr_prefix_covers(family, &route->dst, route->dst_prefixlen, address) > 0)
                        return true;
        }

        if (link->manager->manage_foreign_routes)
                return false;

        /* If we do not manage foreign routes, then there may exist a prefix route we do not know,
         * which was created on configuring an address. Hence, also check the addresses. */
        SET_FOREACH(a, link->addresses) {
                if (!address_is_ready(a))
                        continue;
                if (a->family != family)
                        continue;
                if (FLAGS_SET(a->flags, IFA_F_NOPREFIXROUTE))
                        continue;
                if (in_addr_is_set(a->family, &a->in_addr_peer))
                        continue;
                if (in_addr_prefix_covers(family, &a->in_addr, a->prefixlen, address) > 0)
                        return true;
        }

        return false;
}

bool gateway_is_ready(Link *link, bool onlink, int family, const union in_addr_union *gw) {
        assert(link);
        assert(gw);

        if (onlink)
                return true;

        if (!in_addr_is_set(family, gw))
                return true;

        if (family == AF_INET6 && in6_addr_is_link_local(&gw->in6))
                return true;

        return link_address_is_reachable(link, family, gw);
}

static const char * const route_type_table[__RTN_MAX] = {
        [RTN_UNICAST]     = "unicast",
        [RTN_LOCAL]       = "local",
        [RTN_BROADCAST]   = "broadcast",
        [RTN_ANYCAST]     = "anycast",
        [RTN_MULTICAST]   = "multicast",
        [RTN_BLACKHOLE]   = "blackhole",
        [RTN_UNREACHABLE] = "unreachable",
        [RTN_PROHIBIT]    = "prohibit",
        [RTN_THROW]       = "throw",
        [RTN_NAT]         = "nat",
        [RTN_XRESOLVE]    = "xresolve",
};

assert_cc(__RTN_MAX <= UCHAR_MAX);
DEFINE_STRING_TABLE_LOOKUP(route_type, int);

static const char * const route_scope_table[] = {
        [RT_SCOPE_UNIVERSE] = "global",
        [RT_SCOPE_SITE]     = "site",
        [RT_SCOPE_LINK]     = "link",
        [RT_SCOPE_HOST]     = "host",
        [RT_SCOPE_NOWHERE]  = "nowhere",
};

DEFINE_STRING_TABLE_LOOKUP_WITH_FALLBACK(route_scope, int, UINT8_MAX);

static const char * const route_protocol_table[] = {
        [RTPROT_KERNEL] = "kernel",
        [RTPROT_BOOT]   = "boot",
        [RTPROT_STATIC] = "static",
};

DEFINE_STRING_TABLE_LOOKUP_WITH_FALLBACK(route_protocol, int, UINT8_MAX);

static const char * const route_protocol_full_table[] = {
        [RTPROT_REDIRECT] = "redirect",
        [RTPROT_KERNEL]   = "kernel",
        [RTPROT_BOOT]     = "boot",
        [RTPROT_STATIC]   = "static",
        [RTPROT_GATED]    = "gated",
        [RTPROT_RA]       = "ra",
        [RTPROT_MRT]      = "mrt",
        [RTPROT_ZEBRA]    = "zebra",
        [RTPROT_BIRD]     = "bird",
        [RTPROT_DNROUTED] = "dnrouted",
        [RTPROT_XORP]     = "xorp",
        [RTPROT_NTK]      = "ntk",
        [RTPROT_DHCP]     = "dhcp",
        [RTPROT_MROUTED]  = "mrouted",
        [RTPROT_BABEL]    = "babel",
        [RTPROT_BGP]      = "bgp",
        [RTPROT_ISIS]     = "isis",
        [RTPROT_OSPF]     = "ospf",
        [RTPROT_RIP]      = "rip",
        [RTPROT_EIGRP]    = "eigrp",
};

DEFINE_STRING_TABLE_LOOKUP_WITH_FALLBACK(route_protocol_full, int, UINT8_MAX);

int route_flags_to_string_alloc(uint32_t flags, char **ret) {
        _cleanup_free_ char *str = NULL;
        static const struct {
                uint32_t flag;
                const char *name;
        } map[] = {
                { RTNH_F_DEAD,       "dead"       }, /* Nexthop is dead (used by multipath) */
                { RTNH_F_PERVASIVE,  "pervasive"  }, /* Do recursive gateway lookup */
                { RTNH_F_ONLINK,     "onlink"     }, /* Gateway is forced on link */
                { RTNH_F_OFFLOAD,    "offload"    }, /* Nexthop is offloaded */
                { RTNH_F_LINKDOWN,   "linkdown"   }, /* carrier-down on nexthop */
                { RTNH_F_UNRESOLVED, "unresolved" }, /* The entry is unresolved (ipmr) */
                { RTNH_F_TRAP,       "trap"       }, /* Nexthop is trapping packets */
        };

        assert(ret);

        for (size_t i = 0; i < ELEMENTSOF(map); i++)
                if (flags & map[i].flag &&
                    !strextend_with_separator(&str, ",", map[i].name))
                        return -ENOMEM;

        *ret = TAKE_PTR(str);
        return 0;
}

static const char * const route_table_table[] = {
        [RT_TABLE_DEFAULT] = "default",
        [RT_TABLE_MAIN]    = "main",
        [RT_TABLE_LOCAL]   = "local",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP(route_table, int);

int manager_get_route_table_from_string(const Manager *m, const char *s, uint32_t *ret) {
        uint32_t t;
        int r;

        assert(m);
        assert(s);
        assert(ret);

        r = route_table_from_string(s);
        if (r >= 0) {
                *ret = (uint32_t) r;
                return 0;
        }

        t = PTR_TO_UINT32(hashmap_get(m->route_table_numbers_by_name, s));
        if (t != 0) {
                *ret = t;
                return 0;
        }

        r = safe_atou32(s, &t);
        if (r < 0)
                return r;

        if (t == 0)
                return -ERANGE;

        *ret = t;
        return 0;
}

int manager_get_route_table_to_string(const Manager *m, uint32_t table, char **ret) {
        _cleanup_free_ char *str = NULL;
        const char *s;
        int r;

        assert(m);
        assert(ret);

        if (table == 0)
                return -EINVAL;

        s = route_table_to_string(table);
        if (!s)
                s = hashmap_get(m->route_table_names_by_number, UINT32_TO_PTR(table));

        if (s)
                /* Currently, this is only used in debugging logs. To not confuse any bug
                 * reports, let's include the table number. */
                r = asprintf(&str, "%s(%" PRIu32 ")", s, table);
        else
                r = asprintf(&str, "%" PRIu32, table);
        if (r < 0)
                return -ENOMEM;

        *ret = TAKE_PTR(str);
        return 0;
}

int config_parse_route_table_names(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Manager *m = userdata;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(userdata);

        if (isempty(rvalue)) {
                m->route_table_names_by_number = hashmap_free(m->route_table_names_by_number);
                m->route_table_numbers_by_name = hashmap_free(m->route_table_numbers_by_name);
                return 0;
        }

        for (const char *p = rvalue;;) {
                _cleanup_free_ char *name = NULL;
                uint32_t table;
                char *num;

                r = extract_first_word(&p, &name, NULL, 0);
                if (r == -ENOMEM)
                        return log_oom();
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Invalid RouteTable=, ignoring assignment: %s", rvalue);
                        return 0;
                }
                if (r == 0)
                        return 0;

                num = strchr(name, ':');
                if (!num) {
                        log_syntax(unit, LOG_WARNING, filename, line, 0,
                                   "Invalid route table name and number pair, ignoring assignment: %s", name);
                        continue;
                }

                *num++ = '\0';

                if (STR_IN_SET(name, "default", "main", "local")) {
                        log_syntax(unit, LOG_WARNING, filename, line, 0,
                                   "Route table name %s already predefined. Ignoring assignment: %s:%s", name, name, num);
                        continue;
                }

                r = safe_atou32(num, &table);
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Failed to parse route table number '%s', ignoring assignment: %s:%s", num, name, num);
                        continue;
                }
                if (table == 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, 0,
                                   "Invalid route table number, ignoring assignment: %s:%s", name, num);
                        continue;
                }

                r = hashmap_ensure_put(&m->route_table_numbers_by_name, &string_hash_ops_free, name, UINT32_TO_PTR(table));
                if (r == -ENOMEM)
                        return log_oom();
                if (r == -EEXIST) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Specified route table name and number pair conflicts with others, ignoring assignment: %s:%s", name, num);
                        continue;
                }
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Failed to store route table name and number pair, ignoring assignment: %s:%s", name, num);
                        continue;
                }
                if (r == 0)
                        /* The entry is duplicated. It should not be added to route_table_names_by_number hashmap. */
                        continue;

                r = hashmap_ensure_put(&m->route_table_names_by_number, NULL, UINT32_TO_PTR(table), name);
                if (r < 0) {
                        hashmap_remove(m->route_table_numbers_by_name, name);

                        if (r == -ENOMEM)
                                return log_oom();
                        if (r == -EEXIST)
                                log_syntax(unit, LOG_WARNING, filename, line, r,
                                           "Specified route table name and number pair conflicts with others, ignoring assignment: %s:%s", name, num);
                        else
                                log_syntax(unit, LOG_WARNING, filename, line, r,
                                           "Failed to store route table name and number pair, ignoring assignment: %s:%s", name, num);
                        continue;
                }
                assert(r > 0);

                TAKE_PTR(name);
        }
}
