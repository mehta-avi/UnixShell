Student Information
-------------------
Avi Mehta 
Mark Hamilton 

How to execute the shell
------------------------
In src:
make
./cush

Important Notes
---------------
We pass all basic and advanced tests and have no valgrind errors

Description of Base Functionality
---------------------------------
jobs: Loop through the job_list and print each job

fg: Get the job with the given jid. Give it terminal access, and restore the state of the terminal if necessary.
If the job was stopped, continue it. If the job needs the terminal, make it the terminal's foreground process group.
Set the job's status to foreground, and print the job.

bg: Get the job with the given jid. If the job was stopped, continue the job.
Set the job's status to background, and print the job.

kill: Get the job with the given jid, then we iterate through
the children's pids of said job and kill them all before killing the pgid

stop: Get the job with the given jid, then we iterate through
the children's pids of said job and stop them all before stopping the pgid

\ˆC: Ctrl-C sends the SIGINT signal. We did not need to do anything for it to work.

\ˆZ: Ctrl-Z sends the SIGTSTP signal. We save the terminal state, set the job's status to stopped, 
and print the job.

Description of Extended Functionality
-------------------------------------
I/O: We use iored_input, iored_output, and append_to_output to check for <, > and  >>. 
We used addopen with the correct flags and mode to achieve the correct funtionality.

Pipes: We use two pipes which are continously swapped for each command until you reach the end.
We only create a pipe for before the process if it is not the first process.
We only create a pipe for after the process if it is not the last process.
For each process, the next pipe becomes the previous pipe, and both ends of the previous pipe are closed.
Then we use pipe2 to create the next pipe.
We also redirect stdout to stderr if necessary.

Exclusive Access: Within the case for fg, we check if the status of the current job is "NEEDSTERMINAL".
If so, we make the job's pgid the terminal's foreground process group.

List of Additional Builtins Implemented
---------------------------------------
Custom Built-in 1: Customizable Prompt
For the a low effort built-in, we decided to implement a customizable prompt whenever the cush is entered into.
We took inspiration from Dr.Back's Cush and, thus, ended up with a prompt that displays the username@hostname in cwd.

Custom Built-in 2: Command-line history
We integrated the GNU History Library into our cush.
Executing the command "history" will show all commands previously entered. Each command is numbered.
!! will execute the previous command
!n will execute the nth command
!-n will execute the command n lines ago
!string will execute the most recent command starting with string