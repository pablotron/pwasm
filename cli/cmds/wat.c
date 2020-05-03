#include <stdbool.h> // bool
#include <stdlib.h> // size_t
#include <stdio.h> // fopen(), printf()
#include <err.h> // err()
#include "../utils.h" // cli_read_file()
#include "../../pwasm.h" // pwasm_mod_init(), etc

static void
wat_write_utf8_on_data(
  const pwasm_buf_t buf,
  void *data
) {
  FILE *io = data;
  fwrite(buf.ptr, buf.len, 1, io);
}

static void
wat_write_utf8(
  FILE * const io,
  const pwasm_mod_t * const mod,
  const pwasm_slice_t slice
) {
  cli_write_utf8(mod, slice, wat_write_utf8_on_data, io);
}

static void
wat_indent(
  FILE * const io,
  const size_t depth
) {
  fputs("\n", io);
  for (size_t i = 0; i < depth; i++) {
    fputs("  ", io);
  }
}

static void
wat_write_limits(
  FILE * const io,
  const pwasm_limits_t limits
) {
  if (limits.has_max) {
    fprintf(io, " %u %u", limits.min, limits.max);
  } else {
    fprintf(io, " %u", limits.min);
  }
}

static void
wat_write_global_type(
  FILE * const io,
  const pwasm_global_type_t type
) {
  const pwasm_value_type_t val_type = type.type;
  const char * const val_type_name = pwasm_value_type_get_name(val_type);

  fputc(' ', io);
  if (type.mutable) {
    fprintf(io, "(mut %s)", val_type_name);
  } else {
    fputs(val_type_name, io);
  }
}

static void
wat_write_value_types(
  FILE * const io,
  const pwasm_mod_t * const mod,
  const char * const label,
  const pwasm_slice_t types
) {
  for (size_t i = 0; i < types.len; i++) {
    const pwasm_value_type_t type = mod->u32s[types.ofs + i];
    const char * const type_name = pwasm_value_type_get_name(type);

    if (label) {
      fprintf(io, " (%s %s)", label, type_name);
    } else {
      fputs(type_name, io);
    }
  }
}

static void
wat_write_import_type(
  FILE * const io,
  const pwasm_mod_t * const mod,
  const pwasm_type_t type
) {
  wat_write_value_types(io, mod, "param", type.params);
  wat_write_value_types(io, mod, "result", type.results);
}

static void
wat_write_func_params(
  FILE * const io,
  const pwasm_mod_t * const mod,
  const pwasm_slice_t types
) {
  for (size_t i = 0; i < types.len; i++) {
    const pwasm_value_type_t type = mod->u32s[types.ofs + i];
    const char * const type_name = pwasm_value_type_get_name(type);
    fprintf(io, " (param $v%zu %s)", i, type_name);
  }
}

static void
wat_write_func_type(
  FILE * const io,
  const pwasm_mod_t * const mod,
  const pwasm_type_t type
) {
  wat_write_func_params(io, mod, type.params);
  wat_write_value_types(io, mod, "result", type.results);
}

static const char *IMPORTS[] = {
  "func",
  "table",
  "memory",
  "global",
};

static void
wat_write_import(
  FILE * const io,
  const pwasm_mod_t * const mod,
  const size_t id,
  const pwasm_import_t import
) {
  const char * const type_name = IMPORTS[import.type];

  // write import prefix
  wat_indent(io, 1);
  fputs("(import", io);

  // write import module name
  fputc('"', io);
  wat_write_utf8(io, mod, import.module);
  fputc('"', io);

  fputc(' ', io);

  // write import entry name
  fputc('"', io);
  wat_write_utf8(io, mod, import.name);
  fputc('"', io);

  // write import type descriptor and id
  fprintf(io, "(%s $%c%zu", type_name, type_name[0], id);

  switch (import.type) {
  case PWASM_IMPORT_TYPE_FUNC:
    wat_write_import_type(io, mod, mod->types[import.func]);
    break;
  case PWASM_IMPORT_TYPE_TABLE:
    wat_write_limits(io, import.table.limits);
    break;
  case PWASM_IMPORT_TYPE_MEM:
    wat_write_limits(io, import.mem);
    break;
  case PWASM_IMPORT_TYPE_GLOBAL:
    wat_write_global_type(io, import.global);
    break;
  default:
    errx(EXIT_FAILURE, "Unknown import type: %u", import.type);
  }

  // write import suffix
  fputs("))", io);
}

static void
wat_write_imports(
  FILE * const io,
  const pwasm_mod_t * const mod
) {
  size_t sums[PWASM_IMPORT_TYPE_LAST] = { 0, 0, 0, 0 };

  for (size_t i = 0; i < mod->num_imports; i++) {
    const pwasm_import_t import = mod->imports[i];
    wat_write_import(io, mod, sums[import.type]++, import);
  }
}

static void
wat_write_mems(
  FILE * const io,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_mems; i++) {
    const pwasm_limits_t mem = mod->mems[i];
    const size_t id = mod->num_import_types[PWASM_IMPORT_TYPE_MEM] + i;

    wat_indent(io, 1);
    fprintf(io, "(memory $m%zu", id);
    wat_write_limits(io, mem);
    fputc(')', io);
  }
}

static void
wat_write_globals(
  FILE * const io,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_globals; i++) {
    const pwasm_global_t global = mod->globals[i];
    const size_t id = mod->num_import_types[PWASM_IMPORT_TYPE_GLOBAL] + i;

    // write prefix and id
    wat_indent(io, 1);
    fprintf(io, "(global $g%zu", id);

    // write type
    wat_write_global_type(io, global.type);

    // TODO: write expr

    // write suffix
    fputc(')', io);
  }
}


static void
wat_write_func_locals(
  FILE * const io,
  const pwasm_mod_t * const mod,
  const size_t func_ofs,
  size_t ofs
) {
  const pwasm_slice_t slice = mod->codes[func_ofs].locals;

  for (size_t i = 0; i < slice.len; i++) {
    const pwasm_local_t local = mod->locals[slice.ofs + i];
    const char * const name = pwasm_value_type_get_name(local.type);

    for (size_t j = 0; j < local.num; j++) {
      wat_indent(io, 2);
      fprintf(io, "(local $v%zu %s)", ofs + j, name);
    }

    // increment offset
    ofs += local.num;
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
    // TODO (for now, use literal)
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
  FILE * const io,
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
  case PWASM_IMM_INDEX:
    // write index
    fprintf(io, " %s%u", wat_get_inst_index_prefix(in), in.v_index.id);
    break;
  case PWASM_IMM_MEM:
    // write alignment
    if (in.v_mem.align) {
      fprintf(io, " align=%u", in.v_mem.align);
    }

    // write offset
    if (in.v_mem.offset) {
      fprintf(io, " offset=%u", in.v_mem.offset);
    }

    break;
  case PWASM_IMM_I32_CONST:
    fprintf(io, " %u", in.v_i32.val);
    break;
  case PWASM_IMM_I64_CONST:
    fprintf(io, " %lu", in.v_i64.val);
    break;
  case PWASM_IMM_F32_CONST:
    fprintf(io, " %f", in.v_f32.val);
    break;
  case PWASM_IMM_F64_CONST:
    fprintf(io, " %f", in.v_f64.val);
    break;
  default:
    // TODO: add remaining immediates
    break;
  }
}

static void
wat_write_func_insts(
  FILE * const io,
  const pwasm_mod_t * const mod,
  const size_t func_ofs
) {
  const pwasm_slice_t expr = mod->codes[func_ofs].expr;
  size_t depth = 2;

  for (size_t i = 0; i < expr.len - 1; i++) {
    const pwasm_inst_t in = mod->insts[expr.ofs + i];
    const bool has_imm = pwasm_op_get_imm(in.op) != PWASM_IMM_NONE;

    // update depth
    depth -= (in.op == PWASM_OP_ELSE || in.op == PWASM_OP_END);

    // write prefix
    wat_indent(io, depth);
    fputs(has_imm ? "(" : "", io);
    fputs(pwasm_op_get_name(in.op), io);

    // write immediate
    wat_write_inst_imm(io, mod, in);

    // write suffix
    fputs(has_imm ? ")" : "", io);

    // update depth
    depth += (in.op == PWASM_OP_IF || in.op == PWASM_OP_LOOP || in.op == PWASM_OP_BLOCK);

  }
}

static void
wat_write_func(
  FILE * const io,
  const pwasm_mod_t * const mod,
  const size_t func_ofs
) {
  // get param count
  const size_t num_params = mod->types[mod->funcs[func_ofs]].params.len;
  const size_t id = mod->num_import_types[PWASM_IMPORT_TYPE_FUNC] + func_ofs;

  // write func prefix
  wat_indent(io, 1);
  fprintf(io, "(func $f%zu", id);

  wat_write_func_type(io, mod, mod->types[mod->funcs[func_ofs]]);
  wat_write_func_locals(io, mod, func_ofs, num_params);
  wat_write_func_insts(io, mod, func_ofs);

  // write func suffix
  fputc(')', io);
}

static void
wat_write_funcs(
  FILE * const io,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_funcs; i++) {
    wat_write_func(io, mod, i);
  }
}

static const char *
wat_get_export_type_name(
  const pwasm_export_type_t type
) {
  switch (type) {
  case PWASM_EXPORT_TYPE_FUNC: return "func";
  case PWASM_EXPORT_TYPE_GLOBAL: return "global";
  case PWASM_EXPORT_TYPE_MEM: return "memory";
  case PWASM_EXPORT_TYPE_TABLE: return "table";
  default:
    errx(EXIT_FAILURE, "unkown export type: %u", type);
  }
}

static void
wat_write_exports(
  FILE * const io,
  const pwasm_mod_t * const mod
) {
  for (size_t i = 0; i < mod->num_exports; i++) {
    const pwasm_export_t export = mod->exports[i];
    const char * const type_name = wat_get_export_type_name(export.type);

    // write export prefix
    wat_indent(io, 1);
    fputs("(export ", io);

    // write name
    fputc('"', io);
    wat_write_utf8(io, mod, export.name);
    fputc('"', io);

    // write export descriptor
    fprintf(io, " (%s $%c%u)", type_name, type_name[0], export.id);

    // write export prefix
    fputc(')', io);
  }
}

static void
cmd_wat_on_mod(
  const pwasm_mod_t * const mod,
  void *data
) {
  FILE * const io = data;

  // write mod header
  fputs("(module", io);

  // write body
  wat_write_imports(io, mod);
  wat_write_mems(io, mod);
  wat_write_globals(io, mod);
  wat_write_funcs(io, mod);
  wat_write_exports(io, mod);

  // write mod footer
  fputs(")\n", io);
}

int cmd_wat(
  const int argc,
  const char **argv
) {
  // create memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

  // walk path arguments
  for (int i = 2; i < argc; i++) {
    cli_with_mod(&mem_ctx, argv[i], cmd_wat_on_mod, stdout);
  }

  // return success
  return 0;
}
