# SD卡从 SDHI 切换到 SPI（仍使用 FatFs）改造说明

> 目标：在不改上层 `f_mount/f_open/f_write/f_read` 业务代码的前提下，把底层 SD 卡访问从 `SDHI` 改为 `SPI`。

---

## 1. 结论先说

- `FatFs` 可以继续用，不需要更换文件系统。
- 需要改的是 **底层块设备驱动层**（`diskio.c` + 新增 SPI-SD 驱动）。
- 你当前业务层（如 `Save_Appliance_Data()`、`AI_Commit_Task()`）可以保持不变。

---

## 2. 需要修改的文件清单

1. 新增文件（SPI-SD 驱动）
- `src/sdspi_sdcard/bsp_sdspi_sdcard.h`
- `src/sdspi_sdcard/bsp_sdspi_sdcard.c`

2. 修改文件（FatFs 适配层）
- `src/FatFs/ff15/diskio.c`

3. 配置层（FSP/hal）
- 将 `SDHI` 外设配置替换为 `SPI + CS GPIO (+可选CD脚)`。

---

## 3. 为什么这样改

1. FatFs 是“文件系统层”，与总线无关  
- FatFs 只依赖 `disk_read/disk_write/disk_ioctl` 这几个底层接口。  
- 只要你把这几个接口改成 SPI 版本，上层完全不用动。

2. 你现在受引脚复用限制  
- SDHI 引脚要让给屏幕，因此必须把 SD 卡访问改为 SPI。

3. 最小风险方案  
- 保持上层逻辑不变，只替换底层驱动，联调成本最低。

---

## 4. 具体代码改法（带注释）

### 4.1 新增 `bsp_sdspi_sdcard.h`

```c
#ifndef __BSP_SDSPI_SDCARD_H__
#define __BSP_SDSPI_SDCARD_H__

#include "hal_data.h"
#include "ff.h"
#include <stdbool.h>
#include <stdint.h>

/* SPI-SD 设备信息（用于 disk_ioctl 返回） */
typedef struct
{
    uint32_t sector_count;      /* 总扇区数 */
    uint16_t sector_size_bytes; /* 扇区大小，通常 512 */
    uint32_t erase_sector_count;/* 擦除块大小（扇区数） */
} sdspi_device_t;

extern volatile bool g_sdspi_card_inserted;
extern sdspi_device_t g_sdspi_device;

/* 初始化/反初始化 */
fsp_err_t SD_SPI_Init(void);
fsp_err_t SD_SPI_DeInit(void);

/* 卡初始化（CMD0/CMD8/ACMD41/CMD58 等流程） */
fsp_err_t SD_SPI_MediaInit(void);

/* 扇区读写（LBA） */
fsp_err_t SD_SPI_ReadSectors(uint8_t *buff, uint32_t sector, uint32_t count);
fsp_err_t SD_SPI_WriteSectors(const uint8_t *buff, uint32_t sector, uint32_t count);

/* 状态与同步 */
bool SD_SPI_IsReady(void);
fsp_err_t SD_SPI_Sync(void);

#endif
```

修改原因：
- 把 SPI-SD 能力收敛成 `diskio.c` 需要的最小接口集合，避免 `diskio.c` 与具体 SPI 命令细节耦合。

---

### 4.2 新增 `bsp_sdspi_sdcard.c`（骨架）

```c
#include "bsp_sdspi_sdcard.h"

volatile bool g_sdspi_card_inserted = false;
sdspi_device_t g_sdspi_device = {
    .sector_count = 0,
    .sector_size_bytes = 512,
    .erase_sector_count = 0
};

fsp_err_t SD_SPI_Init(void)
{
    /* 1) 打开 SPI 外设
     * 2) 配置 CS 引脚默认拉高
     * 3) SPI 先低速（100~400kHz）用于卡上电初始化
     */
    return FSP_SUCCESS;
}

fsp_err_t SD_SPI_DeInit(void)
{
    return FSP_SUCCESS;
}

fsp_err_t SD_SPI_MediaInit(void)
{
    /* 典型流程：
     * CMD0 -> CMD8 -> ACMD41 循环 -> CMD58
     * 成功后读取 CSD 解析 sector_count。
     */
    g_sdspi_device.sector_size_bytes = 512;
    /* TODO: 从 CSD 计算真实扇区数 */
    g_sdspi_device.sector_count = 0;
    g_sdspi_device.erase_sector_count = 128;
    return FSP_SUCCESS;
}

fsp_err_t SD_SPI_ReadSectors(uint8_t *buff, uint32_t sector, uint32_t count)
{
    /* TODO: CMD17/CMD18 + data token */
    FSP_PARAMETER_NOT_USED(buff);
    FSP_PARAMETER_NOT_USED(sector);
    FSP_PARAMETER_NOT_USED(count);
    return FSP_SUCCESS;
}

fsp_err_t SD_SPI_WriteSectors(const uint8_t *buff, uint32_t sector, uint32_t count)
{
    /* TODO: CMD24/CMD25 + busy wait */
    FSP_PARAMETER_NOT_USED(buff);
    FSP_PARAMETER_NOT_USED(sector);
    FSP_PARAMETER_NOT_USED(count);
    return FSP_SUCCESS;
}

bool SD_SPI_IsReady(void)
{
    return true;
}

fsp_err_t SD_SPI_Sync(void)
{
    return FSP_SUCCESS;
}
```

修改原因：
- 先搭接口骨架，`diskio.c` 可以先编译通过；后续逐步补全 SPI 协议细节。

---

### 4.3 修改 `diskio.c`（核心）

文件：`src/FatFs/ff15/diskio.c`

#### 改 include

```c
/* Before */
#include "bsp_sdhi_sdcard.h"

/* After */
#include "bsp_sdspi_sdcard.h"
```

原因：
- `diskio.c` 改为调用 SPI-SD 驱动，而不是 SDHI 驱动。

#### 改 `disk_initialize()` 中 `DEV_SDCARD` 分支

```c
case DEV_SDCARD:
{
    if (FSP_SUCCESS != SD_SPI_Init())
    {
        return STA_NOINIT;
    }

    if (FSP_SUCCESS != SD_SPI_MediaInit())
    {
        return STA_NOINIT;
    }

    return RES_OK;
}
```

原因：
- 用 SPI 初始化 + 卡初始化替代 `R_SDHI_Open/R_SDHI_MediaInit`。

#### 改 `disk_read()` 中 `DEV_SDCARD` 分支

```c
case DEV_SDCARD:
{
    if (FSP_SUCCESS != SD_SPI_ReadSectors(buff, (uint32_t)sector, (uint32_t)count))
    {
        return RES_ERROR;
    }
    return RES_OK;
}
```

原因：
- 将 FatFs 的块读取转发到 SPI-SD 扇区读接口。

#### 改 `disk_write()` 中 `DEV_SDCARD` 分支

```c
case DEV_SDCARD:
{
    if (FSP_SUCCESS != SD_SPI_WriteSectors((const uint8_t *)buff, (uint32_t)sector, (uint32_t)count))
    {
        return RES_ERROR;
    }
    return RES_OK;
}
```

原因：
- 将 FatFs 的块写入转发到 SPI-SD 扇区写接口。

#### 改 `disk_ioctl()` 中 `DEV_SDCARD` 分支

```c
case DEV_SDCARD:
{
    switch (cmd)
    {
        case CTRL_SYNC:
            return (FSP_SUCCESS == SD_SPI_Sync()) ? RES_OK : RES_ERROR;

        case GET_SECTOR_SIZE:
            *(WORD *)buff = g_sdspi_device.sector_size_bytes;
            return RES_OK;

        case GET_BLOCK_SIZE:
            *(DWORD *)buff = g_sdspi_device.erase_sector_count;
            return RES_OK;

        case GET_SECTOR_COUNT:
            *(DWORD *)buff = g_sdspi_device.sector_count;
            return RES_OK;

        default:
            return RES_PARERR;
    }
}
```

原因：
- FatFs 需要这几个控制信息才能正确挂载、读写和格式化。

---

## 5. 你现有代码中“不需要改”的部分

- `src/sdcard_data_handle/sdcard_data_handle.c`
- `src/Appliance identification algorithm/appliance_identification.c`
- 所有 `f_mount/f_open/f_write/f_read/f_close` 调用点

原因：
- 这些都在 FatFs 之上，底层总线切换对它们透明。

---

## 6. 调试与验收建议

1. 基础验收
- 上电后 `f_mount("1:",...) == FR_OK`
- `Load_And_Print_All()` 能读到 CSV
- `Save_Appliance_Data()` 可成功追加

2. 稳定性验收
- 连续写入 100 次设备记录，检查是否有 `FR_DISK_ERR/FR_INT_ERR`
- 插拔卡后重挂载，确认可恢复

3. 性能与时序
- SPI 初始化低速，完成后切高速
- 写入后执行 `CTRL_SYNC`，避免掉电丢数据

---

## 7. 常见坑位（务必注意）

1. SPI 时钟太快导致初始化失败  
- 先 100~400kHz 初始化，再升频。

2. CS 时序不规范  
- 命令与数据 token 前后要严格拉低/拉高。

3. `GET_SECTOR_COUNT` 填错  
- 会导致容量识别错误，严重时文件系统损坏。

4. 忽略 busy 等待  
- 写后卡忙期间若直接发新命令，会出现随机写失败。

---

## 8. 建议落地顺序

1. 先搭 `bsp_sdspi_sdcard` 骨架 + `diskio.c` 接口替换（保证能编译）  
2. 打通 `disk_initialize + disk_read`  
3. 打通 `disk_write + CTRL_SYNC`  
4. 最后做性能优化和异常恢复（插拔、超时重试）


---

## 9. 修改前后代码对比（可直接照抄）

### 9.1 `diskio.c` 头文件对比

```c
/* Before (SDHI) */
#include "bsp_sdhi_sdcard.h"
```

```c
/* After (SPI-SD) */
#include "bsp_sdspi_sdcard.h"
```

### 9.2 `disk_initialize()` 的 `DEV_SDCARD` 分支

```c
/* Before (SDHI) */
case DEV_SDCARD :
    SDCard_Init();
    R_SDHI_StatusGet(&g_sdmmc0_ctrl, &status);
    if (!status.card_inserted)
    {
        while (!g_card_inserted)
        {
            printf("请插入SD卡\r\n");
            R_BSP_SoftwareDelay(1000, BSP_DELAY_UNITS_MILLISECONDS);
        }
        printf("\r\n检测到SD卡已插入\r\n");
    }
    R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    R_SDHI_MediaInit(&g_sdmmc0_ctrl, &my_sdmmc_device);
    return RES_OK;
```

```c
/* After (SPI-SD) */
case DEV_SDCARD:
{
    if (FSP_SUCCESS != SD_SPI_Init())
    {
        return STA_NOINIT;
    }

    if (FSP_SUCCESS != SD_SPI_MediaInit())
    {
        return STA_NOINIT;
    }

    return RES_OK;
}
```

### 9.3 `disk_read()` 的 `DEV_SDCARD` 分支

```c
/* Before (SDHI) */
case DEV_SDCARD :
    R_SDHI_Read(&g_sdmmc0_ctrl, buff, sector, count);
    while (g_transfer_complete == 0);
    g_transfer_complete = 0;
    return RES_OK;
```

```c
/* After (SPI-SD) */
case DEV_SDCARD:
{
    if (FSP_SUCCESS != SD_SPI_ReadSectors(buff, (uint32_t)sector, (uint32_t)count))
    {
        return RES_ERROR;
    }
    return RES_OK;
}
```

### 9.4 `disk_write()` 的 `DEV_SDCARD` 分支

```c
/* Before (SDHI) */
case DEV_SDCARD :
    R_SDHI_Write(&g_sdmmc0_ctrl, buff, sector, count);
    while (g_transfer_complete == 0);
    g_transfer_complete = 0;
    return RES_OK;
```

```c
/* After (SPI-SD) */
case DEV_SDCARD:
{
    if (FSP_SUCCESS != SD_SPI_WriteSectors((const uint8_t *)buff, (uint32_t)sector, (uint32_t)count))
    {
        return RES_ERROR;
    }
    return RES_OK;
}
```

### 9.5 `disk_ioctl()` 的 `DEV_SDCARD` 分支

```c
/* Before (SDHI) */
case DEV_SDCARD :
    switch (cmd)
    {
        case GET_SECTOR_SIZE:
            *(WORD *)buff = (WORD)my_sdmmc_device.sector_size_bytes;
            break;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = my_sdmmc_device.erase_sector_count;
            break;
        case GET_SECTOR_COUNT:
            *(DWORD *)buff = my_sdmmc_device.sector_count;
            break;
    }
    return RES_OK;
```

```c
/* After (SPI-SD) */
case DEV_SDCARD:
{
    switch (cmd)
    {
        case CTRL_SYNC:
            return (FSP_SUCCESS == SD_SPI_Sync()) ? RES_OK : RES_ERROR;

        case GET_SECTOR_SIZE:
            *(WORD *)buff = g_sdspi_device.sector_size_bytes;
            return RES_OK;

        case GET_BLOCK_SIZE:
            *(DWORD *)buff = g_sdspi_device.erase_sector_count;
            return RES_OK;

        case GET_SECTOR_COUNT:
            *(DWORD *)buff = g_sdspi_device.sector_count;
            return RES_OK;

        default:
            return RES_PARERR;
    }
}
```

### 9.6 驱动接口签名对比

```c
/* Before (SDHI driver) */
void SDCard_Init(void);
void SDCard_DeInit(void);
extern sdmmc_device_t my_sdmmc_device;
```

```c
/* After (SPI-SD driver) */
fsp_err_t SD_SPI_Init(void);
fsp_err_t SD_SPI_DeInit(void);
fsp_err_t SD_SPI_MediaInit(void);
fsp_err_t SD_SPI_ReadSectors(uint8_t *buff, uint32_t sector, uint32_t count);
fsp_err_t SD_SPI_WriteSectors(const uint8_t *buff, uint32_t sector, uint32_t count);
bool SD_SPI_IsReady(void);
fsp_err_t SD_SPI_Sync(void);
extern sdspi_device_t g_sdspi_device;
```
