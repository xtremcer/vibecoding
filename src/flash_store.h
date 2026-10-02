#ifndef FLASH_STORE_H
#define FLASH_STORE_H

#include <stdbool.h>
#include <stdint.h>

//--------------------------------------------------------------------+
// flash_store：在 LittleFS 上存一份「当前配置」的 JSON 快照，掉电不丢。
//   - 文件：config.json（正式）
//   - 写入走「写 config.tmp → lfs_rename 覆盖 config.json」的原子提交，
//     避免写到一半掉电留下半截文件（LFS rename 内部原子）。
//   - 开机若 LFS 未格式化/损坏 → lfs_format 后挂载（配置回默认）。
//--------------------------------------------------------------------+

// 开机挂载（必要时格式化）。返回是否挂载成功。
bool flash_store_init(void);

// 是否已挂载（诊断用）
bool flash_store_mounted(void);

// 读取已保存配置到 out_buf（结尾补 \0）。返回读取字节数（不含 \0）；
// 0 表示无文件 / 读取失败。
int flash_store_read_config(char* out_buf, int max_len);

// 原子写入配置 JSON（会先写 tmp 再 rename）。成功返回 true。
bool flash_store_write_config(const char* json, int len);

// 最近一次 LFS 操作的错误码（0=无错误；负数为 LFS_ERR_*）。诊断用。
// 之前 CONFIG SET 忽略了 cfg_persist 的返回值，写失败也回 OK，故障被完全掩盖；
// 现在把错误码暴露出来，配合 FLASH? 的 ERR= 字段定位。
int flash_store_last_err(void);

// 原始闪存「擦除 → 编程 → 读回」自检（A3 诊断用，绕过 LFS 直接打闪存）。
//   用来把故障二分：ERASE/PROG 为 0 → RP2040 闪存本身没写进去；
//   都为 1 → 闪存没问题，问题在 LFS 或上层。
//   注意：会毁掉分区内容（含 superblock），结束后强制重新格式化让设备回到可用态
//   （已存配置会丢，属预期）。结果写入 out，返回写入长度。
int flash_store_rwtest(char* out, int maxlen);

#endif // FLASH_STORE_H
