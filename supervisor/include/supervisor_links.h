/**
 * @file supervisor_links.h
 * @brief App和Web的current软链接协调切换接口
 */

#ifndef SUPERVISOR_LINKS_H
#define SUPERVISOR_LINKS_H

/****************************** 版本切换 ******************************/

int supervisor_links_select(const char *app_root, const char *web_root, unsigned int release);

#endif
