#include <stdbool.h> // bool
#include <stdlib.h> // size_t
#include <stdio.h> // fopen(), printf()
#include <err.h> // err()
#include "../utils.h" // cli_read_file()
#include "../../pwasm.h" // pwasm_mod_init(), etc

typedef struct {
  FILE *io;
  pwasm_vec_t *ctrl;
} cmd_wat_t;

static void
wat_write_bytes_on_data(
  const pwasm_buf_t buf,
  void *data
) {
  cmd_wat_t * const wat = data;
  fwrite(buf.ptr, buf.len, 1, wat->io);
}

static void
wat_write_bytes(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const pwasm_slice_t slice
) {
  fputc('"', wat->io);
  cli_escape_bytes(mod, slice, wat_write_bytes_on_data, wat);
  fputc('"', wat->io);
}

static void
wat_indent(
  cmd_wat_t * const wat,
  const size_t depth
) {
  if (depth > 0) {
    fputs("\n", wat->io);
    for (size_t i = 0; i < depth; i++) {
      fputs("  ", wat->io);
    }
  } else {
    fputc(' ', wat->io);
  }
}

static void
wat_write_limits(
  cmd_wat_t * const wat,
  const pwasm_limits_t limits
) {
  if (limits.has_max) {
    fprintf(wat->io, " %u %u", limits.min, limits.max);
  } else {
    fprintf(wat->io, " %u", limits.min);
  }
}

static void
wat_write_global_type(
  cmd_wat_t * const wat,
  const pwasm_global_type_t type
) {
  const pwasm_value_type_t val_type = type.type;
  const char * const val_type_name = pwasm_value_type_get_name(val_type);

  fputc(' ', wat->io);
  if (type.mutable) {
    fprintf(wat->io, "(mut %s)", val_type_name);
  } else {
    fputs(val_type_name, wat->io);
  }
}

static void
wat_write_value_types(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const char * const label,
  const pwasm_slice_t types
) {
  for (size_t i = 0; i < types.len; i++) {
    const pwasm_value_type_t type = mod->u32s[types.ofs + i];
    const char * const type_name = pwasm_value_type_get_name(type);

    if (label) {
      fprintf(wat->io, " (%s %s)", label, type_name);
    } else {
      fputs(type_name, wat->io);
    }
  }
}

static void
wat_write_type(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const pwasm_type_t type
) {
  wat_write_value_types(wat, mod, "param", type.params);
  wat_write_value_types(wat, mod, "result", type.results);
}

static void
wat_write_func_params(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const pwasm_slice_t params
) {
  for (size_t i = 0; i < params.len; i++) {
    const pwasm_value_type_t type = mod->u32s[params.ofs + i];
    const char * const type_name = pwasm_value_type_get_name(type);
    fprintf(wat->io, " (param $v%zu %s)", i, type_name);
  }
}

static void
wat_write_func_type(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const pwasm_type_t type
) {
  wat_write_func_params(wat, mod, type.params);
  wat_write_value_types(wat, mod, "result", type.results);
}

static void
wat_write_import(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const size_t id,
  const pwasm_import_t import
) {
  const char * const type_name = pwasm_import_type_get_name(import.type);

  // write import prefix
  wat_indent(wat, 1);
  fputs("(import", wat->io);

  // write import module name
  wat_write_bytes(wat, mod, import.module);

  fputc(' ', wat->io);

  // write import entry name
  wat_write_bytes(wat, mod, import.name);

  // write import type descriptor and id
  fprintf(wat->io, "(%s $%c%zu", type_name, type_name[0], id);

  switch (import.type) {
  case PWASM_IMPORT_TYPE_FUNC:
    wat_write_type(wat, mod, mod->types[import.func]);
    break;
  case PWASM_IMPORT_TYPE_TABLE:
    wat_write_limits(wat, import.table.limits);
    break;
  case PWASM_IMPORT_TYPE_MEM:
    wat_write_limits(wat, import.mem);
    break;
  case PWASM_IMPORT_TYPE_GLOBAL:
    wat_write_global_type(wat, import.global);
    break;
  default:
    errx(EXIT_FAILURE, "Unknown import type: %u", import.type);
  }

  // write import suffix
  fputs("))", wat->io);
}

static void
wat_write_imports(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod
) {
  size_t sums[PWASM_IMPORT_TYPE_LAST] = { 0, 0, 0, 0 };

  for (size_t i = 0; i < mod->num_imports; i++) {
    const pwasm_import_t import = mod->imports[i];
    wat_write_import(wat, mod, sums[import.type]++, import);
  }
}

static void
wat_write_mems(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_mems; i++) {
    const pwasm_limits_t mem = mod->mems[i];
    const size_t id = mod->num_import_types[PWASM_IMPORT_TYPE_MEM] + i;

    wat_indent(wat, 1);
    fprintf(wat->io, "(memory $m%zu", id);
    wat_write_limits(wat, mem);
    fputc(')', wat->io);
  }
}

static const char *
wat_get_inst_index_prefix(
  const pwasm_inst_t in
) {
  // write prefix
  switch (in.op) {
  case PWASM_OP_BR:
  case PWASM_OP_BR_IF:
    // FIXME: add labels?
    return "";
  case PWASM_OP_LOCAL_GET:
  case PWASM_OP_LOCAL_SET:
  case PWASM_OP_LOCAL_TEE:
    return "$v";
  case PWASM_OP_GLOBAL_GET:
  case PWASM_OP_GLOBAL_SET:
    return "$g";
  case PWASM_OP_CALL:
    return "$f";
  default:
    return "";
  }
}

static void
wat_write_inst_imm(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const pwasm_inst_t in
) {
  // get immediate type
  const pwasm_imm_t imm = pwasm_op_get_imm(in.op);
  (void) mod;

  // write immediate
  switch (imm) {
  case PWASM_IMM_NONE:
    // do nothing
    break;
  case PWASM_IMM_BLOCK:
    // write block result type
    if (in.v_block.type != PWASM_RESULT_TYPE_VOID) {
      fprintf(wat->io, " (result %s)", pwasm_result_type_get_name(in.v_block.type));
    }
    break;
  case PWASM_IMM_INDEX:
    // write index
    fprintf(wat->io, " %s%u", wat_get_inst_index_prefix(in), in.v_index);
    break;
  case PWASM_IMM_MEM:
    // write alignment (disabled for now)
    if (false && in.v_mem.align) {
      fprintf(wat->io, " align=%u", in.v_mem.align);
    }

    // write offset
    if (in.v_mem.offset) {
      fprintf(wat->io, " offset=%u", in.v_mem.offset);
    }

    break;
  case PWASM_IMM_I32_CONST:
    fprintf(wat->io, " %u", in.v_i32);
    break;
  case PWASM_IMM_I64_CONST:
    fprintf(wat->io, " %lu", in.v_i64);
    break;
  case PWASM_IMM_F32_CONST:
    fprintf(wat->io, " %f", in.v_f32);
    break;
  case PWASM_IMM_F64_CONST:
    fprintf(wat->io, " %f", in.v_f64);
    break;
  case PWASM_IMM_BR_TABLE:
    for (size_t i = 0; i < in.v_br_table.len; i++) {
      // print branch target
      fprintf(wat->io, " %u", mod->u32s[in.v_br_table.ofs + i]);
    }

    break;
  case PWASM_IMM_CALL_INDIRECT:
    // write indirect call type
    wat_write_type(wat, mod, mod->types[in.v_index]);
    break;
  default:
    errx(EXIT_FAILURE, "Unknown instruction immediate type: %u", imm);
  }
}

static const pwasm_op_t DUMMY_NOP = PWASM_OP_NOP;

static void
wat_write_expr(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const pwasm_slice_t expr,
  size_t depth
) {
  // clear ctrl stack, push dummy nop
  pwasm_vec_clear(wat->ctrl);
  if (!pwasm_vec_push(wat->ctrl, 1, &DUMMY_NOP, NULL)) {
    errx(EXIT_FAILURE, "control stack overflow");
  }

  for (size_t i = 0; i < expr.len; i++) {
    const pwasm_inst_t in = mod->insts[expr.ofs + i];
    const bool has_imm = pwasm_op_get_imm(in.op) != PWASM_IMM_NONE;

    // update depth
    switch (in.op) {
    case PWASM_OP_END:
      {
        depth--;

        // pop control stack, check for error
        pwasm_op_t op;
        if (!pwasm_vec_pop(wat->ctrl, &op)) {
          errx(EXIT_FAILURE, "control stack underflow");
        }

        wat_indent(wat, depth);
        fputs((op == PWASM_OP_ELSE || op == PWASM_OP_IF) ? "))" : ")", wat->io);
      }

      break;
    case PWASM_OP_ELSE:
      wat_indent(wat, depth - 1);
      fputs(") (else", wat->io);

      // pop control stack, check for error
      if (!pwasm_vec_pop(wat->ctrl, NULL)) {
        errx(EXIT_FAILURE, "control stack underflow");
      }

      // push control stack, check for error
      if (!pwasm_vec_push(wat->ctrl, 1, &(in.op), NULL)) {
        errx(EXIT_FAILURE, "control stack overflow");
      }

      break;
    case PWASM_OP_IF:
      // write prefix
      wat_indent(wat, depth);
      fputs("(if", wat->io);

      // write immediate
      wat_write_inst_imm(wat, mod, in);

      // write
      fputs(" (then", wat->io);

      depth++;

      // push control stack, check for error
      if (!pwasm_vec_push(wat->ctrl, 1, &(in.op), NULL)) {
        errx(EXIT_FAILURE, "control stack overflow");
      }

      break;
    case PWASM_OP_BLOCK:
    case PWASM_OP_LOOP:
      // write prefix
      wat_indent(wat, depth);
      fputs("(", wat->io);
      fputs(pwasm_op_get_name(in.op), wat->io);

      // write immediate
      wat_write_inst_imm(wat, mod, in);

      depth++;

      // push control stack, check for error
      if (!pwasm_vec_push(wat->ctrl, 1, &(in.op), NULL)) {
        errx(EXIT_FAILURE, "control stack overflow");
      }

      break;
    default:
      // write prefix
      wat_indent(wat, depth);
      fputs(has_imm ? "(" : "", wat->io);
      fputs(pwasm_op_get_name(in.op), wat->io);

      // write immediate
      wat_write_inst_imm(wat, mod, in);

      // write suffix
      fputs(has_imm ? ")" : "", wat->io);
    }
  }
}

static void
wat_write_globals(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_globals; i++) {
    const pwasm_global_t global = mod->globals[i];
    const size_t id = mod->num_import_types[PWASM_IMPORT_TYPE_GLOBAL] + i;

    // write prefix and id
    wat_indent(wat, 1);
    fprintf(wat->io, "(global $g%zu", id);

    // write type and expr
    wat_write_global_type(wat, global.type);
    wat_write_expr(wat, mod, global.expr, 0);

    // write suffix
    // fputc(')', wat); // handled by END in expr
  }
}

static void
wat_write_func_locals(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const size_t func_ofs,
  size_t ofs
) {
  const pwasm_slice_t slice = mod->codes[func_ofs].locals;

  for (size_t i = 0; i < slice.len; i++) {
    const pwasm_local_t local = mod->locals[slice.ofs + i];
    const char * const name = pwasm_value_type_get_name(local.type);

    for (size_t j = 0; j < local.num; j++) {
      wat_indent(wat, 2);
      fprintf(wat->io, "(local $v%zu %s)", ofs + j, name);
    }

    // increment offset
    ofs += local.num;
  }
}

static void
wat_write_func(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod,
  const size_t func_ofs
) {
  // get param count
  const pwasm_type_t type = mod->types[mod->funcs[func_ofs]];
  const size_t num_params = type.params.len;
  const size_t id = mod->num_import_types[PWASM_IMPORT_TYPE_FUNC] + func_ofs;

  // write func prefix
  wat_indent(wat, 1);
  fprintf(wat->io, "(func $f%zu", id);

  wat_write_func_type(wat, mod, type);
  wat_write_func_locals(wat, mod, func_ofs, num_params);
  wat_write_expr(wat, mod, mod->codes[func_ofs].expr, 2);

  // write func suffix
  // fputc(')', wat->io); // handled by END in expr
}

static void
wat_write_funcs(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_funcs; i++) {
    wat_write_func(wat, mod, i);
  }
}

static const char *
wat_get_table_type_name(
  const uint32_t type
) {
  if (type == 0x70) {
    return "funcref";
  } else {
    errx(EXIT_FAILURE, "Unknown table type: %u", type);
  }
}

static void
wat_write_tables(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_tables; i++) {
    // get table and ID
    const pwasm_table_t table = mod->tables[i];
    const size_t id = mod->num_import_types[PWASM_IMPORT_TYPE_TABLE] + i;

    // write prefix
    wat_indent(wat, 1);
    fprintf(wat->io, "(table $t%zu", id);

    // write limits and type
    wat_write_limits(wat, table.limits);
    fputc(' ', wat->io);
    fputs(wat_get_table_type_name(table.elem_type), wat->io);

    // write suffix
    fputc(')', wat->io);
  }
}

static void
wat_write_start(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod
) {
  if (mod->has_start) {
    wat_indent(wat, 1);
    fprintf(wat->io, "(start $f%u)", mod->start);
  }
}

static void
wat_write_exports(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_exports; i++) {
    const pwasm_export_t export = mod->exports[i];
    const char * const type_name = pwasm_import_type_get_name(export.type);

    // write export prefix
    wat_indent(wat, 1);
    fputs("(export ", wat->io);

    // write name
    wat_write_bytes(wat, mod, export.name);

    // write export descriptor
    fprintf(wat->io, " (%s $%c%u)", type_name, type_name[0], export.id);

    // write export prefix
    fputc(')', wat->io);
  }
}

static void
wat_write_segments(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_segments; i++) {
    const pwasm_segment_t segment = mod->segments[i];

    // write segment prefix
    wat_indent(wat, 1);
    fputs("(data", wat->io);

    if (segment.mem_id) {
      // append memory ID
      fprintf(wat->io, " %u", segment.mem_id);
    }

    if (segment.expr.len > 0) {
      // append offset
      fputs(" (offset ", wat->io);
      wat_write_expr(wat, mod, segment.expr, 0);
      // fputc(')', wat->io); // handled by END in expr
    }

    // write segment data
    wat_write_bytes(wat, mod, segment.data);

    // write segment suffix
    fputc(')', wat->io);
  }
}

static void
wat_write_elems(
  cmd_wat_t * const wat,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_segments; i++) {
    // get element
    const pwasm_elem_t elem = mod->elems[i];

    // write prefix
    wat_indent(wat, 1);
    fputs("(elem", wat->io);

    if (elem.table_id) {
      // append table ID
      fprintf(wat->io, " $t%u", elem.table_id);
    }

    if (elem.expr.len > 0) {
      // append offset
      fputs(" (offset ", wat->io);
      wat_write_expr(wat, mod, elem.expr, 0);
      // fputc(')', wat->io); // handled by END in expr
    }

    // write func IDs
    for (size_t j = 0; j < elem.funcs.len; j++) {
      fprintf(wat->io, " $f%u", mod->u32s[elem.funcs.ofs + j]);
    }

    // write suffix
    fputc(')', wat->io);
  }
}

static void
cmd_wat_on_mod(
  const pwasm_mod_t * const mod,
  void *data
) {
  cmd_wat_t * const wat = data;

  // write mod header
  fputs("(module", wat->io);

  // write body
  wat_write_imports(wat, mod);
  wat_write_mems(wat, mod);
  wat_write_globals(wat, mod);
  wat_write_funcs(wat, mod);
  wat_write_tables(wat, mod);
  wat_write_start(wat, mod);
  wat_write_exports(wat, mod);
  wat_write_segments(wat, mod);
  wat_write_elems(wat, mod);

  // write mod footer
  fputs(")\n", wat->io);
}

int cmd_wat(
  const int argc,
  const char **argv
) {
  // create memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

  // init control stack
  pwasm_vec_t ctrl_stack;
  if (!pwasm_vec_init(&mem_ctx, &ctrl_stack, sizeof(pwasm_op_t))) {
    errx(EXIT_FAILURE, "pwasm_vec_init()");
  }

  // build command data
  cmd_wat_t wat = {
    .io   = stdout,
    .ctrl = &ctrl_stack,
  };

  // walk path arguments
  for (int i = 2; i < argc; i++) {
    cli_with_mod(&mem_ctx, argv[i], cmd_wat_on_mod, &wat);
  }

  // fini control stack
  pwasm_vec_fini(&ctrl_stack);

  // return success
  return 0;
}
