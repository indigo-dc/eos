//------------------------------------------------------------------------------
// File ConsoleMain.cc
// Author Andreas-Joachim Peters - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "ConsoleMain.hh"
#include "ConsolePipe.hh"
#include "ConsoleCompletion.hh"
#include "console/RegexUtil.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "License"
#include "common/Path.hh"
#include "common/IoPipe.hh"
#include "common/SymKeys.hh"
#include "common/Logging.hh"
#include "common/StringTokenizer.hh"
#include "common/StringConversion.hh"
#include "mq/XrdMqMessage.hh"
#include "mq/XrdMqTiming.hh"
#include "XrdOuc/XrdOucTokenizer.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdCl/XrdClFile.hh"
#include <setjmp.h>
#include <readline/readline.h>
#include <readline/history.h>

#ifdef __APPLE__
#define ENONET 64
#endif

//------------------------------------------------------------------------------
// Implemented commands
//------------------------------------------------------------------------------
extern int com_access(char*);
extern int com_acl(char*);
extern int com_archive(char*);
extern int com_attr(char*);
extern int com_backup(char*);
extern int com_cd(char*);
extern int com_chmod(char*);
extern int com_chown(char*);
extern int com_clear(char*);
extern int com_config(char*);
extern int com_console(char*);
extern int com_cp(char*);
extern int com_debug(char*);
extern int com_dropbox(char*);
extern int com_file(char*);
extern int com_fileinfo(char*);
extern int com_find(char*);
extern int com_find_new(char*);
//extern int com_fs(char*);
extern int com_protofs(char*);
extern int com_fsck(char*);
extern int com_fuse(char*);
extern int com_fusex(char*);
extern int com_geosched(char*);
extern int com_group(char*);
extern int com_health(char*);
extern int com_help(char*);
extern int com_info(char*);
extern int com_io(char*);
extern int com_json(char*);
extern int com_kinetic(char*);
extern int com_license(char*);
extern int com_ln(char*);
extern int com_ls(char*);
extern int com_map(char*);
extern int com_member(char*);
extern int com_mkdir(char*);
extern int com_motd(char*);
extern int com_mv(char*);
extern int com_node(char*);
extern int com_ns(char*);
extern int com_pwd(char*);
extern int com_quit(char*);
extern int com_quota(char*);
extern int com_reconnect(char*);
extern int com_recycle(char*);
extern int com_rm(char*);
extern int com_route(char*);
extern int com_protorm(char*);
extern int com_rmdir(char*);
extern int com_role(char*);
extern int com_rtlog(char*);
extern int com_silent(char*);
extern int com_space(char*);
extern int com_stagerrm(char*);
extern int com_stat(char*);
extern int com_squash(char*);
extern int com_test(char*);
extern int com_timing(char*);
extern int com_transfer(char*);
extern int com_touch(char*);
extern int com_version(char*);
extern int com_vid(char*);
extern int com_vst(char*);
extern int com_whoami(char*);
extern int com_who(char*);
extern int com_accounting(char*);

//------------------------------------------------------------------------------
// Command mapping array
//------------------------------------------------------------------------------
COMMAND commands[] = {
  { (char*) "access", com_access, (char*) "Access Interface"},
  { (char*) "accounting", com_accounting, (char*) "Accounting Interface"},
  { (char*) "acl", com_acl, (char*) "Acl Interface"},
  { (char*) "archive", com_archive, (char*) "Archive Interface"},
  { (char*) "attr", com_attr, (char*) "Attribute Interface"},
  { (char*) "backup", com_backup, (char*) "Backup Interface"},
  { (char*) "clear", com_clear, (char*) "Clear the terminal"},
  { (char*) "cd", com_cd, (char*) "Change directory"},
  { (char*) "chmod", com_chmod, (char*) "Mode Interface"},
  { (char*) "chown", com_chown, (char*) "Chown Interface"},
  { (char*) "config", com_config, (char*) "Configuration System"},
  { (char*) "console", com_console, (char*) "Run Error Console"},
  { (char*) "cp", com_cp, (char*) "Cp command"},
  { (char*) "debug", com_debug, (char*) "Set debug level"},
  { (char*) "dropbox", com_dropbox, (char*) "Drop box"},
  { (char*) "exit", com_quit, (char*) "Exit from EOS console"},
  { (char*) "file", com_file, (char*) "File Handling"},
  { (char*) "fileinfo", com_fileinfo, (char*) "File Information"},
  { (char*) "find", com_find, (char*) "Find files/directories"},
  { (char*) "newfind", com_find_new, (char*) "Find files/directories (new implementation)"},
  { (char*) "fs", com_protofs, (char*) "File System configuration"},
  { (char*) "fsck", com_fsck, (char*) "File System Consistency Checking"},
  { (char*) "fuse", com_fuse, (char*) "Fuse Mounting"},
  { (char*) "fusex", com_fusex, (char*) "Fuse(x) Administration"},
  { (char*) "geosched", com_geosched, (char*) "Geoscheduler Interface"},
  { (char*) "group", com_group, (char*) "Group configuration"},
  { (char*) "health", com_health, (char*) "Health information about system"},
  { (char*) "help", com_help, (char*) "Display this text"},
  { (char*) "info", com_info, (char*) "Retrieve file or directory information"},
  { (char*) "io", com_io, (char*) "IO Interface"},
  { (char*) "json", com_json, (char*) "Toggle JSON output flag for stdout"},
  { (char*) "kinetic", com_kinetic, (char*) "Admin commands for kinetic clusters"},
  { (char*) "license", com_license, (char*) "Display Software License"},
  { (char*) "ls", com_ls, (char*) "List a directory"},
  { (char*) "ln", com_ln, (char*) "Create a symbolic link"},
  { (char*) "map", com_map, (char*) "Path mapping interface"},
  { (char*) "member", com_member, (char*) "Check Egroup membership"},
  { (char*) "mkdir", com_mkdir, (char*) "Create a directory"},
  { (char*) "motd", com_motd, (char*) "Message of the day"},
  { (char*) "mv", com_mv, (char*) "Rename file or directory"},
  { (char*) "node", com_node, (char*) "Node configuration"},
  { (char*) "ns", com_ns, (char*) "Namespace Interface"},
  { (char*) "pwd", com_pwd, (char*) "Print working directory"},
  { (char*) "quit", com_quit, (char*) "Exit from EOS console"},
  { (char*) "quota", com_quota, (char*) "Quota System configuration"},
  { (char*) "reconnect", com_reconnect, (char*) "Forces a re-authentication of the shell"},
  { (char*) "recycle", com_recycle, (char*) "Recycle Bin Functionality"},
  { (char*) "rmdir", com_rmdir, (char*) "Remove a directory"},
  { (char*) "rm", com_protorm, (char*) "Remove a file"},
  { (char*) "role", com_role, (char*) "Set the client role"},
  { (char*) "route", com_route, (char*) "Routing interface"},
  { (char*) "rtlog", com_rtlog, (char*) "Get realtime log output from mgm & fst servers"},
  { (char*) "silent", com_silent, (char*) "Toggle silent flag for stdout"},
  { (char*) "space", com_space, (char*) "Space configuration"},
  { (char*) "stagerrm", com_stagerrm, (char*) "Remove disk replicas of a file if it has tape replicas"},
  { (char*) "stat", com_stat, (char*) "Run 'stat' on a file or directory"},
  { (char*) "squash", com_squash, (char*) "Run 'squashfs' utility function"},
  { (char*) "test", com_test, (char*) "Run performance test"},
  { (char*) "timing", com_timing, (char*) "Toggle timing flag for execution time measurement"},
  { (char*) "touch", com_touch, (char*) "Touch a file"},
  { (char*) "transfer", com_transfer, (char*) "Transfer Interface"},
  { (char*) "version", com_version, (char*) "Verbose client/server version"},
  { (char*) "vid", com_vid, (char*) "Virtual ID System Configuration"},
  { (char*) "vst", com_vst, (char*) "Virtual Storage Interface"},
  { (char*) "whoami", com_whoami, (char*) "Determine how we are mapped on server side"},
  { (char*) "who", com_who, (char*) "Statistics about connected users"},
  { (char*) "?", com_help, (char*) "Synonym for `help'"},
  { (char*) ".q", com_quit, (char*) "Exit from EOS console"},
  { (char*) 0, (int (*)(char*))0, (char*) 0}
};

//------------------------------------------------------------------------------
// Global variables
//------------------------------------------------------------------------------
XrdOucString serveruri = "";
XrdOucString historyfile = "";
XrdOucString pwdfile = "";
XrdOucString gPwd = "/";
XrdOucString rstdout;
XrdOucString rstderr;
XrdOucString rstdjson;
XrdOucString user_role = "";
XrdOucString group_role = "";
XrdOucString global_comment = "";

int global_retc = 0;
bool global_highlighting = true;
bool interactive = true;
bool hasterminal = true;
bool silent = false;
bool timing = false;
bool debug = false;
bool pipemode = false;
bool runpipe = false;
bool ispipe = false;
bool json = false;

eos::common::IoPipe iopipe;
int retcfd = 0;
//! When non-zero, this global means the user is done using this program. */
int done;

// Pointer to the result of client_command. It gets invalid when the
// output_result function is called.
XrdOucEnv* CommandEnv = 0;
static sigjmp_buf sigjump_buf;

//------------------------------------------------------------------------------
// Exit handler
//------------------------------------------------------------------------------
void
exit_handler(int a)
{
  fprintf(stdout, "\n");
  fprintf(stderr, "<Control-C>\n");
  write_history(historyfile.c_str());

  if (ispipe) {
    iopipe.UnLockProducer();
  }

  exit(-1);
}

//------------------------------------------------------------------------------
// Jump handler
//------------------------------------------------------------------------------
void
jump_handler(int a)
{
  siglongjmp(sigjump_buf, 1);
}

//------------------------------------------------------------------------------
// Absolute path conversion function
//------------------------------------------------------------------------------
const char*
abspath(const char* in)
{
  static XrdOucString inpath;
  inpath = in;

  if (inpath.beginswith("/")) {
    return inpath.c_str();
  }

  if (gPwd == "/") {
    // check if we are in a /eos/ mountpoint
    char pwd[4096];
    if (getcwd(pwd, sizeof(pwd))) {
      XrdOucString lpwd = pwd;
      if (lpwd.beginswith("/eos")) {
	      inpath = pwd;
	      inpath += "/";
      } else {
	      inpath = gPwd;
      }
    } else {
      inpath = gPwd;
    }
  } else {
    inpath = gPwd;
  }
  inpath += in;
  eos::common::Path cPath(inpath.c_str());
  inpath = cPath.GetPath();
  return inpath.c_str();
}

//------------------------------------------------------------------------------
// Help flag filter
//------------------------------------------------------------------------------
int
wants_help(const char* arg1)
{
  XrdOucString allargs = " ";
  allargs += arg1;
  allargs += " ";

  if ((allargs.find(" help ") != STR_NPOS) ||
      (allargs.find("\"-h\"") != STR_NPOS) ||
      (allargs.find("\"--help\"") != STR_NPOS) ||
      (allargs.find(" -h ") != STR_NPOS) ||
      (allargs.find(" \"-h\" ") != STR_NPOS) ||
      (allargs.find(" --help ") != STR_NPOS) ||
      (allargs.find(" \"--help\" ") != STR_NPOS)) {
    return 1;
  }

  return 0;
}

//------------------------------------------------------------------------------
// Switches stdin,stdout,stderr to pipe mode where we are a persistant
// communication daemon for a the eospipe command forwarding commands.
//------------------------------------------------------------------------------
bool
startpipe()
{
  XrdOucString pipedir = "";
  XrdOucString stdinname = "";
  XrdOucString stdoutname = "";
  XrdOucString stderrname = "";
  XrdOucString retcname = "";
  ispipe = true;
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);

  if (!iopipe.Init()) {
    fprintf(stderr, "error: cannot set IoPipe\n");
    return false;
  }

  XrdSysLogger* logger = new XrdSysLogger();
  XrdSysError eDest(logger);
  int stdinfd = iopipe.AttachStdin(eDest);
  int stdoutfd = iopipe.AttachStdout(eDest);
  int stderrfd = iopipe.AttachStderr(eDest);
  retcfd = iopipe.AttachRetc(eDest);

  if ((stdinfd < 0) ||
      (stdoutfd < 0) ||
      (stderrfd < 0) ||
      (retcfd < 0)) {
    fprintf(stderr, "error: cannot attach to pipes\n");
    return false;
  }

  if (!iopipe.LockProducer()) {
    return false;
  }

  stdin = fdopen(stdinfd, "r");
  stdout = fdopen(stdoutfd, "w");
  stderr = fdopen(stderrfd, "w");
  return true;
}


/* **************************************************************** */
/*                                                                  */
/*                       EOSConsole Commands                        */
/*                                                                  */
/* **************************************************************** */
void
command_result_stdout_to_vector(std::vector<std::string>& string_vector)
{
  string_vector.clear();

  if (!CommandEnv) {
    fprintf(stderr, "error: command env is 0!\n");
    return;
  }

  rstdout = CommandEnv->Get("mgm.proc.stdout");

  if (!rstdout.length()) {
    return;
  }

  if (rstdout.beginswith("base64:")) {
    XrdOucString ub64out;
    eos::common::SymKey::DeBase64(rstdout, ub64out);
    rstdout = ub64out;
  } else {
    XrdMqMessage::UnSeal(rstdout);
  }

  XrdOucTokenizer subtokenizer((char*) rstdout.c_str());
  const char* nextline = 0;
  int i = 0;

  while ((nextline = subtokenizer.GetLine())) {
    if ((!strlen(nextline)) || (nextline[0] == '\n')) {
      continue;
    }

    string_vector.resize(i + 1);
    string_vector.push_back(nextline);
    i++;
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
output_result(XrdOucEnv* result, bool highlighting)
{
  if (!result) {
    return EINVAL;
  }

  rstdout = result->Get("mgm.proc.stdout");
  rstderr = result->Get("mgm.proc.stderr");
  rstdjson = result->Get("mgm.proc.json");

  if (rstdout.beginswith("base64:")) {
    XrdOucString ub64out;
    eos::common::SymKey::DeBase64(rstdout, ub64out);
    rstdout = ub64out;
  } else {
    XrdMqMessage::UnSeal(rstdout);
  }

  if (rstderr.beginswith("base64:")) {
    XrdOucString ub64out;
    eos::common::SymKey::DeBase64(rstderr, ub64out);
    rstderr = ub64out;
  } else {
    XrdMqMessage::UnSeal(rstderr);
  }

  if (rstdjson.beginswith("base64:")) {
    XrdOucString ub64out;
    eos::common::SymKey::DeBase64(rstdjson, ub64out);
    rstdjson = ub64out;
  } else {
    XrdMqMessage::UnSeal(rstdjson);
  }

  if (highlighting && global_highlighting) {
    // color replacements
    rstdout.replace("[booted]", "\033[1m[booted]\033[0m");
    rstdout.replace("[down]", "\033[49;31m[down]\033[0m");
    rstdout.replace("[failed]", "\033[49;31m[failed]\033[0m");
    rstdout.replace("[booting]", "\033[49;32m[booting]\033[0m");
    rstdout.replace("[compacting]", "\033[49;34m[compacting]\033[0m");
    // replication highlighting
    rstdout.replace("master-rw", "\033[49;31mmaster-rw\033[0m");
    rstdout.replace("master-ro", "\033[49;34mmaster-ro\033[0m");
    rstdout.replace("slave-ro", "\033[1mslave-ro\033[0m");
    rstdout.replace("=ok", "=\033[49;32mok\033[0m");
    rstdout.replace("=compacting", "=\033[49;32mcompacting\033[0m");
    rstdout.replace("=off", "=\033[49;34moff\033[0m");
    rstdout.replace("=blocked", "=\033[49;34mblocked\033[0m");
    rstdout.replace("=wait", "=\033[49;34mwait\033[0m");
    rstdout.replace("=starting", "=\033[49;34mstarting\033[0m");
    rstdout.replace("=true", "=\033[49;32mtrue\033[0m");
    rstdout.replace("=false", "=\033[49;31mfalse\033[0m");
  }

  int retc = 0;

  if (result->Get("mgm.proc.retc")) {
    retc = atoi(result->Get("mgm.proc.retc"));
  }

  if (json) {
    if (rstdjson.length())
      if (!silent) {
        fprintf(stdout, "%s", rstdjson.c_str());

        if (rstdjson.endswith('\n')) {
          fprintf(stdout, "\n");
        }
      }
  } else {
    if (rstdout.length())
      if (!silent) {
        fprintf(stdout, "%s", rstdout.c_str());

        if (!rstdout.endswith('\n')) {
          fprintf(stdout, "\n");
        }
      }

    if (rstderr.length()) {
      fprintf(stderr, "%s (errc=%d) (%s)\n", rstderr.c_str(), retc, strerror(retc));
    }
  }

  fflush(stdout);
  fflush(stderr);
  CommandEnv = 0;
  delete result;
  return retc;
}

//------------------------------------------------------------------------------
// Execute user command
//------------------------------------------------------------------------------
XrdOucEnv*
client_command(XrdOucString& in, bool is_admin, std::string* reply)
{
  if (user_role.length()) {
    in += "&eos.ruid=";
  }

  in += user_role;

  if (group_role.length()) {
    in += "&eos.rgid=";
  }

  in += group_role;

  if (json) {
    in += "&mgm.format=json";
  }

  if (global_comment.length()) {
    in += "&mgm.comment=";
    in += global_comment;
    global_comment = "";
  }

  XrdMqTiming mytiming("eos");
  TIMING("start", &mytiming);
  XrdOucString out = "";
  XrdOucString path = serveruri;

  if (is_admin) {
    path += "//proc/admin/";
  } else {
    path += "//proc/user/";
  }

  path += "?";
  path += in;
  XrdCl::OpenFlags::Flags flags_xrdcl = XrdCl::OpenFlags::Read;
  std::unique_ptr<XrdCl::File> client {new XrdCl::File()};
  XrdCl::XRootDStatus status = client->Open(path.c_str(), flags_xrdcl);

  if (status.IsOK()) {
    off_t offset = 0;
    uint32_t nbytes = 0;
    char buffer[4096 + 1];
    status = client->Read(offset, 4096, buffer, nbytes);

    while (status.IsOK() && (nbytes > 0)) {
      buffer[nbytes] = 0;
      out += buffer;
      offset += nbytes;
      status = client->Read(offset, 4096, buffer, nbytes);
    }

    status = client->Close();
    TIMING("stop", &mytiming);

    if (timing) {
      mytiming.Print();
    }

    if (debug) {
      printf("out=%s\n", out.c_str());
    }

    CommandEnv = new XrdOucEnv(out.c_str());

    // Save the reply string from the server
    if (reply) {
      reply->assign(out.c_str());
    }

    return CommandEnv;
  } else {
    std::string errmsg;
    errmsg = status.GetErrorMessage();
    fprintf(stderr, "error: errc=%d msg=\"%s\"\n", status.errNo, errmsg.c_str());
  }

  return nullptr;
}

//------------------------------------------------------------------------------
// Load and apply the last used directory
//------------------------------------------------------------------------------
void
read_pwdfile()
{
  std::string lpwd;
  eos::common::StringConversion::LoadFileIntoString(pwdfile.c_str(), lpwd);

  if (lpwd.length()) {
    com_cd((char*) lpwd.c_str());
  }
}

//------------------------------------------------------------------------------
// Colour definitions
//------------------------------------------------------------------------------
std::string textnormal("\001\033[0m\002");
std::string textblack("\001\033[49;30m\002");
std::string textred("\001\033[49;31m\002");
std::string textrederror("\001\033[47;31m\e[5m\002");
std::string textblueerror("\001\033[47;34m\e[5m\002");
std::string textgreen("\001\033[49;32m\002");
std::string textyellow("\001\033[49;33m\002");
std::string textblue("\001\033[49;34m\002");
std::string textbold("\001\033[1m\002");
std::string textunbold("\001\033[0m\002");

//------------------------------------------------------------------------------
// Usage Information
//------------------------------------------------------------------------------
void
usage()
{
  fprintf(stderr,
          "`eos' is the command line interface (CLI) of the EOS storage system.\n");
  fprintf(stderr,
          "Usage: eos [-r|--role <uid> <gid>] [-b|--batch] [-v|--version] [-p|--pipe] [-j||--json] [<mgm-url>] [<cmd> {<argN>}|<filename>.eosh]\n");
  fprintf(stderr,
          "            -r, --role <uid> <gid>              : select user role <uid> and group role <gid>\n");
  fprintf(stderr,
          "            -b, --batch                         : run in batch mode without colour and syntax highlighting and without pipe\n");
  fprintf(stderr,
          "            -j, --json                          : switch to json output format\n");
  fprintf(stderr,
          "            -p, --pipe                          : run stdin,stdout,stderr on local pipes and go to background\n");
  fprintf(stderr,
          "            -h, --help                          : print help text\n");
  fprintf(stderr,
          "            -v, --version                       : print version information\n");
  fprintf(stderr,
          "            <mgm-url>                           : xroot URL of the management server e.g. root://<hostname>[:<port>]\n");
  fprintf(stderr,
          "            <cmd>                               : eos shell command (use 'eos help' to see available commands)\n");
  fprintf(stderr,
          "            {<argN>}                            : single or list of arguments for the eos shell command <cmd>\n");
  fprintf(stderr,
          "            <filename>.eosh                     : eos script file name ending with .eosh suffix\n\n");
  fprintf(stderr, "Environment Variables: \n");
  fprintf(stderr,
          "            EOS_MGM_URL                         : set's the redirector URL\n");
  fprintf(stderr,
          "            EOS_HISTORY_FILE                    : set's the command history file - by default '$HOME/.eos_history' is used\n\n");
  fprintf(stderr,
          "            EOS_SOCKS4_HOST                     : set's the SOCKS4 proxy host name\n");
  fprintf(stderr,
          "            EOS_SOCKS4_PORT                     : set's the SOCKS4 proxy port\n");
  fprintf(stderr,
          "            EOS_PWD_FILE                        : set's the file where the last working directory is stored- by default '$HOME/.eos_pwd\n\n");
  fprintf(stderr,
          "            EOS_ENABLE_PIPEMODE                 : allows the EOS shell to split into a session and pipe executable to avoid useless re-authentication\n");
  fprintf(stderr, "Return Value: \n");
  fprintf(stderr,
          "            The return code of the last executed command is returned. 0 is returned in case of success otherwise <errno> (!=0).\n\n");
  fprintf(stderr, "Examples:\n");
  fprintf(stderr,
          "            eos                                 : start the interactive eos shell client connected to localhost or URL defined in environment variabel EOS_MGM_URL\n");
  fprintf(stderr,
          "            eos -r 0 0                          : as before but take role root/root [only numeric IDs are supported]\n");
  fprintf(stderr,
          "            eos root://myeos                    : start the interactive eos shell connecting to MGM host 'myeos'\n");
  fprintf(stderr,
          "            eos -b whoami                       : run the eos shell command 'whoami' in batch mode without syntax highlighting\n");
  fprintf(stderr,
          "            eos space ls --io                   : run the eos shell command 'space' with arguments 'ls --io'\n");
  fprintf(stderr,
          "            eos --version                       : print version information\n");
  fprintf(stderr,
          "            eos -b eosscript.eosh               : run the eos shell script 'eosscript.eosh'. This script has to contain linewise commands which are understood by the eos interactive shell.\n");
  fprintf(stderr, "\n");
  fprintf(stderr,
          " You can leave the interactive shell with <Control-D>. <Control-C> cleans the current shell line or terminates the shell when a command is currently executed.");
  fprintf(stderr, "Report bugs to eos-dev@cern.ch\n");
}

//------------------------------------------------------------------------------
// Main executable
//------------------------------------------------------------------------------
int
Run(int argc, char* argv[])
{
  bool checked_mgm = false;
  char* line, *s;
  serveruri = (char*) "root://localhost";
  // Enable fork handlers for XrdCl
  XrdCl::Env* env = XrdCl::DefaultEnv::GetEnv();
  env->PutInt("RunForkHandler", 1);

  if (getenv("EOS_MGM_URL")) {
    serveruri = getenv("EOS_MGM_URL");
  }

  XrdOucString urole = "";
  XrdOucString grole = "";
  bool selectedrole = false;
  int argindex = 1;
  int retc = system("test -t 0 && test -t 1");

  if (getenv("EOS_ENABLE_PIPEMODE")) {
    runpipe = true;
  } else {
    runpipe = false;
  }

  if (!retc) {
    hasterminal = true;
    global_highlighting = true;
    interactive = true;
  } else {
    hasterminal = false;
    global_highlighting = false;
    interactive = false;
  }

  if (argc > 1) {
    XrdOucString in1 = argv[argindex];

    if (in1.beginswith("-")) {
      if ((in1 != "--help") &&
          (in1 != "--version") &&
          (in1 != "--batch") &&
          (in1 != "--pipe") &&
          (in1 != "--role") &&
          (in1 != "--json") &&
          (in1 != "-h") &&
          (in1 != "-b") &&
          (in1 != "-p") &&
          (in1 != "-v") &&
          (in1 != "-j") &&
          (in1 != "-r")) {
        usage();
        exit(-1);
      }
    }

    if ((in1 == "--help") || (in1 == "-h")) {
      usage();
      exit(-1);
    }

    if ((in1 == "--version") || (in1 == "-v")) {
      fprintf(stderr, "EOS %s (CERN)\n\n", VERSION);
      fprintf(stderr, "Written by CERN-IT-DSS (Andreas-Joachim Peters, "
              "Lukasz Janyst & Elvin Sindrilaru)\n");
      exit(-1);
    }

    if ((in1 == "--batch") || (in1 == "-b")) {
      interactive = false;
      global_highlighting = false;
      runpipe = false;
      argindex++;
      in1 = argv[argindex];
    }

    if ((in1 == "--json") || (in1 == "-j")) {
      interactive = false;
      global_highlighting = false;
      json = true;
      runpipe = false;
      argindex++;
      in1 = argv[argindex];
    }

    if ((in1 == "fuse")) {
      interactive = false;
      global_highlighting = false;
      runpipe = false;
    }

    if ((in1 == "--pipe") || (in1 == "-p")) {
      pipemode = true;
      argindex++;
      in1 = argv[argindex];

      if (!startpipe()) {
        fprintf(stderr, "error: unable to start the pipe - maybe there is "
                "already a process with 'eos -p' running?\n");
        exit(-1);
      }
    }

    if ((in1 == "--role") || (in1 == "-r")) {
      urole = argv[argindex + 1];
      grole = argv[argindex + 2];
      in1 = argv[argindex + 3];
      argindex += 3;
      // execute the role function
      XrdOucString cmdline = "role ";
      cmdline += urole;
      cmdline += " ";
      cmdline += grole;
      in1 = argv[argindex];

      if (in1.length()) {
        silent = true;
      }

      execute_line((char*) cmdline.c_str());

      if (in1.length()) {
        silent = false;
      }

      selectedrole = true;
    }

    if ((in1 == "--batch") || (in1 == "-b")) {
      interactive = false;
      argindex++;
      in1 = argv[argindex];
    }

    if ((in1 == "cp")) {
      interactive = false;
      global_highlighting = false;
      runpipe = false;
    }

    if ((in1 == "fuse")) {
      interactive = false;
    }

    if (in1.beginswith("root://")) {
      serveruri = argv[argindex];
      argindex++;
      in1 = argv[argindex];
      checked_mgm = true;

      if (!CheckMgmOnline(serveruri.c_str())) {
        std::cerr << "error: MGM " << serveruri.c_str()
                  << " not online/reachable" << std::endl;
        exit(ENONET);
      }
    }

    if (in1.length()) {
      // check if this is a file (workaournd for XrdOucString bug
      if ((in1.length() > 5) && (in1.endswith(".eosh")) &&
          (!access(in1.c_str(), R_OK))) {
        // this is a script file
        char str[16384];
        fstream file_op(in1.c_str(), ios::in);

        while (!file_op.eof()) {
          file_op.getline(str, 16384);
          XrdOucString cmdline = "";
          cmdline = str;

          if (!cmdline.length()) {
            break;
          }

          while (cmdline.beginswith(" ")) {
            cmdline.erase(0, 1);
          }

          while (cmdline.endswith(" ")) {
            cmdline.erase(cmdline.length() - 1, 1);
          }

          execute_line((char*) cmdline.c_str());
        }

        file_op.close();
        exit(0);
      } else {
        XrdOucString cmdline = "";

        // this are commands
        for (int i = argindex; i < argc; i++) {
          if (i != argindex) {
            cmdline += "\"";
          }

          cmdline += argv[i];

          if (i != argindex) {
            cmdline += "\"";
          }

          cmdline += " ";
        }

        if ((!selectedrole) && (!getuid()) &&
            (serveruri.beginswith("root://localhost"))) {
          // we are root, we always select also the root role by default
          XrdOucString cmdline = "role 0 0 ";
          silent = true;
          execute_line((char*) cmdline.c_str());
          silent = false;
        }

        // strip leading and trailing white spaces
        while (cmdline.beginswith(" ")) {
          cmdline.erase(0, 1);
        }

        while (cmdline.endswith(" ")) {
          cmdline.erase(cmdline.length() - 1, 1);
        }

        // Here we can use the 'eospipe' mechanism if allowed
        if (runpipe) {
          cmdline += "\n";
          // put the eos daemon into batch mode
          interactive = false;
          global_highlighting = false;
          iopipe.Init(); // need to initialize for Checkproducer

          if (!iopipe.CheckProducer()) {
            // We need to run a pipe daemon, so we fork here and let the fork
            // run the code like 'eos -p'
            if (!fork()) {
              for (int i = 1; i < argc; i++) {
                for (size_t j = 0; j < strlen(argv[i]); j++) {
                  argv[i][j] = '*';
                }
              }

              // detach from the session id
              pid_t sid;

              if ((sid = setsid()) < 0) {
                fprintf(stderr, "ERROR: failed to create new session (setsid())\n");
                exit(-1);
              }

              startpipe();
              pipemode = true;
              // enters down the readline loop with modified stdin,stdout,stderr
            } else {
              // now we just deal with the pipes from the client end
              exit(pipe_command(cmdline.c_str()));
            }
          } else {
            // now we just deal with the pipes from the client end
            exit(pipe_command(cmdline.c_str()));
          }
        } else {
          execute_line((char*) cmdline.c_str());
          exit(global_retc);
        }
      }
    }
  }

  // Make sure to check the MGM is reachable
  if (!checked_mgm) {
    if (!CheckMgmOnline(serveruri.c_str())) {
      std::cerr << "error: MGM " << serveruri.c_str()
                << " not online/reachable" << std::endl;
      exit(ENONET);
    }
  }

  // By default select the root role if we are root@localhost
  if ((!selectedrole) && (!getuid()) &&
      (serveruri.beginswith("root://localhost"))) {
    // we are root, we always select also the root role by default
    XrdOucString cmdline = "role 0 0 ";
    silent = true;
    execute_line((char*) cmdline.c_str());
    silent = false;
  }

  /* configure logging */
  eos::common::Logging& g_logging = eos::common::Logging::GetInstance();
  g_logging.SetUnit("eos");
  g_logging.SetLogPriority(LOG_NOTICE);

  /* install a shutdown handler */
  //signal(SIGINT, exit_handler);

  if (!interactive) {
    textnormal = "";
    textblack = "";
    textred = "";
    textrederror = "";
    textblueerror = "";
    textgreen = "";
    textyellow = "";
    textblue = "";
    textbold = "";
    textunbold = "";
  }

  if (interactive) {
    fprintf(stderr,
            "# ---------------------------------------------------------------------------\n");
    fprintf(stderr, "# EOS  Copyright (C) 2011-2017 CERN/Switzerland\n");
    fprintf(stderr,
            "# This program comes with ABSOLUTELY NO WARRANTY; for details type `license'.\n");
    fprintf(stderr,
            "# This is free software, and you are welcome to redistribute it \n");
    fprintf(stderr, "# under certain conditions; type `license' for details.\n");
    fprintf(stderr,
            "# ---------------------------------------------------------------------------\n");
    execute_line((char*) "motd");
    execute_line((char*) "version");
  }

  char prompt[4096];

  if (pipemode) {
    prompt[0] = 0;
  } else {
    sprintf(prompt, "%sEOS Console%s [%s%s%s] |> ", textbold.c_str(),
            textunbold.c_str(), textred.c_str(), serveruri.c_str(), textnormal.c_str());
  }

  // Bind our completer
  rl_readline_name = (char*) "EOS Console";
  rl_attempted_completion_function = eos_console_completion;
  rl_completion_append_character = '\0';

  if (getenv("EOS_HISTORY_FILE")) {
    historyfile = getenv("EOS_HISTORY_FILE");
  } else {
    if (getenv("HOME")) {
      historyfile = getenv("HOME");
      historyfile += "/.eos_history";
    }
  }

  if (getenv("EOS_PWD_FILE")) {
    pwdfile = getenv("EOS_PWD_FILE");
  } else {
    if (getenv("HOME")) {
      pwdfile = getenv("HOME");
      pwdfile += "/.eos_pwd";
    }
  }

  read_history(historyfile.c_str());
  // load the last used current working directory
  read_pwdfile();

  // Loop reading and executing lines until the user quits.
  for (; done == 0;) {
    char prompt[4096];

    if (pipemode) {
      prompt[0] = 0;
    } else {
      sprintf(prompt, "%sEOS Console%s [%s%s%s] |%s> ", textbold.c_str(),
              textunbold.c_str(), textred.c_str(), serveruri.c_str(), textnormal.c_str(),
              gPwd.c_str());
    }

    if (pipemode) {
      signal(SIGALRM, exit_handler);
      alarm(60);
    }

    signal(SIGINT, jump_handler);

    if (sigsetjmp(sigjump_buf, 1)) {
      signal(SIGINT, jump_handler);
      fprintf(stdout, "\n");
    }

    line = readline(prompt);
    signal(SIGINT, exit_handler);

    if (pipemode) {
      alarm(0);
    }

    if (!line) {
      fprintf(stdout, "\n");
      break;
    }

    // Remove leading and trailing whitespace from the line. Then, if there
    // is anything left, add it to the history list and execute it.
    s = stripwhite(line);

    if (*s) {
      add_history(s);
      // 20 minutes timeout for commands ... that is long !
      signal(SIGALRM, exit_handler);
      alarm(3600);
      execute_line(s);
      alarm(0);
      char newline = '\n';
      int n = 0;
      std::cout << std::flush;
      std::cerr << std::flush;
      fflush(stdout);
      fflush(stderr);

      if (pipemode) {
        n = write(retcfd, &global_retc, sizeof(global_retc));
        n = write(retcfd, &newline, sizeof(newline));

        if (n != 1) {
          fprintf(stderr, "error: unable to write retc to retc-socket\n");
          exit(-1);
        }

        // we send the stop sequence to the pipe thread listeners
        fprintf(stdout, "#__STOP__#\n");
        fprintf(stderr, "#__STOP__#\n");
        fflush(stdout);
        fflush(stderr);
      }
    }

    free(line);
  }

  write_history(historyfile.c_str());
  signal(SIGINT, SIG_IGN);
  exit(0);
}

//------------------------------------------------------------------------------
// Command line execution function
//------------------------------------------------------------------------------
int
execute_line(char* line)
{
  std::string comment;
  if (!(line = parse_comment(line, comment))) {
    fprintf(stderr,
            "error: syntax for comment is '<command> <args> --comment \"<comment>\"'\n");
    global_retc = -1;
    return (-1);
  }
  global_comment = comment.c_str();

  // Isolate the command word and the rest of the arguments
  std::list<std::string> tokens =
    eos::common::StringTokenizer::split<std::list<std::string>>(line, ' ');

  if (!tokens.size()) {
    global_retc = -1;
    return (-1);
  }

  COMMAND* command = find_command(tokens.begin()->c_str());

  if (!command) {
    fprintf(stderr, "%s: No such command for EOS Console.\n",
            tokens.begin()->c_str());
    global_retc = -1;
    return (-1);
  }

  tokens.erase(tokens.begin());
  std::string args = eos::common::StringTokenizer::merge(tokens, ' ');
  return ((*(command->func))((char*)args.c_str()));
}

//------------------------------------------------------------------------------
// Look up NAME as the name of a command, and return a pointer to that command.
// Return a 0 pointer if NAME isn't a command name.
//------------------------------------------------------------------------------
COMMAND*
find_command(const char* name)
{
  for (int i = 0; commands[i].name; ++i) {
    if (strcmp(name, commands[i].name) == 0) {
      return (&commands[i]);
    }
  }

  return ((COMMAND*) 0);
}

//------------------------------------------------------------------------------
// Strip whitespace from the start and end of STRING.  Return a pointer to
// STRING.
//------------------------------------------------------------------------------
char*
stripwhite(char* string)
{
  char* s, *t;

  for (s = string; (*s) == ' '; s++)
    ;

  if (*s == 0) {
    return (s);
  }

  t = s + strlen(s) - 1;

  while (t > s && ((*t) == ' ')) {
    t--;
  }

  *++t = '\0';
  return s;
}

//------------------------------------------------------------------------------
// Parse the command line, extract the comment
// and returns the line without the comment in it
//------------------------------------------------------------------------------
char*
parse_comment(char* line, std::string& comment)
{
  std::string exec_line = line;

  // Commands issued from the EOS shell do not encase arguments in quotes
  // whereas commands issued from the terminal do
  size_t cbpos = exec_line.find("\"--comment\"");
  int size = 11;

  if (cbpos == std::string::npos) {
    cbpos = exec_line.find("--comment");
    size = 9;
  }

  if (cbpos != std::string::npos) {
    // Check that line doesn't end with comment flag
    if (cbpos + size == exec_line.length()) {
      return 0;
    }

    // Check we found a complete word
    if (exec_line[cbpos + size] == ' ') {
      // Check we have comment text
      if (cbpos + size + 3 >= exec_line.length()) {
        return 0;
      }

      // Comment text should always start with quotes: --comment "<comment>"
      if (exec_line[cbpos + size + 1] == '"') {
        size_t cepos = exec_line.find('"', cbpos + size + 2);

        // Comment text should always end with quotes: --comment "<comment>"
        if (cepos != std::string::npos) {
          comment =
              exec_line.substr(cbpos + size + 1, cepos - (cbpos + size)).c_str();
          exec_line.erase(cbpos, cepos - cbpos + 1);
          line = (char *) exec_line.c_str();
        } else {
          return 0;
        }
      } else {
        return 0;
      }
    }
  }

  return line;
}

//------------------------------------------------------------------------------
// Check if input matches pattern and extract the file id if possible
//------------------------------------------------------------------------------
bool RegWrapDenominator(XrdOucString& input, const std::string& key)
{
  try {
    RegexUtil reg;
    reg.SetRegex(key);
    reg.SetOrigin(input.c_str());
    reg.initTokenizerMode();
    std::string temp = reg.Match();
    auto pos = temp.find(':');
    temp = std::string(temp.begin() + pos + 1, temp.end());
    input = XrdOucString(temp.c_str());
    return true;
  } catch (std::string& e) {
    return false;
  }
}

//------------------------------------------------------------------------------
// Extract file id specifier if input is in one of the following formats:
// fxid:<hex_id> | fid:<dec_id>
//------------------------------------------------------------------------------
bool Path2FileDenominator(XrdOucString& input)
{
  if (RegWrapDenominator(input, "fxid:[a-fA-F0-9]+$")) {
    std::string temp = std::to_string(strtoull(input.c_str(), 0, 16));
    input = XrdOucString(temp.c_str());
    return true;
  }

  return RegWrapDenominator(input, "fid:[0-9]+$");
}

//------------------------------------------------------------------------------
// Extract file id specifier if input is in one of the following formats:
// fxid:<hex_id> | fid:<dec_id>
//------------------------------------------------------------------------------
bool Path2FileDenominator(XrdOucString& input, unsigned long long& id)
{
  if (RegWrapDenominator(input, "fxid:[a-fA-F0-9]+$")) {
    id = strtoull(input.c_str(), nullptr, 16);
    return true;
  } else if (RegWrapDenominator(input, "fid:[0-9]+$")) {
    id = strtoull(input.c_str(), nullptr, 10);
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Extract container id specifier if input is in one of the following formats:
// cxid:<hex_id> | cid:<dec_id>
//------------------------------------------------------------------------------
bool Path2ContainerDenominator(XrdOucString& input)
{
  if (RegWrapDenominator(input, "cxid:[a-fA-F0-9]+$")) {
    std::string temp = std::to_string(strtoull(input.c_str(), 0, 16));
    input = XrdOucString(temp.c_str());
    return true;
  }

  return RegWrapDenominator(input, "cid:[0-9]+$");
}

//------------------------------------------------------------------------------
// Extract container id specifier if input is in one of the following formats:
// cxid:<hex_id> | cid:<dec_id>
//------------------------------------------------------------------------------
bool Path2ContainerDenominator(XrdOucString& input, unsigned long long& id)
{
  if (RegWrapDenominator(input, "cxid:[a-fA-F0-9]+$")) {
    id = strtoull(input.c_str(), nullptr, 16);
    return true;
  } else if (RegWrapDenominator(input, "cid:[0-9]+$")) {
    id = strtoull(input.c_str(), nullptr, 10);
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Check if MGM is online and reachable
//------------------------------------------------------------------------------
bool CheckMgmOnline(const std::string& uri)
{
  uint16_t timeout = 10;
  XrdCl::URL url(uri);

  if (!url.IsValid()) {
    std::cerr << "error: " << uri << " not a valid URL" << std::endl;
    return false;
  }

  XrdCl::FileSystem fs(url);
  XrdCl::XRootDStatus status = fs.Ping(timeout);
  return status.IsOK();
}
