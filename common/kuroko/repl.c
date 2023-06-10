/**
 * @file repl.c
 * @brief General-purpose Kuroko repl using rline, with tab completion.
 *
 * This is a generic repl. Initialize the @c __main__ module and then
 * call @c krk_repl() to run a repl session. You can bind a builtin
 * @c exit() function to set @c krk_exitRepl to 1 to exit the repl;
 * the repl will also terminate if rline detects an EOF condition.
 */
#include <kuroko/vm.h>
#include <kuroko/scanner.h>
#include <kuroko/util.h>
#include <rline.h>

#define PROMPT_MAIN  ">>> "
#define PROMPT_BLOCK "  > "

int krk_exitRepl = 0;

void krk_printResult(KrkValue result) {
	if (IS_NONE(result)) return;
	krk_attachNamedValue(&vm.builtins->fields, "_", result);
	KrkClass * type = krk_getType(result);
	const char * formatStr = " \033[1;90m=> %s\033[0m\n";
	if (type->_reprer) {
		krk_push(result);
		result = krk_callDirect(type->_reprer, 1);
	} else if (type->_tostr) {
		krk_push(result);
		result = krk_callDirect(type->_tostr, 1);
	}
	if (!IS_STRING(result)) {
		printf(" \033[1;91m=> Unable to produce representation for value.\033[0m\n");
	} else {
		printf(formatStr, AS_CSTRING(result));
	}
}

#if 1
static KrkValue findFromProperty(KrkValue current, KrkToken next) {
	KrkValue member = OBJECT_VAL(krk_copyString(next.start, next.literalWidth));
	krk_push(member);
	KrkValue value = krk_valueGetAttribute_default(current, AS_CSTRING(member), NONE_VAL());
	krk_pop();
	return value;
}

static char * syn_krk_keywords[] = {
	"and","class","def","else","for","if","in","import","del",
	"let","not","or","return","while","try","except","raise",
	"continue","break","as","from","elif","lambda","with","is",
	"pass","assert","yield","finally","async","await",
	"True","False","None",
	NULL
};

static void tab_complete_func(rline_context_t * c) {
	/* Figure out where the cursor is and if we should be completing anything. */
	if (c->offset) {
		size_t stackIn = krk_currentThread.stackTop - krk_currentThread.stack;
		/* Copy up to the cursor... */
		char * tmp = malloc(c->offset + 1);
		memcpy(tmp, c->buffer, c->offset);
		tmp[c->offset] = '\0';
		/* and pass it to the scanner... */
		KrkScanner scanner = krk_initScanner(tmp);
		/* Logically, there can be at most (offset) tokens, plus some wiggle room. */
		KrkToken * space = malloc(sizeof(KrkToken) * (c->offset + 2));
		int count = 0;
		do {
			space[count++] = krk_scanToken(&scanner);
		} while (space[count-1].type != TOKEN_EOF && space[count-1].type != TOKEN_ERROR);

		/* If count == 1, it was EOF or an error and we have nothing to complete. */
		if (count == 1) {
			goto _cleanup;
		}

		/* Otherwise we want to see if we're on an identifier or a dot. */
		int base = 2;
		int n = base;
		if (space[count-base].type == TOKEN_DOT) {
			/* Dots we need to look back at the previous tokens for */
			n--;
			base--;
		} else if (space[count-base].type >= TOKEN_IDENTIFIER && space[count-base].type <= TOKEN_WITH) {
			/* Something alphanumeric; only for the last element */
		} else {
			/* Some other symbol */
			goto _cleanup;
		}

		/* Work backwards to find the start of this chain of identifiers */
		while (n < count) {
			if (space[count-n-1].type != TOKEN_DOT) break;
			n++;
			if (n == count) break;
			if (space[count-n-1].type != TOKEN_IDENTIFIER) break;
			n++;
		}

		if (n <= count) {
			/* Now work forwards, starting from the current globals. */
			KrkValue root = OBJECT_VAL(krk_currentThread.module);
			int isGlobal = 1;
			while (n > base) {
				/* And look at the potential fields for instances/classes */
				KrkValue next = findFromProperty(root, space[count-n]);
				if (IS_NONE(next)) {
					/* If we hit None, we found something invalid (or literally hit a None
					 * object, but really the difference is minimal in this case: Nothing
					 * useful to tab complete from here. */
					if (!isGlobal) goto _cleanup;
					/* Does this match a builtin? */
					if (!krk_tableGet_fast(&vm.builtins->fields,
						krk_copyString(space[count-n].start,space[count-n].literalWidth), &next) || IS_NONE(next)) {
						goto _cleanup;
					}
				}
				isGlobal = 0;
				root = next;
				n -= 2; /* To skip every other dot. */
			}

			if (isGlobal && n < count && (space[count-n-1].type == TOKEN_IMPORT || space[count-n-1].type == TOKEN_FROM)) {
				KrkInstance * modules = krk_newInstance(vm.baseClasses->objectClass);
				root = OBJECT_VAL(modules);
				krk_push(root);
				for (size_t i = 0; i < vm.modules.capacity; ++i) {
					KrkTableEntry * entry = &vm.modules.entries[i];
					if (IS_KWARGS(entry->key)) continue;
					krk_attachNamedValue(&modules->fields, AS_CSTRING(entry->key), NONE_VAL());
				}
			}

			/* Now figure out what we're completing - did we already have a partial symbol name? */
			int length = (space[count-base].type == TOKEN_DOT) ? 0 : (space[count-base].length);
			isGlobal = isGlobal && (length != 0);

			/* Collect up to 256 of those that match */
			char * matches[256];
			int matchCount = 0;

			/* Take the last symbol name from the chain and get its member list from dir() */

			for (;;) {
				KrkValue dirList = krk_dirObject(1,(KrkValue[]){root},0);
				krk_push(dirList);
				if (!IS_INSTANCE(dirList)) {
					fprintf(stderr,"\nInternal error while tab completting.\n");
					goto _cleanup;
				}

				for (size_t i = 0; i < AS_LIST(dirList)->count; ++i) {
					KrkString * s = AS_STRING(AS_LIST(dirList)->values[i]);
					krk_push(OBJECT_VAL(s));
					KrkToken asToken = {.start = s->chars, .literalWidth = s->length};
					KrkValue thisValue = findFromProperty(root, asToken);
					krk_push(thisValue);
					if (IS_CLOSURE(thisValue) || IS_BOUND_METHOD(thisValue) || IS_NATIVE(thisValue)) {
						size_t allocSize = s->length + 2;
						char * tmp = malloc(allocSize);
						size_t len = snprintf(tmp, allocSize, "%s(", s->chars);
						s = krk_takeString(tmp, len);
						krk_pop();
						krk_push(OBJECT_VAL(s));
					} else {
						krk_pop();
					}

					/* If this symbol is shorter than the current submatch, skip it. */
					if (length && (int)s->length < length) continue;

					/* See if it's already in the matches */
					int found = 0;
					for (int i = 0; i < matchCount; ++i) {
						if (!strcmp(matches[i], s->chars)) {
							found = 1;
							break;
						}
					}
					if (found) continue;

					if (!memcmp(s->chars, space[count-base].start, length)) {
						matches[matchCount] = s->chars;
						matchCount++;
						if (matchCount == 255) goto _toomany;
					}
				}

				/*
				 * If the object we were scanning was the current module,
				 * then we should also throw the builtins into the ring.
				 */
				if (isGlobal && AS_OBJECT(root) == (KrkObj*)krk_currentThread.module) {
					root = OBJECT_VAL(vm.builtins);
					continue;
				} else if (isGlobal && AS_OBJECT(root) == (KrkObj*)vm.builtins) {
					KrkInstance * fakeKeywordsObject = krk_newInstance(vm.baseClasses->objectClass);
					root = OBJECT_VAL(fakeKeywordsObject);
					krk_push(root);
					for (char ** keyword = syn_krk_keywords; *keyword; keyword++) {
						krk_attachNamedValue(&fakeKeywordsObject->fields, *keyword, NONE_VAL());
					}
					continue;
				} else {
					break;
				}
			}
_toomany:

			/* Now we can do things with the matches. */
			if (matchCount == 1) {
				/* If there was only one, just fill it. */
				rline_insert(c, matches[0] + length);
				rline_place_cursor();
			} else if (matchCount) {
				/* Otherwise, try to find a common substring among them... */
				int j = length;
				while (1) {
					char m = matches[0][j];
					if (!m) break;
					int diff = 0;
					for (int i = 1; i < matchCount; ++i) {
						if (matches[i][j] != m) {
							diff = 1;
							break;
						}
					}
					if (diff) break;
					j++;
				}
				/* If no common sub string could be filled in, we print the list. */
				if (j == length) {
					/* First find the maximum width of an entry */
					int maxWidth = 0;
					for (int i = 0; i < matchCount; ++i) {
						if ((int)strlen(matches[i]) > maxWidth) maxWidth = strlen(matches[i]);
					}
					/* Now how many can we fit in a screen */
					int colsPerLine = rline_terminal_width / (maxWidth + 2); /* +2 for the spaces */
					fprintf(stderr, "\n");
					int column = 0;
					for (int i = 0; i < matchCount; ++i) {
						fprintf(stderr, "%-*s  ", maxWidth, matches[i]);
						column += 1;
						if (column >= colsPerLine) {
							fprintf(stderr, "\n");
							column = 0;
						}
					}
					if (column != 0) fprintf(stderr, "\n");
				} else {
					/* If we do have a common sub string, fill in those characters. */
					for (int i = length; i < j; ++i) {
						char tmp[2] = {matches[0][i], '\0'};
						rline_insert(c, tmp);
					}
				}
			}
		}
_cleanup:
		free(tmp);
		free(space);
		krk_currentThread.stackTop = &krk_currentThread.stack[stackIn];
		return;
	}
}

void krk_repl(void) {
	krk_exitRepl = 0;
	while (!krk_exitRepl) {
		size_t lineCapacity = 8;
		size_t lineCount = 0;
		char ** lines = ALLOCATE(char *, lineCapacity);
		size_t totalData = 0;
		int valid = 1;
		char * allData = NULL;
		int inBlock = 0;
		int blockWidth = 0;
		rline_exp_set_prompts(PROMPT_MAIN, "", 4, 0);
		rline_exit_string="exit";
		rline_exp_set_syntax("krk");
		rline_exp_set_tab_complete_func(tab_complete_func);

		while (1) {
			char buf[4096] = {0};
			if (inBlock) {
				rline_exp_set_prompts(PROMPT_BLOCK, "", 4, 0);
				rline_preload = malloc(blockWidth + 1);
				for (int i = 0; i < blockWidth; ++i) {
					rline_preload[i] = ' ';
				}
				rline_preload[blockWidth] = '\0';
			}
			rline_scroll = 0;
			if (rline(buf, 4096) == 0) {
				valid = 0;
				krk_exitRepl = 1;
				break;
			}
			if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) {
				/* We should actually be properly raising the interrupt and printing it with an empty traceback, but whatever. */
				krk_currentThread.flags &= ~(KRK_THREAD_SIGNALLED); /* Clear signal flag */
				printf("KeyboardInterrupt\n");
				valid = 0;
				break;
			}
			if (buf[strlen(buf)-1] != '\n') {
				valid = 0;
				break;
			}
			if (lineCapacity < lineCount + 1) {
				size_t old = lineCapacity;
				lineCapacity = GROW_CAPACITY(old);
				lines = GROW_ARRAY(char *,lines,old,lineCapacity);
			}
			int i = lineCount++;
			lines[i] = strdup(buf);
			size_t lineLength = strlen(lines[i]);
			totalData += lineLength;
			int isSpaces = 1;
			int countSpaces = 0;
			for (size_t j = 0; j < lineLength; ++j) {
				if (lines[i][j] != ' ' && lines[i][j] != '\n') {
					isSpaces = 0;
					break;
				}
				countSpaces += 1;
			}
			if (lineLength > 1 && lines[i][lineLength-2] == ':') {
				inBlock = 1;
				blockWidth = countSpaces + 4;
				continue;
			} else if (lineLength > 1 && lines[i][lineLength-2] == '\\') {
				inBlock = 1;
				continue;
			} else if (inBlock && lineLength != 1) {
				if (isSpaces) {
					free(lines[i]);
					totalData -= lineLength;
					lineCount--;
					break;
				}
				blockWidth = countSpaces;
				continue;
			} else if (lineLength > 1 && lines[i][countSpaces] == '@') {
				inBlock = 1;
				blockWidth = countSpaces;
				continue;
			}
			if (isSpaces && !i) valid = 0;
			break;
		}
		if (valid) {
			allData = malloc(totalData + 1);
			allData[0] = '\0';
		}
		for (size_t i = 0; i < lineCount; ++i) {
			if (valid) strcat(allData, lines[i]);
			rline_history_insert(strdup(lines[i]));
			rline_scroll = 0;
			free(lines[i]);
		}
		FREE_ARRAY(char *, lines, lineCapacity);
		if (valid) {
			KrkValue result = krk_interpret(allData, "<stdin>");
			krk_printResult(result);
			krk_resetStack();
		}
		(void)blockWidth;
	}
}
#else

/* repl without rline */
void krk_repl(void) {
	krk_exitRepl = 0;
	while (!krk_exitRepl) {
		size_t lineCapacity = 8;
		size_t lineCount = 0;
		char ** lines = ALLOCATE(char *, lineCapacity);
		size_t totalData = 0;
		int valid = 1;
		char * allData = NULL;
		int inBlock = 0;
		int blockWidth = 0;

		while (1) {
			char buf[4096] = {0};
			print(">>> ");
			readline("", buf, 4096);
			if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) {
				/* We should actually be properly raising the interrupt and printing it with an empty traceback, but whatever. */
				krk_currentThread.flags &= ~(KRK_THREAD_SIGNALLED); /* Clear signal flag */
				printf("KeyboardInterrupt\n");
				valid = 0;
				break;
			}
			if (buf[strlen(buf)-1] != '\n') {
				valid = 0;
				break;
			}
			if (lineCapacity < lineCount + 1) {
				size_t old = lineCapacity;
				lineCapacity = GROW_CAPACITY(old);
				lines = GROW_ARRAY(char *,lines,old,lineCapacity);
			}
			int i = lineCount++;
			lines[i] = strdup(buf);
			size_t lineLength = strlen(lines[i]);
			totalData += lineLength;
			int isSpaces = 1;
			int countSpaces = 0;
			for (size_t j = 0; j < lineLength; ++j) {
				if (lines[i][j] != ' ' && lines[i][j] != '\n') {
					isSpaces = 0;
					break;
				}
				countSpaces += 1;
			}
			if (lineLength > 1 && lines[i][lineLength-2] == ':') {
				inBlock = 1;
				blockWidth = countSpaces + 4;
				continue;
			} else if (lineLength > 1 && lines[i][lineLength-2] == '\\') {
				inBlock = 1;
				continue;
			} else if (inBlock && lineLength != 1) {
				if (isSpaces) {
					free(lines[i]);
					totalData -= lineLength;
					lineCount--;
					break;
				}
				blockWidth = countSpaces;
				continue;
			} else if (lineLength > 1 && lines[i][countSpaces] == '@') {
				inBlock = 1;
				blockWidth = countSpaces;
				continue;
			}
			if (isSpaces && !i) valid = 0;
			break;
		}
		if (valid) {
			allData = malloc(totalData + 1);
			allData[0] = '\0';
		}
		for (size_t i = 0; i < lineCount; ++i) {
			if (valid) strcat(allData, lines[i]);
			free(lines[i]);
		}
		FREE_ARRAY(char *, lines, lineCapacity);
		if (valid) {
			KrkValue result = krk_interpret(allData, "<stdin>");
			krk_printResult(result);
			krk_resetStack();
		}
		(void)blockWidth;
	}
}

#endif
