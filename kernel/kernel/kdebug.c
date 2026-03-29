#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <kernel/kdebug.h>
#include <kernel/keyboard.h>
#include <kernel/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/vfs.h>

#define KDEBUG_LINE_MAX 128

static bool debugger_active;

static void kdebug_print_prompt(void) {
	printf("\n[kdebug] ");
}

static bool starts_with(const char* text, const char* prefix) {
	return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void kdebug_show_tasks(void) {
	scheduler_task_snapshot_t snapshot;
	const uint32_t count = scheduler_task_count();

	for (uint32_t i = 0; i < count; i++) {
		if (!scheduler_get_task_snapshot(i, &snapshot))
			continue;
		printf("task %u %-12s state=%s wake=%u\n",
			snapshot.id, snapshot.name, snapshot.state, snapshot.wakeup_tick);
	}
}

static void kdebug_list_files(void) {
	vfs_node_t node;
	for (uint32_t i = 0; i < vfs_file_count(); i++) {
		if (vfs_get_file(i, &node))
			printf("%s (%u bytes)\n", node.name, (unsigned int) node.size);
	}
}

static void kdebug_cat_file(const char* path) {
	vfs_node_t node;

	if (!vfs_open(path, &node)) {
		printf("no such file: %s\n", path);
		return;
	}

	printf("----- %s -----\n", node.name);
	for (size_t i = 0; i < node.size; i++)
		putchar(node.data[i]);
	if (node.size == 0 || node.data[node.size - 1] != '\n')
		putchar('\n');
}

static void kdebug_execute(char* line) {
	if (strcmp(line, "help") == 0) {
		printf("commands: help, tasks, mem, ls, cat <file>, continue\n");
	} else if (strcmp(line, "tasks") == 0) {
		kdebug_show_tasks();
	} else if (strcmp(line, "mem") == 0) {
		printf("memory: total=%u KiB free=%u KiB\n",
			pmm_total_memory_kib(), pmm_free_memory_kib());
	} else if (strcmp(line, "ls") == 0) {
		kdebug_list_files();
	} else if (starts_with(line, "cat ")) {
		kdebug_cat_file(line + 4);
	} else if (strcmp(line, "continue") == 0) {
		debugger_active = false;
		printf("leaving debugger\n");
		return;
	} else if (line[0] != '\0') {
		printf("unknown command: %s\n", line);
	}

	if (debugger_active)
		kdebug_print_prompt();
}

static void kdebug_task(void* arg) {
	(void) arg;
	char line[KDEBUG_LINE_MAX];
	size_t line_length = 0;

	while (true) {
		if (!debugger_active && keyboard_debug_requested()) {
			keyboard_clear_debug_request();
			debugger_active = true;
			printf("\nentered kernel debugger, type 'help'\n");
			kdebug_print_prompt();
		}

		if (!debugger_active) {
			scheduler_sleep(1);
			continue;
		}

		char c;
		if (!keyboard_getchar_nonblocking(&c)) {
			// The debugger is a normal scheduler task, so it sleeps instead of spinning.
			scheduler_sleep(1);
			continue;
		}

		if (c == '\r')
			c = '\n';

		if (c == '\n') {
			putchar('\n');
			line[line_length] = '\0';
			// Commands are interpreted in-place from the line buffer to keep the debugger tiny.
			kdebug_execute(line);
			line_length = 0;
			continue;
		}

		if (c == '\b') {
			if (line_length > 0) {
				line_length--;
				putchar('\b');
			}
			continue;
		}

		if (line_length + 1 < sizeof(line)) {
			line[line_length++] = c;
			putchar(c);
		}
	}
}

void kdebug_init(void) {
	debugger_active = false;
	scheduler_create_kernel_task("kdebug", kdebug_task, NULL);
}
