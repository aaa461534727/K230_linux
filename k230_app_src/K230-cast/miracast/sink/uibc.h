#ifndef UIBC_H
#define UIBC_H

/* UIBC (反控) 对外接口 —— 实现在 uibc.c */

/* 本端 UIBC 监听端口 (写进 wfd_uibc_capability 的 port=) */
int uibc_port(void);
/* 拼 wfd_uibc_capability 的值 (input_category_list=…;port=…) */
const char *uibc_capability(void);
/* 是否已建立 UIBC 通道 (真有手机连上) */
int uibc_active(void);
/* 起反控线程: peer_port>0 先尝试连 source, 否则监听 listen_port 等 source 连入 */
int uibc_start(const char *peer_ip, int peer_port, int listen_port);
/* 停反控线程 (会话结束/退出时调) */
void uibc_stop(void);

#endif