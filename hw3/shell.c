#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

/* PATH env variable to hold search paths for programs */
char *PATH;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_export(struct tokens *tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_pwd, "pwd", "show the current working directory"},
  {cmd_cd, "cd", "change current working directory to the first argument"},
  {cmd_export, "export", "export PATH to add directory to program search index"},
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

int cmd_pwd(unused struct tokens *tokens) {
  char cwd[100];
   if (getcwd(cwd, sizeof(cwd)) != NULL) {
       printf("%s\n", cwd);
   } else {
       perror("getcwd() error");
       return 1;
   }
   return 0;
}

int cmd_cd(unused struct tokens *tokens) {
  assert(strcmp(tokens_get_token(tokens, 0), "cd") == 0);
  char *path = tokens_get_token(tokens, 1);
  printf("Changing working dir to %s\n", path);
  if (chdir(path) != 0) {
    perror("chdir() failed.");
    return 1;
  } else {
    return 0;
  }
}

int cmd_export(unused struct tokens *tokens) {
  assert(strcmp(tokens_get_token(tokens, 0), "export") == 0);
  char *arg = tokens_get_token(tokens, 1);
  char *path = strtok(arg, ":");
  path = strtok(path, "=");
  path = strtok(NULL, "=");
  char *const_colon = ":";
  PATH = strcat(strcat(path, const_colon), PATH);
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  // PATH initialization
  PATH = "/usr/bin";

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

bool is_in_path(const char *prog, const char *path) {
  DIR *dir;
  struct dirent *ent;
  if ((dir = opendir(path)) != NULL) {
    while ((ent = readdir(dir)) != NULL) {
      if (strcmp(ent->d_name, prog) == 0) {
        closedir(dir);
        return true;
      }
    }
    closedir(dir);
    return false;
  } else {
    perror("Error happened in opening path");
    return false;
  }
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      char *args[10]; // 10 args at most
      char *in_file = NULL; // store usr provided file for redirecting stdin stdout if provided
      char *out_file = NULL;
      int position = 0;
      char *token;
      while ((token = tokens_get_token(tokens, position)) != NULL) {
        if (strcmp(token, ">") == 0) {
          out_file = tokens_get_token(tokens, ++position);
          printf("outfile: %s\n", out_file);
          // position++;
          // continue;
          break;
        } else if (strcmp(token, "<") == 0) {
          in_file = tokens_get_token(tokens, ++position);
          printf("infile: %s\n", in_file);
          args[position++] = "<";
          args[position++] = in_file;
          // position++;
          // continue;
          break;
        }
        args[position++] = token;
        printf("added token to args: %s\n", token);
      }
      args[position] = NULL;  // must add NULL to the end of args!! :(
      // process the first arg (path) if resolving to finding program in PATH
      if (!(strlen(args[0]) > 0 && args[0][0] == '/')) {
        char *path = strtok(PATH, ":");
        while (path) {
          if (is_in_path(args[0], path)) {
            char *const_str = "/";
            char *program = malloc(strlen(path) + 2 + strlen(args[0]));
            strcpy(program, path);
            strcat(program, const_str);
            strcat(program, args[0]);
            args[0] = program;
            break;
          }
          path = strtok(NULL, ":");
        }
      }
      int pid = fork();
      if (pid == 0) {
        // child process
        if (out_file) {
          FILE *new_fd = fopen(out_file, "wb");
          dup2(fileno(new_fd), STDOUT_FILENO);
          fclose(new_fd);
          printf("@@ sharon out dup2 ok.\n");
        }
        if (in_file) {
          fclose(in_file);
          FILE *new_fd = fopen(in_file, "rb");
          dup2(fileno(new_fd), STDIN_FILENO);
          fclose(new_fd);
          printf("@@ sharon in dup2 ok.\n");
        }
        
        if (execv(args[0], args) != 0) {
          perror("Program failed.");
        }
        exit(0);
      } else {
        // parent process
        wait(pid);
      }
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
