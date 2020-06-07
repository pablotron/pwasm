;;
;; 13-ops.wat: Module containing test opcode test functions
;;
(module
  ;;
  ;; test_nop: expect i32 1234
  ;;
  (func $test_nop (result i32)
    (nop)
    (i32.const 1234)
  )

  (export "test_nop" (func $test_nop))

  ;;
  ;; test_block: expect i32 34
  ;;
  (func $test_block (result i32)
    (local $val i32)
    (local.set $val (i32.const 12))
    (block
      (local.set $val (i32.const 34))
    )
    (local.get $val)
  )

  (export "test_block" (func $test_block))

  ;;
  ;; test_loop: expect i32 5
  ;;
  (func $test_loop (result i32)
    (local $i i32)
    (local $val i32)
    (local.set $i (i32.const 5))
    (local.set $val (i32.const 0))
    (loop
      (local.set $val (i32.add (local.get $val) (i32.const 1)))
      (local.tee $i (i32.sub (local.get $i) (i32.const 1)))
      (br_if 0)
    )
    (local.get $val)
  )

  (export "test_loop" (func $test_loop))

  ;;
  ;; test_if_else:
  ;;   even param: expect 22
  ;;   odd param: expect 33
  ;;
  (func $test_if_else (param $val i32)
                      (result i32)
    (i32.rem_u (local.get $val) (i32.const 2))
    (if (result i32) (then
      (i32.const 33)
    ) (else
      (i32.const 22)
    ))
  )

  (export "test_if_else" (func $test_if_else))

  ;; test_call: expect i32 55
  (func $test_call (result i32)
    (i32.add
      (call $test_if_else (i32.const 0))
      (call $test_if_else (i32.const 1)))
  )

  (export "test_call" (func $test_call))

  ;; test for call_indirect
  (func $add (param $a i32)
             (param $b i32)
             (result i32)
    (i32.add (local.get $a) (local.get $b))
  )

  ;; test for call_indirect
  (func $sub (param $a i32)
             (param $b i32)
             (result i32)
    (i32.sub (local.get $a) (local.get $b))
  )

  ;; test for call_indirect
  (func $mul (param $a i32)
             (param $b i32)
             (result i32)
    (i32.mul (local.get $a) (local.get $b))
  )

  ;; test for call_indirect
  (func $none (param $a i32)
              (param $b i32)
              (result i32)
    (local.get $a)
  )

  ;; call indirect function table
  (table $fns funcref (elem $add $sub $mul $none))

  ;; test_call_indirect: expect i32 55
  (func $test_call_indirect (param $op i32)
                            (param $a i32)
                            (param $b i32)
                            (result i32)
    (local.get $a)
    (local.get $b)
    (local.get $op)
    (call_indirect (param i32) (param i32) (result i32))
  )

  (export "test_call_indirect" (func $test_call_indirect))

  ;;
  ;; test_drop: expect i32 2
  ;;
  (func $test_drop (result i32)
    (i32.const 2)
    (i32.const 1)
    (drop)
  )

  (export "test_drop" (func $test_drop))

  ;;
  ;; test_select:
  ;;   even param: expect i32 0
  ;;   odd param: expect i32 1
  ;;
  (func $test_select (param $a i32)
                     (result i32)
    (select
      (i32.const 1)
      (i32.const 0)
      (i32.rem_u (local.get $a) (i32.const 2))
    )
  )

  (export "test_select" (func $test_select))

  ;;
  ;; test_local_get:
  ;;   $a = 0: expect i32 $b
  ;;   $a = 1: expect i32 31337
  ;;
  (func $test_local_get (param $a i32)
                        (param $b i32)
                        (result i32)
    (local $c i32)
    (local.set $c (i32.const 31337))
    (select
      (local.get $c)
      (local.get $b)
      (local.get $a)
    )
  )

  (export "test_local_get" (func $test_local_get))

  ;;
  ;; test_local_get_f32:
  ;;   expect f32 $a * 2.0
  ;;
  (func $test_local_get_f32 (param $a f32)
                        (result f32)
    (f32.mul (local.get $a) (f32.const 2))
  )

  (export "test_local_get_f32" (func $test_local_get_f32))

  ;;
  ;; test_local_set:
  ;;   expect i32 $a + 22
  ;;
  (func $test_local_set (param $a i32)
                        (result i32)
    (local $b i32)
    (local.set $b (i32.add (local.get $a) (i32.const 22)))
    (local.get $b)
  )

  (export "test_local_set" (func $test_local_set))

  ;;
  ;; test_local_tee:
  ;;   expect i32 $a + 44
  ;;
  (func $test_local_tee (param $a i32)
                        (result i32)
    (local $b i32)
    (local.tee $b (i32.add (local.get $a) (i32.const 44)))
  )

  (export "test_local_tee" (func $test_local_tee))

  ;; test_global_get test value
  (global $test_global_get_val (mut i32) (i32.const 314159))

  ;;
  ;; test_global_get:
  ;;   expect i32 314159
  ;;
  (func $test_global_get (result i32)
    (global.get $test_global_get_val)
  )

  (export "test_global_get" (func $test_global_get))

  ;; test_global_set test value
  (global $test_global_set_val (mut i32) (i32.const 27))

  ;;
  ;; test_global_set:
  ;;   expect i32 42
  ;;
  (func $test_global_set (result i32)
    (global.set $test_global_set_val (i32.const 42))
    (global.get $test_global_set_val)
  )

  (export "test_global_set" (func $test_global_set))

  ;; test memory (used by load and mem funcs)
  (memory $mem (export "mem") 1)

  ;;
  ;; test_i32_load:
  ;;   expect i32 1234
  ;;
  (func $test_i32_load (result i32)
    (i32.store (i32.const 2000) (i32.const 1234))
    (i32.load (i32.const 2000))
  )

  (export "test_i32_load" (func $test_i32_load))

  ;;
  ;; test_i64_load:
  ;;   expect i64 987654321
  ;;
  (func $test_i64_load (result i64)
    (i64.store (i32.const 2000) (i64.const 987654321))
    (i64.load (i32.const 2000))
  )

  (export "test_i64_load" (func $test_i64_load))

  ;;
  ;; test_f32_load:
  ;;   expect f32 3.14159
  ;;
  (func $test_f32_load (result f32)
    (f32.store (i32.const 3000) (f32.const 3.14159))
    (f32.load (i32.const 3000))
  )

  (export "test_f32_load" (func $test_f32_load))

  ;;
  ;; test_f64_load:
  ;;   expect f64 3.14159
  ;;
  (func $test_f64_load (result f64)
    (f64.store (i32.const 4000) (f64.const 3.14159))
    (f64.load (i32.const 4000))
  )

  (export "test_f64_load" (func $test_f64_load))

  ;;
  ;; test_i32_load8_s:
  ;;   expect $a = 127: 127 (signed)
  ;;   expect $a = 128: -1  (signed)
  ;;
  (func $test_i32_load8_s (param $a i32)
                          (result i32)
    (i32.store (i32.const 1000) (local.get $a))
    (i32.load8_s (i32.const 1000))
  )

  (export "test_i32_load8_s" (func $test_i32_load8_s))

  ;;
  ;; test_i32_load8_u:
  ;;   expect $a = 127: 127 (unsigned)
  ;;   expect $a = 128: 128 (unsigned)
  ;;
  (func $test_i32_load8_u (param $a i32)
                          (result i32)
    (i32.store (i32.const 1000) (local.get $a))
    (i32.load8_u (i32.const 1000))
  )

  (export "test_i32_load8_u" (func $test_i32_load8_u))

  ;;
  ;; test_i32_load16_s:
  ;;   expect $a = 32767: 32767 (signed)
  ;;   expect $a = 32768: -1 (signed)
  ;;
  (func $test_i32_load16_s (param $a i32)
                           (result i32)
    (i32.store (i32.const 1000) (local.get $a))
    (i32.load16_s (i32.const 1000))
  )

  (export "test_i32_load16_s" (func $test_i32_load16_s))

  ;;
  ;; test_i32_load16_u:
  ;;   expect $a = 32767: 32767 (signed)
  ;;   expect $a = 32768: 32768 (signed)
  ;;
  (func $test_i32_load16_u (param $a i32)
                           (result i32)
    (i32.store (i32.const 1000) (local.get $a))
    (i32.load16_u (i32.const 1000))
  )

  (export "test_i32_load16_u" (func $test_i32_load16_u))

  ;;
  ;; test_i64_load8_s:
  ;;   expect $a = 127: 127 (signed)
  ;;   expect $a = 128: -1  (signed)
  ;;
  (func $test_i64_load8_s (param $a i64)
                          (result i64)
    (i64.store (i32.const 1000) (local.get $a))
    (i64.load8_s (i32.const 1000))
  )

  (export "test_i64_load8_s" (func $test_i64_load8_s))

  ;;
  ;; test_i64_load8_u:
  ;;   expect $a = 127: 127 (unsigned)
  ;;   expect $a = 128: 128 (unsigned)
  ;;
  (func $test_i64_load8_u (param $a i64)
                          (result i64)
    (i64.store (i32.const 1000) (local.get $a))
    (i64.load8_u (i32.const 1000))
  )

  (export "test_i64_load8_u" (func $test_i64_load8_u))

  ;;
  ;; test_i64_load16_s:
  ;;   expect $a = 32767: 32767 (signed)
  ;;   expect $a = 32768: -1 (signed)
  ;;
  (func $test_i64_load16_s (param $a i64)
                           (result i64)
    (i64.store (i32.const 1000) (local.get $a))
    (i64.load16_s (i32.const 1000))
  )

  (export "test_i64_load16_s" (func $test_i64_load16_s))

  ;;
  ;; test_i64_load16_u:
  ;;   expect $a = 32767: 32767 (signed)
  ;;   expect $a = 32768: 32768 (signed)
  ;;
  (func $test_i64_load16_u (param $a i64)
                           (result i64)
    (i64.store (i32.const 1000) (local.get $a))
    (i64.load16_u (i32.const 1000))
  )

  (export "test_i64_load16_u" (func $test_i64_load16_u))

  ;;
  ;; test_i64_load16_s:
  ;;   expect $a = 2147483647: 2147483647 (signed)
  ;;   expect $a = 2147483648: -1 (signed)
  ;;
  (func $test_i64_load32_s (param $a i64)
                           (result i64)
    (i64.store (i32.const 1000) (local.get $a))
    (i64.load32_s (i32.const 1000))
  )

  (export "test_i64_load32_s" (func $test_i64_load32_s))

  ;;
  ;; test_i64_load32_u:
  ;;   expect $a = 2147483647: 2147483647 (signed)
  ;;   expect $a = 2147483648: 2147483648 (signed)
  ;;
  (func $test_i64_load32_u (param $a i64)
                           (result i64)
    (i64.store (i32.const 1000) (local.get $a))
    (i64.load32_u (i32.const 1000))
  )

  (export "test_i64_load32_u" (func $test_i64_load32_u))

  ;;
  ;; test_i32_store:
  ;;   expect $a
  ;;
  (func $test_i32_store (param $a i32)
                        (result i32)
    (i32.store (i32.const 1000) (local.get $a))
    (i32.load (i32.const 1000))
  )

  (export "test_i32_store" (func $test_i32_store))

  ;;
  ;; test_i64_store:
  ;;   expect $a
  ;;
  (func $test_i64_store (param $a i64)
                        (result i64)
    (i64.store (i32.const 1000) (local.get $a))
    (i64.load (i32.const 1000))
  )

  (export "test_i64_store" (func $test_i64_store))

  ;;
  ;; test_f32_store:
  ;;   expect $a
  ;;
  (func $test_f32_store (param $a f32)
                        (result f32)
    (f32.store (i32.const 2000) (local.get $a))
    (f32.load (i32.const 2000))
  )

  (export "test_f32_store" (func $test_f32_store))

  ;;
  ;; test_f64_store:
  ;;   expect $a
  ;;
  (func $test_f64_store (param $a f64)
                        (result f64)
    (f64.store (i32.const 1000) (local.get $a))
    (f64.load (i32.const 1000))
  )

  (export "test_f64_store" (func $test_f64_store))

  ;;
  ;; test_i32_store8:
  ;;   expect i32 $a & 0xFF
  ;;
  (func $test_i32_store8 (param $a i32)
                        (result i32)
    (i32.store (i32.const 1000) (i32.const 0))
    (i32.store8 (i32.const 1000) (local.get $a))
    (i32.load (i32.const 1000))
  )

  (export "test_i32_store8" (func $test_i32_store8))

  ;;
  ;; test_i32_store16:
  ;;   expect i32 $a & 0xFFFF
  ;;
  (func $test_i32_store16 (param $a i32)
                        (result i32)
    (i32.store (i32.const 1000) (i32.const 0))
    (i32.store16 (i32.const 1000) (local.get $a))
    (i32.load (i32.const 1000))
  )

  (export "test_i32_store16" (func $test_i32_store16))

  ;;
  ;; test_i64_store8:
  ;;   expect i64 $a & 0xFF
  ;;
  (func $test_i64_store8 (param $a i64)
                         (result i64)
    (i64.store (i32.const 1000) (i64.const 0))
    (i64.store8 (i32.const 1000) (local.get $a))
    (i64.load (i32.const 1000))
  )

  (export "test_i64_store8" (func $test_i64_store8))

  ;;
  ;; test_i64_store16:
  ;;   expect i64 $a & 0xFFFF
  ;;
  (func $test_i64_store16 (param $a i64)
                          (result i64)
    (i64.store (i32.const 1000) (i64.const 0))
    (i64.store16 (i32.const 1000) (local.get $a))
    (i64.load (i32.const 1000))
  )

  (export "test_i64_store16" (func $test_i64_store16))

  ;;
  ;; test_i64_store32:
  ;;   expect i64 $a & 0xFFFFFFFF
  ;;
  (func $test_i64_store32 (param $a i64)
                          (result i64)
    (i64.store (i32.const 1000) (i64.const 0))
    (i64.store32 (i32.const 1000) (local.get $a))
    (i64.load (i32.const 1000))
  )

  (export "test_i64_store32" (func $test_i64_store32))

  ;;
  ;; test_memory_size:
  ;;   expect i32 1
  ;;
  (func $test_memory_size (result i32)
    (memory.size)
  )

  (export "test_memory_size" (func $test_memory_size))

  ;;
  ;; test_memory_grow:
  ;;   expect i32 1
  ;;
  (func $test_memory_grow (result i32)
    (memory.grow (i32.const 1))
  )

  (export "test_memory_grow" (func $test_memory_grow))

  ;;
  ;; test_i32_eqz:
  ;;   expect $a = 0: i32 1
  ;;   expect $a = 1: i32 0
  ;;
  (func $test_i32_eqz (param $a i32)
                      (result i32)
    (i32.eqz (local.get $a))
  )

  (export "test_i32_eqz" (func $test_i32_eqz))

  ;;
  ;; test_i32_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $test_i32_eq (param $a i32)
                     (param $b i32)
                     (result i32)
    (i32.eq (local.get $a) (local.get $b))
  )

  (export "test_i32_eq" (func $test_i32_eq))

  ;;
  ;; test_i32_eq:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $test_i32_ne (param $a i32)
                     (param $b i32)
                     (result i32)
    (i32.ne (local.get $a) (local.get $b))
  )

  (export "test_i32_ne" (func $test_i32_ne))

  ;;
  ;; test_i32_lt_s:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $test_i32_lt_s (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.lt_s (local.get $a) (local.get $b))
  )

  (export "test_i32_lt_s" (func $test_i32_lt_s))

  ;;
  ;; test_i32_lt_u:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $test_i32_lt_u (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.lt_u (local.get $a) (local.get $b))
  )

  (export "test_i32_lt_u" (func $test_i32_lt_u))

  ;;
  ;; test_i32_gt_s:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $test_i32_gt_s (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.gt_s (local.get $a) (local.get $b))
  )

  (export "test_i32_gt_s" (func $test_i32_gt_s))

  ;;
  ;; test_i32_gt_u:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $test_i32_gt_u (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.gt_u (local.get $a) (local.get $b))
  )

  (export "test_i32_gt_u" (func $test_i32_gt_u))

  ;;
  ;; test_i32_le_s:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $test_i32_le_s (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.le_s (local.get $a) (local.get $b))
  )

  (export "test_i32_le_s" (func $test_i32_le_s))

  ;;
  ;; test_i32_le_u:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $test_i32_le_u (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.le_u (local.get $a) (local.get $b))
  )

  (export "test_i32_le_u" (func $test_i32_le_u))

  ;;
  ;; test_i32_ge_s:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $test_i32_ge_s (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.ge_s (local.get $a) (local.get $b))
  )

  (export "test_i32_ge_s" (func $test_i32_ge_s))

  ;;
  ;; test_i32_ge_u:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $test_i32_ge_u (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.ge_u (local.get $a) (local.get $b))
  )

  (export "test_i32_ge_u" (func $test_i32_ge_u))

  ;;
  ;; test_i64_eqz:
  ;;   expect $a = 0: i32 1
  ;;   expect $a = 1: i32 0
  ;;
  (func $test_i64_eqz (param $a i64)
                      (result i32)
    (i64.eqz (local.get $a))
  )

  (export "test_i64_eqz" (func $test_i64_eqz))

  ;;
  ;; test_i64_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $test_i64_eq (param $a i64)
                     (param $b i64)
                     (result i32)
    (i64.eq (local.get $a) (local.get $b))
  )

  (export "test_i64_eq" (func $test_i64_eq))

  ;;
  ;; test_i64_eq:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $test_i64_ne (param $a i64)
                     (param $b i64)
                     (result i32)
    (i64.ne (local.get $a) (local.get $b))
  )

  (export "test_i64_ne" (func $test_i64_ne))

  ;;
  ;; test_i64_lt_s:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $test_i64_lt_s (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.lt_s (local.get $a) (local.get $b))
  )

  (export "test_i64_lt_s" (func $test_i64_lt_s))

  ;;
  ;; test_i64_lt_u:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $test_i64_lt_u (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.lt_u (local.get $a) (local.get $b))
  )

  (export "test_i64_lt_u" (func $test_i64_lt_u))

  ;;
  ;; test_i64_gt_s:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $test_i64_gt_s (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.gt_s (local.get $a) (local.get $b))
  )

  (export "test_i64_gt_s" (func $test_i64_gt_s))

  ;;
  ;; test_i64_gt_u:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $test_i64_gt_u (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.gt_u (local.get $a) (local.get $b))
  )

  (export "test_i64_gt_u" (func $test_i64_gt_u))

  ;;
  ;; test_i64_le_s:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $test_i64_le_s (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.le_s (local.get $a) (local.get $b))
  )

  (export "test_i64_le_s" (func $test_i64_le_s))

  ;;
  ;; test_i64_le_u:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $test_i64_le_u (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.le_u (local.get $a) (local.get $b))
  )

  (export "test_i64_le_u" (func $test_i64_le_u))

  ;;
  ;; test_i64_ge_s:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $test_i64_ge_s (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.ge_s (local.get $a) (local.get $b))
  )

  (export "test_i64_ge_s" (func $test_i64_ge_s))

  ;;
  ;; test_i64_ge_u:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $test_i64_ge_u (param $a i64)
                       (param $b i64)
                       (result i32)
    (i64.ge_u (local.get $a) (local.get $b))
  )

  (export "test_i64_ge_u" (func $test_i64_ge_u))

  ;;
  ;; test_f32_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $test_f32_eq (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.eq (local.get $a) (local.get $b))
  )

  (export "test_f32_eq" (func $test_f32_eq))

  ;;
  ;; test_f32_ne:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $test_f32_ne (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.ne (local.get $a) (local.get $b))
  )

  (export "test_f32_ne" (func $test_f32_ne))

  ;;
  ;; test_f32_lt:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $test_f32_lt (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.lt (local.get $a) (local.get $b))
  )

  (export "test_f32_lt" (func $test_f32_lt))

  ;;
  ;; test_f32_gt:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $test_f32_gt (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.gt (local.get $a) (local.get $b))
  )

  (export "test_f32_gt" (func $test_f32_gt))

  ;;
  ;; test_f32_le:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $test_f32_le (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.le (local.get $a) (local.get $b))
  )

  (export "test_f32_le" (func $test_f32_le))

  ;;
  ;; test_f32_ge:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $test_f32_ge (param $a f32)
                     (param $b f32)
                     (result i32)
    (f32.ge (local.get $a) (local.get $b))
  )

  (export "test_f32_ge" (func $test_f32_ge))

  ;;
  ;; test_f64_eq:
  ;;   expect $a == $b: i32 1
  ;;   expect $a != $b: i32 0
  ;;
  (func $test_f64_eq (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.eq (local.get $a) (local.get $b))
  )

  (export "test_f64_eq" (func $test_f64_eq))

  ;;
  ;; test_f64_ne:
  ;;   expect $a != $b: i32 1
  ;;   expect $a == $b: i32 0
  ;;
  (func $test_f64_ne (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.ne (local.get $a) (local.get $b))
  )

  (export "test_f64_ne" (func $test_f64_ne))

  ;;
  ;; test_f64_lt:
  ;;   expect $a < $b: i32 1
  ;;   expect $a >= $b: i32 0
  ;;
  (func $test_f64_lt (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.lt (local.get $a) (local.get $b))
  )

  (export "test_f64_lt" (func $test_f64_lt))

  ;;
  ;; test_f64_gt:
  ;;   expect $a > $b: i32 1
  ;;   expect $a <= $b: i32 0
  ;;
  (func $test_f64_gt (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.gt (local.get $a) (local.get $b))
  )

  (export "test_f64_gt" (func $test_f64_gt))

  ;;
  ;; test_f64_le:
  ;;   expect $a <= $b: i32 1
  ;;   expect $a > $b: i32 0
  ;;
  (func $test_f64_le (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.le (local.get $a) (local.get $b))
  )

  (export "test_f64_le" (func $test_f64_le))

  ;;
  ;; test_f64_ge:
  ;;   expect $a >= $b: i32 1
  ;;   expect $a < $b: i32 0
  ;;
  (func $test_f64_ge (param $a f64)
                     (param $b f64)
                     (result i32)
    (f64.ge (local.get $a) (local.get $b))
  )

  (export "test_f64_ge" (func $test_f64_ge))

  ;;
  ;; test_i32_clz:
  ;;   0x00: expect i32 32
  ;;   0x0f: expect i32 24
  ;;   0x0000f00f: expect i32 16
  ;;
  (func $test_i32_clz (param $a i32)
                      (result i32)
    (i32.clz (local.get $a))
  )

  (export "test_i32_clz" (func $test_i32_clz))

  ;;
  ;; test_i32_ctz:
  ;;   0x00: expect i32 32
  ;;   0xf0: expect i32 4
  ;;   0xf00f0000: expect i32 16
  ;;
  (func $test_i32_ctz (param $a i32)
                      (result i32)
    (i32.ctz (local.get $a))
  )

  (export "test_i32_ctz" (func $test_i32_ctz))

  ;;
  ;; test_i32_popcnt:
  ;;   0x00: expect i32 0
  ;;   0xf0: expect i32 4
  ;;   0xf0f0f0f0: expect i32 16
  ;;
  (func $test_i32_popcnt (param $a i32)
                      (result i32)
    (i32.popcnt (local.get $a))
  )

  (export "test_i32_popcnt" (func $test_i32_popcnt))

  ;;
  ;; test_i32_add:
  ;;   expect i32 $a + $b
  ;;
  (func $test_i32_add (param $a i32)
                      (param $b i32)
                      (result i32)
    (i32.add (local.get $a) (local.get $b))
  )

  (export "test_i32_add" (func $test_i32_add))

  ;;
  ;; test_i32_sub:
  ;;   expect i32 $a - $b
  ;;
  (func $test_i32_sub (param $a i32)
                      (param $b i32)
                      (result i32)
    (i32.sub (local.get $a) (local.get $b))
  )

  (export "test_i32_sub" (func $test_i32_sub))

  ;;
  ;; test_i32_mul:
  ;;   expect i32 $a * $b
  ;;
  (func $test_i32_mul (param $a i32)
                      (param $b i32)
                      (result i32)
    (i32.mul (local.get $a) (local.get $b))
  )

  (export "test_i32_mul" (func $test_i32_mul))

  ;;
  ;; test_i32_div_s:
  ;;   expect i32 $a / $b (signed)
  ;;
  (func $test_i32_div_s (param $a i32)
                        (param $b i32)
                        (result i32)
    (i32.div_s (local.get $a) (local.get $b))
  )

  (export "test_i32_div_s" (func $test_i32_div_s))

  ;;
  ;; test_i32_div_u:
  ;;   expect i32 $a / $b (unsigned)
  ;;
  (func $test_i32_div_u (param $a i32)
                        (param $b i32)
                        (result i32)
    (i32.div_u (local.get $a) (local.get $b))
  )

  (export "test_i32_div_u" (func $test_i32_div_u))

  ;;
  ;; test_i32_rem_s:
  ;;   expect i32 $a % $b (signed)
  ;;
  (func $test_i32_rem_s (param $a i32)
                        (param $b i32)
                        (result i32)
    (i32.rem_s (local.get $a) (local.get $b))
  )

  (export "test_i32_rem_s" (func $test_i32_rem_s))

  ;;
  ;; test_i32_rem_u:
  ;;   expect i32 $a % $b (unsigned)
  ;;
  (func $test_i32_rem_u (param $a i32)
                        (param $b i32)
                        (result i32)
    (i32.rem_u (local.get $a) (local.get $b))
  )

  (export "test_i32_rem_u" (func $test_i32_rem_u))

  ;;
  ;; test_i32_and:
  ;;   expect i32 $a & $b
  ;;
  (func $test_i32_and (param $a i32)
                      (param $b i32)
                      (result i32)
    (i32.and (local.get $a) (local.get $b))
  )

  (export "test_i32_and" (func $test_i32_and))

  ;;
  ;; test_i32_or:
  ;;   expect i32 $a | $b
  ;;
  (func $test_i32_or (param $a i32)
                     (param $b i32)
                     (result i32)
    (i32.or (local.get $a) (local.get $b))
  )

  (export "test_i32_or" (func $test_i32_or))

  ;;
  ;; test_i32_xor:
  ;;   expect i32 $a ^ $b
  ;;
  (func $test_i32_xor (param $a i32)
                      (param $b i32)
                      (result i32)
    (i32.xor (local.get $a) (local.get $b))
  )

  (export "test_i32_xor" (func $test_i32_xor))

  ;;
  ;; test_i32_shl:
  ;;   expect i32 $a << $b
  ;;
  (func $test_i32_shl (param $a i32)
                      (param $b i32)
                      (result i32)
    (i32.shl (local.get $a) (local.get $b))
  )

  (export "test_i32_shl" (func $test_i32_shl))

  ;;
  ;; test_i32_shr_s:
  ;;   expect i32 $a >> $b (signed)
  ;;
  (func $test_i32_shr_s (param $a i32)
                        (param $b i32)
                        (result i32)
    (i32.shr_s (local.get $a) (local.get $b))
  )

  (export "test_i32_shr_s" (func $test_i32_shr_s))

  ;;
  ;; test_i32_shr_u:
  ;;   expect i32 $a >> $b (unsigned)
  ;;
  (func $test_i32_shr_u (param $a i32)
                        (param $b i32)
                        (result i32)
    (i32.shr_u (local.get $a) (local.get $b))
  )

  (export "test_i32_shr_u" (func $test_i32_shr_u))

  ;;
  ;; test_i32_rotl:
  ;;   expect i32 $a <<< $b
  ;;
  (func $test_i32_rotl (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.rotl (local.get $a) (local.get $b))
  )

  (export "test_i32_rotl" (func $test_i32_rotl))

  ;;
  ;; test_i32_rotr:
  ;;   expect i32 $a >>> $b
  ;;
  (func $test_i32_rotr (param $a i32)
                       (param $b i32)
                       (result i32)
    (i32.rotr (local.get $a) (local.get $b))
  )

  (export "test_i32_rotr" (func $test_i32_rotr))

  ;;
  ;; test_i64_clz:
  ;;   0x00: expect i64 32
  ;;   0x0f: expect i64 24
  ;;   0x0000f00f: expect i64 16
  ;;
  (func $test_i64_clz (param $a i64)
                      (result i64)
    (i64.clz (local.get $a))
  )

  (export "test_i64_clz" (func $test_i64_clz))

  ;;
  ;; test_i64_ctz:
  ;;   0x00: expect i64 32
  ;;   0xf0: expect i64 4
  ;;   0xf00f0000: expect i64 16
  ;;
  (func $test_i64_ctz (param $a i64)
                      (result i64)
    (i64.ctz (local.get $a))
  )

  (export "test_i64_ctz" (func $test_i64_ctz))

  ;;
  ;; test_i64_popcnt:
  ;;   0x00: expect i64 0
  ;;   0xf0: expect i64 4
  ;;   0xf0f0f0f0: expect i64 16
  ;;
  (func $test_i64_popcnt (param $a i64)
                      (result i64)
    (i64.popcnt (local.get $a))
  )

  (export "test_i64_popcnt" (func $test_i64_popcnt))

  ;;
  ;; test_i64_add:
  ;;   expect i64 $a + $b
  ;;
  (func $test_i64_add (param $a i64)
                      (param $b i64)
                      (result i64)
    (i64.add (local.get $a) (local.get $b))
  )

  (export "test_i64_add" (func $test_i64_add))

  ;;
  ;; test_i64_sub:
  ;;   expect i64 $a - $b
  ;;
  (func $test_i64_sub (param $a i64)
                      (param $b i64)
                      (result i64)
    (i64.sub (local.get $a) (local.get $b))
  )

  (export "test_i64_sub" (func $test_i64_sub))

  ;;
  ;; test_i64_mul:
  ;;   expect i64 $a * $b
  ;;
  (func $test_i64_mul (param $a i64)
                      (param $b i64)
                      (result i64)
    (i64.mul (local.get $a) (local.get $b))
  )

  (export "test_i64_mul" (func $test_i64_mul))

  ;;
  ;; test_i64_div_s:
  ;;   expect i64 $a / $b (signed)
  ;;
  (func $test_i64_div_s (param $a i64)
                        (param $b i64)
                        (result i64)
    (i64.div_s (local.get $a) (local.get $b))
  )

  (export "test_i64_div_s" (func $test_i64_div_s))

  ;;
  ;; test_i64_div_u:
  ;;   expect i64 $a / $b (unsigned)
  ;;
  (func $test_i64_div_u (param $a i64)
                        (param $b i64)
                        (result i64)
    (i64.div_u (local.get $a) (local.get $b))
  )

  (export "test_i64_div_u" (func $test_i64_div_u))

  ;;
  ;; test_i64_rem_s:
  ;;   expect i64 $a % $b (signed)
  ;;
  (func $test_i64_rem_s (param $a i64)
                        (param $b i64)
                        (result i64)
    (i64.rem_s (local.get $a) (local.get $b))
  )

  (export "test_i64_rem_s" (func $test_i64_rem_s))

  ;;
  ;; test_i64_rem_u:
  ;;   expect i64 $a % $b (unsigned)
  ;;
  (func $test_i64_rem_u (param $a i64)
                        (param $b i64)
                        (result i64)
    (i64.rem_u (local.get $a) (local.get $b))
  )

  (export "test_i64_rem_u" (func $test_i64_rem_u))

  ;;
  ;; test_i64_and:
  ;;   expect i64 $a & $b
  ;;
  (func $test_i64_and (param $a i64)
                      (param $b i64)
                      (result i64)
    (i64.and (local.get $a) (local.get $b))
  )

  (export "test_i64_and" (func $test_i64_and))

  ;;
  ;; test_i64_or:
  ;;   expect i64 $a | $b
  ;;
  (func $test_i64_or (param $a i64)
                     (param $b i64)
                     (result i64)
    (i64.or (local.get $a) (local.get $b))
  )

  (export "test_i64_or" (func $test_i64_or))

  ;;
  ;; test_i64_xor:
  ;;   expect i64 $a ^ $b
  ;;
  (func $test_i64_xor (param $a i64)
                      (param $b i64)
                      (result i64)
    (i64.xor (local.get $a) (local.get $b))
  )

  (export "test_i64_xor" (func $test_i64_xor))

  ;;
  ;; test_i64_shl:
  ;;   expect i64 $a << $b
  ;;
  (func $test_i64_shl (param $a i64)
                      (param $b i64)
                      (result i64)
    (i64.shl (local.get $a) (local.get $b))
  )

  (export "test_i64_shl" (func $test_i64_shl))

  ;;
  ;; test_i64_shr_s:
  ;;   expect i64 $a >> $b (signed)
  ;;
  (func $test_i64_shr_s (param $a i64)
                        (param $b i64)
                        (result i64)
    (i64.shr_s (local.get $a) (local.get $b))
  )

  (export "test_i64_shr_s" (func $test_i64_shr_s))

  ;;
  ;; test_i64_shr_u:
  ;;   expect i64 $a >> $b (unsigned)
  ;;
  (func $test_i64_shr_u (param $a i64)
                        (param $b i64)
                        (result i64)
    (i64.shr_u (local.get $a) (local.get $b))
  )

  (export "test_i64_shr_u" (func $test_i64_shr_u))

  ;;
  ;; test_i64_rotl:
  ;;   expect i64 $a <<< $b
  ;;
  (func $test_i64_rotl (param $a i64)
                       (param $b i64)
                       (result i64)
    (i64.rotl (local.get $a) (local.get $b))
  )

  (export "test_i64_rotl" (func $test_i64_rotl))

  ;;
  ;; test_i64_rotr:
  ;;   expect i64 $a >>> $b
  ;;
  (func $test_i64_rotr (param $a i64)
                       (param $b i64)
                       (result i64)
    (i64.rotr (local.get $a) (local.get $b))
  )

  (export "test_i64_rotr" (func $test_i64_rotr))

  ;;
  ;; test_f32_abs:
  ;;   expect f32 $a
  ;;
  (func $test_f32_abs (param $a f32)
                      (result f32)
    (f32.abs (local.get $a))
  )

  (export "test_f32_abs" (func $test_f32_abs))

  ;;
  ;; test_f32_neg:
  ;;   expect f32 -$a
  ;;
  (func $test_f32_neg (param $a f32)
                      (result f32)
    (f32.neg (local.get $a))
  )

  (export "test_f32_neg" (func $test_f32_neg))

  ;;
  ;; test_f32_ceil:
  ;;   expect f32 ceil($a)
  ;;
  (func $test_f32_ceil (param $a f32)
                       (result f32)
    (f32.ceil (local.get $a))
  )

  (export "test_f32_ceil" (func $test_f32_ceil))

  ;;
  ;; test_f32_floor:
  ;;   expect f32 floor($a)
  ;;
  (func $test_f32_floor (param $a f32)
                        (result f32)
    (f32.floor (local.get $a))
  )

  (export "test_f32_floor" (func $test_f32_floor))

  ;;
  ;; test_f32_trunc:
  ;;   expect f32 trunc($a)
  ;;
  (func $test_f32_trunc (param $a f32)
                        (result f32)
    (f32.trunc (local.get $a))
  )

  (export "test_f32_trunc" (func $test_f32_trunc))

  ;;
  ;; test_f32_nearest:
  ;;   expect f32 nearest($a)
  ;;
  (func $test_f32_nearest (param $a f32)
                          (result f32)
    (f32.nearest (local.get $a))
  )

  (export "test_f32_nearest" (func $test_f32_nearest))

  ;;
  ;; test_f32_sqrt:
  ;;   expect f32 sqrt($a)
  ;;
  (func $test_f32_sqrt (param $a f32)
                       (result f32)
    (f32.sqrt (local.get $a))
  )

  (export "test_f32_sqrt" (func $test_f32_sqrt))

  ;;
  ;; test_f32_add:
  ;;   expect f32 $a + $b
  ;;
  (func $test_f32_add (param $a f32)
                      (param $b f32)
                      (result f32)
    (f32.add (local.get $a) (local.get $b))
  )

  (export "test_f32_add" (func $test_f32_add))

  ;;
  ;; test_f32_sub:
  ;;   expect f32 $a - $b
  ;;
  (func $test_f32_sub (param $a f32)
                      (param $b f32)
                      (result f32)
    (f32.sub (local.get $a) (local.get $b))
  )

  (export "test_f32_sub" (func $test_f32_sub))

  ;;
  ;; test_f32_mul:
  ;;   expect f32 $a * $b
  ;;
  (func $test_f32_mul (param $a f32)
                      (param $b f32)
                      (result f32)
    (f32.mul (local.get $a) (local.get $b))
  )

  (export "test_f32_mul" (func $test_f32_mul))

  ;;
  ;; test_f32_div:
  ;;   expect f32 $a / $b
  ;;
  (func $test_f32_div (param $a f32)
                      (param $b f32)
                      (result f32)
    (f32.div (local.get $a) (local.get $b))
  )

  (export "test_f32_div" (func $test_f32_div))

  ;;
  ;; test_f32_min:
  ;;   expect f32 min($a, $b)
  ;;
  (func $test_f32_min (param $a f32)
                      (param $b f32)
                      (result f32)
    (f32.min (local.get $a) (local.get $b))
  )

  (export "test_f32_min" (func $test_f32_min))

  ;;
  ;; test_f32_max:
  ;;   expect f32 max($a, $b)
  ;;
  (func $test_f32_max (param $a f32)
                      (param $b f32)
                      (result f32)
    (f32.max (local.get $a) (local.get $b))
  )

  (export "test_f32_max" (func $test_f32_max))

  ;;
  ;; test_f32_copysign:
  ;;   expect f32 copysign($a, $b)
  ;;
  (func $test_f32_copysign (param $a f32)
                      (param $b f32)
                      (result f32)
    (f32.copysign (local.get $a) (local.get $b))
  )

  (export "test_f32_copysign" (func $test_f32_copysign))

  ;;
  ;; test_f64_abs:
  ;;   expect f64 $a
  ;;
  (func $test_f64_abs (param $a f64)
                      (result f64)
    (f64.abs (local.get $a))
  )

  (export "test_f64_abs" (func $test_f64_abs))

  ;;
  ;; test_f64_neg:
  ;;   expect f64 -$a
  ;;
  (func $test_f64_neg (param $a f64)
                      (result f64)
    (f64.neg (local.get $a))
  )

  (export "test_f64_neg" (func $test_f64_neg))

  ;;
  ;; test_f64_ceil:
  ;;   expect f64 ceil($a)
  ;;
  (func $test_f64_ceil (param $a f64)
                       (result f64)
    (f64.ceil (local.get $a))
  )

  (export "test_f64_ceil" (func $test_f64_ceil))

  ;;
  ;; test_f64_floor:
  ;;   expect f64 floor($a)
  ;;
  (func $test_f64_floor (param $a f64)
                        (result f64)
    (f64.floor (local.get $a))
  )

  (export "test_f64_floor" (func $test_f64_floor))

  ;;
  ;; test_f64_trunc:
  ;;   expect f64 trunc($a)
  ;;
  (func $test_f64_trunc (param $a f64)
                        (result f64)
    (f64.trunc (local.get $a))
  )

  (export "test_f64_trunc" (func $test_f64_trunc))

  ;;
  ;; test_f64_nearest:
  ;;   expect f64 nearest($a)
  ;;
  (func $test_f64_nearest (param $a f64)
                          (result f64)
    (f64.nearest (local.get $a))
  )

  (export "test_f64_nearest" (func $test_f64_nearest))

  ;;
  ;; test_f64_sqrt:
  ;;   expect f64 sqrt($a)
  ;;
  (func $test_f64_sqrt (param $a f64)
                       (result f64)
    (f64.sqrt (local.get $a))
  )

  (export "test_f64_sqrt" (func $test_f64_sqrt))

  ;;
  ;; test_f64_add:
  ;;   expect f64 $a + $b
  ;;
  (func $test_f64_add (param $a f64)
                      (param $b f64)
                      (result f64)
    (f64.add (local.get $a) (local.get $b))
  )

  (export "test_f64_add" (func $test_f64_add))

  ;;
  ;; test_f64_sub:
  ;;   expect f64 $a - $b
  ;;
  (func $test_f64_sub (param $a f64)
                      (param $b f64)
                      (result f64)
    (f64.sub (local.get $a) (local.get $b))
  )

  (export "test_f64_sub" (func $test_f64_sub))

  ;;
  ;; test_f64_mul:
  ;;   expect f64 $a * $b
  ;;
  (func $test_f64_mul (param $a f64)
                      (param $b f64)
                      (result f64)
    (f64.mul (local.get $a) (local.get $b))
  )

  (export "test_f64_mul" (func $test_f64_mul))

  ;;
  ;; test_f64_div:
  ;;   expect f64 $a / $b
  ;;
  (func $test_f64_div (param $a f64)
                      (param $b f64)
                      (result f64)
    (f64.div (local.get $a) (local.get $b))
  )

  (export "test_f64_div" (func $test_f64_div))

  ;;
  ;; test_f64_min:
  ;;   expect f64 min($a, $b)
  ;;
  (func $test_f64_min (param $a f64)
                      (param $b f64)
                      (result f64)
    (f64.min (local.get $a) (local.get $b))
  )

  (export "test_f64_min" (func $test_f64_min))

  ;;
  ;; test_f64_max:
  ;;   expect f64 max($a, $b)
  ;;
  (func $test_f64_max (param $a f64)
                      (param $b f64)
                      (result f64)
    (f64.max (local.get $a) (local.get $b))
  )

  (export "test_f64_max" (func $test_f64_max))

  ;;
  ;; test_f64_copysign:
  ;;   expect f64 copysign($a, $b)
  ;;
  (func $test_f64_copysign (param $a f64)
                      (param $b f64)
                      (result f64)
    (f64.copysign (local.get $a) (local.get $b))
  )

  (export "test_f64_copysign" (func $test_f64_copysign))

  ;;
  ;; test_i32_wrap_i64:
  ;;   expect i32 wrap($a)
  ;;
  (func $test_i32_wrap_i64 (param $a i64)
                           (result i32)
    (i32.wrap_i64 (local.get $a))
  )

  (export "test_i32_wrap_i64" (func $test_i32_wrap_i64))

  ;;
  ;; test_i32_trunc_f32_s:
  ;;   expect i32 trunc($a)
  ;;
  (func $test_i32_trunc_f32_s (param $a f32)
                              (result i32)
    (i32.trunc_f32_s (local.get $a))
  )

  (export "test_i32_trunc_f32_s" (func $test_i32_trunc_f32_s))

  ;;
  ;; test_i32_trunc_f32_u:
  ;;   expect i32 trunc($a)
  ;;
  (func $test_i32_trunc_f32_u (param $a f32)
                              (result i32)
    (i32.trunc_f32_u (local.get $a))
  )

  (export "test_i32_trunc_f32_u" (func $test_i32_trunc_f32_u))

  ;;
  ;; test_i32_trunc_f64_s:
  ;;   expect i32 trunc($a)
  ;;
  (func $test_i32_trunc_f64_s (param $a f64)
                              (result i32)
    (i32.trunc_f64_s (local.get $a))
  )

  (export "test_i32_trunc_f64_s" (func $test_i32_trunc_f64_s))

  ;;
  ;; test_i32_trunc_f64_u:
  ;;   expect i32 trunc($a)
  ;;
  (func $test_i32_trunc_f64_u (param $a f64)
                              (result i32)
    (i32.trunc_f64_u (local.get $a))
  )

  (export "test_i32_trunc_f64_u" (func $test_i32_trunc_f64_u))

  ;;
  ;; test_i64_extend_i32_s:
  ;;   expect i64 extend_i32_s($a)
  ;;
  (func $test_i64_extend_i32_s (param $a i32)
                               (result i64)
    (i64.extend_i32_s (local.get $a))
  )

  (export "test_i64_extend_i32_s" (func $test_i64_extend_i32_s))

  ;;
  ;; test_i64_extend_i32_u:
  ;;   expect i64 extend_i32_u($a)
  ;;
  (func $test_i64_extend_i32_u (param $a i32)
                               (result i64)
    (i64.extend_i32_u (local.get $a))
  )

  (export "test_i64_extend_i32_u" (func $test_i64_extend_i32_u))

  ;;
  ;; test_i64_trunc_f32_s:
  ;;   expect i64 trunc($a)
  ;;
  (func $test_i64_trunc_f32_s (param $a f32)
                              (result i64)
    (i64.trunc_f32_s (local.get $a))
  )

  (export "test_i64_trunc_f32_s" (func $test_i64_trunc_f32_s))

  ;;
  ;; test_i64_trunc_f32_u:
  ;;   expect i64 trunc($a)
  ;;
  (func $test_i64_trunc_f32_u (param $a f32)
                              (result i64)
    (i64.trunc_f32_u (local.get $a))
  )

  (export "test_i64_trunc_f32_u" (func $test_i64_trunc_f32_u))

  ;;
  ;; test_i64_trunc_f64_s:
  ;;   expect i64 trunc($a)
  ;;
  (func $test_i64_trunc_f64_s (param $a f64)
                              (result i64)
    (i64.trunc_f64_s (local.get $a))
  )

  (export "test_i64_trunc_f64_s" (func $test_i64_trunc_f64_s))

  ;;
  ;; test_i64_trunc_f64_u:
  ;;   expect i64 trunc($a)
  ;;
  (func $test_i64_trunc_f64_u (param $a f64)
                              (result i64)
    (i64.trunc_f64_u (local.get $a))
  )

  (export "test_i64_trunc_f64_u" (func $test_i64_trunc_f64_u))

  ;;
  ;; test_f32_convert_i32_s:
  ;;   expect f32 convert_i32_s($a)
  ;;
  (func $test_f32_convert_i32_s (param $a i32)
                                (result f32)
    (f32.convert_i32_s (local.get $a))
  )

  (export "test_f32_convert_i32_s" (func $test_f32_convert_i32_s))

  ;;
  ;; test_f32_convert_i32_u:
  ;;   expect f32 convert_i32_u($a)
  ;;
  (func $test_f32_convert_i32_u (param $a i32)
                                (result f32)
    (f32.convert_i32_u (local.get $a))
  )

  (export "test_f32_convert_i32_u" (func $test_f32_convert_i32_u))

  ;;
  ;; test_f32_convert_i64_s:
  ;;   expect f32 convert_i64_s($a)
  ;;
  (func $test_f32_convert_i64_s (param $a i64)
                                (result f32)
    (f32.convert_i64_s (local.get $a))
  )

  (export "test_f32_convert_i64_s" (func $test_f32_convert_i64_s))

  ;;
  ;; test_f32_convert_i64_u:
  ;;   expect f32 convert_i64_u($a)
  ;;
  (func $test_f32_convert_i64_u (param $a i64)
                                (result f32)
    (f32.convert_i64_u (local.get $a))
  )

  (export "test_f32_convert_i64_u" (func $test_f32_convert_i64_u))

  ;;
  ;; test_f32_demote_f64:
  ;;   expect f32 demote_f64($a)
  ;;
  (func $test_f32_demote_f64 (param $a f64)
                              (result f32)
    (f32.demote_f64 (local.get $a))
  )

  (export "test_f32_demote_f64" (func $test_f32_demote_f64))

  ;;
  ;; test_f64_convert_i32_s:
  ;;   expect f64 convert_i32_s($a)
  ;;
  (func $test_f64_convert_i32_s (param $a i32)
                                (result f64)
    (f64.convert_i32_s (local.get $a))
  )

  (export "test_f64_convert_i32_s" (func $test_f64_convert_i32_s))

  ;;
  ;; test_f64_convert_i32_u:
  ;;   expect f64 convert_i32_u($a)
  ;;
  (func $test_f64_convert_i32_u (param $a i32)
                                (result f64)
    (f64.convert_i32_u (local.get $a))
  )

  (export "test_f64_convert_i32_u" (func $test_f64_convert_i32_u))

  ;;
  ;; test_f64_convert_i64_s:
  ;;   expect f64 convert_i64_s($a)
  ;;
  (func $test_f64_convert_i64_s (param $a i64)
                                (result f64)
    (f64.convert_i64_s (local.get $a))
  )

  (export "test_f64_convert_i64_s" (func $test_f64_convert_i64_s))

  ;;
  ;; test_f64_convert_i64_u:
  ;;   expect f64 convert_i64_u($a)
  ;;
  (func $test_f64_convert_i64_u (param $a i64)
                                (result f64)
    (f64.convert_i64_u (local.get $a))
  )

  (export "test_f64_convert_i64_u" (func $test_f64_convert_i64_u))

  ;;
  ;; test_f64_promote_f32:
  ;;   expect f64 promote_f32($a)
  ;;
  (func $test_f64_promote_f32 (param $a f32)
                              (result f64)
    (f64.promote_f32 (local.get $a))
  )

  (export "test_f64_promote_f32" (func $test_f64_promote_f32))

  ;;
  ;; test_i32_reinterpret_f32:
  ;;   expect i32 reinterpret($a)
  ;;
  (func $test_i32_reinterpret_f32 (param $a f32)
                                  (result i32)
    (i32.reinterpret_f32 (local.get $a))
  )

  (export "test_i32_reinterpret_f32" (func $test_i32_reinterpret_f32))

  ;;
  ;; test_i64_reinterpret_f64:
  ;;   expect i64 reinterpret($a)
  ;;
  (func $test_i64_reinterpret_f64 (param $a f64)
                                  (result i64)
    (i64.reinterpret_f64 (local.get $a))
  )

  (export "test_i64_reinterpret_f64" (func $test_i64_reinterpret_f64))

  ;;
  ;; test_f32_reinterpret_i32:
  ;;   expect f32 reinterpret($a)
  ;;
  (func $test_f32_reinterpret_i32 (param $a i32)
                                  (result f32)
    (f32.reinterpret_i32 (local.get $a))
  )

  (export "test_f32_reinterpret_i32" (func $test_f32_reinterpret_i32))

  ;;
  ;; test_f64_reinterpret_i64:
  ;;   expect f64 reinterpret($a)
  ;;
  (func $test_f64_reinterpret_i64 (param $a i64)
                                  (result f64)
    (f64.reinterpret_i64 (local.get $a))
  )

  (export "test_f64_reinterpret_i64" (func $test_f64_reinterpret_i64))

  ;;
  ;; test_i32_extend8_s:
  ;;   expect i32 extend8_s($a)
  ;;
  (func $test_i32_extend8_s (param $a i32)
                            (result i32)
    (i32.extend8_s (local.get $a))
  )

  (export "test_i32_extend8_s" (func $test_i32_extend8_s))

  ;;
  ;; test_i32_extend16_s:
  ;;   expect i32 extend16_s($a)
  ;;
  (func $test_i32_extend16_s (param $a i32)
                             (result i32)
    (i32.extend16_s (local.get $a))
  )

  (export "test_i32_extend16_s" (func $test_i32_extend16_s))

  ;;
  ;; test_i64_extend8_s:
  ;;   expect i64 extend8_s($a)
  ;;
  (func $test_i64_extend8_s (param $a i64)
                            (result i64)
    (i64.extend8_s (local.get $a))
  )

  (export "test_i64_extend8_s" (func $test_i64_extend8_s))

  ;;
  ;; test_i64_extend16_s:
  ;;   expect i64 extend16_s($a)
  ;;
  (func $test_i64_extend16_s (param $a i64)
                             (result i64)
    (i64.extend16_s (local.get $a))
  )

  (export "test_i64_extend16_s" (func $test_i64_extend16_s))

  ;;
  ;; test_i64_extend32_s:
  ;;   expect i64 extend32_s($a)
  ;;
  (func $test_i64_extend32_s (param $a i64)
                             (result i64)
    (i64.extend32_s (local.get $a))
  )

  (export "test_i64_extend32_s" (func $test_i64_extend32_s))

  ;;
  ;; test_i32_trunc_sat_f32_s
  ;;   expect i32 trunc_sat_f32_s($a)
  ;;
  (func $test_i32_trunc_sat_f32_s (param $a f32)
                                  (result i32)
    (i32.trunc_sat_f32_s (local.get $a))
  )

  (export "test_i32_trunc_sat_f32_s" (func $test_i32_trunc_sat_f32_s))

  ;;
  ;; test_i32_trunc_sat_f32_u
  ;;   expect i32 trunc_sat_f32_u($a)
  ;;
  (func $test_i32_trunc_sat_f32_u (param $a f32)
                                  (result i32)
    (i32.trunc_sat_f32_u (local.get $a))
  )

  (export "test_i32_trunc_sat_f32_u" (func $test_i32_trunc_sat_f32_u))

  ;;
  ;; test_i32_trunc_sat_f64_s
  ;;   expect i32 trunc_sat_f64_s($a)
  ;;
  (func $test_i32_trunc_sat_f64_s (param $a f64)
                                  (result i32)
    (i32.trunc_sat_f64_s (local.get $a))
  )

  (export "test_i32_trunc_sat_f64_s" (func $test_i32_trunc_sat_f64_s))

  ;;
  ;; test_i32_trunc_sat_f64_u
  ;;   expect i32 trunc_sat_f64_u($a)
  ;;
  (func $test_i32_trunc_sat_f64_u (param $a f64)
                                  (result i32)
    (i32.trunc_sat_f64_u (local.get $a))
  )

  (export "test_i32_trunc_sat_f64_u" (func $test_i32_trunc_sat_f64_u))

  ;;
  ;; test_i64_trunc_sat_f32_s
  ;;   expect i64 trunc_sat_f32_s($a)
  ;;
  (func $test_i64_trunc_sat_f32_s (param $a f32)
                                  (result i64)
    (i64.trunc_sat_f32_s (local.get $a))
  )

  (export "test_i64_trunc_sat_f32_s" (func $test_i64_trunc_sat_f32_s))

  ;;
  ;; test_i64_trunc_sat_f32_u
  ;;   expect i64 trunc_sat_f32_u($a)
  ;;
  (func $test_i64_trunc_sat_f32_u (param $a f32)
                                  (result i64)
    (i64.trunc_sat_f32_u (local.get $a))
  )

  (export "test_i64_trunc_sat_f32_u" (func $test_i64_trunc_sat_f32_u))

  ;;
  ;; test_i64_trunc_sat_f64_s
  ;;   expect i64 trunc_sat_f64_s($a)
  ;;
  (func $test_i64_trunc_sat_f64_s (param $a f64)
                                  (result i64)
    (i64.trunc_sat_f64_s (local.get $a))
  )

  (export "test_i64_trunc_sat_f64_s" (func $test_i64_trunc_sat_f64_s))

  ;;
  ;; test_i64_trunc_sat_f64_u
  ;;   expect i64 trunc_sat_f64_u($a)
  ;;
  (func $test_i64_trunc_sat_f64_u (param $a f64)
                                  (result i64)
    (i64.trunc_sat_f64_u (local.get $a))
  )

  (export "test_i64_trunc_sat_f64_u" (func $test_i64_trunc_sat_f64_u))

  ;;
  ;; test_v128_load
  ;;   expect i64 0x161412100e0c0a08
  ;;
  (func $test_v128_load (result i64)
    (i64.store (i32.const 0) (i64.const 0x0706050403020100))
    (i64.store (i32.const 8) (i64.const 0x0f0e0d0c0b0a0908))
    (i64.store (i32.const 16) (i64.const 0x0000000000000000))

    (i64x2.extract_lane 0
      (i64x2.add
        (v128.load (i32.const 0))
        (v128.load (i32.const 8))))
  )

  (export "test_v128_load" (func $test_v128_load))

  ;;
  ;; test_i16x8_load8x8_s
  ;;   expect i32 28
  ;;
  (func $test_i16x8_load8x8_s (result i32)
    (i64.store (i32.const 0) (i64.const 0x0706050403020100))
    (i64.store (i32.const 8) (i64.const 0x0000000000000000))
    (i64.store (i32.const 16) (i64.const 0x0000000000000000))

    (i16x8.extract_lane_s 0
      (i16x8.add
        (i16x8.load8x8_s (i32.const 7))
        (i16x8.add
          (i16x8.load8x8_s (i32.const 6))
          (i16x8.add
            (i16x8.load8x8_s (i32.const 5))
            (i16x8.add
              (i16x8.load8x8_s (i32.const 4))
              (i16x8.add
                (i16x8.load8x8_s (i32.const 3))
                (i16x8.add
                  (i16x8.load8x8_s (i32.const 2))
                  (i16x8.add
                    (i16x8.load8x8_s (i32.const 1))
                    (i16x8.load8x8_s (i32.const 0))))))))))
  )

  (export "test_i16x8_load8x8_s" (func $test_i16x8_load8x8_s))

  ;;
  ;; test_i16x8_load8x8_u
  ;;   expect i32 5040
  ;;
  (func $test_i16x8_load8x8_u (result i32)
    (i64.store (i32.const 0) (i64.const 0x0706050403020101))
    (i64.store (i32.const 8) (i64.const 0x0000000000000000))
    (i64.store (i32.const 16) (i64.const 0x0000000000000000))

    (i16x8.extract_lane_u 0
      (i16x8.mul
        (i16x8.load8x8_u (i32.const 7))
        (i16x8.mul
          (i16x8.load8x8_u (i32.const 6))
          (i16x8.mul
            (i16x8.load8x8_u (i32.const 5))
            (i16x8.mul
              (i16x8.load8x8_u (i32.const 4))
              (i16x8.mul
                (i16x8.load8x8_u (i32.const 3))
                (i16x8.mul
                  (i16x8.load8x8_u (i32.const 2))
                  (i16x8.mul
                    (i16x8.load8x8_u (i32.const 1))
                    (i16x8.load8x8_u (i32.const 0))))))))))
  )

  (export "test_i16x8_load8x8_u" (func $test_i16x8_load8x8_u))
)
