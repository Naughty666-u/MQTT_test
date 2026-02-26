#include "sdcard_data_handle.h"
#include "ff.h"
#include "stdio.h"
#include "string.h"



/* ----------------------- 手术 2：检查设备是否已存在 --------------------------- */
/**
 * @brief 在CSV文件中查找是否存在同名设备
 * @return 1: 已存在, 0: 不存在, -1: 文件读取错误
 */
int Check_Device_Exist(const char *name)
{
    //在 FatFs 中，FIL 结构体包含了文件管理的各种状态，加上 buffer[128]，这个函数一进来就要在栈上开辟很大的空间。
    static FIL check_fil;   // 【关键修改】改为静态，不占用栈空间
    FRESULT res;
    char line[128];
    char temp_name[24];
    int found = 0;

    // 如果文件不存在，自然就不存在同名设备
    res = f_open(&check_fil, DEVICE_DB_PATH, FA_READ | FA_OPEN_EXISTING);
    if (res == FR_NO_FILE || res == FR_NO_PATH) return 0;
    if (res != FR_OK) return -1;

    // 逐行扫描
    while (f_gets(line, sizeof(line), &check_fil))
    {
        // 手术 1 & 4 结合：使用 %19[^,] 限制读取长度，防止 temp_name 溢出
        // 我们只需要解析出名字进行对比即可
        if (sscanf(line, "%*d,%19[^,]", temp_name) == 1)
        {
            if (strcmp(name, temp_name) == 0)
            {
                found = 1;
                break;
            }
        }
    }

    f_close(&check_fil);
    return found;
}

/* ----------------------- 手术 3 & 2：带表头且防重复的写入 ----------------------- */
FRESULT Save_Appliance_Data(Appliance_Data_t *device)
{
    //在 FatFs 中，FIL 结构体包含了文件管理的各种状态，加上 buffer[128]，这个函数一进来就要在栈上开辟很大的空间。

    static FIL write_fil;   // 【关键修改】改为静态，不占用栈空间
    FRESULT res;
    UINT bw;
    char buffer[128];

    // 1. 手术 2：先检查是否存在
    if (Check_Device_Exist(device->name) == 1)
    {
        printf("[SD] 设备 '%s' 已存在，跳过写入。\r\n", device->name);
        return FR_OK; // 认为是成功操作，只是不需要重复写
    }

    // 2. 打开文件（如果不存在则创建）
    // 注意：这里用 OPEN_ALWAYS 而不是 OPEN_APPEND，为了方便判断是否需要写表头
    res = f_open(&write_fil, DEVICE_DB_PATH, FA_OPEN_ALWAYS | FA_WRITE);
    if (res != FR_OK) return res;

    // 3. 手术 3：CSV 仪式感 - 检查是否为空文件，写入表头
    if (f_size(&write_fil) == 0)
    {
        printf("[SD] 正在创建新数据库并写入表头...\r\n");
        const char *header = "ID,Name,Power,PF,SurgeRatio,Q_Reactive\n";
        f_write(&write_fil, header, (UINT)strlen(header), &bw);
    }

    // 4. 将指针移动到文件末尾进行追加
    f_lseek(&write_fil, f_size(&write_fil));

    // 5. 手术 1：安全地格式化数据
   // 格式化输出：ID,名称,功率,功率因数,激增比,无功功率
    snprintf(buffer, sizeof(buffer), "%d,%s,%.2f,%.2f,%.2f,%.2f\n", 
             device->id, device->name, device->power, 
             device->pf, device->i_surge_ratio, device->q_reactive);

    res = f_write(&write_fil, buffer, (UINT)strlen(buffer), &bw);
    
    f_close(&write_fil);
    printf("[SD] 设备 '%s' 写入成功。\r\n", device->name);
    return res;
}

/* ----------------------- 手术 1：带防火墙的读取打印 --------------------------- */
FRESULT Load_And_Print_All(void)
{
    //在 FatFs 中，FIL 结构体包含了文件管理的各种状态，加上 buffer[128]，这个函数一进来就要在栈上开辟很大的空间。

    static FIL read_fil;   // 【关键修改】改为静态，不占用栈空间
    FRESULT res;
    char line[128];
    Appliance_Data_t temp;

    res = f_open(&read_fil, DEVICE_DB_PATH, FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) return res;

    printf("\r\n--- SD卡内存放的用电器指纹库 ---\r\n");
    printf("ID\t名称\t\t功率(W)\tPF\t激增比\t无功(Var)\r\n");
    printf("------------------------------------------------------------\r\n");

    // 循环读取每一行数据
    while (f_gets(line, sizeof(line), &read_fil)) 
    {
        // 跳过 CSV 表头行：如果这一行包含 "ID" 字符串，则不解析
        if (strstr(line, "ID")) continue;

        // 手术 1：使用 %19[^,] 防火墙，确保 temp.name 不会因为 CSV 损坏而溢出
        // 严格对应五维度解析
        int count = sscanf(line, "%hu,%19[^,],%f,%f,%f,%f", 
                           &temp.id, temp.name, &temp.power, 
                           &temp.pf, &temp.i_surge_ratio, &temp.q_reactive);

        if (count == 6)
        {
           printf("%-4d\t%-10s\t%.2f\t%.2f\t%.2f\t%.2f\r\n", 
                   temp.id, temp.name, temp.power, 
                   temp.pf, temp.i_surge_ratio, temp.q_reactive);
        }
    }

    printf("------------------------------------------------------------\r\n");
    f_close(&read_fil);
    return FR_OK;
}

