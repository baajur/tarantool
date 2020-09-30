#include <stdio.h>
#include <stdbool.h>
#include <msgpuck.h>

#include "module.h"

/*
 * Just make sure we've been called.
 */
int
cfunc_sole_call(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	say_info("-- cfunc_sole_call -  called --");
	printf("ok - cfunc_sole_call\n");
	return 0;
}

/*
 * Fetch N-array
 */
int
cfunc_fetch_array(box_function_ctx_t *ctx, const char *args, const char *args_end)
{
	int arg_count = mp_decode_array(&args);
	int field_count = mp_decode_array(&args);

	(void)arg_count;
	(void)field_count;

	say_info("-- cfunc_fetch_array -  called --");

	for (int i = 0; i < field_count; i++) {
		int val = mp_decode_uint(&args);
		int needed = 2 * (i+1);
		if (val != needed) {
			char res[128];
			snprintf(res, sizeof(res), "%s %d != %d",
				 "invalid argument", val, needed);
			return box_error_set(__FILE__, __LINE__,
					     ER_PROC_C, "%s", res);
		}
	}

	printf("ok - cfunc_fetch_array\n");
	return 0;
}
