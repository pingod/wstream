#ifndef WSTREAM_INSTANCE_H
#define WSTREAM_INSTANCE_H

#define WSTREAM_VERSION "0.10"   /* 版本单一来源：改这里 → 打 tag vN → gh release */

/* 子命令编排（start/stop/list/stop-all/dashboard/version/help）。
 * argv[0] 是子命令名。返回进程退出码。 */
int instance_main(int argc, char **argv);

#endif