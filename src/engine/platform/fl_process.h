#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Path of the ffmpeg executable: $FLOW_FFMPEG, then PATH, then common install folders. NULL when none is found. */
const char *fl_ffmpeg_path(void);

/*
 * Append arg to a shell command line, quoted so spaces and quotes in it stay one argument. False when the buffer
 * is too small; command is then unchanged.
 */
bool fl_command_append(char *command, size_t capacity, const char *arg);

/* Run a command line to completion; true when it exits with status zero. */
bool fl_command_run(const char *command);

/* Start a command whose standard input is written by the caller; close with fl_command_close. NULL on failure. */
FILE *fl_command_open_writer(const char *command);
/* Close a writer and report whether the command exited with status zero. */
bool fl_command_close(FILE *writer);
