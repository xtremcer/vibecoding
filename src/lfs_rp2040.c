#include "lfs_rp2040.h"

#include "hardware/flash.h"   // flash_range_erase / flash_range_program / XIP_BASE
#include <string.h>

//--------------------------------------------------------------------+
// 块设备回调：直接驱动 RP2040 片内闪存。
//   read  ：闪存经 XIP 映射到 0x10000000，未擦除/编程时直接 memcpy 读取，
//           比 flash_range_read 更快，且读取与写入从不并发（LFS 串行）。
//   prog  ：flash_range_program，要求地址 256 对齐、长度 256 倍数（LFS 保证）。
//   erase ：按块擦除一个扇区（4KB）。
//   sync  ：无需操作（无写缓存）。
//--------------------------------------------------------------------+

static int rp2040_read(const struct lfs_config* c, lfs_block_t block,
                       lfs_off_t off, void* buffer, lfs_size_t size)
{
    (void)c;
    uint32_t addr = LFS_PART_OFFSET + (uint32_t)block * LFS_BLOCK_SIZE + (uint32_t)off;
    memcpy(buffer, (const void*)(XIP_BASE + addr), size);
    return LFS_ERR_OK;
}

static int rp2040_prog(const struct lfs_config* c, lfs_block_t block,
                       lfs_off_t off, const void* buffer, lfs_size_t size)
{
    (void)c;
    uint32_t addr = LFS_PART_OFFSET + (uint32_t)block * LFS_BLOCK_SIZE + (uint32_t)off;
    flash_range_program(addr, (const uint8_t*)buffer, size);
    return LFS_ERR_OK;
}

static int rp2040_erase(const struct lfs_config* c, lfs_block_t block)
{
    (void)c;
    uint32_t addr = LFS_PART_OFFSET + (uint32_t)block * LFS_BLOCK_SIZE;
    flash_range_erase(addr, LFS_BLOCK_SIZE);
    return LFS_ERR_OK;
}

static int rp2040_sync(const struct lfs_config* c)
{
    (void)c;
    return LFS_ERR_OK;
}

//--------------------------------------------------------------------+
// LittleFS 把 CRC-32 委派给平台实现（lfs_util.h 在未定义 LFS_CRC 时只声明、
// 不定义 lfs_crc）。这里提供与 LittleFS 参考实现完全一致的 CRC-32
// （多项式 0x04C11DB7 的反射形式 0xEDB88320，逐位处理），保证新写与已落盘
// 数据的 CRC 互相校验通过。
//--------------------------------------------------------------------+
uint32_t lfs_crc(uint32_t crc, const void* buffer, size_t size)
{
    const uint8_t* data = (const uint8_t*)buffer;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return crc;
}

// 静态缓冲，避免 LFS 在堆上分配缓存（行为更可预测；RP2040 堆不宽裕）
static uint8_t read_buf[256];
static uint8_t prog_buf[256];
static uint8_t lookahead[16];

static struct lfs_config cfg = {
    .read  = rp2040_read,
    .prog  = rp2040_prog,
    .erase = rp2040_erase,
    .sync  = rp2040_sync,

    .read_size       = 16,                  // 读粒度（≤ prog_size，整除 block_size）
    .prog_size       = 256,                 // 编程粒度 = 闪存页（256B）
    .block_size      = LFS_BLOCK_SIZE,      // 4096
    .block_count     = LFS_BLOCK_COUNT,     // 64
    .block_cycles    = 256,                 // 磨损均衡阈值（配置极少写，可不激进）
    .cache_size      = 256,                 // 必须是 prog_size 倍数
    .lookahead_size  = 16,                  // 8 的倍数、≤ block_size/8

    .name_max        = 64,                  // 文件名上限（够用）
    .file_max        = 64,
    .attr_max        = 0,

    .read_buffer     = read_buf,            // 使用静态缓冲而非 malloc
    .prog_buffer     = prog_buf,
    .lookahead_buffer = lookahead,
};

const struct lfs_config* lfs_rp2040_config(void)
{
    return &cfg;
}
