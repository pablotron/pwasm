;;
;; 17-aot-basics.wat: basic aot tests
;;
(module
  ;;
  ;; add_i32s: add two i32s and return the results
  ;;   expect i32 579
  ;;
  (func $add_i32s (result i32)
    (i32.add (i32.const 123) (i32.const 456))
  )

  (export "add_i32s" (func $add_i32s))

  ;;
  ;; trap: trap
  ;;   expect trap
  ;;
  (func $trap (result i32)
    (unreachable)
  )

  (export "trap" (func $trap))

  ;;
  ;; if_else_true: test if/else true
  ;;   expect 321
  ;;
  (func $if_else_true (result i32)
    (i32.const 1)
    (if (result i32) (then
      (i32.const 321)
    ) (else
      (i32.const 456)
    ))
  )

  (export "if_else_true" (func $if_else_true))

  ;;
  ;; if_else_false: test if/else false
  ;;   expect 456
  ;;
  (func $if_else_false (result i32)
    (i32.const 0)
    (if (result i32) (then
      (i32.const 32)
    ) (else
      (i32.const 45)
    ))
  )

  (export "if_else_false" (func $if_else_false))

  ;;
  ;; if_true: test if true
  ;;   expect 314159
  ;;
  (func $if_true (result i32)
    (i32.const 1024)
    (i32.const 1)
    (if (param i32) (result i32) (then
      (drop)
      (i32.const 314159)
    ))
  )

  (export "if_true" (func $if_true))

  ;;
  ;; if_false: test if false
  ;;   expect 22
  ;;
  (func $if_false (result i32)
    (i32.const 22)
    (i32.const 0)
    (if (param i32) (result i32) (then
      (drop)
      (i32.const 44)
    ))
  )

  (export "if_false" (func $if_false))

  ;;
  ;; br_outer: test br to outer block
  ;;   expect 1234
  ;;
  (func $br_outer (result i32)
    (i32.const 1234)
    (br 0)
    (unreachable)
  )

  (export "br_outer" (func $br_outer))

  ;;
  ;; br_inner: test br to outer block
  ;;   expect 5678
  ;;
  (func $br_inner (result i32)
    (i32.const 5678)
    (block
      (br 0)
      (unreachable)
    )
  )

  (export "br_inner" (func $br_inner))

  ;;
  ;; sub: test i32.sub
  ;;   expect i32 a - b
  ;;
  (func $sub (param $a i32) (param $b i32) (result i32)
    (i32.sub (local.get $a) (local.get $b))
  )

  (export "sub" (func $sub))

  ;;
  ;; is_99: test select, i32.sub
  ;;   i32 99: expect 1
  ;;   else: expect 0
  (func $is_99 (param $v i32) (result i32)
    (select
      (i32.const 0)
      (i32.const 1)
      (i32.sub (i32.const 99) (local.get $v))
    )
  )

  (export "is_99" (func $is_99))

  (global $i32 (mut i32) (i32.const 42))

  ;;
  ;; i32_get: get value of i32 global.
  ;;
  (func $i32_get (result i32) ;; return value
    (global.get $i32)
  )

  (export "i32_get" (func $i32_get))

  ;;
  ;; i32_set: set value of i32 global.
  ;;
  (func $i32_set (param $val i32) ;; value
    (global.set $i32 (local.get $val))
  )

  (export "i32_set" (func $i32_set))

  ;;
  ;; i32_eqz: compare value to zero
  ;;
  (func $i32_eqz (param $a i32) (result i32)
    (i32.eqz (local.get $a))
  )

  (export "i32_eqz" (func $i32_eqz))

  ;;
  ;; i32_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $i32_eq (param $a i32) (param $b i32) (result i32)
    (i32.eq (local.get $a) (local.get $b))
  )

  (export "i32_eq" (func $i32_eq))

  ;;
  ;; i32_ne:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $i32_ne (param $a i32) (param $b i32) (result i32)
    (i32.ne (local.get $a) (local.get $b))
  )

  (export "i32_ne" (func $i32_ne))

  ;;
  ;; i32_lt_s:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $i32_lt_s (param $a i32) (param $b i32) (result i32)
    (i32.lt_s (local.get $a) (local.get $b))
  )

  (export "i32_lt_s" (func $i32_lt_s))

  ;;
  ;; i32_lt_u:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $i32_lt_u (param $a i32) (param $b i32) (result i32)
    (i32.lt_u (local.get $a) (local.get $b))
  )

  (export "i32_lt_u" (func $i32_lt_u))

  ;;
  ;; i32_gt_s:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $i32_gt_s (param $a i32) (param $b i32) (result i32)
    (i32.gt_s (local.get $a) (local.get $b))
  )

  (export "i32_gt_s" (func $i32_gt_s))

  ;;
  ;; i32_gt_u:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $i32_gt_u (param $a i32) (param $b i32) (result i32)
    (i32.gt_u (local.get $a) (local.get $b))
  )

  (export "i32_gt_u" (func $i32_gt_u))

  ;;
  ;; i32_le_s:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $i32_le_s (param $a i32) (param $b i32) (result i32)
    (i32.le_s (local.get $a) (local.get $b))
  )

  (export "i32_le_s" (func $i32_le_s))

  ;;
  ;; i32_le_u:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $i32_le_u (param $a i32) (param $b i32) (result i32)
    (i32.le_u (local.get $a) (local.get $b))
  )

  (export "i32_le_u" (func $i32_le_u))

  ;;
  ;; i32_ge_s:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $i32_ge_s (param $a i32) (param $b i32) (result i32)
    (i32.ge_s (local.get $a) (local.get $b))
  )

  (export "i32_ge_s" (func $i32_ge_s))

  ;;
  ;; i32_ge_u:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $i32_ge_u (param $a i32) (param $b i32) (result i32)
    (i32.ge_u (local.get $a) (local.get $b))
  )

  (export "i32_ge_u" (func $i32_ge_u))

  ;;
  ;; i64_eqz:
  ;;   expect $a = 0: i32 1
  ;;   expect $a = 1: i32 0
  ;;
  (func $i64_eqz (param $a i64) (result i32)
    (i64.eqz (local.get $a))
  )

  (export "i64_eqz" (func $i64_eqz))

  ;;
  ;; i64_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $i64_eq (param $a i64) (param $b i64) (result i32)
    (i64.eq (local.get $a) (local.get $b))
  )

  (export "i64_eq" (func $i64_eq))

  ;;
  ;; i64_eq:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $i64_ne (param $a i64) (param $b i64) (result i32)
    (i64.ne (local.get $a) (local.get $b))
  )

  (export "i64_ne" (func $i64_ne))

  ;;
  ;; i64_lt_s:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $i64_lt_s (param $a i64) (param $b i64) (result i32)
    (i64.lt_s (local.get $a) (local.get $b))
  )

  (export "i64_lt_s" (func $i64_lt_s))

  ;;
  ;; i64_lt_u:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $i64_lt_u (param $a i64) (param $b i64) (result i32)
    (i64.lt_u (local.get $a) (local.get $b))
  )

  (export "i64_lt_u" (func $i64_lt_u))

  ;;
  ;; i64_gt_s:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $i64_gt_s (param $a i64) (param $b i64) (result i32)
    (i64.gt_s (local.get $a) (local.get $b))
  )

  (export "i64_gt_s" (func $i64_gt_s))

  ;;
  ;; i64_gt_u:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $i64_gt_u (param $a i64) (param $b i64) (result i32)
    (i64.gt_u (local.get $a) (local.get $b))
  )

  (export "i64_gt_u" (func $i64_gt_u))

  ;;
  ;; i64_le_s:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $i64_le_s (param $a i64) (param $b i64) (result i32)
    (i64.le_s (local.get $a) (local.get $b))
  )

  (export "i64_le_s" (func $i64_le_s))

  ;;
  ;; i64_le_u:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $i64_le_u (param $a i64) (param $b i64) (result i32)
    (i64.le_u (local.get $a) (local.get $b))
  )

  (export "i64_le_u" (func $i64_le_u))

  ;;
  ;; i64_ge_s:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $i64_ge_s (param $a i64) (param $b i64) (result i32)
    (i64.ge_s (local.get $a) (local.get $b))
  )

  (export "i64_ge_s" (func $i64_ge_s))

  ;;
  ;; i64_ge_u:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $i64_ge_u (param $a i64) (param $b i64) (result i32)
    (i64.ge_u (local.get $a) (local.get $b))
  )

  (export "i64_ge_u" (func $i64_ge_u))

  ;;
  ;; f32_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $f32_eq (param $a f32) (param $b f32) (result i32)
    (f32.eq (local.get $a) (local.get $b))
  )

  (export "f32_eq" (func $f32_eq))

  ;;
  ;; f32_ne:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $f32_ne (param $a f32) (param $b f32) (result i32)
    (f32.ne (local.get $a) (local.get $b))
  )

  (export "f32_ne" (func $f32_ne))

  ;;
  ;; f32_lt:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $f32_lt (param $a f32) (param $b f32) (result i32)
    (f32.lt (local.get $a) (local.get $b))
  )

  (export "f32_lt" (func $f32_lt))

  ;;
  ;; f32_gt:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $f32_gt (param $a f32) (param $b f32) (result i32)
    (f32.gt (local.get $a) (local.get $b))
  )

  (export "f32_gt" (func $f32_gt))

  ;;
  ;; f32_le:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $f32_le (param $a f32) (param $b f32) (result i32)
    (f32.le (local.get $a) (local.get $b))
  )

  (export "f32_le" (func $f32_le))

  ;;
  ;; f32_ge:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $f32_ge (param $a f32) (param $b f32) (result i32)
    (f32.ge (local.get $a) (local.get $b))
  )

  (export "f32_ge" (func $f32_ge))

  ;;
  ;; f64_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $f64_eq (param $a f64) (param $b f64) (result i32)
    (f64.eq (local.get $a) (local.get $b))
  )

  (export "f64_eq" (func $f64_eq))

  ;;
  ;; f64_ne:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $f64_ne (param $a f64) (param $b f64) (result i32)
    (f64.ne (local.get $a) (local.get $b))
  )

  (export "f64_ne" (func $f64_ne))

  ;;
  ;; f64_lt:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $f64_lt (param $a f64) (param $b f64) (result i32)
    (f64.lt (local.get $a) (local.get $b))
  )

  (export "f64_lt" (func $f64_lt))

  ;;
  ;; f64_gt:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $f64_gt (param $a f64) (param $b f64) (result i32)
    (f64.gt (local.get $a) (local.get $b))
  )

  (export "f64_gt" (func $f64_gt))

  ;;
  ;; f64_le:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $f64_le (param $a f64) (param $b f64) (result i32)
    (f64.le (local.get $a) (local.get $b))
  )

  (export "f64_le" (func $f64_le))

  ;;
  ;; f64_ge:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $f64_ge (param $a f64) (param $b f64) (result i32)
    (f64.ge (local.get $a) (local.get $b))
  )

  (export "f64_ge" (func $f64_ge))

  ;;
  ;; i32_clz:
  ;;   0x00: expect i32 32
  ;;   0x0f: expect i32 24
  ;;   0x0000f00f: expect i32 16
  ;;
  (func $i32_clz (param $a i32) (result i32)
    (i32.clz (local.get $a))
  )

  (export "i32_clz" (func $i32_clz))

  ;;
  ;; i32_ctz:
  ;;   0x00: expect i32 32
  ;;   0xf0: expect i32 4
  ;;   0xf00f0000: expect i32 16
  ;;
  (func $i32_ctz (param $a i32) (result i32)
    (i32.ctz (local.get $a))
  )

  (export "i32_ctz" (func $i32_ctz))

  ;;
  ;; i32_popcnt:
  ;;   0x00: expect i32 0
  ;;   0xf0: expect i32 4
  ;;   0xf0f0f0f0: expect i32 16
  ;;
  (func $i32_popcnt (param $a i32) (result i32)
    (i32.popcnt (local.get $a))
  )

  (export "i32_popcnt" (func $i32_popcnt))

  ;;
  ;; i32_add:
  ;;   expect i32 $a + $b
  ;;
  (func $i32_add (param $a i32) (param $b i32) (result i32)
    (i32.add (local.get $a) (local.get $b))
  )

  (export "i32_add" (func $i32_add))

  ;;
  ;; i32_sub:
  ;;   expect i32 $a - $b
  ;;
  (func $i32_sub (param $a i32) (param $b i32) (result i32)
    (i32.sub (local.get $a) (local.get $b))
  )

  (export "i32_sub" (func $i32_sub))

  ;;
  ;; i32_mul:
  ;;   expect i32 $a * $b
  ;;
  (func $i32_mul (param $a i32) (param $b i32) (result i32)
    (i32.mul (local.get $a) (local.get $b))
  )

  (export "i32_mul" (func $i32_mul))

  ;;
  ;; i32_div_s:
  ;;   expect i32 $a / $b (signed)
  ;;
  (func $i32_div_s (param $a i32) (param $b i32) (result i32)
    (i32.div_s (local.get $a) (local.get $b))
  )

  (export "i32_div_s" (func $i32_div_s))

  ;;
  ;; i32_div_u:
  ;;   expect i32 $a / $b (unsigned)
  ;;
  (func $i32_div_u (param $a i32) (param $b i32) (result i32)
    (i32.div_u (local.get $a) (local.get $b))
  )

  (export "i32_div_u" (func $i32_div_u))

  ;;
  ;; i32_rem_s:
  ;;   expect i32 $a % $b (signed)
  ;;
  (func $i32_rem_s (param $a i32) (param $b i32) (result i32)
    (i32.rem_s (local.get $a) (local.get $b))
  )

  (export "i32_rem_s" (func $i32_rem_s))

  ;;
  ;; i32_rem_u:
  ;;   expect i32 $a % $b (unsigned)
  ;;
  (func $i32_rem_u (param $a i32) (param $b i32) (result i32)
    (i32.rem_u (local.get $a) (local.get $b))
  )

  (export "i32_rem_u" (func $i32_rem_u))

  ;;
  ;; i32_and:
  ;;   expect i32 $a & $b
  ;;
  (func $i32_and (param $a i32) (param $b i32) (result i32)
    (i32.and (local.get $a) (local.get $b))
  )

  (export "i32_and" (func $i32_and))

  ;;
  ;; i32_or:
  ;;   expect i32 $a | $b
  ;;
  (func $i32_or (param $a i32) (param $b i32) (result i32)
    (i32.or (local.get $a) (local.get $b))
  )

  (export "i32_or" (func $i32_or))

  ;;
  ;; i32_xor:
  ;;   expect i32 $a ^ $b
  ;;
  (func $i32_xor (param $a i32) (param $b i32) (result i32)
    (i32.xor (local.get $a) (local.get $b))
  )

  (export "i32_xor" (func $i32_xor))

  ;;
  ;; i32_shl:
  ;;   expect i32 $a << $b
  ;;
  (func $i32_shl (param $a i32) (param $b i32) (result i32)
    (i32.shl (local.get $a) (local.get $b))
  )

  (export "i32_shl" (func $i32_shl))

  ;;
  ;; i32_shr_s:
  ;;   expect i32 $a >> $b (signed)
  ;;
  (func $i32_shr_s (param $a i32) (param $b i32) (result i32)
    (i32.shr_s (local.get $a) (local.get $b))
  )

  (export "i32_shr_s" (func $i32_shr_s))

  ;;
  ;; i32_shr_u:
  ;;   expect i32 $a >> $b (unsigned)
  ;;
  (func $i32_shr_u (param $a i32) (param $b i32) (result i32)
    (i32.shr_u (local.get $a) (local.get $b))
  )

  (export "i32_shr_u" (func $i32_shr_u))

  ;;
  ;; i32_rotl:
  ;;   expect i32 $a <<< $b
  ;;
  (func $i32_rotl (param $a i32) (param $b i32) (result i32)
    (i32.rotl (local.get $a) (local.get $b))
  )

  (export "i32_rotl" (func $i32_rotl))

  ;;
  ;; i32_rotr:
  ;;   expect i32 $a >>> $b
  ;;
  (func $i32_rotr (param $a i32) (param $b i32) (result i32)
    (i32.rotr (local.get $a) (local.get $b))
  )

  (export "i32_rotr" (func $i32_rotr))

  ;;
  ;; i64_clz:
  ;;   0x00: expect i64 32
  ;;   0x0f: expect i64 24
  ;;   0x0000f00f: expect i64 16
  ;;
  (func $i64_clz (param $a i64) (result i64)
    (i64.clz (local.get $a))
  )

  (export "i64_clz" (func $i64_clz))

  ;;
  ;; i64_ctz:
  ;;   0x00: expect i64 32
  ;;   0xf0: expect i64 4
  ;;   0xf00f0000: expect i64 16
  ;;
  (func $i64_ctz (param $a i64) (result i64)
    (i64.ctz (local.get $a))
  )

  (export "i64_ctz" (func $i64_ctz))

  ;;
  ;; i64_popcnt:
  ;;   0x00: expect i64 0
  ;;   0xf0: expect i64 4
  ;;   0xf0f0f0f0: expect i64 16
  ;;
  (func $i64_popcnt (param $a i64) (result i64)
    (i64.popcnt (local.get $a))
  )

  (export "i64_popcnt" (func $i64_popcnt))


  ;;
  ;; i64_add:
  ;;   expect i64 $a + $b
  ;;
  (func $i64_add (param $a i64) (param $b i64) (result i64)
    (i64.add (local.get $a) (local.get $b))
  )

  (export "i64_add" (func $i64_add))

  ;;
  ;; i64_sub:
  ;;   expect i64 $a - $b
  ;;
  (func $i64_sub (param $a i64) (param $b i64) (result i64)
    (i64.sub (local.get $a) (local.get $b))
  )

  (export "i64_sub" (func $i64_sub))

  ;;
  ;; i64_mul:
  ;;   expect i64 $a * $b
  ;;
  (func $i64_mul (param $a i64) (param $b i64) (result i64)
    (i64.mul (local.get $a) (local.get $b))
  )

  (export "i64_mul" (func $i64_mul))

  ;;
  ;; i64_div_s:
  ;;   expect i64 $a / $b (signed)
  ;;
  (func $i64_div_s (param $a i64) (param $b i64) (result i64)
    (i64.div_s (local.get $a) (local.get $b))
  )

  (export "i64_div_s" (func $i64_div_s))

  ;;
  ;; i64_div_u:
  ;;   expect i64 $a / $b (unsigned)
  ;;
  (func $i64_div_u (param $a i64) (param $b i64) (result i64)
    (i64.div_u (local.get $a) (local.get $b))
  )

  (export "i64_div_u" (func $i64_div_u))

  ;;
  ;; i64_rem_s:
  ;;   expect i64 $a % $b (signed)
  ;;
  (func $i64_rem_s (param $a i64) (param $b i64) (result i64)
    (i64.rem_s (local.get $a) (local.get $b))
  )

  (export "i64_rem_s" (func $i64_rem_s))

  ;;
  ;; i64_rem_u:
  ;;   expect i64 $a % $b (unsigned)
  ;;
  (func $i64_rem_u (param $a i64) (param $b i64) (result i64)
    (i64.rem_u (local.get $a) (local.get $b))
  )

  (export "i64_rem_u" (func $i64_rem_u))

  ;;
  ;; i64_and:
  ;;   expect i64 $a & $b
  ;;
  (func $i64_and (param $a i64) (param $b i64) (result i64)
    (i64.and (local.get $a) (local.get $b))
  )

  (export "i64_and" (func $i64_and))

  ;;
  ;; i64_or:
  ;;   expect i64 $a | $b
  ;;
  (func $i64_or (param $a i64) (param $b i64) (result i64)
    (i64.or (local.get $a) (local.get $b))
  )

  (export "i64_or" (func $i64_or))

  ;;
  ;; i64_xor:
  ;;   expect i64 $a ^ $b
  ;;
  (func $i64_xor (param $a i64) (param $b i64) (result i64)
    (i64.xor (local.get $a) (local.get $b))
  )

  (export "i64_xor" (func $i64_xor))

  ;;
  ;; i64_shl:
  ;;   expect i64 $a << $b
  ;;
  (func $i64_shl (param $a i64) (param $b i64) (result i64)
    (i64.shl (local.get $a) (local.get $b))
  )

  (export "i64_shl" (func $i64_shl))

  ;;
  ;; i64_shr_s:
  ;;   expect i64 $a >> $b (signed)
  ;;
  (func $i64_shr_s (param $a i64) (param $b i64) (result i64)
    (i64.shr_s (local.get $a) (local.get $b))
  )

  (export "i64_shr_s" (func $i64_shr_s))

  ;;
  ;; i64_shr_u:
  ;;   expect i64 $a >> $b (unsigned)
  ;;
  (func $i64_shr_u (param $a i64) (param $b i64) (result i64)
    (i64.shr_u (local.get $a) (local.get $b))
  )

  (export "i64_shr_u" (func $i64_shr_u))

  ;;
  ;; i64_rotl:
  ;;   expect i64 $a <<< $b
  ;;
  (func $i64_rotl (param $a i64) (param $b i64) (result i64)
    (i64.rotl (local.get $a) (local.get $b))
  )

  (export "i64_rotl" (func $i64_rotl))

  ;;
  ;; i64_rotr:
  ;;   expect i64 $a >>> $b
  ;;
  (func $i64_rotr (param $a i64) (param $b i64) (result i64)
    (i64.rotr (local.get $a) (local.get $b))
  )

  (export "i64_rotr" (func $i64_rotr))

  ;;
  ;; f32_abs:
  ;;   expect f32 $a
  ;;
  (func $f32_abs (param $a f32) (result f32)
    (f32.abs (local.get $a))
  )

  (export "f32_abs" (func $f32_abs))

  ;;
  ;; f32_neg:
  ;;   expect f32 -$a
  ;;
  (func $f32_neg (param $a f32) (result f32)
    (f32.neg (local.get $a))
  )

  (export "f32_neg" (func $f32_neg))

  ;;
  ;; f32_ceil:
  ;;   expect f32 ceil($a)
  ;;
  (func $f32_ceil (param $a f32) (result f32)
    (f32.ceil (local.get $a))
  )

  (export "f32_ceil" (func $f32_ceil))

  ;;
  ;; f32_floor:
  ;;   expect f32 floor($a)
  ;;
  (func $f32_floor (param $a f32) (result f32)
    (f32.floor (local.get $a))
  )

  (export "f32_floor" (func $f32_floor))

  ;;
  ;; f32_trunc:
  ;;   expect f32 trunc($a)
  ;;
  (func $f32_trunc (param $a f32) (result f32)
    (f32.trunc (local.get $a))
  )

  (export "f32_trunc" (func $f32_trunc))

  ;;
  ;; f32_nearest:
  ;;   expect f32 nearest($a)
  ;;
  (func $f32_nearest (param $a f32) (result f32)
    (f32.nearest (local.get $a))
  )

  (export "f32_nearest" (func $f32_nearest))

  ;;
  ;; f32_sqrt:
  ;;   expect f32 sqrt($a)
  ;;
  (func $f32_sqrt (param $a f32) (result f32)
    (f32.sqrt (local.get $a))
  )

  (export "f32_sqrt" (func $f32_sqrt))

  ;;
  ;; f32_add:
  ;;   expect f32 $a + $b
  ;;
  (func $f32_add (param $a f32) (param $b f32) (result f32)
    (f32.add (local.get $a) (local.get $b))
  )

  (export "f32_add" (func $f32_add))

  ;;
  ;; f32_sub:
  ;;   expect f32 $a - $b
  ;;
  (func $f32_sub (param $a f32) (param $b f32) (result f32)
    (f32.sub (local.get $a) (local.get $b))
  )

  (export "f32_sub" (func $f32_sub))

  ;;
  ;; f32_mul:
  ;;   expect f32 $a * $b
  ;;
  (func $f32_mul (param $a f32) (param $b f32) (result f32)
    (f32.mul (local.get $a) (local.get $b))
  )

  (export "f32_mul" (func $f32_mul))

  ;;
  ;; f32_div:
  ;;   expect f32 $a / $b
  ;;
  (func $f32_div (param $a f32) (param $b f32) (result f32)
    (f32.div (local.get $a) (local.get $b))
  )

  (export "f32_div" (func $f32_div))

  ;;
  ;; f32_min:
  ;;   expect f32 min($a, $b)
  ;;
  (func $f32_min (param $a f32) (param $b f32) (result f32)
    (f32.min (local.get $a) (local.get $b))
  )

  (export "f32_min" (func $f32_min))

  ;;
  ;; f32_max:
  ;;   expect f32 max($a, $b)
  ;;
  (func $f32_max (param $a f32) (param $b f32) (result f32)
    (f32.max (local.get $a) (local.get $b))
  )

  (export "f32_max" (func $f32_max))

  ;;
  ;; f32_copysign:
  ;;   expect f32 copysign($a, $b)
  ;;
  (func $f32_copysign (param $a f32) (param $b f32) (result f32)
    (f32.copysign (local.get $a) (local.get $b))
  )

  (export "f32_copysign" (func $f32_copysign))

  ;;
  ;; f64_abs:
  ;;   expect f64 $a
  ;;
  (func $f64_abs (param $a f64) (result f64)
    (f64.abs (local.get $a))
  )

  (export "f64_abs" (func $f64_abs))

  ;;
  ;; f64_neg:
  ;;   expect f64 -$a
  ;;
  (func $f64_neg (param $a f64) (result f64)
    (f64.neg (local.get $a))
  )

  (export "f64_neg" (func $f64_neg))

  ;;
  ;; f64_ceil:
  ;;   expect f64 ceil($a)
  ;;
  (func $f64_ceil (param $a f64) (result f64)
    (f64.ceil (local.get $a))
  )

  (export "f64_ceil" (func $f64_ceil))

  ;;
  ;; f64_floor:
  ;;   expect f64 floor($a)
  ;;
  (func $f64_floor (param $a f64) (result f64)
    (f64.floor (local.get $a))
  )

  (export "f64_floor" (func $f64_floor))

  ;;
  ;; f64_trunc:
  ;;   expect f64 trunc($a)
  ;;
  (func $f64_trunc (param $a f64) (result f64)
    (f64.trunc (local.get $a))
  )

  (export "f64_trunc" (func $f64_trunc))

  ;;
  ;; f64_nearest:
  ;;   expect f64 nearest($a)
  ;;
  (func $f64_nearest (param $a f64) (result f64)
    (f64.nearest (local.get $a))
  )

  (export "f64_nearest" (func $f64_nearest))

  ;;
  ;; f64_sqrt:
  ;;   expect f64 sqrt($a)
  ;;
  (func $f64_sqrt (param $a f64) (result f64)
    (f64.sqrt (local.get $a))
  )

  (export "f64_sqrt" (func $f64_sqrt))

  ;;
  ;; f64_add:
  ;;   expect f64 $a + $b
  ;;
  (func $f64_add (param $a f64) (param $b f64) (result f64)
    (f64.add (local.get $a) (local.get $b))
  )

  (export "f64_add" (func $f64_add))

  ;;
  ;; f64_sub:
  ;;   expect f64 $a - $b
  ;;
  (func $f64_sub (param $a f64) (param $b f64) (result f64)
    (f64.sub (local.get $a) (local.get $b))
  )

  (export "f64_sub" (func $f64_sub))

  ;;
  ;; f64_mul:
  ;;   expect f64 $a * $b
  ;;
  (func $f64_mul (param $a f64) (param $b f64) (result f64)
    (f64.mul (local.get $a) (local.get $b))
  )

  (export "f64_mul" (func $f64_mul))

  ;;
  ;; f64_div:
  ;;   expect f64 $a / $b
  ;;
  (func $f64_div (param $a f64) (param $b f64) (result f64)
    (f64.div (local.get $a) (local.get $b))
  )

  (export "f64_div" (func $f64_div))

  ;;
  ;; f64_min:
  ;;   expect f64 min($a, $b)
  ;;
  (func $f64_min (param $a f64) (param $b f64) (result f64)
    (f64.min (local.get $a) (local.get $b))
  )

  (export "f64_min" (func $f64_min))

  ;;
  ;; f64_max:
  ;;   expect f64 max($a, $b)
  ;;
  (func $f64_max (param $a f64) (param $b f64) (result f64)
    (f64.max (local.get $a) (local.get $b))
  )

  (export "f64_max" (func $f64_max))

  ;;
  ;; f64_copysign:
  ;;   expect f64 copysign($a, $b)
  ;;
  (func $f64_copysign (param $a f64) (param $b f64) (result f64)
    (f64.copysign (local.get $a) (local.get $b))
  )

  (export "f64_copysign" (func $f64_copysign))

  ;;
  ;; i32_wrap_i64:
  ;;   expect i32 wrap($a)
  ;;
  (func $i32_wrap_i64 (param $a i64) (result i32)
    (i32.wrap_i64 (local.get $a))
  )

  (export "i32_wrap_i64" (func $i32_wrap_i64))

  ;;
  ;; i32_trunc_f32_s:
  ;;   expect i32 trunc($a)
  ;;
  (func $i32_trunc_f32_s (param $a f32) (result i32)
    (i32.trunc_f32_s (local.get $a))
  )

  (export "i32_trunc_f32_s" (func $i32_trunc_f32_s))

  ;;
  ;; i32_trunc_f32_u:
  ;;   expect i32 trunc($a)
  ;;
  (func $i32_trunc_f32_u (param $a f32) (result i32)
    (i32.trunc_f32_u (local.get $a))
  )

  (export "i32_trunc_f32_u" (func $i32_trunc_f32_u))

  ;;
  ;; i32_trunc_f64_s:
  ;;   expect i32 trunc($a)
  ;;
  (func $i32_trunc_f64_s (param $a f64) (result i32)
    (i32.trunc_f64_s (local.get $a))
  )

  (export "i32_trunc_f64_s" (func $i32_trunc_f64_s))

  ;;
  ;; i32_trunc_f64_u:
  ;;   expect i32 trunc($a)
  ;;
  (func $i32_trunc_f64_u (param $a f64) (result i32)
    (i32.trunc_f64_u (local.get $a))
  )

  (export "i32_trunc_f64_u" (func $i32_trunc_f64_u))

  ;;
  ;; i64_trunc_f32_s:
  ;;   expect i64 trunc($a)
  ;;
  (func $i64_trunc_f32_s (param $a f32) (result i64)
    (i64.trunc_f32_s (local.get $a))
  )

  (export "i64_trunc_f32_s" (func $i64_trunc_f32_s))

  ;;
  ;; i64_trunc_f32_u:
  ;;   expect i64 trunc($a)
  ;;
  (func $i64_trunc_f32_u (param $a f32) (result i64)
    (i64.trunc_f32_u (local.get $a))
  )

  (export "i64_trunc_f32_u" (func $i64_trunc_f32_u))

  ;;
  ;; i64_trunc_f64_s:
  ;;   expect i64 trunc($a)
  ;;
  (func $i64_trunc_f64_s (param $a f64) (result i64)
    (i64.trunc_f64_s (local.get $a))
  )

  (export "i64_trunc_f64_s" (func $i64_trunc_f64_s))

  ;;
  ;; i64_trunc_f64_u:
  ;;   expect i64 trunc($a)
  ;;
  (func $i64_trunc_f64_u (param $a f64) (result i64)
    (i64.trunc_f64_u (local.get $a))
  )

  (export "i64_trunc_f64_u" (func $i64_trunc_f64_u))

  ;;
  ;; f32_convert_i32_s:
  ;;   expect f32 convert_i32_s($a)
  ;;
  (func $f32_convert_i32_s (param $a i32) (result f32)
    (f32.convert_i32_s (local.get $a))
  )

  (export "f32_convert_i32_s" (func $f32_convert_i32_s))

  ;;
  ;; f32_convert_i32_u:
  ;;   expect f32 convert_i32_u($a)
  ;;
  (func $f32_convert_i32_u (param $a i32) (result f32)
    (f32.convert_i32_u (local.get $a))
  )

  (export "f32_convert_i32_u" (func $f32_convert_i32_u))

  ;;
  ;; f32_convert_i64_s:
  ;;   expect f32 convert_i64_s($a)
  ;;
  (func $f32_convert_i64_s (param $a i64) (result f32)
    (f32.convert_i64_s (local.get $a))
  )

  (export "f32_convert_i64_s" (func $f32_convert_i64_s))

  ;;
  ;; f32_convert_i64_u:
  ;;   expect f32 convert_i64_u($a)
  ;;
  (func $f32_convert_i64_u (param $a i64) (result f32)
    (f32.convert_i64_u (local.get $a))
  )

  (export "f32_convert_i64_u" (func $f32_convert_i64_u))

  ;;
  ;; f32_demote_f64:
  ;;   expect f32 demote_f64($a)
  ;;
  (func $f32_demote_f64 (param $a f64) (result f32)
    (f32.demote_f64 (local.get $a))
  )

  (export "f32_demote_f64" (func $f32_demote_f64))

  ;;
  ;; f64_convert_i32_s:
  ;;   expect f64 convert_i32_s($a)
  ;;
  (func $f64_convert_i32_s (param $a i32) (result f64)
    (f64.convert_i32_s (local.get $a))
  )

  (export "f64_convert_i32_s" (func $f64_convert_i32_s))

  ;;
  ;; f64_convert_i32_u:
  ;;   expect f64 convert_i32_u($a)
  ;;
  (func $f64_convert_i32_u (param $a i32) (result f64)
    (f64.convert_i32_u (local.get $a))
  )

  (export "f64_convert_i32_u" (func $f64_convert_i32_u))

  ;;
  ;; f64_convert_i64_s:
  ;;   expect f64 convert_i64_s($a)
  ;;
  (func $f64_convert_i64_s (param $a i64) (result f64)
    (f64.convert_i64_s (local.get $a))
  )

  (export "f64_convert_i64_s" (func $f64_convert_i64_s))

  ;;
  ;; f64_convert_i64_u:
  ;;   expect f64 convert_i64_u($a)
  ;;
  (func $f64_convert_i64_u (param $a i64) (result f64)
    (f64.convert_i64_u (local.get $a))
  )

  (export "f64_convert_i64_u" (func $f64_convert_i64_u))

  ;;
  ;; f64_promote_f32:
  ;;   expect f64 promote_f32($a)
  ;;
  (func $f64_promote_f32 (param $a f32) (result f64)
    (f64.promote_f32 (local.get $a))
  )

  (export "f64_promote_f32" (func $f64_promote_f32))

  ;;
  ;; i32_reinterpret_f32:
  ;;   expect i32 reinterpret($a)
  ;;
  (func $i32_reinterpret_f32 (param $a f32) (result i32)
    (i32.reinterpret_f32 (local.get $a))
  )

  (export "i32_reinterpret_f32" (func $i32_reinterpret_f32))

  ;;
  ;; i64_reinterpret_f64:
  ;;   expect i64 reinterpret($a)
  ;;
  (func $i64_reinterpret_f64 (param $a f64) (result i64)
    (i64.reinterpret_f64 (local.get $a))
  )

  (export "i64_reinterpret_f64" (func $i64_reinterpret_f64))

  ;;
  ;; f32_reinterpret_i32:
  ;;   expect f32 reinterpret($a)
  ;;
  (func $f32_reinterpret_i32 (param $a i32) (result f32)
    (f32.reinterpret_i32 (local.get $a))
  )

  (export "f32_reinterpret_i32" (func $f32_reinterpret_i32))

  ;;
  ;; f64_reinterpret_i64:
  ;;   expect f64 reinterpret($a)
  ;;
  (func $f64_reinterpret_i64 (param $a i64) (result f64)
    (f64.reinterpret_i64 (local.get $a))
  )

  (export "f64_reinterpret_i64" (func $f64_reinterpret_i64))

  ;;
  ;; i32_extend8_s:
  ;;   expect i32 extend8_s($a)
  ;;
  (func $i32_extend8_s (param $a i32) (result i32)
    (i32.extend8_s (local.get $a))
  )

  (export "i32_extend8_s" (func $i32_extend8_s))

  ;;
  ;; i32_extend16_s:
  ;;   expect i32 extend16_s($a)
  ;;
  (func $i32_extend16_s (param $a i32) (result i32)
    (i32.extend16_s (local.get $a))
  )

  (export "i32_extend16_s" (func $i32_extend16_s))

  ;;
  ;; i64_extend8_s:
  ;;   expect i64 extend8_s($a)
  ;;
  (func $i64_extend8_s (param $a i64) (result i64)
    (i64.extend8_s (local.get $a))
  )

  (export "i64_extend8_s" (func $i64_extend8_s))

  ;;
  ;; i64_extend16_s:
  ;;   expect i64 extend16_s($a)
  ;;
  (func $i64_extend16_s (param $a i64) (result i64)
    (i64.extend16_s (local.get $a))
  )

  (export "i64_extend16_s" (func $i64_extend16_s))

  ;;
  ;; i64_extend32_s:
  ;;   expect i64 extend32_s($a)
  ;;
  (func $i64_extend32_s (param $a i64) (result i64)
    (i64.extend32_s (local.get $a))
  )

  (export "i64_extend32_s" (func $i64_extend32_s))

  ;;
  ;; i32_trunc_sat_f32_s
  ;;   expect i32 trunc_sat_f32_s($a)
  ;;
  (func $i32_trunc_sat_f32_s (param $a f32) (result i32)
    (i32.trunc_sat_f32_s (local.get $a))
  )

  (export "i32_trunc_sat_f32_s" (func $i32_trunc_sat_f32_s))

  ;;
  ;; i32_trunc_sat_f32_u
  ;;   expect i32 trunc_sat_f32_u($a)
  ;;
  (func $i32_trunc_sat_f32_u (param $a f32) (result i32)
    (i32.trunc_sat_f32_u (local.get $a))
  )

  (export "i32_trunc_sat_f32_u" (func $i32_trunc_sat_f32_u))

  ;;
  ;; i32_trunc_sat_f64_s
  ;;   expect i32 trunc_sat_f64_s($a)
  ;;
  (func $i32_trunc_sat_f64_s (param $a f64) (result i32)
    (i32.trunc_sat_f64_s (local.get $a))
  )

  (export "i32_trunc_sat_f64_s" (func $i32_trunc_sat_f64_s))

  ;;
  ;; i32_trunc_sat_f64_u
  ;;   expect i32 trunc_sat_f64_u($a)
  ;;
  (func $i32_trunc_sat_f64_u (param $a f64) (result i32)
    (i32.trunc_sat_f64_u (local.get $a))
  )

  (export "i32_trunc_sat_f64_u" (func $i32_trunc_sat_f64_u))

  ;;
  ;; i64_trunc_sat_f32_s
  ;;   expect i64 trunc_sat_f32_s($a)
  ;;
  (func $i64_trunc_sat_f32_s (param $a f32) (result i64)
    (i64.trunc_sat_f32_s (local.get $a))
  )

  (export "i64_trunc_sat_f32_s" (func $i64_trunc_sat_f32_s))

  ;;
  ;; i64_trunc_sat_f32_u
  ;;   expect i64 trunc_sat_f32_u($a)
  ;;
  (func $i64_trunc_sat_f32_u (param $a f32) (result i64)
    (i64.trunc_sat_f32_u (local.get $a))
  )

  (export "i64_trunc_sat_f32_u" (func $i64_trunc_sat_f32_u))

  ;;
  ;; i64_trunc_sat_f64_s
  ;;   expect i64 trunc_sat_f64_s($a)
  ;;
  (func $i64_trunc_sat_f64_s (param $a f64) (result i64)
    (i64.trunc_sat_f64_s (local.get $a))
  )

  (export "i64_trunc_sat_f64_s" (func $i64_trunc_sat_f64_s))

  ;;
  ;; i64_trunc_sat_f64_u
  ;;   expect i64 trunc_sat_f64_u($a)
  ;;
  (func $i64_trunc_sat_f64_u (param $a f64) (result i64)
    (i64.trunc_sat_f64_u (local.get $a))
  )

  (export "i64_trunc_sat_f64_u" (func $i64_trunc_sat_f64_u))

  ;;
  ;; v128_const
  ;;   expect i32 0x03020100
  ;;
  (func $v128_const (result i32)
    (i32x4.extract_lane 0
      (v128.const i32x4 0x03020100 0x07060504 0x0b0a0908 0x0f0e0d0c))
  )

  (export "v128_const" (func $v128_const))

  ;;
  ;; v8x16_shuffle
  ;;   expect i32 0x0b090301
  ;;
  (func $v8x16_shuffle (result i32)
    (i32x4.extract_lane 0
      (v8x16.shuffle 1 3 17 19  0 0 0 0  0 0 0 0  0 0 0 0
        (v128.const i32x4 0x03020100 0x07060504 0x0b0a0908 0x0f0e0d0c)
        (v128.const i32x4 0x0b0a0908 0x0f0e0d0c 0x03020100 0x07060504)))
  )

  (export "v8x16_shuffle" (func $v8x16_shuffle))

  ;;
  ;; v8x16_swizzle
  ;;   expect i32 0xf6f4f2f0
  ;;
  (func $v8x16_swizzle (result i32)
    (i32x4.extract_lane 0
      (v8x16.swizzle
        (v128.const i32x4 0xf3f2f1f0 0xf7f6f5f4 0x0b0a0908 0x0f0e0d0c)
        (v128.const i32x4 0x06040200 0x00000000 0x00000000 0x00000000)))
  )

  (export "v8x16_swizzle" (func $v8x16_swizzle))

  ;;
  ;; i8x16_splat
  ;;   expect i32 0xF2F2F2F2
  ;;
  (func $i8x16_splat (result i32)
    (i32x4.extract_lane 0
      (i8x16.splat (i32.const 0xF2)))
  )

  (export "i8x16_splat" (func $i8x16_splat))

  ;;
  ;; i16x8_splat
  ;;   expect i32 0xF00FF00F
  ;;
  (func $i16x8_splat (result i32)
    (i32x4.extract_lane 0
      (i16x8.splat (i32.const 0xF00F)))
  )

  (export "i16x8_splat" (func $i16x8_splat))

  ;;
  ;; i32x4_splat
  ;;   expect i32 0x08070605
  ;;
  (func $i32x4_splat (result i32)
    (i32x4.extract_lane 0
      (i32x4.splat (i32.const 0x08070605)))
  )

  (export "i32x4_splat" (func $i32x4_splat))

  ;;
  ;; i64x2_splat
  ;;   expect i64 0x7EDCBA9876543210
  ;;
  ;; NOTE: this was originally 0xFEDCBA..., but there appears to be a
  ;; bug in WABT when extracting the high bit of i64 constants.
  ;;
  (func $i64x2_splat (result i64)
    (i64x2.extract_lane 0
      (i64x2.splat (i64.const 0x7EDCBA9876543210)))
  )

  (export "i64x2_splat" (func $i64x2_splat))

  ;;
  ;; f32x4_splat
  ;;   expect f32 3.14159
  ;;
  (func $f32x4_splat (result f32)
    (f32x4.extract_lane 1
      (f32x4.splat (f32.const 3.14159)))
  )

  (export "f32x4_splat" (func $f32x4_splat))

  ;;
  ;; f64x2_splat
  ;;   expect f64 3.14159
  ;;
  (func $f64x2_splat (result f64)
    (f64x2.extract_lane 1
      (f64x2.splat (f64.const 3.14159)))
  )

  (export "f64x2_splat" (func $f64x2_splat))

  ;;
  ;; i8x16_extract_lane_s
  ;;   expect i32 -2
  ;;
  (func $i8x16_extract_lane_s (result i32)
    (i8x16.extract_lane_s 3
      (v128.const i32x4 0xfe040200 0x00000000 0x00000000 0x00000000))
  )

  (export "i8x16_extract_lane_s" (func $i8x16_extract_lane_s))

  ;;
  ;; i8x16_extract_lane_u
  ;;   expect i32 0x7f
  ;;
  (func $i8x16_extract_lane_u (result i32)
    (i8x16.extract_lane_u 4
      (v128.const i32x4 0x00040200 0x0000007f 0x00000000 0x00000000))
  )

  (export "i8x16_extract_lane_u" (func $i8x16_extract_lane_u))

  ;;
  ;; i8x16_replace_lane
  ;;   expect i32 0xcd
  ;;
  (func $i8x16_replace_lane (result i32)
    (i8x16.extract_lane_u 4
      (i8x16.replace_lane 4
        (v128.const i32x4 0x00000000 0x00000000 0x00000000 0x00000000)
        (i32.const 0xcd)))
  )

  (export "i8x16_replace_lane" (func $i8x16_replace_lane))

  ;;
  ;; i16x8_extract_lane_s
  ;;   expect i32 -1
  ;;
  (func $i16x8_extract_lane_s (result i32)
    (i16x8.extract_lane_s 0
      (i16x8.splat (i32.const 0x0000FFFF)))
  )

  (export "i16x8_extract_lane_s" (func $i16x8_extract_lane_s))

  ;;
  ;; i16x8_extract_lane_u
  ;;   expect i32 0xFFFF
  ;;
  (func $i16x8_extract_lane_u (result i32)
    (i16x8.extract_lane_u 0
      (i16x8.splat (i32.const 0x0000FFFF)))
  )

  (export "i16x8_extract_lane_u" (func $i16x8_extract_lane_u))

  ;;
  ;; i16x8_replace_lane
  ;;   expect i32 0xF00F
  ;;
  (func $i16x8_replace_lane (result i32)
    (i16x8.extract_lane_u 0
      (i16x8.replace_lane 0
        (i16x8.splat (i32.const 0x0000FFFF))
        (i32.const 0xF00F)))
  )

  (export "i16x8_replace_lane" (func $i16x8_replace_lane))

  ;;
  ;; i32x4_extract_lane
  ;;   expect i32 0xFEDCBA98
  ;;
  (func $i32x4_extract_lane (result i32)
    (i32x4.extract_lane 1
      (v128.const i32x4 0x00000000 0xFEDCBA98 0x00000000 0x00000000))
  )

  (export "i32x4_extract_lane" (func $i32x4_extract_lane))

  ;;
  ;; i32x4_replace_lane
  ;;   expect i32 0xBEDCBA98
  ;;
  ;; NOTE: The original value was 0xFEDCBA98, but the current version of
  ;; WABT (1.0.13) is incorrectly treating i32.const values as signed
  ;; and compacting them.
  ;;
  (func $i32x4_replace_lane (result i32)
    (i32x4.extract_lane 3
      (i32x4.replace_lane 3
        (i32x4.splat (i32.const 0x0))
        (i32.const 0xBEDCBA98)))
  )

  (export "i32x4_replace_lane" (func $i32x4_replace_lane))

  ;;
  ;; i64x2_extract_lane
  ;;   expect i64 0xBEDCBA9876543210
  ;;
  ;; NOTE: The original value was 0xFEDCBA9876543210, but the current
  ;; version of WABT (1.0.13) is incorrectly treating i64.const values
  ;; as signed and compacting them.
  ;;
  (func $i64x2_extract_lane (result i64)
    (i64x2.extract_lane 1
      (i64x2.replace_lane 1
        (i64x2.splat (i64.const 0x0))
        (i64.const 0xBEDCBA9876543210)))
  )

  (export "i64x2_extract_lane" (func $i64x2_extract_lane))

  ;;
  ;; i64x2_replace_lane
  ;;   expect i64 0xBDDDFFFFDDDDFFFF
  ;;
  ;; NOTE: The original value was 0xDDDDFFFFDDDDFFFF, but the current
  ;; version of WABT (1.0.13) is incorrectly treating i64.const values
  ;; as signed and compacting them.
  ;;
  (func $i64x2_replace_lane (result i64)
    (i64x2.extract_lane 0
      (i64x2.replace_lane 0
        (i64x2.splat (i64.const 0x0))
        (i64.const 0xBDDDFFFFDDDDFFFF)))
  )

  (export "i64x2_replace_lane" (func $i64x2_replace_lane))

  ;;
  ;; f32x4_extract_lane
  ;;   expect f32 3.14159
  ;;
  (func $f32x4_extract_lane (result f32)
    (f32x4.extract_lane 0
      (v128.const f32x4 3.14159 0 0 0))
  )

  (export "f32x4_extract_lane" (func $f32x4_extract_lane))

  ;;
  ;; f32x4_replace_lane
  ;;   expect f32 -1.5
  ;;
  (func $f32x4_replace_lane (result f32)
    (f32x4.extract_lane 1
      (f32x4.replace_lane 1
        (v128.const f32x4 0 0 0 0)
        (f32.const -1.5)))
  )

  (export "f32x4_replace_lane" (func $f32x4_replace_lane))

  ;;
  ;; f64x2_extract_lane
  ;;   expect f64 3.14159
  ;;
  (func $f64x2_extract_lane (result f64)
    (f64x2.extract_lane 0
      (v128.const f64x2 3.14159 0))
  )

  (export "f64x2_extract_lane" (func $f64x2_extract_lane))

  ;;
  ;; f64x2_replace_lane
  ;;   expect f64 -3.14159
  ;;
  (func $f64x2_replace_lane (result f64)
    (f64x2.extract_lane 1
      (f64x2.replace_lane 1
        (v128.const f64x2 0 0)
        (f64.const -3.14159)))
  )

  (export "f64x2_replace_lane" (func $f64x2_replace_lane))

  ;;
  ;; i64_const
  ;;   expect i64 0x0123456789abcdef
  ;;
  (func $i64_const (result i64)
    (i64.const 0x0123456789abcdef)
  )

  (export "i64_const" (func $i64_const))

  ;;
  ;; f32_const
  ;;   expect f32 3.14159
  ;;
  (func $f32_const (result f32)
    (f32.const 3.14159)
  )

  (export "f32_const" (func $f32_const))

  ;;
  ;; f64_const
  ;;   expect f64 3.14159
  ;;
  (func $f64_const (result f64)
    (f64.const 3.14159)
  )

  (export "f64_const" (func $f64_const))

  ;;
  ;; i8x16_eq
  ;;   expect i32 0xff00ff00
  ;;
  (func $i8x16_eq (result i32)
    (i32x4.extract_lane 0
      (i8x16.eq
        (v128.const i8x16 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15)
        (v128.const i8x16 1 1 3 3 4 5 6 7 8 9 10 11 12 13 14 15)))
  )

  (export "i8x16_eq" (func $i8x16_eq))

  ;;
  ;; i8x16_ne
  ;;   expect i32 0x00ff00ff
  ;;
  (func $i8x16_ne (result i32)
    (i32x4.extract_lane 0
      (i8x16.ne
        (v128.const i8x16 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15)
        (v128.const i8x16 1 1 3 3 4 5 6 7 8 9 10 11 12 13 14 15)))
  )

  (export "i8x16_ne" (func $i8x16_ne))

  ;;
  ;; i8x16_lt_s
  ;;   expect i32 0x00ff00ff
  ;;
  (func $i8x16_lt_s (result i32)
    (i32x4.extract_lane 0
      (i8x16.lt_s
        (v128.const i8x16 -1 0   2 3  0 0 0 0  0 0 0 0  0 0 0 0)
        (v128.const i8x16  1 0 127 3  0 0 0 0  0 0 0 0  0 0 0 0)))
  )

  (export "i8x16_lt_s" (func $i8x16_lt_s))

  ;;
  ;; i8x16_lt_u
  ;;   expect i32 0x00ff00ff
  ;;
  (func $i8x16_lt_u (result i32)
    (i32x4.extract_lane 0
      (i8x16.lt_u
        (v128.const i8x16 0   0   2 3  0 0 0 0  0 0 0 0  0 0 0 0)
        (v128.const i8x16 255 0 127 3  0 0 0 0  0 0 0 0  0 0 0 0)))
  )

  (export "i8x16_lt_u" (func $i8x16_lt_u))

  ;;
  ;; i8x16_gt_s
  ;;   expect i32 0x00ff00ff
  ;;
  (func $i8x16_gt_s (result i32)
    (i32x4.extract_lane 0
      (i8x16.gt_s
        (v128.const i8x16  1 0 127 3  0 0 0 0  0 0 0 0  0 0 0 0)
        (v128.const i8x16 -1 0   2 3  0 0 0 0  0 0 0 0  0 0 0 0)))
  )

  (export "i8x16_gt_s" (func $i8x16_gt_s))

  ;;
  ;; i8x16_gt_u
  ;;   expect i32 0x00ff00ff
  ;;
  (func $i8x16_gt_u (result i32)
    (i32x4.extract_lane 0
      (i8x16.gt_u
        (v128.const i8x16 255 0 127 3  0 0 0 0  0 0 0 0  0 0 0 0)
        (v128.const i8x16 0   0   2 3  0 0 0 0  0 0 0 0  0 0 0 0)))
  )

  (export "i8x16_gt_u" (func $i8x16_gt_u))

  ;;
  ;; i8x16_le_s
  ;;   expect i32 0x00ff00ff
  ;;
  (func $i8x16_le_s (result i32)
    (i32x4.extract_lane 0
      (i8x16.le_s
        (v128.const i8x16 0  0 -1 4  0 0 0 0  0 0 0 0  0 0 0 0)
        (v128.const i8x16 0 -1  0 0  0 0 0 0  0 0 0 0  0 0 0 0)))
  )

  (export "i8x16_le_s" (func $i8x16_le_s))

  ;;
  ;; i8x16_le_u
  ;;   expect i32 0x00ff00ff
  ;;
  (func $i8x16_le_u (result i32)
    (i32x4.extract_lane 0
      (i8x16.le_u
        (v128.const i8x16 0 255   0 255  0 0 0 0  0 0 0 0  0 0 0 0)
        (v128.const i8x16 0   0 255   0  0 0 0 0  0 0 0 0  0 0 0 0)))
  )

  (export "i8x16_le_u" (func $i8x16_le_u))

  ;;
  ;; i8x16_ge_s
  ;;   expect i32 0x00ff00ff
  ;;
  (func $i8x16_ge_s (result i32)
    (i32x4.extract_lane 0
      (i8x16.ge_s
        (v128.const i8x16 100 -1 127 0  0 0 0 0  0 0 0 0  0 0 0 0)
        (v128.const i8x16   0  1  -1 1  0 0 0 0  0 0 0 0  0 0 0 0)))
  )

  (export "i8x16_ge_s" (func $i8x16_ge_s))

  ;;
  ;; i8x16_ge_u
  ;;   expect i32 0x00ff00ff
  ;;
  (func $i8x16_ge_u (result i32)
    (i32x4.extract_lane 0
      (i8x16.ge_u
        (v128.const i8x16 1 0 255 0  0 0 0 0  0 0 0 0  0 0 0 0)
        (v128.const i8x16 0 1   0 1  0 0 0 0  0 0 0 0  0 0 0 0)))
  )

  (export "i8x16_ge_u" (func $i8x16_ge_u))

  ;;
  ;; i16x8_eq
  ;;   expect i32 0x0000ffff
  ;;
  (func $i16x8_eq (result i32)
    (i32x4.extract_lane 0
      (i16x8.eq
        (v128.const i16x8 0 0  0 0  0 0  0 0)
        (v128.const i16x8 0 1  0 0  0 0  0 0)))
  )

  (export "i16x8_eq" (func $i16x8_eq))

  ;;
  ;; i16x8_ne
  ;;   expect i32 0xffff0000
  ;;
  (func $i16x8_ne (result i32)
    (i32x4.extract_lane 0
      (i16x8.ne
        (v128.const i16x8 0 0  0 0  0 0  0 0)
        (v128.const i16x8 0 1  0 0  0 0  0 0)))
  )

  (export "i16x8_ne" (func $i16x8_ne))

  ;;
  ;; i16x8_lt_s
  ;;   expect i32 0xffff0000
  ;;
  (func $i16x8_lt_s (result i32)
    (i32x4.extract_lane 0
      (i16x8.lt_s
        (v128.const i16x8 0 -1  0 0  0 0  0 0)
        (v128.const i16x8 0  0  0 0  0 0  0 0)))
  )

  (export "i16x8_lt_s" (func $i16x8_lt_s))

  ;;
  ;; i16x8_lt_u
  ;;   expect i32 0xffff0000
  ;;
  (func $i16x8_lt_u (result i32)
    (i32x4.extract_lane 0
      (i16x8.lt_u
        (v128.const i16x8 0 0x0000  0 0  0 0  0 0)
        (v128.const i16x8 0 0xFFFF  0 0  0 0  0 0)))
  )

  (export "i16x8_lt_u" (func $i16x8_lt_u))

  ;;
  ;; i16x8_gt_s
  ;;   expect i32 0xffff0000
  ;;
  (func $i16x8_gt_s (result i32)
    (i32x4.extract_lane 0
      (i16x8.gt_s
        (v128.const i16x8 0  0  0 0  0 0  0 0)
        (v128.const i16x8 0 -1  0 0  0 0  0 0)))
  )

  (export "i16x8_gt_s" (func $i16x8_gt_s))

  ;;
  ;; i16x8_gt_u
  ;;   expect i32 0xffff0000
  ;;
  (func $i16x8_gt_u (result i32)
    (i32x4.extract_lane 0
      (i16x8.gt_u
        (v128.const i16x8 0 0xFFFF  0 0  0 0  0 0)
        (v128.const i16x8 0 0x0000  0 0  0 0  0 0)))
  )

  (export "i16x8_gt_u" (func $i16x8_gt_u))

  ;;
  ;; i16x8_le_s
  ;;   expect i32 0xffff0000
  ;;
  (func $i16x8_le_s (result i32)
    (i32x4.extract_lane 0
      (i16x8.le_s
        (v128.const i16x8  0 0  0 0  0 0  0 0)
        (v128.const i16x8 -1 0  0 0  0 0  0 0)))
  )

  (export "i16x8_le_s" (func $i16x8_le_s))

  ;;
  ;; i16x8_le_u
  ;;   expect i32 0xffff0000
  ;;
  (func $i16x8_le_u (result i32)
    (i32x4.extract_lane 0
      (i16x8.le_u
        (v128.const i16x8 0xFFFF 0  0 0  0 0  0 0)
        (v128.const i16x8 0x0000 0  0 0  0 0  0 0)))
  )

  (export "i16x8_le_u" (func $i16x8_le_u))

  ;;
  ;; i16x8_ge_s
  ;;   expect i32 0xffff0000
  ;;
  (func $i16x8_ge_s (result i32)
    (i32x4.extract_lane 0
      (i16x8.ge_s
        (v128.const i16x8 -1 0  0 0  0 0  0 0)
        (v128.const i16x8  0 0  0 0  0 0  0 0)))
  )

  (export "i16x8_ge_s" (func $i16x8_ge_s))

  ;;
  ;; i16x8_ge_u
  ;;   expect i32 0xffff0000
  ;;
  (func $i16x8_ge_u (result i32)
    (i32x4.extract_lane 0
      (i16x8.ge_u
        (v128.const i16x8 0x0000 0  0 0  0 0  0 0)
        (v128.const i16x8 0xFFFF 0  0 0  0 0  0 0)))
  )

  (export "i16x8_ge_u" (func $i16x8_ge_u))
)
