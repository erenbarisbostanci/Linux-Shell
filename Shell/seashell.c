#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>

const char* sysname = "seashell";

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};
struct command_t {
	char* name;
	bool background;
	bool auto_complete;
	int arg_count;
	char** args;
	char* redirects[3]; // in/out redirection
	struct command_t* next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t* command)
{
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");
	for (i = 0;i < 3;i++)
		printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i = 0;i < command->arg_count;++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}


}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t* command)
{
	if (command->arg_count)
	{
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i = 0;i < 3;++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next = NULL;
	}
	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("\033[1;32m%s@%s\033[0m:\033[1;34m%s\033[0m \033[1;36m%s\033[0m\033[1;33m$\033[0m ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char* buf, struct command_t* command)
{
	const char* splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);
	while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
		buf[--len] = 0; // trim right whitespace

	if (len > 0 && buf[len - 1] == '?') // auto-complete
		command->auto_complete = true;
	if (len > 0 && buf[len - 1] == '&') // background
		command->background = true;

	char* pch = strtok(buf, splitters);
	command->name = (char*)malloc(strlen(pch) + 1);
	if (pch == NULL)
		command->name[0] = 0;
	else
		strcpy(command->name, pch);

	command->args = (char**)malloc(sizeof(char*));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], * arg;
	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch) break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		if (len == 0) continue; // empty arg, go for next
		while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL) arg[--len] = 0; // trim right whitespace
		if (len == 0) continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|") == 0)
		{
			struct command_t* c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t') index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0)
			continue; // handled before

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<')
			redirect_index = 0;
		if (arg[0] == '>')
		{
			if (len > 1 && arg[1] == '>')
			{
				redirect_index = 2;
				arg++;
				len--;
			}
			else redirect_index = 1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"')
			|| (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}
		command->args = (char**)realloc(command->args, sizeof(char*) * (arg_index + 1));
		command->args[arg_index] = (char*)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;
	return 0;
}

void prompt_backspace()
{
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t* command)
{
	int index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


	//FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state = 0;
	buf[0] = 0;
	while (1)
	{
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c == 9) // handle tab
		{
			buf[index++] = '?'; // autocomplete
			break;
		}

		if (c == 127) // handle backspace
		{
			if (index > 0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c == 27 && multicode_state == 0) // handle multi-code keys
		{
			multicode_state = 1;
			continue;
		}
		if (c == 91 && multicode_state == 1)
		{
			multicode_state = 2;
			continue;
		}
		if (c == 65 && multicode_state == 2) // up arrow
		{
			int i;
			while (index > 0)
			{
				prompt_backspace();
				index--;
			}
			for (i = 0;oldbuf[i];++i)
			{
				putchar(oldbuf[i]);
				buf[i] = oldbuf[i];
			}
			index = i;
			continue;
		}
		else
			multicode_state = 0;

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1) break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}
	if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
		index--;
	buf[index++] = 0; // null terminate string

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	// print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t* command);

struct command_t* old_command;

FILE* fp;
FILE* fp2;
char name[100];
char name2[100];
char* tf = "/shortdir_memory.txt";
char* tf2 = "/tmp.txt";
char cwd[100];

int main()
{
	getcwd(name, sizeof(name));
	strcat(name, tf);

	getcwd(name2, sizeof(name2));
	strcat(name2, tf2);
	while (1)
	{
		struct command_t* command = malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code == EXIT) break;

		code = process_command(command);
		if (code == EXIT) break;

		if (strcmp(command->name, "!!") != 0) {
			if (old_command != NULL) {
				free_command(old_command);
			}
			old_command = command;
		}
	}

	printf("\n");
	return 0;
}

char* toLower(char* s) {
	for (char* p = s; *p; p++) *p = tolower(*p);
	return s;
}

int highlight(int argc, char* argv[]) {
	if (argc > 2) {
		fp = fopen(argv[2], "r");
		if (fp == NULL) {
			return SUCCESS;
		}

		toLower(argv[0]);

		char line[100];
		char word[50];
		while (fgets(line, 100, fp)) {
			char l2[100];
			memset(l2, '\0', sizeof(l2));
			strcpy(l2, strtok(line, "\r"));

			char* token = strtok(l2, " ");
			while (token != NULL) {
				memset(word, '\0', sizeof(word));
				strcpy(word, token);
				toLower(word);
				if ((strcmp(word, argv[0]) == 0)) {
					if (strcmp("r", argv[1]) == 0) {
						printf("\033[0;31m");
						printf("%s ", token);
						printf("\033[0m");
					}
					else if (strcmp("g", argv[1]) == 0) {
						printf("\033[0;32m");
						printf("%s ", token);
						printf("\033[0m");
					}
					else if (strcmp("b", argv[1]) == 0) {
						printf("\033[0;34m");
						printf("%s ", token);
						printf("\033[0m");
					}
				}
				else {
					printf("%s ", token);
				}
				token = strtok(NULL, " ");
			}
			printf("\n");
		}
		fclose(fp);
	}
	return SUCCESS;
}

int shortdirDelete(char* n) {
	fp = fopen(name, "r");
	if (!fp) {
		fp = fopen(name, "w");
		fclose(fp);
		fp = fopen(name, "r");
	}
	fp2 = fopen(name2, "w");
	if (!fp2)
		perror("fopen");

	char line[100];
	while (fgets(line, 100, fp)) {
		char l2[100];
		strcpy(l2, line);
		char* token = strtok(l2, ":");
		if (strcmp(token, n) != 0) {
			fputs(line, fp2);
		}
	}
	fclose(fp);
	fclose(fp2);

	fp = fopen(name, "w");
	if (!fp)
		perror("fopen");
	fp2 = fopen(name2, "r");
	if (!fp) {
		fp2 = fopen(name2, "w");
		fclose(fp);
		fp2 = fopen(name2, "r");
	}

	while (fgets(line, 100, fp2)) {
		fputs(line, fp);
	}
	fclose(fp);
	fclose(fp2);

	return SUCCESS;
}

int shortdir(int argc, char* argv[]) {

	if (argc == 2) {
		if (strcmp(argv[0], "set") == 0) {
			shortdirDelete(argv[1]);
			getcwd(cwd, sizeof(cwd));
			fp = fopen(name, "a");
			if (!fp) {
				fp = fopen(name, "w");
				fclose(fp);
				fp = fopen(name, "a");
			}
			char tmp[100];
			tmp[0] = '\0';
			strcat(tmp, argv[1]);
			strcat(tmp, ":");
			strcat(tmp, cwd);
			strcat(tmp, "\n");
			fputs(tmp, fp);
			fclose(fp);
			return SUCCESS;
		}
		else if (strcmp(argv[0], "jump") == 0) {
			fp = fopen(name, "r");
			if (!fp) {
				fp = fopen(name, "w");
				fclose(fp);
				fp = fopen(name, "r");
			}
			char line[100];

			while (fgets(line, 100, fp)) {
				char* token = strtok(line, ":");
				if (strcmp(token, argv[1]) == 0) {
					token = strtok(NULL, ":");
					token[strlen(token) - 1] = '\0';
					chdir(token);

					getcwd(cwd, sizeof(cwd));
					return SUCCESS;
				}
			}

			printf("There is no any shortdir called: %s\n", argv[1]);
			fclose(fp);
			return SUCCESS;
		}
		else if (strcmp(argv[0], "del") == 0) {
			return shortdirDelete(argv[1]);
		}
	}
	else if (argc == 1) {
		if (strcmp(argv[0], "clear") == 0) {
			fp = fopen(name, "w");
			if (!fp)
				perror("fopen");
			fclose(fp);
			return SUCCESS;
		}
		else if (strcmp(argv[0], "list") == 0) {
			fp = fopen(name, "r");
			if (!fp) {
				fp = fopen(name, "w");
				fclose(fp);
				fp = fopen(name, "r");
			}
			char line[100];

			while (fgets(line, 100, fp)) {
				char l[100];
				strcpy(l, line);
				char* token = strtok(l, ":");
				printf("%s -> ", token);
				token = strtok(NULL, ":");
				printf("%s\n", token);
			}
			fclose(fp);
			return SUCCESS;
		}

	}
	return EXIT;
}

void compare_bytes(FILE* fp1, FILE* fp2) {
	unsigned long pos;
	int c1, c2, bytes, bytes1, bytes2;
	for (pos = 0;; pos++) {
		c1 = getc(fp1);
		c2 = getc(fp2);
		if (c1 != c2 || c1 == EOF)
			break;
	}

	if (c1 == c2) {
		printf("The two files are identical\n");
	}
	else {
		for (bytes1 = 0; getc(fp1) != EOF; ++bytes1);
		for (bytes2 = 0; getc(fp2) != EOF; ++bytes2);
		bytes = bytes1 > bytes2 ? bytes1 : bytes2;
		printf("The two files are different in %lu bytes\n", bytes - pos);
		printf("file1 and file2 differ at position %lu: 0x%X <> 0x%X\n", pos, c1, c2);
	}
}

int kdiff(int argc, char* argv[]) {
	int ff = 1;
	int sf = 2;
	int i = 1, j = 1;
	if (argc == 2) {
		ff = 0;
		sf = 1;
	}
	char* ext = strchr(argv[ff], '.');
	if (ext != NULL) {
		i = strcmp(ext + 1, "txt");
	}
	char* ext1 = strchr(argv[sf], '.');
	if (ext != NULL) {
		j = strcmp(ext1 + 1, "txt");
	}

	if ((i != 0 || j != 0) && strcmp(argv[0], "-b") != 0) {
		printf("Try again\n");
		return SUCCESS;
	}
	if (argc >= 2) {

		int counter = 0;
		int line = 0;
		if (strcmp(argv[0],"-b") != 0) {
			if (i == 0 && j == 0) {
				fp = fopen(argv[ff], "r");
				fp2 = fopen(argv[sf], "r");
				if (fp == NULL || fp2 == NULL) {
					printf("Missing file\n");
					return SUCCESS;
				}
				char line1[100];
				char line2[100];
				while (fgets(line1, 100, fp) != NULL && fgets(line2, 100, fp2) != NULL) {
					line++;
					if (strcmp(line1, line2) != 0) {
						printf("%s:Line %d: %s\n", argv[ff], line, line1);
						printf("%s:Line %d: %s\n", argv[sf], line, line2);
						counter++;
					}
				}

				while (fgets(line1, 100, fp) != NULL) {
					line++;
					printf("%s:Line %d: %s\n", argv[ff], line, line1);
					counter++;
				}

				while (fgets(line2, 100, fp2) != NULL) {
					line++;
					printf("%s:Line %d: %s\n", argv[sf], line, line2);
					counter++;
				}

				if (counter != 0) {
					printf("%d different lines found\n", counter);
				}
				else {
					printf("The two files are identical\n");
				}
				fclose(fp);
				fclose(fp2);
			}
			else {
				printf("Error on kdiff\n");
			}
		}
		else if (strcmp(argv[0], "-b") == 0) {
			fp = fopen(argv[1], "rb");
			fp2 = fopen(argv[2], "rb");
			if (fp == NULL || fp2 == NULL) {
				printf("Missing file\n");
				return SUCCESS;
			}
			compare_bytes(fp, fp2);
			fclose(fp);
			fclose(fp2);
		}


	}
	else {
		printf("Error on kdiff\n");
	}

	return SUCCESS;
}

int donkeySay(int argc, char* argv[]) {
	if (argc < 1) {
		return SUCCESS;
	}
	printf(" ____________\n");
	printf("< %s >\n", argv[0]);
	printf(" ------------\n");
	printf("        \\   ^__^\n");
	printf("         \\  (oo)\\_______\n");
	printf("            (__)\\       )\\/\\\n");
	printf("                ||-----||\n");
	printf("                ||     ||\n");
	printf("============================\n");
	printf("COMP304COMP304COMP304COMP304\n");
	return SUCCESS;
}

int highLowGame(int argc, char* argv[]) {
	char input[20];
	memset(input, '\0', sizeof(input));
	int coin = 50;
	int bet = 2;
	srand(time(NULL));
	int start = 1;
	int toCompare = rand() % 10 + 1;
	int compared = 5;
	while (strcmp(input, "exit\n") != 0 && coin > 0)
	{
		if (strcmp(input, "change bet\n") == 0) {
			printf("Please enter your desired bet: ");
			scanf("%d", &bet);
			printf("Enter your guess: ");
			scanf("%s", input);
			coin = coin + bet;
		}
		else {
			printf("\033[2J\n");
			if (!start) {
				printf("The number was %d\n", toCompare);
			}
			if (start) {
				printf("Guess high or low\n");
				printf("If you guess right, you win 2 coins else you lose 2 coins\n");
				start = 0;
			}
			else if (compared > toCompare && strcmp(input, "high\n") == 0) {
				printf("You guess right!\n");
				coin = coin + bet;
			}
			else if (compared < toCompare && strcmp(input, "low\n") == 0) {
				printf("You guess right!\n");
				coin = coin + bet;
			}
			else {
				if (!start && strcmp(input, "change bet\n") != 0) {
					printf("You guess wrong!\n");
					coin = coin - bet;
				}
			}
			toCompare = compared;
			compared = rand() % 10 + 1;
			printf("Coin: %d\n", coin);
			printf("Bet: %d\n", bet);

			printf("Number %d\n", toCompare);
			printf("Enter your guess: ");
			fgets(input, 99, stdin);
		}
	}
	if (coin <= 0) {
		printf("Bankrupt coin: %d\n", coin);
	}
	printf("End of the Game\n");
	return SUCCESS;
}



int process_command(struct command_t* command)
{
	int r;
	if (strcmp(command->name, "") == 0) return SUCCESS;

	if (strcmp(command->name, "exit") == 0)
		return EXIT;

	if (strcmp(command->name, "cd") == 0)
	{
		if (command->arg_count > 0)
		{
			r = chdir(command->args[0]);
			if (r == -1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			return SUCCESS;
		}
	}

	if (strcmp(command->name, "shortdir") == 0)
	{
		return shortdir(command->arg_count, command->args);
	}

	if (strcmp(command->name, "kdiff") == 0) 
	{
		return kdiff(command->arg_count, command->args);
	}

	if (strcmp(command->name, "highlight") == 0)
	{
		return highlight(command->arg_count, command->args);
	}

	if (strcmp(command->name, "donkey_say") == 0)
	{
		return donkeySay(command->arg_count, command->args);
	}

	if (strcmp(command->name, "game") == 0)
	{
		return highLowGame(command->arg_count, command->args);
	}

	if (strcmp(command->name, "!!") == 0)
	{
		if (old_command == NULL) {
			printf("No commands on history\n");
		}
		else {
			return process_command(old_command);
		}
	}

	pid_t pid = fork();
	if (pid == 0) // child
	{

		if (strcmp(command->name, "goodMorning") == 0)
		{
			char* time = command->args[0];

			fp = fopen("command.txt", "w");

			char* hour = strtok(command->args[0], ".");
			char* minute = strtok(NULL, ".");

			fputs(minute, fp);
			fputs(" ", fp);
			fputs(hour, fp);
			fputs(" * * * XDG_RUNTIME_DIR=/run/user/$(id -u) rhythmbox-client ", fp);
			fputs(command->args[1], fp);
			fputs(" --play\n", fp);
			fclose(fp);

			char** argss;
			argss = (char**)malloc(sizeof(char*) * 3 + sizeof(char) * 100 * 3);
			char* ptr = (char*)(argss + 100);

			for (int i = 0; i < 3; i++) {
				argss[i] = (ptr + 100 * i);
			}

			strcpy(argss[0], "crontab");
			strcpy(argss[1], "command.txt");
			argss[2] = NULL;

			execvp("crontab", argss);
		}

		if (command->redirects[0] != NULL) {
			int fd = open(command->redirects[0], O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			dup2(fd, 0);
			close(fd);
		}
		if (command->redirects[1] != NULL) {
			int fd = open(command->redirects[1], O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			dup2(fd, 1);
			close(fd);
		}
		else if (command->redirects[2] != NULL) {
			int fd = open(command->redirects[2], O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			dup2(fd, 1);
			close(fd);
		}

		// increase args size by 2
		command->args = (char**)realloc(
			command->args, sizeof(char*) * (command->arg_count += 2));

		// shift everything forward by 1
		for (int i = command->arg_count - 2;i > 0;--i)
			command->args[i] = command->args[i - 1];

		// set args[0] as a copy of name
		command->args[0] = strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count - 1] = NULL;

		char* pth = "/bin/";
		int size = strlen(pth) + strlen(command->name) + 1;

		char path[size];
		strcpy(path, pth);
		strcat(path, command->name);
		path[size] = '\0';

		execv(path, command->args);
		exit(0);
	}
	else
	{
		if (!command->background)
			wait(0); // wait for child process to finish

		if (command->next != NULL) {

			return process_command(command->next);
		}
		return SUCCESS;
	}

	// TODO: your implementation here

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}
