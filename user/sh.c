// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

int jobs[NPROC];
int job_count = 0;

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);
void panic(char*);
struct cmd *parsecmd(char*);
void runcmd(struct cmd*) __attribute__((noreturn));

// Helper function to poll and reap background jobs
void
poll_background_jobs(void)
{
  uint64 status;
  int pid;
  while ((pid = wait_noblock(&status)) > 0) {
    printf("[bg %d] exited with status %d\n", pid, (int)status);
    // Remove from jobs array
    for(int i = 0; i < job_count; i++) {
      if(jobs[i] == pid) {
        for(int j = i; j < job_count - 1; j++) {
          jobs[j] = jobs[j + 1];
        }
        job_count--;
        break;
      }
    }
  }
}

// Helper function to add a job to the jobs array
void
add_job(int pid)
{
  if(job_count < NPROC) {
    jobs[job_count++] = pid;
  }
}

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    exit(1);

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit(1);
    exec(ecmd->argv[0], ecmd->argv);
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    if(fork1() == 0) {
      runcmd(bcmd->cmd);
    }
    break;
  }
  exit(0);
}

int
getcmd(char *buf, int nbuf, int fd)
{
  int i = 0;
  char c;
  int bytes_read;
  
  memset(buf, 0, nbuf);
  while(i < nbuf - 1) {
    bytes_read = read(fd, &c, 1);
    if(bytes_read <= 0) {
      // EOF or error
      break;
    }
    if(c == '\n') {
      break;
    }
    buf[i++] = c;
  }
  
  if(i == 0)
    return -1;  // EOF with no data
  return 0;
}

int
main(int argc, char *argv[])
{
  static char buf[100];
  int fd = 0;
  int fd_temp;
  int is_script = 0;

  // Handle script file argument
  if(argc > 1) {
    fd = open(argv[1], O_RDONLY);
    if(fd < 0) {
      printf("sh: cannot open %s\n", argv[1]);
      exit(1);
    }
    is_script = 1;
  } else {
    fd = 0; // stdin (keyboard)
    is_script = 0;
    
    // Ensure that three file descriptors are open
    while((fd_temp = open("console", O_RDWR)) >= 0){
      if(fd_temp >= 3){
        close(fd_temp);
        break;
      }
    }
    
    printf("$ ");
  }

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf), fd) >= 0){
    // Handle 'cd' command
    if(buf[0] == 'c' && buf[1] == 'd' && (buf[2] == ' ' || buf[2] == 0)){
      if(buf[2] == ' ') {
        if(chdir(buf+3) < 0)
          fprintf(2, "cannot cd %s\n", buf+3);
      }
      if(!is_script)
        printf("$ ");
      continue;
    }

    // Handle 'jobs' command
    if(buf[0] == 'j' && buf[1] == 'o' && buf[2] == 'b' && 
       buf[3] == 's' && (buf[4] == 0 || buf[4] == ' ')) {
      for(int i = 0; i < job_count; i++) {
        printf("%d\n", jobs[i]);
      }
      if(!is_script)
        printf("$ ");
      continue;
    }

    // Parse command
    struct cmd *parsed_cmd = parsecmd(buf);
    
    // Check if this is a background command
    int is_bg = 0;
    if(parsed_cmd && parsed_cmd->type == BACK) {
      is_bg = 1;
    }

    // Fork and execute command
    int child_pid = fork1();
    if(child_pid == 0) {
      // Child process - execute the command
      if(is_bg) {
        struct backcmd *bcmd = (struct backcmd*)parsed_cmd;
        runcmd(bcmd->cmd);
      } else {
        runcmd(parsed_cmd);
      }
    } else {
      // Parent process
      if(is_bg) {
        // Background job - print PID and add to jobs array
        printf("[%d]\n", child_pid);
        add_job(child_pid);
        // Give the background job a chance to start and complete
        sleep(1);
      } else {
        // Foreground job - wait for this specific child
        // Use wait_noblock in a loop to handle both foreground and background jobs
        uint64 status;
        int wait_pid;
        
        while(1) {
          wait_pid = wait_noblock(&status);
          if(wait_pid == child_pid) {
            // Found our foreground process
            break;
          } else if(wait_pid > 0) {
            // Background process finished
            printf("[bg %d] exited with status %d\n", wait_pid, (int)status);
            // Remove from jobs array
            for(int i = 0; i < job_count; i++) {
              if(jobs[i] == wait_pid) {
                for(int j = i; j < job_count - 1; j++) {
                  jobs[j] = jobs[j + 1];
                }
                job_count--;
                break;
              }
            }
          } else {
            // No zombie children yet, try again after brief sleep
            sleep(1);
          }
        }
      }
    }
    
    // Poll for remaining background jobs that may have finished
    poll_background_jobs();
    
    // Print next prompt (only in interactive mode)
    if(!is_script)
      printf("$ ");
  }

  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}