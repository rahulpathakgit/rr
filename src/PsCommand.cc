/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include <map>

#include "Command.h"
#include "main.h"
#include "TraceStream.h"
#include "TraceTaskEvent.h"

using namespace std;

namespace rr {

class PsCommand : public Command {
public:
  virtual int run(std::vector<std::string>& args);

protected:
  PsCommand(const char* name, const char* help) : Command(name, help) {}

  static PsCommand singleton;
};

PsCommand PsCommand::singleton("ps", " rr ps [<trace_dir>]\n");

static void print_exec_cmd_line(const TraceTaskEvent& event, FILE* out) {
  bool first = true;
  for (auto& word : event.cmd_line()) {
    fprintf(out, "%s%s", first ? "" : " ", word.c_str());
    first = false;
  }
  fprintf(out, "\n");
}

static void update_tid_to_pid_map(std::map<pid_t, pid_t>& tid_to_pid,
                                  const TraceTaskEvent& e) {
  if (e.is_fork()) {
    // Some kind of fork. This task is its own pid.
    tid_to_pid[e.tid()] = e.tid();
  } else if (e.type() == TraceTaskEvent::CLONE) {
    // thread clone. Record thread's pid.
    tid_to_pid[e.tid()] = tid_to_pid[e.parent_tid()];
  } else if (e.type() == TraceTaskEvent::EXIT) {
    tid_to_pid.erase(e.tid());
  }
}

static int count_tids_for_pid(const std::map<pid_t, pid_t> tid_to_pid,
                              pid_t pid) {
  int count = 0;
  for (auto& tp : tid_to_pid) {
    if (tp.second == pid) {
      ++count;
    }
  }
  return count;
}

static ssize_t find_cmd_line(pid_t pid, const vector<TraceTaskEvent>& events,
                             size_t current_event,
                             const std::map<pid_t, pid_t> current_tid_to_pid) {
  std::map<pid_t, pid_t> tid_to_pid = current_tid_to_pid;
  for (size_t i = current_event; i < events.size(); ++i) {
    const TraceTaskEvent& e = events[i];
    if (e.type() == TraceTaskEvent::EXEC && tid_to_pid[e.tid()] == pid) {
      return i;
    }
    if (e.type() == TraceTaskEvent::EXIT && tid_to_pid[e.tid()] == pid &&
        count_tids_for_pid(tid_to_pid, pid) == 1) {
      return -1;
    }
    update_tid_to_pid_map(tid_to_pid, e);
  }
  return -1;
}

static int find_exit_code(pid_t pid, const vector<TraceTaskEvent>& events,
                          size_t current_event,
                          const std::map<pid_t, pid_t> current_tid_to_pid) {
  std::map<pid_t, pid_t> tid_to_pid = current_tid_to_pid;
  for (size_t i = current_event; i < events.size(); ++i) {
    const TraceTaskEvent& e = events[i];
    if (e.type() == TraceTaskEvent::EXIT && tid_to_pid[e.tid()] == pid &&
        count_tids_for_pid(tid_to_pid, pid) == 1) {
      int status = e.exit_status();
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      }
      assert(WIFSIGNALED(status));
      return -WTERMSIG(status);
    }
    update_tid_to_pid_map(tid_to_pid, e);
  }
  return -SIGKILL;
}

static int ps(const string& trace_dir, FILE* out) {
  TraceReader trace(trace_dir);

  fprintf(out, "PID\tPPID\tEXIT\tCMD\n");

  vector<TraceTaskEvent> events;
  while (trace.good()) {
    events.push_back(trace.read_task_event());
  }

  if (events.empty() || events[0].type() != TraceTaskEvent::EXEC) {
    fprintf(stderr, "Invalid trace\n");
    return 1;
  }

  std::map<pid_t, pid_t> tid_to_pid;

  pid_t initial_tid = events[0].tid();
  tid_to_pid[initial_tid] = initial_tid;
  fprintf(out, "%d\t--\t%d\t", initial_tid,
          find_exit_code(initial_tid, events, 0, tid_to_pid));
  print_exec_cmd_line(events[0], out);

  for (size_t i = 1; i < events.size(); ++i) {
    auto& e = events[i];
    update_tid_to_pid_map(tid_to_pid, e);

    if (e.is_fork()) {
      pid_t pid = tid_to_pid[e.tid()];
      fprintf(out, "%d\t%d\t%d\t", e.tid(), tid_to_pid[e.parent_tid()],
              find_exit_code(pid, events, i, tid_to_pid));

      ssize_t cmd_line_index = find_cmd_line(pid, events, i, tid_to_pid);
      if (cmd_line_index < 0) {
        // The main thread exited. All other threads must too, so there
        // is no more opportunity for e's pid to exec.
        fprintf(out, "(forked without exec)\n");
      } else {
        print_exec_cmd_line(events[cmd_line_index], out);
      }
    }
  }
  return 0;
}

int PsCommand::run(std::vector<std::string>& args) {
  while (parse_global_option(args)) {
  }

  string trace_dir;
  if (!parse_optional_trace_dir(args, &trace_dir)) {
    print_help(stderr);
    return 1;
  }

  return ps(trace_dir, stdout);
}

} // namespace rr
