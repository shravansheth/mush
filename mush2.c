#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <pwd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "mush.h"
#include <errno.h>

#define STDIN_STDOUT 1
#define FILE_IN 0
#define TRUE 1
#define FALSE 0
#define EVEN 2
#define PIPE_IN 0
#define PIPE_OUT 1

static int sig = 0;

void sighandler(int signum){
    sig = 1;
}


int main(int argc, char *argv[]){
    int i;
    FILE *input_file;
    char *input;
    int interactive;
    char *prompt = "8-P ";
    pipeline piped;
    char *dir;
    int one_prog;
    int pipe1[2];
    int pipe2[2];
    int fork_pid;
    int fd_in;
    int fd_out;
    int status;


    /*set input stream(stdin, or file)*/
    if (argc == 1){
        input_file = stdin;
        interactive = STDIN_STDOUT;
    }
    /*read input from the file given*/
    else if(argc == 2){
        input_file = fopen(argv[1], "r");
        if (input_file == NULL){
            fprintf(stderr, "Couldn't open %s\n", argv[1]);
            exit(1);
        }
        interactive = FILE_IN;
    }
    /*else usage error*/
    else{
        fprintf(stderr, "usage: %s\n", argv[0]);
        exit(1);
    }

    /*set sighandler*/
    signal(SIGINT, sighandler);

    while(TRUE){
        if(feof(input_file)){
            break;
        }
        /*If interactive mode(using stdin for input), print prompt 8-P*/
        if(interactive){
            printf("%s", prompt);
        }
        /*read input*/
        input = readLongString(input_file);
        if (input == NULL){
            /*if eof reached or signal caught, go to new line and start again*/
            if(feof(input_file) || (sig && interactive)){
                if (interactive){
                    fprintf(input_file, "\n");
                }
                continue;
            }
            else{
                perror("readLongString failed\n");
                exit(1);
            }
            /*free the input char* */
            free(input);
            /*resent signal indicator*/
            sig = 0;
        }
        /*pipeline time*/
        piped = crack_pipeline(input);
        if(piped == NULL){
            free_pipeline(piped);
            yylex_destroy();
            free(input);
            continue;
        }

        /*just free input and destroy yylex*/
        yylex_destroy();
        free(input);

        /*go back up if signal*/
        if(sig){
            free_pipeline(piped);
            continue;
        }
        /*flag to indicate if we have more than 1 programs in pipe*/
        if(piped->length == 1){
            one_prog = TRUE;
        }
        else{
            one_prog = FALSE;
        }

        /*check for special cases(cd)*/
        if(!strcmp(piped->stage->argv[0], "cd")){
            if(piped->stage->argc > 2){
                fprintf(stderr,"usage: cd\n");
            }
            else if(piped->stage->argc == 2){
                if(chdir(piped->stage[0].argv[1]) == -1){
                    perror("chdir failed\n");
                }
            }
            /*no args given to cd*/
            else if(piped->stage->argc == 1){
                /*try getenv*/
                dir = getenv("HOME");
                if(dir == NULL){
                    /*try getpwuid*/
                    if ((dir = getpwuid(getuid())->pw_dir) == NULL){
                        /*give up, nothing worked*/
                        fprintf(stderr, "unable to determine home directory\n");
                    }
                }
                if(chdir(dir) == -1){
                    perror("chdir failed\n");
                }
            }
        }
        else if(!strcmp(piped->stage->argv[0], "exit")){
            exit(0);
        }
        else{
            /*go through the pipeline*/
            for(i = 0; i < piped->length;i++){
                /*if more than one progs given*/
                if (!one_prog){
                    if (i % EVEN){
                        /*if odd, open pipe1*/
                        if(pipe(pipe1) == -1){
                            perror("pipe failed\n");
                            break;
                        }
                    }
                    else{
                        /*if even, open pipe2*/
                        if(pipe(pipe2) == -1){
                            perror("pipe failed\n");
                            break;
                        }
                    }
                }
                /*fork*/
                fork_pid = fork();
                if (fork_pid == -1){
                    perror("fork failed\n");
                    break;
                }

                /*restore default signal handling if in child process*/
                if (fork_pid == 0){
                    signal(SIGINT, SIG_DFL);
                }

                /*first process*/
                if (i == 0){
                    /*if in child process*/
                    if(fork_pid == 0){
                        /*handle input redirection if file given*/
                        if (piped->stage[0].inname){
                            fd_in = open(piped->stage[0].inname, O_RDONLY);
                            if (fd_in == -1){
                                perror("open failed\n");
                                break;
                            }
                            /*dup that file with stdin*/
                            if (dup2(fd_in, STDIN_FILENO) == -1){
                                perror("dup2 failed\n");
                                break;
                            }
                            close(fd_in);
                        }
                        /*if one prog, redirect output if file given*/
                        if(one_prog){
                            /*if out file given for redirection*/
                            if(piped->stage[0].outname){
                                /*open that file*/
                                fd_out = open(piped->stage[0].outname, 
                                O_RDWR | O_CREAT | O_TRUNC,
                                S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP
                                | S_IWOTH | S_IROTH);
                                if (fd_out == -1){
                                    perror("open failed\n");
                                    break;
                                }
                                /*dup2 that fd with stdout*/
                                if(dup2(fd_out, STDOUT_FILENO) == -1){
                                    perror("dup2 failed\n");
                                    break;
                                }
                                close(fd_out);
                            }
                        }
                        /*if more than one prog, open pipe2 out*/
                        else{
                            if(dup2(pipe2[PIPE_OUT], STDOUT_FILENO) == -1){
                                perror("dup2 failed\n");
                                break;
                            }
                        }
                    }
                    /*in parent process*/
                    else{
                        /*close the pipe outs since we're done writin, 
                        and it's time to readin*/
                        if(!one_prog){
                            close(pipe2[PIPE_OUT]);
                        }
                    }
                }
                /*last process*/
                else if(i == (piped->length - 1)){
                    /*child process*/
                    if (fork_pid == 0){
                        /*handle input redirection if file given*/
                        if(piped->stage[i].outname){
                            fd_out = open(piped->stage[0].outname, 
                            O_RDWR | O_CREAT | O_TRUNC,
                            S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP
                            | S_IWOTH | S_IROTH);
                            /*dup2 that file with stdout*/
                            if (dup2(fd_out, STDOUT_FILENO) == -1){
                                perror("dup2 failed\n");
                                break;
                            }
                        }
                        /*input redirection*/
                        /*if i is odd, read from pipe2 in*/
                        if(i % EVEN){
                            if(dup2(pipe2[PIPE_IN], STDIN_FILENO) == -1){
                                perror("dup2 failed\n");
                                break;
                            }
                        }
                        /*else read from pipe1 in*/
                        else{
                            if(dup2(pipe1[PIPE_IN], STDIN_FILENO) == -1){
                                perror("dup2 failed\n");
                                break;
                            }
                        }
                    }
                    /*if in parent process, just close the pipe outs*/
                    else{
                        if(!one_prog){
                            /*if i is odd, close pipe2 out*/
                            if(i % EVEN){
                                close(pipe1[PIPE_OUT]);
                            }
                            /*else close pipe1 out*/
                            else{
                                close(pipe2[PIPE_OUT]);
                            }
                        }
                    }
                }
                /*all progs apart from first & last*/
                else{
                    /*if in child process*/
                    if (fork_pid == 0){
                        /*if on odd prog, use pipe2 in for reading,
                        and pipe2 out for writing*/
                        if(i % EVEN){
                            if(dup2(pipe2[PIPE_IN], STDIN_FILENO) == -1){
                                perror("dup2 failed\n");
                                break;
                            }
                            if(dup2(pipe1[PIPE_OUT], STDOUT_FILENO) == -1){
                                perror("dup2 failed\n");
                                break;
                            }
                        }
                        /*else, use pipe1 for reading, and pipe2 for writing*/
                        else{
                            if(dup2(pipe1[PIPE_IN], STDIN_FILENO) == -1){
                                perror("dup2 failed\n");
                                break;
                            }
                            if(dup2(pipe2[PIPE_OUT], STDOUT_FILENO) == -1){
                                perror("dup2 failed\n");
                                break;
                            }
                        }
                    }
                    /*if in child process*/
                    /*close both pipes*/
                    else{
                        if(!one_prog){
                            /*if odd, close pipe2 read, and pipe1 write as 
                            opened*/
                            if(i % EVEN){
                                close(pipe2[PIPE_IN]);
                                close(pipe1[PIPE_OUT]);
                            }
                            else{
                                close(pipe1[PIPE_IN]);
                                close(pipe2[PIPE_OUT]);
                            }
                        }
                    }
                }
                /*if in child, exec all those porcesses*/
                if(fork_pid == 0){
                    if (execvp(piped->stage[i].argv[0], 
                    piped->stage[i].argv) == -1){
                        perror("execvp failed\n");
                    }
                }
            }
            /*wait for children processes to die*/
            for(i = 0; i < piped->length; i++){
                if (wait(&status) == -1){
                    if(errno == EINTR){
                        i--;
                    }
                    else{
                        perror("wait failed\n");
                    }
                }
            }
        }
        /*if signal caught, continue back up and reset handler*/
        if (sig && interactive){
            fprintf(stdin, "\n");
            sig = 0;
        }
        /*free the pipeline as it's done executing*/
        free_pipeline(piped);
    }
    if (interactive){
        fprintf(stdin, "\n");
    }
    fclose(input_file);
    return 0;
}