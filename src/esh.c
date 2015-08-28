/*
 * esh - the 'pluggable' shell.
 *
 * Developed by Godmar Back for CS 3214 Fall 2009
 * Virginia Tech.
 */
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "esh.h"

struct list jobs_list; //The jobs list
struct termios *shell_termios = NULL; //The status of the shell

/**
 * Determines if the command entered by the user is a function built into the
 * shell itself. If so it will execute the builtin function, otherwise it will
 * attempt to execute the command entered by the user.
 *
 * cmd - The command entered by the user
 * Return :
    0 - The command is not builtin
    1 - The command is builtin
**/
static bool esh_isBuiltIn(char *cmd)
{
    if ((strcmp(cmd, "kill") == 0) || (strcmp(cmd, "stop") == 0)
        ||strcmp(cmd, "jobs") == 0 || (strcmp(cmd, "fg") == 0)
        ||strcmp(cmd, "bg") == 0)
        return 1;
    return 0;
}

/**
 * A sighandler that is implemented to handle interceptions of the
 * SIGCHLD signal. Based off of how the signal was intercepted, it will
 * determine what should be done with the signal.
 *
 * sig - The signal to intercept
 * siginfo - Information about the signal
 * ptr - Callback
**/
static void esh_sighandler(int sig, siginfo_t *siginfo, void *ptr)
{
    int child_status;
    pid_t pid;
    while ((pid = waitpid(-1, &child_status, WUNTRACED | WNOHANG)) > 0)
    {
        if (WIFEXITED(child_status)) //If child exited normally
        {
            struct list_elem *j = list_begin(&jobs_list);
            for (; j != list_end(&jobs_list); j = list_next(j))
            {
                struct esh_pipeline *pipe = list_entry(j, struct esh_pipeline, elem);
                if (pipe->pgrp == pid)
                {
                    //Notify the user if a background process ended
                    if (pipe->status != FOREGROUND)
                    {
                        printf("\n[%d] DONE\n", pipe->jid);
                        list_remove(j);
                        break;
                    }
                    else
                    {
                        list_remove(j);
                    }
                }
            }
        }
        else
        {
            if (WIFSTOPPED(child_status)) //If the child process was stopped (ex. ^Z)
            {
                //Set job to stopped
                struct list_elem *j = list_begin(&jobs_list);
                for (; j != list_end(&jobs_list); j = list_next(j))
                {
                    struct esh_pipeline *pipe = list_entry(j, struct esh_pipeline, elem);
                    if (pipe->pgrp == pid)
                    {
                        pipe->status = STOPPED;
                        break;
                    }
                }
            }
        }
    }
}

/**
 * Handles certain situations just like the SIGCHLD handler except it is used
 * when the SIGCHLD handler needs to be blocked for special occasions. Mainly
 * for when the user issues ^Z.
 *
 * status - The status of the process
 * pid - The pid of the process
**/
static void possible_job_update(int status, pid_t pid)
{
    if (WIFEXITED(status)) //The child process exits normally
    {
        struct list_elem *j = list_begin(&jobs_list);
        for (; j != list_end(&jobs_list); j = list_next(j))
        {
            struct esh_pipeline *pipe = list_entry(j, struct esh_pipeline, elem);
            if (pipe->pgrp == pid)
            {
                //Notify the user if it was a background process
                if (pipe->status != FOREGROUND)
                {
                    printf("\n[%d] DONE\n", pipe->jid);
                    list_remove(j);
                    break;
                }
                else
                {
                    list_remove(j);
                }
            }
        }
    }
    else
    {
        if (WIFSTOPPED(status)) //The child process is stopped (ex. ^Z)
        {
            struct list_elem *j = list_begin(&jobs_list);
            for (; j != list_end(&jobs_list); j = list_next(j))
            {
                struct esh_pipeline *pipe = list_entry(j, struct esh_pipeline, elem);
                struct list_elem *c = list_begin (&pipe->commands);
                struct esh_command *cmd = list_entry(c, struct esh_command, elem);
                if (pipe->pgrp == pid)
                {
                    //Set job to stopped
                    pipe->status = STOPPED;
                    printf("[%d] Stopped   (%s %s)\n", pipe->jid, cmd->argv[0], cmd->argv[1]);
                    break;
                }
            }
        }
    }
}

/**
 * Helper function to give the terminal to a specific process. Handles all the
 * necessary blocks and tcsetpgrp's that are necessary to safely hand the terminal
 * over.
 *
 * Code Snippet provided by the instructors of CS3214 on the CS3214 website
 *
 * pgrp - The process group to hand it over to
 * pg_tty_state - The state of the terminal
**/
static void give_terminal_to(pid_t pgrp, struct termios *pg_tty_state)
{
  esh_signal_block(SIGTTOU);
  int rc = tcsetpgrp(esh_sys_tty_getfd(), pgrp);
  if (rc == -1)
    esh_sys_fatal_error("tcsetpgrp: ");

  if (pg_tty_state)
    esh_sys_tty_restore(pg_tty_state);
  esh_signal_unblock(SIGTTOU);
}

/**
 * Kills the job with the given jobID. Essentially sends a SIGTERM to all processes
 * in the jobID group that way they can safely terminate. Will also remove the job
 * from the job list.
 *
 * jobID - The id of the job to terminate
**/
static void killJob(int jobID)
{
    struct list_elem *e = list_begin (&jobs_list);

    for(; e != list_end(&jobs_list); e = list_next(e))
    {
        struct esh_pipeline *job = list_entry(e, struct esh_pipeline, elem);
        if (job->jid == jobID)
        {
            if (kill(-(job->pgrp), SIGTERM) < 0)
                esh_sys_fatal_error("Error kill: killJob SIGTERM Error");
            list_remove(e);
            break;
        }
    }
}

/**
 * Stops the job with the given jobID. It will send a SIGSTOP to the given process
 * group. These processes can later be continued by using the fg and bg commands
 * builtin to esh.
 *
 * jobID - The id of the job to stop
**/
static void stopJob(int jobID)
{
    struct list_elem *e = list_begin (&jobs_list);

    for(; e != list_end(&jobs_list); e = list_next(e))
    {
        struct esh_pipeline *job = list_entry(e, struct esh_pipeline, elem);
        if (job->jid == jobID)
        {
            job->status = BACKGROUND;
            if (kill(-(job->pgrp), SIGSTOP) < 0)
                esh_sys_fatal_error("Error stop: stopJob SIGSTOP Error");
            break;
        }
    }
}

/**
 * Shows the user the list of all jobs that are currently running and stopped
 * by the terminal. They are provided the jobid, the status of the process and
 * what command was entered for that process.
 *
 *
**/
static void showJobs()
{
    struct list_elem *e = list_begin (&jobs_list);

    for(; e != list_end(&jobs_list); e = list_next(e))
    {
        struct esh_pipeline *job = list_entry(e, struct esh_pipeline, elem);
        struct list_elem *c = list_begin (&job->commands);
        struct esh_command *cmd = list_entry(c, struct esh_command, elem);

        if (job->status == (FOREGROUND || BACKGROUND))
        {
            printf("[%d] Running   (%s %s)\n", job->jid, cmd->argv[0], cmd->argv[1]);
        }
        else if (job->status == STOPPED)
        {
            printf("[%d] Stopped   (%s %s)\n", job->jid, cmd->argv[0], cmd->argv[1]);
        }
    }
}

/**
 * Brings the job with the given jobID to the foreground. This process will then
 * take over control of the terminal until it is stopped, killed, or has finised
 * running.
 *
 * jobID - The id of the job to bring to the foreground
**/
static void fg(int jobID)
{
    struct list_elem *e = list_begin (&jobs_list);
    esh_signal_block(SIGCHLD);

    for(; e != list_end(&jobs_list); e = list_next(e))
    {
        struct esh_pipeline *job = list_entry(e, struct esh_pipeline, elem);
        struct list_elem *c = list_begin (&job->commands);
        struct esh_command *cmd = list_entry(c, struct esh_command, elem);

        if (job->jid == jobID)
        {
            printf("%s %s\n", cmd->argv[0], cmd->argv[1]);
            job->status = FOREGROUND;
            if (kill(-(job->pgrp), SIGCONT) < 0)
                esh_sys_fatal_error("Error fg: fg SIGCONT Error");
            give_terminal_to(job->pgrp, shell_termios);
            int status;
            pid_t id;
            if ((id = waitpid(job->pgrp, &status, WUNTRACED)) < 0)
            {
                printf("ERROR");
            }
            possible_job_update(status, id);
            give_terminal_to(getpgrp(), shell_termios);
            esh_signal_unblock(SIGCHLD);
            break;
        }
    }
}

/**
 * Sends the job with the given jobID a SIGCONT signal. This signal will tell
 * the process to continue running but it will be in the background that way the
 * user still has control of the shell.
 *
 * jobID - The id of the job to continue in the background
**/
static void bg(int jobID)
{
    struct list_elem *e = list_begin (&jobs_list);

    for(; e != list_end(&jobs_list); e = list_next(e))
    {
        struct esh_pipeline *job = list_entry(e, struct esh_pipeline, elem);
        struct list_elem *c = list_begin (&job->commands);
        struct esh_command *cmd = list_entry(c, struct esh_command, elem);

        if (job->jid == jobID)
        {
            job->status = BACKGROUND;
            if (kill(-(job->pgrp), SIGCONT) < 0)
                esh_sys_fatal_error("Error bg: bg SIGCONT Error");
            printf("[%d] %s\n", job->jid, cmd->argv[0]);
            break;
        }
    }
}

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n"
        " -p  plugindir directory from which to load plug-ins\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt by assembling fragments from loaded plugins that 
 * implement 'make_prompt.'
 *
 * This function demonstrates how to iterate over all loaded plugins.
 */
static char *
build_prompt_from_plugins(void)
{
    char *prompt = NULL;
    struct list_elem * e = list_begin(&esh_plugin_list);

    for (; e != list_end(&esh_plugin_list); e = list_next(e)) {
        struct esh_plugin *plugin = list_entry(e, struct esh_plugin, elem);

        if (plugin->make_prompt == NULL)
            continue;

        /* append prompt fragment created by plug-in */
        char * p = plugin->make_prompt();
        if (prompt == NULL) {
            prompt = p;
        } else {
            prompt = realloc(prompt, strlen(prompt) + strlen(p) + 1);
            strcat(prompt, p);
            free(p);
        }
    }

    /* default prompt */
    if (prompt == NULL)
        prompt = strdup("esh> ");

    return prompt;
}

/* The shell object plugins use.
 * Some methods are set to defaults.
 */
struct esh_shell shell =
{
    .build_prompt = build_prompt_from_plugins,
    .readline = readline,       /* GNU readline(3) */ 
    .parse_command_line = esh_parse_command_line /* Default parser */
};

int
main(int ac, char *av[])
{
    int opt;
    list_init(&esh_plugin_list);
    list_init(&jobs_list); //Initialize the jobs list

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "hp:")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;

        case 'p':
            esh_plugin_load_from_directory(optarg);
            break;
        }
    }

    esh_plugin_initialize(&shell);

    //Set the initial state of the shell, give it a pid, and hand the terminal
    //over to it
    shell_termios = esh_sys_tty_init();

    setpgid(0, 0);
    give_terminal_to(getpgrp(), shell_termios);

    int numJobs = 1;

    /* Read/eval loop. */
    for (;;) {
        /* Do not output a prompt unless shell's stdin is a terminal */
        char * prompt = isatty(0) ? shell.build_prompt() : NULL;
        char * cmdline = shell.readline(prompt);
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;

        struct esh_command_line * cline = shell.parse_command_line(cmdline);
        free (cmdline);
        if (cline == NULL)                  /* Error in command line */
            continue;

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            esh_command_line_free(cline);
            continue;
        }

/////////////////////////////////////////////////////////////

        esh_signal_sethandler(SIGCHLD, esh_sighandler);
     
        struct list_elem *e = list_begin (&cline->pipes);

        //If the job list is empty, the first job will be 1
        if (list_empty(&jobs_list))
        {
            numJobs = 1;
        }
        else //Otherwise we get the highest jid and add 1 to it
        {
            struct list_elem *j = list_back (&jobs_list);
            struct esh_pipeline *job = list_entry (j, struct esh_pipeline, elem);
            numJobs = job->jid + 1;
        }


        for (; e != list_end (&cline->pipes); e = list_next (e)) 
        {
            bool ranBuiltIn = false;
            bool ranPlugin = false;
            struct esh_pipeline *pipeline = list_entry(e, struct esh_pipeline, elem);

            pipeline->jid = numJobs++;
            pipeline->pgrp = -1;

            int pipe1[2]; //Our pipe
            int fd2 = 0; //A file descriptor to assist the pipe
            bool piped = false;
            int *pipeArray[50]; //A suitable size for the possible number of pipes
            int count = 0;

            //If we have more than 1 command, we are piped
            if (list_size(&pipeline->commands) > 1)
            {
                piped = true;
            }
                    
            struct list_elem * p = list_begin (&pipeline->commands);
            struct list_elem *cmdTail = list_tail(&pipeline->commands);
            pid_t pid;

            for (; p != list_end (&pipeline->commands); p = list_next (p)) 
            {
                struct esh_command *cmd = list_entry(p, struct esh_command, elem);

                struct list_elem *plug = list_begin(&esh_plugin_list);
                for (; plug != list_end(&esh_plugin_list); plug = list_next(plug))
                {
                    struct esh_plugin *plugin = list_entry (plug, struct esh_plugin, elem);

                    if (plugin->process_builtin(cmd))
                    {
                        ranPlugin = true;
                        continue;
                    }
                }

                if (!ranPlugin)
                {
                    //Check if command is builtin
                    if (esh_isBuiltIn(cmd->argv[0]))
                    {
                        /*
                        We are given a small number of builtin commands initially. Therefore
                        for now we can easily just do a simple if else loop through the builtin
                        commands to see which one was entered. If we were to add many more, we could
                        easily rewrite this to handle the expansion.
                        */
                        ranBuiltIn = true;
                        if (strcmp(cmd->argv[0], "kill") == 0)
                        {
                            if (cmd->argv[1] == NULL)
                            {
                                printf("kill: usage: kill jobid\n");
                            }
                            else
                            {
                                killJob(atoi(cmd->argv[1]));
                            }
                        }
                        else if (strcmp(cmd->argv[0], "stop") == 0)
                        {
                            if (cmd->argv[1] == NULL)
                            {
                                printf("stop: usage: stop jobid\n");
                            }
                            else
                            {
                                stopJob(atoi(cmd->argv[1]));
                            }
                        }
                        else if (strcmp(cmd->argv[0], "jobs") == 0)
                        {
                            showJobs();
                        }
                        else if (strcmp(cmd->argv[0], "bg") == 0)
                        {
                            if (cmd->argv[1] == NULL)
                            {
                                printf("bg: usage: bg jobid\n");
                            }
                            else
                            {
                                bg(atoi(cmd->argv[1]));
                            }
                        }
                        else if (strcmp(cmd->argv[0], "fg") == 0)
                        {
                            if (cmd->argv[1] == NULL)
                            {
                                printf("fg: usage: fg jobid\n");
                            }
                            else
                            {
                                fg(atoi(cmd->argv[1]));
                            }
                        }
                    }
                    else //We have to execute a command that isn't builtin
                    {
                        esh_signal_block(SIGCHLD); //BLOCK SIGCHLD
                        pipe(pipe1); //Setup our pipe

                        if ((pid = fork()) == 0) //FORK SUCCEEDS
                        {
                            pid = getpid();

                            if (pipeline->pgrp == -1)
                            {
                                pipeline->pgrp = pid;
                            }

                            if (setpgid(pid, pipeline->pgrp) < 0)
                            {
                                esh_sys_fatal_error("Error setpgid: Couldn't set process group in child");
                            }

                            esh_signal_unblock(SIGCHLD); //UNBLOCK SIGCHLD in child

                            //If there is input from a file
                            if (cmd->iored_input != NULL)
                            {
                                int fd0 = open(cmd->iored_input, O_RDONLY, 0);

                                if (dup2(fd0, STDIN_FILENO) < 0)
                                    esh_sys_fatal_error("Error dup2: Couldn't perform dup2 in input");

                                if (close(fd0) < 0)
                                    esh_sys_fatal_error("Error close: Couldn't close fd0");
                            }

                            //If we are outputting to a file
                            if (cmd->iored_output != NULL)
                            {
                                int fd1;

                                //Check if we are outputting or appending
                                if (cmd->append_to_output)
                                {
                                    fd1 = open(cmd->iored_output, O_CREAT|O_WRONLY|O_APPEND, S_IRWXU);    
                                }
                                else
                                {
                                    fd1 = open(cmd->iored_output, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
                                }

                                if(dup2(fd1, STDOUT_FILENO) < 0)
                                    esh_sys_fatal_error("Error dup2: Couldn't perform dup2 in output");

                                if (close(fd1) < 0)
                                    esh_sys_fatal_error("Error close: Couldn't close fd1");
                            }

                            //Everything else is necessary to pipe our commands together
                            if (dup2(fd2, 0) < 0)
                                esh_sys_fatal_error("Error dup2: Couldn't perform dup2 for piping");

                            if (list_next(p) != cmdTail && piped)
                            {
                                if (dup2(pipe1[1], 1) < 0)
                                    esh_sys_fatal_error("Error dup2: Couldn't perform dup2 for pipe1");
                            }

                            //Close all fd's before we execute the child process
                            if (close(pipe1[0]) < 0)
                                esh_sys_fatal_error("Error close: Couldn't close pipe1[0] in child");

                            if (close(pipe1[1]) < 0)
                                esh_sys_fatal_error("Error close: Couldn't close pipe1[1] in child");

                            if (execvp(cmd->argv[0], &cmd->argv[0]) < 0)
                            {
                                printf("%s: command not found\n", cmd->argv[0]);
                                exit(0);
                            }
                            exit(0);
                        }
                        else if (pid < 0) //The child process failed to fork
                        {
                            printf("There was an error forking the child process\n");
                            return -1;
                        }

                        //Close the write end of the pipe. Also add any fd's that will
                        //need to be closed at the end of the execution to our pipeArray.
                        if (close(pipe1[1]) < 0)
                            esh_sys_fatal_error("Error close: Couldn't close pipe1[1] in parent");

                        fd2 = pipe1[0];
                        int *i = malloc(sizeof(int));
                        *i = pipe1[0];
                        pipeArray[count] = i;

                        if (pipeline->pgrp == -1)
                        {
                            pipeline->pgrp = pid;
                        }

                        if (setpgid(pid, pipeline->pgrp) < 0)
                            esh_sys_fatal_error("Error setpgid: Couldn't set process group in parent");
                    }
                    count++;
                }
                else
                {
                    give_terminal_to(getpgrp(), shell_termios);
                }
            }

            //If we didn't run a builtin command, clean up all the file descriptors
            //by closing all the ones that were opened up
            int i;
            if (!ranBuiltIn)
            {
                for (i = 0; i < count; i++)
                {
                    if (close(*pipeArray[i]) < 0)
                        esh_sys_fatal_error("Error close: Couldn't close a pipe in the pipeArray");
                }
            }

            if (!ranBuiltIn && !ranPlugin)
            {
                if (!pipeline->bg_job)
                {
                    //Set job status to FOREGROUND and add to jobs list
                    pipeline->status = FOREGROUND;
                    e = list_pop_front(&cline->pipes);
                    list_push_back(&jobs_list, e);
                    e = list_begin(&cline->pipes);

                    //Hand the terminal over to the job
                    give_terminal_to(pipeline->pgrp, shell_termios);

                    //Wait for the job to finish running unless there is an interruption
                    int status;
		            pid_t id;

                    if ((id = waitpid(pid, &status, WUNTRACED)) < 0)
                    {
                        printf("ERROR");
                    }

                    //Make any changes to the jobs list if something happened
                    //while the SIGCHLD handler was blocked
		            possible_job_update(status, id);

                    //Hand the terminal back to the shell and unblock SIGCHLD
                    give_terminal_to(getpgrp(), shell_termios);

                    esh_signal_unblock(SIGCHLD);
                }
                else
                {
                    //If the process is in the background, add it to the job list
                    //and notify the user it is in the background
                    pipeline->status = BACKGROUND;
                    e = list_pop_front(&cline->pipes);
                    list_push_back(&jobs_list, e);
                    e = list_begin(&cline->pipes);
                    printf("[%d] %d\n", pipeline->jid, pipeline->pgrp);
                    esh_signal_unblock(SIGCHLD);
                }

                //Stop executing commands if we are at the end
                if (e == list_tail(&cline->pipes))
                {
                    break;
                }
            }
        }
    }
    return 0;
}