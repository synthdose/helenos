/*
 * Copyright (c) 2005 Jakub Jermar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup genericconsole
 * @{
 */

/**
 * @file  kconsole.c
 * @brief Kernel console.
 *
 * This file contains kernel thread managing the kernel console.
 *
 */

#include <console/kconsole.h>
#include <console/console.h>
#include <console/chardev.h>
#include <console/cmd.h>
#include <print.h>
#include <panic.h>
#include <arch/types.h>
#include <adt/list.h>
#include <arch.h>
#include <macros.h>
#include <debug.h>
#include <func.h>
#include <string.h>
#include <macros.h>
#include <sysinfo/sysinfo.h>
#include <ddi/device.h>
#include <symtab.h>
#include <errno.h>
#include <putchar.h>
#include <string.h>

/** Simple kernel console.
 *
 * The console is realized by kernel thread kconsole.
 * It doesn't understand any useful command on its own,
 * but makes it possible for other kernel subsystems to
 * register their own commands.
 */

/** Locking.
 *
 * There is a list of cmd_info_t structures. This list
 * is protected by cmd_lock spinlock. Note that specially
 * the link elements of cmd_info_t are protected by
 * this lock.
 *
 * Each cmd_info_t also has its own lock, which protects
 * all elements thereof except the link element.
 *
 * cmd_lock must be acquired before any cmd_info lock.
 * When locking two cmd info structures, structure with
 * lower address must be locked first.
 */

SPINLOCK_INITIALIZE(cmd_lock);  /**< Lock protecting command list. */
LIST_INITIALIZE(cmd_head);      /**< Command list. */

static wchar_t history[KCONSOLE_HISTORY][MAX_CMDLINE] = {};
static size_t history_pos = 0;

/** Initialize kconsole data structures
 *
 * This is the most basic initialization, almost no
 * other kernel subsystem is ready yet.
 *
 */
void kconsole_init(void)
{
	unsigned int i;
	
	cmd_init();
	for (i = 0; i < KCONSOLE_HISTORY; i++)
		history[i][0] = 0;
}

/** Register kconsole command.
 *
 * @param cmd Structure describing the command.
 *
 * @return False on failure, true on success.
 *
 */
bool cmd_register(cmd_info_t *cmd)
{
	link_t *cur;
	
	spinlock_lock(&cmd_lock);
	
	/*
	 * Make sure the command is not already listed.
	 */
	for (cur = cmd_head.next; cur != &cmd_head; cur = cur->next) {
		cmd_info_t *hlp = list_get_instance(cur, cmd_info_t, link);
		
		if (hlp == cmd) {
			/* The command is already there. */
			spinlock_unlock(&cmd_lock);
			return false;
		}
		
		/* Avoid deadlock. */
		if (hlp < cmd) {
			spinlock_lock(&hlp->lock);
			spinlock_lock(&cmd->lock);
		} else {
			spinlock_lock(&cmd->lock);
			spinlock_lock(&hlp->lock);
		}
		
		if (str_cmp(hlp->name, cmd->name) == 0) {
			/* The command is already there. */
			spinlock_unlock(&hlp->lock);
			spinlock_unlock(&cmd->lock);
			spinlock_unlock(&cmd_lock);
			return false;
		}
		
		spinlock_unlock(&hlp->lock);
		spinlock_unlock(&cmd->lock);
	}
	
	/*
	 * Now the command can be added.
	 */
	list_append(&cmd->link, &cmd_head);
	
	spinlock_unlock(&cmd_lock);
	return true;
}

/** Print count times a character */
static void print_cc(wchar_t ch, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
		putchar(ch);
}

/** Try to find a command beginning with prefix */
static const char *cmdtab_search_one(const char *name, link_t **startpos)
{
	size_t namelen = str_length(name);
	
	spinlock_lock(&cmd_lock);
	
	if (*startpos == NULL)
		*startpos = cmd_head.next;
	
	for (; *startpos != &cmd_head; *startpos = (*startpos)->next) {
		cmd_info_t *hlp = list_get_instance(*startpos, cmd_info_t, link);
		
		const char *curname = hlp->name;
		if (str_length(curname) < namelen)
			continue;
		
		if (str_lcmp(curname, name, namelen) == 0) {
			spinlock_unlock(&cmd_lock);
			return (curname + str_lsize(curname, namelen));
		}
	}
	
	spinlock_unlock(&cmd_lock);
	return NULL;
}

/** Command completion of the commands
 *
 * @param name String to match, changed to hint on exit
 * @param size Input buffer size
 *
 * @return Number of found matches
 *
 */
static int cmdtab_compl(char *input, size_t size)
{
	const char *name = input;
	
	size_t found = 0;
	link_t *pos = NULL;
	const char *hint;
	char output[MAX_CMDLINE];
	
	output[0] = 0;
	
	while ((hint = cmdtab_search_one(name, &pos))) {
		if ((found == 0) || (str_length(output) > str_length(hint)))
			str_cpy(output, MAX_CMDLINE, hint);
		
		pos = pos->next;
		found++;
	}
	
	if ((found > 1) && (str_length(output) != 0)) {
		printf("\n");
		pos = NULL;
		while ((hint = cmdtab_search_one(name, &pos))) {
			cmd_info_t *hlp = list_get_instance(pos, cmd_info_t, link);
			printf("%s (%s)\n", hlp->name, hlp->description);
			pos = pos->next;
		}
	}
	
	if (found > 0)
		str_cpy(input, size, output);
	
	return found;
}

static wchar_t *clever_readline(const char *prompt, indev_t *indev)
{
	printf("%s> ", prompt);
	
	size_t position = 0;
	wchar_t *current = history[history_pos];
	current[0] = 0;
	
	while (true) {
		wchar_t ch = indev_pop_character(indev);
		
		if (ch == '\n') {
			/* Enter */
			putchar(ch);
			break;
		}
		
		if (ch == '\b') {
			/* Backspace */
			if (position == 0)
				continue;
			
			if (wstr_remove(current, position - 1)) {
				position--;
				putchar('\b');
				printf("%ls ", current + position);
				print_cc('\b', wstr_length(current) - position + 1);
				continue;
			}
		}
		
		if (ch == '\t') {
			/* Tab completion */
			
			/* Move to the end of the word */
			for (; (current[position] != 0) && (!isspace(current[position]));
			    position++)
				putchar(current[position]);
			
			if (position == 0)
				continue;
			
			/* Find the beginning of the word
			   and copy it to tmp */
			size_t beg;
			for (beg = position - 1; (beg > 0) && (!isspace(current[beg]));
			    beg--);
			
			if (isspace(current[beg]))
				beg++;
			
			char tmp[STR_BOUNDS(MAX_CMDLINE)];
			wstr_to_str(tmp, position - beg + 1, current + beg);
			
			int found;
			if (beg == 0) {
				/* Command completion */
				found = cmdtab_compl(tmp, STR_BOUNDS(MAX_CMDLINE));
			} else {
				/* Symbol completion */
				found = symtab_compl(tmp, STR_BOUNDS(MAX_CMDLINE));
			}
			
			if (found == 0)
				continue;
			
			if (found > 1) {
				/* No unique hint, list was printed */
				printf("%s> ", prompt);
				printf("%ls", current);
				print_cc('\b', wstr_length(current) - position);
				continue;
			}
			
			/* We have a hint */
			
			size_t off = 0;
			size_t i = 0;
			while ((ch = str_decode(tmp, &off, STR_NO_LIMIT)) != 0) {
				if (!wstr_linsert(current, ch, position + i, MAX_CMDLINE))
					break;
				i++;
			}
			
			printf("%ls", current + position);
			position += str_length(tmp);
			print_cc('\b', wstr_length(current) - position);
			
			if (position == wstr_length(current)) {
				/* Insert a space after the last completed argument */
				if (wstr_linsert(current, ' ', position, MAX_CMDLINE)) {
					printf("%ls", current + position);
					position++;
				}
			}
			continue;
		}
		
		if (ch == U_LEFT_ARROW) {
			/* Left */
			if (position > 0) {
				putchar('\b');
				position--;
			}
			continue;
		}
		
		if (ch == U_RIGHT_ARROW) {
			/* Right */
			if (position < wstr_length(current)) {
				putchar(current[position]);
				position++;
			}
			continue;
		}
		
		if ((ch == U_UP_ARROW) || (ch == U_DOWN_ARROW)) {
			/* Up, down */
			print_cc('\b', position);
			print_cc(' ', wstr_length(current));
			print_cc('\b', wstr_length(current));
			
			if (ch == U_UP_ARROW) {
				/* Up */
				if (history_pos == 0)
					history_pos = KCONSOLE_HISTORY - 1;
				else
					history_pos--;
			} else {
				/* Down */
				history_pos++;
				history_pos = history_pos % KCONSOLE_HISTORY;
			}
			current = history[history_pos];
			printf("%ls", current);
			position = wstr_length(current);
			continue;
		}
		
		if (ch == U_HOME_ARROW) {
			/* Home */
			print_cc('\b', position);
			position = 0;
			continue;
		}
		
		if (ch == U_END_ARROW) {
			/* End */
			printf("%ls", current + position);
			position = wstr_length(current);
			continue;
		}
		
		if (ch == U_DELETE) {
			/* Delete */
			if (position == wstr_length(current))
				continue;
			
			if (wstr_remove(current, position)) {
				printf("%ls ", current + position);
				print_cc('\b', wstr_length(current) - position + 1);
			}
			continue;
		}
		
		if (wstr_linsert(current, ch, position, MAX_CMDLINE)) {
			printf("%ls", current + position);
			position++;
			print_cc('\b', wstr_length(current) - position);
		}
	}
	
	if (wstr_length(current) > 0) {
		history_pos++;
		history_pos = history_pos % KCONSOLE_HISTORY;
	}
	
	return current;
}

bool kconsole_check_poll(void)
{
	return check_poll(stdin);
}

static bool parse_int_arg(const char *text, size_t len, unative_t *result)
{
	bool isaddr = false;
	bool isptr = false;
	
	/* If we get a name, try to find it in symbol table */
	if (text[0] == '&') {
		isaddr = true;
		text++;
		len--;
	} else if (text[0] == '*') {
		isptr = true;
		text++;
		len--;
	}
	
	if ((text[0] < '0') || (text[0] > '9')) {
		char symname[MAX_SYMBOL_NAME];
		str_ncpy(symname, MAX_SYMBOL_NAME, text, len + 1);
		
		uintptr_t symaddr;
		int rc = symtab_addr_lookup(symname, &symaddr);
		switch (rc) {
		case ENOENT:
			printf("Symbol %s not found.\n", symname);
			return false;
		case EOVERFLOW:
			printf("Duplicate symbol %s.\n", symname);
			symtab_print_search(symname);
			return false;
		case ENOTSUP:
			printf("No symbol information available.\n");
			return false;
		}
		
		if (isaddr)
			*result = (unative_t) symaddr;
		else if (isptr)
			*result = **((unative_t **) symaddr);
		else
			*result = *((unative_t *) symaddr);
	} else {
		/* It's a number - convert it */
		*result = atoi(text);
		if (isptr)
			*result = *((unative_t *) *result);
	}
	
	return true;
}

/** Parse argument.
 *
 * Find start and end positions of command line argument.
 *
 * @param cmdline Command line as read from the input device.
 * @param size    Size (in bytes) of the string.
 * @param start   On entry, 'start' contains pointer to the offset
 *                of the first unprocessed character of cmdline.
 *                On successful exit, it marks beginning of the next argument.
 * @param end     Undefined on entry. On exit, 'end' is the offset of the first
 *                character behind the next argument.
 *
 * @return False on failure, true on success.
 *
 */
static bool parse_argument(const char *cmdline, size_t size, size_t *start, size_t *end)
{
	ASSERT(start != NULL);
	ASSERT(end != NULL);
	
	bool found_start = false;
	size_t offset = *start;
	size_t prev = *start;
	wchar_t ch;
	
	while ((ch = str_decode(cmdline, &offset, size)) != 0) {
		if (!found_start) {
			if (!isspace(ch)) {
				*start = prev;
				found_start = true;
			}
		} else {
			if (isspace(ch))
				break;
		}
		
		prev = offset;
	}
	*end = prev;
	
	return found_start;
}

/** Parse command line.
 *
 * @param cmdline Command line as read from input device. 
 * @param size    Size (in bytes) of the string.
 *
 * @return Structure describing the command.
 *
 */
static cmd_info_t *parse_cmdline(const char *cmdline, size_t size)
{
	size_t start = 0;
	size_t end = 0;
	if (!parse_argument(cmdline, size, &start, &end)) {
		/* Command line did not contain alphanumeric word. */
		return NULL;
	}
	spinlock_lock(&cmd_lock);
	
	cmd_info_t *cmd = NULL;
	link_t *cur;
	
	for (cur = cmd_head.next; cur != &cmd_head; cur = cur->next) {
		cmd_info_t *hlp = list_get_instance(cur, cmd_info_t, link);
		spinlock_lock(&hlp->lock);
		
		if (str_lcmp(hlp->name, cmdline + start,
		    max(str_length(hlp->name),
		    str_nlength(cmdline + start, (size_t) (end - start) - 1))) == 0) {
			cmd = hlp;
			break;
		}
		
		spinlock_unlock(&hlp->lock);
	}
	
	spinlock_unlock(&cmd_lock);
	
	if (!cmd) {
		/* Unknown command. */
		printf("Unknown command.\n");
		return NULL;
	}
	
	/* cmd == hlp is locked */
	
	/*
	 * The command line must be further analyzed and
	 * the parameters therefrom must be matched and
	 * converted to those specified in the cmd info
	 * structure.
	 */
	
	bool error = false;
	size_t i;
	for (i = 0; i < cmd->argc; i++) {
		start = end;
		if (!parse_argument(cmdline, size, &start, &end)) {
			printf("Too few arguments.\n");
			spinlock_unlock(&cmd->lock);
			return NULL;
		}
		
		char *buf;
		switch (cmd->argv[i].type) {
		case ARG_TYPE_STRING:
			buf = (char *) cmd->argv[i].buffer;
			str_ncpy(buf, cmd->argv[i].len, cmdline + start,
			    end - start);
			break;
		case ARG_TYPE_INT:
			if (!parse_int_arg(cmdline + start, end - start,
			    &cmd->argv[i].intval))
				error = true;
			break;
		case ARG_TYPE_VAR:
			if ((start < end - 1) && (cmdline[start] == '"')) {
				if (cmdline[end - 1] == '"') {
					buf = (char *) cmd->argv[i].buffer;
					str_ncpy(buf, cmd->argv[i].len,
					    cmdline + start + 1,
					    (end - start) - 1);
					cmd->argv[i].intval = (unative_t) buf;
					cmd->argv[i].vartype = ARG_TYPE_STRING;
				} else {
					printf("Wrong synxtax.\n");
					error = true;
				}
			} else if (parse_int_arg(cmdline + start,
			    end - start, &cmd->argv[i].intval)) {
				cmd->argv[i].vartype = ARG_TYPE_INT;
			} else {
				printf("Unrecognized variable argument.\n");
				error = true;
			}
			break;
		case ARG_TYPE_INVALID:
		default:
			printf("Invalid argument type\n");
			error = true;
			break;
		}
	}
	
	if (error) {
		spinlock_unlock(&cmd->lock);
		return NULL;
	}
	
	start = end;
	if (parse_argument(cmdline, size, &start, &end)) {
		printf("Too many arguments.\n");
		spinlock_unlock(&cmd->lock);
		return NULL;
	}
	
	spinlock_unlock(&cmd->lock);
	return cmd;
}

/** Kernel console prompt.
 *
 * @param prompt Kernel console prompt (e.g kconsole/panic).
 * @param msg    Message to display in the beginning.
 * @param kcon   Wait for keypress to show the prompt
 *               and never exit.
 *
 */
void kconsole(char *prompt, char *msg, bool kcon)
{
	if (!stdin) {
		LOG("No stdin for kernel console");
		return;
	}
	
	if (msg)
		printf("%s", msg);
	
	if (kcon)
		indev_pop_character(stdin);
	else
		printf("Type \"exit\" to leave the console.\n");
	
	while (true) {
		wchar_t *tmp = clever_readline((char *) prompt, stdin);
		size_t len = wstr_length(tmp);
		if (!len)
			continue;
		
		char cmdline[STR_BOUNDS(MAX_CMDLINE)];
		wstr_to_str(cmdline, STR_BOUNDS(MAX_CMDLINE), tmp);
		
		if ((!kcon) && (len == 4) && (str_lcmp(cmdline, "exit", 4) == 0))
			break;
		
		cmd_info_t *cmd_info = parse_cmdline(cmdline, STR_BOUNDS(MAX_CMDLINE));
		if (!cmd_info)
			continue;
		
		(void) cmd_info->func(cmd_info->argv);
	}
}

/** Kernel console managing thread.
 *
 */
void kconsole_thread(void *data)
{
	kconsole("kconsole", "Kernel console ready (press any key to activate)\n", true);
}

/** @}
 */
