/*
 * SPDX-FileCopyrightText: 2026 FreeRDP-M contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <winpr/wlog.h>

#if !defined(_WIN32) && !defined(ANDROID)
#include <poll.h>
#include <unistd.h>
#endif

int TestWLogConsoleFlush(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
#if !defined(_WIN32) && !defined(ANDROID)
	int result = 1;
	int descriptors[2] = { -1, -1 };
	wLog* root = WLog_GetRoot();
	if (!root || !WLog_SetLogAppenderType(root, WLOG_APPENDER_CONSOLE) ||
	    !WLog_SetLogLevel(root, WLOG_INFO) || pipe(descriptors) != 0)
		return 1;
	(void)fflush(stdout);
	const int saved = dup(STDOUT_FILENO);
	if (saved < 0)
		goto out;
	if (dup2(descriptors[1], STDOUT_FILENO) < 0)
		goto restore;
	WLog_Print(root, WLOG_INFO, "console flush regression");
	struct pollfd descriptor = { .fd = descriptors[0], .events = POLLIN };
	// No explicit fflush: the console appender must make INFO visible while
	// the process is still running, even when stdout is connected to a pipe.
	if ((poll(&descriptor, 1, 500) > 0) && (descriptor.revents & POLLIN))
		result = 0;
restore:
	(void)fflush(stdout);
	(void)dup2(saved, STDOUT_FILENO);
	(void)close(saved);
out:
	(void)close(descriptors[0]);
	(void)close(descriptors[1]);
	return result;
#else
	return 0;
#endif
}
