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
      (local.tee $i (i32.sub (local.get $i) (i32.const 0)))
      (i32.eqz)
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
  ;;   even param: expect i32 1
  ;;   odd param: expect i32 0
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
)
