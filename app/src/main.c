
#include <stdio.h>

#include <zephyr/settings/settings.h>

#include <errno.h>
#include <zephyr/sys/printk.h>

#if defined(CONFIG_SETTINGS_FILE)
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#endif

#define STORAGE_PARTITION    storage_partition
#define STORAGE_PARTITION_ID FIXED_PARTITION_ID(STORAGE_PARTITION)

uint8_t angle_val;
uint64_t length_val = 100;
uint16_t length_1_val = 40;
uint32_t length_2_val = 60;

int alpha_handle_export(int (*cb)(const char *name, const void *value, size_t val_len))
{
	printk("export keys under <alpha> handler\n");
	(void)cb("alpha/angle/1", &angle_val, sizeof(angle_val));
	printk("angle_val = %d\n", angle_val);
	(void)cb("alpha/length", &length_val, sizeof(length_val));
	printk("length_val = %" PRId64 "\n", length_val);
	(void)cb("alpha/length/1", &length_1_val, sizeof(length_1_val));
	printk("length_1_val = %d\n", length_1_val);
	(void)cb("alpha/length/2", &length_2_val, sizeof(length_2_val));
	printk("length_2_val = %d\n", length_2_val);

	return 0;
}

int alpha_handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	size_t next_len;
	int rc;

	printk("loading key: <%s>\n", name);

	if (settings_name_steq(name, "angle/1", &next) && !next) {
		if (len != sizeof(angle_val)) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, &angle_val, sizeof(angle_val));
		printk("<alpha/angle/1> = %d\n", angle_val);
		return 0;
	}

	next_len = settings_name_next(name, &next);

	if (!next) {
		return -ENOENT;
	}

	if (!strncmp(name, "length", next_len)) {
		next_len = settings_name_next(name, &next);

		if (!next) {
			rc = read_cb(cb_arg, &length_val, sizeof(length_val));
			printk("<alpha/length> = %" PRId64 "\n", length_val);
			return 0;
		}

		if (!strncmp(next, "1", next_len)) {
			rc = read_cb(cb_arg, &length_1_val, sizeof(length_1_val));
			printk("<alpha/length/1> = %d\n", length_1_val);
			return 0;
		}

		if (!strncmp(next, "2", next_len)) {
			rc = read_cb(cb_arg, &length_2_val, sizeof(length_2_val));
			printk("<alpha/length/2> = %d\n", length_2_val);
			return 0;
		}

		return -ENOENT;
	}

	return -ENOENT;
}

int alpha_handle_commit(void)
{
	printk("loading all settings under <alpha> handler is done\n");
	return 0;
}

/* dynamic main tree handler */
struct settings_handler alph_handler = {.name = "alpha",
					.h_get = NULL,
					.h_set = alpha_handle_set,
					// .h_commit = alpha_handle_commit,
					.h_export = alpha_handle_export};

static int user_setting_initial(void)
{
	int rc = 0;

#if defined(CONFIG_SETTINGS_FILE)
	FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(cstorage);

	/* mounting info */
	static struct fs_mount_t littlefs_mnt = {.type = FS_LITTLEFS,
						 .fs_data = &cstorage,
						 .storage_dev = (void *)STORAGE_PARTITION_ID,
						 .mnt_point = "/ff"};

	rc = fs_mount(&littlefs_mnt);
	if (rc != 0) {
		printk("mounting littlefs error: [%d]\n", rc);
	} else {

		rc = fs_unlink(CONFIG_SETTINGS_FILE_PATH);
		if ((rc != 0) && (rc != -ENOENT)) {
			printk("can't delete config file%d\n", rc);
		} else {
			printk("FS initialized: OK\n");
		}
	}
#endif

	rc = settings_subsys_init();
	if (rc) {
		printk("settings subsys initialization: fail (err %d)\n", rc);
		return rc;
	}

	printk("settings subsys initialization: OK.\n");

	rc = settings_register(&alph_handler);
	if (rc) {
		printk("subtree <%s> handler registered: fail (err %d)\n", alph_handler.name, rc);
	}

	printk("subtree <%s> handler registered: OK\n", alph_handler.name);

	return rc;
}

void alph_inc(void)
{
	angle_val++;
	length_val++;
	length_1_val++;
	length_2_val++;
}

void alph_dump(void)
{
	printk("::::angle_val = %d\n", angle_val);
	printk("::::length_val = %" PRId64 "\n", length_val);
	printk("::::length_1_val = %d\n", length_1_val);
	printk("::::length_2_val = %d\n", length_2_val);
}

int main(void)
{
	int rc;

	printf("Hello World! %s\n", CONFIG_BOARD);

	user_setting_initial();

	rc = settings_load();
	if (rc) {
		printk("settings_load fail: %d\n", rc);
	}

	alph_inc();
	alph_dump();

	rc = settings_save();
	if (rc) {
		printk("settings_save fail: %d\n", rc);
	}

	return 0;
}
