/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <net/ethernet.h>
#include <linux/nl80211.h>

#include "ether-addr-util.h"
#include "netlink-util.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-wifi.h"
#include "string-util.h"
#include "wifi-util.h"

static int link_get_wlan_interface(Link *link) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        int r;

        assert(link);

        r = sd_genl_message_new(link->manager->genl, NL80211_GENL_NAME, NL80211_CMD_GET_INTERFACE, &req);
        if (r < 0)
                return log_link_debug_errno(link, r, "Failed to create generic netlink message: %m");

        r = sd_netlink_message_append_u32(req, NL80211_ATTR_IFINDEX, link->ifindex);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append NL80211_ATTR_IFINDEX attribute: %m");

        r = sd_netlink_call(link->manager->genl, req, 0, &reply);
        if (r < 0)
                return log_link_debug_errno(link, r, "Failed to request information about wlan interface: %m");
        if (!reply) {
                log_link_debug(link, "No reply received to request for information about wifi interface, ignoring.");
                return 0;
        }

        return manager_genl_process_nl80211_config(link->manager->genl, reply, link->manager);
}

int manager_genl_process_nl80211_config(sd_netlink *genl, sd_netlink_message *message, Manager *manager) {
        _cleanup_free_ char *ssid = NULL;
        uint32_t ifindex, wlan_iftype;
        const char *family, *ifname;
        uint8_t cmd;
        size_t len;
        Link *link;
        int r;

        assert(genl);
        assert(message);
        assert(manager);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_message_warning_errno(message, r, "nl80211: received error message, ignoring");

                return 0;
        }

        r = sd_genl_message_get_family_name(genl, message, &family);
        if (r < 0) {
                log_debug_errno(r, "nl80211: failed to determine genl family, ignoring: %m");
                return 0;
        }
        if (!streq(family, NL80211_GENL_NAME)) {
                log_debug("nl80211: received message of unexpected genl family '%s', ignoring.", family);
                return 0;
        }

        r = sd_genl_message_get_command(genl, message, &cmd);
        if (r < 0) {
                log_debug_errno(r, "nl80211: failed to determine genl message command, ignoring: %m");
                return 0;
        }
        if (!IN_SET(cmd, NL80211_CMD_SET_INTERFACE, NL80211_CMD_NEW_INTERFACE, NL80211_CMD_DEL_INTERFACE)) {
                log_debug("nl80211: ignoring nl80211 %s(%u) message.",
                          strna(nl80211_cmd_to_string(cmd)), cmd);
                return 0;
        }

        r = sd_netlink_message_read_u32(message, NL80211_ATTR_IFINDEX, &ifindex);
        if (r < 0) {
                log_debug_errno(r, "nl80211: received %s(%u) message without valid ifindex, ignoring: %m",
                                strna(nl80211_cmd_to_string(cmd)), cmd);
                return 0;
        }

        r = link_get_by_index(manager, ifindex, &link);
        if (r < 0) {
                log_debug_errno(r, "nl80211: received %s(%u) message for link '%"PRIu32"' we don't know about, ignoring.",
                                strna(nl80211_cmd_to_string(cmd)), cmd, ifindex);
                return 0;
        }

        r = sd_netlink_message_read_string(message, NL80211_ATTR_IFNAME, &ifname);
        if (r < 0) {
                log_link_debug_errno(link, r, "nl80211: received %s(%u) message without valid interface name, ignoring: %m",
                                     strna(nl80211_cmd_to_string(cmd)), cmd);
                return 0;
        }

        if (!streq(ifname, link->ifname)) {
                log_link_debug_errno(link, r, "nl80211: received %s(%u) message with invalid interface name '%s', ignoring: %m",
                                     strna(nl80211_cmd_to_string(cmd)), cmd, ifname);
                return 0;
        }

        r = sd_netlink_message_read_u32(message, NL80211_ATTR_IFTYPE, &wlan_iftype);
        if (r < 0) {
                log_link_debug_errno(link, r, "nl80211: received %s(%u) message without valid wlan interface type, ignoring: %m",
                                     strna(nl80211_cmd_to_string(cmd)), cmd);
                return 0;
        }

        r = sd_netlink_message_read_data_suffix0(message, NL80211_ATTR_SSID, &len, (void**) &ssid);
        if (r < 0 && r != -ENODATA) {
                log_link_debug_errno(link, r, "nl80211: received %s(%u) message without valid SSID, ignoring: %m",
                                     strna(nl80211_cmd_to_string(cmd)), cmd);
                return 0;
        }
        if (r >= 0) {
                if (len == 0) {
                        log_link_debug(link, "nl80211: received SSID has zero length, ignoring the received SSID: %m");
                        ssid = mfree(ssid);
                } else if (strlen_ptr(ssid) != len) {
                        log_link_debug(link, "nl80211: received SSID contains NUL character(s), ignoring the received SSID.");
                        ssid = mfree(ssid);
                }
        }

        log_link_debug(link, "nl80211: received %s(%u) message: iftype=%s, ssid=%s",
                       strna(nl80211_cmd_to_string(cmd)), cmd,
                       strna(nl80211_iftype_to_string(wlan_iftype)), ssid);

        switch(cmd) {
        case NL80211_CMD_SET_INTERFACE:
        case NL80211_CMD_NEW_INTERFACE:
                link->wlan_iftype = wlan_iftype;
                free_and_replace(link->ssid, ssid);
                break;

        case NL80211_CMD_DEL_INTERFACE:
                link->wlan_iftype = NL80211_IFTYPE_UNSPECIFIED;
                link->ssid = mfree(link->ssid);
                break;

        default:
                assert_not_reached();
        }

        return 0;
}

int manager_genl_process_nl80211_mlme(sd_netlink *genl, sd_netlink_message *message, Manager *manager) {
        const char *family;
        uint32_t ifindex;
        uint8_t cmd;
        Link *link;
        int r;

        assert(genl);
        assert(message);
        assert(manager);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_message_warning_errno(message, r, "nl80211: received error message, ignoring");

                return 0;
        }

        r = sd_genl_message_get_family_name(genl, message, &family);
        if (r < 0) {
                log_debug_errno(r, "nl80211: failed to determine genl family, ignoring: %m");
                return 0;
        }
        if (!streq(family, NL80211_GENL_NAME)) {
                log_debug("nl80211: Received message of unexpected genl family '%s', ignoring.", family);
                return 0;
        }

        r = sd_genl_message_get_command(genl, message, &cmd);
        if (r < 0) {
                log_debug_errno(r, "nl80211: failed to determine genl message command, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_u32(message, NL80211_ATTR_IFINDEX, &ifindex);
        if (r < 0) {
                log_debug_errno(r, "nl80211: received %s(%u) message without valid ifindex, ignoring: %m",
                                strna(nl80211_cmd_to_string(cmd)), cmd);
                return 0;
        }

        r = link_get_by_index(manager, ifindex, &link);
        if (r < 0) {
                log_debug_errno(r, "nl80211: received %s(%u) message for link '%"PRIu32"' we don't know about, ignoring.",
                                strna(nl80211_cmd_to_string(cmd)), cmd, ifindex);
                return 0;
        }

        switch(cmd) {
        case NL80211_CMD_NEW_STATION:
        case NL80211_CMD_DEL_STATION: {
                struct ether_addr bssid;

                r = sd_netlink_message_read_ether_addr(message, NL80211_ATTR_MAC, &bssid);
                if (r < 0) {
                        log_link_debug_errno(link, r, "nl80211: received %s(%u) message without valid BSSID, ignoring: %m",
                                             strna(nl80211_cmd_to_string(cmd)), cmd);
                        return 0;
                }

                log_link_debug(link, "nl80211: received %s(%u) message: bssid=%s",
                               strna(nl80211_cmd_to_string(cmd)), cmd, ETHER_ADDR_TO_STR(&bssid));

                if (cmd == NL80211_CMD_DEL_STATION) {
                        link->bssid = ETHER_ADDR_NULL;
                        return 0;
                }

                link->bssid = bssid;

                if (manager->enumerating &&
                    link->wlan_iftype == NL80211_IFTYPE_STATION && link->ssid)
                        log_link_info(link, "Connected WiFi access point: %s (%s)",
                                      link->ssid, ETHER_ADDR_TO_STR(&link->bssid));
                break;
        }
        case NL80211_CMD_CONNECT: {
                struct ether_addr bssid;
                uint16_t status_code;

                r = sd_netlink_message_read_ether_addr(message, NL80211_ATTR_MAC, &bssid);
                if (r < 0 && r != -ENODATA) {
                        log_link_debug_errno(link, r, "nl80211: received %s(%u) message without valid BSSID, ignoring: %m",
                                             strna(nl80211_cmd_to_string(cmd)), cmd);
                        return 0;
                }

                r = sd_netlink_message_read_u16(message, NL80211_ATTR_STATUS_CODE, &status_code);
                if (r < 0) {
                        log_link_debug_errno(link, r, "nl80211: received %s(%u) message without valid status code, ignoring: %m",
                                             strna(nl80211_cmd_to_string(cmd)), cmd);
                        return 0;
                }

                log_link_debug(link, "nl80211: received %s(%u) message: status=%u, bssid=%s",
                               strna(nl80211_cmd_to_string(cmd)), cmd, status_code, ETHER_ADDR_TO_STR(&bssid));

                if (status_code != 0)
                        return 0;

                link->bssid = bssid;

                if (!manager->enumerating) {
                        r = link_get_wlan_interface(link);
                        if (r < 0) {
                                log_link_warning_errno(link, r, "Failed to update wireless LAN interface: %m");
                                link_enter_failed(link);
                                return 0;
                        }
                }

                if (link->wlan_iftype == NL80211_IFTYPE_STATION && link->ssid)
                        log_link_info(link, "Connected WiFi access point: %s (%s)",
                                      link->ssid, ETHER_ADDR_TO_STR(&link->bssid));
                break;
        }
        case NL80211_CMD_DISCONNECT:
                log_link_debug(link, "nl80211: received %s(%u) message.",
                               strna(nl80211_cmd_to_string(cmd)), cmd);

                link->bssid = ETHER_ADDR_NULL;
                free_and_replace(link->previous_ssid, link->ssid);
                break;

        default:
                log_link_debug(link, "nl80211: received %s(%u) message.",
                               strna(nl80211_cmd_to_string(cmd)), cmd);
        }

        return 0;
}
