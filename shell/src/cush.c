/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <readline/history.h>
#include "../posix_spawn/spawn.h"

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"


static void handle_child_status(pid_t pid, int status);

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    char* buffer = malloc(1000*sizeof(char));
    char* cwd = malloc(1000*sizeof(char));
    if(getcwd(cwd, 1000*sizeof(char)) == NULL){
        printf("getcwd failed\n");
    }
    cwd = strrchr(cwd, '/')+1;
    if(cwd == NULL){
        printf("cwd failed\n");
    }
    char* hostname = malloc(1000*sizeof(char));
    char* username = malloc(1000*sizeof(char));
    if(getlogin_r(username, 1000*sizeof(char)) != 0){
        printf("getlogin failed\n");
    }
    if(gethostname(hostname, 1000*sizeof(char)) != 0){
        printf("gethostname failed\n");
    }
    
    int returnval = snprintf(buffer, 1000, "%s@%s in %s> ", username, hostname, cwd);
    if(returnval < 0){
        printf("snprintf failed\n");
    }
    return strdup(buffer);
    // return strdup("cush> ");
}

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The state of the terminal when this job was 
                                        stopped after having been in foreground */

    /* Add additional fields here if needed. */
    pid_t pgid;     /* PGID . */
    pid_t * pid_array;
    bool has_saved_tty;
    int pid_counter;
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job * jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job * 
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}



static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));
    struct job *curr_job = NULL;
    for (struct list_elem * job_elem = list_begin(&job_list);   //loop through job list
    job_elem != list_end(&job_list);
    job_elem = list_next(job_elem)){
        struct job *list_job = list_entry(job_elem, struct job, elem);  //get each job
        //loop through pid array
        for (int k=0; k<list_job->pid_counter; k++){
            if(list_job->pid_array[k] == pid){   //compare pid in the job's pid list to the pid we are looking for
                curr_job = list_job;
                break;      //exit loop when found
            }
        }
    }

    if(curr_job == NULL){
        printf("job not found :(\n");
        exit(0);
    }
    // Step 2. Determine what status change occurred using the WIF*() macros.
    // // Step 3. Update the job status accordingly, and adjust num_processes_alive if appropriate. 
    // // If a process was stopped, save the terminal state.
    if(WIFSTOPPED(status)){
        if(WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN){
            curr_job->status = NEEDSTERMINAL;
        }
        else if(WSTOPSIG(status) == SIGTSTP || WSTOPSIG(status) == SIGSTOP){
            termstate_save(&curr_job->saved_tty_state);
            curr_job->status = STOPPED;
            print_job(curr_job);
            curr_job->has_saved_tty = true;
        }
    }
    else if(WIFEXITED(status)){
        curr_job->num_processes_alive--;
    }
    else if(WIFSIGNALED(status)){
        curr_job->num_processes_alive--;
        if(WTERMSIG(status)==SIGFPE){
            printf("floating point exception");
        }
        else if(WTERMSIG(status) == SIGSEGV){
            printf("segmentation fault");
        }
        else if(WTERMSIG(status) == SIGABRT){
            printf("aborted");
        }
        else if(WTERMSIG(status) == SIGKILL){
            printf("killed");
        }
        else if(WTERMSIG(status) == SIGTERM){
            printf("terminated");
        }
        else{
            printf("unknown signal");
        }
    }
    termstate_give_terminal_back_to_shell();
}

//removes all jobs with no more processes alive from the job list
//also deletes the job
static void clean_jobs_list(){
    struct list_elem * job_elem = list_begin(&job_list);
    while (job_elem != list_end(&job_list)){
        struct job *list_job = list_entry(job_elem, struct job, elem);
        if(list_job->num_processes_alive==0){
            job_elem = list_remove(job_elem);
            delete_job(list_job);
        }
        else{
            job_elem = list_next(job_elem);
        }
    }
}

int
main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    //start history session
    using_history();
    char* expand = malloc(sizeof(char*));
    /* Read/eval loop. */
    for (;;) {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char * prompt = isatty(0) ? build_prompt() : NULL;
        char * cmdline = readline(prompt);
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;

        if(history_expand(cmdline, &expand) == 1){
            cmdline = expand;
        }
        add_history(cmdline);

        struct ast_command_line * cline = ast_parse_command_line(cmdline);
        free (cmdline);
        if (cline == NULL)                  /* Error in command line */
            continue;

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }

        // ast_command_line_print(cline);      /* Output a representation of
                                            //    the entered command line */

        signal_block(SIGCHLD);
        bool is_builtin_flag = false;
        //loop through command line struct (terminal input)
        for (struct list_elem * command_line_elem = list_begin (&cline->pipes); 
         command_line_elem != list_end (&cline->pipes); 
         command_line_elem = list_remove(command_line_elem)) {
            struct ast_pipeline *pipe = list_entry(command_line_elem, struct ast_pipeline, elem);
            struct job *added_job = NULL;
            bool spawn_success = true;
            int pipeinput[2] = {0, 0};
            int pipeoutput[2] = {0, 0};
            //loop through pipeline struct (terminal input)
            for (struct list_elem * pipeline_elem = list_begin(&pipe->commands); 
                pipeline_elem != list_end(&pipe->commands); 
                pipeline_elem = list_next(pipeline_elem)) {
                struct ast_command *cmd = list_entry(pipeline_elem, struct ast_command, elem);
                char **p = cmd->argv;
                //look at commands (terminal input)
                if(strcmp(p[0], "jobs")==0){          //jobs built-in command
                    is_builtin_flag = true;
                    //loop through job_list and print each job
                    for (struct list_elem * job_list_elem = list_begin(&job_list); 
                    job_list_elem != list_end(&job_list);
                    job_list_elem = list_next(job_list_elem)){
                        struct job *job_in_list = list_entry(job_list_elem, struct job, elem);
                        print_job(job_in_list);
                    }
                } 
                else if(strcmp(p[0], "kill")==0){      //kill built-in command
                    is_builtin_flag = true;
                    if(p[1] == NULL){
                        printf("job id missing\n");
                    }
                    struct job * kill_job = get_job_from_jid(atoi(p[1]));
                    if(kill_job == NULL){
                        printf("No such job\n");
                    }
                    //loop through child pids then kill all child pids
                    for(int k = 0; k < kill_job->num_processes_alive; k++){
                        if(kill(kill_job->pid_array[k], SIGTERM) != 0){
                            printf("error detected");
                        };
                    }
                    //then kill the pgid
                    if(kill(kill_job->pgid, SIGTERM) != 0){
                        printf("error detected");
                    }
                }
                else if(strcmp(p[0], "stop")==0){      //stop built-in command
                    is_builtin_flag = true;
                    if(p[1] == NULL){
                        printf("job id missing\n");
                    }
                    struct job * stop_job = get_job_from_jid(atoi(p[1]));
                    if(stop_job == NULL){
                        printf("No such job\n");
                    }
                    for(int k = 0; k < stop_job->num_processes_alive; k++){
                        if(kill(stop_job->pid_array[k], SIGSTOP) != 0){
                            printf("error detected");
                        }
                    }
                    stop_job->status = STOPPED;
                    if(kill(stop_job->pgid, SIGSTOP)!=0){
                        printf("stop failed\n");
                    }  //kill the entire process group
                }
                else if(strcmp(p[0], "exit")==0){      //exit built-in command
                    is_builtin_flag = true;
                    exit(EXIT_SUCCESS);
                }
                else if(strcmp(p[0], "fg")==0){     //fg built-in command
                    is_builtin_flag = true;
                    struct job *fg_job = get_job_from_jid(atoi(p[1]));
                    
                    if(fg_job->has_saved_tty == true){
                        termstate_give_terminal_to(&fg_job->saved_tty_state, fg_job->pgid);
                    }
                    else{
                    termstate_give_terminal_to(NULL, fg_job->pgid);
                    }
                    if(fg_job->status == STOPPED){
                        if(killpg(fg_job->pgid, SIGCONT) != 0){
                            printf("error detected");
                        }
                    }
                    if(fg_job->status == NEEDSTERMINAL){
                        tcsetpgrp(termstate_get_tty_fd(), fg_job->pgid);
                        if(killpg(fg_job->pgid, SIGCONT) != 0){
                            printf("error detected");
                        }
                    }
                    fg_job->status = FOREGROUND;
                    print_cmdline(fg_job->pipe);
                    printf("\n");
                    
                    wait_for_job(fg_job);
                }
                else if(strcmp(p[0], "bg")==0){     //bg built-in command
                    is_builtin_flag = true;
                    struct job *bg_job = get_job_from_jid(atoi(p[1]));
                    if(bg_job->status == STOPPED){
                        if(killpg(bg_job->pgid, SIGCONT) != 0){
                            printf("error detected");
                        }
                        bg_job->status = BACKGROUND; //how to change from current state to running
                    }
                    printf("[%d] %d\n", bg_job->jid, bg_job->pgid);
                }
                else if(strcmp(p[0], "history")==0){
                    is_builtin_flag = true;
                    HISTORY_STATE *history = history_get_history_state();
                    for(int k=0; k<history->length; k++){
                        printf("%d  %s\n", k+1, history->entries[k]->line);
                    }
                }
                //if not a built-in command, posix spawn and add to job list
                else{
                    if(pipeline_elem == list_begin(&pipe->commands)){//only add job for first process in pipe
                        added_job = add_job(pipe);  //add job
                        added_job->pid_array = malloc(list_size(&pipe->commands)*sizeof(pid_t));
                        added_job->pid_counter = 0;
                        added_job->has_saved_tty = false;
                        if(pipe->bg_job){   //set status
                            added_job->status=BACKGROUND;
                        }
                        else{
                            added_job->status=FOREGROUND;
                        }
                    }
                    
                    pid_t pid;
                    posix_spawnattr_t child_spawn_attr;
                    posix_spawn_file_actions_t child_file_attr;
                    posix_spawnattr_init(&child_spawn_attr);
                    posix_spawn_file_actions_init(&child_file_attr);
                    posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_TCSETPGROUP);
                    if(pipeline_elem == list_begin(&pipe->commands)){
                        posix_spawnattr_setpgroup(&child_spawn_attr, 0);//set process group
                    }
                    else{
                        posix_spawnattr_setpgroup(&child_spawn_attr, added_job->pgid);
                    }
                    //if fg:
                    if(added_job->status == FOREGROUND){
                        posix_spawnattr_tcsetpgrp_np(&child_spawn_attr, termstate_get_tty_fd());
                    }

                    //if they are NOT null, that means either a '<', '>', or '>>" was useed
                    //check for io input file ( < )
                    if(pipe->iored_input != NULL && pipeline_elem == list_begin(&pipe->commands)){
                        int psx_add_open_val = posix_spawn_file_actions_addopen(&child_file_attr, STDIN_FILENO, pipe->iored_input, O_RDONLY | O_CREAT, S_IRUSR);
                        if(psx_add_open_val != 0){
                            fprintf(stderr, "error: cannot open file\n");
                        }
                    }
                    //check for io output file
                    if(pipe->iored_output != NULL && list_next(pipeline_elem) == list_end(&pipe->commands)){
                        //append ( >> )
                        if(pipe->append_to_output){
                            int psx_add_open_val = posix_spawn_file_actions_addopen(&child_file_attr, STDOUT_FILENO, pipe->iored_output, O_WRONLY | O_CREAT | O_APPEND, S_IRWXU);
                            if(psx_add_open_val != 0){
                                fprintf(stderr, "error: cannot open file\n");
                            }
                        }
                        //overwrite ( > )
                        else{
                            int psx_add_open_val = posix_spawn_file_actions_addopen(&child_file_attr, STDOUT_FILENO, pipe->iored_output, O_WRONLY | O_CREAT, S_IRWXU);
                            if(psx_add_open_val != 0){
                                fprintf(stderr, "error: cannot open file\n");
                            }
                        }
                    }
                    
                    if(list_next(pipeline_elem) != list_end(&pipe->commands)){
                        int initial_pipe2 = pipe2(pipeoutput, O_CLOEXEC);
                        if(initial_pipe2 != 0){
                            printf("error detected");
                        }
                        posix_spawn_file_actions_adddup2(&child_file_attr, pipeoutput[1], STDOUT_FILENO);
                    }

                    if(pipeline_elem != list_begin(&pipe->commands)){
                        posix_spawn_file_actions_adddup2(&child_file_attr, pipeinput[0], STDIN_FILENO);
                    }

                    if(cmd->dup_stderr_to_stdout){  //also redirect stderr
                        posix_spawn_file_actions_adddup2(&child_file_attr, STDOUT_FILENO, STDERR_FILENO);
                    }

                    extern char **environ;
                    int spawned = posix_spawnp(&pid, p[0], &child_file_attr, &child_spawn_attr, p, environ);
                    if(spawned != 0){
                        spawn_success = false;
                        errno = spawned;
                        perror("Spawning: ");
                    }

                    if(pipeline_elem != list_begin(&pipe->commands)){
                        int close_pipe1 = close(pipeinput[0]); //
                        if (close_pipe1 != 0){
                            printf("error detected");
                        }
                        int close_pipe2 = close(pipeinput[1]); //
                        if (close_pipe2 != 0){
                            printf("error detected");
                        }
                    }

                    pipeinput[0]=pipeoutput[0];
                    pipeinput[1]=pipeoutput[1];

                    //print jid and pid if it is a background process
                    if(added_job->status == BACKGROUND){
                        printf("[%d] %d\n", added_job->jid, pid);
                    }

                    // add pid to job's pid array
                    added_job->pid_counter++;
                    added_job->pid_array[added_job->num_processes_alive] = pid;
                    added_job->num_processes_alive = added_job->num_processes_alive + 1;

                    if(pipeline_elem == list_begin(&pipe->commands)){
                        added_job->pgid=pid; //store pgid of first process of the job
                    }
                }
            }
            if(!is_builtin_flag && spawn_success){ //if posix spawn works with given commands and it is not a built-in command
                if(added_job->status == FOREGROUND){
                    wait_for_job(added_job);
                }
                termstate_give_terminal_back_to_shell();
            }
            if(!spawn_success){ //if posix spawn fails
                list_remove(&added_job->elem);
                termstate_give_terminal_back_to_shell();
            }
            clean_jobs_list();      //remove all jobs from jobs list that have no more processes alive
        }
        signal_unblock(SIGCHLD);


        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        ast_command_line_free(cline);
    }
    return 0;
}