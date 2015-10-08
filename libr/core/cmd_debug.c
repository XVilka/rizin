/* radare - LGPL - Copyright 2009-2015 - pancake */

#define TN_KEY_LEN 32
#define TN_KEY_FMT "%"PFMT64u

struct dot_trace_ght {
	RGraph *graph;
	Sdb *graphnodes;
};

struct trace_node {
	ut64 addr;
	int refs;
};

static int checkbpcallback(RCore *core);

static void cmd_debug_cont_syscall (RCore *core, const char *_str) {
	// TODO : handle more than one stopping syscall
	int i, *syscalls = NULL;
	int count = 0;
	if (_str && *_str) {
		char *str = strdup (_str);
		count = r_str_word_set0 (str);
		syscalls = calloc (sizeof (int), count);
		for (i=0; i<count; i++) {
			const char *sysnumstr = r_str_word_get0 (str, i);
			int sig = (int)r_num_math (core->num, sysnumstr);
			if (sig == -1) { // trace ALL syscalls
				syscalls[i] = -1;
			} else
			if (sig == 0) {
				sig = r_syscall_get_num (core->anal->syscall, sysnumstr);
				if (sig == -1) {
					eprintf ("Unknown syscall number\n");
					free (str);
					free (syscalls);
					return;
				}
				syscalls[i] = sig;
			}
		}
		eprintf ("Running child until syscalls:");
		for (i=0; i<count; i++)
			eprintf ("%d ", syscalls[i]);
		eprintf ("\n");
		free (str);
	} else {
		eprintf ("Running child until next syscall\n");
	}
	r_reg_arena_swap (core->dbg->reg, true);
	r_debug_continue_syscalls (core->dbg, syscalls, count);
	checkbpcallback (core);
	free (syscalls);
}

static RGraphNode *get_graphtrace_node (RGraph *g, Sdb *nodes, struct trace_node *tn) {
	RGraphNode *gn;
	char tn_key[TN_KEY_LEN];

	snprintf (tn_key, TN_KEY_LEN, TN_KEY_FMT, tn->addr);
	gn = (RGraphNode *)(size_t)sdb_num_get (nodes, tn_key, NULL);
	if (!gn) {
		gn = r_graph_add_node (g, tn);
		sdb_num_set (nodes, tn_key, (ut64)(size_t)gn, 0);
	}
	return gn;
}

static void dot_trace_create_node (RTreeNode *n, RTreeVisitor *vis) {
	struct dot_trace_ght *data = (struct dot_trace_ght *)vis->data;
	struct trace_node *tn = n->data;

	if (tn)
		get_graphtrace_node (data->graph, data->graphnodes, tn);
}

static void dot_trace_discover_child (RTreeNode *n, RTreeVisitor *vis) {
	struct dot_trace_ght *data = (struct dot_trace_ght *)vis->data;
	RGraph *g = data->graph;
	Sdb *gnodes = data->graphnodes;
	RTreeNode *parent = n->parent;
	struct trace_node *tn = n->data;
	struct trace_node *tn_parent = parent->data;

	if (tn && tn_parent) {
		RGraphNode *gn = get_graphtrace_node (g, gnodes, tn);
		RGraphNode *gn_parent = get_graphtrace_node (g, gnodes, tn_parent);

		if (!r_graph_adjacent (g, gn_parent, gn))
			r_graph_add_edge (g, gn_parent, gn);
	}
}

static void dot_trace_traverse(RCore *core, RTree *t) {
	const char *gfont = r_config_get (core->config, "graph.font");
	struct dot_trace_ght aux_data;
	RTreeVisitor vis = { 0 };
	const RList *nodes;
	RListIter *iter;
	RGraphNode *n;

	aux_data.graph = r_graph_new ();
	aux_data.graphnodes = sdb_new0 ();

	/* build a callgraph from the execution trace */
	vis.data = &aux_data;
	vis.pre_visit = (RTreeNodeVisitCb)dot_trace_create_node;
	vis.discover_child = (RTreeNodeVisitCb)dot_trace_discover_child;
	r_tree_bfs (t, &vis);

	/* traverse the callgraph to print the dot file */
	nodes = r_graph_get_nodes (aux_data.graph);
	r_cons_printf ("digraph code {\n"
		"graph [bgcolor=white];\n"
		"    node [color=lightgray, style=filled"
		" shape=box fontname=\"%s\" fontsize=\"8\"];\n", gfont);
	r_list_foreach (nodes, iter, n) {
		struct trace_node *tn = (struct trace_node *)n->data;
		const RList *neighbours = r_graph_get_neighbours (aux_data.graph, n);
		RListIter *it_n;
		RGraphNode *w;

		if (tn) {
			r_cons_printf ("\"0x%08"PFMT64x"\" [URL=\"0x%08"PFMT64x
					"\" color=\"lightgray\" label=\"0x%08"PFMT64x
					" (%d)\"]\n", tn->addr, tn->addr, tn->addr, tn->refs);
		}
		r_list_foreach (neighbours, it_n, w) {
			 struct trace_node *tv = (struct trace_node *)w->data;

			 if (tv && tn) {
				 r_cons_printf ("\"0x%08"PFMT64x"\" -> \"0x%08"PFMT64x
						 "\" [color=\"red\"];\n", tn->addr, tv->addr);
			 }
		}
	}
	r_cons_printf ("}\n");

	r_graph_free (aux_data.graph);
	sdb_free (aux_data.graphnodes);
}

/* TODO: refactor all those step_until* function into a single one
 * TODO: handle when the process is dead
 * TODO: handle ^C */

static int checkbpcallback(RCore *core) ;
static int step_until(RCore *core, ut64 addr) {
	ut64 off = r_debug_reg_get (core->dbg, "pc");
	if (off == 0LL) {
		eprintf ("Cannot 'drn pc'\n");
		return false;
	}
	if (addr == 0LL) {
		eprintf ("Cannot continue until address 0\n");
		return false;
	}
	r_cons_break (NULL, NULL);
	do {
		if (r_cons_singleton ()->breaked)
			break;
		if (r_debug_is_dead (core->dbg))
			break;
		r_debug_step (core->dbg, 1);
		if (checkbpcallback (core)) {
			eprintf ("Interrupted by a breakpoint\n");
			break;
		}
		off = r_debug_reg_get (core->dbg, "pc");
		// check breakpoint here
	} while (off != addr);
	r_cons_break_end();
	return true;
}

static int step_until_esil(RCore *core, const char *esilstr) {
	if (!core || !esilstr || !core->dbg || !core->dbg->anal \
			|| !core->dbg->anal->esil) {
		eprintf ("Not initialized %p. Run 'aei' first.\n", core->anal->esil);
		return false;
	}
	r_cons_break (NULL, NULL);
	for (;;) {
		if (r_cons_singleton ()->breaked)
			break;
		if (r_debug_is_dead (core->dbg))
			break;
		r_debug_step (core->dbg, 1);
		r_debug_reg_sync (core->dbg, -1, 0);
		if (checkbpcallback (core)) {
			eprintf ("Interrupted by a breakpoint\n");
			break;
		}
		if (r_anal_esil_condition (core->anal->esil, esilstr)) {
			eprintf ("ESIL BREAK!\n");
			break;
		}
	}
	r_cons_break_end();
	return true;
}

static int step_until_inst(RCore *core, const char *instr) {
	RAsmOp asmop;
	ut8 buf[32];
	ut64 pc;
	int ret;

	instr = r_str_chop_ro (instr);
	if (!core || !instr|| !core->dbg) {
		eprintf ("Wrong state\n");
		return false;
	}
	r_cons_break (NULL, NULL);
	for (;;) {
		if (r_cons_singleton ()->breaked)
			break;
		if (r_debug_is_dead (core->dbg))
			break;
		r_debug_step (core->dbg, 1);
		r_debug_reg_sync (core->dbg, -1, 0);
		if (checkbpcallback (core)) {
			eprintf ("Interrupted by a breakpoint\n");
			break;
		}
		/* TODO: disassemble instruction and strstr */
		pc = r_debug_reg_get (core->dbg, "pc");
		r_asm_set_pc (core->assembler, pc);
		// TODO: speedup if instructions are in the same block as the previous
		r_io_read_at (core->io, pc, buf, sizeof (buf));
		ret = r_asm_disassemble (core->assembler, &asmop, buf, sizeof (buf));
		eprintf ("0x%08"PFMT64x" %d %s\n", pc, ret, asmop.buf_asm);
		if (ret>0) {
			if (strstr (asmop.buf_asm, instr)) {
				eprintf ("Stop.\n");
				break;
			}
		}
	}
	r_cons_break_end();
	return true;
}

static int step_until_flag(RCore *core, const char *instr) {
	const RList *list;
	RListIter *iter;
	RFlagItem *f;
	ut64 pc;

	instr = r_str_chop_ro (instr);
	if (!core || !instr|| !core->dbg) {
		eprintf ("Wrong state\n");
		return false;
	}
	r_cons_break (NULL, NULL);
	for (;;) {
		if (r_cons_singleton ()->breaked)
			break;
		if (r_debug_is_dead (core->dbg))
			break;
		r_debug_step (core->dbg, 1);
		r_debug_reg_sync (core->dbg, -1, 0);
		if (checkbpcallback (core)) {
			eprintf ("Interrupted by a breakpoint\n");
			break;
		}
		pc = r_debug_reg_get (core->dbg, "pc");
		list = r_flag_get_list (core->flags, pc);
		r_list_foreach (list, iter, f) {
			if (!instr|| !*instr || strstr(f->realname, instr)) {
				r_cons_printf ("[ 0x%08"PFMT64x" ] %s\n",
					f->offset, f->realname);
				goto beach;
			}
		}
	}
beach:
	r_cons_break_end();
	return true;
}

/* until end of frame */
static int step_until_eof(RCore *core) {
	ut64 off, now = r_debug_reg_get (core->dbg, "sp");
	r_cons_break (NULL, NULL);
	do {
		if (r_cons_singleton ()->breaked)
			break;
		if (!r_debug_step (core->dbg, 1))
			break;
		if (checkbpcallback (core)) {
			eprintf ("Interrupted by a breakpoint\n");
			break;
		}
		off = r_debug_reg_get (core->dbg, "sp");
		// check breakpoint here
	} while (off <= now);
	r_cons_break_end();
	return true;
}

static int step_line(RCore *core, int times) {
	char file[512], file2[512];
	int find_meta, line = -1, line2 = -1;
	char *tmp_ptr = NULL;
	ut64 off = r_debug_reg_get (core->dbg, "pc");
	if (off == 0LL) {
		eprintf ("Cannot 'drn pc'\n");
		return false;
	}
	file[0] = 0;
	file2[0] = 0;
	if (r_bin_addr2line (core->bin, off, file, sizeof (file), &line)) {
		char* ptr = r_file_slurp_line (file, line, 0);
		eprintf ("--> 0x%08"PFMT64x" %s : %d\n", off, file, line);
		eprintf ("--> %s\n", ptr);
		find_meta = false;
		free (ptr);
	} else {
		eprintf ("--> Stepping until dwarf line\n");
		find_meta = true;
	}
	do {
		r_debug_step (core->dbg, 1);
		if (checkbpcallback (core)) {
			eprintf ("Interrupted by a breakpoint\n");
			break;
		}
		off = r_debug_reg_get (core->dbg, "pc");
		if (!r_bin_addr2line (core->bin, off, file2, sizeof (file2), &line2)) {
			if (find_meta)
				continue;
			eprintf ("Cannot retrieve dwarf info at 0x%08"PFMT64x"\n", off);
			return false;
		}
	} while (!strcmp (file, file2) && line == line2);

	eprintf ("--> 0x%08"PFMT64x" %s : %d\n", off, file2, line2);
	tmp_ptr = r_file_slurp_line (file2, line2, 0);
	eprintf ("--> %s\n", tmp_ptr);
	free (tmp_ptr);

	return true;
}

static void cmd_debug_pid(RCore *core, const char *input) {
	int pid, sig;
	const char *ptr, *help_msg[] = {
		"Usage:", "dp", " # Process commands",
		"dp", "", "List current pid and childrens",
		"dp", " <pid>", "List children of pid",
		"dp*", "", "List all attachable pids",
		"dp=", "<pid>", "Select pid",
		"dpa", " <pid>", "Attach and select pid",
		"dpe", "", "Show path to executable",
		"dpf", "", "Attach to pid like file fd // HACK",
		"dpk", " <pid> <signal>", "Send signal to process",
		"dpn", "", "Create new process (fork)",
		"dpnt", "", "Create new thread (clone)",
		"dpt", "", "List threads of current pid",
		"dpt", " <pid>", "List threads of process",
		"dpt=", "<thread>", "Attach to thread",
		NULL};
	switch (input[1]) {
	case 'k':
		/* stop, print, pass -- just use flags*/
		/* XXX: not for threads? signal is for a whole process!! */
		/* XXX: but we want fine-grained access to process resources */
		pid = atoi (input+2);
		ptr = strchr (input, ' ');
		sig = ptr? atoi (ptr+1): 0;
		if (pid > 0) {
			eprintf ("Sending signal '%d' to pid '%d'\n", sig, pid);
			r_debug_kill (core->dbg, 0, false, sig);
		} else eprintf ("cmd_debug_pid: Invalid arguments (%s)\n", input);
		break;
	case 'n':
		eprintf ("TODO: debug_fork: %d\n", r_debug_child_fork (core->dbg));
		break;
	case 't':
		switch (input[2]) {
		case 'n':
			eprintf ("TODO: debug_clone: %d\n", r_debug_child_clone (core->dbg));
			break;
		case '=':
		case ' ':
			r_debug_select (core->dbg, core->dbg->pid,
				(int) r_num_math (core->num, input+3));
			break;
		default:
			r_debug_thread_list (core->dbg, core->dbg->pid);
			break;
		}
		break;
	case 'a':
		if (input[2]) {
			r_debug_attach (core->dbg, (int) r_num_math (
				core->num, input+2));
		} else {
			if (core->file && core->file->desc) {
				r_debug_attach (core->dbg, core->file->desc->fd);
			}
		}
		r_debug_select (core->dbg, core->dbg->pid, core->dbg->tid);
		r_config_set_i (core->config, "dbg.swstep",
			(core->dbg->h && !core->dbg->h->canstep));
		r_core_cmdf (core, "=!pid %d", core->dbg->pid);
		break;
	case 'f':
		if (core->file && core->file->desc) {
			r_debug_select (core->dbg, core->file->desc->fd, core->dbg->tid);
		}
		break;
	case '=':
		r_debug_select (core->dbg,
			(int) r_num_math (core->num, input+2), core->dbg->tid);
		break;
	case '*':
		r_debug_pid_list (core->dbg, 0, 0);
		break;
	case 'j':
		r_debug_pid_list (core->dbg, core->dbg->pid, 'j');
		break;
	case 'e':
		{
			int pid = (input[2] == ' ')? atoi(input+2): core->dbg->pid;
			char *exe = r_sys_pid_to_path (pid);
			if (exe) {
				r_cons_printf ("%s\n", exe);
				free (exe);
			}
		}
		break;
	case ' ':
		r_debug_pid_list (core->dbg,
			(int) R_MAX (0, (int)r_num_math (core->num, input+2)), 0);
		break;
	case '?':
		r_core_cmd_help (core, help_msg);
		break;
	default:
		eprintf ("Selected: %d %d\n", core->dbg->pid, core->dbg->tid);
		r_debug_pid_list (core->dbg, core->dbg->pid, 0);
		break;
	}
}

static void cmd_debug_backtrace (RCore *core, const char *input) {
	RAnalOp analop;
	ut64 addr, len = r_num_math (core->num, input);
	if (len == 0) {
		r_bp_traptrace_list (core->dbg->bp);
	} else {
		ut64 oaddr = 0LL;
		eprintf ("Trap tracing 0x%08"PFMT64x"-0x%08"PFMT64x"\n",
			core->offset, core->offset+len);
		r_reg_arena_swap (core->dbg->reg, true);
		r_bp_traptrace_reset (core->dbg->bp, true);
		r_bp_traptrace_add (core->dbg->bp, core->offset, core->offset+len);
		r_bp_traptrace_enable (core->dbg->bp, true);
		do {
			ut8 buf[32];
			r_debug_continue (core->dbg);
			if (checkbpcallback (core)) {
				eprintf ("Interrupted by breakpoint\n");
				break;
			}
			addr = r_debug_reg_get (core->dbg, "pc");
			if (addr == 0LL) {
				eprintf ("pc=0\n");
				break;
			}
			if (addr == oaddr) {
				eprintf ("pc=opc\n");
				break;
			}
			oaddr = addr;
			/* XXX Bottleneck..we need to reuse the bytes read by traptrace */
			// XXX Do asm.arch should define the max size of opcode?
			r_core_read_at (core, addr, buf, 32); // XXX longer opcodes?
			r_anal_op (core->anal, &analop, addr, buf, sizeof (buf));
		} while (r_bp_traptrace_at (core->dbg->bp, addr, analop.size));
		r_bp_traptrace_enable (core->dbg->bp, false);
	}
}

static int __r_debug_snap_diff(RCore *core, int idx) {
	ut32 count = 0;
	RDebug *dbg = core->dbg;
	ut32 oflags = core->print->flags;
	int col = core->cons->columns>123;
	RDebugSnap *snap;
	RListIter *iter;
	core->print->flags |= R_PRINT_FLAGS_DIFFOUT;
	r_list_foreach (dbg->snaps, iter, snap) {
		if (count == idx) {
			ut8 *b = malloc (snap->size);
			if (!b) {
				eprintf ("Cannot allocate snapshot\n");
				continue;
			}
			dbg->iob.read_at (dbg->iob.io, snap->addr, b , snap->size);
                        r_print_hexdiff (core->print,
				snap->addr, snap->data,
				snap->addr, b,
				snap->size, col);
                        free (b);
		}
		count ++;
	}
	core->print->flags = oflags;
	return 0;
}

static int cmd_debug_map_snapshot(RCore *core, const char *input) {
	const char* help_msg[] = {
		"Usage:", "dms", " # Memory map snapshots",
		"dms", "", "List memory snapshots",
		"dmsj", "", "list snapshots in JSON",
		"dms*", "", "list snapshots in r2 commands",
		"dms", " addr", "take snapshot with given id of map at address",
		"dms", "-id", "delete memory snapshot",
		"dmsC", " id comment", "add comment for given snapshot",
		"dmsd", " id", "hexdiff given snapshot. See `ccc`.",
		"dmsw", "", "snapshot of the writable maps",
		"dmsa", "", "full snapshot of all `dm` maps",
		"dmsf", " [file] @ addr", "read snapshot from disk",
		"dmst", " [file] @ addr", "dump snapshot to disk",
		// TODO: dmsj - for json
		NULL
	};
	switch (*input) {
	case 'f':
		{
		char *file;
		RDebugSnap *snap;
		if (input[1] == ' ') {
			file = strdup (input+2);
		} else {
			file = r_str_newf ("0x%08"PFMT64x".dump", core->offset);
		}
		snap = r_debug_snap_get (core->dbg, core->offset);
		if (!snap) {
			r_debug_snap (core->dbg, core->offset);
			snap = r_debug_snap_get (core->dbg, core->offset);
		}
		if (snap) {
			int fsz = 0;
			char *data = r_file_slurp (file, &fsz);
			if (data) {
				if (fsz >= snap->size) {
					memcpy (snap->data, data, snap->size);
				} else {
					eprintf ("This file is smaller than the snapshot size\n");
				}
				free (data);
			} else eprintf ("Cannot slurp '%s'\n", file);
		} else {
			eprintf ("Unable to find a snapshot for 0x%08"PFMT64x"\n", core->offset);
		}
		free (file);
		}
		break;
	case 't':
		{
		char *file;
		RDebugSnap *snap;
		if (input[1] == ' ') {
			file = strdup (input+2);
		} else {
			file = r_str_newf ("0x%08"PFMT64x".dump", core->offset);
		}
		snap = r_debug_snap_get (core->dbg, core->offset);
		if (snap) {
			if (!r_file_dump (file, snap->data, snap->size, 0)) {
				eprintf ("Cannot slurp '%s'\n", file);
			}
		} else {
			eprintf ("Unable to find a snapshot for 0x%08"PFMT64x"\n", core->offset);
		}
		free (file);
		}
		break;
	case '?':
		r_core_cmd_help (core, help_msg);
		break;
	case '-':
		if (input[1]=='*') {
			r_debug_snap_delete (core->dbg, -1);
		} else {
			r_debug_snap_delete (core->dbg, r_num_math (core->num, input+1));
		}
		break;
	case ' ':
		r_debug_snap (core->dbg, r_num_math (core->num, input+1));
		break;
	case 'C':
		r_debug_snap_comment (core->dbg, atoi (input+1), strchr (input, ' '));
		break;
	case 'd':
		__r_debug_snap_diff (core, atoi (input+1));
		break;
	case 'a':
		r_debug_snap_all (core->dbg, 0);
		break;
	case 'w':
		r_debug_snap_all (core->dbg, R_IO_RW);
		break;
	case 0:
	case 'j':
	case '*':
		r_debug_snap_list (core->dbg, -1, input[0]);
		break;
	}
	return 0;
}

#define MAX_MAP_SIZE 1024*1024*512
static int dump_maps(RCore *core, int perm, const char *filename) {
	RDebugMap *map;
	char file[128];
	RListIter *iter;
	r_debug_map_sync (core->dbg); // update process memory maps
	ut64 addr = core->offset;
	int do_dump = false;
	int ret = r_list_empty(core->dbg->maps)? false: true;
	r_list_foreach (core->dbg->maps, iter, map) {
		do_dump = false;
		if (perm == -1) {
			if (addr >= map->addr && addr < map->addr_end) {
				do_dump = true;
			}
		} else if (perm == 0) {
			do_dump = true;
		} else if (perm == (map->perm & perm)) {
			do_dump = true;
		}
		if (do_dump) {
			ut8 *buf = malloc (map->size);
			//TODO: use mmap here. we need a portable implementation
			if (!buf) {
				eprintf ("Cannot allocate 0x%08"PFMT64x" bytes\n", map->size);
				free (buf);
				/// XXX: TODO: read by blocks!!1
				continue;
			}
			if (map->size > MAX_MAP_SIZE) {
				eprintf ("Do not dumping 0x%08"PFMT64x" because it's too big\n", map->addr);
				free (buf);
				continue;
			}
			r_io_read_at (core->io, map->addr, buf, map->size);
			if (filename) {
				snprintf (file, sizeof (file), "%s", filename);
			} else snprintf (file, sizeof (file),
				"0x%08"PFMT64x"-0x%08"PFMT64x"-%s.dmp",
			map->addr, map->addr_end, r_str_rwx_i (map->perm));
			if (!r_file_dump (file, buf, map->size, 0)) {
				eprintf ("Cannot write '%s'\n", file);
				ret = 0;
			} else {
				eprintf ("Dumped %d bytes into %s\n", (int)map->size, file);
			}
			free (buf);
		}
	}
	//eprintf ("No debug region found here\n");
	return ret;
}

static void cmd_debug_modules(RCore *core, int mode) { // "dmm"
	ut64 addr = core->offset;
	RDebugMap *map;
	RList *list;
	RListIter *iter;

	if (mode == '?') {
		eprintf ("Usage: dmm[j*]\n");
		return;
	}
	if (mode == 'j') {
		r_cons_printf ("[");
	}
	// TODO: honor mode
	list = r_debug_modules_list (core->dbg);
	r_list_foreach (list, iter, map) {
		switch (mode) {
		case '.':
			if (addr >= map->addr && addr < map->addr_end) {
				r_cons_printf ("0x%08"PFMT64x" %s\n",
					map->addr, map->file);
				goto beach;
			}
			break;
		case 'j':
			r_cons_printf ("{\"address\":%"PFMT64d",\"file\":\"%s\"}%s",
				map->addr, map->file, iter->n?",":"");
			break;
		case '*':
			{
				char *fn = strdup (map->file);
				r_name_filter (fn, 0);
				r_cons_printf ("f mod.%s = 0x%08"PFMT64x"\n",
					fn, map->addr);
				free (fn);
			}
			break;
		default:
			r_cons_printf ("0x%08"PFMT64x" %s\n", map->addr, map->file);
		}
	}
beach:
	if (mode == 'j') {
		r_cons_printf ("]\n");
	}
	r_list_free (list);
}

static int cmd_debug_map(RCore *core, const char *input) {
	const char* help_msg[] = {
		"Usage:", "dm", " # Memory maps commands",
		"dm", "", "List memory maps of target process",
		"dm", " <address> <size>", "Allocate <size> bytes at <address> (anywhere if address is -1) in child process",
		"dm.", "", "Show map name of current address",
		"dm*", "", "List memmaps in radare commands",
		"dm-", "<address>", "Deallocate memory map of <address>",
		"dmd", "[a] [file]", "Dump current (all) debug map region to a file (from-to.dmp) (see Sd)",
		"dmi", " [addr|libname] [symname]", "List symbols of target lib",
		"dmi*", " [addr|libname] [symname]", "List symbols of target lib in radare commands",
		"dmj", "", "List memmaps in JSON format",
		"dml", " <file>", "Load contents of file into the current map region (see Sl)",
		"dmm", "[j*]", "List modules (libraries, binaries loaded in memory)",
		"dmp", " <address> <size> <perms>", "Change page at <address> with <size>, protection <perms> (rwx)",
		"dms", " <id> <mapaddr>", "take memory snapshot",
		"dms-", " <id> <mapaddr>", "restore memory snapshot",
		//"dm, " rw- esp 9K", "set 9KB of the stack as read+write (no exec)",
		"TODO:", "", "map files in process memory. (dmf file @ [addr])",
		NULL};
	RListIter *iter;
	RDebugMap *map;
	ut64 addr = core->offset;

	switch (input[0]) {
	case 's':
		cmd_debug_map_snapshot (core, input+1);
		break;
	case '.':
		r_list_foreach (core->dbg->maps, iter, map) {
			if (addr >= map->addr && addr < map->addr_end) {
				r_cons_printf ("%s\n", map->name);
				break;
			}
		}
		break;
	case 'm': // "dmm"
		cmd_debug_modules (core, input[1]);
		break;
	case '?':
		r_core_cmd_help (core, help_msg);
		break;
	case 'p':
		if (input[1] == ' ') {
			int perms;
			char *p, *q;
			ut64 size, addr;
			p = strchr (input+2, ' ');
			if (p) {
				*p++ = 0;
				q = strchr (p, ' ');
				if (q) {
					*q++ = 0;
					addr = r_num_math (core->num, input+2);
					size = r_num_math (core->num, p);
					perms = r_str_rwx (q);
					eprintf ("(%s)(%s)(%s)\n", input+2, p, q);
					eprintf ("0x%08"PFMT64x" %d %o\n", addr, (int) size, perms);
					r_debug_map_protect (core->dbg, addr, size, perms);
				} else eprintf ("See dm?\n");
			} else eprintf ("See dm?\n");
		} else eprintf ("See dm?\n");
		break;
	case 'd':
		switch (input[1]) {
		case 'a': return dump_maps (core, 0, NULL);
		case 'w': return dump_maps (core, R_IO_RW, NULL);
		case ' ': return dump_maps (core, -1, input+2);
		case 0: return dump_maps (core, -1, NULL);
		case '?':
		default:
			eprintf ("Usage: dmd[aw]  - dump (all-or-writable) debug maps\n");
			break;
		}
		break;
	case 'l':
		if (input[1] != ' ') {
			eprintf ("Usage: dml [file]\n");
			return false;
		}
		r_debug_map_sync (core->dbg); // update process memory maps
		r_list_foreach (core->dbg->maps, iter, map) {
			if (addr >= map->addr && addr < map->addr_end) {
				int sz;
				char *buf = r_file_slurp (input+2, &sz);
				//TODO: use mmap here. we need a portable implementation
				if (!buf) {
					eprintf ("Cannot allocate 0x%08"PFMT64x" bytes\n", map->size);
					return false;
				}
				r_io_write_at (core->io, map->addr, (const ut8*)buf, sz);
				if (sz != map->size)
					eprintf	("File size differs from region size (%d vs %"PFMT64d")\n",
						sz, map->size);
				eprintf ("Loaded %d bytes into the map region at 0x%08"PFMT64x"\n",
					sz, map->addr);
				free (buf);
				return true;
			}
		}
		eprintf ("No debug region found here\n");
		return false;
	case 'i':
		{ // Move to a separate function
		RCoreBinFilter filter;
		const char *libname = NULL, *symname = NULL;
		char *ptr = strdup (r_str_trim_head ((char*)input+2));
		int i;
		ut64 baddr;

		addr = 0LL;
		i = r_str_word_set0 (ptr);
		switch (i) {
		case 2: // get symname
			symname = r_str_word_get0 (ptr, 1);
		case 1: // get addr|libname
			addr = r_num_math (core->num, r_str_word_get0 (ptr, 0));
			if (!addr) libname = r_str_word_get0 (ptr, 0);
		}
		r_debug_map_sync (core->dbg); // update process memory maps
		r_list_foreach (core->dbg->maps, iter, map) {
			if (core->bin &&
				((addr != -1 && (addr >= map->addr && addr < map->addr_end)) ||
				(libname != NULL && (strstr (map->name, libname))))) {
				filter.offset = 0LL;
				filter.name = (char *)symname;
				baddr = r_bin_get_baddr (core->bin);
				r_bin_set_baddr (core->bin, map->addr);
				r_core_bin_info (core, R_CORE_BIN_ACC_SYMBOLS, (input[1]=='*'),
						true, &filter, NULL);
				r_bin_set_baddr (core->bin, baddr);
				break;
			}
		}
		free (ptr);
		}
		break;
	case ' ':
		{
			char *p;
			int size;
			p = strchr (input+2, ' ');
			if (p) {
				*p++ = 0;
				addr = r_num_math (core->num, input+1);
				size = r_num_math (core->num, p);
				r_debug_map_alloc(core->dbg, addr, size);
			} else {
				eprintf ("Usage: dm addr size\n");
				return false;
			}
		}
		break;
	case '-':
		addr = r_num_math (core->num, input+2);
		r_list_foreach (core->dbg->maps, iter, map) {
			if (addr >= map->addr && addr < map->addr_end) {
				r_debug_map_dealloc(core->dbg, map);
				r_debug_map_sync (core->dbg);
				return true;
			}
		}
		eprintf ("The address doesn't match with any map.\n");
		break;
	case '\0':
	case '*':
	case 'j':
		r_debug_map_sync (core->dbg); // update process memory maps
		r_debug_map_list (core->dbg, core->offset, input[0]);
		break;
	}
	return true;
}

R_API void r_core_debug_rr (RCore *core, RReg *reg) {
	ut64 value;
	int bits = core->assembler->bits;
	RList *list = r_reg_get_list (reg, R_REG_TYPE_GPR);
	RListIter *iter;
	RRegItem *r;
	r_debug_map_sync (core->dbg);
	r_list_foreach (list, iter, r) {
		char *rrstr;
		if (r->size != bits) continue;
		value = r_reg_get_value (core->dbg->reg, r);
		rrstr = r_core_anal_hasrefs (core, value);
		if (bits == 64) {
			r_cons_printf ("%6s 0x%016"PFMT64x, r->name, value);
		} else {
			r_cons_printf ("%6s 0x%08"PFMT64x, r->name, value);
		}
		if (rrstr) {
			r_cons_printf (" %s\n", rrstr);
			free (rrstr);
		}
	}
}

static void cmd_debug_reg(RCore *core, const char *str) {
	int size, i, type = R_REG_TYPE_GPR;
	int bits = (core->dbg->bits & R_SYS_BITS_64)? 64: 32;
	int use_colors = r_config_get_i(core->config, "scr.color");
	const char *use_color;
	if (use_colors) {
#undef ConsP
#define ConsP(x) (core->cons && core->cons->pal.x)? core->cons->pal.x
		use_color = ConsP(creg): Color_BWHITE;
	} else {
		use_color = NULL;
	}
	struct r_reg_item_t *r;
	const char *name;
	char *arg;
	switch (str[0]) {
	case '-':
		r_debug_reg_list (core->dbg, R_REG_TYPE_GPR, bits, '-', 0);
		break;
	case '?':
		if (str[1]) {
			const char *p = str+1;
			ut64 off;
			while (IS_WHITESPACE (*p)) p++;
			r_debug_reg_sync (core->dbg, -1, 0); //R_REG_TYPE_GPR, false);
			off = r_debug_reg_get (core->dbg, p);
	//		r = r_reg_get (core->dbg->reg, str+1, 0);
	//		if (r == NULL) eprintf ("Unknown register (%s)\n", str+1);
			r_cons_printf ("0x%08"PFMT64x"\n", off);
			core->num->value = off;
			//r_reg_get_value (core->dbg->reg, r));
		} else {
			const char * help_message[] = {
				"Usage: dr", "", "Registers commands",
				"dr", "", "Show 'gpr' registers",
				"dr", " <register>=<val>", "Set register value",
				"dr=", "", "Show registers in columns",
				"dr?", "<register>", "Show value of given register",
				"drb", " [type]", "Display hexdump of gpr arena (WIP)",
				"drc", " [name]", "Related to conditional flag registers",
				"drd", "", "Show only different registers",
				"drl", "", "List all register names",
				"drn", " <pc>", "Get regname for pc,sp,bp,a0-3,zf,cf,of,sg",
				"dro", "", "Show previous (old) values of registers",
				"drp", " <file>", "Load register metadata file",
				"drp", "", "Display current register profile",
				"drr", "", "Show registers references (telescoping)",
				"drs", " [?]", "Stack register states",
				"drt", "", "Show all register types",
				"drt", " flg", "Show flag registers",
				"drt", " all", "Show all registers",
				"drt", " 16", "Show 16 bit registers",
				"drt", " 32", "Show 32 bit registers",
				"drt", " 80", "Show 80 bit registers (long double)",
				"drx", "", "Show all debug registers",
				"drx", " idx addr len rwx", "Modify hardware breakpoint",
				"drx-", "number", "Clear hardware breakpoint",
				"drf","","show fpu registers (80 bit long double)",
				"drm","","show multimedia packed registers",
				"drm"," mmx0 0 32 = 12","set the first 32 bit word of the mmx reg to 12",
				".dr", "*", "Include common register values in flags",
				".dr", "-", "Unflag all registers",
				NULL
			};
			// TODO: 'drs' to swap register arenas and display old register valuez

			r_core_cmd_help (core, help_message);
		}
		break;
	case 'l':
		//r_core_cmd0 (core, "drp~[1]");
		{
			RRegSet *rs = r_reg_regset_get (core->dbg->reg, R_REG_TYPE_GPR);
			if (rs) {
				RRegItem *r;
				RListIter *iter;
				r_list_foreach (rs->regs, iter, r) {
					r_cons_printf ("%s\n", r->name);
				}
			}
		}
		break;
	case 'b':
		{ // WORK IN PROGRESS // DEBUG COMMAND
		int len;
		const ut8 *buf = r_reg_get_bytes (core->dbg->reg, R_REG_TYPE_GPR, &len);
		//r_print_hexdump (core->print, 0LL, buf, len, 16, 16);
		r_print_hexdump (core->print, 0LL, buf, len, 32, 4);
		}
		break;
	case 'c':
// TODO: set flag values with drc zf=1
		{
		RRegItem *r;
		const char *name = str+1;
		while (*name==' ') name++;
		if (*name && name[1]) {
			r = r_reg_cond_get (core->dbg->reg, name);
			if (r) {
				r_cons_printf ("%s\n", r->name);
			} else {
				int id = r_reg_cond_from_string (name);
				RRegFlags* rf = r_reg_cond_retrieve (core->dbg->reg, NULL);
				if (rf) {
					int o = r_reg_cond_bits (core->dbg->reg, id, rf);
					core->num->value = o;
					// ORLY?
					r_cons_printf ("%d\n", o);
free (rf);
				} else eprintf ("unknown conditional or flag register\n");
			}
		} else {
			RRegFlags *rf = r_reg_cond_retrieve (core->dbg->reg, NULL);
			if (rf) {
				r_cons_printf ("| s:%d z:%d c:%d o:%d p:%d\n",
					rf->s, rf->z, rf->c, rf->o, rf->p);
				if (*name=='=') {
					for (i=0; i<R_REG_COND_LAST; i++) {
						r_cons_printf ("%s:%d ",
							r_reg_cond_to_string (i),
							r_reg_cond_bits (core->dbg->reg, i, rf));
					}
					r_cons_newline ();
				} else {
					for (i=0; i<R_REG_COND_LAST; i++) {
						r_cons_printf ("%d %s\n",
							r_reg_cond_bits (core->dbg->reg, i, rf),
							r_reg_cond_to_string (i));
					}
				}
				free (rf);
			}
		}
		}
		break;
	case 'x':
		switch (str[1]) {
		case '-':
			r_debug_reg_sync (core->dbg, R_REG_TYPE_DRX, false);
			r_debug_drx_unset (core->dbg, atoi (str+2));
			r_debug_reg_sync (core->dbg, R_REG_TYPE_DRX, true);
			break;
		case ' ': {
			char *s = strdup (str+2);
			char sl, n, rwx;
			int len;
			ut64 off;

			sl = r_str_word_set0 (s);
			if (sl == 4) {
#define ARG(x) r_str_word_get0(s,x)
				n = (char)r_num_math (core->num, ARG(0));
				off = r_num_math (core->num, ARG(1));
				len = (int)r_num_math (core->num, ARG(2));
				rwx = (char)r_str_rwx (ARG(3));
				if (len== -1) {
					r_debug_reg_sync (core->dbg, R_REG_TYPE_DRX, false);
					r_debug_drx_set (core->dbg, n, 0, 0, 0, 0);
					r_debug_reg_sync (core->dbg, R_REG_TYPE_DRX, true);
				} else {
					r_debug_reg_sync (core->dbg, R_REG_TYPE_DRX, false);
					r_debug_drx_set (core->dbg, n, off, len, rwx, 0);
					r_debug_reg_sync (core->dbg, R_REG_TYPE_DRX, true);
				}
			} else eprintf ("|Usage: drx N [address] [length] [rwx]\n");
			free (s);
			} break;
		case '\0':
			r_debug_reg_sync (core->dbg, R_REG_TYPE_DRX, false);
			r_debug_drx_list (core->dbg);
			break;
		default: {
			const char * help_message[] = {
				"Usage: drx", "", "Hardware breakpoints commands",
				"drx", "", "List all (x86?) hardware breakpoints",
				"drx", " <number> <address> <length> <perms>", "Modify hardware breakpoint",
				"drx-", "<number>", "Clear hardware breakpoint",
				NULL
			};
			r_core_cmd_help (core, help_message);
			}
			break;
		}
		break;
	case 's': // "drs"
		switch (str[1]) {
		case '-':
			r_reg_arena_pop (core->dbg->reg);
			// restore debug registers if in debugger mode
			r_debug_reg_sync (core->dbg, 0, 1);
			break;
		case '+':
			r_reg_arena_push (core->dbg->reg);
			break;
		case '?': {
			const char * help_message[] = {
				"Usage: drs", "", "Register states commands",
				"drs", "", "List register stack",
				"drs", "+", "Push register state",
				"drs", "-", "Pop register state",
				NULL
			};
			r_core_cmd_help (core, help_message);
			}
			break;
		default:
			r_cons_printf ("%d\n", r_list_length (
				core->dbg->reg->regset[0].pool));
			break;
		}
		break;
	case 'm': // "drm"
		if (str[1]=='?') {
			eprintf ("Usage: drm [reg] [idx] [wordsize] [= value]\n");
		} else if (str[1]==' ') {
			int word = 0;
			int size = 0; // auto
			char *q, *p, *name = strdup (str+2);
			char *eq = strchr (name, '=');
			if (eq) {
				*eq++ = 0;
			}
			p = strchr (name, ' ');
			if (p) {
				*p++ = 0;
				q = strchr (p, ' ');
				if (q) {
					*q++ = 0;
					size = r_num_math (core->num, q);
				}
				word = r_num_math (core->num, p);
			}
			RRegItem *item = r_reg_get (core->dbg->reg, name, -1);
			if (item) {
				if (eq) {
					ut64 val = r_num_math (core->num, eq);
					r_reg_set_pack (core->dbg->reg, item, word, size, val);
					r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, true);
					r_debug_reg_sync (core->dbg, R_REG_TYPE_MMX, true);
				} else {
					r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false);
					r_debug_reg_sync (core->dbg, R_REG_TYPE_MMX, false);
					ut64 res = r_reg_get_pack (core->dbg->reg, item, word, size);
					r_cons_printf ("0x%08"PFMT64x"\n", res);
				}
			} else {
				eprintf ("Cannot find multimedia register '%s'\n", name);
			}
			free (name);
		} else {
			r_debug_reg_sync (core->dbg, -R_REG_TYPE_MMX, false);
		}
		//r_debug_drx_list (core->dbg);
		break;
	case 'f': // "drf"
		/* Note, that negative type forces sync to print the regs from the backend */
		r_debug_reg_sync (core->dbg, -R_REG_TYPE_FPU, false);
		//r_debug_drx_list (core->dbg);
		if (str[1]=='?') {
			eprintf ("Usage: drf [fpureg] [= value]\n");
		} else if (str[1]==' ') {
			char *p, *name = strdup (str+2);
			char *eq = strchr (name, '=');
			if (eq) {
				*eq++ = 0;
			}
			p = strchr (name, ' ');
			if (p) {
				*p++ = 0;
			}
			RRegItem *item = r_reg_get (core->dbg->reg, name, -1);
			if (item) {
				if (eq) {
					long double val = 0.0f;
#if __WINDOWS__
					double dval = 0.0f;
					sscanf (eq, "%lf", (double*)&dval);
					val = dval;
#else
					sscanf (eq, "%Lf", &val);
#endif
					r_reg_set_double (core->dbg->reg, item, val);
					r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, true);
					r_debug_reg_sync (core->dbg, R_REG_TYPE_FPU, true);
				} else {
					r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false);
					r_debug_reg_sync (core->dbg, R_REG_TYPE_FPU, false);
					long double res = r_reg_get_double (core->dbg->reg, item);
					r_cons_printf ("%Lf\n", res);
				}
			} else {
				eprintf ("Cannot find multimedia register '%s'\n", name);
			}
			free (name);
		} else {
			r_debug_reg_sync (core->dbg, -R_REG_TYPE_FPU, false);
		}
		break;
	case 'p': // "drp"
		if (!str[1]) {
			if (core->dbg->reg->reg_profile_str) {
				//core->anal->reg = core->dbg->reg;
				r_cons_printf ("%s\n", core->dbg->reg->reg_profile_str);
				//r_cons_printf ("%s\n", core->anal->reg->reg_profile);
			} else eprintf ("No register profile defined. Try 'dr.'\n");
		} else if (str[1] == 'j') {
			// "drpj" .. dup from "arpj"
			RListIter *iter;
			RRegItem *r;
			int i;
			int first = 1;
			static const char *types[R_REG_TYPE_LAST+1] = {
				"gpr", "drx", "fpu", "mmx", "xmm", "flg", "seg", NULL
			};
			static const char *roles[R_REG_NAME_LAST+1] = {
				"pc", "sp", "sr", "bp", "ao", "a1",
				"a2", "a3", "a4", "a5", "a6", "zf",
				"sf", "cf", "of", "sb", NULL
			};
			r_cons_printf ("{\"alias_info\":[");
			for (i = 0; i < R_REG_NAME_LAST; i++) {
				if (core->dbg->reg->name[i]) {
					if (!first) r_cons_printf (",");
					r_cons_printf ("{\"role\":%d,", i);
					r_cons_printf ("\"role_str\":\"%s\",", roles[i]);
					r_cons_printf ("\"reg\":\"%s\"}",
						core->dbg->reg->name[i]);
					first = 0;
				}
			}
			r_cons_printf ("],\"reg_info\":[");
			first = 1;
			for (i = 0; i < R_REG_TYPE_LAST; i++) {
				r_list_foreach (core->dbg->reg->regset[i].regs, iter, r) {
					if (!first) r_cons_printf (",");
					r_cons_printf ("{\"type\":%d,", r->type);
					r_cons_printf ("\"type_str\":\"%s\",", types[r->type]);
					r_cons_printf ("\"name\":\"%s\",", r->name);
					r_cons_printf ("\"size\":%d,", r->size);
					r_cons_printf ("\"offset\":%d}", r->offset);
					first = 0;
				}
			}
			r_cons_printf ("]}");
		} else {
			r_reg_set_profile (core->dbg->reg, str+2);
		}
		break;
	case 't': // "drt"
		switch (str[1]) {
		case '?':
			{
			const char *help_msg[] = {
				"Usage:", "drt", " [type] [size]    # debug register types",
				"drt", "", "List all available register types",
				"drt", " [size]", "Show all regs in the profile of size",
				"drt", " [type]", "Show all regs in the profile of this type",
				"drt", " [type] [size]", "Same as above for type and size",
				NULL};
			r_core_cmd_help (core, help_msg);
			}
			break;
		case ' ':
			{
			int role = r_reg_get_name_idx (str+2);
			const char *regname = r_reg_get_name (core->dbg->reg, role);
			if (!regname)
				regname = str+2;
			size = atoi (regname);
			if (size<1) {
				char *arg = strchr (str+2, ' ');
				size = -1;
				if (arg) {
					*arg++ = 0;
					size = atoi (arg);
				}
				type = r_reg_type_by_name (str+2);
				if (size < 0)
					size = core->dbg->bits * 8;
				r_debug_reg_sync (core->dbg, type, false);
				r_debug_reg_list (core->dbg, type, size,
					strchr (str,'*')? 1: 0, use_color);
			} else {
				if (type != R_REG_TYPE_LAST) {
					r_debug_reg_sync (core->dbg, type, false);
					r_debug_reg_list (core->dbg, type, size,
						strchr (str,'*')?1:0, use_color);
				} else eprintf ("cmd_debug_reg: Unknown type\n");
			}
			} break;
		default:
			for (i=0; (name = r_reg_get_type (i)); i++)
				r_cons_printf ("%s\n", name);
			break;
		}
		break;
	case 'n':
		name = r_reg_get_name (core->dbg->reg, r_reg_get_name_idx (str+2));
		if (name && *name)
			r_cons_printf ("%s\n", name);
		else eprintf ("Oops. try drn [pc|sp|bp|a0|a1|a2|a3|zf|sf|nf|of]\n");
		break;
	case 'd':
		r_debug_reg_list (core->dbg, R_REG_TYPE_GPR, bits, 3, use_color); // XXX detect which one is current usage
		break;
	case 'o':
		r_reg_arena_swap (core->dbg->reg, false);
		r_debug_reg_list (core->dbg, R_REG_TYPE_GPR, bits, 0, use_color); // XXX detect which one is current usage
		r_reg_arena_swap (core->dbg->reg, false);
		break;
	case '=': // 'dr='
		if (r_config_get_i (core->config, "cfg.debug")) {
			if (r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false)) {
				r_debug_reg_list (core->dbg, R_REG_TYPE_GPR, bits, 2, use_color); // XXX detect which one is current usage
			} //else eprintf ("Cannot retrieve registers from pid %d\n", core->dbg->pid);
		} else {
			RReg *orig = core->dbg->reg;
			core->dbg->reg = core->anal->reg;
			r_debug_reg_list (core->dbg, R_REG_TYPE_GPR, bits, 2, use_color); // XXX detect which one is current usage
			core->dbg->reg = orig;
		}
		break;
	case '*':
		if (r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false)) {
			r_cons_printf ("fs+regs\n");
			r_debug_reg_list (core->dbg, R_REG_TYPE_GPR, bits, '*', use_color);
			r_flag_space_pop (core->flags);
			r_cons_printf ("fs-\n");
		}
		break;
	case 'r': // "drr"
		r_core_debug_rr (core, core->dbg->reg);
		break;
	case 'j':
	case '\0':
		if (r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false)) {
			r_debug_reg_list (core->dbg, R_REG_TYPE_GPR, bits, str[0], use_color);
		} else eprintf ("Cannot retrieve registers from pid %d\n", core->dbg->pid);
		break;
	case ' ':
		arg = strchr (str+1, '=');
		if (arg) {
			char *string;
			const char *regname;
			*arg = 0;
			string = r_str_chop (strdup (str+1));
			regname = r_reg_get_name (core->dbg->reg,
				r_reg_get_name_idx (string));
			if (!regname)
				regname = string;
			r = r_reg_get (core->dbg->reg, regname, -1); //R_REG_TYPE_GPR);
			if (r) {
				if (r->flags) {
					r_cons_printf ("0x%08"PFMT64x" ->",
							r_reg_get_value (core->dbg->reg, r));
					r_reg_set_bvalue (core->dbg->reg, r, arg+1);
					r_debug_reg_sync (core->dbg, -1, true);
					r_cons_printf ("0x%08"PFMT64x"\n",
							r_reg_get_value (core->dbg->reg, r));
				} else {
					r_cons_printf ("0x%08"PFMT64x" ->",
							r_reg_get_value (core->dbg->reg, r));
					r_reg_set_value (core->dbg->reg, r,
							r_num_math (core->num, arg+1));
					r_debug_reg_sync (core->dbg, -1, true);
					r_cons_printf ("0x%08"PFMT64x"\n",
							r_reg_get_value (core->dbg->reg, r));
				}
			} else eprintf ("Unknown register '%s'\n", string);
			free (string);
			// update flags here
			r_core_cmd0 (core, ".dr*");
			return;
		} else {
			ut64 off;
			r_debug_reg_sync (core->dbg, -1, 0); //R_REG_TYPE_GPR, false);
			off = r_debug_reg_get (core->dbg, str+1);
	//		r = r_reg_get (core->dbg->reg, str+1, 0);
	//		if (r == NULL) eprintf ("Unknown register (%s)\n", str+1);
			r_cons_printf ("0x%08"PFMT64x"\n", off);
			core->num->value = off;
			//r_reg_get_value (core->dbg->reg, r));
		}
	}
}

static int checkbpcallback(RCore *core) {
	ut64 pc = r_debug_reg_get (core->dbg, "pc");
	RBreakpointItem *bpi = r_bp_get_at (core->dbg->bp, pc);
	if (bpi) {
		const char *cmdbp = r_config_get (core->config, "cmd.bp");
		if (bpi->data)
			r_core_cmd (core, bpi->data, 0);
		if (cmdbp && *cmdbp)
			r_core_cmd (core, cmdbp, 0);
		return true;
	}
	return false;
}

static int bypassbp(RCore *core) {
	RBreakpointItem *bpi;
	ut64 addr;
	r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false);
	addr = r_debug_reg_get (core->dbg, "pc");
	bpi = r_bp_get_at (core->dbg->bp, addr);
	if (!bpi) return false;
	/* XXX 2 if libr/debug/debug.c:226 is enabled */
	r_debug_step (core->dbg, 1);
	return true;
}

static int validAddress(RCore *core, ut64 addr) {
	RDebugMap *map;
	RListIter *iter;
	if (!r_config_get_i (core->config, "dbg.bpinmaps")) {
		return core->num->value = 1;
	}
	r_debug_map_sync (core->dbg);
	r_list_foreach (core->dbg->maps, iter, map) {
		if (addr >= map->addr && addr < map->addr_end) {
			return core->num->value = 1;
		}
	}
	// TODO: try to read memory, expect no 0xffff
	// TODO: check map permissions
	return core->num->value = 0;
}

static void static_debug_stop(void *u) {
	RDebug *dbg = (RDebug *)u;
	r_debug_stop (dbg);
}

static void r_core_cmd_bp(RCore *core, const char *input) {
	RBreakpointItem *bpi;
	const char* help_msg[] = {
		"Usage: db", "", " # Breakpoints commands",
		"db", "", "List breakpoints",
		"db", " sym.main", "Add breakpoint into sym.main",
		"db", " <addr>", "Add breakpoint",
		"db", " -<addr>", "Remove breakpoint",
		"db.", "", "Show breakpoint info in current offset",
		"dbj", "", "List breakpoints in JSON format",
		// "dbi", " 0x848 ecx=3", "stop execution when condition matches",
		"dbc", " <addr> <cmd>", "Run command when breakpoint is hit",
		"dbd", " <addr>", "Disable breakpoint",
		"dbe", " <addr>", "Enable breakpoint",
		"dbs", " <addr>", "Toggle breakpoint",

		"dbt", "", "Display backtrace based on dbg.btdepth and dbg.btalgo",
		"dbt=", "", "Display backtrace in one line",
		"dbtj", "", "Display backtrace in JSON",
		"dbte", " <addr>", "Enable Breakpoint Trace",
		"dbtd", " <addr>", "Disable Breakpoint Trace",
		"dbts", " <addr>", "Swap Breakpoint Trace",
		"dbn", " [<name>]", "Show or set name for current breakpoint",
		//
		"dbi", "", "List breakpoint indexes",
		"dbic", " <index> <cmd>", "Run command at breakpoint index",
		"dbie", " <index>", "Enable breakpoint by index",
		"dbid", " <index>", "Disable breakpoint by index",
		"dbis", " <index>", "Swap Nth breakpoint",
		"dbite", " <index>", "Enable breakpoint Trace by index",
		"dbitd", " <index>", "Disable breakpoint Trace by index",
		"dbits", " <index>", "Swap Nth breakpoint trace",
		//
		"dbh", " x86", "Set/list breakpoint plugin handlers",
		"drx", " number addr len rwx", "Modify hardware breakpoint",
		"drx-", "number", "Clear hardware breakpoint",
		NULL};
	int i, hwbp = r_config_get_i (core->config, "dbg.hwbp");
	RDebugFrame *frame;
	RListIter *iter;
	const char *p;
	RList *list;
	ut64 addr;
	p = strchr (input, ' ');
	addr = p? r_num_math (core->num, p+1): 0LL;

	switch (input[1]) {
	case '.':
		{
			RBreakpointItem *bpi = r_bp_get_at (core->dbg->bp, core->offset);
			if (bpi) {
				r_cons_printf ("breakpoint %s %s %s\n",
						r_str_rwx_i (bpi->rwx),
						bpi->enabled ?
						"enabled" : "disabled",
						bpi->name ? bpi->name : "");
			}
		}
		break;
	case 't':
		switch (input[2]) {
		case 'e':
			for (p = input + 3; *p == ' '; p++) { /* nothing to do here */ }
			if (*p == '*') {
				r_bp_set_trace_all (core->dbg->bp,true);
			} else if (!r_bp_set_trace (core->dbg->bp,
						addr, true)) {
				eprintf ("Cannot set tracepoint\n");
			}
			break;
		case 'd':
			for (p = input + 3; *p==' ';p++);
			if (*p == '*') {
				r_bp_set_trace_all (core->dbg->bp,false);
			} else if (!r_bp_set_trace (core->dbg->bp, addr, false))
				eprintf ("Cannot unset tracepoint\n");
			break;
		case 's':
			bpi = r_bp_get_at (core->dbg->bp, addr);
			if (bpi) bpi->trace = !!!bpi->trace;
			else eprintf ("Cannot unset tracepoint\n");
			break;
		case 'j': // dbtj
			addr = UT64_MAX;
			if (input[2] == ' ' && input[3])
				addr = r_num_math (core->num, input+2);
			i = 0;
			list = r_debug_frames (core->dbg, addr);
			r_cons_printf ("[");
			r_list_foreach (list, iter, frame) {
				r_cons_printf ("%s%08"PFMT64d,
						(i ? "," : ""), frame->addr);
				i++;
			}
			r_cons_printf ("]\n");
			r_list_free (list);
			break;
		case '=': // dbt=
			addr = UT64_MAX;
			if (input[2] == ' ' && input[3])
				addr = r_num_math (core->num, input+2);
			i = 0;
			list = r_debug_frames (core->dbg, addr);
			r_list_reverse (list);
			r_list_foreach (list, iter, frame) {
				r_cons_printf ("%s0x%08"PFMT64x,
						(i ? " > " : ""), frame->addr);
				i++;
			}
			r_cons_newline ();
			r_list_free (list);
			break;
		case 0:
			addr = UT64_MAX;
			if (input[2] == ' ' && input[3])
				addr = r_num_math (core->num, input + 2);
			i = 0;
			list = r_debug_frames (core->dbg, addr);
			r_cons_printf ("#  Address  Size  [Func]  Desc1 Desc2\n");
			r_list_foreach (list, iter, frame) {
				char flagdesc[1024], flagdesc2[1024];
				RFlagItem *f = r_flag_get_at (core->flags,
							frame->addr);
				if (f) {
					if (f->offset != addr) {
						int delta = (int)(frame->addr - f->offset);
						if (delta > 0) {
							snprintf (flagdesc,
								sizeof(flagdesc),
								"%s+%d",
								f->name, delta);
						} else if (delta < 0) {
							snprintf (flagdesc,
								sizeof(flagdesc),
								"%s%d",
								f->name, delta);
						} else {
							snprintf (flagdesc,
								sizeof(flagdesc),
								"%s", f->name);
						}
					} else {
						snprintf (flagdesc,
							sizeof(flagdesc),
							"%s", f->name);
					}
				} else {
					flagdesc[0] = 0;
				}
				f = r_flag_get_at (core->flags, frame->addr-1);
				if (f) {
					if (f->offset != addr) {
						int delta = (int)(frame->addr - 1 - f->offset);
						if (delta > 0) {
							snprintf (flagdesc2,
								sizeof(flagdesc2),
								"%s+%d",
								f->name, delta + 1);
						} else if (delta<0) {
							snprintf (flagdesc2,
								sizeof(flagdesc2),
								"%s%d",
								f->name, delta + 1);
						} else {
							snprintf (flagdesc2,
								sizeof(flagdesc2),
								"%s+1", f->name);
						}
					} else {
						snprintf (flagdesc2,
							sizeof(flagdesc2),
							"%s", f->name);
					}
				} else {
					flagdesc2[0] = 0;
				}
				if (!strcmp (flagdesc, flagdesc2)) {
					flagdesc2[0] = 0;
				}
				RAnalFunction *fcn = r_anal_get_fcn_in (core->anal, frame->addr, 0);
				if (fcn) {
					r_cons_printf ("%d  0x%08"PFMT64x"  %d \
							[%s]  %s %s\n", i++,
							frame->addr, frame->size,
							fcn->name, flagdesc,
							flagdesc2);
				} else {
					r_cons_printf ("%d  0x%08"PFMT64x"  %d  \
							%s %s\n", i++, frame->addr,
							frame->size, flagdesc,
							flagdesc2);
				}
			}
			r_list_free (list);
			break;
		default:
			eprintf ("See db?\n");
			break;
		}
		break;
	case 'j': r_bp_list (core->dbg->bp, 'j'); break;
	case '*': r_bp_list (core->dbg->bp, 1); break;
	case '\0': r_bp_list (core->dbg->bp, 0); break;
	case '-':
		if (input[2] == '*') r_bp_del_all (core->dbg->bp);
		else r_bp_del (core->dbg->bp, r_num_math (core->num, input + 2));
		break;
	case 'c':
		addr = r_num_math (core->num, input + 2);
		bpi = r_bp_get_at (core->dbg->bp, addr);
		if (bpi) {
			char *arg = strchr (input + 2, ' ');
			if (arg) arg = strchr (arg + 1, ' ');
			if (arg) {
				free (bpi->data);
				bpi->data = strdup (arg + 1);
			} else {
				free (bpi->data);
				bpi->data = NULL;
			}
		} else eprintf ("No breakpoint defined at 0x%08"PFMT64x"\n", addr);
		break;
	case 's':
		addr = r_num_math (core->num, input + 2);
		bpi = r_bp_get_at (core->dbg->bp, addr);
		if (bpi) {
			//bp->enabled = !bp->enabled;
			r_bp_del (core->dbg->bp, addr);
		} else {
			if (hwbp) bpi = r_bp_add_hw (core->dbg->bp, addr,
						1, R_BP_PROT_EXEC);
			else bpi = r_bp_add_sw (core->dbg->bp, addr,
						1, R_BP_PROT_EXEC);
			if (!bpi) eprintf ("Cannot set breakpoint "
					"(%s)\n", input + 2);
		}
		r_bp_enable (core->dbg->bp, r_num_math (core->num,
							input + 2), 0);
		break;
	case 'n':
		bpi = r_bp_get_at (core->dbg->bp, core->offset);
		if (input[2] == ' ') {
			if (bpi) {
				free (bpi->name);
				bpi->name = strdup (input + 3);
			} else {
				eprintf ("Cant find breakpoint at "
					"0x%08"PFMT64x"\n", core->offset);
			}
		} else {
			if (bpi && bpi->name) {
				r_cons_printf ("%s\n", bpi->name);
			}
		}
		break;
	case 'e':
		for (p = input + 2; *p == ' '; p++);
		if (*p == '*') r_bp_enable_all (core->dbg->bp,true);
		else r_bp_enable (core->dbg->bp, r_num_math (core->num,
							input + 2), true);
		break;
	case 'd':
		for (p = input + 2; *p == ' '; p++);
		if (*p == '*') r_bp_enable_all (core->dbg->bp, false);
		r_bp_enable (core->dbg->bp, r_num_math (core->num,
							input + 2), false);
		break;
	case 'h':
		switch (input[2]) {
		case ' ':
			if (!r_bp_use (core->dbg->bp, input + 3, core->anal->bits))
				eprintf ("Invalid name: '%s'.\n", input+3);
			break;
		case 0:
			r_bp_plugin_list (core->dbg->bp);
			break;
		default:
			eprintf ("Usage: dh [plugin-name]  # select a debug handler plugin\n");
			break;
		}
		break;
	case ' ':
		for (p = input + 1; *p == ' '; p++);
		if (*p == '-') {
			r_bp_del (core->dbg->bp, r_num_math (core->num, p + 1));
		} else {
			addr = r_num_math (core->num, input+2);
			if (validAddress (core, addr)) {
				bpi = hwbp \
				? r_bp_add_hw (core->dbg->bp, addr,
						1, R_BP_PROT_EXEC)
				: r_bp_add_sw (core->dbg->bp, addr,
						1, R_BP_PROT_EXEC);
				if (bpi) {
					free (bpi->name);
					if (!strcmp (input + 2, "$$")) {
						char *newname = NULL;
						//RFlagItem *f = r_flag_get_at (core->flags, addr);
						RFlagItem *f = r_flag_get_i2 (core->flags, addr);
						if (f) {
							if (f->offset != addr) {
								newname = r_str_newf ("%s+%d\n",
									f->name, (int)(addr - f->offset));
							} else {
								newname = strdup (f->name);
							}
						}
						bpi->name = newname;
					} else {
						bpi->name = strdup (input + 2);
					}
				} else {
					eprintf ("Cannot set breakpoint at "
						"'%s'\n", input+2);
				}
			} else {
				eprintf ("Can't place a breakpoint here. "
					"No mapped memory\n");
			}
		}
		break;
	case 'i':
		switch (input[2]) {
		case 0: // "dbi"
			for (i = 0;i < core->dbg->bp->bps_idx_count; i++) {
				if ((bpi = core->dbg->bp->bps_idx[i])) {
					r_cons_printf ("%d 0x%08"PFMT64x" E:%d T:%d\n",
						i, bpi->addr, bpi->enabled, bpi->trace);
				}
			}
			break;
		case 'c': // "dbic"
			p = strchr (input+3, ' ');
			if (p) {
				if ((bpi = r_bp_get_index (core->dbg->bp, addr))) {
					bpi->data = strdup (p+1);
				} else eprintf ("Cannot set command\n");
			} else {
				eprintf ("|Usage: dbic # cmd\n");
			}
			break;
		case 'e': // "dbie"
			if ((bpi = r_bp_get_index (core->dbg->bp, addr))) {
				bpi->enabled = true;
			} else eprintf ("Cannot unset tracepoint\n");
			break;
		case 'd': // "dbid"
			if ((bpi = r_bp_get_index (core->dbg->bp, addr))) {
				bpi->enabled = false;
			} else eprintf ("Cannot unset tracepoint\n");
			break;
		case 's': // "dbis"
			if ((bpi = r_bp_get_index (core->dbg->bp, addr))) {
				bpi->enabled = !!!bpi->enabled;
			} else eprintf ("Cannot unset tracepoint\n");
			break;
		case 't': // "dbite" "dbitd" ...
			switch (input[3]) {
			case 'e':
				if ((bpi = r_bp_get_index (core->dbg->bp, addr))) {
					bpi->trace = true;
				} else eprintf ("Cannot unset tracepoint\n");
				break;
			case 'd':
				if ((bpi = r_bp_get_index (core->dbg->bp, addr))) {
					bpi->trace = false;
				} else eprintf ("Cannot unset tracepoint\n");
				break;
			case 's':
				if ((bpi = r_bp_get_index (core->dbg->bp, addr))) {
					bpi->trace = !!!bpi->trace;
				} else eprintf ("Cannot unset tracepoint\n");
				break;
			}
			break;
		}
		break;
	case '?':
	default:
		r_core_cmd_help (core, help_msg);
		break;
	}
}

static RTreeNode *add_trace_tree_child (Sdb *db, RTree *t, RTreeNode *cur, ut64 addr) {
	struct trace_node *t_node;
	RTreeNode *node;
	char dbkey[TN_KEY_LEN];

	snprintf (dbkey, TN_KEY_LEN, TN_KEY_FMT, addr);
	t_node = (struct trace_node *)(size_t)sdb_num_get (db, dbkey, NULL);
	if (!t_node) {
		t_node = (struct trace_node *)malloc (sizeof(*t_node));
		t_node->addr = addr;
		t_node->refs = 1;
		sdb_num_set (db, dbkey, (ut64)(size_t)t_node, 0);
	} else {
		t_node->refs++;
	}

	node = r_tree_add_node (t, cur, t_node);
	return node;
}

static void trace_traverse_pre (RTreeNode *n, RTreeVisitor *vis) {
	struct trace_node *tn = n->data;
	unsigned int i;

	if (!tn)
		return;

	for (i = 0; i < n->depth - 1; ++i)
		r_cons_printf ("   ");

	r_cons_printf (" 0x%08"PFMT64x" refs %d\n",
			tn->addr, tn->refs);
}

static void trace_traverse (RTree *t) {
	RTreeVisitor vis = { 0 };

	/* clear the line on stderr, because somebody has written there */
	fprintf (stderr, "\x1b[2K\r");
	fflush (stderr);
	vis.pre_visit = (RTreeNodeVisitCb)trace_traverse_pre;
	r_tree_dfs (t, &vis);
}

static void do_debug_trace_calls (RCore *core, ut64 from, ut64 to, ut64 final_addr) {
	int shallow_trace = r_config_get_i (core->config, "dbg.shallow_trace");
	Sdb *tracenodes = core->dbg->tracenodes;
	RTree *tr = core->dbg->tree;
	RDebug *dbg = core->dbg;
	ut64 debug_to = UT64_MAX;
	RTreeNode *cur;
	int n = 0;

	/* set root if not already present */
	r_tree_add_node (tr, NULL, NULL);
	cur = tr->root;

	while (true) {
		ut8 buf[32];
		ut64 addr;
		RAnalOp aop;
		int addr_in_range;

		if (r_cons_singleton ()->breaked)
			break;
		if (r_debug_is_dead (dbg))
			break;
		if (debug_to != UT64_MAX && !r_debug_continue_until (dbg, debug_to))
			break;
		else if (!r_debug_step (dbg, 1))
			break;
		debug_to = UT64_MAX;
		if (!r_debug_reg_sync (dbg, R_REG_TYPE_GPR, false))
			break;
		addr = r_debug_reg_get (dbg, "pc");
		addr_in_range = addr >= from && addr < to;

		r_io_read_at (core->io, addr, buf, sizeof (buf));
		r_anal_op (core->anal, &aop, addr, buf, sizeof (buf));
		eprintf (" %d %"PFMT64x"\r", n++, addr);
		switch (aop.type) {
		case R_ANAL_OP_TYPE_UCALL:
		{
			ut64 called_addr;
			int called_in_range;
			// store regs
			// step into
			// get pc
			r_debug_step (dbg, 1);
			r_debug_reg_sync (dbg, R_REG_TYPE_GPR, false);
			called_addr = r_debug_reg_get (dbg, "pc");
			called_in_range = called_addr >= from && called_addr < to;
			if (!called_in_range && addr_in_range && shallow_trace)
				debug_to = addr;
			if (addr_in_range) {
				cur = add_trace_tree_child(tracenodes, tr, cur, addr);
				if (debug_to != UT64_MAX)
					cur = cur->parent;
			}
			// TODO: push pc+aop.length into the call path stack
			break;
		}
		case R_ANAL_OP_TYPE_CALL:
		{
			int called_in_range = aop.jump >= from && aop.jump < to;
			if (!called_in_range && addr_in_range && shallow_trace)
				debug_to = aop.addr + aop.size;
			if (addr_in_range) {
				cur = add_trace_tree_child(tracenodes, tr, cur, addr);
				if (debug_to != UT64_MAX)
					cur = cur->parent;
			}
			break;
		}
		case R_ANAL_OP_TYPE_RET:
#if 0
			// TODO: we must store ret value for each call in the graph path to do this check
			r_debug_step (dbg, 1);
			r_debug_reg_sync (dbg, R_REG_TYPE_GPR, false);
			addr = r_debug_reg_get (dbg, "pc");
			// TODO: step into and check return address if correct
			// if not correct we are hijacking the control flow (exploit!)
#endif
			if (cur != tr->root)
				cur = cur->parent;
#if 0
			if (addr != gn->addr) {
				eprintf ("Oops. invalid return address 0x%08"PFMT64x
						"\n0x%08"PFMT64x"\n", addr, gn->addr);
			}
#endif
			break;
		}
		if (checkbpcallback (core)) {
			eprintf ("Interrupted by a breakpoint\n");
			break;
		}
	}
}

static void debug_trace_calls (RCore *core, const char *input) {
	RBreakpointItem *bp_final = NULL;
	int t = core->dbg->trace->enabled;
	ut64 from = 0, to = UT64_MAX, final_addr = UT64_MAX;

	if (r_debug_is_dead (core->dbg)) {
		eprintf ("No process to debug.");
		return;
	}

	if (*input == ' ') {
		ut64 first_n;

		while (*input == ' ') input++;
		first_n = r_num_math (core->num, input);
		input = strchr (input, ' ');
		if (input) {
			while (*input == ' ') input++;
			from = first_n;
			to = r_num_math (core->num, input);
			input = strchr (input, ' ');
			if (input) {
				while (*input == ' ') input++;
				final_addr = r_num_math (core->num, input);
			}
		} else {
			final_addr = first_n;
		}
	}

	core->dbg->trace->enabled = 0;
	r_cons_break (static_debug_stop, core->dbg);
	r_reg_arena_swap (core->dbg->reg, true);

	if (final_addr != UT64_MAX) {
		int hwbp = r_config_get_i (core->config, "dbg.hwbp");
		if (hwbp) bp_final = r_bp_add_hw (core->dbg->bp, final_addr, 1, R_BP_PROT_EXEC);
		else bp_final = r_bp_add_sw (core->dbg->bp, final_addr, 1, R_BP_PROT_EXEC);
		if (!bp_final)
			eprintf ("Cannot set breakpoint at final address (%"PFMT64x")\n", final_addr);
	}

	do_debug_trace_calls (core, from, to, final_addr);
	if (bp_final)
		r_bp_del (core->dbg->bp, final_addr);

	trace_traverse (core->dbg->tree);
	core->dbg->trace->enabled = t;
	r_cons_break_end();
}

static void r_core_debug_esil (RCore *core, const char *input) {
	switch (input[0]) {
	case ' ':
		{
		char *line = strdup (input+1);
		char *p, *q;
		int done = 0;
		int rwx = 0, dev = 0;
		p = strchr (line, ' ');
		if (p) {
			*p++ = 0;
			if (strchr (line, 'r')) rwx |= R_IO_READ;
			if (strchr (line, 'w')) rwx |= R_IO_WRITE;
			if (strchr (line, 'x')) rwx |= R_IO_EXEC;
			q = strchr (p, ' ');
			if (q) {
				*q++ = 0;
				dev = p[0];
				if (q) {
					r_debug_esil_watch (core->dbg, rwx, dev, q);
					done = 1;
				}
			}
		}
		if (!done) {
			eprintf ("Usage: de [rwx] [reg|mem] [expr]\n");
		}
		free (line);
		}
		break;
	case '-':
		r_debug_esil_watch_reset (core->dbg);
		break;
	case 's':
		if (input[1] == 'u' && input[2] == ' ') { // "desu"
			ut64 addr, naddr, fin = r_num_math (core->num, input+2);
			r_core_cmd0 (core, "aei");
			addr = r_debug_reg_get (core->dbg, "pc");
			while (addr != fin) {
				r_debug_esil_prestep (core->dbg, r_config_get_i (
					core->config, "esil.prestep"));
				r_debug_esil_step (core->dbg, 1);
				naddr = r_debug_reg_get (core->dbg, "pc");
				if (naddr == addr) {
					eprintf ("Detected loophole\n");
					break;
				}
				addr = naddr;
			}
		} else if (input[1] == '?' || !input[1]) {
			// TODO: use r_core_help here
			eprintf ("Usage: des[u] [arg]\n");
			eprintf (" des [num-of-instructions]\n");
			eprintf (" desu [address]\n");
		} else {
			r_core_cmd0 (core, "aei");
			r_debug_esil_prestep (core->dbg, r_config_get_i (core->config, "esil.prestep"));
			// continue
			r_debug_esil_step (core->dbg, r_num_math (core->num, input+1));
		}
		break;
	case 'c':
		if (r_debug_esil_watch_empty (core->dbg)) {
			eprintf ("Error: no esil watchpoints defined\n");
		} else {
			r_core_cmd0 (core, "aei");
			r_debug_esil_prestep (core->dbg, r_config_get_i (core->config, "esil.prestep"));
			r_debug_esil_continue (core->dbg);
		}
		break;
	case 0:
		// list
		r_debug_esil_watch_list (core->dbg);
		break;
	case '?':
	default:
		eprintf ("Usage: de[-sc] [rwx] [rm] [expr]\n");
		eprintf ("Examples:\n");
		eprintf ("> de               # list esil watchpoints\n");
		eprintf ("> de-*             # delete all esil watchpoints\n");
		eprintf ("> de r r rip       # stop when reads rip\n");
		eprintf ("> de rw m ADDR     # stop when read or write in ADDR\n");
		eprintf ("> de w r rdx       # stop when rdx register is modified\n");
		eprintf ("> de x m FROM..TO  # stop when rip in range\n");
		eprintf ("> dec              # continue execution until matching expression\n");
		eprintf ("> des [num]        # step-in N instructions with esildebug\n");
		eprintf ("> desu [addr]      # esildebug until specific address\n");
		eprintf ("TODO: Add support for conditionals in expressions like rcx == 4 or rcx<10\n");
		eprintf ("TODO: Turn on/off debugger trace of esil debugging\n");
		break;
	}
}

static void r_core_debug_kill (RCore *core, const char *input) {
	if (!input || *input=='?') {
		if (input && input[1]) {
			const char *signame, *arg = input+1;
			int signum = atoi (arg);
			if (signum>0) {
				signame = r_debug_signal_resolve_i (core->dbg, signum);
				if (signame)
					r_cons_printf ("%s\n", signame);
			} else {
				signum = r_debug_signal_resolve (core->dbg, arg);
				if (signum>0)
					r_cons_printf ("%d\n", signum);
			}
		} else {
			const char * help_message[] = {
				"Usage: dk", "", "Signal commands",
				"dk", "", "List all signal handlers of child process",
				"dk", " <signal>", "Send KILL signal to child",
				"dk", " <signal>=1", "Set signal handler for <signal> in child",
				"dk?", "<signal>", "Name/signum resolver",
				"dko", " <signal>", "Reset skip or cont options for given signal",
				"dko", " <signal> [|skip|cont]", "On signal SKIP handler or CONT into",
				"dkj", "", "List all signal handlers in JSON",
				NULL
			};
			r_core_cmd_help (core, help_message);
		}
	} else if (*input=='o') {
		char *p, *name = strdup (input+2);
		int signum = atoi (name);
		p = strchr (name, ' ');
		if (p) {
			*p++ = 0;
			// Actions:
			//  - pass
			//  - trace
			//  - stop
			if (signum<1) signum = r_debug_signal_resolve (core->dbg, name);
			if (signum>0) {
				if (strchr (p, 's')) {
					r_debug_signal_setup (core->dbg, signum, R_DBG_SIGNAL_SKIP);
				} else if (strchr (p, 'c')) {
					r_debug_signal_setup (core->dbg, signum, R_DBG_SIGNAL_CONT);
				} else {
					eprintf ("Invalid option\n");
				}
			} else {
				eprintf ("Invalid signal\n");
			}
		} else {
			switch (input[1]) {
			case 0:
				r_debug_signal_list (core->dbg, 1);
				break;
			case '?':
				eprintf ("|Usage: dko SIGNAL [skip|cont]\n"
					"| 'SIGNAL' can be a number or a string that resolves with dk?..\n"
					"| s - skip (do not enter into the signal handler\n"
					"| c - continue into the signal handler\n"
					"|   - no option means stop when signal is catched\n");
				break;
			default:
				if (signum<1) signum = r_debug_signal_resolve (core->dbg, name);
				r_debug_signal_setup (core->dbg, signum, 0);
				break;
			}
		}
		free (name);
	} else if (*input == 'j') {
		r_debug_signal_list (core->dbg, 2);
	} else if (!*input) {
		r_debug_signal_list (core->dbg, 0);
#if 0
		RListIter *iter;
		RDebugSignal *ds;
		eprintf ("TODO: list signal handlers of child\n");
		RList *list = r_debug_kill_list (core->dbg);
		r_list_foreach (list, iter, ds) {
			// TODO: resolve signal name by number and show handler offset
			eprintf ("--> %d\n", ds->num);
		}
		r_list_free (list);
#endif
	} else {
		int sig = atoi (input);
		char *p = strchr (input, '=');
		if (p) {
			r_debug_kill_setup (core->dbg, sig, r_num_math (core->num, p+1));
		} else {
			r_debug_kill (core->dbg, core->dbg->pid, core->dbg->tid, sig);
		}
	}
}

static int cmd_debug_continue (RCore *core, const char *input) {
	int pid, old_pid, signum;
	ut64 addr;
	char *ptr;
	const char * help_message[] = {
		"Usage: dc", "", "Execution continuation commands",
		"dc", "", "Continue execution of all children",
		"dc", " <pid>", "Continue execution of pid",
		"dc", "[-pid]", "Stop execution of pid",
		"dca", " [sym] [sym].", "Continue at every hit on any given symbol",
		"dcc", "", "Continue until call (use step into)",
		"dccu", "", "Continue until unknown call (call reg)",
		"dcf", "", "Continue until fork (TODO)",
		"dck", " <signal> <pid>", "Continue sending signal to process",
		"dco", " <num>", "Step over <num> instructions",
		"dcp", "", "Continue until program code (mapped io section)",
		"dcr", "", "Continue until ret (uses step over)",
		"dcs", " <num>", "Continue until syscall",
		"dct", " <len>", "Traptrace from curseek to len, no argument to list",
		"dcu", " [addr]", "Continue until address",
		"dcu", " <address> [end]", "Continue until given address range",
		/*"TODO: dcu/dcr needs dbg.untilover=true??",*/
		/*"TODO: same for only user/libs side, to avoid steping into libs",*/
		/*"TODO: support for threads?",*/
		NULL
	};
	// TODO: we must use this for step 'ds' too maybe...
	switch (input[1]) {
	case '?':
		r_core_cmd_help (core, help_message);
		return 0;
	case 'a':
		eprintf ("TODO: dca\n");
		break;
	case 'f':
		eprintf ("[+] Running 'dcs vfork' behind the scenes...\n");
		// we should stop in fork and vfork syscalls
		//TODO: multiple syscalls not handled yet
		// r_core_cmd0 (core, "dcs vfork fork");
		r_core_cmd0 (core, "dcs vfork fork");
		break;
	case 'c':
		r_reg_arena_swap (core->dbg->reg, true);
		if (input[2] == 'u') {
			r_debug_continue_until_optype (core->dbg, R_ANAL_OP_TYPE_UCALL, 0);
		} else {
			r_debug_continue_until_optype (core->dbg, R_ANAL_OP_TYPE_CALL, 0);
		}
		checkbpcallback (core);
		break;
	case 'r':
		r_reg_arena_swap (core->dbg->reg, true);
		r_debug_continue_until_optype (core->dbg, R_ANAL_OP_TYPE_RET, 1);
		checkbpcallback (core);
		break;
	case 'k':
		// select pid and r_debug_continue_kill (core->dbg,
		r_reg_arena_swap (core->dbg->reg, true);
		signum = r_num_math (core->num, input+2);
		ptr = strchr (input+3, ' ');
		if (ptr) {
			bypassbp (core);
			int old_pid = core->dbg->pid;
			int old_tid = core->dbg->tid;
			int pid = atoi (ptr+1);
			int tid = pid; // XXX
			*ptr = 0;
			r_debug_select (core->dbg, pid, tid);
			r_debug_continue_kill (core->dbg, signum);
			r_debug_select (core->dbg, old_pid, old_tid);
		} else {
			r_debug_continue_kill (core->dbg, signum);
		}
		checkbpcallback (core);
		break;
	case 's':
		switch (input[2]) {
		case '*':
			cmd_debug_cont_syscall (core, "-1");
			break;
		case ' ':
			cmd_debug_cont_syscall (core, input+3);
			break;
		case '\0':
			cmd_debug_cont_syscall (core, NULL);
			break;
		default:
		case '?':
			eprintf ("|Usage: dcs [syscall-name-or-number]\n");
			eprintf ("|dcs         : continue until next syscall\n");
			eprintf ("|dcs mmap    : continue until next call to mmap\n");
			eprintf ("|dcs*        : trace all syscalls (strace)\n");
			eprintf ("|dcs?        : show this help\n");
			break;
		}
		break;
	case 'p':
		{ // XXX: this is very slow
			RIOSection *s;
			ut64 pc;
			int n = 0;
			int t = core->dbg->trace->enabled;
			core->dbg->trace->enabled = 0;
			r_cons_break (static_debug_stop, core->dbg);
			do {
				r_debug_step (core->dbg, 1);
				r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false);
				pc = r_debug_reg_get (core->dbg, "pc");
				eprintf (" %d %"PFMT64x"\r", n++, pc);
				s = r_io_section_vget (core->io, pc);
				if (r_cons_singleton ()->breaked)
					break;
			} while (!s);
			eprintf ("\n");
			core->dbg->trace->enabled = t;
			r_cons_break_end();
			return 1;
		}
	case 'u':
		if (input[2] != ' ') {
			eprintf ("|Usage: dcu <address>\n");
			return 1;
		}
		ptr = strchr (input+3, ' ');
// TODO : handle ^C here
		if (ptr) { // TODO: put '\0' in *ptr to avoid
			ut64 from, to, pc;
			from = r_num_math (core->num, input+3);
			to = r_num_math (core->num, ptr+1);
			do {
				r_debug_step (core->dbg, 1);
				r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false);
				pc = r_debug_reg_get (core->dbg, "pc");
				eprintf ("Continue 0x%08"PFMT64x" > 0x%08"PFMT64x" < 0x%08"PFMT64x"\n",
						from, pc, to);
			} while (pc < from || pc > to);
			return 1;
		}
		addr = r_num_math (core->num, input+2);
		if (addr) {
			eprintf ("Continue until 0x%08"PFMT64x"\n", addr);
			bypassbp (core);
			r_reg_arena_swap (core->dbg->reg, true);
			r_bp_add_sw (core->dbg->bp, addr, 1, R_BP_PROT_EXEC);
			r_debug_continue (core->dbg);
			checkbpcallback (core);
			r_bp_del (core->dbg->bp, addr);
		} else {
			eprintf ("Cannot continue until address 0\n");
			return 0;
		}
		break;
	case ' ':
		old_pid = core->dbg->pid;
		pid = atoi (input+2);
		bypassbp (core);
		r_reg_arena_swap (core->dbg->reg, true);
		r_debug_select (core->dbg, pid, core->dbg->tid);
		r_debug_continue (core->dbg);
		r_debug_select (core->dbg, old_pid, core->dbg->tid);
		checkbpcallback (core);
		break;
	case 't':
		cmd_debug_backtrace (core, input+2);
		break;
	default:
		bypassbp (core);
		r_reg_arena_swap (core->dbg->reg, true);
		r_debug_continue (core->dbg);
		checkbpcallback (core);
	}
	return 1;
}

static int cmd_debug_step (RCore *core, const char *input) {
	ut64 addr;
	ut8 buf[64];
	RAnalOp aop;
	int i, times = 1;
	const char * help_message[] = {
		"Usage: ds", "", "Step commands",
		"ds", "", "Step one instruction",
		"ds", " <num>", "Step <num> instructions",
		"dsf", "", "Step until end of frame",
		"dsi", " <cond>", "Continue until condition matches",
		"dsl", "", "Step one source line",
		"dsl", " <num>", "Step <num> source lines",
		"dso", " <num>", "Step over <num> instructions",
		"dsp", "", "Step into program (skip libs)",
		"dss", " <num>", "Skip <num> step instructions",
		"dsu", " <address>", "Step until address",
		"dsui", " <instr>", "Step until an instruction that matches `instr`",
		"dsue", " <esil>", "Step until esil expression matches",
		"dsuf", " <flag>", "Step until pc == flag matching name",
		NULL
	};
	if (strlen (input) > 2)
		times = atoi (input+2);
	if (times<1) times = 1;
	switch (input[1]) {
	case '?':
		r_core_cmd_help (core, help_message);
		return 0;
	case 'i':
		if (input[2] == ' ') {
			int n = 0;
			r_cons_break (static_debug_stop, core->dbg);
			do {
				if (r_cons_singleton ()->breaked)
					break;
				r_debug_step (core->dbg, 1);
				if (r_debug_is_dead (core->dbg))
					break;
				if (checkbpcallback (core)) {
					eprintf ("Interrupted by a breakpoint\n");
					break;
				}
				r_core_cmd0 (core, ".dr*");
				n++;
			} while (!r_num_conditional (core->num, input+3));
			eprintf ("Stopped after %d instructions\n", n);
		} else eprintf ("Missing argument\n");
		break;
	case 'f':
		step_until_eof (core);
		break;
	case 'u':
		switch (input[2]) {
		case 'f':
			step_until_flag (core, input+3);
			break;
		case 'i':
			step_until_inst (core, input+3);
			break;
		case 'e':
			step_until_esil (core, input+3);
			break;
		case ' ':
			r_reg_arena_swap (core->dbg->reg, true);
			step_until (core, r_num_math (core->num, input+2)); // XXX dupped by times
			break;
		default:
			eprintf ("Usage: dsu[fei] [arg]  . step until address ' ',"
					" 'f'lag, 'e'sil or 'i'nstruction matching\n");
			return 0;
		}
		break;
	case 'p':
		r_reg_arena_swap (core->dbg->reg, true);
		for (i=0; i<times; i++) {
			ut8 buf[64];
			ut64 addr;
			RAnalOp aop;
			r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false);
			addr = r_debug_reg_get (core->dbg, "pc");
			r_io_read_at (core->io, addr, buf, sizeof (buf));
			r_anal_op (core->anal, &aop, addr, buf, sizeof (buf));
			if (aop.type == R_ANAL_OP_TYPE_CALL) {
				RIOSection *s = r_io_section_vget (core->io, aop.jump);
				if (!s) {
					r_debug_step_over (core->dbg, times);
					continue;
				}
			}
			r_debug_step (core->dbg, 1);
			if (checkbpcallback (core)) {
				eprintf ("Interrupted by a breakpoint\n");
				break;
			}
		}
		break;
	case 's':
		addr = r_debug_reg_get (core->dbg, "pc");
		r_reg_arena_swap (core->dbg->reg, true);
		for (i = 0; i < times; i++) {
			r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false);
			r_io_read_at (core->io, addr, buf, sizeof (buf));
			r_anal_op (core->anal, &aop, addr, buf, sizeof (buf));
			if (aop.jump != UT64_MAX && aop.fail != UT64_MAX) {
				eprintf ("Don't know how to skip this instruction\n");
				break;
			}
			addr += aop.size;
		}
		r_debug_reg_set (core->dbg, "pc", addr);
		break;
	case 'o':
		r_reg_arena_swap (core->dbg->reg, true);
		r_debug_step_over (core->dbg, times);
		if (checkbpcallback (core)) {
			eprintf ("Interrupted by a breakpoint\n");
			break;
		}
		break;
	case 'l':
		r_reg_arena_swap (core->dbg->reg, true);
		step_line (core, times);
		break;
	default:
		r_reg_arena_swap (core->dbg->reg, true);
		r_debug_reg_sync (core->dbg, R_REG_TYPE_GPR, false);
		r_debug_step (core->dbg, times);
		if (checkbpcallback (core)) {
			eprintf ("Interrupted by a breakpoint\n");
			break;
		}
	}
	return 1;
}

static int cmd_debug(void *data, const char *input) {
	RCore *core = (RCore *)data;
	int follow = 0;

	if (r_sandbox_enable (0)) {
		eprintf ("Debugger commands disabled in sandbox mode\n");
		return 0;
	}
	if (!strncmp (input, "ate", 3)) {
		char str[128];
		str[0] = 0;
		r_print_date_get_now (core->print, str);
		r_cons_printf ("%s\n", str);
		return 0;
	}

	switch (input[0]) {
	case 't':
// TODO: define ranges? to display only some traces, allow to scroll on this disasm? ~.. ?
		switch (input[1]) {
		case '?': {
			const char * help_message[] = {
				"Usage: dt", "", "Trace commands",
				"dt", "", "List all traces ",
				"dtd", "", "List all traced disassembled",
				"dtc [addr]|([from] [to] [addr])", "", "Trace call/ret",
				"dtg", "", "Graph call/ret trace",
				"dtr", "", "Reset traces (instruction//cals)",
				NULL
			};
			r_core_cmd_help (core, help_message);
			}
			break;
		case 'c': // "dtc"
			debug_trace_calls (core, input + 2);
			break;
		case 'd':
			// TODO: reimplement using the api
			r_core_cmd0 (core, "pd 1 @@= `dt~[0]`");
			break;
		case 'g': // "dtg"
			dot_trace_traverse (core, core->dbg->tree);
			break;
		case 'r':
			r_tree_reset (core->dbg->tree);
			r_debug_trace_free (core->dbg);
			r_debug_tracenodes_reset (core->dbg);
			core->dbg->trace = r_debug_trace_new ();
			break;
		case '\0':
			r_debug_trace_list (core->dbg, -1);
			break;
		default:
			eprintf ("Wrong arg. See dt?\n");
			break;
		}
		break;
	case 'd':
		switch (input[1]) {
		case '\0':
			r_debug_desc_list (core->dbg, 0);
			break;
		case '*':
			r_debug_desc_list (core->dbg, 1);
			break;
		case 's':
			// r_debug_desc_seek()
			// TODO: handle read, readwrite, append
			break;
		case 'd':
			// r_debug_desc_dup()
			break;
		case 'r':
			// r_debug_desc_read()
			break;
		case 'w':
			// r_debug_desc_write()
			break;
		case '-': // "dd-"
			// close file
			//r_core_syscallf (core, "close", "%d", atoi (input+2));
			r_core_cmdf (core, "dxs close %d", (int)r_num_math (
				core->num, input+2));
			// TODO: run
			break;
		case ' ':
			// TODO: handle read, readwrite, append
			r_core_syscallf (core, "open", "%s, %d, %d",
				input+2, 2, 0644);
			// open file
			break;
		case '?':
		default: {
			const char * help_message[] = {
				"Usage: dd", "", "Descriptors commands",
				"dd", "", "List file descriptors",
				"dd", " <file>", "Open and map that file into the UI",
				"dd-", "<fd>", "Close stdout fd",
				"dd*", "", "List file descriptors (in radare commands)",
				NULL
			};
			r_core_cmd_help (core, help_message);
			}
			break;
		}
		break;
	case 's':
		if (cmd_debug_step (core, input)) {
			follow = r_config_get_i (core->config, "dbg.follow");
		}
		break;
	case 'b':
		r_core_cmd_bp (core, input);
		break;
	case 'H':
		eprintf ("TODO: transplant process\n");
		break;
	case 'c': // "dc"
		r_cons_break (static_debug_stop, core->dbg);
		(void)cmd_debug_continue (core, input);
		follow = r_config_get_i (core->config, "dbg.follow");
		r_cons_break_end ();
		break;
	case 'm': // "dm"
		cmd_debug_map (core, input + 1);
		break;
	case 'r': // "dr"
		if (core->io->debug || input[1] == '?') {
			cmd_debug_reg (core, input+1);
		} else {
			void cmd_anal_reg(RCore *core, const char *str);
			cmd_anal_reg (core, input+1);
		}
		//r_core_cmd (core, "|reg", 0);
		break;
	case 'p':
		cmd_debug_pid (core, input);
		break;
	case 'h':
		if (input[1]==' ')
			r_debug_use (core->dbg, input+2);
		else r_debug_plugin_list (core->dbg);
		break;
	case 'i':
		{
		const char * help_message[] = {
			"Usage: di", "", "Debugger target information",
			"di", "", "Show debugger target information",
			"dij", "", "Same as above, but in JSON format",
			NULL
		};
		RDebugInfo *rdi = r_debug_info (core->dbg, input+2);
		int stop = r_debug_stop_reason(core->dbg);
		char *escaped_str;
		switch (input[1]) {
		case '\0':
#define P r_cons_printf
#define PS(X, Y) {escaped_str = r_str_escape (Y);r_cons_printf(X, escaped_str);free(escaped_str);}
			if (rdi) {
				const char *s = r_debug_signal_resolve_i (core->dbg, core->dbg->reason.signum);
				P ("type=%s\n", r_debug_reason_to_string (core->dbg->reason.type));
				P ("signal=%s\n", s? s: "none");
				P ("signum=%d\n", core->dbg->reason.signum);
				P ("sigpid=%d\n", core->dbg->reason.tid);
				P ("addr=0x%"PFMT64x"\n", core->dbg->reason.addr);
				P ("inbp=%s\n", core->dbg->reason.bpi? "true": "false");
				P ("pid=%d\n", rdi->pid);
				P ("tid=%d\n", rdi->tid);
				if (rdi->exe && *rdi->exe)
					P ("exe=%s\n", rdi->exe);
				if (rdi->cmdline && *rdi->cmdline)
					P ("cmdline=%s\n", rdi->cmdline);
				if (rdi->cwd && *rdi->cwd)
					P ("cwd=%s\n", rdi->cwd);
			}
			if (stop != -1) P ("stopreason=%d\n", stop);
			break;
		case 'j':
			P ("{");
			if (rdi) {
				const char *s = r_debug_signal_resolve_i (core->dbg, core->dbg->reason.signum);
				P ("\"type\":\"%s\",", r_debug_reason_to_string (core->dbg->reason.type));
				P ("\"signal\":\"%s\",", s? s: "none");
				P ("\"signum\":%d,", core->dbg->reason.signum);
				P ("\"sigpid\":%d,", core->dbg->reason.tid);
				P ("\"addr\":%"PFMT64d",", core->dbg->reason.addr);
				P ("\"inbp\":%s,", core->dbg->reason.bpi? "true": "false");
				P ("\"pid\":%d,", rdi->pid);
				P ("\"tid\":%d,", rdi->tid);
				if (rdi->exe) PS("\"exe\":\"%s\",", rdi->exe)
				if (rdi->cmdline) PS ("\"cmdline\":\"%s\",", rdi->cmdline);
				if (rdi->cwd) PS ("\"cwd\":\"%s\",", rdi->cwd);
			}
			P ("\"stopreason\":%d}\n", stop);
			break;
#undef P
#undef PS
		case '?':
		default:
			r_core_cmd_help (core, help_message);
		}
		if (rdi)
			r_debug_info_free (rdi);
		}
		break;
	case 'x':
		switch (input[1]) {
		case 'a':
			{
			RAsmCode *acode;
			r_asm_set_pc (core->assembler, core->offset);
			acode = r_asm_massemble (core->assembler, input+2);
			if (acode && *acode->buf_hex) {
				r_reg_arena_push (core->dbg->reg);
				r_debug_execute (core->dbg, acode->buf,
						acode->len, 0);
				r_reg_arena_pop (core->dbg->reg);
			}
			r_asm_code_free (acode);
			}
			break;
		case 'e':
			{
			REgg *egg = core->egg;
			RBuffer *b;
			const char *asm_arch = r_config_get (core->config, "asm.arch");
			int asm_bits = r_config_get_i (core->config, "asm.bits");
			const char *asm_os = r_config_get (core->config, "asm.os");
			r_egg_setup (egg, asm_arch, asm_bits, 0, asm_os);
			r_egg_reset (egg);
			r_egg_load (egg, input+1, 0);
			r_egg_compile (egg);
			b = r_egg_get_bin (egg);
			r_asm_set_pc (core->assembler, core->offset);
			r_reg_arena_push (core->dbg->reg);
			r_debug_execute (core->dbg, b->buf, b->length, 0);
			r_reg_arena_pop (core->dbg->reg);
			}
			break;
		case 's':
			if (input[2]) {
				r_cons_push ();
				char * str = r_core_cmd_str (core,
					sdb_fmt (0, "gs %s", input + 2));
				r_cons_pop ();
				r_core_cmdf (core, "dx %s", str); //`gs %s`", input+2);
				free (str);
			} else {
				eprintf ("Missing parameter used in gs by dxs\n");
			}
			break;
		case 'r':
			r_reg_arena_push (core->dbg->reg);
			if (input[2] == ' ') {
				ut8 bytes[4096];
				if (strlen (input + 2) < 4096){
					int bytes_len = r_hex_str2bin (input + 2,
									bytes);
					if (bytes_len > 0) {
						r_debug_execute (core->dbg,
								bytes, bytes_len,
								0);
					} else {
						eprintf ("Invalid hexpairs\n");
					}
				} else eprintf ("Injection opcodes so long\n");
			}
			r_reg_arena_pop (core->dbg->reg);
			break;
		case ' ':
			{
			ut8 bytes[4096];
			if (strlen (input + 2) < 4096){
				int bytes_len = r_hex_str2bin (input + 2, bytes);
				if (bytes_len>0) r_debug_execute (core->dbg,
							bytes, bytes_len, 0);
				else eprintf ("Invalid hexpairs\n");
			} else eprintf ("Injection opcodes so long\n");
			}
			break;
		default:{
			const char* help_msg[] = {
			"Usage: dx", "", " # Code injection commands",
			"dx", " <opcode>...", "Inject opcodes",
			"dxa", " nop", "Assemble code and inject",
			"dxe", " egg-expr", "compile egg expression and inject it",
			"dxr", " <opcode>...", "Inject opcodes and restore state",
			"dxs", " write 1, 0x8048, 12", "Syscall injection (see gs)",
			"\nExamples:", "", "",
			"dx", " 9090", "Inject two x86 nop",
			"\"dxa mov eax,6;mov ebx,0;int 0x80\"", "", "Inject and restore state",
			NULL};
			r_core_cmd_help (core, help_msg);
			}
			break;
		}
		break;
	case 'o':
		r_core_file_reopen (core, input[1] ? input + 2: NULL, 0, 1);
		break;
	case 'w':
		r_cons_break (static_debug_stop, core->dbg);
		for (;!r_cons_singleton ()->breaked;) {
			int pid = atoi (input + 1);
			//int opid = core->dbg->pid = pid;
			int res = r_debug_kill (core->dbg, pid, 0, 0);
			if (!res) break;
			r_sys_usleep (200);
		}
			r_cons_break_end();
		break;
	case 'k':
		r_core_debug_kill (core, input + 1);
		break;
	case 'e':
		r_core_debug_esil (core, input + 1);
		break;
	default:{
			const char* help_msg[] = {
			"Usage:", "d", " # Debug commands",
			"db", "[?]", "Breakpoints commands",
			"dbt", "", "Display backtrace based on dbg.btdepth and dbg.btalgo",
			"dc", "[?]", "Continue execution",
			"dd", "[?]", "File descriptors (!fd in r1)",
			"de", "[-sc] [rwx] [rm] [e]", "Debug with ESIL (see de?)",
			"dh", " [handler]", "List or set debugger handler",
			"dH", " [handler]", "Transplant process to a new handler",
			"di", "", "Show debugger backend information (See dh)",
			"dk", "[?]", "List, send, get, set, signal handlers of child",
			"dm", "[?]", "Show memory maps",
			"do", "", "Open process (reload, alias for 'oo')",
			"dp", "[?]", "List, attach to process or thread id",
			"dr", "[?]", "Cpu registers",
			"ds", "[?]", "Step, over, source line",
			"dt", "[?]", "Display instruction traces (dtr=reset)",
			"dw", " <pid>", "Block prompt until pid dies",
			"dx", "[?]", "Inject and run code on target process (See gs)",
			NULL};
			r_core_cmd_help (core, help_msg);
		}
		break;
	}
	if (follow > 0) {
		ut64 pc = r_debug_reg_get (core->dbg, "pc");
		if ((pc < core->offset) || (pc > (core->offset + follow)))
			r_core_cmd0 (core, "sr pc");
	}
	return 0;
}
