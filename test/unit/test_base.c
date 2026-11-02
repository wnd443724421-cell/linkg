/**
 * @file test_base.c
 * @brief 基础文本、时间和原子文件逻辑测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-02
 */

#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linkg_atomic_file.h"
#include "linkg_file.h"
#include "linkg_text.h"
#include "linkg_time.h"

static void test_text_edit(void)
{
    char  *content;
    size_t length;

    content = strdup("alpha=1\n beta = old\nalpha=duplicate\ntail=3");
    TEST_CHECK(content != NULL);
    length = strlen(content);

    TEST_CHECK_EQ(linkg_text_replace_key_value(&content, &length, "alpha", "2"), 0);
    TEST_CHECK(strcmp(content, "alpha=2\n beta = old\ntail=3") == 0);
    TEST_CHECK_EQ(length, strlen(content));

    TEST_CHECK_EQ(linkg_text_set_key_value(&content, &length, "new_key", "value"), 0);
    TEST_CHECK(strcmp(content, "alpha=2\n beta = old\ntail=3\nnew_key=value\n") == 0);

    TEST_CHECK_EQ(linkg_text_remove_key_value(&content, &length, "beta"), 0);
    TEST_CHECK(strcmp(content, "alpha=2\ntail=3\nnew_key=value\n") == 0);
    TEST_CHECK_EQ(linkg_text_replace_key_value(&content, &length, "missing", "x"), -ENOENT);
    TEST_CHECK_EQ(linkg_text_set_key_value(&content, &length, "bad key", "x"), -EINVAL);
    TEST_CHECK_EQ(linkg_text_set_key_value(&content, &length, "safe", "bad\nvalue"), -EINVAL);

    free(content);
}

static void test_time_logic(void)
{
    char     buffer[LINKG_TIME_US_STR_SIZE];
    uint64_t before;
    uint64_t after;

    TEST_CHECK_EQ(linkg_time_format_ms(3723004ULL, buffer, sizeof(buffer)), 0);
    TEST_CHECK(strcmp(buffer, "01:02:03.004") == 0);
    TEST_CHECK_EQ(linkg_time_format_us(3723004005ULL, buffer, sizeof(buffer)), 0);
    TEST_CHECK(strcmp(buffer, "01:02:03.004005") == 0);

    TEST_CHECK_EQ(linkg_time_format_ms(0U, NULL, 0U), -EINVAL);
    TEST_CHECK_EQ(linkg_time_format_us(1U, buffer, 4U), -ENOSPC);
    TEST_CHECK_EQ(linkg_time_init(), 0);

    before = linkg_time_monotonic_us();
    TEST_CHECK_EQ(linkg_time_sleep_ms(2U), 0);
    after = linkg_time_monotonic_us();

    TEST_CHECK(after >= before);
    TEST_CHECK(after - before >= 1000U);
}

static void test_file_and_atomic_logic(void)
{
    const char data[] = "linkg-v1";
    char       directory[] = "/tmp/linkg-v1-base-XXXXXX";
    char       atomic_path[256];
    char       binary_path[256];
    char       file_path[256];
    char       link_path[256];
    char       read_buffer[32];
    char      *content;
    size_t     length;
    FILE      *stream;

    TEST_CHECK(mkdtemp(directory) != NULL);
    TEST_CHECK(snprintf(file_path, sizeof(file_path), "%s/file.txt", directory) > 0);
    TEST_CHECK(snprintf(atomic_path, sizeof(atomic_path), "%s/state.txt", directory) > 0);
    TEST_CHECK(snprintf(binary_path, sizeof(binary_path), "%s/binary.txt", directory) > 0);
    TEST_CHECK(snprintf(link_path, sizeof(link_path), "%s/state.link", directory) > 0);

    TEST_CHECK_EQ(linkg_file_write_all_atomic(file_path, data, sizeof(data) - 1U, 0600), 0);
    content = NULL;
    length  = 0U;
    TEST_CHECK_EQ(linkg_file_read_all(file_path, 64U, &content, &length), 0);
    TEST_CHECK_EQ(length, sizeof(data) - 1U);
    TEST_CHECK(memcmp(content, data, length) == 0);
    free(content);

    content = NULL;
    TEST_CHECK_EQ(linkg_file_read_all(file_path, 2U, &content, &length), -EFBIG);
    TEST_CHECK(content == NULL);

    TEST_CHECK_EQ(linkg_atomic_write(atomic_path, data, sizeof(data) - 1U), 0);
    memset(read_buffer, 0, sizeof(read_buffer));
    TEST_CHECK_EQ(linkg_atomic_read(atomic_path, read_buffer, sizeof(read_buffer)), 0);
    TEST_CHECK(strcmp(read_buffer, data) == 0);

    stream = fopen(binary_path, "wb");
    TEST_CHECK(stream != NULL);
    TEST_CHECK_EQ(fwrite("a\0b", 1U, 3U, stream), 3U);
    TEST_CHECK_EQ(fclose(stream), 0);
    TEST_CHECK_EQ(linkg_atomic_read(binary_path, read_buffer, sizeof(read_buffer)), -EINVAL);

    TEST_CHECK_EQ(symlink(atomic_path, link_path), 0);
    TEST_CHECK_EQ(linkg_atomic_read(link_path, read_buffer, sizeof(read_buffer)), -ELOOP);

    TEST_CHECK_EQ(linkg_atomic_remove(atomic_path), 0);
    TEST_CHECK_EQ(linkg_atomic_read(atomic_path, read_buffer, sizeof(read_buffer)), -ENOENT);

    TEST_CHECK_EQ(unlink(link_path), 0);
    TEST_CHECK_EQ(unlink(binary_path), 0);
    TEST_CHECK_EQ(unlink(file_path), 0);
    TEST_CHECK_EQ(rmdir(directory), 0);
}

int main(void)
{
    TEST_RUN(test_text_edit);
    TEST_RUN(test_time_logic);
    TEST_RUN(test_file_and_atomic_logic);

    return linkg_test_finish("base");
}
