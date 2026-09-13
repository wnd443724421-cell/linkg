#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define DEFAULT_UPGRADE_PATH "/tmp/upgrade.tar.gz"
#define DEFAULT_CONFIG_PATH "/tmp/config_import.json"
#define TMP_PART_PATH "/tmp/upload.tmp.part"

static void print_json(const char *code, const char *msg)
{
    printf("Content-Type: application/json\r\n\r\n");
    printf("{\"code\":\"%s\",\"msg\":\"%s\"}", code, msg);
}

int main(void)
{
    char *type_header = getenv("HTTP_X_UPLOAD_TYPE");
    char *target_path = DEFAULT_UPGRADE_PATH;
    if (type_header && strcmp(type_header, "config") == 0)
        target_path = DEFAULT_CONFIG_PATH;

    char *len_str = getenv("CONTENT_LENGTH");
    long len = len_str ? atol(len_str) : 0;
    if (len <= 0)
    {
        print_json("error", "No data");
        return 0;
    }

    FILE *fp = fopen(TMP_PART_PATH, "wb");
    if (!fp)
    {
        print_json("error", "Disk error");
        return 0;
    }

    char buf[8192];
    long total_read = 0;
    while (total_read < len)
    {
        size_t to_read = (size_t)((len - total_read) > (long)sizeof(buf) ? sizeof(buf) : (len - total_read));
        size_t n = fread(buf, 1, to_read, stdin);
        if (n <= 0)
            break;
        fwrite(buf, 1, n, fp);
        total_read += (long)n;
    }
    fclose(fp);

    if (total_read != len)
    {
        unlink(TMP_PART_PATH);
        print_json("error", "Size mismatch");
        return 0;
    }

    // 使用局部变量 target_path 决定重命名目的地
    if (rename(TMP_PART_PATH, target_path) != 0)
    {
        unlink(TMP_PART_PATH);
        print_json("error", "Rename failed");
        return 0;
    }

    print_json("ok", "Success");
    return 0;
}
