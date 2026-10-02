#include "flash_store.h"

#include "lfs.h"
#include "lfs_rp2040.h"

#include "hardware/flash.h"   // flash_range_erase / flash_range_program / XIP_BASE
#include <stdio.h>
#include <string.h>

static lfs_t lfs;
static bool  mounted = false;
static int   last_err = 0;      // 最近一次 LFS 操作错误码（诊断用）

#define CFG_FILE "config.json"
#define CFG_TMP  "config.tmp"

bool flash_store_init(void)
{
    const struct lfs_config* cfg = lfs_rp2040_config();
    int err = lfs_mount(&lfs, cfg);
    if (err) {
        // 未格式化或文件系统损坏 → 重新格式化再挂载
        err = lfs_format(&lfs, cfg);
        if (err) return false;
        err = lfs_mount(&lfs, cfg);
        if (err) return false;
    }
    mounted = true;
    return true;
}

bool flash_store_mounted(void)
{
    return mounted;
}

int flash_store_last_err(void)
{
    return last_err;
}

int flash_store_read_config(char* out_buf, int max_len)
{
    if (!mounted) return 0;

    lfs_file_t file;
    int err = lfs_file_open(&lfs, &file, CFG_FILE, LFS_O_RDONLY);
    if (err) { last_err = err; return 0; }  // 文件不存在 / 打开失败（记下原因）

    int total = 0;
    char chunk[128];
    while (total < max_len - 1) {
        lfs_ssize_t n = lfs_file_read(&lfs, &file, chunk, sizeof(chunk));
        if (n <= 0) break;                  // 0 = EOF；<0 = 错误
        if (total + (int)n > max_len - 1)
            n = max_len - 1 - total;
        memcpy(out_buf + total, chunk, (size_t)n);
        total += (int)n;
    }
    lfs_file_close(&lfs, &file);
    out_buf[total] = 0;
    return total;
}

bool flash_store_write_config(const char* json, int len)
{
    last_err = 0;
    if (!mounted) { last_err = -1; return false; }

    lfs_file_t file;
    // 1) 先写临时文件（整段一次写；config 很小，无需分片）
    int err = lfs_file_open(&lfs, &file, CFG_TMP,
                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err) { last_err = err; return false; }

    lfs_ssize_t w = lfs_file_write(&lfs, &file, json, (lfs_size_t)len);
    // close 才真正把数据刷进闪存 —— 原实现忽略了它的返回值，落盘失败也被当成成功
    int cerr = lfs_file_close(&lfs, &file);
    if (w != (lfs_ssize_t)len) {            // 没写全 → 丢弃 tmp
        last_err = (int)w;
        lfs_remove(&lfs, CFG_TMP);
        return false;
    }
    if (cerr) {                             // 落盘失败 → 丢弃 tmp
        last_err = cerr;
        lfs_remove(&lfs, CFG_TMP);
        return false;
    }

    // 2) 原子提交：rename 覆盖正式文件（LFS rename 内部原子，掉电不会半截）
    err = lfs_rename(&lfs, CFG_TMP, CFG_FILE);
    if (err) {
        last_err = err;
        lfs_remove(&lfs, CFG_TMP);
        return false;
    }
    return true;
}

//--------------------------------------------------------------------+
// A3 诊断：绕过 LFS，直接对分区做「擦除 → 编程 → 读回」原始自检。
//   用来把故障二分：
//     ERASE=0 或 PROG=0 → RP2040 闪存本身没写进去（硬件 / SDK 层问题）
//     ERASE=1 且 PROG=1 → 闪存没问题，问题在 LFS 或上层逻辑
//   注意：会毁掉分区内容（含 superblock），结束后强制重新格式化让设备回到可用态
//   （已存配置会丢失，属预期）。
//--------------------------------------------------------------------+
int flash_store_rwtest(char* out, int maxlen)
{
    if (!out || maxlen < 48) return 0;

    uint32_t addr = LFS_PART_OFFSET;          // 分区第 0 块
    uint8_t  pattern[256];
    uint8_t  rb[256];
    for (int i = 0; i < 256; i++) pattern[i] = 0xA5;

    // 1) 擦除一个扇区
    flash_range_erase(addr, LFS_BLOCK_SIZE);

    // 2) 读回应为全 0xFF
    memcpy(rb, (const void*)(XIP_BASE + addr), sizeof(rb));
    bool erased = true;
    for (int i = 0; i < 256; i++) if (rb[i] != 0xFF) { erased = false; break; }

    // 3) 写入已知图案
    flash_range_program(addr, pattern, sizeof(pattern));

    // 4) 读回校验
    memcpy(rb, (const void*)(XIP_BASE + addr), sizeof(rb));
    bool progd = true;
    for (int i = 0; i < 256; i++) if (rb[i] != 0xA5) { progd = false; break; }

    int n = snprintf(out, (size_t)maxlen,
                     "RWTEST ERASE=%d PROG=%d FIRST=%02X",
                     erased ? 1 : 0, progd ? 1 : 0, rb[0]);

    // 自检毁掉了分区 → 强制重新格式化，恢复可用状态
    mounted = false;
    const struct lfs_config* cfg = lfs_rp2040_config();
    int err = lfs_format(&lfs, cfg);
    if (err) { last_err = err; return n; }
    err = lfs_mount(&lfs, cfg);
    if (err) { last_err = err; return n; }
    mounted = true;
    return n;
}
