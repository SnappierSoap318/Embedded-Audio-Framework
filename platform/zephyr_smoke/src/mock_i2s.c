/* Test-only I2S device: models slab ownership, not DMA timing. */
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
static struct i2s_config config;
static void *pending;
static bool started;
unsigned eaf_mock_commits;
int32_t eaf_mock_last;
bool eaf_mock_fail_write, eaf_mock_fail_start, eaf_mock_fail_drop, eaf_mock_fail_drain;
static int configure(const struct device *dev, enum i2s_dir dir, const struct i2s_config *cfg) {
    (void)dev;
    if (dir != I2S_DIR_TX || cfg->word_size != 32 || cfg->channels != 2)
        return -EINVAL;
    config = *cfg;
    return 0;
}
static int write_block(const struct device *dev, void *block, size_t bytes) {
    (void)dev;
    if (eaf_mock_fail_write || pending || bytes != config.block_size)
        return -EIO;
    eaf_mock_last = ((int32_t *)block)[bytes / sizeof(int32_t) - 1u];
    ++eaf_mock_commits;
    if (started)
        k_mem_slab_free(config.mem_slab, block);
    else
        pending = block;
    return 0;
}
static int trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd) {
    (void)dev;
    (void)dir;
    if (cmd == I2S_TRIGGER_START) {
        if (eaf_mock_fail_start || !pending)
            return -EIO;
        started = true;
    } else if (cmd == I2S_TRIGGER_DRAIN) {
        if (eaf_mock_fail_drain)
            return -EIO;
        started = false;
    } else if (cmd == I2S_TRIGGER_DROP) {
        if (eaf_mock_fail_drop)
            return -EIO;
        started = false;
    } else
        return -EINVAL;
    if (pending) {
        k_mem_slab_free(config.mem_slab, pending);
        pending = NULL;
    }
    return 0;
}
static DEVICE_API(i2s, api) = {.configure = configure, .write = write_block, .trigger = trigger};
DEVICE_DEFINE(eaf_test_i2s, "eaf_mock_i2s", NULL, NULL, NULL, NULL, POST_KERNEL, 50, &api);
