#include "sdcard_data_handle.h"
#include "ff.h"
#include "stdio.h"
#include "string.h"




/**
 * @brief 在 CSV 数据库中检查是否存在同名设备 (基于 A-format 6列格式)
 * @param name 要查找的设备名称字符串
 * @return 1: 已存在, 0: 不存在, -1: 硬件/文件读取错误
 */
int Check_Device_Exist(const char *name)
{
    static FIL fil;          // 静态 FIL 结构体，避免在函数执行时从栈空间分配大量内存 (FatFs 建议)
    FRESULT res;
    char line[160];          // 行缓冲区，支持最长 160 字节的 CSV 行读取
    char lib_name[20] = {0}; // 用于临时存放从文件里解析出的名称
    unsigned long id_dummy;  // 临时存放 ID，仅用于满足 sscanf 格式占位
    float p, pf, sr, q;      // 临时存放四维度数据，仅用于验证行格式是否完整

    /* --- 1. 打开文件 --- */
    // 以只读方式打开现有的数据库文件
    res = f_open(&fil, DEVICE_DB_PATH, FA_READ | FA_OPEN_EXISTING);
    
    // 如果文件压根不存在或路径不对，说明肯定没有重复设备，直接返回 0
    if (res == FR_NO_FILE || res == FR_NO_PATH) return 0;
    
    // 如果是其他错误（如 SD 卡掉线），返回错误代码 -1
    if (res != FR_OK) return -1;

    /* --- 2. 逐行扫描 --- */
    while (f_gets(line, sizeof(line), &fil))
    {
        // 过滤：跳过空行、纯回车行，防止 sscanf 解析无效行
        if (line[0] == '\0' || line[0] == '\r' || line[0] == '\n') continue;
        
        // 过滤：跳过 CSV 表头行，防止把 "Name" 字段本身当成一个设备名
        if (strstr(line, "ID,Name,Power") != NULL) continue;

        /* --- 3. 严格解析 --- */
        /**
         * 格式说明: %lu(ID), %19[^,](名称,不含逗号), %f(功率), %f(PF), %f(激增比), %f(无功)
         * 即使我们这里只需要对比名称，也要解析出全部 6 列，
         * 这样可以确保这一行数据是健康、完整的 A-format 格式。
         */
        int cnt = sscanf(line, "%lu,%19[^,],%f,%f,%f,%f",
                         &id_dummy, lib_name, &p, &pf, &sr, &q);
        
        // 只有 6 个字段全部解析成功才进行比对
        if (cnt == 6)
        {
            // 清洗：去掉字符串末尾可能存在的 Windows 换行符 '\r'
            // 否则 "Lamp\r" 与 "Lamp" 的 strcmp 结果会是不匹配
            size_t len = strlen(lib_name);
            if (len && lib_name[len - 1] == '\r') lib_name[len - 1] = '\0';

            // 核心对比：将读取到的名字与目标名字进行字符串比对
            if (strcmp(lib_name, name) == 0)
            {
                f_close(&fil); // 匹配成功，必须先关闭文件再退出
                return 1;      // 返回“已存在”
            }
        }
        // 如果 cnt != 6，说明这一行数据已损坏或格式不对，直接跳过处理下一行
    }

    /* --- 4. 善后 --- */
    f_close(&fil); // 遍历完整个文件都没找到，关闭文件
    return 0;      // 返回“不存在”
}

/**
 * 辅助函数：校验 CSV 表头是否合法
 * 作用：确保文件里的列顺序和你预想的一模一样，防止读错数据
 */
static int header_is_valid_a_format(const char *header_line)
{
    // strstr 查找子字符串。如果当前文件的第一行包含这串标准的列名，说明格式正确
    return (strstr(header_line, "ID,Name,Power,PF,SurgeRatio,Q_Reactive") != NULL);
}

FRESULT Save_Appliance_Data(Appliance_Data_t *device)
{
    static FIL fil;         // 静态变量，防止函数运行过程中栈溢出
    FRESULT res;
    UINT bw;
    char line[160];         // 用于读取现有表头的缓冲区
    char out[160];          // 用于构造写入数据的缓冲区

    /* --- 步骤 1：防重复检查 --- */
    if (Check_Device_Exist(device->name) == 1)
    {
        printf("[SD] 设备 '%s' 已存在，跳过写入。\r\n", device->name);
        return FR_OK;       // 逻辑上是成功的，所以返回 FR_OK
    }

    /* --- 步骤 2：打开文件 --- */
    // FA_OPEN_ALWAYS: 如果文件不存在就创建，存在就打开
    res = f_open(&fil, DEVICE_DB_PATH, FA_OPEN_ALWAYS | FA_WRITE | FA_READ); 
    if (res != FR_OK)
    {
        printf("[SD] open failed: %d\r\n", res);
        return res;
    }

    /* --- 步骤 3：处理文件头部 --- */
    if (f_size(&fil) == 0) 
    {
        // 如果是全新文件（大小为0），直接写入标准表头
        const char *header = "ID,Name,Power,PF,SurgeRatio,Q_Reactive\r\n";
        res = f_write(&fil, header, (UINT)strlen(header), &bw);
        if (res != FR_OK)
        {
            f_close(&fil);
            printf("[SD] write header failed: %d\r\n", res);
            return res;
        }
    }
    else 
    {
        // 如果文件已经有内容，我们要确认它的表头是不是我们认识的“A-format”
        f_lseek(&fil, 0); // 回到文件最开头
        if (f_gets(line, sizeof(line), &fil))
        {
            if (!header_is_valid_a_format(line))
            {
                // 如果发现表头对不上，打印警告。这能帮你发现是不是误用了旧版本的数据库文件
                printf("[SD] WARNING: CSV header mismatch!\r\n");
                printf("[SD] header read: %s\r\n", line);
                printf("[SD] expected : ID,Name,Power,PF,SurgeRatio,Q_Reactive\r\n");
            }
        }
    }

    /* --- 步骤 4：追加数据 --- */
    // 无论前面做了什么校验，写数据前必须把“光标”移到文件末尾
    f_lseek(&fil, f_size(&fil));

    // 使用 snprintf 组合字符串，%.2f 保证了浮点数只保留两位小数，节省空间且美观
    snprintf(out, sizeof(out), "%lu,%s,%.2f,%.2f,%.2f,%.2f\r\n",
             (unsigned long)device->id,
             device->name,
             device->power,
             device->pf,
             device->i_surge_ratio,
             device->q_reactive);

    // 真正执行写入
    res = f_write(&fil, out, (UINT)strlen(out), &bw);
    
    /* --- 步骤 5：关闭并反馈 --- */
    f_close(&fil);

    if (res == FR_OK)
        printf("[SD] 设备 '%s' 写入成功。\r\n", device->name);
    else
        printf("[SD] write failed: %d\r\n", res);

    return res;
}



FRESULT Load_And_Print_All(void)
{
    static FIL fil;         // 静态变量：在 FatFs 中 FIL 结构体很大，放在这里避免函数调用时栈溢出（单片机内存保护）
    FRESULT res;
    char line[160];         // 缓冲区：比 Version 2 略大，能更安全地容纳一整行文本

    // 定义临时变量，用于从 CSV 中“接住”数据
    unsigned long id;
    char name[20];
    float p, pf, sr, q;

    /* --- 1. 打开文件 --- */
    res = f_open(&fil, DEVICE_DB_PATH, FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK)
    {
        printf("[SD] open failed: %d\r\n", res);
        return res;
    }

    // 打印漂亮的表头 UI
    printf("\r\n--- SD卡用电器指纹库 (A-format) ---\r\n");
    printf("ID\tName\t\tP(W)\tPF\tSR\tQ(Var)\r\n");
    printf("------------------------------------------------------------\r\n");

    int ok = 0, bad = 0; // 计数器：这是“老码农”的直觉，一定要知道读成功了多少，失败了多少

    /* --- 2. 逐行循环解析 --- */
    while (f_gets(line, sizeof(line), &fil))
    {
        // 健壮性检查：跳过完全的空行、只有回车的行或结束符
        if (line[0] == '\0' || line[0] == '\r' || line[0] == '\n') continue;
        
        // 精确匹配表头：防止误删名字里带 "ID" 的正常设备数据
        if (strstr(line, "ID,Name,Power") != NULL) continue;

        // 清空名字缓冲区，确保不会被上一行数据污染
        memset(name, 0, sizeof(name));

        /* --- 3. 核心：格式化解析 --- */
        // %19[^,] 是防火墙：最多读19个字符，直到遇到逗号。防止 CSV 损坏导致字符串溢出。
        int cnt = sscanf(line, "%lu,%19[^,],%f,%f,%f,%f", &id, name, &p, &pf, &sr, &q);
        
        if (cnt == 6) // 只有当 6 个字段全部成功解析时，才认为这一行是有效的
        {
            // 处理 Windows 风格的换行符 \r\n，把结尾那个讨厌的 \r 删掉
            size_t len = strlen(name);
            if (len && name[len - 1] == '\r') name[len - 1] = '\0';

            // 打印解析出的精美数据
            printf("%lu\t%-10s\t%.2f\t%.2f\t%.2f\t%.2f\r\n",
                   id, name, p, pf, sr, q);
            ok++;
        }
        else
        {
            // 解析失败时，不仅计数，还把坏掉的那一行打印出来，方便你查错
            bad++;
            printf("[SD] BAD LINE: %s\r\n", line);
        }
    }

    /* --- 4. 善后处理 --- */
    printf("------------------------------------------------------------\r\n");
    printf("[SD] rows_ok=%d rows_bad=%d\r\n", ok, bad); // 输出统计报告

    f_close(&fil); // 养成好习惯：打开必有关
    return FR_OK;
}