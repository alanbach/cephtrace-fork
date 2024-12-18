//Main purpose for this client tracing
//1. observe the latency client -> osds for each read/write, if possible, including the network latency and the process latency
//   
#include <bpf/libbpf.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>
#include <signal.h>
#include <bits/stdc++.h>
#include <signal.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "radostrace.skel.h"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

#include "bpf_ceph_types.h"
#include "dwarf_parser.h"

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
using namespace std;

typedef std::map<std::string, int> func_id_t;

std::vector<std::string> probe_units = {"Objecter.cc"};

func_id_t func_id = {
      {"Objecter::_send_op", 0},
      {"Objecter::_finish_op", 20}

};


std::map<std::string, int> func_progid = {
      {"Objecter::_send_op", 0},
      {"Objecter::_finish_op", 1}

};


DwarfParser::probes_t rados_probes = {
      {"Objecter::_send_op",
       {{"op", "tid"},
	{"this", "monc", "global_id"},
        {"op", "target", "osd"},
        {"op", "target", "base_oid", "name", "_M_string_length"},
        {"op", "target", "base_oid", "name", "_M_dataplus", "_M_p"},
        {"op", "target", "flags"},
        {"op", "target", "actual_pgid", "pgid", "m_pool"},
        {"op", "target", "actual_pgid", "pgid", "m_seed"},
        {"op", "target", "acting", "_M_impl", "_M_start"},
        {"op", "target", "acting", "_M_impl", "_M_finish"},
	{"op", "ops", "m_holder", "m_start"},
	{"op", "ops", "m_holder", "m_size"}}},
        //{"op", "ops", "m_holder", "m_start", "op", "op", "v"}}},
        //{"op", "ops", "m_holder", "m_start", "op", "extent", "offset", "v"},
        //{"op", "ops", "m_holder", "m_start", "op", "extent", "length", "v"}}},

      {"Objecter::_finish_op", 
       {{"op", "tid"},
	{"this", "monc", "global_id"},
	{"op", "target", "osd"}}}
};

volatile sig_atomic_t timeout_occurred = 0;

const char * ceph_osd_op_str(int opc) {
    const char *op_str = NULL;
#define GENERATE_CASE_ENTRY(op, opcode, str)	case CEPH_OSD_OP_##op: op_str=str; break;
    switch (opc) {
    __CEPH_FORALL_OSD_OPS(GENERATE_CASE_ENTRY)
    }
    return op_str;
}

void fill_map_hprobes(std::string mod_path, DwarfParser &dwarfparser, struct bpf_map *hprobes) {
  auto &func2vf = dwarfparser.mod_func2vf[mod_path];
  for (auto x : func2vf) {
    std::string funcname = x.first;
    int key_idx = func_id[funcname];
    for (auto vf : x.second) {
      struct VarField_Kernel vfk;
      vfk.varloc = vf.varloc;
      clog << "fill_map_hprobes: "
           << "function " << funcname << " var location : register "
           << vfk.varloc.reg << " offset " << vfk.varloc.offset << " stack "
           << vfk.varloc.stack << endl;
      vfk.size = vf.fields.size();
      for (int i = 0; i < vfk.size; ++i) {
        vfk.fields[i] = vf.fields[i];
      }
      bpf_map__update_elem(hprobes, &key_idx, sizeof(key_idx), &vfk,
                           sizeof(vfk), 0);
      ++key_idx;
    }
  }
}

void signal_handler(int signum){
  clog << "Caught signal " << signum << endl;
  if (signum == SIGINT) {
      clog << "process killed" << endl;
  }
  exit(signum);
}

void timeout_handler(int signum) {
    if (signum == SIGALRM) {
        timeout_occurred = 1;
    }
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
                           va_list args) {
  if (level == LIBBPF_DEBUG) return 0;
  return vfprintf(stderr, format, args);
}

int attach_uprobe(struct radostrace_bpf *skel,
	           DwarfParser &dp,
	           std::string path,
		   std::string funcname,
		   int v = 0) {

  auto &func2pc = dp.mod_func2pc[path];
  size_t func_addr = func2pc[funcname];
  if (v > 0)
      funcname = funcname + "_v" + std::to_string(v); 
  int pid = func_progid[funcname];
  struct bpf_link *ulink = bpf_program__attach_uprobe(
      *skel->skeleton->progs[pid].prog, 
      false /* not uretprobe */,
      -1,
      path.c_str(), func_addr);
  if (!ulink) {
    cerr << "Failed to attach uprobe to " << funcname << endl;
    return -errno;
  }

  clog << "uprobe " << funcname <<  " attached" << endl;
  return 0;
}

int attach_retuprobe(struct radostrace_bpf *skel,
	           DwarfParser &dp,
	           std::string path,
		   std::string funcname,
		   int v = 0) {
  auto &func2pc = dp.mod_func2pc[path];
  size_t func_addr = func2pc[funcname];
  if (v > 0)
      funcname = funcname + "_v" + std::to_string(v); 
  int pid = func_progid[funcname];
  struct bpf_link *ulink = bpf_program__attach_uprobe(
      *skel->skeleton->progs[pid].prog, 
      true /* uretprobe */,
      -1,
      path.c_str(), func_addr);
  if (!ulink) {
    cerr << "Failed to attach uretprobe to " << funcname << endl;
    return -errno;
  }

  clog << "uretprobe " << funcname <<  " attached" << endl;
  return 0;
}

int digitnum(int x) {
  int cnt = 1;
  while(x / 10) {
    cnt++;
    x /= 10;
  }
  return cnt;
}

static int handle_event(void *ctx, void *data, size_t size) {
    struct client_op_v * op_v = (struct client_op_v *)data;
    std::stringstream ss;
    ss << std::hex << op_v->m_seed;
    std::string pgid(ss.str()); 

    static bool firsttime = true;
    if (firsttime) {
      printf("     pid  client     tid  pool  pg     acting            w/r    size  latency     other\n");
      firsttime = false;
    }


    printf("%8d%8lld%8lld%6lld%4s", 
	    op_v->pid, op_v->cid, op_v->tid,
	    op_v->m_pool, pgid.c_str()); 
    
    /*printf("pid:%d client.%lld tid %lld pgid %lld.%s object:%s %s osd.%d acting[%d %d %d] lat %lld ", 
	    op_v->pid, op_v->cid, op_v->tid, 
	    op_v->m_pool, pgid.c_str(), op_v->object_name, 
	    op_v->rw & CEPH_OSD_FLAG_WRITE ? "W" : "R",
	    op_v->target_osd, op_v->acting[0], op_v->acting[1], op_v->acting[2],
	    (op_v->finish_stamp - op_v->sent_stamp) / 1000);*/

    int acting_length = 4;
    for (int i = 0; i < 8; ++i) {
      if(op_v->acting[i] < 0) break;
      acting_length += digitnum(op_v->acting[i]);
    }
    printf("     [");
    bool first = true;
    for (int i = 0; i < 8; ++i) {
      if(op_v->acting[i] < 0) break;
      if (!first) printf(",");
      printf("%d", op_v->acting[i]);
      first = false;
    }
    printf("]");
    int left_spaces = 18 - acting_length;
    for (int i =0; i < left_spaces; ++i) printf(" "); 

    //print "WR"
    printf("%s  ", op_v->rw & CEPH_OSD_FLAG_WRITE ? "W" : "R");
    //print length
    printf("%7lld", op_v->length);

    printf("%8lld", (op_v->finish_stamp - op_v->sent_stamp) / 1000);

    printf("     %s ", op_v->object_name);

    printf("[");
    bool print_offset_length = false;
    for (int i = 0; i < op_v->ops_size; ++i) {
      if (ceph_osd_op_extent(op_v->ops[i])) {
        printf("%s ", ceph_osd_op_str(op_v->ops[i]));
        print_offset_length = true;
      } else if (ceph_osd_op_call(op_v->ops[i])) {
        printf("call(%s.%s) ", op_v->cls_ops[i].cls_name, op_v->cls_ops[i].method_name);
      } else {
        printf("%s ", ceph_osd_op_str(op_v->ops[i]));
      }
    }
    printf("]");
    if (print_offset_length) {
      printf("[%lld, %lld]\n", op_v->offset, op_v->length);
    } else {
      printf("\n");
    }
    return 0;
}


int main(int argc, char **argv) {
  signal(SIGINT, signal_handler); 

  /* Default to unlimited execution time */
  int timeout = -1;

  /* Parse arguments */
  for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if ((arg == "-t" || arg == "--timeout") && i + 1 < argc) {
          try {
              timeout = std::stoi(argv[++i]);
              if (timeout <= 0) throw std::invalid_argument("Negative timeout");
          } catch (...) {
              std::cerr << "Invalid timeout value. Must be a positive integer.\n";
              return 1;
          }
      } else if (arg == "-h" || arg == "--help") {
          std::cout << "Usage: " << argv[0] << " [-t <timeout seconds>] [--timeout <timeout seconds>]\n";
          return 0;
      }
  }

  /* Set up timeout if provided */
  if (timeout > 0) {
      signal(SIGALRM, timeout_handler);
      alarm(timeout);
      std::cout << "Execution timeout set to " << timeout << " seconds.\n";
  } else {
      std::cout << "No execution timeout set (unlimited).\n";
  }

  struct radostrace_bpf *skel;
  // long uprobe_offset;
  int ret = 0;
  struct ring_buffer *rb;

  clog << "Start to parse dwarf info" << endl;
  std::string librbd_path = "/lib/x86_64-linux-gnu/librbd.so.1";
  std::string librados_path = "/lib/x86_64-linux-gnu/librados.so.2";
  std::string libceph_common_path = "/usr/lib/x86_64-linux-gnu/ceph/libceph-common.so.2";

  DwarfParser dwarfparser(rados_probes, probe_units);
  dwarfparser.add_module(librbd_path);
  dwarfparser.add_module(librados_path);
  dwarfparser.add_module(libceph_common_path);
  dwarfparser.parse();

  libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

  /* Set up libbpf errors and debug info callback */
  libbpf_set_print(libbpf_print_fn);

  /* Load and verify BPF application */
  clog << "Start to load uprobe" << endl;

  skel = radostrace_bpf__open_and_load();
  if (!skel) {
    cerr << "Failed to open and load BPF skeleton" << endl;
    return 1;
  }

  // map_fd = bpf_object__find_map_fd_by_name(skel->obj, "hprobes");

  //fill_map_hprobes(libceph_common_path, dwarfparser, skel->maps.hprobes);
  fill_map_hprobes(librados_path, dwarfparser, skel->maps.hprobes);

  clog << "BPF prog loaded" << endl;

  attach_uprobe(skel, dwarfparser, librados_path, "Objecter::_send_op");
  attach_uprobe(skel, dwarfparser, librbd_path, "Objecter::_send_op");
  //attach_uprobe(skel, dwarfparser, libceph_common_path, "Objecter::_send_op");
  attach_uprobe(skel, dwarfparser, librados_path, "Objecter::_finish_op");
  attach_uprobe(skel, dwarfparser, librbd_path, "Objecter::_finish_op");
  //attach_uprobe(skel, dwarfparser, libceph_common_path, "Objecter::_finish_op");

  clog << "New a ring buffer" << endl;

  rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, NULL, NULL);
  if (!rb) {
    cerr << "failed to setup ring_buffer" << endl;
    goto cleanup;
  }

  clog << "Started to poll from ring buffer" << endl;

  while ((!timeout_occurred || timeout == -1) && (ret = ring_buffer__poll(rb, 1000)) >= 0) {
      // Continue polling while timeout hasn't occurred or if unlimited execution time
  }

  if (timeout_occurred) {
      cerr << "Timeout occurred. Exiting." << endl;
  }

cleanup:
  clog << "Clean up the eBPF program" << endl;
  ring_buffer__free(rb);
  radostrace_bpf__destroy(skel);
  return timeout_occurred ? -1 : -errno;
}

