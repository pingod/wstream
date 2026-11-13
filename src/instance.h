#ifndef WSTREAM_INSTANCE_H
#define WSTREAM_INSTANCE_H

#define WSTREAM_VERSION "1.6.0"

/* 子命令编排（start/stop/list/stop-all/dashboard/version/help）。
 * argv[0] 是子命令名。返回进程退出码。 */
int instance_main(int argc, char **argv);

#endif