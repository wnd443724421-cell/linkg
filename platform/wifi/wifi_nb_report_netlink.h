/**
 * @file wifi_nb_report_netlink.h
 * @brief LinkG HI1105窄带状态Netlink内部接口
 */

#ifndef WIFI_NB_REPORT_NETLINK_H
#define WIFI_NB_REPORT_NETLINK_H

#include "wifi_nb_report.h"

/****************************** Netlink接口 ******************************/

int wifi_nb_report_netlink_open(void);
int wifi_nb_report_netlink_query(int netlink_fd, wifi_nb_report_status_t *status);

#endif
