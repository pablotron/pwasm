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
  (func $i32_eq (param $a i32)
                     (param $b i32)
                     (result i32)
    (i32.eq (local.get $a) (local.get $b))
  )

  (export "i32_eq" (func $i32_eq))

  ;;
  ;; i32_ne:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $i32_ne (param $a i32)
                     (param $b i32)
                     (result i32)
    (i32.ne (local.get $a) (local.get $b))
  )

  (export "i32_ne" (func $i32_ne))

  ;;
  ;; i32_lt_s:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $i32_lt_s (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.lt_s (local.get $a) (local.get $b))
  )

  (export "i32_lt_s" (func $i32_lt_s))

  ;;
  ;; i32_lt_u:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $i32_lt_u (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.lt_u (local.get $a) (local.get $b))
  )

  (export "i32_lt_u" (func $i32_lt_u))

  ;;
  ;; i32_gt_s:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $i32_gt_s (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.gt_s (local.get $a) (local.get $b))
  )

  (export "i32_gt_s" (func $i32_gt_s))

  ;;
  ;; i32_gt_u:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $i32_gt_u (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.gt_u (local.get $a) (local.get $b))
  )

  (export "i32_gt_u" (func $i32_gt_u))

  ;;
  ;; i32_le_s:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $i32_le_s (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.le_s (local.get $a) (local.get $b))
  )

  (export "i32_le_s" (func $i32_le_s))

  ;;
  ;; i32_le_u:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $i32_le_u (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.le_u (local.get $a) (local.get $b))
  )

  (export "i32_le_u" (func $i32_le_u))

  ;;
  ;; i32_ge_s:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $i32_ge_s (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.ge_s (local.get $a) (local.get $b))
  )

  (export "i32_ge_s" (func $i32_ge_s))

  ;;
  ;; i32_ge_u:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $i32_ge_u (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.ge_u (local.get $a) (local.get $b))
  )

  (export "i32_ge_u" (func $i32_ge_u))

  ;;
  ;; i64_eqz:
  ;;   expect $a = 0: i32 1
  ;;   expect $a = 1: i32 0
  ;;
  (func $i64_eqz (param $a i64)
                      (result i32)
    (i64.eqz (local.get $a))
  )

  (export "i64_eqz" (func $i64_eqz))

  ;;
  ;; i64_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $i64_eq (param $a i64)
                     (param $b i64)
                     (result i32)
    (i64.eq (local.get $a) (local.get $b))
  )

  (export "i64_eq" (func $i64_eq))

  ;;
  ;; i64_eq:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $i64_ne (param $a i64)
                     (param $b i64)
                     (result i32)
    (i64.ne (local.get $a) (local.get $b))
  )

  (export "i64_ne" (func $i64_ne))

  ;;
  ;; i64_lt_s:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $i64_lt_s (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.lt_s (local.get $a) (local.get $b))
  )

  (export "i64_lt_s" (func $i64_lt_s))

  ;;
  ;; i64_lt_u:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $i64_lt_u (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.lt_u (local.get $a) (local.get $b))
  )

  (export "i64_lt_u" (func $i64_lt_u))

  ;;
  ;; i64_gt_s:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $i64_gt_s (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.gt_s (local.get $a) (local.get $b))
  )

  (export "i64_gt_s" (func $i64_gt_s))

  ;;
  ;; i64_gt_u:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $i64_gt_u (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.gt_u (local.get $a) (local.get $b))
  )

  (export "i64_gt_u" (func $i64_gt_u))

  ;;
  ;; i64_le_s:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $i64_le_s (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.le_s (local.get $a) (local.get $b))
  )

  (export "i64_le_s" (func $i64_le_s))

  ;;
  ;; i64_le_u:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $i64_le_u (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.le_u (local.get $a) (local.get $b))
  )

  (export "i64_le_u" (func $i64_le_u))

  ;;
  ;; i64_ge_s:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $i64_ge_s (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.ge_s (local.get $a) (local.get $b))
  )

  (export "i64_ge_s" (func $i64_ge_s))

  ;;
  ;; i64_ge_u:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $i64_ge_u (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.ge_u (local.get $a) (local.get $b))
  )

  (export "i64_ge_u" (func $i64_ge_u))

  ;;
  ;; f32_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $f32_eq (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.eq (local.get $a) (local.get $b))
  )

  (export "f32_eq" (func $f32_eq))

  ;;
  ;; f32_ne:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $f32_ne (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.ne (local.get $a) (local.get $b))
  )

  (export "f32_ne" (func $f32_ne))

  ;;
  ;; f32_lt:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $f32_lt (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.lt (local.get $a) (local.get $b))
  )

  (export "f32_lt" (func $f32_lt))

  ;;
  ;; f32_gt:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $f32_gt (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.gt (local.get $a) (local.get $b))
  )

  (export "f32_gt" (func $f32_gt))

  ;;
  ;; f32_le:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $f32_le (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.le (local.get $a) (local.get $b))
  )

  (export "f32_le" (func $f32_le))

  ;;
  ;; f32_ge:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $f32_ge (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.ge (local.get $a) (local.get $b))
  )

  (export "f32_ge" (func $f32_ge))

  ;;
  ;; f64_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $f64_eq (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.eq (local.get $a) (local.get $b))
  )

  (export "f64_eq" (func $f64_eq))

  ;;
  ;; f64_ne:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $f64_ne (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.ne (local.get $a) (local.get $b))
  )

  (export "f64_ne" (func $f64_ne))

  ;;
  ;; f64_lt:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $f64_lt (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.lt (local.get $a) (local.get $b))
  )

  (export "f64_lt" (func $f64_lt))

  ;;
  ;; f64_gt:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $f64_gt (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.gt (local.get $a) (local.get $b))
  )

  (export "f64_gt" (func $f64_gt))

  ;;
  ;; f64_le:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $f64_le (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.le (local.get $a) (local.get $b))
  )

  (export "f64_le" (func $f64_le))

  ;;
  ;; f64_ge:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $f64_ge (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.ge (local.get $a) (local.get $b))
  )

  (export "f64_ge" (func $f64_ge))
)
