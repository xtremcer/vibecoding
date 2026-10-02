#ifndef LFS_RP2040_H
#define LFS_RP2040_H

#include "lfs.h"
#include <stdint.h>

//--------------------------------------------------------------------+
// RP2040 上的 LittleFS 块设备：把 LFS 分区映射到片内 QSPI 闪存末尾。
//
// 为何放在末尾：UF2 烧录只写固件镜像（当前 .bin ≈ 60KB，远小于此偏移），
// 不会触碰 LFS 分区；固件增长也几乎不可能逼近 1.75MB。
// 注意：若用户走「整片擦除再烧录」流程，会连 LFS 一起抹掉——此时开机
// 会自动 lfs_format 并回退到编译期默认（已存配置丢失）。这是预期行为。
//--------------------------------------------------------------------+

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#define LFS_PART_SIZE   (256 * 1024)                       // 分区大小 256KB
#define LFS_PART_OFFSET (PICO_FLASH_SIZE_BYTES - LFS_PART_SIZE)
#define LFS_BLOCK_SIZE  4096                                // RP2040 擦除扇区 = 4KB
#define LFS_BLOCK_COUNT (LFS_PART_SIZE / LFS_BLOCK_SIZE)    // = 64

// 返回填好块设备回调与参数的全局 lfs_config（供 lfs_mount/lfs_format 使用）
const struct lfs_config* lfs_rp2040_config(void);

#endif // LFS_RP2040_H
