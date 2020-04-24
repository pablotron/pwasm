*TODO*
* refactor examples (add `examples/`)
* refactor tests (add `tests/`)
* add `docs/` (`mkdocs`?)
* add jit (`dynasm`?)

*DONE*
* add interpreter (mostly done)
* add generic vec(u32) (added `u32s` to `pwasm_mod_t` and builder)
* add function code parsing (done)
* limit elements to const ops (and globals, in `mod_check_const_expr`)
* add generic vector handling (adde `pwasm_vec_t` from ecs)
* limit globals to const ops (added `parse_const_expr`)
