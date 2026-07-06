#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define PROC_NODE "/proc/e84523d7b60d5d341a7c4d1861773ecd"
#define MY_TASK_COMM_LEN 16

#pragma pack(push, 1)
struct ioctl_request {
    char     cmd;
    uint64_t param1;
    uint64_t param2;
    uint64_t param3;
    uint64_t buf_size;
};

struct init_device_info {
    int    pid;
    int    tgid;
    char   my_name[MY_TASK_COMM_LEN + 1];
    char   my_auxv[1024];
    int    my_auxv_size;
};

struct arg_info {
    uint64_t arg_start;
    uint64_t arg_end;
};

struct map_entry {
    unsigned long start;
    unsigned long end;
    unsigned char flags[4];
    char path[1024];
};
#pragma pack(pop)

static int read_auxv(char *buf, int max_size) {
    FILE *f = fopen("/proc/self/auxv", "rb");
    if (!f) return -1;
    int n = fread(buf, 1, max_size, f);
    fclose(f);
    return n;
}

static int read_cmdline(char *buf, int max_size) {
    FILE *f = fopen("/proc/self/cmdline", "rb");
    if (!f) return -1;
    int n = fread(buf, 1, max_size - 1, f);
    if (n > 0) buf[n] = '\0';
    fclose(f);
    return n > 0 ? n : 0;
}

static ssize_t send_cmd(int fd, struct ioctl_request *hdr, void *data, void *out, size_t out_size) {
    size_t total = sizeof(*hdr) + hdr->buf_size;
    char *buf = malloc(total);
    if (!buf) return -ENOMEM;

    memcpy(buf, hdr, sizeof(*hdr));
    if (data && hdr->buf_size > 0)
        memcpy(buf + sizeof(*hdr), data, hdr->buf_size);

    ssize_t ret = read(fd, buf, total);
    if (ret > 0 && out && out_size > 0) {
        size_t copy = (size_t)ret < out_size ? (size_t)ret : out_size;
        memcpy(out, buf, copy);
    }
    free(buf);
    return ret;
}

static void print_maps_list(char *data, ssize_t size) {
    struct map_entry *e;
    int count = size / sizeof(struct map_entry);
    printf("  共 %d 个内存映射:\n", count);
    for (int i = 0; i < count && i < 30; i++) {
        e = (struct map_entry *)(data + i * sizeof(struct map_entry));
        printf("  %2d: %016lx-%016lx %c%c%c%c %s\n", i + 1,
               e->start, e->end,
               (e->flags[0] & 1) ? 'r' : '-',
               (e->flags[0] & 2) ? 'w' : '-',
               (e->flags[0] & 4) ? 'x' : '-',
               (e->flags[0] & 8) ? 's' : 'p',
               e->path[0] ? e->path : "[anon]");
    }
    if (count > 30) printf("  ... (省略 %d 条)\n", count - 30);
}

static void hexdump(const unsigned char *data, size_t size, size_t max_lines) {
    for (size_t i = 0; i < size && i < max_lines * 16; i += 16) {
        printf("  %08zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size) printf("%02x ", data[i + j]);
            else printf("   ");
        }
        printf(" ");
        for (size_t j = 0; j < 16 && i + j < size; j++) {
            unsigned char c = data[i + j];
            putchar((c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printf("\n");
    }
    if (size > max_lines * 16)
        printf("  ... (省略 %zu 字节)\n", size - max_lines * 16);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
usage:
        printf("用法: rwmem_test <命令> [参数...]\n\n");
        printf("命令:\n");
        printf("  init              初始化设备（必须先执行）\n");
        printf("  pidlist           列出所有进程PID\n");
        printf("  open <pid>        打开进程\n");
        printf("  close <handle>    关闭进程句柄\n");
        printf("  mapcount <handle> 获取内存映射数量\n");
        printf("  maplist <handle>  列出内存映射\n");
        printf("  rss <handle>      获取物理内存占用\n");
        printf("  root <handle>     提升进程权限到root\n");
        printf("  cmdline <handle>  获取进程cmdline地址\n");
        printf("  check <handle> <addr>  检查虚拟地址是否有物理页\n");
        printf("  read <handle> <addr> <size>  读取进程内存\n");
        printf("  write <handle> <addr> <data> 写入进程内存(hex字符串)\n");
        printf("  hide              隐藏驱动模块\n");
        printf("  test <pid>        一键测试: 打开→maps→rss→关闭\n");
        return 1;
    }

    int fd = open(PROC_NODE, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "无法打开 %s: %s\n驱动已加载？\n", PROC_NODE, strerror(errno));
        return 1;
    }

    struct ioctl_request hdr;
    const char *cmd = argv[1];

    /* ── init ── */
    if (!strcmp(cmd, "init")) {
        struct init_device_info info;
        memset(&info, 0, sizeof(info));

        info.pid  = getpid();
        info.tgid = getpid();
        info.my_auxv_size = read_auxv(info.my_auxv, sizeof(info.my_auxv));
        info.my_name[0] = '\0';
        read_cmdline(info.my_name, sizeof(info.my_name));
        if (!info.my_name[0]) strcpy(info.my_name, "rwmem_test");

        printf("[*] 初始化设备...\n");
        printf("    pid=%d tgid=%d name=%s auxv_size=%d\n",
               info.pid, info.tgid, info.my_name, info.my_auxv_size);

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 1; // CMD_INIT_DEVICE_INFO
        hdr.buf_size = sizeof(info);

        ssize_t ret = send_cmd(fd, &hdr, &info, NULL, 0);
        printf("    结果: %s\n", ret == 0 ? "成功" : (ret < 0 ? strerror((int)(-ret)) : "OK"));
    }

    /* ── pidlist ── */
    else if (!strcmp(cmd, "pidlist")) {
        printf("[*] 获取进程列表...\n");
        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 9; // CMD_GET_PID_LIST
        hdr.buf_size = 65536;

        char *buf = malloc(65536);
        ssize_t ret = send_cmd(fd, &hdr, NULL, buf, 65536);
        if (ret >= 0) {
            int count = ret;
            int *pids = (int *)buf;
            printf("    共 %d 个进程:\n", count);
            for (int i = 0; i < count && i < 50; i++) {
                printf("    PID: %d\n", pids[i]);
            }
            if (count > 50) printf("    ... (省略 %d 个)\n", count - 50);
        } else {
            printf("    失败: %s\n", strerror((int)(-ret)));
        }
        free(buf);
    }

    /* ── hide ── */
    else if (!strcmp(cmd, "hide")) {
        printf("[*] 隐藏驱动模块...\n");
        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 13; // CMD_HIDE_KERNEL_MODULE
        ssize_t ret = send_cmd(fd, &hdr, NULL, NULL, 0);
        printf("    结果: %s\n", ret == 0 ? "成功" : "失败");
    }

    /* ── open ── */
    else if (!strcmp(cmd, "open")) {
        if (argc < 3) { printf("用法: rwmem_test open <pid>\n"); return 1; }
        int pid = atoi(argv[2]);
        printf("[*] 打开进程 PID=%d...\n", pid);

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 2; // CMD_OPEN_PROCESS
        hdr.param1 = pid;

        uint64_t handle = 0;
        ssize_t ret = send_cmd(fd, &hdr, NULL, &handle, sizeof(handle));
        if (ret >= 0) {
            printf("    句柄: 0x%llx\n", (unsigned long long)handle);
        } else {
            printf("    失败: %s\n", strerror((int)(-ret)));
        }
    }

    /* ── close ── */
    else if (!strcmp(cmd, "close")) {
        if (argc < 3) { printf("用法: rwmem_test close <handle>\n"); return 1; }
        uint64_t handle = strtoull(argv[2], NULL, 0);
        printf("[*] 关闭句柄 0x%llx...\n", (unsigned long long)handle);

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 5; // CMD_CLOSE_PROCESS
        hdr.param1 = handle;

        ssize_t ret = send_cmd(fd, &hdr, NULL, NULL, 0);
        printf("    结果: %s\n", ret == 0 ? "成功" : "失败");
    }

    /* ── mapcount ── */
    else if (!strcmp(cmd, "mapcount")) {
        if (argc < 3) { printf("用法: rwmem_test mapcount <handle>\n"); return 1; }
        uint64_t handle = strtoull(argv[2], NULL, 0);
        printf("[*] 获取内存映射数量...\n");

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 6; // CMD_GET_PROCESS_MAPS_COUNT
        hdr.param1 = handle;

        ssize_t ret = send_cmd(fd, &hdr, NULL, NULL, 0);
        printf("    映射数量: %ld\n", (long)ret);
    }

    /* ── maplist ── */
    else if (!strcmp(cmd, "maplist")) {
        if (argc < 3) { printf("用法: rwmem_test maplist <handle>\n"); return 1; }
        uint64_t handle = strtoull(argv[2], NULL, 0);
        printf("[*] 获取内存映射列表...\n");

        size_t buf_size = 65536;
        char *buf = malloc(buf_size);

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 7; // CMD_GET_PROCESS_MAPS_LIST
        hdr.param1 = handle;
        hdr.buf_size = buf_size;

        ssize_t ret = send_cmd(fd, &hdr, NULL, buf, buf_size);
        if (ret >= 0) {
            print_maps_list(buf, ret);
        } else {
            printf("    失败\n");
        }
        free(buf);
    }

    /* ── rss ── */
    else if (!strcmp(cmd, "rss")) {
        if (argc < 3) { printf("用法: rwmem_test rss <handle>\n"); return 1; }
        uint64_t handle = strtoull(argv[2], NULL, 0);
        printf("[*] 获取进程物理内存占用...\n");

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 11; // CMD_GET_PROCESS_RSS
        hdr.param1 = handle;

        uint64_t rss = 0;
        ssize_t ret = send_cmd(fd, &hdr, NULL, &rss, sizeof(rss));
        if (ret >= 0) {
            printf("    RSS: %llu 页 (%.2f MB)\n",
                   (unsigned long long)rss,
                   (double)rss * 4 / 1024); // 4KB per page
        } else {
            printf("    失败\n");
        }
    }

    /* ── root ── */
    else if (!strcmp(cmd, "root")) {
        if (argc < 3) { printf("用法: rwmem_test root <handle>\n"); return 1; }
        uint64_t handle = strtoull(argv[2], NULL, 0);
        printf("[*] 提升进程权限到root...\n");

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 10; // CMD_SET_PROCESS_ROOT
        hdr.param1 = handle;

        ssize_t ret = send_cmd(fd, &hdr, NULL, NULL, 0);
        printf("    结果: %s\n", ret == 0 ? "成功" : "失败");
    }

    /* ── cmdline ── */
    else if (!strcmp(cmd, "cmdline")) {
        if (argc < 3) { printf("用法: rwmem_test cmdline <handle>\n"); return 1; }
        uint64_t handle = strtoull(argv[2], NULL, 0);
        printf("[*] 获取进程cmdline地址...\n");

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 12; // CMD_GET_PROCESS_CMDLINE_ADDR
        hdr.param1 = handle;

        struct arg_info aginfo;
        ssize_t ret = send_cmd(fd, &hdr, NULL, &aginfo, sizeof(aginfo));
        if (ret >= 0) {
            printf("    arg_start: 0x%llx\n", (unsigned long long)aginfo.arg_start);
            printf("    arg_end:   0x%llx\n", (unsigned long long)aginfo.arg_end);
        } else {
            printf("    失败\n");
        }
    }

    /* ── check ── */
    else if (!strcmp(cmd, "check")) {
        if (argc < 4) { printf("用法: rwmem_test check <handle> <addr>\n"); return 1; }
        uint64_t handle = strtoull(argv[2], NULL, 0);
        uint64_t addr   = strtoull(argv[3], NULL, 0);
        printf("[*] 检查地址 0x%llx 是否有物理页...\n", (unsigned long long)addr);

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 8; // CMD_CHECK_PROCESS_ADDR_PHY
        hdr.param1 = handle;
        hdr.param2 = addr;

        ssize_t ret = send_cmd(fd, &hdr, NULL, NULL, 0);
        printf("    结果: %s (ret=%ld)\n", ret > 0 ? "有物理页" : "无物理页", (long)ret);
    }

    /* ── read ── */
    else if (!strcmp(cmd, "read")) {
        if (argc < 5) { printf("用法: rwmem_test read <handle> <addr> <size>\n"); return 1; }
        uint64_t handle = strtoull(argv[2], NULL, 0);
        uint64_t addr   = strtoull(argv[3], NULL, 0);
        size_t   size   = strtoul(argv[4], NULL, 0);
        printf("[*] 读取进程内存: handle=0x%llx addr=0x%llx size=%zu\n",
               (unsigned long long)handle, (unsigned long long)addr, size);

        unsigned char *out = malloc(size);
        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 3; // CMD_READ_PROCESS_MEMORY
        hdr.param1 = handle;
        hdr.param2 = addr;
        hdr.buf_size = size;

        ssize_t ret = send_cmd(fd, &hdr, NULL, out, size);
        if (ret > 0) {
            printf("    读取了 %ld 字节:\n", (long)ret);
            hexdump(out, ret, 16);
        } else {
            printf("    失败\n");
        }
        free(out);
    }

    /* ── write ── */
    else if (!strcmp(cmd, "write")) {
        if (argc < 5) { printf("用法: rwmem_test write <handle> <addr> <hex_data>\n"); return 1; }
        uint64_t handle = strtoull(argv[2], NULL, 0);
        uint64_t addr   = strtoull(argv[3], NULL, 0);
        const char *hex = argv[4];
        size_t hexlen = strlen(hex);
        size_t datalen = hexlen / 2;
        unsigned char *data = malloc(datalen);
        for (size_t i = 0; i < datalen; i++) {
            unsigned int byte;
            sscanf(hex + i * 2, "%2x", &byte);
            data[i] = (unsigned char)byte;
        }

        printf("[*] 写入进程内存: handle=0x%llx addr=0x%llx size=%zu\n",
               (unsigned long long)handle, (unsigned long long)addr, datalen);

        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 4; // CMD_WRITE_PROCESS_MEMORY
        hdr.param1 = handle;
        hdr.param2 = addr;
        hdr.param3 = 1; // force write
        hdr.buf_size = datalen;

        ssize_t ret = send_cmd(fd, &hdr, data, NULL, 0);
        printf("    结果: 写入了 %ld 字节\n", ret > 0 ? (long)ret : 0);
        free(data);
    }

    /* ── test ── */
    else if (!strcmp(cmd, "test")) {
        if (argc < 3) { printf("用法: rwmem_test test <pid>\n"); return 1; }
        int pid = atoi(argv[2]);
        printf("══════════════════════════════════════\n");
        printf("  一键测试 PID=%d\n", pid);
        printf("══════════════════════════════════════\n\n");

        // 1. open
        uint64_t handle = 0;
        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 2;
        hdr.param1 = pid;
        ssize_t ret = send_cmd(fd, &hdr, NULL, &handle, sizeof(handle));
        if (ret < 0 || handle == 0) {
            printf("[1/4] 打开进程... 失败 (ret=%ld)\n", (long)ret);
            return 1;
        }
        printf("[1/4] 打开进程... 句柄=0x%llx\n", (unsigned long long)handle);

        // 2. mapcount
        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 6;
        hdr.param1 = handle;
        printf("[2/4] 映射数量... %ld\n", (long)send_cmd(fd, &hdr, NULL, NULL, 0));

        // 3. rss
        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 11;
        hdr.param1 = handle;
        uint64_t rss = 0;
        send_cmd(fd, &hdr, NULL, &rss, sizeof(rss));
        printf("[3/4] 物理内存... %llu 页 (%.2f MB)\n",
               (unsigned long long)rss, (double)rss * 4 / 1024);

        // 4. close
        memset(&hdr, 0, sizeof(hdr));
        hdr.cmd = 5;
        hdr.param1 = handle;
        send_cmd(fd, &hdr, NULL, NULL, 0);
        printf("[4/4] 关闭进程... 完成\n");

        printf("\n══════════════════════════════════════\n");
    }

    else {
        goto usage;
    }

    close(fd);
    return 0;
}
